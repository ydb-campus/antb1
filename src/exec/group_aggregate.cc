#include "antb1/exec/group_aggregate.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>
#include <arrow/compute/row/grouper.h>

#include "antb1/exec/grouped_aggregate_state.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "double_key.h"

namespace antb1::exec {
namespace {

std::shared_ptr<arrow::Schema> OutputOf(const std::vector<plan::BoundColumn>& keys,
                                        const std::vector<plan::AggregateCall>& aggregates) {
  arrow::FieldVector fields;
  fields.reserve(keys.size() + aggregates.size());
  for (std::size_t i = 0; i < keys.size(); ++i) {
    fields.push_back(arrow::field(std::format("key{}", i), plan::ToArrow(keys[i].type)));
  }
  for (std::size_t i = 0; i < aggregates.size(); ++i) {
    fields.push_back(arrow::field(std::format("agg{}", i), plan::ToArrow(aggregates[i].type)));
  }
  return arrow::schema(std::move(fields));
}

}  // namespace

GroupAggregateOperator::GroupAggregateOperator(std::unique_ptr<Operator> input,
                                               std::vector<plan::BoundColumn> keys,
                                               std::vector<plan::AggregateCall> aggregates)
    : input_(std::move(input)),
      keys_(std::move(keys)),
      aggregates_(std::move(aggregates)),
      schema_(OutputOf(keys_, aggregates_)) {}

GroupAggregateOperator::~GroupAggregateOperator() = default;

arrow::Status GroupAggregateOperator::Open(ExecContext& ctx) {
  pool_ = ctx.pool;
  memory_.Reset(ctx.budget);
  done_ = false;
  num_groups_ = 0;
  chunk_groups_.clear();
  next_chunk_ = 0;
  next_group_ = 0;
  states_.clear();
  first_keys_.assign(keys_.size(), {});
  const int width = input_->output_schema()->num_fields();
  std::vector<arrow::TypeHolder> key_types;
  for (const plan::BoundColumn& key : keys_) {
    if (key.index < 0 || key.index >= width) {
      return arrow::Status::Invalid("grouping key outside its input");
    }
    key_types.emplace_back(plan::ToArrow(key.type));
  }
  for (const plan::AggregateCall& call : aggregates_) {
    std::optional<plan::LogicalType> input;
    if (call.arg.has_value()) {
      if (call.arg->index < 0 || call.arg->index >= width) {
        return arrow::Status::Invalid("aggregate over a column outside its input");
      }
      input = call.arg->type;
    }
    ARROW_ASSIGN_OR_RAISE(auto state,
                          MakeGroupedAggregateState(call.kind, input, call.type, pool_));
    states_.push_back(std::move(state));
  }
  kernels_ = std::make_unique<arrow::compute::ExecContext>(pool_);
  grouper_.reset();
  if (!keys_.empty()) {
    ARROW_ASSIGN_OR_RAISE(grouper_, arrow::compute::Grouper::Make(key_types, kernels_.get()));
  }
  opened_ = true;
  return input_->Open(ctx);
}

arrow::Status GroupAggregateOperator::Consume(const arrow::RecordBatch& rows) {
  const std::int64_t n = rows.num_rows();
  if (keys_.empty()) {  // one group of every row
    if (num_groups_ == 0) {
      num_groups_ = 1;
      chunk_groups_.push_back(1);
    }
    const std::vector<std::uint32_t> group_ids(static_cast<std::size_t>(n), 0);
    return ConsumeAggregates(rows, group_ids);
  }
  std::vector<arrow::Datum> keys;
  keys.reserve(keys_.size());
  for (const plan::BoundColumn& key : keys_) {
    std::shared_ptr<arrow::Array> column = rows.column(key.index);
    if (key.type == plan::LogicalType::kDouble) {
      ARROW_ASSIGN_OR_RAISE(column, NormalizeDoubleKey(column, pool_));
    }
    keys.emplace_back(std::move(column));
  }
  const arrow::compute::ExecBatch key_batch(std::move(keys), n);
  ARROW_ASSIGN_OR_RAISE(const arrow::Datum id_datum,
                        grouper_->Consume(arrow::compute::ExecSpan(key_batch)));
  const auto ids = std::static_pointer_cast<arrow::UInt32Array>(id_datum.make_array());
  const std::span<const std::uint32_t> group_ids(ids->raw_values(),
                                                 static_cast<std::size_t>(ids->length()));
  const std::uint32_t before = num_groups_;
  const std::uint32_t after = grouper_->num_groups();
  if (after > before) {
    // Keep the original key values (not the normalized ones) of each new group's first row. The
    // grouper numbers new groups in an order of its own, so the first rows are found per id.
    std::vector<std::int64_t> first(after - before, -1);
    std::uint32_t found = 0;
    for (std::size_t i = 0; i < group_ids.size() && found < after - before; ++i) {
      const std::uint32_t id = group_ids[i];
      if (id >= before && first[id - before] < 0) {
        first[id - before] = static_cast<std::int64_t>(i);
        ++found;
      }
    }
    arrow::Int64Builder first_rows(pool_);
    ARROW_RETURN_NOT_OK(first_rows.AppendValues(first));
    ARROW_ASSIGN_OR_RAISE(const auto indices, first_rows.Finish());
    for (std::size_t k = 0; k < keys_.size(); ++k) {
      ARROW_ASSIGN_OR_RAISE(
          const arrow::Datum taken,
          arrow::compute::Take(rows.column(keys_[k].index), indices,
                               arrow::compute::TakeOptions::NoBoundsCheck(), kernels_.get()));
      first_keys_[k].push_back(taken.make_array());
    }
    chunk_groups_.push_back(after - before);
    num_groups_ = after;
  }
  return ConsumeAggregates(rows, group_ids);
}

arrow::Status GroupAggregateOperator::ConsumeAggregates(const arrow::RecordBatch& rows,
                                                        std::span<const std::uint32_t> group_ids) {
  for (std::size_t i = 0; i < aggregates_.size(); ++i) {
    states_[i]->Resize(num_groups_);
    const plan::AggregateCall& call = aggregates_[i];
    const arrow::Array* values =
        call.arg.has_value() ? rows.column(call.arg->index).get() : nullptr;
    ARROW_RETURN_NOT_OK(states_[i]->Consume(values, group_ids));
  }
  return Account();
}

arrow::Status GroupAggregateOperator::Account() {
  auto bytes = static_cast<std::int64_t>(chunk_groups_.capacity() * sizeof(std::uint32_t));
  for (const auto& state : states_) {
    bytes += state->memory_usage();
  }
  return memory_.Resize(bytes);
}

arrow::Status GroupAggregateOperator::Close() {
  states_.clear();
  memory_.Release();
  return input_->Close();
}

arrow::Result<Batch> GroupAggregateOperator::Next() {
  if (!opened_) {
    return arrow::Status::Invalid("group aggregate: Next() before Open()");
  }
  if (done_) {
    if (next_chunk_ >= chunk_groups_.size()) {
      states_.clear();
      memory_.Release();
      return Batch{};
    }
    // One batch per chunk of new groups: its keys as first seen, the aggregates of those groups.
    const std::uint32_t rows = chunk_groups_[next_chunk_];
    arrow::ArrayVector columns;
    columns.reserve(keys_.size() + states_.size());
    for (auto& chunks : first_keys_) {
      columns.push_back(std::move(chunks[next_chunk_]));
    }
    for (const auto& state : states_) {
      ARROW_ASSIGN_OR_RAISE(auto column, state->Finalize(next_group_, next_group_ + rows, pool_));
      columns.push_back(std::move(column));
    }
    ++next_chunk_;
    next_group_ += rows;
    return Batch{.data = arrow::RecordBatch::Make(schema_, rows, std::move(columns)),
                 .selection = {}};
  }
  while (true) {
    ARROW_ASSIGN_OR_RAISE(const Batch in, input_->Next());
    if (in.end()) {
      break;
    }
    ARROW_ASSIGN_OR_RAISE(const auto rows, Materialize(in, pool_));
    if (rows->num_rows() > 0) {
      ARROW_RETURN_NOT_OK(Consume(*rows));
    }
  }
  for (const auto& state : states_) {
    state->Resize(num_groups_);
  }
  ARROW_RETURN_NOT_OK(Account());
  done_ = true;
  return Next();
}

}  // namespace antb1::exec
