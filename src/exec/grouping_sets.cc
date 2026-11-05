#include "antb1/exec/grouping_sets.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/exec.h>

#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

#include "group_table.h"
#include "grouping_sets_shape.h"

namespace antb1::exec {

GroupingSetsOperator::GroupingSetsOperator(std::unique_ptr<Operator> input,
                                           std::vector<plan::BoundColumn> keys,
                                           const std::vector<std::vector<std::size_t>>& sets,
                                           std::vector<plan::AggregateCall> aggregates,
                                           std::vector<plan::GroupingCall> grouping, bool per_set)
    : input_(std::move(input)),
      keys_(std::move(keys)),
      aggregates_(std::move(aggregates)),
      grouping_(std::move(grouping)),
      shape_(std::make_unique<GroupingSetsShape>(
          PlanGroupingSets(sets, !per_set && MergesExactly(keys_, aggregates_)))),
      schema_(GroupingSetsSchema(keys_, aggregates_, grouping_.size())) {}

GroupingSetsOperator::~GroupingSetsOperator() = default;

arrow::Status GroupingSetsOperator::Open(ExecContext& ctx) {
  ARROW_RETURN_NOT_OK(Close());
  ctx_ = ctx;
  opened_ = true;
  return input_->Open(ctx);
}

arrow::Status GroupingSetsOperator::Close() {
  tables_.clear();
  rows_.clear();
  next_entry_ = 0;
  next_batch_ = 0;
  aggregated_ = false;
  const bool opened = std::exchange(opened_, false);
  return opened ? input_->Close() : arrow::Status::OK();
}

arrow::Status GroupingSetsOperator::Aggregate() {
  const GroupingSetsShape& shape = *shape_;
  const int width = input_->output_schema()->num_fields();
  tables_.resize(shape.sets.size());
  for (std::size_t s = 0; s < shape.sets.size(); ++s) {
    ARROW_ASSIGN_OR_RAISE(tables_[s], GroupTable::Make(SetKeys(keys_, shape.sets[s]), aggregates_,
                                                       width, ctx_.pool, ctx_.budget));
  }
  while (true) {
    ARROW_ASSIGN_OR_RAISE(const Batch in, input_->Next());
    if (in.end()) {
      break;
    }
    ARROW_ASSIGN_OR_RAISE(const auto rows, Materialize(in, ctx_.pool));
    if (rows->num_rows() == 0) {
      continue;
    }
    for (std::size_t s = 0; s < shape.sets.size(); ++s) {
      if (shape.root(s)) {
        ARROW_RETURN_NOT_OK(tables_[s]->Consume(*rows));
      }
    }
  }
  // The second level: every other set from all of its root's groups, the root's keys taken once.
  for (std::size_t r = 0; r < shape.sets.size(); ++r) {
    if (!shape.root(r) || tables_[r]->num_groups() == 0) {
      continue;
    }
    std::vector<std::uint32_t> every(tables_[r]->num_groups());
    std::ranges::iota(every, 0U);
    std::optional<arrow::compute::ExecBatch> uniques;
    for (std::size_t s = 0; s < shape.sets.size(); ++s) {
      if (shape.sources[s] != r || s == r) {
        continue;
      }
      if (!uniques.has_value() && !shape.sets[r].empty()) {
        ARROW_ASSIGN_OR_RAISE(uniques, tables_[r]->uniques());
      }
      const arrow::compute::ExecBatch keys =
          uniques.has_value() ? ProjectKeys(*uniques, shape.columns[s])
                              : arrow::compute::ExecBatch({}, static_cast<int64_t>(every.size()));
      ARROW_RETURN_NOT_OK(tables_[s]->MergeProjected(*tables_[r], keys, every));
    }
  }
  for (std::size_t s = 0; s < shape.sets.size(); ++s) {
    if (shape.sets[s].empty()) {
      ARROW_RETURN_NOT_OK(tables_[s]->EnsureGroup());
    }
    // Every set's finalize errors before the first batch, which a LIMIT may stop after.
    ARROW_RETURN_NOT_OK(tables_[s]->CheckFinalize());
  }
  rows_.assign(shape.sets.size(), {});
  return arrow::Status::OK();
}

arrow::Result<Batch> GroupingSetsOperator::Next() {
  if (!opened_) {
    return arrow::Status::Invalid("grouping sets: Next() before Open()");
  }
  if (!aggregated_) {
    ARROW_RETURN_NOT_OK(Aggregate());
    aggregated_ = true;
  }
  const GroupingSetsShape& shape = *shape_;
  for (; next_entry_ < shape.entries.size(); ++next_entry_, next_batch_ = 0) {
    const std::size_t s = shape.entries[next_entry_];
    if (tables_[s] != nullptr) {  // its first entry: the rows built once
      const std::shared_ptr<arrow::Schema> set_schema =
          SetSchema(keys_, shape.sets[s], aggregates_);
      const std::vector<std::int64_t> grouping = GroupingValues(shape.sets[s], grouping_);
      while (true) {
        ARROW_ASSIGN_OR_RAISE(auto chunk, tables_[s]->NextChunk(set_schema));
        if (chunk == nullptr) {
          break;
        }
        ARROW_ASSIGN_OR_RAISE(auto rows,
                              SetRows(*chunk, shape.sets[s], grouping, schema_, ctx_.pool));
        rows_[s].push_back(std::move(rows));
      }
      tables_[s].reset();  // gives the memory back
    }
    std::vector<std::shared_ptr<arrow::RecordBatch>>& rows = rows_[s];
    if (next_batch_ < rows.size()) {
      return Batch{.data = rows[next_batch_++], .selection = {}};
    }
    if (shape.last_entries[s] == next_entry_) {
      rows = {};
    }
  }
  return Batch{};
}

}  // namespace antb1::exec
