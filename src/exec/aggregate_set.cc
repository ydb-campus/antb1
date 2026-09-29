#include "aggregate_set.h"

#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <arrow/api.h>

#include "antb1/exec/aggregate_state.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

namespace antb1::exec {

arrow::Result<AggregateSet> AggregateSet::Make(const std::vector<plan::AggregateCall>& aggregates,
                                               int input_width, arrow::MemoryPool* pool) {
  AggregateSet set(&aggregates);
  set.states_.reserve(aggregates.size());
  for (const plan::AggregateCall& call : aggregates) {
    std::optional<plan::LogicalType> input;
    if (call.arg.has_value()) {
      if (call.arg->index < 0 || call.arg->index >= input_width) {
        return arrow::Status::Invalid("aggregate over a column outside its input");
      }
      input = call.arg->type;
    }
    ARROW_ASSIGN_OR_RAISE(auto state, MakeAggregateState(call.kind, input, call.type, pool));
    set.states_.push_back(std::move(state));
  }
  return set;
}

std::shared_ptr<arrow::Schema> AggregateSet::ResultSchema(
    const std::vector<plan::AggregateCall>& aggregates) {
  arrow::FieldVector fields;
  fields.reserve(aggregates.size());
  for (std::size_t i = 0; i < aggregates.size(); ++i) {
    fields.push_back(arrow::field(std::format("agg{}", i), plan::ToArrow(aggregates[i].type)));
  }
  return arrow::schema(std::move(fields));
}

arrow::Status AggregateSet::Consume(const Batch& batch) {
  for (std::size_t i = 0; i < states_.size(); ++i) {
    const plan::AggregateCall& call = (*aggregates_)[i];
    if (call.arg.has_value()) {
      ARROW_RETURN_NOT_OK(
          states_[i]->Consume(*batch.data->column(call.arg->index), batch.selection.get()));
    } else {
      ARROW_RETURN_NOT_OK(states_[i]->ConsumeRows(batch.data->num_rows(), batch.selection.get()));
    }
  }
  return arrow::Status::OK();
}

arrow::Status AggregateSet::Merge(const AggregateSet& other) {
  if (other.states_.size() != states_.size()) {
    return arrow::Status::Invalid("merge of aggregates of other calls");
  }
  for (std::size_t i = 0; i < states_.size(); ++i) {
    ARROW_RETURN_NOT_OK(states_[i]->Merge(*other.states_[i]));
  }
  return arrow::Status::OK();
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> AggregateSet::Finalize(
    const std::shared_ptr<arrow::Schema>& schema, arrow::MemoryPool* pool) const {
  arrow::ArrayVector results;
  results.reserve(states_.size());
  for (const auto& state : states_) {
    ARROW_ASSIGN_OR_RAISE(auto value, state->Finalize(pool));
    results.push_back(std::move(value));
  }
  return arrow::RecordBatch::Make(schema, 1, std::move(results));
}

}  // namespace antb1::exec
