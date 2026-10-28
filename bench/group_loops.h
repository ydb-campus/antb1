// Step 0 of the GROUP BY key tables (follow-up of docs/adr/0019-own-aggregation-hash-table.md):
// candidate loops that number fixed-width keys, benchmarked in micro_bench.cc against
// arrow::compute::Grouper on the same inputs. Not engine code: the tables here only turn key rows
// into dense first-seen group ids, without NULLs, and exist to decide which loop step 1 builds.
//
// What they share: a fixed-seed one-multiply hash (foldhash's fast tier), 7-bit tags matched eight
// at a time in a 64-bit word (SWAR), control bytes whose first eight are mirrored past the end so
// that an unaligned group load never wraps (hashbrown's layout), a triangular probe that stops at
// the first group holding an empty slot (there are no deletions), and every byte charged to a
// MemoryBudget before it is allocated, never per insert. The loop works a chunk of rows at a time:
// one room check, one pass that hashes the chunk, then the probes, each prefetching the buckets of
// the row a few places on (a loop of one row at a time with a room check per row, DataFusion's
// shape, lost to it). What varies is the slot (Slots below); TableOptions holds the prefetch
// distance and the load factor at which the table doubles.

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include <arrow/status.h>

#include "antb1/common/int128.h"
#include "antb1/exec/memory_budget.h"

namespace antb1::bench::step0 {

// After foldhash 0.2.0 (fast.rs, lib.rs; Zlib licence): fold(x, y) is the low xor the high half
// of the 128-bit product, and a word mixes in as fold(v ^ prev ^ A3, A6), prev being the seed for
// the first word. The seed is fixed, so a table's ids are a function of its input.
inline constexpr std::uint64_t kFoldA3 = 0x082efa98ec4e6c89ULL;
inline constexpr std::uint64_t kFoldA6 = 0xc0ac29b7c97c50ddULL;
inline constexpr std::uint64_t kSeed = 0x243f6a8885a308d3ULL;

inline std::uint64_t Mix(std::uint64_t value, std::uint64_t prev) {
  const UInt128 full = static_cast<UInt128>(value ^ prev ^ kFoldA3) * kFoldA6;
  return static_cast<std::uint64_t>(full) ^ static_cast<std::uint64_t>(full >> 64U);
}

// One BIGINT key.
struct Int64Key {
  struct Columns {
    const std::int64_t* k = nullptr;
  };
  static Int64Key At(const Columns& columns, std::size_t row) { return {columns.k[row]}; }
  [[nodiscard]] std::uint64_t Hash() const { return Mix(static_cast<std::uint64_t>(k), kSeed); }
  friend bool operator==(const Int64Key&, const Int64Key&) = default;

  std::int64_t k;
};

// A BIGINT and an INTEGER key.
struct Int64Int32Key {
  struct Columns {
    const std::int64_t* k = nullptr;
    const std::int32_t* j = nullptr;
  };
  static Int64Int32Key At(const Columns& columns, std::size_t row) {
    return {columns.k[row], columns.j[row]};
  }
  [[nodiscard]] std::uint64_t Hash() const {
    return Mix(static_cast<std::uint32_t>(j), Mix(static_cast<std::uint64_t>(k), kSeed));
  }
  friend bool operator==(const Int64Int32Key&, const Int64Int32Key&) = default;

  std::int64_t k;
  std::int32_t j;
};

// Eight control bytes in one word: EMPTY is 0x80, a full slot holds the hash's top seven bits.
// Match may flag a byte next to a true match that is not one (the borrow of the subtraction);
// the key compare settles it. It never flags an EMPTY byte, whose top bit survives the xor.
struct ControlGroup {
  static constexpr std::size_t kWidth = 8;
  static constexpr std::uint64_t kLo = 0x0101010101010101ULL;
  static constexpr std::uint64_t kHi = 0x8080808080808080ULL;

  static ControlGroup Load(const std::uint8_t* bytes) {
    std::uint64_t word = 0;
    std::memcpy(&word, bytes, sizeof(word));
    return {word};
  }
  [[nodiscard]] std::uint64_t Match(std::uint8_t tag) const {
    const std::uint64_t x = word ^ (kLo * static_cast<std::uint64_t>(tag));
    return (x - kLo) & ~x & kHi;
  }
  [[nodiscard]] std::uint64_t MatchEmpty() const { return word & kHi; }
  // The byte of the lowest flagged slot. Spelt with bit_width rather than the obvious standard
  // call, whose name the repository's spell check rejects (as plan/join_order.cc does).
  static std::size_t Index(std::uint64_t mask) {
    return static_cast<std::size_t>(std::bit_width(mask & (~mask + 1)) - 1) / 8U;
  }

  std::uint64_t word;
};

// What a slot holds beside the control byte: the group id alone, the key then read from the
// id-ordered key array (4-byte slots), or 32 bits of the hash beside the id (8-byte slots), which a
// probe compares before reading the key and which lets the table grow by walking its old slots in
// order: a group's new position is its old home or that plus the old capacity, so the writes stream
// instead of landing at random as they do when the groups are re-placed in id order. Step 0 also
// measured the key beside the id and the whole 64-bit hash beside it; both lost to these.
enum class Slots : std::uint8_t { kIndirect, kHash32 };

struct TableOptions {
  std::size_t load_num = 7;  // the share of slots filled before the table doubles
  std::size_t load_den = 8;
  std::size_t ahead = 8;  // how many rows ahead the chunked loop prefetches; 0 for none
};

template <class Key, Slots kSlots>
class FixedTable {
 public:
  static constexpr std::uint8_t kEmpty = 0x80;
  static constexpr std::size_t kChunk = 256;  // rows per room check and hash pass
  // Positions come from hash bits 32 and up and tags from bits 57 to 63, so a 32-bit stored hash
  // rebuilds both; the two stay apart up to 2^25 slots.
  static constexpr std::size_t kMaxCapacity = std::size_t{1} << 25U;

  FixedTable(exec::MemoryBudget* budget, TableOptions options) : options_(options) {
    reservation_.Reset(budget);
  }

  [[nodiscard]] std::uint32_t num_groups() const {
    return static_cast<std::uint32_t>(keys_.size());
  }
  // The groups' keys in id order.
  [[nodiscard]] const std::vector<Key>& keys() const { return keys_; }
  // Room for `groups` groups before the first row (a diagnostic of what growth costs: sizing up
  // front from anything but the data is not allowed in the engine).
  arrow::Status Reserve(std::size_t groups) { return MakeRoom(groups); }

  // Writes the group of each of `rows` rows into `ids`, a chunk at a time: one room check, one hash
  // pass, then the probes with the buckets of the row options.ahead places on prefetched.
  arrow::Status ConsumeChunked(const typename Key::Columns& columns, std::size_t rows,
                               std::uint32_t* ids) {
    const std::size_t ahead = options_.ahead;
    for (std::size_t start = 0; start < rows; start += kChunk) {
      const std::size_t count = std::min(kChunk, rows - start);
      ARROW_RETURN_NOT_OK(MakeRoom(count));
      for (std::size_t i = 0; i < count; ++i) {
        chunk_keys_[i] = Key::At(columns, start + i);
        chunk_hashes_[i] = chunk_keys_[i].Hash();
      }
      for (std::size_t i = 0; i < count; ++i) {
        if (ahead != 0 && i + ahead < count) {
          const std::size_t pos = Position(chunk_hashes_[i + ahead]);
          __builtin_prefetch(control_.data() + pos);
          __builtin_prefetch(slots_.data() + pos);
        }
        ids[start + i] = FindOrInsert(chunk_keys_[i], chunk_hashes_[i]);
      }
    }
    return arrow::Status::OK();
  }

 private:
  struct Hash32Slot {
    std::uint32_t hash;  // bits 32 to 63 of the hash
    std::uint32_t group;
  };
  using Slot = std::conditional_t<kSlots == Slots::kHash32, Hash32Slot, std::uint32_t>;
  static constexpr bool kStoresHash = kSlots == Slots::kHash32;

  [[nodiscard]] std::size_t Position(std::uint64_t hash) const {
    return static_cast<std::size_t>(hash >> 32U) & mask_;
  }
  static std::uint8_t Tag(std::uint64_t hash) { return static_cast<std::uint8_t>(hash >> 57U); }

  static Slot MakeSlot(std::uint64_t hash, std::uint32_t group) {
    if constexpr (kStoresHash) {
      return Hash32Slot{static_cast<std::uint32_t>(hash >> 32U), group};
    } else {
      return group;
    }
  }
  // The hash a slot keeps, as far as positions and tags need it.
  static std::uint64_t StoredHash(const Slot& slot) {
    return static_cast<std::uint64_t>(slot.hash) << 32U;
  }

  // The bytes the table holds with `capacity` slots and room for `keys` keys.
  [[nodiscard]] static std::int64_t BytesFor(std::size_t capacity, std::size_t keys) {
    const std::size_t bytes =
        capacity + ControlGroup::kWidth + (capacity * sizeof(Slot)) + (keys * sizeof(Key));
    return static_cast<std::int64_t>(bytes + sizeof(chunk_keys_) + sizeof(chunk_hashes_));
  }

  // Room for `extra` more groups without a check: the slots double until the groups fit the load
  // factor and the key array doubles, each charged before it is allocated (the old and the new
  // slots both, while they coexist).
  arrow::Status MakeRoom(std::size_t extra) {
    const std::size_t need = keys_.size() + extra;
    if (need > grow_at_) {
      std::size_t capacity = std::max<std::size_t>(capacity_, 16);
      while (capacity * options_.load_num / options_.load_den < need) {
        capacity *= 2;
      }
      if (capacity > kMaxCapacity) {
        return arrow::Status::CapacityError("step 0 table: more than ", kMaxCapacity, " slots");
      }
      ARROW_RETURN_NOT_OK(Rehash(capacity));
    }
    if (need > keys_.capacity()) {
      const std::size_t keys =
          std::max<std::size_t>(need, std::max<std::size_t>(16, keys_.capacity() * 2));
      ARROW_RETURN_NOT_OK(reservation_.Resize(BytesFor(capacity_, keys)));
      keys_.reserve(keys);
    }
    return arrow::Status::OK();
  }

  arrow::Status Rehash(std::size_t capacity) {
    ARROW_RETURN_NOT_OK(
        reservation_.Resize(BytesFor(capacity_, keys_.capacity()) + BytesFor(capacity, 0)));
    std::vector<std::uint8_t> control(capacity + ControlGroup::kWidth, kEmpty);
    std::vector<Slot> slots(capacity);
    control_.swap(control);
    slots_.swap(slots);
    const std::size_t old_capacity = capacity_;
    capacity_ = capacity;
    mask_ = capacity - 1;
    grow_at_ = capacity * options_.load_num / options_.load_den;
    if constexpr (kStoresHash) {
      for (std::size_t old = 0; old < old_capacity; ++old) {  // `control` and `slots` are the old
        if (control[old] != kEmpty) {
          const std::uint64_t hash = StoredHash(slots[old]);
          Place(FirstEmpty(hash), Tag(hash), slots[old]);
        }
      }
    } else {
      for (std::uint32_t group = 0; group < keys_.size(); ++group) {
        const Key& key = keys_[group];
        const std::uint64_t hash = key.Hash();
        Place(FirstEmpty(hash), Tag(hash), MakeSlot(hash, group));
      }
    }
    control.clear();
    control.shrink_to_fit();
    slots.clear();
    slots.shrink_to_fit();
    return reservation_.Resize(BytesFor(capacity_, keys_.capacity()));
  }

  [[nodiscard]] std::size_t FirstEmpty(std::uint64_t hash) const {
    std::size_t pos = Position(hash);
    for (std::size_t stride = ControlGroup::kWidth;; stride += ControlGroup::kWidth) {
      const std::uint64_t empty = ControlGroup::Load(control_.data() + pos).MatchEmpty();
      if (empty != 0) {
        return (pos + ControlGroup::Index(empty)) & mask_;
      }
      pos = (pos + stride) & mask_;
    }
  }

  void Place(std::size_t slot, std::uint8_t tag, const Slot& value) {
    control_[slot] = tag;
    if (slot < ControlGroup::kWidth) {
      control_[capacity_ + slot] = tag;  // the mirror an unaligned load past the end reads
    }
    slots_[slot] = value;
  }

  [[nodiscard]] bool Holds(std::size_t slot, const Key& key, std::uint64_t hash) const {
    if constexpr (kStoresHash) {
      return slots_[slot].hash == static_cast<std::uint32_t>(hash >> 32U) &&
             keys_[slots_[slot].group] == key;
    } else {
      return keys_[slots_[slot]] == key;
    }
  }
  [[nodiscard]] std::uint32_t GroupAt(std::size_t slot) const {
    if constexpr (kStoresHash) {
      return slots_[slot].group;
    } else {
      return slots_[slot];
    }
  }

  // The group of `key`, inserted as the next id when it is new; MakeRoom has made room.
  std::uint32_t FindOrInsert(const Key& key, std::uint64_t hash) {
    const std::uint8_t tag = Tag(hash);
    std::size_t pos = Position(hash);
    for (std::size_t stride = ControlGroup::kWidth;; stride += ControlGroup::kWidth) {
      const ControlGroup group = ControlGroup::Load(control_.data() + pos);
      for (std::uint64_t match = group.Match(tag); match != 0; match &= match - 1) {
        const std::size_t slot = (pos + ControlGroup::Index(match)) & mask_;
        if (Holds(slot, key, hash)) {
          return GroupAt(slot);
        }
      }
      if (const std::uint64_t empty = group.MatchEmpty(); empty != 0) {
        const auto id = static_cast<std::uint32_t>(keys_.size());
        keys_.push_back(key);  // within the reserved capacity
        Place((pos + ControlGroup::Index(empty)) & mask_, tag, MakeSlot(hash, id));
        return id;
      }
      pos = (pos + stride) & mask_;
    }
  }

  TableOptions options_;
  exec::MemoryReservation reservation_;
  std::vector<std::uint8_t> control_;
  std::vector<Slot> slots_;
  std::vector<Key> keys_;
  std::size_t capacity_ = 0;
  std::size_t mask_ = 0;
  std::size_t grow_at_ = 0;
  std::array<Key, kChunk> chunk_keys_{};
  std::array<std::uint64_t, kChunk> chunk_hashes_{};
};

}  // namespace antb1::bench::step0
