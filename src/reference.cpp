#include "reference.hpp"

#include <vector>

namespace sielab {

std::uint64_t trial_division_count(std::uint64_t n) {
  std::uint64_t c = 0;
  for (std::uint64_t x = 2; x <= n; ++x) {
    bool ok = true;
    for (std::uint64_t d = 2; d * d <= x; ++d)
      if (x % d == 0) { ok = false; break; }
    if (ok) ++c;
  }
  return c;
}

// Straightforward byte-per-odd sieve, no wheel table, no buckets.
std::uint64_t classic_odd_sieve_count(std::uint64_t n) {
  if (n < 2) return 0;
  const std::uint64_t size = n / 2 + 1;
  std::vector<std::uint8_t> composite(size, 0);   // index i => number 2*i+1
  std::uint64_t c = 1;                            // prime 2
  for (std::uint64_t i = 1; 2 * i + 1 <= n; ++i) {
    if (composite[i]) continue;
    const std::uint64_t p = 2 * i + 1;
    ++c;
    for (std::uint64_t m = p * p; m <= n; m += 2 * p)
      composite[(m - 1) / 2] = 1;
  }
  return c;
}

} // namespace sielab