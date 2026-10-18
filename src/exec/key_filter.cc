#include "antb1/exec/key_filter.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>

#include "antb1/common/check.h"
#include "antb1/common/narrow.h"

namespace antb1::exec {
namespace {

// The salts of Parquet's split-block Bloom filter, one per word of a block: multiplying the hash by
// each and taking the top five bits of the product picks one bit of that word, so a key sets eight
// bits spread over the block and no two words are picked the same way.
constexpr std::array<std::uint32_t, 8> kSalt = {0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
                                                0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U};

// The eight words a key sets in its block, one bit each.
std::array<std::uint32_t, 8> MaskOf(std::uint64_t hash) {
  const auto low = static_cast<std::uint32_t>(hash);
  std::array<std::uint32_t, 8> mask{};
  for (std::size_t word = 0; word < mask.size(); ++word) {
    mask[word] = std::uint32_t{1} << ((low * kSalt[word]) >> 27U);
  }
  return mask;
}

// The hash's block. The high bits choose it, because the low ones already chose the bits within it;
// a multiply-shift rather than a modulo, so the count need not be a prime.
std::size_t BlockOf(std::uint64_t hash, std::uint64_t block_mask) {
  return static_cast<std::size_t>((hash >> 32U) & block_mask);
}

}  // namespace

arrow::Result<std::shared_ptr<KeyFilter>> KeyFilter::Make(int64_t keys, MemoryBudget* budget) {
  if (keys <= 0) {
    return nullptr;
  }
  // Whole blocks, rounded up to a power of two so the block index is a mask and not a division.
  const int64_t bits = keys * kBitsPerKey;
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
  filter->words_.assign(static_cast<std::size_t>(blocks) * kWordsPerBlock, 0);
  filter->block_mask_ = blocks - 1;
  filter->keys_ = keys;
  return filter;
}

void KeyFilter::Add(std::uint64_t hash) {
  const std::array<std::uint32_t, 8> mask = MaskOf(hash);
  std::uint32_t* block = words_.data() + (BlockOf(hash, block_mask_) * kWordsPerBlock);
  for (std::size_t word = 0; word < kWordsPerBlock; ++word) {
    block[word] |= mask[word];
  }
}

void KeyFilter::Add(std::span<const std::uint64_t> hashes) {
  for (const std::uint64_t hash : hashes) {
    Add(hash);
  }
}

bool KeyFilter::MayContain(std::uint64_t hash) const {
  const std::array<std::uint32_t, 8> mask = MaskOf(hash);
  const std::uint32_t* block = words_.data() + (BlockOf(hash, block_mask_) * kWordsPerBlock);
  for (std::size_t word = 0; word < kWordsPerBlock; ++word) {
    if ((block[word] & mask[word]) != mask[word]) {
      return false;
    }
  }
  return true;
}

}  // namespace antb1::exec
