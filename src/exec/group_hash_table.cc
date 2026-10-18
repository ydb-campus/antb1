#include "group_hash_table.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/bit_util.h>

#include "antb1/common/narrow.h"

namespace antb1::exec {
namespace {

constexpr std::uint32_t kNoGroup = 0xFFFFFFFFU;
// Slots are probed eight at a time, as one 64-bit word of tags.
constexpr std::size_t kTagsPerWord = 8;
// The table grows past this share of its slots. Eight-slot words want room to spare: at seven
// eighths full a probe reads several words, which is what linear probing costs and tags avoid.
constexpr std::size_t kLoadNumerator = 7;
constexpr std::size_t kLoadDenominator = 10;
constexpr std::size_t kMinCapacity = 64;
// 0x0101..., 0x8080...: the two constants a byte-wise equality needs.
constexpr std::uint64_t kOnes = 0x0101010101010101ULL;
constexpr std::uint64_t kHighs = 0x8080808080808080ULL;

// The bytes of `word` that equal `byte`, as the high bit of each such byte. The classic byte-wise
// search: subtracting one from a zero byte borrows into its high bit and from no other.
std::uint64_t MatchingBytes(std::uint64_t word, std::uint8_t byte) {
  const std::uint64_t x = word ^ (kOnes * byte);
  return (x - kOnes) & ~x & kHighs;
}

// Which byte of a word MatchingBytes marked first: the lowest set bit's byte. `x & (~x + 1)` keeps
// only that bit, and bit_width then counts it.
std::size_t FirstMatch(std::uint64_t matches) {
  return static_cast<std::size_t>(std::bit_width(matches & (~matches + 1)) - 1) / 8U;
}

// The tag of a hash: its top seven bits, never 0, which is what an empty slot holds.
std::uint8_t TagOf(std::uint64_t hash) { return static_cast<std::uint8_t>(0x80U | (hash >> 57U)); }

std::size_t CapacityFor(int64_t groups) {
  const auto wanted = static_cast<std::size_t>(std::max<int64_t>(groups, 0));
  std::size_t capacity = kMinCapacity;
  while ((capacity * kLoadNumerator) / kLoadDenominator < wanted) {
    capacity *= 2;
  }
  return capacity;
}

}  // namespace

bool GroupHashTable::Supports(const std::vector<arrow::TypeHolder>& types) {
  if (types.empty()) {
    return false;  // a table without keys has one group, which GroupTable handles itself
  }
  return std::ranges::all_of(types, [](const arrow::TypeHolder& type) {
    return type.type != nullptr && type.id() != arrow::Type::BOOL && type.type->byte_width() > 0;
  });
}

GroupHashTable::GroupHashTable(std::vector<arrow::TypeHolder> types, std::vector<int> widths,
                               std::size_t bitmap, arrow::MemoryPool* pool)
    : types_(std::move(types)), widths_(std::move(widths)), bitmap_(bitmap), pool_(pool) {
  row_ = bitmap_;
  for (const int width : widths_) {
    row_ += static_cast<std::size_t>(width);
  }
  // Rounded up to whole words, so that hashing and comparing a row are word loops with no ragged
  // tail and every row starts aligned. One BIGINT key packs to nine bytes without this -- a NULL
  // bitmap byte and the key -- which costs a byte-at-a-time tail in both, for every row.
  row_ = (row_ + (sizeof(std::uint64_t) - 1)) & ~(sizeof(std::uint64_t) - 1);
}

arrow::Result<std::unique_ptr<GroupHashTable>> GroupHashTable::Make(
    std::vector<arrow::TypeHolder> types, arrow::MemoryPool* pool, MemoryBudget* budget,
    int64_t expected_groups) {
  if (!Supports(types)) {
    return arrow::Status::Invalid("a group hash table of keys it does not hold");
  }
  std::vector<int> widths;
  widths.reserve(types.size());
  for (const arrow::TypeHolder& type : types) {
    widths.push_back(type.type->byte_width());
  }
  const auto bitmap =
      static_cast<std::size_t>(arrow::bit_util::BytesForBits(Narrow<int64_t>(types.size())));
  auto table = std::unique_ptr<GroupHashTable>(
      new GroupHashTable(std::move(types), std::move(widths), bitmap, pool));
  table->memory_.Reset(budget);
  ARROW_RETURN_NOT_OK(table->Resize(CapacityFor(expected_groups)));
  return table;
}

arrow::Status GroupHashTable::Reserve() {
  const int64_t bytes = Narrow<int64_t>(
      (tags_.capacity() * sizeof(std::uint8_t)) + (slots_.capacity() * sizeof(std::uint32_t)) +
      keys_.capacity() + (hashes_.capacity() * sizeof(std::uint64_t)));
  if (bytes == charged_) {
    return arrow::Status::OK();
  }
  charged_ = bytes;
  return memory_.Resize(bytes);
}

arrow::Status GroupHashTable::Resize(std::size_t capacity) {
  std::vector<std::uint8_t> tags;
  std::vector<std::uint32_t> slots;
  try {
    tags.assign(capacity, 0);
    slots.assign(capacity, kNoGroup);
  } catch (const std::bad_alloc&) {
    return arrow::Status::OutOfMemory("out of memory in a group hash table");
  }
  tags_.swap(tags);
  slots_.swap(slots);
  mask_ = capacity - 1;
  ARROW_RETURN_NOT_OK(Reserve());
  // Every group back into the new slots, by the hash it was inserted with: no key is read again.
  for (std::uint32_t group = 0; group < num_groups_; ++group) {
    const std::uint64_t hash = hashes_[group];
    std::size_t at = static_cast<std::size_t>(hash) & mask_ & ~(kTagsPerWord - 1);
    while (true) {
      std::uint64_t word = 0;
      std::memcpy(&word, tags_.data() + at, sizeof(word));
      const std::uint64_t empty = MatchingBytes(word, 0);
      if (empty != 0) {
        const std::size_t slot = at + FirstMatch(empty);
        tags_[slot] = TagOf(hash);
        slots_[slot] = group;
        break;
      }
      at = (at + kTagsPerWord) & mask_;
    }
  }
  return arrow::Status::OK();
}

std::uint64_t GroupHashTable::HashRow(const std::uint8_t* row) const {
  // A multiply-xor over the row's bytes, eight at a time, with a final avalanche: the tag takes the
  // top seven bits and the bucket the low ones, so both ends have to move.
  std::uint64_t h = 0x9E3779B97F4A7C15ULL;
  std::size_t i = 0;
  for (; i + sizeof(std::uint64_t) <= row_; i += sizeof(std::uint64_t)) {
    std::uint64_t word = 0;
    std::memcpy(&word, row + i, sizeof(word));
    h = (h ^ word) * 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33U;
  }
  h ^= h >> 29U;
  h *= 0xC4CEB9FE1A85EC53ULL;
  return h ^ (h >> 32U);
}

GroupHashTable::Probe GroupHashTable::Find(std::uint64_t hash, const std::uint8_t* key) const {
  const std::uint8_t tag = TagOf(hash);
  std::size_t at = static_cast<std::size_t>(hash) & mask_ & ~(kTagsPerWord - 1);
  while (true) {
    std::uint64_t word = 0;
    std::memcpy(&word, tags_.data() + at, sizeof(word));
    // The slots whose tag matches: a hit is almost always the first of them, and a tag that matches
    // a different key is one in 128, so the key bytes are read about once per probe.
    for (std::uint64_t hits = MatchingBytes(word, tag); hits != 0; hits &= hits - 1) {
      const std::size_t slot = at + FirstMatch(hits);
      const std::uint32_t group = slots_[slot];
      if (std::memcmp(keys_.data() + (static_cast<std::size_t>(group) * row_), key, row_) == 0) {
        return Probe{.slot = slot, .found = true};
      }
    }
    // An empty slot in this word ends the search: an insert would have taken it.
    const std::uint64_t empty = MatchingBytes(word, 0);
    if (empty != 0) {
      return Probe{.slot = at + FirstMatch(empty), .found = false};
    }
    at = (at + kTagsPerWord) & mask_;
  }
}

arrow::Result<std::shared_ptr<arrow::UInt32Array>> GroupHashTable::Consume(
    const arrow::compute::ExecBatch& keys) {
  if (keys.values.size() != types_.size()) {
    return arrow::Status::Invalid("a group hash table of ", types_.size(), " keys given ",
                                  keys.values.size());
  }
  const auto rows = static_cast<std::size_t>(keys.length);
  // The key columns as arrays once, not per row.
  std::vector<std::shared_ptr<arrow::Array>> columns;
  columns.reserve(keys.values.size());
  for (const arrow::Datum& datum : keys.values) {
    columns.push_back(datum.make_array());
    if (!columns.back()->type()->Equals(*types_[columns.size() - 1].type)) {
      return arrow::Status::Invalid("a group hash table of ",
                                    types_[columns.size() - 1].type->ToString(), " given ",
                                    columns.back()->type()->ToString());
    }
  }
  // Pack every row of the batch first, one key column at a time, then hash the packed rows, then
  // probe. The per-column work -- its width, its offset in the row, whether it has NULLs at all --
  // leaves the inner loop that way, and each pass walks memory in order. Doing all three per row
  // instead was 2.7x the grouper at a thousand groups
  // (docs/adr/0019-own-aggregation-hash-table.md).
  try {
    // assign, not resize: the buffer is reused between batches, and a row's NULL bitmap bit is only
    // ever set here, never cleared -- so a row position that held a NULL in an earlier batch would
    // read as NULL again and collapse distinct keys into the NULL group. Zeroing the whole buffer
    // is worth about 0.1 ns a row; the bug it prevents cost 26 tests.
    packed_.assign(rows * row_, std::uint8_t{0});
  } catch (const std::bad_alloc&) {
    return arrow::Status::OutOfMemory("out of memory in a group hash table");
  }
  std::size_t at = bitmap_;
  for (std::size_t k = 0; k < columns.size(); ++k) {
    const arrow::Array& column = *columns[k];
    const auto width = static_cast<std::size_t>(widths_[k]);
    const std::uint8_t* bytes =
        column.data()->buffers[1]->data() + (static_cast<std::size_t>(column.offset()) * width);
    std::uint8_t* out = packed_.data() + at;
    if (column.null_count() == 0) {  // the common case: no validity test per row
      for (std::size_t row = 0; row < rows; ++row) {
        std::memcpy(out + (row * row_), bytes + (row * width), width);
      }
    } else {
      for (std::size_t row = 0; row < rows; ++row) {
        if (column.IsValid(Narrow<int64_t>(row))) {
          std::memcpy(out + (row * row_), bytes + (row * width), width);
        } else {
          // Zeroed already by the assign above, so that rows NULL in the same keys are one group.
          arrow::bit_util::SetBit(packed_.data() + (row * row_), Narrow<int64_t>(k));
        }
      }
    }
    at += width;
  }
  try {
    row_hashes_.resize(rows);
  } catch (const std::bad_alloc&) {
    return arrow::Status::OutOfMemory("out of memory in a group hash table");
  }
  {
    static const char* pe = std::getenv("ANTB1_PHASE");
    if (pe == nullptr || std::string_view(pe) != "pack") {
      for (std::size_t row = 0; row < rows; ++row) {
        row_hashes_[row] = HashRow(packed_.data() + (row * row_));
      }
    }
  }
  // TEMPORARY, NOT FOR MERGE: ANTB1_PHASE=pack|hash stops after that phase, to time them apart.
  static const char* phase_env = std::getenv("ANTB1_PHASE");
  static const std::string_view phase = phase_env == nullptr ? "" : phase_env;
  if (phase == "pack" || phase == "hash") {
    return std::static_pointer_cast<arrow::UInt32Array>(
        *arrow::MakeArrayOfNull(arrow::uint32(), Narrow<int64_t>(rows)));
  }
  arrow::UInt32Builder ids(pool_);
  ARROW_RETURN_NOT_OK(ids.Reserve(Narrow<int64_t>(rows)));
  // The probe of one row is a cache miss once the table leaves cache, and a row-at-a-time loop
  // waits for each one: the next bucket's address is known long before it is needed, so the loop
  // asks for it some rows ahead and keeps several misses in flight at once.
  constexpr std::size_t kPrefetchAhead = 8;
  for (std::size_t row = 0; row < rows; ++row) {
    if (row + kPrefetchAhead < rows) {
      const std::size_t ahead =
          static_cast<std::size_t>(row_hashes_[row + kPrefetchAhead]) & mask_ & ~(kTagsPerWord - 1);
      __builtin_prefetch(tags_.data() + ahead);
    }
    const std::uint8_t* key = packed_.data() + (row * row_);
    // Room for one more group before the slot is found, since growing moves every slot. At the
    // threshold this grows once, whether or not the row turns out to be a group already there.
    if ((static_cast<std::size_t>(num_groups_) + 1) * kLoadDenominator >
        (mask_ + 1) * kLoadNumerator) {
      ARROW_RETURN_NOT_OK(Resize((mask_ + 1) * 2));
    }
    const std::uint64_t hash = row_hashes_[row];
    const Probe probe = Find(hash, key);
    if (probe.found) {
      ids.UnsafeAppend(slots_[probe.slot]);
      continue;
    }
    const std::uint32_t group = num_groups_;
    try {
      keys_.insert(keys_.end(), key, key + row_);
      hashes_.push_back(hash);
    } catch (const std::bad_alloc&) {
      return arrow::Status::OutOfMemory("out of memory in a group hash table");
    }
    num_groups_ = group + 1;
    ARROW_RETURN_NOT_OK(Reserve());
    tags_[probe.slot] = TagOf(hash);
    slots_[probe.slot] = group;
    ids.UnsafeAppend(group);
  }
  std::shared_ptr<arrow::Array> out;
  ARROW_RETURN_NOT_OK(ids.Finish(&out));
  return std::static_pointer_cast<arrow::UInt32Array>(out);
}

arrow::Result<arrow::compute::ExecBatch> GroupHashTable::GetUniques() const {
  std::vector<arrow::Datum> values;
  values.reserve(types_.size());
  std::size_t at = bitmap_;
  for (std::size_t k = 0; k < types_.size(); ++k) {
    const auto width = static_cast<std::size_t>(widths_[k]);
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> data,
                          arrow::AllocateBuffer(Narrow<int64_t>(num_groups_ * width), pool_));
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> valid,
                          arrow::AllocateBitmap(Narrow<int64_t>(num_groups_), pool_));
    std::uint8_t* const bytes = data->mutable_data();
    std::uint8_t* const bits = valid->mutable_data();
    int64_t nulls = 0;
    for (std::uint32_t group = 0; group < num_groups_; ++group) {
      const std::uint8_t* row = keys_.data() + (static_cast<std::size_t>(group) * row_);
      const bool null = arrow::bit_util::GetBit(row, static_cast<std::uint64_t>(k));
      arrow::bit_util::SetBitTo(bits, static_cast<int64_t>(group), !null);
      nulls += null ? 1 : 0;
      std::memcpy(bytes + (static_cast<std::size_t>(group) * width), row + at, width);
    }
    values.emplace_back(arrow::ArrayData::Make(types_[k].GetSharedPtr(), num_groups_,
                                               {nulls == 0 ? nullptr : valid, data}, nulls));
    at += width;
  }
  return arrow::compute::ExecBatch(std::move(values), num_groups_);
}

}  // namespace antb1::exec
