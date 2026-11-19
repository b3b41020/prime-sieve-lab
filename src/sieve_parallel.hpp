#ifndef SIELAB_SIEVE_PARALLEL_HPP
#define SIELAB_SIEVE_PARALLEL_HPP

#include "sieve_core.hpp"

#include <cstdint>

namespace sielab {

// Parallel segmented sieve.
//
// Race-freedom argument (see docs/parallelization.md):
//   * segments are processed one at a time; a segment's work lists are built
//     single-threaded before any worker touches it, so they are immutable while
//     the segment is live;
//   * threads own disjoint block ranges of the bit array and private outboxes;
//   * a crossing may only target a segment AFTER the current one, which is what
//     the "2 * prime >= segment_size" tier rule guarantees, so no thread can
//     write work into a segment that is being sieved;
//   * merging is single-threaded and ordered by thread id, and the only shared
//     counters are reduced after a barrier.
//
// Consequence: the number of bit-clear operations and the final count are
// identical for every thread count, which is exactly what tests assert.
std::uint64_t count_primes_parallel(std::uint64_t limit, const Options& opts,
                                    std::uint32_t threads, Stats* stats = nullptr);

} // namespace sielab

#endif