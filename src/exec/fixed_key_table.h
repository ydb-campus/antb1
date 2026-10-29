#pragma once

// Private to exec: the key table of one or two fixed-width keys, step 1 of
// docs/adr/0019-own-aggregation-hash-table.md (GroupTable takes it through MakeKeyTable).

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <arrow/array/data.h>
#include <arrow/buffer.h>
#include <arrow/compute/exec.h>
#include <arrow/datum.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type.h>
#include <arrow/util/bit_util.h>

#include "antb1/common/check.h"
#include "antb1/common/int128.h"

#include "key_table.h"
#include "pool_vector.h"

namespace antb1::exec {

// The hash of a FixedKeyTable: the mixing scheme of foldhash 0.2.0's fast tier. fold(x, y) is the
// low xor the high half of the 128-bit product (a folded multiply, as wyhash and others use), and a
// word mixes in as fold(v ^ prev ^ A3, A6), prev being the seed for the first word. The three
// constants are hexadecimal digits of pi, the ones foldhash picks for the same roles. The seed is
// fixed, so a table's ids are a function of its input. The table takes a slot from the hash's low
// bits, which the high half of the product makes depend on every bit of the key, and a tag from its
// top seven.
struct FoldHash {
  static constexpr std::uint64_t kFoldA3 = 0x082efa98ec4e6c89ULL;
  static constexpr std::uint64_t kFoldA6 = 0xc0ac29b7c97c50ddULL;
  static constexpr std::uint64_t kSeed = 0x243f6a8885a308d3ULL;

  static std::uint64_t Mix(std::uint64_t value, std::uint64_t prev) {
    const UInt128 full = static_cast<UInt128>(value ^ prev ^ kFoldA3) * kFoldA6;
    return static_cast<std::uint64_t>(full) ^ static_cast<std::uint64_t>(full >> 64U);
  }
  // A key's words, mixed in one after another.
  template <std::size_t kWords>
  static std::uint64_t Hash(const std::array<std::uint64_t, kWords>& words) {
    std::uint64_t hash = kSeed;
    for (const std::uint64_t word : words) {
      hash = Mix(word, hash);
    }
    return hash;
  }
};

// How a FixedKeyTable lays out a row's keys as one key of whole eight-byte words, which it hashes
// and compares a word at a time:
// - one key: the value's bytes, zero-extended to 8 bytes (a type of 2, 4 or 8) or 16 (a DECIMAL of
//   more than 18 digits, HUGEINT). A NULL is not a key: the table keeps its group apart;
// - two keys: the values' bytes one after the other, a byte whose bit c is set when column c is
//   NULL, then zeros up to a whole word (BIGINT and INTEGER: 8 + 4 + 1 bytes in 16). A NULL value's
//   bytes are zeros, so the bytes under it in the input never matter.
struct FixedKeyLayout {
  struct Column {
    std::shared_ptr<arrow::DataType> type;  // exactly the type the table was made for
    std::size_t width = 0;                  // a value's bytes: 2, 4, 8 or 16
    std::size_t offset = 0;                 // where they start in the key
  };
  std::vector<Column> columns;  // one or two
  std::size_t key_bytes = 0;    // 8, 16, 24, 32 or 40
  std::size_t mask_offset = 0;  // two keys: the NULL byte's place in the key
};

// The layout of keys of `types`, or nullopt when a FixedKeyTable does not take them. It takes one
// or two keys of the fixed-width types plan::ToArrow gives a key, whose values are equal exactly
// when their bytes are: SMALLINT, USMALLINT, INTEGER, DATE, BIGINT, TIMESTAMP, DOUBLE (normalized,
// double_key.h) and DECIMAL of either width. BOOLEAN (bits), VARCHAR and three keys or more go to
// arrow's Grouper.
std::optional<FixedKeyLayout> FixedKeyLayoutOf(const std::vector<arrow::TypeHolder>& types);

// A key of `kBytes` bytes (FixedKeyLayout).
template <std::size_t kBytes>
struct FixedKey {
  static_assert(kBytes % 8 == 0);
  std::array<std::uint64_t, kBytes / 8> words;
  friend bool operator==(const FixedKey&, const FixedKey&) = default;
};

// How a FixedKeyTable re-places its groups when its slots double; neither changes an id.
// - kSlotOrder walks the old slots in order and rehashes each group's key: the key reads are random
//   (and prefetched), but the writes stream, since a group's new home is its old one or that plus
//   the old capacity;
// - kIdOrder re-places the groups in id order: the key reads stream, the writes land at random.
// Step 0 measured both (PR #253): on a Xeon id order fell below arrow's Grouper at 4 Mi groups
// whose ids never repeat early, and on an M4 Pro it was 10-48% faster from 256 Ki groups up. The
// engine grows in slot order; KeyTableKind::kFixedIdOrder runs the other in tests.
enum class Growth : std::uint8_t { kSlotOrder, kIdOrder };

// Eight control bytes in one word: EMPTY is 0x80, a full slot holds the hash's top seven bits.
// Match may flag a byte next to a true match that is not one (the borrow of the subtraction); the
// key compare settles it. It never flags an EMPTY byte, whose top bit survives the xor. Byte i of
// the word is the control byte at i, so the lowest flagged byte is the first slot.
struct ControlGroup {
  static_assert(std::endian::native == std::endian::little);
  static constexpr std::size_t kWidth = 8;
  static constexpr std::uint64_t kLo = 0x0101010101010101ULL;
  static constexpr std::uint64_t kHi = 0x8080808080808080ULL;

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

// The key table of one or two fixed-width keys (FixedKeyLayout), built from the loop step 0 of
// docs/adr/0019-own-aggregation-hash-table.md chose. It numbers groups in the order their first
// rows come. A slot holds a group id alone (4 bytes); the keys sit in an array in id order, which a
// probe reads to compare. It uses a fixed-seed one-multiply hash (Hasher, FoldHash in the engine),
// 7-bit tags matched eight at a time in a 64-bit word (SWAR, the well-known zero-byte test),
// control bytes whose first eight are mirrored past the end so that an unaligned group load never
// wraps, and a triangular probe that stops at the first group holding an empty slot (there are no
// deletions): the design of Rust's hashbrown, written here from its description, not its code.
// Rows go a chunk at a time: one room check, a pass that packs the chunk's keys, one that hashes
// them, then the probes, each prefetching the slots of the row a few places on. The slots double at
// a load of 7/8, or are sized ahead once from the table's own first two calls (options.estimate,
// Estimate). Every array comes from the pool, so a MemoryBudget charges each allocation before it
// happens, never a row.
template <std::size_t kKeyBytes, Growth kGrowth, class Hasher = FoldHash>
class FixedKeyTable final : public KeyTable {
 public:
  using Key = FixedKey<kKeyBytes>;
  static constexpr std::size_t kChunk = 256;  // rows per room check and hash pass
  static constexpr std::size_t kAhead = 8;    // how many rows ahead a probe prefetches

  // A table over keys of `layout` (whose key_bytes is kKeyBytes) that allocates from `pool`.
  FixedKeyTable(FixedKeyLayout layout, arrow::MemoryPool* pool, const KeyTableOptions& options)
      : layout_(std::move(layout)),
        pool_(pool),
        estimate_(options.estimate),
        max_groups_(options.max_groups),
        control_(pool),
        slots_(pool),
        keys_(pool),
        chunk_keys_(pool),
        chunk_hashes_(pool),
        chunk_nulls_(pool) {
    ANTB1_CHECK(layout_.key_bytes == kKeyBytes);
    ANTB1_CHECK(layout_.columns.size() == 1 || layout_.columns.size() == 2);
    ANTB1_CHECK(max_groups_ > 0 && max_groups_ < kNoGroup);
  }

  arrow::Result<KeyIds> Consume(const arrow::compute::ExecBatch& keys) override {
    return NoBadAlloc([&] -> arrow::Result<KeyIds> { return ConsumeRows(keys); });
  }

  [[nodiscard]] arrow::Result<arrow::compute::ExecBatch> Uniques() const override {
    return NoBadAlloc([&] -> arrow::Result<arrow::compute::ExecBatch> { return BuildUniques(); });
  }

  [[nodiscard]] std::uint32_t num_groups() const override {
    return static_cast<std::uint32_t>(keys_.size());
  }
  // The slots: 0 before the first row, then a power of two from 16.
  [[nodiscard]] std::size_t capacity() const { return capacity_; }

 private:
  static constexpr std::uint8_t kEmpty = 0x80;
  static constexpr std::size_t kMinCapacity = 16;  // more than one group wide: see Find
  static constexpr std::size_t kLoadNum = 7;       // the share of slots filled before they double
  static constexpr std::size_t kLoadDen = 8;
  // Never an id: max_groups is below it.
  static constexpr std::uint32_t kNoGroup = std::numeric_limits<std::uint32_t>::max();
  // How far ahead slot-order growth prefetches keys.
  static constexpr std::size_t kRehashAhead = 8;
  // Below this many hits the second call says too little about the groups to size the table by.
  static constexpr std::size_t kMinHits = 64;
  // A reservation never passes this many times the groups already seen.
  static constexpr std::size_t kMaxGrowth = 8;

  // Where a key column's rows are.
  struct Source {
    const std::uint8_t* values = nullptr;    // row 0's bytes; nullptr when every row is NULL
    const std::uint8_t* validity = nullptr;  // nullptr when no row is NULL
    std::uint64_t offset = 0;                // row 0's bit in `validity`
  };

  // The arrays a probe reads, as locals of its loop: a store of a control byte may alias anything,
  // so members would be read again after every insert.
  struct View {
    std::uint8_t* control;
    std::uint32_t* slots;
    const Key* keys;
    std::size_t mask;
  };

  static std::byte* BytesOf(Key& key) {
    return std::as_writable_bytes(std::span(key.words)).data();
  }
  static const std::byte* BytesOf(const Key& key) {
    return std::as_bytes(std::span(key.words)).data();
  }
  static std::uint8_t Tag(std::uint64_t hash) { return static_cast<std::uint8_t>(hash >> 57U); }

  static Source SourceOf(const arrow::ArrayData& data, std::size_t width) {
    Source source;
    if (data.buffers.size() > 1 && data.buffers[1] != nullptr) {
      source.values = data.buffers[1]->data() + (static_cast<std::size_t>(data.offset) * width);
    } else {
      ANTB1_CHECK(data.GetNullCount() == data.length);  // the format allows that, and only that
    }
    if (data.MayHaveNulls()) {
      source.validity = data.buffers[0]->data();
      source.offset = static_cast<std::uint64_t>(data.offset);
    }
    return source;
  }

  static bool IsNull(const Source& source, std::size_t row) {
    return source.values == nullptr ||
           (source.validity != nullptr &&
            !arrow::bit_util::GetBit(source.validity, source.offset + row));
  }

  arrow::Result<KeyIds> ConsumeRows(const arrow::compute::ExecBatch& keys) {
    ANTB1_CHECK(keys.values.size() == layout_.columns.size());
    const auto rows = static_cast<std::size_t>(keys.length);
    std::array<Source, 2> sources;
    for (std::size_t c = 0; c < layout_.columns.size(); ++c) {
      ANTB1_CHECK(keys.values[c].is_array());
      sources[c] = SourceOf(*keys.values[c].array(), layout_.columns[c].width);
    }
    const std::span<const Source> columns(sources.data(), layout_.columns.size());
    KeyIds out;
    out.before = num_groups();
    ARROW_ASSIGN_OR_RAISE(
        std::shared_ptr<arrow::Buffer> owner,
        arrow::AllocateBuffer(static_cast<std::int64_t>(rows * sizeof(std::uint32_t)), pool_));
    auto* const ids = owner->mutable_data_as<std::uint32_t>();
    if (rows > 0 && chunk_hashes_.empty()) {
      ARROW_RETURN_NOT_OK(chunk_keys_.Resize(kChunk));
      ARROW_RETURN_NOT_OK(chunk_hashes_.Assign(kChunk, 0));
      ARROW_RETURN_NOT_OK(chunk_nulls_.Assign(kChunk, 0));
    }
    for (std::size_t start = 0; start < rows; start += kChunk) {
      const std::size_t count = std::min(kChunk, rows - start);
      // Room for every row to be a new group, up to the limit that Insert enforces: a chunk of
      // known keys never fails for it.
      ARROW_RETURN_NOT_OK(MakeRoom(std::min<std::size_t>(count, max_groups_ - keys_.size())));
      const bool nulls = PackAndHash(columns, start, count);
      ARROW_RETURN_NOT_OK(nulls ? Probe<true>(count, ids + start)
                                : Probe<false>(count, ids + start));
    }
    out.after = num_groups();
    if (estimate_ && rows > 0) {
      ARROW_RETURN_NOT_OK(Estimate(rows, ids));
    }
    out.ids = {ids, rows};
    out.owner = std::move(owner);
    return out;
  }

  // Packs rows [start, start + count) of `columns` into chunk_keys_ (FixedKeyLayout) and hashes
  // them into chunk_hashes_. With one key its NULL rows are flagged in chunk_nulls_ instead, and
  // the result is whether any row may be one. Each key is built whole for its widths, known at
  // compile time here, then stored and hashed in the same pass: no key is written piecewise and
  // read back.
  bool PackAndHash(std::span<const Source> columns, std::size_t start, std::size_t count) {
    if (columns.size() == 1) {
      switch (layout_.columns[0].width) {
        case 2:
          return PackOne<2>(columns[0], start, count);
        case 4:
          return PackOne<4>(columns[0], start, count);
        case 8:
          return PackOne<8>(columns[0], start, count);
        default:
          return PackOne<16>(columns[0], start, count);
      }
    }
    switch (layout_.columns[0].width) {
      case 2:
        PackTwoAfter<2>(columns, start, count);
        break;
      case 4:
        PackTwoAfter<4>(columns, start, count);
        break;
      case 8:
        PackTwoAfter<8>(columns, start, count);
        break;
      default:
        PackTwoAfter<16>(columns, start, count);
        break;
    }
    return false;
  }

  template <std::size_t kWidth>
  bool PackOne(const Source& source, std::size_t start, std::size_t count) {
    if constexpr (kWidth > kKeyBytes) {
      ANTB1_CHECK(kWidth <= kKeyBytes);  // the layout picked kKeyBytes for this width
      return false;
    } else {
      Key* const keys = chunk_keys_.data();
      std::uint64_t* const hashes = chunk_hashes_.data();
      std::uint8_t* const nulls = chunk_nulls_.data();
      if (source.values == nullptr) {
        std::fill_n(nulls, count, std::uint8_t{1});
        return true;
      }
      const std::uint8_t* const values = source.values + (start * kWidth);
      for (std::size_t i = 0; i < count; ++i) {
        Key key{};
        std::memcpy(BytesOf(key), values + (i * kWidth), kWidth);
        keys[i] = key;
        hashes[i] = Hasher::Hash(key.words);
      }
      if (source.validity == nullptr) {
        return false;
      }
      for (std::size_t i = 0; i < count; ++i) {
        nulls[i] = IsNull(source, start + i) ? 1 : 0;
      }
      return true;
    }
  }

  template <std::size_t kFirst>
  void PackTwoAfter(std::span<const Source> columns, std::size_t start, std::size_t count) {
    switch (layout_.columns[1].width) {
      case 2:
        return PackTwo<kFirst, 2>(columns[0], columns[1], start, count);
      case 4:
        return PackTwo<kFirst, 4>(columns[0], columns[1], start, count);
      case 8:
        return PackTwo<kFirst, 8>(columns[0], columns[1], start, count);
      default:
        return PackTwo<kFirst, 16>(columns[0], columns[1], start, count);
    }
  }

  template <std::size_t kFirst, std::size_t kSecond>
  void PackTwo(const Source& first, const Source& second, std::size_t start, std::size_t count) {
    if constexpr (kFirst + kSecond + 1 > kKeyBytes) {
      ANTB1_CHECK(kFirst + kSecond + 1 <= kKeyBytes);  // the layout picked kKeyBytes for these
    } else {
      Key* const keys = chunk_keys_.data();
      std::uint64_t* const hashes = chunk_hashes_.data();
      if (first.values != nullptr && first.validity == nullptr && second.values != nullptr &&
          second.validity == nullptr) {
        const std::uint8_t* const a = first.values + (start * kFirst);
        const std::uint8_t* const b = second.values + (start * kSecond);
        for (std::size_t i = 0; i < count; ++i) {
          Key key{};  // the NULL byte 0, and the padding
          std::memcpy(BytesOf(key), a + (i * kFirst), kFirst);
          std::memcpy(BytesOf(key) + kFirst, b + (i * kSecond), kSecond);
          keys[i] = key;
          hashes[i] = Hasher::Hash(key.words);
        }
        return;
      }
      for (std::size_t i = 0; i < count; ++i) {
        Key key{};
        unsigned mask = 0;
        if (IsNull(first, start + i)) {
          mask |= 1U;
        } else {
          std::memcpy(BytesOf(key), first.values + ((start + i) * kFirst), kFirst);
        }
        if (IsNull(second, start + i)) {
          mask |= 2U;
        } else {
          std::memcpy(BytesOf(key) + kFirst, second.values + ((start + i) * kSecond), kSecond);
        }
        BytesOf(key)[kFirst + kSecond] = static_cast<std::byte>(mask);
        keys[i] = key;
        hashes[i] = Hasher::Hash(key.words);
      }
    }
  }

  // The group of each packed row of the chunk into `ids`, a new key taking the next id. MakeRoom
  // has made room for every row to be new.
  template <bool kNulls>
  arrow::Status Probe(std::size_t count, std::uint32_t* ids) {
    const View view{
        .control = control_.data(), .slots = slots_.data(), .keys = keys_.data(), .mask = mask_};
    const Key* const keys = chunk_keys_.data();
    const std::uint64_t* const hashes = chunk_hashes_.data();
    const std::uint8_t* const nulls = chunk_nulls_.data();
    for (std::size_t i = 0; i < count; ++i) {
      if (i + kAhead < count) {
        const std::size_t ahead = hashes[i + kAhead] & view.mask;
        __builtin_prefetch(view.control + ahead);
        __builtin_prefetch(view.slots + ahead);
      }
      std::uint32_t id = kNoGroup;
      if (kNulls && nulls[i] != 0) {
        id = NullGroup();
      } else {
        std::size_t empty = 0;
        id = Find(view, keys[i], hashes[i], empty);
        if (id == kNoGroup) {
          id = Insert(view, keys[i], hashes[i], empty);
        }
      }
      if (id == kNoGroup) {
        return arrow::Status::OutOfMemory("a GROUP BY of more than ", max_groups_, " groups");
      }
      ids[i] = id;
    }
    return arrow::Status::OK();
  }

  // The eight control bytes from slot `pos` on. Never past the mirror: pos is at most the last
  // slot, and the mirror holds the first eight again.
  static ControlGroup GroupAt(const View& view, std::size_t pos) {
    ANTB1_DCHECK(pos <= view.mask);
    std::uint64_t word = 0;
    std::memcpy(&word, view.control + pos, sizeof(word));
    return {word};
  }

  // The group of `key` (of hash `hash`) when the table has it; otherwise kNoGroup, with `empty` the
  // slot it would take. The probe ends: the slots are never more than 7/8 full, and from 16 of them
  // the triangular steps of a group's width visit every group window of the table.
  static std::uint32_t Find(const View& view, const Key& key, std::uint64_t hash,
                            std::size_t& empty) {
    const std::uint8_t tag = Tag(hash);
    std::size_t pos = hash & view.mask;
    for (std::size_t stride = ControlGroup::kWidth;; stride += ControlGroup::kWidth) {
      const ControlGroup group = GroupAt(view, pos);
      for (std::uint64_t match = group.Match(tag); match != 0; match &= match - 1) {
        const std::uint32_t id = view.slots[(pos + ControlGroup::Index(match)) & view.mask];
        if (view.keys[id] == key) {
          return id;
        }
      }
      if (const std::uint64_t free = group.MatchEmpty(); free != 0) {
        empty = (pos + ControlGroup::Index(free)) & view.mask;
        return kNoGroup;
      }
      pos = (pos + stride) & view.mask;
    }
  }

  // `key` as the next group, in slot `slot`; kNoGroup when the table already holds max_groups.
  std::uint32_t Insert(const View& view, const Key& key, std::uint64_t hash, std::size_t slot) {
    if (keys_.size() >= max_groups_) {
      return kNoGroup;
    }
    const auto id = static_cast<std::uint32_t>(keys_.size());
    keys_.push_back(key);  // within the room MakeRoom made, so `view.keys` stays valid
    Place(view, slot, Tag(hash), id);
    return id;
  }

  static void Place(const View& view, std::size_t slot, std::uint8_t tag, std::uint32_t id) {
    view.control[slot] = tag;
    if (slot < ControlGroup::kWidth) {
      view.control[view.mask + 1 + slot] = tag;  // the mirror an unaligned load past the end reads
    }
    view.slots[slot] = id;
  }

  // The group of the NULL key (one key only), made when first seen; kNoGroup when it is new and the
  // table already holds max_groups. It takes the next id like any group, with a key of zeros that
  // no slot points to, so that the key array stays in id order.
  std::uint32_t NullGroup() {
    if (null_group_ == kNoGroup) {
      if (keys_.size() >= max_groups_) {
        return kNoGroup;
      }
      null_group_ = static_cast<std::uint32_t>(keys_.size());
      keys_.push_back(Key{});
    }
    return null_group_;
  }

  // Room for `extra` more groups without a check: the slots double until the groups fit the load
  // factor, and the key array doubles. A new array is allocated before the one it replaces is
  // given back, and a refused allocation leaves the table whole: at most its slots have grown.
  arrow::Status MakeRoom(std::size_t extra) {
    const std::size_t need = keys_.size() + extra;
    if (need > grow_at_) {
      std::size_t capacity = std::max(capacity_, kMinCapacity);
      while (capacity * kLoadNum / kLoadDen < need) {
        capacity *= 2;
      }
      ARROW_RETURN_NOT_OK(Rehash(capacity));
    }
    if (need > keys_.capacity()) {
      ARROW_RETURN_NOT_OK(keys_.Reserve(std::max({need, 2 * keys_.capacity(), kMinCapacity})));
    }
    return arrow::Status::OK();
  }

  // The groups in `capacity` slots (kGrowth).
  arrow::Status Rehash(std::size_t capacity) {
    PoolVector<std::uint8_t> control(pool_);
    ARROW_RETURN_NOT_OK(control.Assign(capacity + ControlGroup::kWidth, kEmpty));
    PoolVector<std::uint32_t> slots(pool_);
    ARROW_RETURN_NOT_OK(slots.Resize(capacity));
    std::swap(control, control_);  // `control` and `slots` are the old ones from here
    std::swap(slots, slots_);
    const std::size_t old_capacity = capacity_;
    capacity_ = capacity;
    mask_ = capacity - 1;
    grow_at_ = capacity * kLoadNum / kLoadDen;
    const View view{
        .control = control_.data(), .slots = slots_.data(), .keys = keys_.data(), .mask = mask_};
    if constexpr (kGrowth == Growth::kSlotOrder) {
      const std::uint8_t* const old_control = control.data();
      const std::uint32_t* const old_slots = slots.data();
      for (std::size_t old = 0; old < old_capacity; ++old) {
        if (const std::size_t ahead = old + kRehashAhead;
            ahead < old_capacity && old_control[ahead] != kEmpty) {
          __builtin_prefetch(view.keys + old_slots[ahead]);
        }
        if (old_control[old] != kEmpty) {
          Reinsert(view, old_slots[old]);
        }
      }
    } else {
      for (std::uint32_t id = 0; id < keys_.size(); ++id) {
        if (id != null_group_) {
          Reinsert(view, id);
        }
      }
    }
    return arrow::Status::OK();  // the old slots go back to the pool here
  }

  // Group `id` placed again in the slots of `view`, which hold no key equal to its own.
  static void Reinsert(const View& view, std::uint32_t id) {
    const std::uint64_t hash = Hasher::Hash(view.keys[id].words);
    std::size_t pos = hash & view.mask;
    for (std::size_t stride = ControlGroup::kWidth;; stride += ControlGroup::kWidth) {
      if (const std::uint64_t free = GroupAt(view, pos).MatchEmpty(); free != 0) {
        Place(view, (pos + ControlGroup::Index(free)) & view.mask, Tag(hash), id);
        return;
      }
      pos = (pos + stride) & view.mask;
    }
  }

  // After the first call with rows, remembers its groups. After the second, the share of that
  // call's rows whose group the first call made estimates (the first call's groups / all groups)
  // when rows hit groups in proportion to their frequency: a capture-recapture (Lincoln-Petersen)
  // estimate. The table then reserves 9/8 of it. It fails small: too few hits reserve nothing (ids
  // that never repeat early, sorted or clustered keys), skew lowers the estimate (heavy groups take
  // more hits), and kMaxGrowth bounds an overestimate. It reads only the table's own input, so it
  // is the same on any number of threads.
  arrow::Status Estimate(std::size_t rows, const std::uint32_t* ids) {
    if (calls_ >= 2) {
      return arrow::Status::OK();
    }
    ++calls_;
    if (calls_ == 1) {
      first_groups_ = keys_.size();
      return arrow::Status::OK();
    }
    std::size_t hits = 0;
    for (std::size_t row = 0; row < rows; ++row) {
      hits += ids[row] < first_groups_ ? 1U : 0U;
    }
    if (hits < kMinHits) {
      return arrow::Status::OK();
    }
    const UInt128 groups = static_cast<UInt128>(first_groups_) * rows / hits;
    const UInt128 wanted = groups + (groups / 8);
    const std::size_t bound =
        std::min(keys_.size() * kMaxGrowth, static_cast<std::size_t>(max_groups_));
    const std::size_t estimate = wanted < bound ? static_cast<std::size_t>(wanted) : bound;
    if (estimate <= keys_.size()) {
      return arrow::Status::OK();
    }
    // The reservation is a guess about rows to come, so a budget that refuses it does not fail the
    // table: it keeps growing as it would without the estimate. MakeRoom leaves a whole table when
    // an allocation is refused: at most its slots have grown, and its groups are untouched.
    const arrow::Status reserved = MakeRoom(estimate - keys_.size());
    return reserved.IsOutOfMemory() ? arrow::Status::OK() : reserved;
  }

  arrow::Result<arrow::compute::ExecBatch> BuildUniques() const {
    const std::size_t groups = keys_.size();
    std::vector<arrow::Datum> columns;
    columns.reserve(layout_.columns.size());
    for (std::size_t c = 0; c < layout_.columns.size(); ++c) {
      const FixedKeyLayout::Column& column = layout_.columns[c];
      ARROW_ASSIGN_OR_RAISE(
          std::shared_ptr<arrow::Buffer> values,
          arrow::AllocateBuffer(static_cast<std::int64_t>(groups * column.width), pool_));
      std::uint8_t* const out = values->mutable_data();
      for (std::size_t g = 0; g < groups; ++g) {
        std::memcpy(out + (g * column.width), BytesOf(keys_[g]) + column.offset, column.width);
      }
      // A group's value is NULL when it is the NULL group (one key), or by its NULL byte (two).
      const auto is_null = [&](std::size_t g) {
        if (layout_.columns.size() == 1) {
          return g == null_group_;
        }
        const auto mask = std::to_integer<unsigned>(BytesOf(keys_[g])[layout_.mask_offset]);
        return (mask & (1U << c)) != 0;
      };
      std::size_t nulls = 0;
      for (std::size_t g = 0; g < groups; ++g) {
        nulls += is_null(g) ? 1U : 0U;
      }
      std::shared_ptr<arrow::Buffer> validity;
      if (nulls > 0) {
        // AllocateBuffer rather than AllocateBitmap: those make the shared_ptr inside a noexcept
        // frame of Arrow's, where a failed allocation ends the process; here NoBadAlloc catches it.
        const std::int64_t bytes = arrow::bit_util::BytesForBits(static_cast<std::int64_t>(groups));
        ARROW_ASSIGN_OR_RAISE(validity, arrow::AllocateBuffer(bytes, pool_));
        std::uint8_t* const bits = validity->mutable_data();
        std::memset(bits, 0, static_cast<std::size_t>(bytes));
        for (std::size_t g = 0; g < groups; ++g) {
          if (!is_null(g)) {
            arrow::bit_util::SetBit(bits, static_cast<std::int64_t>(g));
          }
        }
      }
      columns.emplace_back(arrow::ArrayData::Make(column.type, static_cast<std::int64_t>(groups),
                                                  {std::move(validity), std::move(values)},
                                                  static_cast<std::int64_t>(nulls)));
    }
    return arrow::compute::ExecBatch(std::move(columns), static_cast<std::int64_t>(groups));
  }

  FixedKeyLayout layout_;
  arrow::MemoryPool* pool_;
  bool estimate_;
  std::uint32_t max_groups_;
  PoolVector<std::uint8_t> control_;  // capacity_ + 8 control bytes, the last 8 mirroring the first
  PoolVector<std::uint32_t> slots_;   // capacity_ group ids, valid where the control byte is a tag
  PoolVector<Key> keys_;              // every group's key in id order (the NULL group's zeros)
  std::size_t capacity_ = 0;
  std::size_t mask_ = 0;
  std::size_t grow_at_ = 0;  // the groups the slots hold before they double
  std::uint32_t null_group_ = kNoGroup;
  std::size_t calls_ = 0;         // Consume calls with rows seen while estimating
  std::size_t first_groups_ = 0;  // the groups after the first of them
  // A chunk's packed keys, their hashes, and (one key) which rows are NULL; allocated with the
  // first row.
  PoolVector<Key> chunk_keys_;
  PoolVector<std::uint64_t> chunk_hashes_;
  PoolVector<std::uint8_t> chunk_nulls_;
};

// The table of `layout` (FixedKeyLayoutOf) growing by `growth`, for MakeKeyTable.
std::unique_ptr<KeyTable> MakeFixedKeyTable(FixedKeyLayout layout, Growth growth,
                                            arrow::MemoryPool* pool,
                                            const KeyTableOptions& options);

// The engine's tables, made in fixed_key_table.cc.
extern template class FixedKeyTable<8, Growth::kSlotOrder>;
extern template class FixedKeyTable<16, Growth::kSlotOrder>;
extern template class FixedKeyTable<24, Growth::kSlotOrder>;
extern template class FixedKeyTable<32, Growth::kSlotOrder>;
extern template class FixedKeyTable<40, Growth::kSlotOrder>;
extern template class FixedKeyTable<8, Growth::kIdOrder>;
extern template class FixedKeyTable<16, Growth::kIdOrder>;
extern template class FixedKeyTable<24, Growth::kIdOrder>;
extern template class FixedKeyTable<32, Growth::kIdOrder>;
extern template class FixedKeyTable<40, Growth::kIdOrder>;

}  // namespace antb1::exec
