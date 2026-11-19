// check.cpp — correctness oracle comparison across every knob combination.
#include "sieve_core.hpp"
#include "reference.hpp"
#include "wheel_table.hpp"

#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
  if (!ok) { std::printf("FAIL  %s\n", what); ++failures; }
}
}

int main() {
  using namespace sielab;

  // Wheel table invariants.
  for (std::uint32_t m : {2u, 30u, 210u}) {
    WheelTable w(m);
    std::uint32_t expected = m == 2 ? 1 : (m == 30 ? 8 : 48);
    check(w.bits_per_block() == expected, "wheel residue count");
    for (std::uint32_t r : w.residues())
      check(gcd_u32(r, m) == 1, "wheel residues coprime");
    for (std::uint32_t i = 0; i < w.bits_per_block(); ++i)
      check(w.position(w.residues()[i]) == static_cast<int>(i), "wheel inverse map");
  }

  // Reference oracles agree on small inputs.
  for (std::uint64_t n = 0; n <= 3000; ++n)
    check(trial_division_count(n) == classic_odd_sieve_count(n), "oracle agreement");

  // Every knob combination vs the independent trial-division oracle.
  for (std::uint32_t wheel : {2u, 30u, 210u}) {
    for (bool bucket : {false, true}) {
      Options o;
      o.wheel = wheel; o.bucket = bucket; o.segment_bytes = 8 * 1024;
      for (std::uint64_t n : {2ull, 3ull, 16ull, 17ull, 100ull, 999ull,
                              4096ull, 10000ull, 100000ull}) {
        const std::uint64_t got = count_primes(n, o);
        const std::uint64_t want = trial_division_count(n);
        if (got != want)
          std::printf("n=%llu wheel=%u bucket=%d got=%llu want=%llu\n",
                      (unsigned long long)n, wheel, (int)bucket,
                      (unsigned long long)got, (unsigned long long)want);
        check(got == want, "count matches oracle");
      }
    }
  }

  // Known values at scale, including non-default segment sizes.
  struct { std::uint64_t n, want; } known[] = {
    {1000000ull, 78498ull}, {10000000ull, 664579ull},
    {100000000ull, 5761455ull}, {1000000000ull, 50847534ull}
  };
  for (auto& k : known) {
    for (std::uint32_t wheel : {2u, 30u, 210u}) {
      Options o; o.wheel = wheel; o.bucket = true; o.segment_bytes = 64 * 1024;
      const std::uint64_t got = count_primes(k.n, o);
      if (got != k.want)
        std::printf("pi(%llu) wheel=%u got=%llu want=%llu\n",
                    (unsigned long long)k.n, wheel,
                    (unsigned long long)got, (unsigned long long)k.want);
      check(got == k.want, "known pi(n)");
    }
  }

  std::printf(failures ? "%d FAILURES\n" : "ALL CHECKS PASSED\n", failures);
  return failures ? 1 : 0;
}