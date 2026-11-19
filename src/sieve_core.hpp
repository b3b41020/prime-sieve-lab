#ifndef SIELAB_SIEVE_CORE_HPP
#define SIELAB_SIEVE_CORE_HPP

#include <cstdint>
#include <cstddef>

namespace sielab {

// Knobs under study.  Every knob has a measurable cost/benefit; see
// experiments/RESULTS.md for the measured trade-offs.
struct Options {
  std::uint32_t wheel = 210;        // 2 = odd-only, 30, 210
  bool bucket = true;               // bucket re-dispatch for far primes
  std::size_t segment_bytes = 32768;
  // Test hook: only primes >= crossing_from are treated as "far" crossings.
  // 0 = auto (2*prime >= segment_size tier rule).
  std::uint64_t crossing_from = 0;
};

struct Stats {
  std::uint64_t primes = 0;
  double seconds = 0.0;
  std::size_t segments = 0;
  std::uint64_t marks = 0;          // single-bit clear operations
};

// Count primes <= limit with the configured knobs.
std::uint64_t count_primes(std::uint64_t limit, const Options& opts = {},
                           Stats* stats = nullptr);

} // namespace sielab

#endif