// check_parallel.cpp — parallel vs serial: identical count AND identical mark
// count for every thread count (the race-freedom invariant).
#include "sieve_core.hpp"
#include "sieve_parallel.hpp"

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

  struct Case { std::uint64_t n; std::uint32_t wheel; std::size_t seg_kb; };
  std::vector<Case> cases = {
    {100000ull, 2, 16}, {100000ull, 210, 32}, {1000000ull, 210, 32},
    {10000000ull, 210, 64}, {10000000ull, 30, 16}, {50000000ull, 210, 32},
  };

  for (const auto& c : cases) {
    Options ser; ser.wheel = c.wheel; ser.segment_bytes = c.seg_kb * 1024;
    Stats sser;
    const std::uint64_t want = count_primes(c.n, ser, &sser);

    for (std::uint32_t t : {1u, 2u, 3u, 4u}) {
      Options par; par.wheel = c.wheel; par.segment_bytes = c.seg_kb * 1024;
      Stats spar;
      const std::uint64_t got = count_primes_parallel(c.n, par, t, &spar);
      std::printf("n=%llu wheel=%u seg=%zuK t=%u -> count=%llu marks=%llu (serial %llu/%llu)\n",
                  (unsigned long long)c.n, c.wheel, c.seg_kb, t,
                  (unsigned long long)got, (unsigned long long)spar.marks,
                  (unsigned long long)want, (unsigned long long)sser.marks);
      check(got == want, "parallel count == serial count");
      check(spar.marks == sser.marks, "parallel marks == serial marks (thread-count independent)");
    }
  }

  // A crossing-heavy case: force many primes into the far tier so the queue
  // path is exercised at small scale (not just the direct tier).  crossing_from
  // must respect the tier rule (crossing_from >= segNums/2, i.e. at most one
  // multiple per prime per segment); we pick it to stay inside the rule.
  {
    std::uint64_t n = 1000000ull;
    Stats sser;
    Options ser; ser.wheel = 210; ser.segment_bytes = 8 * 1024;
    const std::uint64_t want = count_primes(n, ser, &sser);
    for (std::uint32_t t : {1u, 2u, 4u}) {
      Options par; par.wheel = 210; par.segment_bytes = 8 * 1024; par.crossing_from = 2000;
      Stats spar;
      const std::uint64_t got = count_primes_parallel(n, par, t, &spar);
      std::printf("crossing-heavy n=%llu t=%u -> count=%llu marks=%llu\n",
                  (unsigned long long)n, t, (unsigned long long)got,
                  (unsigned long long)spar.marks);
      check(got == want, "crossing-heavy parallel count == serial");
      check(spar.marks == sser.marks, "crossing-heavy marks match serial");
    }
  }

  // Known large value through the parallel path.
  {
    Options par; par.wheel = 210; par.segment_bytes = 64 * 1024;
    const std::uint64_t got = count_primes_parallel(1000000000ull, par, 4, nullptr);
    std::printf("parallel pi(1e9) = %llu\n", (unsigned long long)got);
    check(got == 50847534ull, "parallel pi(1e9) == 50847534");
  }

  std::printf(failures ? "%d FAILURES\n" : "ALL PARALLEL CHECKS PASSED\n", failures);
  return failures ? 1 : 0;
}