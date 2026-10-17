#include "group_hash_table.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/bit_util.h>

#include "antb1/common/narrow.h"

#include "group_table.h"

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

arrow::Status GroupHashTable::Reserve(std::uint32_t groups) {
  const int64_t bytes = Narrow<int64_t>(
      (tags_.capacity() * sizeof(std::uint8_t)) + (slots_.capacity() * sizeof(std::uint32_t)) +
      (static_cast<std::size_t>(groups) * row_) + (hashes_.capacity() * sizeof(std::uint64_t)));
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
  ARROW_RETURN_NOT_OK(Reserve(num_groups_));
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
  ARROW_ASSIGN_OR_RAISE(const std::vector<std::uint64_t> hashes, KeyHashes(keys));
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
  arrow::UInt32Builder ids(pool_);
  ARROW_RETURN_NOT_OK(ids.Reserve(Narrow<int64_t>(rows)));
  std::vector<std::uint8_t> key(row_);
  for (std::size_t row = 0; row < rows; ++row) {
    // The row packed: its NULL bitmap, then each key's bytes, zeroed where it is NULL so that rows
    // NULL in the same keys are one group.
    std::ranges::fill(key, std::uint8_t{0});
    std::size_t at = bitmap_;
    for (std::size_t k = 0; k < columns.size(); ++k) {
      const arrow::Array& column = *columns[k];
      const auto width = static_cast<std::size_t>(widths_[k]);
      if (column.IsValid(Narrow<int64_t>(row))) {
        const std::uint8_t* bytes = column.data()->buffers[1]->data() +
                                    ((static_cast<std::size_t>(column.offset()) + row) * width);
        std::memcpy(key.data() + at, bytes, width);
      } else {
        arrow::bit_util::SetBit(key.data(), Narrow<int64_t>(k));
      }
      at += width;
    }
    // Room for one more group before the slot is found, since growing moves every slot. At the
    // threshold this grows once, whether or not the row turns out to be a group already there.
    if ((static_cast<std::size_t>(num_groups_) + 1) * kLoadDenominator >
        (mask_ + 1) * kLoadNumerator) {
      ARROW_RETURN_NOT_OK(Resize((mask_ + 1) * 2));
    }
    const std::uint64_t hash = hashes[row];
    const Probe probe = Find(hash, key.data());
    if (probe.found) {
      ids.UnsafeAppend(slots_[probe.slot]);
      continue;
    }
    const std::uint32_t group = num_groups_;
    try {
      keys_.insert(keys_.end(), key.begin(), key.end());
      hashes_.push_back(hash);
    } catch (const std::bad_alloc&) {
      return arrow::Status::OutOfMemory("out of memory in a group hash table");
    }
    ARROW_RETURN_NOT_OK(Reserve(group + 1));
    num_groups_ = group + 1;
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
