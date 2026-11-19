#include "sieve_core.hpp"
#include "wheel_table.hpp"
#include "bucket_queue.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace sielab {

namespace {

using Clock = std::chrono::steady_clock;

double elapsed(const Clock::time_point& t0) {
  return std::chrono::duration<double>(Clock::now() - t0).count();
}

// Independent helper: all primes <= n via a plain odd-only sieve.  Used only
// to obtain the sieving-prime list; the research knobs live in count_primes.
std::vector<std::uint32_t> sieving_primes(std::uint64_t n) {
  std::vector<std::uint32_t> out;
  if (n < 2) return out;
  out.push_back(2);
  const std::uint64_t half = (n - 1) / 2;             // odds 3,5,7,...
  std::vector<std::uint8_t> comp(half + 1, 0);
  for (std::uint64_t i = 0; 2 * i + 3 <= n; ++i) {
    if (comp[i]) continue;
    const std::uint64_t p = 2 * i + 3;
    out.push_back(static_cast<std::uint32_t>(p));
    for (std::uint64_t m = p * p; m <= n; m += 2 * p) {
      const std::uint64_t idx = (m - 3) / 2;
      if (idx <= half) comp[idx] = 1;
    }
  }
  return out;
}

// Prime factors of the wheel modulus: these primes have no bit in the bitmap
// because every multiple of them is a non-coprime residue.
std::vector<std::uint64_t> wheel_prime_factors(std::uint32_t m) {
  std::vector<std::uint64_t> f;
  for (std::uint64_t p : {2ull, 3ull, 5ull, 7ull})
    if (m % p == 0) f.push_back(p);
  return f;
}

class Engine {
public:
  Engine(std::uint64_t limit, const Options& o)
      : limit_(limit), opts_(o), wheel_(o.wheel) {
    bpB_ = wheel_.bits_per_block();
    blocks_ = (o.segment_bytes * 8) / bpB_;
    if (blocks_ == 0) blocks_ = 1;
    while ((blocks_ * bpB_) % 8 != 0) ++blocks_;   // keep byte alignment
    segNums_ = 1ull * blocks_ * wheel_.modulus();
    bytes_ = blocks_ * bpB_ / 8;
    segments_ = static_cast<std::size_t>(limit / segNums_) + 1;
  }

  std::uint64_t run(Stats* st) {
    const auto t0 = Clock::now();
    std::uint64_t count = 0;
    for (std::uint64_t p : wheel_prime_factors(opts_.wheel))
      if (p <= limit_) ++count;

    if (limit_ < 17) {
      // Below the first segment the bitmap does not cover the range: count the
      // handful of candidates directly.
      std::uint64_t small = 0;
      for (std::uint64_t q : {2ull, 3ull, 5ull, 7ull, 11ull, 13ull})
        if (q <= limit_) ++small;
      if (st) { st->primes = small; st->seconds = elapsed(t0); st->segments = 0; }
      return small;
    }

    const auto primes = sieving_primes(isqrt(limit_));
    buf_.assign(bytes_, 0);
    if (opts_.bucket) buckets_.resize(segments_);

    std::size_t nextCrosser = 0;    // primes not yet handed to buckets
    std::size_t directEnd_ = 0;     // fixed small/large split (segment 0)
    std::size_t seg = 0;
    for (std::uint64_t base = 0; base <= limit_; base += segNums_, ++seg) {
      const std::uint64_t high = std::min(base + segNums_, limit_ + 1); // exclusive
      std::memset(buf_.data(), 0xFF, bytes_);
      if (base == 0) clear_bit(wheel_.position(1));   // number 1 is not prime

      if (opts_.bucket) consume_bucket(seg, base, high, count);

      // Split the primes once, in segment 0: small ones are marked directly
      // in every segment, the trailing slice is owned by buckets forever.
      // Letting the boundary drift would double-mark primes whose p*p falls
      // inside a later segment.
      std::size_t direct = primes.size();
      if (opts_.bucket) {
        if (seg == 0) {
          while (directEnd_ < primes.size() &&
                 1ull * primes[directEnd_] * primes[directEnd_] < high)
            ++directEnd_;
        }
        direct = directEnd_;
        for (; nextCrosser < primes.size(); ++nextCrosser) {
          const std::uint64_t p = primes[nextCrosser];
          if (wheel_.modulus() % p == 0) continue;                 // 2,3,5,7
          buckets_.push(static_cast<std::size_t>((p * p) / segNums_),
                        static_cast<std::uint32_t>(p), p);
        }
      }

      // Main marking loop over the primes handled directly.
      for (std::size_t i = 0; i < direct; ++i) {
        const std::uint32_t p = primes[i];
        if (opts_.wheel % p == 0) continue;           // no bit exists
        mark_multiples(p, base, high);
      }

      count += count_set_bits(base, high);
    }

    if (st) {
      st->primes = count;
      st->seconds = elapsed(t0);
      st->segments = seg;
      st->marks = marks_;
    }
    return count;
  }

private:
  std::uint64_t isqrt(std::uint64_t n) const {
    std::uint64_t r = static_cast<std::uint64_t>(
        __builtin_sqrtl(static_cast<long double>(n)));
    while (r > n / r) --r;
    while ((r + 1) <= n / (r + 1)) ++r;
    return r;
  }

  void clear_bit(std::uint32_t bit) { buf_[bit >> 3] &= mask_off(bit & 7); }
  static std::uint8_t mask_off(std::uint32_t b) {
    return static_cast<std::uint8_t>(~(1u << b));
  }

  // Mark m = k*p for every wheel-coprime cofactor k >= max(p, first >= low/p),
  // while m < high.
  void mark_multiples(std::uint32_t p, std::uint64_t base, std::uint64_t high) {
    const std::uint64_t pp = 1ull * p * p;
    std::uint64_t k0 = (std::max(pp, base) + p - 1) / p;
    if (k0 < p) k0 = p;
    std::uint64_t k;
    std::uint32_t pos = wheel_.locate(k0, &k);
    const std::uint32_t bpB = bpB_;
    const std::uint32_t mod = wheel_.modulus();
    for (; k * p < high; ) {
      const std::uint64_t m = k * p;
      const std::uint64_t rel = m - base;
      const std::uint32_t bit = static_cast<std::uint32_t>(
          (rel / mod) * bpB + wheel_.position(static_cast<std::uint32_t>(rel % mod)));
      buf_[bit >> 3] &= mask_off(bit & 7);
      ++marks_;
      k += wheel_.step(pos);
      pos = (pos + 1) % bpB;
    }
  }

  void consume_bucket(std::size_t seg, std::uint64_t base, std::uint64_t high,
                      std::uint64_t& count) {
    auto& items = buckets_.at(seg);
    for (const Crossing& c : items) {
      std::uint64_t k = c.cofactor;
      std::uint32_t pos = wheel_.position(static_cast<std::uint32_t>(k % wheel_.modulus()));
      const std::uint32_t bpB = bpB_;
      const std::uint32_t mod = wheel_.modulus();
      const std::uint64_t p = c.prime;
      for (; k * p < high; ) {
        const std::uint64_t m = k * p;
        const std::uint64_t rel = m - base;
        const std::uint32_t bit = static_cast<std::uint32_t>(
            (rel / mod) * bpB + wheel_.position(static_cast<std::uint32_t>(rel % mod)));
        buf_[bit >> 3] &= mask_off(bit & 7);
        ++marks_;
        k += wheel_.step(pos);
        pos = (pos + 1) % bpB;
      }
      // Re-queue at the segment holding the next multiple, if in range.
      if (k * p <= limit_) {
        buckets_.push(static_cast<std::size_t>((k * p) / segNums_),
                      c.prime, k);
      }
      (void)count;
    }
    buckets_.clear(seg);
    (void)base;
  }

  // Count set bits representing numbers in [base, high).
  std::uint64_t count_set_bits(std::uint64_t base, std::uint64_t high) const {
    std::uint64_t total = 0;
    const std::uint32_t mod = wheel_.modulus();
    const std::uint32_t bpB = bpB_;
    const std::uint64_t nBlocks = (high - base + mod - 1) / mod;
    for (std::uint64_t b = 0; b < nBlocks; ++b) {
      const std::uint64_t blockStart = base + b * mod;
      const std::uint64_t blockEnd = blockStart + mod;
      if (blockEnd <= high) {
        // fast path: whole block inside range
        const std::uint32_t first = static_cast<std::uint32_t>(b * bpB);
        for (std::uint32_t by = 0; by < bpB_ / 8; ++by)
          total += __builtin_popcount(
              static_cast<unsigned>(buf_[(first + by * 8) >> 3]));
        if (bpB_ % 8) {
          for (std::uint32_t j = bpB_ - bpB_ % 8; j < bpB_; ++j) {
            const std::uint32_t bit = first + j;
            total += (buf_[bit >> 3] >> (bit & 7)) & 1u;
          }
        }
      } else {
        for (std::uint32_t j = 0; j < bpB_; ++j) {
          const std::uint64_t num = blockStart + wheel_.residues()[j];
          if (num >= high) break;
          const std::uint32_t bit = static_cast<std::uint32_t>(b * bpB + j);
          total += (buf_[bit >> 3] >> (bit & 7)) & 1u;
        }
      }
    }
    return total;
  }

  std::uint64_t limit_;
  Options opts_;
  WheelTable wheel_;
  std::uint32_t bpB_ = 0;
  std::uint64_t blocks_ = 0;
  std::uint64_t segNums_ = 0;
  std::size_t bytes_ = 0;
  std::size_t segments_ = 0;
  std::vector<std::uint8_t> buf_;
  BucketQueue buckets_;
  std::uint64_t marks_ = 0;
};

} // namespace

std::uint64_t count_primes(std::uint64_t limit, const Options& opts, Stats* stats) {
  Engine e(limit, opts);
  return e.run(stats);
}

} // namespace sielab