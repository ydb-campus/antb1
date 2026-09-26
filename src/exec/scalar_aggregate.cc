#include "antb1/exec/scalar_aggregate.h"

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
namespace {

std::shared_ptr<arrow::Schema> ResultsOf(const std::vector<plan::AggregateCall>& aggregates) {
  arrow::FieldVector fields;
  fields.reserve(aggregates.size());
  for (std::size_t i = 0; i < aggregates.size(); ++i) {
    fields.push_back(arrow::field(std::format("agg{}", i), plan::ToArrow(aggregates[i].type)));
  }
  return arrow::schema(std::move(fields));
}

}  // namespace

ScalarAggregateOperator::ScalarAggregateOperator(std::unique_ptr<Operator> input,
                                                 std::vector<plan::AggregateCall> aggregates)
    : input_(std::move(input)),
      aggregates_(std::move(aggregates)),
      schema_(ResultsOf(aggregates_)) {}

arrow::Status ScalarAggregateOperator::Open(ExecContext& ctx) {
  pool_ = ctx.pool;
  done_ = false;
  states_.clear();
  const int width = input_->output_schema()->num_fields();
  for (const plan::AggregateCall& call : aggregates_) {
    std::optional<plan::LogicalType> input;
    if (call.arg.has_value()) {
      if (call.arg->index < 0 || call.arg->index >= width) {
        return arrow::Status::Invalid("aggregate over a column outside its input");
      }
      input = call.arg->type;
    }
    ARROW_ASSIGN_OR_RAISE(auto state, MakeAggregateState(call.kind, input, call.type, pool_));
    states_.push_back(std::move(state));
  }
  return input_->Open(ctx);
}

arrow::Result<Batch> ScalarAggregateOperator::Next() {
  if (done_) {
    return Batch{};
  }
  if (states_.size() != aggregates_.size()) {
    return arrow::Status::Invalid("aggregate: Next() before Open()");
  }
  while (true) {
    ARROW_ASSIGN_OR_RAISE(const Batch in, input_->Next());
    if (in.end()) {
      break;
    }
    for (std::size_t i = 0; i < aggregates_.size(); ++i) {
      const plan::AggregateCall& call = aggregates_[i];
      if (call.arg.has_value()) {
        ARROW_RETURN_NOT_OK(
            states_[i]->Consume(*in.data->column(call.arg->index), in.selection.get()));
      } else {
        ARROW_RETURN_NOT_OK(states_[i]->ConsumeRows(in.data->num_rows(), in.selection.get()));
      }
    }
  }
  done_ = true;
  arrow::ArrayVector results;
  results.reserve(states_.size());
  for (const auto& state : states_) {
    ARROW_ASSIGN_OR_RAISE(auto value, state->Finalize(pool_));
    results.push_back(std::move(value));
  }
  return Batch{.data = arrow::RecordBatch::Make(schema_, 1, std::move(results)), .selection = {}};
}

}  // namespace antb1::exec
