#include "wheel_table.hpp"

namespace sielab {

std::uint32_t gcd_u32(std::uint32_t a, std::uint32_t b) {
  while (b) { const std::uint32_t t = a % b; a = b; b = t; }
  return a;
}

WheelTable::WheelTable(std::uint32_t modulus) : modulus_(modulus) {
  residues_.clear();
  for (std::uint32_t r = 1; r < modulus_; ++r)
    if (gcd_u32(r, modulus_) == 1) residues_.push_back(r);

  position_.assign(modulus_, -1);
  for (std::uint32_t i = 0; i < residues_.size(); ++i)
    position_[residues_[i]] = static_cast<int>(i);

  // increments_[i] = distance (in integers) from residues_[i] to the next
  // coprime residue, wrapping past the modulus.
  increments_.resize(residues_.size());
  for (std::uint32_t i = 0; i < residues_.size(); ++i) {
    const std::uint32_t cur = residues_[i];
    const std::uint32_t nxt =
        (i + 1 < residues_.size()) ? residues_[i + 1] : residues_[0] + modulus_;
    increments_[i] = nxt - cur;
  }
}

std::uint64_t WheelTable::first_coprime_at_or_above(std::uint64_t n) const {
  if (n <= 1) return 1;
  const std::uint64_t block = (n / modulus_) * modulus_;
  const std::uint32_t off = static_cast<std::uint32_t>(n - block);
  // First residue >= off inside the block.
  for (std::uint32_t r : residues_)
    if (r >= off) return block + r;
  return block + modulus_ + residues_.front();   // next block
}

std::uint32_t WheelTable::locate(std::uint64_t n, std::uint64_t* value) const {
  const std::uint64_t block = (n / modulus_) * modulus_;
  const std::uint32_t off = static_cast<std::uint32_t>(n - block);
  for (std::uint32_t r : residues_) {
    if (r >= off) {
      if (value) *value = block + r;
      return position_[r];
    }
  }
  if (value) *value = block + modulus_ + residues_.front();
  return 0;
}

} // namespace sielab