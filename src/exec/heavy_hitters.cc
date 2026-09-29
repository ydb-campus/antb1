#include "heavy_hitters.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace antb1::exec {

HeavyHitters::HeavyHitters(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1)) {
  counters_.reserve(capacity_ + 1);
}

void HeavyHitters::Add(std::uint64_t value) {
  ++count_;
  if (const auto it = counters_.find(value); it != counters_.end()) {
    ++it->second;
    return;
  }
  if (counters_.size() < capacity_) {
    counters_.emplace(value, 1);
    return;
  }
  // No room: every counter (and the new value's) goes down by one. The total decrease never
  // exceeds the values added, so this costs O(1) per value on average.
  std::erase_if(counters_, [](auto& counter) { return --counter.second == 0; });
}

std::vector<std::uint64_t> HeavyHitters::Above(double share) const {
  // A counter is at most n / (capacity + 1) below its value's count: a value seen more than
  // share * n times keeps a counter above share * n - n / (capacity + 1).
  const auto n = static_cast<double>(count_);
  const double floor = (share * n) - (n / static_cast<double>(capacity_ + 1));
  std::vector<std::uint64_t> heavy;
  for (const auto& [value, counter] : counters_) {
    if (static_cast<double>(counter) > floor) {
      heavy.push_back(value);
    }
  }
  std::ranges::sort(heavy);
  return heavy;
}

}  // namespace antb1::exec
