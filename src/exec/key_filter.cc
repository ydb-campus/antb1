#include "antb1/exec/key_filter.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "antb1/common/check.h"
#include "antb1/common/narrow.h"

namespace antb1::exec {

arrow::Result<std::shared_ptr<KeyFilter>> KeyFilter::Make(int64_t num_keys, MemoryBudget* budget) {
  if (num_keys <= 0) {
    return nullptr;
  }
  // Whole blocks, rounded up to a power of two so the block index is a mask and not a division.
  const int64_t bits = num_keys * kBitsPerKey;
  const int64_t words = (bits + 31) / 32;
  const auto blocks_wanted = static_cast<std::uint64_t>(
      (words + Narrow<int64_t>(kWordsPerBlock) - 1) / Narrow<int64_t>(kWordsPerBlock));
  const std::uint64_t blocks = std::bit_ceil(blocks_wanted);
  const int64_t bytes = Narrow<int64_t>(blocks) * Narrow<int64_t>(kWordsPerBlock) *
                        Narrow<int64_t>(sizeof(std::uint32_t));
  if (bytes > kMaxBytes) {
    return nullptr;  // past the cache, a filter costs more than it saves (kMaxBytes)
  }
  // A filter the budget cannot hold is one the probe does without, which is what it does today.
  // Checked before reserving so that a build is never failed by a filter it did not need.
  if (budget != nullptr) {
    const std::optional<int64_t> limit = budget->limit();
    if (limit.has_value() && (bytes > *limit / 8 || budget->bytes_allocated() + bytes > *limit)) {
      return nullptr;
    }
  }
  auto filter = std::shared_ptr<KeyFilter>(new KeyFilter());
  filter->memory_.Reset(budget);
  ARROW_RETURN_NOT_OK(filter->memory_.Resize(bytes));
  filter->words_.assign(blocks * kWordsPerBlock, 0);
  filter->block_mask_ = blocks - 1;
  filter->keys_ = num_keys;
  return filter;
}

void KeyFilter::Add(std::uint64_t hash) {
  std::uint32_t* block = words_.data() + BlockAt(hash);
  for (std::size_t word = 0; word < kWordsPerBlock; ++word) {
    block[word] |= MaskWord(hash, word);
  }
}

void KeyFilter::Add(std::span<const std::uint64_t> hashes) {
  for (const std::uint64_t hash : hashes) {
    Add(hash);
  }
}

void KeyFilter::PublishKeys(std::vector<Int128> keys, const Int128& min, const Int128& max) {
  key_range_ = KeyRange{.min = min, .max = max};
  if (keys.empty()) {
    return;  // the interval alone: above kMaxPublishedKeys the values were never collected
  }
  // Charged on top of the bits, which the reservation already holds. A refusal here keeps the old
  // reservation (MemoryReservation::Resize), so dropping the values is all that is needed: the
  // interval still prunes, and a filter that prunes fewer parts is slower and never wrong.
  const int64_t bytes =
      memory_.bytes() + (Narrow<int64_t>(keys.size()) * Narrow<int64_t>(sizeof(Int128)));
  if (!memory_.Resize(bytes).ok()) {
    return;
  }
  std::ranges::sort(keys);
  key_values_ = std::move(keys);
}

bool KeyFilter::MayHoldAKeyIn(const Int128& min, const Int128& max) const {
  if (!key_range_.has_value()) {
    return true;  // nothing published: this filter rules out no part
  }
  if (!key_values_.empty()) {
    // The first key at or after the part's lowest value; the part holds it when it is also at or
    // before the part's highest. One binary search a part, which is why publishing the values is
    // worth it even when they rule out nothing.
    const auto key = std::ranges::lower_bound(key_values_, min);
    return key != key_values_.end() && *key <= max;
  }
  return max >= key_range_->min && min <= key_range_->max;
}

}  // namespace antb1::exec
