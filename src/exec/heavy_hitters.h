#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace antb1::exec {

// The frequent values of a stream of 64-bit hashes in bounded memory: a Misra-Gries summary (the
// deterministic counterpart of SpaceSaving) of `capacity` counters. After n values, every value
// seen more than n / (capacity + 1) times has a counter, and a counter never exceeds its value's
// count, nor falls short of it by more than n / (capacity + 1). The summary depends only on the
// values and their order. Used by the two-level aggregation to find heavy grouping keys
// (docs/adr/0014-two-level-aggregation.md). Single-threaded.
class HeavyHitters {
 public:
  explicit HeavyHitters(std::size_t capacity);

  void Add(std::uint64_t value);
  // The values that may have been seen more than `share` of all values, sorted: every such value
  // when share >= 1 / (capacity + 1), and only values seen more than share - 1 / (capacity + 1)
  // of them.
  [[nodiscard]] std::vector<std::uint64_t> Above(double share) const;
  [[nodiscard]] std::int64_t count() const { return count_; }

 private:
  std::size_t capacity_;
  std::unordered_map<std::uint64_t, std::int64_t> counters_;
  std::int64_t count_ = 0;
};

}  // namespace antb1::exec
