#include "key_table.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <arrow/array/data.h>
#include <arrow/array/util.h>
#include <arrow/compute/exec.h>
#include <arrow/compute/row/grouper.h>
#include <arrow/datum.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include "antb1/common/check.h"

#include "fixed_key_table.h"

namespace antb1::exec {
namespace {

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

}  // namespace

arrow::Result<std::unique_ptr<KeyTable>> MakeKeyTable(
    const std::vector<arrow::TypeHolder>& key_types, arrow::compute::ExecContext* ctx,
    const KeyTableOptions& options) {
  ANTB1_CHECK(!key_types.empty());
  return NoBadAlloc([&] -> arrow::Result<std::unique_ptr<KeyTable>> {
    // The shapes of our own tables (docs/adr/0019-own-aggregation-hash-table.md); the Grouper
    // takes every other.
    if (options.kind != KeyTableKind::kGrouper) {
      if (std::optional<FixedKeyLayout> layout = FixedKeyLayoutOf(key_types); layout.has_value()) {
        const Growth growth =
            options.kind == KeyTableKind::kFixedIdOrder ? Growth::kIdOrder : Growth::kSlotOrder;
        return MakeFixedKeyTable(*std::move(layout), growth, ctx->memory_pool(), options);
      }
      if (options.kind != KeyTableKind::kAuto) {
        return arrow::Status::Invalid("a fixed-width key table cannot take ", key_types.size(),
                                      " keys of these types");
      }
    }
    ARROW_ASSIGN_OR_RAISE(auto grouper, arrow::compute::Grouper::Make(key_types, ctx));
    return std::make_unique<GrouperKeyTable>(std::move(grouper), ctx->memory_pool());
  });
}

}  // namespace antb1::exec
