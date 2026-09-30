#include "outer_groups.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>
#include <arrow/compute/row/grouper.h>

#include "antb1/common/check.h"
#include "antb1/common/narrow.h"
#include "antb1/exec/grouped_aggregate_state.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "group_table.h"

namespace antb1::exec {
namespace {

// The rows `rows` of every column of `batch`.
arrow::Result<arrow::compute::ExecBatch> TakeRows(const arrow::compute::ExecBatch& batch,
                                                  std::span<const std::uint32_t> rows,
                                                  arrow::compute::ExecContext* kernels) {
  arrow::UInt32Builder indices(kernels->memory_pool());
  ARROW_RETURN_NOT_OK(indices.AppendValues(rows.data(), Narrow<int64_t>(rows.size())));
  ARROW_ASSIGN_OR_RAISE(const auto taken_rows, indices.Finish());
  std::vector<arrow::Datum> values;
  values.reserve(batch.values.size());
  for (const arrow::Datum& column : batch.values) {
    ARROW_ASSIGN_OR_RAISE(
        arrow::Datum taken,
        arrow::compute::Take(column, taken_rows, arrow::compute::TakeOptions::NoBoundsCheck(),
                             kernels));
    values.push_back(std::move(taken));
  }
  return arrow::compute::ExecBatch(std::move(values), Narrow<int64_t>(rows.size()));
}

}  // namespace

OuterGroups::OuterGroups(std::vector<plan::LogicalType> key_types, arrow::MemoryPool* pool)
    : key_types_(std::move(key_types)), pool_(pool) {}

OuterGroups::~OuterGroups() = default;

arrow::Result<std::unique_ptr<OuterGroups>> OuterGroups::Make(
    std::vector<plan::LogicalType> key_types, std::size_t distinct_columns,
    const std::vector<plan::AggregateCall>& plain, arrow::MemoryPool* pool, MemoryBudget* budget) {
  std::unique_ptr<OuterGroups> table(new OuterGroups(std::move(key_types), pool));
  table->memory_.Reset(budget);
  table->kernels_ = std::make_unique<arrow::compute::ExecContext>(pool);
  if (!table->key_types_.empty()) {
    std::vector<arrow::TypeHolder> types;
    for (const plan::LogicalType type : table->key_types_) {
      ANTB1_CHECK(type != plan::LogicalType::kDouble);  // TwoLevelAggregation rules it out
      types.emplace_back(plan::ToArrow(type));
    }
    ARROW_ASSIGN_OR_RAISE(table->grouper_,
                          arrow::compute::Grouper::Make(types, table->kernels_.get()));
  }
  table->counts_.assign(distinct_columns, {});
  for (const plan::AggregateCall& call : plain) {
    std::optional<plan::LogicalType> input;
    if (call.arg.has_value()) {
      input = call.arg->type;
    }
    ARROW_ASSIGN_OR_RAISE(auto state, MakeGroupedAggregateState(call.kind, input, call.type, pool));
    table->states_.push_back(std::move(state));
  }
  return table;
}

arrow::Status OuterGroups::Resize(std::uint32_t groups) {
  if (groups <= num_groups_) {
    return arrow::Status::OK();
  }
  num_groups_ = groups;
  for (std::vector<std::int64_t>& counts : counts_) {
    counts.resize(groups, 0);
  }
  for (const auto& state : states_) {
    state->Resize(groups);
  }
  uniques_.reset();
  return Account();
}

arrow::Result<std::vector<std::uint32_t>> OuterGroups::GroupsOf(
    const arrow::compute::ExecBatch& keys) {
  const auto rows = static_cast<std::size_t>(keys.length);
  if (grouper_ == nullptr) {  // every row in the one group
    if (rows > 0) {
      ARROW_RETURN_NOT_OK(Resize(1));
    }
    return std::vector<std::uint32_t>(rows, 0);
  }
  ANTB1_CHECK(keys.values.size() >= key_types_.size());
  std::vector<arrow::Datum> columns(
      keys.values.begin(), keys.values.begin() + static_cast<std::ptrdiff_t>(key_types_.size()));
  const arrow::compute::ExecBatch key_batch(std::move(columns), keys.length);
  ARROW_ASSIGN_OR_RAISE(const arrow::Datum ids,
                        grouper_->Consume(arrow::compute::ExecSpan(key_batch)));
  const auto array = std::static_pointer_cast<arrow::UInt32Array>(ids.make_array());
  ARROW_RETURN_NOT_OK(Resize(grouper_->num_groups()));
  return std::vector<std::uint32_t>(array->raw_values(), array->raw_values() + array->length());
}

arrow::Status OuterGroups::AddDistinct(std::size_t column, const arrow::compute::ExecBatch& inner) {
  ANTB1_CHECK(column < counts_.size());
  ANTB1_CHECK(inner.values.size() == key_types_.size() + 1);
  ARROW_ASSIGN_OR_RAISE(const std::vector<std::uint32_t> groups, GroupsOf(inner));
  const std::shared_ptr<arrow::Array> values = inner.values.back().make_array();
  std::vector<std::int64_t>& counts = counts_[column];
  for (std::size_t i = 0; i < groups.size(); ++i) {
    if (values->IsValid(static_cast<std::int64_t>(i))) {  // COUNT(DISTINCT) skips NULL
      ++counts[groups[i]];
    }
  }
  return arrow::Status::OK();
}

arrow::Status OuterGroups::AddPlain(const GroupTable& plain) {
  if (plain.num_groups() == 0) {
    return arrow::Status::OK();
  }
  std::vector<std::uint32_t> to;
  if (grouper_ == nullptr) {
    ARROW_ASSIGN_OR_RAISE(to, GroupsOf(arrow::compute::ExecBatch({}, plain.num_groups())));
  } else {
    ARROW_ASSIGN_OR_RAISE(const arrow::compute::ExecBatch keys, plain.uniques());
    ARROW_ASSIGN_OR_RAISE(to, GroupsOf(keys));
  }
  std::vector<std::uint32_t> from(to.size());
  for (std::size_t i = 0; i < from.size(); ++i) {
    from[i] = static_cast<std::uint32_t>(i);
  }
  for (std::size_t i = 0; i < states_.size(); ++i) {
    ARROW_RETURN_NOT_OK(states_[i]->MergeGroups(plain.state(i), from, to));
  }
  return Account();
}

arrow::Status OuterGroups::Merge(const OuterGroups& other, std::span<const std::uint32_t> from) {
  if (from.empty()) {
    return arrow::Status::OK();
  }
  ANTB1_CHECK(other.counts_.size() == counts_.size());
  ANTB1_CHECK(other.states_.size() == states_.size());
  std::vector<std::uint32_t> to;
  if (grouper_ == nullptr) {
    ARROW_ASSIGN_OR_RAISE(to,
                          GroupsOf(arrow::compute::ExecBatch({}, Narrow<int64_t>(from.size()))));
  } else {
    ARROW_ASSIGN_OR_RAISE(const arrow::compute::ExecBatch uniques, other.grouper_->GetUniques());
    ARROW_ASSIGN_OR_RAISE(const arrow::compute::ExecBatch keys,
                          TakeRows(uniques, from, kernels_.get()));
    ARROW_ASSIGN_OR_RAISE(to, GroupsOf(keys));
  }
  for (std::size_t c = 0; c < counts_.size(); ++c) {
    for (std::size_t i = 0; i < from.size(); ++i) {
      counts_[c][to[i]] += other.counts_[c][from[i]];
    }
  }
  for (std::size_t i = 0; i < states_.size(); ++i) {
    ARROW_RETURN_NOT_OK(states_[i]->MergeGroups(*other.states_[i], from, to));
  }
  return Account();
}

arrow::Status OuterGroups::EnsureGroup() {
  ANTB1_CHECK(grouper_ == nullptr);
  return Resize(1);
}

arrow::Result<std::vector<std::uint64_t>> OuterGroups::Hashes() const {
  ANTB1_CHECK(grouper_ != nullptr);
  ARROW_ASSIGN_OR_RAISE(const arrow::compute::ExecBatch uniques, grouper_->GetUniques());
  return KeyHashes(uniques);
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> OuterGroups::Rows(
    std::uint32_t begin, std::uint32_t end, std::span<const std::uint32_t> groups,
    const std::vector<OuterSlot>& slots, const std::shared_ptr<arrow::Schema>& schema,
    bool with_keys) {
  ANTB1_CHECK(begin <= end);
  ANTB1_CHECK(end <= num_groups_);
  // Positions of `groups` within [begin, end).
  arrow::UInt32Builder position_builder(pool_);
  for (const std::uint32_t group : groups) {
    ANTB1_CHECK(group >= begin);
    ANTB1_CHECK(group < end);
    ARROW_RETURN_NOT_OK(position_builder.Append(group - begin));
  }
  ARROW_ASSIGN_OR_RAISE(const auto positions, position_builder.Finish());
  arrow::ArrayVector columns;
  if (with_keys && grouper_ != nullptr) {
    if (!uniques_.has_value()) {
      ARROW_ASSIGN_OR_RAISE(uniques_, grouper_->GetUniques());
    }
    ARROW_ASSIGN_OR_RAISE(const arrow::compute::ExecBatch keys,
                          TakeRows(*uniques_, groups, kernels_.get()));
    for (const arrow::Datum& key : keys.values) {
      columns.push_back(key.make_array());
    }
  }
  for (const OuterSlot& slot : slots) {
    std::shared_ptr<arrow::Array> column;
    if (slot.distinct) {
      ANTB1_CHECK(slot.index < counts_.size());
      arrow::Int64Builder counts(pool_);
      ARROW_RETURN_NOT_OK(counts.Reserve(Narrow<int64_t>(groups.size())));
      for (const std::uint32_t group : groups) {
        counts.UnsafeAppend(counts_[slot.index][group]);
      }
      ARROW_ASSIGN_OR_RAISE(column, counts.Finish());
    } else {
      ANTB1_CHECK(slot.index < states_.size());
      ARROW_ASSIGN_OR_RAISE(const auto range, states_[slot.index]->Finalize(begin, end, pool_));
      ARROW_ASSIGN_OR_RAISE(
          const arrow::Datum taken,
          arrow::compute::Take(range, positions, arrow::compute::TakeOptions::NoBoundsCheck(),
                               kernels_.get()));
      column = taken.make_array();
    }
    columns.push_back(std::move(column));
  }
  ANTB1_CHECK(std::cmp_equal(columns.size(), schema->num_fields()));
  return arrow::RecordBatch::Make(schema, Narrow<int64_t>(groups.size()), std::move(columns));
}

arrow::Status OuterGroups::Account() {
  std::int64_t bytes = 0;
  for (const std::vector<std::int64_t>& counts : counts_) {
    bytes += Narrow<std::int64_t>(counts.capacity() * sizeof(std::int64_t));
  }
  for (const auto& state : states_) {
    bytes += state->memory_usage();
  }
  return memory_.Resize(bytes);
}

}  // namespace antb1::exec
