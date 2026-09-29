#include "antb1/exec/scalar_aggregate.h"

#include <memory>
#include <utility>
#include <vector>

#include <arrow/api.h>

#include "antb1/plan/logical_plan.h"

#include "aggregate_set.h"

namespace antb1::exec {

ScalarAggregateOperator::ScalarAggregateOperator(std::unique_ptr<Operator> input,
                                                 std::vector<plan::AggregateCall> aggregates)
    : input_(std::move(input)),
      aggregates_(std::move(aggregates)),
      schema_(AggregateSet::ResultSchema(aggregates_)) {}

ScalarAggregateOperator::~ScalarAggregateOperator() = default;

arrow::Status ScalarAggregateOperator::Open(ExecContext& ctx) {
  pool_ = ctx.pool;
  done_ = false;
  states_.reset();
  ARROW_ASSIGN_OR_RAISE(
      auto states, AggregateSet::Make(aggregates_, input_->output_schema()->num_fields(), pool_));
  states_ = std::make_unique<AggregateSet>(std::move(states));
  return input_->Open(ctx);
}

arrow::Result<Batch> ScalarAggregateOperator::Next() {
  if (done_) {
    return Batch{};
  }
  if (states_ == nullptr) {
    return arrow::Status::Invalid("aggregate: Next() before Open()");
  }
  while (true) {
    ARROW_ASSIGN_OR_RAISE(const Batch in, input_->Next());
    if (in.end()) {
      break;
    }
    ARROW_RETURN_NOT_OK(states_->Consume(in));
  }
  done_ = true;
  ARROW_ASSIGN_OR_RAISE(auto row, states_->Finalize(schema_, pool_));
  return Batch{.data = std::move(row), .selection = {}};
}

}  // namespace antb1::exec
