#ifndef SIELAB_REFERENCE_HPP
#define SIELAB_REFERENCE_HPP

#include <cstdint>

namespace sielab {

// Deliberately simple, independent counters used as correctness oracles.
std::uint64_t trial_division_count(std::uint64_t n);
std::uint64_t classic_odd_sieve_count(std::uint64_t n);

} // namespace sielab

#endif