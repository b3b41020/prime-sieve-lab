#include "sieve_parallel.hpp"
#include "wheel_table.hpp"
#include "bucket_queue.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace sielab {

namespace {

using Clock = std::chrono::steady_clock;

// Minimal C++17 barrier: workers meet once per segment.
class Barrier {
public:
  explicit Barrier(std::size_t n) : total_(n), count_(n) {}
  void wait() {
    std::unique_lock<std::mutex> lk(m_);
    const std::uint64_t gen = gen_;
    if (--count_ == 0) { ++gen_; count_ = total_; cv_.notify_all(); return; }
    cv_.wait(lk, [&] { return gen_ != gen; });
  }
private:
  std::mutex m_;
  std::condition_variable cv_;
  std::size_t total_;
  std::size_t count_;
  std::uint64_t gen_ = 0;
};

std::vector<std::uint32_t> sieving_primes(std::uint64_t n) {
  std::vector<std::uint32_t> out;
  if (n < 2) return out;
  out.push_back(2);
  const std::uint64_t half = (n - 1) / 2;
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

std::uint64_t isqrt_u64(std::uint64_t n) {
  std::uint64_t r = static_cast<std::uint64_t>(
      __builtin_sqrtl(static_cast<long double>(n)));
  while (r > n / r) --r;
  while ((r + 1) <= n / (r + 1)) ++r;
  return r;
}

// One unit of work handed to a worker: a single multiple to clear.
struct MarkJob {
  std::uint32_t prime;
  std::uint64_t multiple;
};

struct Slice {
  std::size_t firstBlock = 0;
  std::size_t blockCount = 0;
  std::vector<MarkJob> jobs;
  std::uint64_t cleared = 0;
  std::uint64_t counted = 0;
};

} // namespace

std::uint64_t count_primes_parallel(std::uint64_t limit, const Options& opts,
                                    std::uint32_t threads, Stats* stats) {
  const auto t0 = Clock::now();
  if (threads == 0) threads = 1;

  const WheelTable wheel(opts.wheel);

  // Small primes that divide the wheel have no bit in the bitmap.  For the
  // degenerate range below the first block, count every small prime directly.
  std::uint64_t small = 0;
  if (limit < 17) {
    for (std::uint64_t q : {2ull, 3ull, 5ull, 7ull, 11ull, 13ull})
      if (q <= limit) ++small;
    if (stats) { stats->primes = small; stats->seconds = 0.0; stats->segments = 0; }
    return small;
  }
  for (std::uint64_t q : {2ull, 3ull, 5ull, 7ull})
    if (q <= limit && wheel.modulus() % q == 0) ++small;
  const std::uint32_t bpB = wheel.bits_per_block();
  const std::uint32_t mod = wheel.modulus();
  std::uint64_t blocks = (opts.segment_bytes * 8) / bpB;
  if (blocks == 0) blocks = 1;
  while ((blocks * bpB) % 8 != 0) ++blocks;
  const std::uint64_t segNums = blocks * mod;
  const std::size_t bytes = static_cast<std::size_t>(blocks * bpB / 8);
  const std::uint64_t segments = limit / segNums + 1;

  const auto primes = sieving_primes(isqrt_u64(limit));

  // Tier rule (the whole race-freedom argument rests on this): a crossing is
  // only tracked across segments when 2*prime >= segNums, i.e. a prime has at
  // most one multiple per segment.  `crossing_from` is a test hook that lowers
  // the threshold; the coordinator then emits one job per multiple, so even
  // that case stays well defined.
  const std::uint64_t threshold = opts.crossing_from > 0
      ? opts.crossing_from
      : std::max<std::uint64_t>(segNums / 2, 1);
  const std::size_t directEnd = static_cast<std::size_t>(
      std::lower_bound(primes.begin(), primes.end(), threshold) - primes.begin());

  const std::uint32_t T = std::max<std::uint32_t>(
      1, std::min<std::uint32_t>(threads, static_cast<std::uint32_t>(blocks)));

  std::vector<Slice> slices(T);
  for (std::uint32_t t = 0; t < T; ++t) {
    const std::size_t first = blocks * t / T;
    const std::size_t last = blocks * (t + 1) / T;
    slices[t].firstBlock = first;
    slices[t].blockCount = last - first;
  }

  std::vector<std::uint8_t> buf(bytes, 0);
  Barrier barrier(T);
  std::uint64_t total = 0;
  std::uint64_t marks = 0;

  // ---- coordinator-only state: the segment queue lives here, single-threaded
  std::vector<std::vector<Crossing>> far(static_cast<std::size_t>(segments));
  std::size_t activation = directEnd;

  const auto slice_of = [&](std::uint64_t number, std::uint64_t base) -> std::uint32_t {
    if (number < base) return 0;
    const std::uint64_t blk = (number - base) / mod;
    std::uint32_t s = static_cast<std::uint32_t>(blk * T / blocks);
    return s >= T ? T - 1 : s;
  };

  // Coordinator-only: build one segment's immutable job lists.  Called once
  // before any worker starts, then once after each barrier for the next
  // segment.  Crossing bookkeeping never happens inside a worker.
  auto prepare = [&](std::uint64_t seg) {
    const std::uint64_t nb = seg * segNums;
    const std::uint64_t nh = std::min(nb + segNums, limit + 1);
    for (std::uint32_t t = 0; t < T; ++t) slices[t].jobs.clear();

    auto& queued = far[static_cast<std::size_t>(seg)];
    auto emit = [&](std::uint64_t p, std::uint64_t k) {
      std::uint32_t pos = wheel.position(static_cast<std::uint32_t>(k % mod));
      std::uint64_t m = k * p;
      while (m < nh) {
        if (m >= nb) slices[slice_of(m, nb)].jobs.push_back(
            {static_cast<std::uint32_t>(p), m});
        k += wheel.step(pos);
        pos = (pos + 1) % bpB;
        m = k * p;
      }
      if (m <= limit) {
        const std::size_t target = static_cast<std::size_t>(m / segNums);
        far[std::min(target, far.size() - 1)].push_back({static_cast<std::uint32_t>(p), k});
      }
    };

    for (const Crossing& c : queued) emit(c.prime, c.cofactor);
    queued.clear();
    while (activation < primes.size()) {
      const std::uint64_t p = primes[activation];
      if (mod % p == 0) { ++activation; continue; }       // no bit exists
      const std::uint64_t pp = p * p;
      if (pp < nb) { ++activation; continue; }            // already active
      if (pp >= nh) break;                                // later segment
      emit(p, p);
      ++activation;
    }
  };

  auto worker = [&](std::uint32_t tid) {
    Slice& sl = slices[tid];
    for (std::uint64_t seg = 0; seg < segments; ++seg) {
      const std::uint64_t base = seg * segNums;
      const std::uint64_t high = std::min(base + segNums, limit + 1);
      const std::uint64_t sliceLow = base + sl.firstBlock * mod;
      const std::uint64_t sliceHigh = std::min(base + (sl.firstBlock + sl.blockCount) * mod, high);

      const std::size_t byteFirst = static_cast<std::size_t>(sl.firstBlock * bpB / 8);
      const std::size_t byteLast = static_cast<std::size_t>((sl.firstBlock + sl.blockCount) * bpB / 8);
      if (byteLast > byteFirst && sliceLow < high)
        std::memset(buf.data() + byteFirst, 0xFF, byteLast - byteFirst);
      if (seg == 0 && sl.firstBlock == 0) {
        const std::uint32_t bit = wheel.position(1);
        buf[bit >> 3] &= static_cast<std::uint8_t>(~(1u << (bit & 7)));
      }

      std::uint64_t cleared = 0;
      // Direct tier: stateless per slice, so threads share nothing here.
      for (std::size_t i = 0; i < directEnd; ++i) {
        const std::uint32_t p = primes[i];
        if (mod % p == 0) continue;
        const std::uint64_t pp = 1ull * p * p;
        if (pp >= sliceHigh) break;
        std::uint64_t k0 = (std::max(pp, sliceLow) + p - 1) / p;
        if (k0 < p) k0 = p;
        std::uint64_t k;
        std::uint32_t pos = wheel.locate(k0, &k);
        for (; k * p < sliceHigh; ) {
          const std::uint64_t m = k * p;
          const std::uint64_t rel = m - base;
          const std::uint32_t bit = static_cast<std::uint32_t>(
              (rel / mod) * bpB + wheel.position(static_cast<std::uint32_t>(rel % mod)));
          buf[bit >> 3] &= static_cast<std::uint8_t>(~(1u << (bit & 7)));
          ++cleared;
          k += wheel.step(pos);
          pos = (pos + 1) % bpB;
        }
      }

      // Crossing jobs: pre-computed by the coordinator, one multiple each,
      // all inside this slice by construction.
      for (const MarkJob& job : sl.jobs) {
        const std::uint64_t rel = job.multiple - base;
        const std::uint32_t bit = static_cast<std::uint32_t>(
            (rel / mod) * bpB + wheel.position(static_cast<std::uint32_t>(rel % mod)));
        buf[bit >> 3] &= static_cast<std::uint8_t>(~(1u << (bit & 7)));
        ++cleared;
      }

      std::uint64_t counted = 0;
      const std::uint64_t nBlocks = (sliceHigh > sliceLow)
          ? (sliceHigh - sliceLow + mod - 1) / mod : 0;
      for (std::uint64_t b = 0; b < nBlocks; ++b) {
        const std::uint64_t blockStart = sliceLow + b * mod;
        const std::size_t firstBit = static_cast<std::size_t>((blockStart - base) / mod * bpB);
        if (blockStart + mod <= sliceHigh && blockStart + mod <= high) {
          // Whole block: popcount the full bytes, then the leftover bits
          // (needed for wheel 2 where one block is a single bit).
          for (std::uint32_t by = 0; by < bpB / 8; ++by)
            counted += static_cast<std::uint64_t>(
                __builtin_popcount(static_cast<unsigned>(buf[(firstBit >> 3) + by])));
          for (std::uint32_t j = bpB - bpB % 8; j < bpB; ++j) {
            const std::size_t bit = firstBit + j;
            counted += (buf[bit >> 3] >> (bit & 7)) & 1u;
          }
        } else {
          for (std::uint32_t j = 0; j < bpB; ++j) {
            const std::uint64_t num = blockStart + wheel.residues()[j];
            if (num >= high) break;
            const std::size_t bit = firstBit + j;
            counted += (buf[bit >> 3] >> (bit & 7)) & 1u;
          }
        }
      }
      sl.cleared = cleared;
      sl.counted = counted;

      barrier.wait();        // segment complete: bit slices and counters settled

      if (tid == 0) {
        for (std::uint32_t t = 0; t < T; ++t) {
          total += slices[t].counted;
          marks += slices[t].cleared;
        }
        if (seg + 1 < segments) prepare(seg + 1);
      }
      barrier.wait();        // next segment's lists are in place
    }
  };

  prepare(0);

  std::vector<std::thread> pool;
  pool.reserve(T - 1);
  for (std::uint32_t t = 1; t < T; ++t) pool.emplace_back(worker, t);
  worker(0);
  for (auto& th : pool) th.join();

  if (stats) {
    stats->primes = small + total;
    stats->seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    stats->segments = static_cast<std::size_t>(segments);
    stats->marks = marks;
  }
  return small + total;
}

} // namespace sielab