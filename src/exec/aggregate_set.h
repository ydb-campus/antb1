#pragma once

#include <memory>
#include <vector>

#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

#include "antb1/exec/aggregate_state.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::exec {

// The states of a global aggregation, one AggregateState per call: what ScalarAggregateOperator
// consumes its input into, and what a part of the input is aggregated into before the parts are
// merged in order.
class AggregateSet {
 public:
  // Checks every call's argument against an input `input_width` columns wide.
  static arrow::Result<AggregateSet> Make(const std::vector<plan::AggregateCall>& aggregates,
                                          int input_width, arrow::MemoryPool* pool);

  // The schema of Finalize's row: plan::ToArrow of each call's result type, named agg<i>.
  static std::shared_ptr<arrow::Schema> ResultSchema(
      const std::vector<plan::AggregateCall>& aggregates);

  // Adds a batch's selected rows.
  arrow::Status Consume(const Batch& batch);
  // Adds the rows `other` consumed after the rows this set consumed.
  arrow::Status Merge(const AggregateSet& other);
  // One row with one column per call.
  arrow::Result<std::shared_ptr<arrow::RecordBatch>> Finalize(
      const std::shared_ptr<arrow::Schema>& schema, arrow::MemoryPool* pool) const;

 private:
  explicit AggregateSet(const std::vector<plan::AggregateCall>* aggregates)
      : aggregates_(aggregates) {}

  const std::vector<plan::AggregateCall>* aggregates_;  // owned by the operator
  std::vector<std::unique_ptr<AggregateState>> states_;
};

}  // namespace antb1::exec
