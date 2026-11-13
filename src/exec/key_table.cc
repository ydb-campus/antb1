#include "key_table.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include <arrow/array.h>
#include <arrow/array/data.h>
#include <arrow/array/util.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>
#include <arrow/compute/row/grouper.h>
#include <arrow/datum.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/util/bit_util.h>
#include <arrow/util/bitmap_ops.h>

#include "antb1/common/check.h"
#include "antb1/common/narrow.h"

namespace antb1::exec {
namespace {

// `body()`, with a std::bad_alloc from an allocation outside the pool (Arrow's small objects) as
// OutOfMemory: no exception leaves exec, and GroupAggregateOperator makes its table in Open, which
// nothing converts (as join_table.cc does for the join).
template <class Body>
auto NoBadAlloc(const Body& body) -> decltype(body()) {
  try {
    return body();
  } catch (const std::bad_alloc&) {
    return arrow::Status::OutOfMemory("out of memory in a GROUP BY's key table");
  }
}

// A fixed-width column with no values buffer, which the Arrow format allows when it has no rows or
// only NULLs.
bool LacksValues(const arrow::ArrayData& data) {
  return data.type->byte_width() > 0 && data.buffers.size() > 1 && data.buffers[1] == nullptr;
}

// `keys` with every such column replaced by NULLs over a zeroed values buffer: arrow's Grouper
// reads a fixed-width column's values even where every row is NULL.
arrow::Result<arrow::compute::ExecBatch> WithValues(const arrow::compute::ExecBatch& keys,
                                                    arrow::MemoryPool* pool) {
  arrow::compute::ExecBatch out = keys;
  for (arrow::Datum& key : out.values) {
    const arrow::ArrayData& data = *key.array();
    if (LacksValues(data)) {
      ANTB1_CHECK(data.GetNullCount() == data.length);  // valid rows have values
      ARROW_ASSIGN_OR_RAISE(key, arrow::MakeArrayOfNull(data.type, data.length, pool));
    }
  }
  return out;
}

// arrow::compute::Grouper, for every key shape: ids in an order of its own.
class GrouperKeyTable final : public KeyTable {
 public:
  GrouperKeyTable(std::unique_ptr<arrow::compute::Grouper> grouper, arrow::MemoryPool* pool)
      : grouper_(std::move(grouper)), pool_(pool) {}

  arrow::Result<KeyIds> Consume(const arrow::compute::ExecBatch& keys) override {
    return NoBadAlloc([&] -> arrow::Result<KeyIds> {
      if (std::ranges::any_of(keys.values,
                              [](const arrow::Datum& key) { return LacksValues(*key.array()); })) {
        ARROW_ASSIGN_OR_RAISE(const arrow::compute::ExecBatch with_values, WithValues(keys, pool_));
        return ConsumeWithValues(with_values);
      }
      return ConsumeWithValues(keys);
    });
  }

  arrow::Result<std::shared_ptr<arrow::UInt32Array>> Lookup(
      const arrow::compute::ExecBatch& keys) override {
    return NoBadAlloc([&] -> arrow::Result<std::shared_ptr<arrow::UInt32Array>> {
      const auto lookup = [this](const arrow::compute::ExecBatch& batch)
          -> arrow::Result<std::shared_ptr<arrow::UInt32Array>> {
        ARROW_ASSIGN_OR_RAISE(const arrow::Datum datum,
                              grouper_->Lookup(arrow::compute::ExecSpan(batch)));
        return std::static_pointer_cast<arrow::UInt32Array>(datum.make_array());
      };
      if (std::ranges::any_of(keys.values,
                              [](const arrow::Datum& key) { return LacksValues(*key.array()); })) {
        ARROW_ASSIGN_OR_RAISE(const arrow::compute::ExecBatch with_values, WithValues(keys, pool_));
        return lookup(with_values);
      }
      return lookup(keys);
    });
  }

  [[nodiscard]] arrow::Result<arrow::compute::ExecBatch> Uniques() const override {
    return NoBadAlloc([&] { return grouper_->GetUniques(); });
  }

  [[nodiscard]] std::uint32_t num_groups() const override { return grouper_->num_groups(); }

 private:
  // Consume of keys whose fixed-width columns all have a values buffer.
  arrow::Result<KeyIds> ConsumeWithValues(const arrow::compute::ExecBatch& keys) {
    KeyIds ids;
    ids.before = grouper_->num_groups();
    ARROW_ASSIGN_OR_RAISE(const arrow::Datum datum,
                          grouper_->Consume(arrow::compute::ExecSpan(keys)));
    const std::shared_ptr<arrow::ArrayData>& data = datum.array();
    ids.owner = data->buffers[1];
    ids.ids = {data->GetValues<std::uint32_t>(1), static_cast<std::size_t>(data->length)};
    ids.after = grouper_->num_groups();
    return ids;
  }

  std::unique_ptr<arrow::compute::Grouper> grouper_;
  arrow::MemoryPool* pool_;
};

arrow::Result<std::unique_ptr<KeyTable>> MakeGrouper(const std::vector<arrow::TypeHolder>& types,
                                                     arrow::compute::ExecContext* ctx) {
  ARROW_ASSIGN_OR_RAISE(auto grouper, arrow::compute::Grouper::Make(types, ctx));
  return std::make_unique<GrouperKeyTable>(std::move(grouper), ctx->memory_pool());
}

arrow::Result<std::shared_ptr<arrow::Buffer>> AllocateWords(int64_t rows, int64_t width,
                                                            arrow::MemoryPool* pool) {
  if (rows < 0 || rows > std::numeric_limits<int64_t>::max() / width) {
    return arrow::Status::CapacityError("packed key buffer size overflow");
  }
  return arrow::AllocateBuffer(rows * width, pool);
}

bool SupportsPackedStrings(const std::vector<arrow::TypeHolder>& types) {
  return types.size() <= 2 && std::ranges::all_of(types, [](const arrow::TypeHolder& type) {
           return type.id() == arrow::Type::STRING || type.id() == arrow::Type::BINARY;
         });
}

// A reversible, endian-independent word: the high byte is zero for NULL, otherwise length + 1;
// the low seven bytes are the value, zero-padded. Empty, NULL and embedded zero bytes stay
// distinct. Packing is a batch operation, with no mutation of the Grouper until all columns fit.
arrow::Result<std::optional<arrow::compute::ExecBatch>> PackStrings(
    const arrow::compute::ExecBatch& keys, arrow::MemoryPool* pool,
    std::vector<std::shared_ptr<arrow::Buffer>>& scratch) {
  if (keys.length < 0 || keys.length > std::numeric_limits<int64_t>::max() / 8) {
    return arrow::Status::CapacityError("packed key buffer size overflow");
  }
  std::vector<arrow::Datum> columns;
  columns.reserve(keys.values.size());
  for (std::size_t column = 0; column < keys.values.size(); ++column) {
    const arrow::ArrayData& data = *keys.values[column].array();
    // Keep only small packing buffers between calls. Large batches retain the original
    // temporary-buffer lifetime, without evicting a useful small cache.
    constexpr int64_t kMaxCachedRows = 256;
    std::shared_ptr<arrow::Buffer> temporary;
    auto& buffer = keys.length <= kMaxCachedRows ? scratch[column] : temporary;
    if (buffer == nullptr || buffer->size() < keys.length * 8) {
      // No backend operation retains the packed input. Release old capacity before growing,
      // keeping the query budget from charging both buffers during allocation.
      buffer.reset();
      ARROW_ASSIGN_OR_RAISE(buffer, AllocateWords(keys.length, 8, pool));
    }
    auto* out = reinterpret_cast<std::uint64_t*>(buffer->mutable_data());
    const auto* offsets = data.GetValuesSafe<std::int32_t>(1);
    const auto* bytes = data.GetValuesSafe<std::uint8_t>(2, 0);
    const auto* valid = data.GetValuesSafe<std::uint8_t>(0, 0);
    for (int64_t row = 0; row < keys.length; ++row) {
      if (valid != nullptr &&
          !arrow::bit_util::GetBit(valid, Narrow<std::uint64_t>(data.offset + row))) {
        out[row] = 0;
        continue;
      }
      const std::int32_t length = offsets[row + 1] - offsets[row];
      if (length > 7) {
        return std::nullopt;
      }
      std::uint64_t word = Narrow<std::uint64_t>(length + 1) << 56U;
      for (std::int32_t byte = 0; byte < length; ++byte) {
        word |= std::uint64_t{bytes[offsets[row] + byte]} << (Narrow<unsigned>(byte) * 8U);
      }
      out[row] = word;
    }
    columns.emplace_back(
        arrow::ArrayData::Make(arrow::uint64(), keys.length, {nullptr, buffer}, 0));
  }
  return arrow::compute::ExecBatch(std::move(columns), keys.length);
}

// Decode only unique keys, rather than allocating/copying a string for each input row.
arrow::Result<arrow::Datum> UnpackStrings(const arrow::ArrayData& packed,
                                          const std::shared_ptr<arrow::DataType>& type,
                                          arrow::MemoryPool* pool) {
  const auto* words = packed.GetValues<std::uint64_t>(1);
  int64_t byte_count = 0;
  int64_t null_count = 0;
  for (int64_t row = 0; row < packed.length; ++row) {
    const std::uint64_t tag = words[row] >> 56U;
    ANTB1_CHECK(tag <= 8);
    byte_count += tag == 0 ? 0 : Narrow<int64_t>(tag - 1);
    null_count += tag == 0 ? 1 : 0;
  }
  if (byte_count > std::numeric_limits<std::int32_t>::max()) {
    return arrow::Status::CapacityError("packed string uniques exceed binary offset range");
  }
  // The number of groups is uint32, so adding the terminal offset cannot overflow int64.
  ARROW_ASSIGN_OR_RAISE(auto offsets, AllocateWords(packed.length + 1, 4, pool));
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> bytes,
                        arrow::AllocateBuffer(byte_count, pool));
  std::shared_ptr<arrow::Buffer> valid;
  if (null_count != 0) {
    ARROW_ASSIGN_OR_RAISE(valid, arrow::AllocateEmptyBitmap(packed.length, pool));
  }
  auto* out_offsets = reinterpret_cast<std::int32_t*>(offsets->mutable_data());
  std::uint8_t* out_bytes = bytes->mutable_data();
  std::int32_t offset = 0;
  out_offsets[0] = 0;
  for (int64_t row = 0; row < packed.length; ++row) {
    std::uint64_t word = words[row];
    const auto tag = Narrow<std::int32_t>(word >> 56U);
    if (valid != nullptr) {
      arrow::bit_util::SetBitTo(valid->mutable_data(), row, tag != 0);
    }
    for (std::int32_t byte = 0; byte < tag - 1; ++byte) {
      out_bytes[offset] = Narrow<std::uint8_t>(word & 0xffU);
      ++offset;
      word >>= 8U;
    }
    out_offsets[row + 1] = offset;
  }
  return arrow::Datum(arrow::ArrayData::Make(
      type, packed.length, {std::move(valid), std::move(offsets), std::move(bytes)}, null_count));
}

class PackedStringKeyTable final : public KeyTable {
 public:
  PackedStringKeyTable(std::unique_ptr<KeyTable> backend,
                       std::vector<arrow::TypeHolder> original_types,
                       arrow::compute::ExecContext* ctx)
      : backend_(std::move(backend)),
        original_types_(std::move(original_types)),
        ctx_(ctx),
        scratch_(original_types_.size()) {}

  arrow::Result<KeyIds> Consume(const arrow::compute::ExecBatch& keys) override {
    return NoBadAlloc([&] -> arrow::Result<KeyIds> {
      if (packed_) {
        ARROW_ASSIGN_OR_RAISE(auto packed, PackStrings(keys, ctx_->memory_pool(), scratch_));
        if (packed.has_value()) {
          return backend_->Consume(*packed);
        }
        ARROW_RETURN_NOT_OK(Promote());
      }
      ARROW_ASSIGN_OR_RAISE(KeyIds result, backend_->Consume(keys));
      if (backend_to_public_ == nullptr) {
        return result;
      }
      ARROW_ASSIGN_OR_RAISE(
          auto values, AllocateWords(Narrow<int64_t>(result.ids.size()), 4, ctx_->memory_pool()));
      auto* out = reinterpret_cast<std::uint32_t*>(values->mutable_data());
      for (std::size_t row = 0; row < result.ids.size(); ++row) {
        out[row] = PublicId(result.ids[row]);
      }
      result.owner = std::move(values);
      result.ids = {out, result.ids.size()};
      return result;
    });
  }

  arrow::Result<std::shared_ptr<arrow::UInt32Array>> Lookup(
      const arrow::compute::ExecBatch& keys) override {
    return NoBadAlloc([&] -> arrow::Result<std::shared_ptr<arrow::UInt32Array>> {
      if (packed_) {
        ARROW_ASSIGN_OR_RAISE(auto packed, PackStrings(keys, ctx_->memory_pool(), scratch_));
        if (packed.has_value()) {
          return backend_->Lookup(*packed);
        }
        // Promotion changes the representation only: no lookup row is inserted.
        ARROW_RETURN_NOT_OK(Promote());
      }
      ARROW_ASSIGN_OR_RAISE(auto result, backend_->Lookup(keys));
      if (backend_to_public_ == nullptr) {
        return result;
      }
      ARROW_ASSIGN_OR_RAISE(auto values, AllocateWords(result->length(), 4, ctx_->memory_pool()));
      auto* out = reinterpret_cast<std::uint32_t*>(values->mutable_data());
      for (int64_t row = 0; row < result->length(); ++row) {
        out[row] = result->IsNull(row) ? 0 : PublicId(result->Value(row));
      }
      std::shared_ptr<arrow::Buffer> valid;
      if (result->null_bitmap() != nullptr) {
        ARROW_ASSIGN_OR_RAISE(
            valid, arrow::internal::CopyBitmap(ctx_->memory_pool(), result->null_bitmap_data(),
                                               result->offset(), result->length()));
      }
      return std::make_shared<arrow::UInt32Array>(result->length(), std::move(values),
                                                  std::move(valid), result->null_count());
    });
  }

  [[nodiscard]] arrow::Result<arrow::compute::ExecBatch> Uniques() const override {
    return NoBadAlloc([&] -> arrow::Result<arrow::compute::ExecBatch> {
      ARROW_ASSIGN_OR_RAISE(auto uniques, backend_->Uniques());
      if (packed_) {
        for (std::size_t column = 0; column < uniques.values.size(); ++column) {
          ARROW_ASSIGN_OR_RAISE(
              uniques.values[column],
              UnpackStrings(*uniques.values[column].array(), original_types_[column].GetSharedPtr(),
                            ctx_->memory_pool()));
        }
      } else if (public_to_backend_ != nullptr) {
        ARROW_ASSIGN_OR_RAISE(auto values, AllocateWords(uniques.length, 4, ctx_->memory_pool()));
        auto* indices = reinterpret_cast<std::uint32_t*>(values->mutable_data());
        const auto* old_indices =
            reinterpret_cast<const std::uint32_t*>(public_to_backend_->data());
        for (std::uint32_t id = 0; id < num_groups(); ++id) {
          indices[id] = id < mapped_groups_ ? old_indices[id] : id;
        }
        const arrow::Datum order(arrow::ArrayData::Make(arrow::uint32(), uniques.length,
                                                        {nullptr, std::move(values)}, 0));
        for (arrow::Datum& column : uniques.values) {
          ARROW_ASSIGN_OR_RAISE(
              column, arrow::compute::Take(column, order,
                                           arrow::compute::TakeOptions::NoBoundsCheck(), ctx_));
        }
      }
      return uniques;
    });
  }

  [[nodiscard]] std::uint32_t num_groups() const override { return backend_->num_groups(); }

 private:
  // Build the replacement in isolation; even a failed allocation leaves the packed table intact.
  // A Grouper may number a seeding batch in its own order. Remember that permutation explicitly.
  arrow::Status Promote() {
    // Scratch is disposable even when promotion fails; retain only the logical key state.
    for (auto& buffer : scratch_) {
      buffer.reset();
    }
    ARROW_ASSIGN_OR_RAISE(const auto uniques, Uniques());
    ARROW_ASSIGN_OR_RAISE(auto replacement, MakeGrouper(original_types_, ctx_));
    ARROW_ASSIGN_OR_RAISE(const KeyIds seed, replacement->Consume(uniques));
    ANTB1_CHECK(seed.after == num_groups());
    bool identity = true;
    for (std::size_t id = 0; id < seed.ids.size(); ++id) {
      identity = identity && seed.ids[id] == id;
    }
    std::shared_ptr<arrow::Buffer> to_public;
    std::shared_ptr<arrow::Buffer> to_backend;
    if (!identity) {
      ARROW_ASSIGN_OR_RAISE(to_public, AllocateWords(seed.after, 4, ctx_->memory_pool()));
      ARROW_ASSIGN_OR_RAISE(to_backend, AllocateWords(seed.after, 4, ctx_->memory_pool()));
      auto* public_ids = reinterpret_cast<std::uint32_t*>(to_public->mutable_data());
      auto* backend_ids = reinterpret_cast<std::uint32_t*>(to_backend->mutable_data());
      for (std::size_t id = 0; id < seed.ids.size(); ++id) {
        public_ids[seed.ids[id]] = Narrow<std::uint32_t>(id);
        backend_ids[id] = seed.ids[id];
      }
    }
    mapped_groups_ = seed.after;
    backend_to_public_ = std::move(to_public);
    public_to_backend_ = std::move(to_backend);
    backend_ = std::move(replacement);
    packed_ = false;
    return arrow::Status::OK();
  }

  [[nodiscard]] std::uint32_t PublicId(std::uint32_t backend_id) const {
    const auto* map = reinterpret_cast<const std::uint32_t*>(backend_to_public_->data());
    return backend_id < mapped_groups_ ? map[backend_id] : backend_id;
  }

  std::unique_ptr<KeyTable> backend_;
  std::vector<arrow::TypeHolder> original_types_;
  arrow::compute::ExecContext* ctx_;
  std::vector<std::shared_ptr<arrow::Buffer>> scratch_;
  std::shared_ptr<arrow::Buffer> backend_to_public_;
  std::shared_ptr<arrow::Buffer> public_to_backend_;
  std::uint32_t mapped_groups_ = 0;
  bool packed_ = true;
};

}  // namespace

arrow::Result<std::unique_ptr<KeyTable>> MakeKeyTable(
    const std::vector<arrow::TypeHolder>& key_types, arrow::compute::ExecContext* ctx,
    const KeyTableOptions& options) {
  ANTB1_CHECK(!key_types.empty());
  // Short string shapes keep the Grouper with a compact input representation; other shapes
  // use it directly (docs/adr/0019-own-aggregation-hash-table.md).
  const bool known_kind = options.kind == KeyTableKind::kAuto ||
                          options.kind == KeyTableKind::kGrouper ||
                          options.kind == KeyTableKind::kPackedStrings;
  ANTB1_CHECK(known_kind);
  return NoBadAlloc([&] -> arrow::Result<std::unique_ptr<KeyTable>> {
    if (options.kind != KeyTableKind::kGrouper && SupportsPackedStrings(key_types)) {
      const std::vector<arrow::TypeHolder> packed_types(key_types.size(), arrow::uint64());
      ARROW_ASSIGN_OR_RAISE(auto backend, MakeGrouper(packed_types, ctx));
      return std::make_unique<PackedStringKeyTable>(std::move(backend), key_types, ctx);
    }
    return MakeGrouper(key_types, ctx);
  });
}

}  // namespace antb1::exec
