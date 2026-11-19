#ifndef SIELAB_BUCKET_QUEUE_HPP
#define SIELAB_BUCKET_QUEUE_HPP

#include <cstdint>
#include <vector>

namespace sielab {

// A prime whose next multiple has to be marked in a later segment.
struct Crossing {
  std::uint32_t prime;        // sieving prime
  std::uint64_t cofactor;     // first wheel-coprime k with k * prime in range
};

// One queue per future segment inside the current window.
class BucketQueue {
public:
  void resize(std::size_t count) { buckets_.assign(count, {}); }

  void push(std::size_t segment, std::uint32_t prime, std::uint64_t cofactor) {
    buckets_[segment].push_back({prime, cofactor});
  }

  std::vector<Crossing>& at(std::size_t i) { return buckets_[i]; }
  const std::vector<Crossing>& at(std::size_t i) const { return buckets_[i]; }
  std::size_t size() const { return buckets_.size(); }

  void clear(std::size_t i) { buckets_[i].clear(); }

private:
  std::vector<std::vector<Crossing>> buckets_;
};

} // namespace sielab

#endif