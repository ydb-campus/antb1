#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <arrow/result.h>

#include "antb1/exec/memory_budget.h"

namespace antb1::exec {

// A Bloom filter over the key hashes of a hash join's build, for the scan of its probe side to
// reject rows that can match nothing (docs/adr/0029-dynamic-join-filters.md, D1). It answers one
// question -- "is this hash absent?" -- and it must never answer yes for a hash that is present:
// a false positive costs one row's work, a false negative would lose an answer.
//
// The layout is the split-block filter Parquet's own Bloom filters use: one 32-byte block a key,
// chosen by the hash's high bits, with one bit set in each of the block's eight words. Every insert
// and every test touches a single cache line, which is what makes testing hundreds of millions of
// probe rows worth doing at all.
//
// Filled once, on the thread that finished the build, and then only read; `MayContain` is const and
// any number of probe threads call it at once.
class KeyFilter {
 public:
  KeyFilter(const KeyFilter&) = delete;
  KeyFilter& operator=(const KeyFilter&) = delete;
  KeyFilter(KeyFilter&&) = delete;
  KeyFilter& operator=(KeyFilter&&) = delete;
  ~KeyFilter() = default;

  // The bits per key the size is chosen at, which fixes the false-positive rate (about 0.5% for a
  // filter this full, and lower for one the rounding up to whole blocks leaves emptier).
  static constexpr int64_t kBitsPerKey = 16;

  // The largest filter worth building. A filter is probed once a row with no locality at all, so it
  // pays only while it stays in cache: past that every probe row costs a memory round trip, and the
  // filter loses more than the rows it rejects save. Measured at SF 100 on the 22: filters of 6 to
  // 14 MB are worth 0.73x to 0.96x on their queries, and a 45 MB one cost 17% on a query whose
  // build holds a sixth of its probe's keys -- so the boundary is between them, and this is the
  // conservative side of it.
  static constexpr int64_t kMaxBytes = int64_t{16} * 1024 * 1024;

  // A filter sized for `num_keys` of them, every bit clear, charged to `budget` (nullptr: no
  // budget). Null -- not an error -- when `num_keys` is not positive or when the filter would not
  // fit the
  // budget: a filter that cannot be afforded is one the probe does without, exactly as it does
  // today, which is why this is not an OutOfMemory. OutOfMemory only if the reservation the size
  // test already approved then fails.
  static arrow::Result<std::shared_ptr<KeyFilter>> Make(int64_t num_keys, MemoryBudget* budget);

  // Adds `hash`. Not thread-safe: the build fills the filter on one thread before any probe reads
  // it.
  void Add(std::uint64_t hash);
  // Adds every hash of `hashes`.
  void Add(std::span<const std::uint64_t> hashes);

  // Whether `hash` may have been added: false means it certainly was not. Const and callable from
  // any number of threads at once, with no synchronisation of its own -- the build's completion
  // already orders every Add before the first probe (hash_join.cc's PrepareBuilds).
  //
  // Defined here, not in the .cc, because the probe calls it once a row from another translation
  // unit and the build has no link-time optimization: out of line it would cost a call and the
  // eight multiplications of the mask could not fold into the caller.
  [[nodiscard]] bool MayContain(std::uint64_t hash) const {
    const std::uint32_t* block = BlockOf(hash);
    for (std::size_t word = 0; word < kWordsPerBlock; ++word) {
      if ((block[word] & MaskWord(hash, word)) == 0) {
        return false;
      }
    }
    return true;
  }

  // Warms the cache line `MayContain(hash)` will read. A probe tests one row after another with no
  // locality at all, so the read is a cache miss whenever the filter outgrows the cache -- and the
  // caller knows every hash of its batch before it tests the first, so it can ask for the line of a
  // later row now. ADR 0019 measured the same hint on a hash bucket at 44.16 ns a row to 20.14.
  void Prefetch(std::uint64_t hash) const { __builtin_prefetch(BlockOf(hash)); }

  // The keys it was sized for, and its bytes.
  [[nodiscard]] int64_t keys() const { return keys_; }
  [[nodiscard]] int64_t bytes() const { return memory_.bytes(); }

 private:
  KeyFilter() = default;

  // One block a cache line, eight 32-bit words of it.
  static constexpr std::size_t kWordsPerBlock = 8;

  // The salts of Parquet's split-block Bloom filter, one a word: multiplying the hash by each and
  // taking the top five bits of the product picks one bit of that word, so a key sets eight bits
  // spread over its block and no two words are picked the same way.
  static constexpr std::array<std::uint32_t, kWordsPerBlock> kSalt = {
      0x47b6137bU, 0x44974d91U, 0x8824ad5bU, 0xa2b7289dU,
      0x705495c7U, 0x2df1424bU, 0x9efc4947U, 0x5c6bfb31U};

  // The one bit `hash` sets in word `word` of its block.
  static std::uint32_t MaskWord(std::uint64_t hash, std::size_t word) {
    return std::uint32_t{1} << ((static_cast<std::uint32_t>(hash) * kSalt[word]) >> 27U);
  }
  // Where the hash's block starts in `words_`. The high bits choose it, because the low ones
  // already chose the bits within it; a mask rather than a modulo, so the count need not be a
  // prime.
  [[nodiscard]] std::size_t BlockAt(std::uint64_t hash) const {
    // No cast: the masked hash is a uint64_t, which *is* size_t on linux-64, so GCC reads a cast
    // here as useless and -Werror stops the build. Both are 64-bit unsigned on every platform this
    // builds for, so the implicit conversion changes no value and warns nowhere.
    return ((hash >> 32U) & block_mask_) * kWordsPerBlock;
  }
  [[nodiscard]] const std::uint32_t* BlockOf(std::uint64_t hash) const {
    return words_.data() + BlockAt(hash);
  }

  std::vector<std::uint32_t> words_;  // kWordsPerBlock per block, a power of two of blocks
  std::uint64_t block_mask_ = 0;      // blocks - 1
  int64_t keys_ = 0;
  MemoryReservation memory_;
};

}  // namespace antb1::exec
