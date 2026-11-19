#ifndef SIELAB_WHEEL_TABLE_HPP
#define SIELAB_WHEEL_TABLE_HPP

#include <cstdint>
#include <vector>

namespace sielab {

// A wheel of modulus m keeps only the residues r in [1, m) that are coprime
// to m.  For m = 2 that is the classic "odd numbers only" representation; for
// m = 30 / 210 it is the 8 / 48 residue wheel.
//
// Layout of one block: `bits_per_block()` bits, one per residue, covering the
// m integers of that block.  A set bit means "still possible prime".
class WheelTable {
public:
  explicit WheelTable(std::uint32_t modulus);

  std::uint32_t modulus() const { return modulus_; }
  std::uint32_t bits_per_block() const { return residues_.size(); }
  const std::vector<std::uint32_t>& residues() const { return residues_; }

  // Position of residue r inside a block, or -1 when r is not coprime.
  int position(std::uint32_t r) const { return position_[r % modulus_]; }

  // Next coprime residue after the one at `pos` (wraps around, adding m).
  std::uint32_t step(std::uint32_t pos) const { return increments_[pos]; }

  // Advance a residue by `k` wheel slots (used for bucket re-dispatch).
  std::uint32_t advance(std::uint32_t pos, std::uint32_t k) const {
    return (pos + k % bits_per_block()) % bits_per_block();
  }

  // Number of the first coprime integer >= n (n >= 1).
  std::uint64_t first_coprime_at_or_above(std::uint64_t n) const;

  // Residue index for the first coprime integer >= n; also reports that
  // integer through `value`.
  std::uint32_t locate(std::uint64_t n, std::uint64_t* value) const;

private:
  std::uint32_t modulus_;
  std::vector<std::uint32_t> residues_;
  std::vector<int> position_;
  std::vector<std::uint32_t> increments_;  // in integers, not slots
};

// gcd helper used by the tests.
std::uint32_t gcd_u32(std::uint32_t a, std::uint32_t b);

} // namespace sielab

#endif