#include "column_key_table.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <arrow/array/array_primitive.h>
#include <arrow/array/data.h>
#include <arrow/array/util.h>
#include <arrow/buffer.h>
#include <arrow/compute/exec.h>
#include <arrow/stl_allocator.h>
#include <arrow/type.h>
#include <arrow/util/bit_util.h>

#include "antb1/common/check.h"
#include "antb1/common/narrow.h"

#include "fold_hash.h"
#include "pool_vector.h"

namespace antb1::exec {
namespace {

constexpr std::uint32_t kMissing = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t kChunk = 256;
constexpr std::size_t kMaskBits = std::numeric_limits<std::uint64_t>::digits;

// A borrowed array for one call. Its validity and (for BOOLEAN) value bitmaps can start at any
// bit. With no values buffer every row is NULL, including when the validity buffer is absent.
struct Source {
  const std::uint8_t* values;
  const std::uint8_t* validity;
  std::size_t offset;

  bool Present(std::size_t row) const {
    return values != nullptr &&
           (validity == nullptr || arrow::bit_util::GetBit(validity, offset + row));
  }
};

// At each group capacity the store contains successive columns of [validity bitmap, values].
// A BOOL's values are a second bitmap. Capacities are multiples of eight, so multiplying the
// total preceding bits per group by capacity / 8 locates a column with no padding.
struct Column {
  std::shared_ptr<arrow::DataType> type;
  std::size_t width;  // zero for a bit-packed BOOLEAN
  std::size_t preceding_bits;
  std::uint8_t* validity = nullptr;
  std::uint8_t* values = nullptr;
  std::uint32_t nulls = 0;

  std::size_t ValueBytes(std::uint32_t groups) const {
    return width == 0 ? static_cast<std::size_t>(arrow::bit_util::BytesForBits(groups))
                      : groups * width;
  }

  bool Equal(const Source& source, std::size_t row, std::uint32_t id) const {
    const bool present = source.Present(row);
    if (present != arrow::bit_util::GetBit(validity, id)) {
      return false;
    }
    if (!present) {
      return true;
    }
    const auto index = source.offset + row;
    return width == 0
               ? arrow::bit_util::GetBit(source.values, index) ==
                     arrow::bit_util::GetBit(values, id)
               : std::memcmp(source.values + (index * width), values + (id * width), width) == 0;
  }

  void Append(const Source& source, std::size_t row, std::uint32_t id) {
    const bool present = source.Present(row);
    arrow::bit_util::SetBitTo(validity, id, present);
    const auto index = source.offset + row;
    if (width == 0) {
      arrow::bit_util::SetBitTo(values, id,
                                present && arrow::bit_util::GetBit(source.values, index));
    } else if (present) {
      std::memcpy(values + (id * width), source.values + (index * width), width);
    } else {
      std::memset(values + (id * width), 0, width);
    }
    nulls += present ? 0U : 1U;
  }
};

// Width is chosen once per column per chunk. A NULL skips the input payload and sets this
// column's bit in its row's mask. HashChunk mixes the mask after each block of up to 64 columns.
template <std::size_t kWidth>
void HashColumn(const Source& source, std::size_t start, std::span<std::uint64_t> hashes,
                std::span<std::uint64_t> null_masks, std::uint64_t column_bit) {
  for (std::size_t i = 0; i < hashes.size(); ++i) {
    const auto row = start + i;
    if (!source.Present(row)) {
      null_masks[i] |= column_bit;
      hashes[i] = FoldHash::Mix(FoldHash::kSeed, hashes[i]);
      continue;
    }
    const auto index = source.offset + row;
    if constexpr (kWidth == 0) {
      hashes[i] = FoldHash::Mix(arrow::bit_util::GetBit(source.values, index) ? 1U : 0U, hashes[i]);
    } else {
      const auto* bytes = source.values + (index * kWidth);
      std::uint64_t word = 0;
      std::memcpy(&word, bytes, std::min(kWidth, sizeof(word)));
      hashes[i] = FoldHash::Mix(word, hashes[i]);
      if constexpr (kWidth == 16) {
        std::memcpy(&word, bytes + sizeof(word), sizeof(word));
        hashes[i] = FoldHash::Mix(word, hashes[i]);
      }
    }
  }
}

arrow::Result<std::shared_ptr<arrow::Buffer>> CopyBytes(const std::uint8_t* source,
                                                        std::size_t bytes,
                                                        arrow::MemoryPool* pool) {
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> out,
                        arrow::AllocateBuffer(Narrow<std::int64_t>(bytes), pool));
  if (bytes > 0) {
    std::memcpy(out->mutable_data(), source, bytes);
  }
  return out;
}

class ColumnKeyTable final : public KeyTable {
 public:
  ColumnKeyTable(const std::vector<arrow::TypeHolder>& types, arrow::MemoryPool* pool,
                 std::uint32_t max_groups)
      : pool_(pool),
        max_groups_(max_groups),
        columns_(arrow::stl::allocator<Column>(pool)),
        storage_(pool),
        hashes_(pool),
        slots_(pool),
        chunk_(pool) {
    ANTB1_CHECK(max_groups > 0);
    ANTB1_CHECK(max_groups < kMissing);
    columns_.reserve(types.size());
    for (const auto& type : types) {
      const auto width =
          type.id() == arrow::Type::BOOL ? 0U : Narrow<std::size_t>(type.type->byte_width());
      columns_.push_back(
          {.type = type.GetSharedPtr(), .width = width, .preceding_bits = row_bits_});
      row_bits_ += (width == 0 ? 1U : width * 8) + 1U;
    }
  }

  arrow::Result<KeyIds> Consume(const arrow::compute::ExecBatch& batch) override {
    return KeyTableNoBadAlloc([&] -> arrow::Result<KeyIds> {
      const std::uint32_t before = groups_;
      ARROW_ASSIGN_OR_RAISE(auto owner, IdBuffer(batch));
      auto* ids = owner->mutable_data_as<std::uint32_t>();
      ARROW_RETURN_NOT_OK(ProbeRows<true>(batch, ids));
      return KeyIds{.owner = std::move(owner),
                    .ids = {ids, Narrow<std::size_t>(batch.length)},
                    .before = before,
                    .after = groups_};
    });
  }

  arrow::Result<std::shared_ptr<arrow::UInt32Array>> Lookup(
      const arrow::compute::ExecBatch& batch) override {
    return KeyTableNoBadAlloc([&] -> arrow::Result<std::shared_ptr<arrow::UInt32Array>> {
      ARROW_RETURN_NOT_OK(CheckBatch(batch));
      if (groups_ == 0) {
        ARROW_ASSIGN_OR_RAISE(auto misses,
                              arrow::MakeArrayOfNull(arrow::uint32(), batch.length, pool_));
        return std::static_pointer_cast<arrow::UInt32Array>(std::move(misses));
      }
      ARROW_ASSIGN_OR_RAISE(auto owner, IdBuffer(batch));
      auto* ids = owner->mutable_data_as<std::uint32_t>();
      ARROW_RETURN_NOT_OK(ProbeRows<false>(batch, ids));
      std::shared_ptr<arrow::Buffer> validity;
      std::int64_t missing = 0;
      for (std::int64_t row = 0; row < batch.length; ++row) {
        if (ids[row] == kMissing) {
          if (validity == nullptr) {
            const auto bytes = arrow::bit_util::BytesForBits(batch.length);
            ARROW_ASSIGN_OR_RAISE(validity, arrow::AllocateBuffer(bytes, pool_));
            std::memset(validity->mutable_data(), 0xff, Narrow<std::size_t>(bytes));
          }
          arrow::bit_util::ClearBit(validity->mutable_data(), row);
          ids[row] = 0;
          ++missing;
        }
      }
      return std::make_shared<arrow::UInt32Array>(batch.length, std::move(owner),
                                                  std::move(validity), missing);
    });
  }

  arrow::Result<arrow::compute::ExecBatch> Uniques() const override {
    return KeyTableNoBadAlloc([&] -> arrow::Result<arrow::compute::ExecBatch> {
      std::vector<arrow::Datum> out;
      out.reserve(columns_.size());
      for (const Column& col : columns_) {
        ARROW_ASSIGN_OR_RAISE(auto values, CopyBytes(col.values, col.ValueBytes(groups_), pool_));
        std::shared_ptr<arrow::Buffer> validity;
        if (col.nulls > 0) {
          ARROW_ASSIGN_OR_RAISE(
              validity,
              CopyBytes(col.validity, Narrow<std::size_t>(arrow::bit_util::BytesForBits(groups_)),
                        pool_));
        }
        // Each returned column owns a pool-backed descriptor, including at zero rows. The
        // ExecBatch datum vector and ArrayData's vector of buffer handles are Arrow API metadata.
        std::vector<std::shared_ptr<arrow::Buffer>> buffers = {std::move(validity),
                                                               std::move(values)};
        out.emplace_back(std::allocate_shared<arrow::ArrayData>(
            arrow::stl::allocator<arrow::ArrayData>(pool_), col.type, groups_, std::move(buffers),
            col.nulls));
      }
      return arrow::compute::ExecBatch(std::move(out), groups_);
    });
  }

  std::uint32_t num_groups() const override { return groups_; }

 private:
  arrow::Status CheckBatch(const arrow::compute::ExecBatch& batch) const {
    ANTB1_CHECK(batch.values.size() == columns_.size());
    if (batch.length < 0 || batch.length > std::numeric_limits<std::int64_t>::max() / 4) {
      return arrow::Status::CapacityError("column key ID buffer size overflow");
    }
    return arrow::Status::OK();
  }

  arrow::Result<std::shared_ptr<arrow::Buffer>> IdBuffer(
      const arrow::compute::ExecBatch& batch) const {
    ARROW_RETURN_NOT_OK(CheckBatch(batch));
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> buffer,
                          arrow::AllocateBuffer(batch.length * 4, pool_));
    return buffer;
  }

  void HashChunk(const PoolVector<Source>& sources, std::size_t start,
                 std::span<std::uint64_t> hashes, std::span<std::uint64_t> null_masks) const {
    std::ranges::fill(hashes, FoldHash::kSeed);
    for (std::size_t block = 0; block < columns_.size(); block += kMaskBits) {
      std::ranges::fill(null_masks, std::uint64_t{0});
      const auto count = std::min(kMaskBits, columns_.size() - block);
      for (std::size_t bit = 0; bit < count; ++bit) {
        const auto c = block + bit;
        const auto column_bit = std::uint64_t{1} << bit;
        switch (columns_[c].width) {
          case 0:
            HashColumn<0>(sources[c], start, hashes, null_masks, column_bit);
            break;
          case 2:
            HashColumn<2>(sources[c], start, hashes, null_masks, column_bit);
            break;
          case 4:
            HashColumn<4>(sources[c], start, hashes, null_masks, column_bit);
            break;
          case 8:
            HashColumn<8>(sources[c], start, hashes, null_masks, column_bit);
            break;
          default:
            HashColumn<16>(sources[c], start, hashes, null_masks, column_bit);
            break;
        }
      }
      for (std::size_t i = 0; i < hashes.size(); ++i) {
        hashes[i] = FoldHash::Mix(null_masks[i], hashes[i]);
      }
    }
  }

  template <bool kInsert>
  arrow::Status ProbeRows(const arrow::compute::ExecBatch& batch, std::uint32_t* ids) {
    const auto rows = Narrow<std::size_t>(batch.length);
    if (rows == 0) {
      return arrow::Status::OK();
    }
    PoolVector<Source> sources(pool_);
    ARROW_RETURN_NOT_OK(sources.Resize(columns_.size()));
    for (std::size_t c = 0; c < columns_.size(); ++c) {
      ANTB1_CHECK(batch.values[c].is_array());
      const auto& data = *batch.values[c].array();
      sources[c] = {
          .values = data.buffers.size() > 1 && data.buffers[1] ? data.buffers[1]->data() : nullptr,
          .validity = !data.buffers.empty() && data.buffers[0] ? data.buffers[0]->data() : nullptr,
          .offset = Narrow<std::size_t>(data.offset)};
    }
    ARROW_RETURN_NOT_OK(chunk_.Resize(2 * kChunk));
    for (std::size_t start = 0; start < rows; start += kChunk) {
      std::span<std::uint64_t> hashes(chunk_.data(), std::min(kChunk, rows - start));
      HashChunk(sources, start, hashes, {chunk_.data() + kChunk, hashes.size()});
      for (std::size_t i = 0; i < hashes.size(); ++i) {
        const auto row = start + i;
        const auto hash = hashes[i];
        auto slot = Find(hash, sources, row);
        std::uint32_t id = slots_.empty() ? kMissing : slots_[slot];
        if constexpr (kInsert) {
          if (id == kMissing) {
            if (groups_ >= max_groups_) {
              return arrow::Status::OutOfMemory("a GROUP BY of more than ", max_groups_, " groups");
            }
            if (groups_ == hashes_.size()) {
              ARROW_RETURN_NOT_OK(Grow());
              slot = EmptySlot(slots_, hash);
            }
            id = groups_;
            for (std::size_t c = 0; c < columns_.size(); ++c) {
              columns_[c].Append(sources[c], row, id);
            }
            slots_[slot] = id;
            hashes_[id] = hash;
            ++groups_;
          }
        }
        ids[row] = id;
      }
    }
    return arrow::Status::OK();
  }

  // Linear probing in power-of-two slots. At most three quarters are occupied. A full hash
  // match is only a candidate: check each column's validity and value before returning its ID.
  std::size_t Find(std::uint64_t hash, const PoolVector<Source>& sources, std::size_t row) const {
    if (slots_.empty()) {
      return 0;
    }
    const auto mask = slots_.size() - 1;
    auto slot = static_cast<std::size_t>(hash) & mask;
    while (slots_[slot] != kMissing) {
      const auto id = slots_[slot];
      if (hashes_[id] == hash) {
        std::size_t c = 0;
        while (c < columns_.size() && columns_[c].Equal(sources[c], row, id)) {
          ++c;
        }
        if (c == columns_.size()) {
          break;
        }
      }
      slot = (slot + 1) & mask;
    }
    return slot;
  }

  static std::size_t EmptySlot(const PoolVector<std::uint32_t>& slots, std::uint64_t hash) {
    const auto mask = slots.size() - 1;
    auto slot = static_cast<std::size_t>(hash) & mask;
    while (slots[slot] != kMissing) {
      slot = (slot + 1) & mask;
    }
    return slot;
  }

  // Stage every replacement before changing any live pointer. Growing from 32 buckets doubles
  // storage for all columns together. Only previously inserted values and bits are copied.
  arrow::Status Grow() {
    const auto buckets = std::max<std::size_t>(32, slots_.size() * 2);
    const auto capacity = buckets - (buckets / 4);
    const auto bitmap_bytes = capacity / 8;
    if (row_bits_ > std::numeric_limits<std::int64_t>::max() / bitmap_bytes) {
      return arrow::Status::CapacityError("column key storage size overflow");
    }
    PoolVector<std::uint8_t> store(pool_);
    PoolVector<std::uint64_t> hashes(pool_);
    PoolVector<std::uint32_t> slots(pool_);
    ARROW_RETURN_NOT_OK(store.Resize(row_bits_ * bitmap_bytes));
    ARROW_RETURN_NOT_OK(hashes.Resize(capacity));
    ARROW_RETURN_NOT_OK(slots.Assign(buckets, kMissing));
    for (std::uint32_t id = 0; id < groups_; ++id) {
      hashes[id] = hashes_[id];
      slots[EmptySlot(slots, hashes[id])] = id;
    }
    const auto valid_bytes = Narrow<std::size_t>(arrow::bit_util::BytesForBits(groups_));
    for (Column& col : columns_) {
      auto* const validity = store.data() + (col.preceding_bits * bitmap_bytes);
      auto* const values = validity + bitmap_bytes;
      std::memset(validity, 0, bitmap_bytes);
      if (col.width == 0) {
        std::memset(values, 0, bitmap_bytes);
      }
      if (groups_ > 0) {
        std::memcpy(validity, col.validity, valid_bytes);
        std::memcpy(values, col.values, col.ValueBytes(groups_));
      }
      col.validity = validity;
      col.values = values;
    }
    storage_ = std::move(store);
    hashes_ = std::move(hashes);
    slots_ = std::move(slots);
    return arrow::Status::OK();
  }

  arrow::MemoryPool* pool_;
  std::uint32_t max_groups_;
  std::vector<Column, arrow::stl::allocator<Column>> columns_;
  std::size_t row_bits_ = 0;
  std::uint32_t groups_ = 0;
  PoolVector<std::uint8_t> storage_;  // owned columns and their validity, in dense ID order
  PoolVector<std::uint64_t> hashes_;  // group ID -> full probe hash
  PoolVector<std::uint32_t> slots_;   // bucket -> group ID or kMissing
  PoolVector<std::uint64_t> chunk_;   // kChunk hashes followed by kChunk column NULL masks
};

}  // namespace

std::unique_ptr<KeyTable> MakeColumnKeyTable(const std::vector<arrow::TypeHolder>& types,
                                             arrow::MemoryPool* pool,
                                             const KeyTableOptions& options) {
  return std::make_unique<ColumnKeyTable>(types, pool, options.max_groups);
}

}  // namespace antb1::exec
