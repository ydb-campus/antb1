#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include <arrow/array/array_base.h>
#include <arrow/array/array_primitive.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

// Aggregate functions of the scalar (global) aggregation (docs/sql-subset.md, "Semantics"): each
// state consumes batches, can merge another state of the same aggregate and produces one value.
//
//   COUNT(*)   rows (a true count under WHERE); BIGINT, 0 over no rows
//   COUNT(c)   non-NULL values; BIGINT, 0 over no rows
//   SUM(c)     integers exactly in 128 bits, returned as HUGEINT (decimal128(38, 0)); an overflow
//              of HUGEINT's range is an ExecutionError. DOUBLE: DOUBLE. NULL over no values
//   AVG(c)     integers: the exact 128-bit sum divided once by the count (ExactDivideToDouble);
//              DOUBLE: sum / count. DOUBLE, NULL over no values
//   MIN/MAX(c) Arrow's min_max over the selected values (VARCHAR byte-wise, DATE chronologically);
//              NaN is ignored, in every batch split: NaN only when every value is NaN. The
//              column's type, NULL over no values
//
// NULL values are skipped. Arrow's integer "sum" kernel is never used: it wraps at 64 bits.

namespace antb1::exec {

class AggregateState {
 public:
  AggregateState() = default;
  AggregateState(const AggregateState&) = delete;
  AggregateState& operator=(const AggregateState&) = delete;
  AggregateState(AggregateState&&) = delete;
  AggregateState& operator=(AggregateState&&) = delete;
  virtual ~AggregateState() = default;

  // Adds the selected, non-NULL values of the argument column. selection: nullptr for every row;
  // otherwise a boolean array of values.length() where NULL counts as false. COUNT(*) counts the
  // selected rows of `values`.
  virtual arrow::Status Consume(const arrow::Array& values,
                                const arrow::BooleanArray* selection) = 0;
  // Adds rows without values; only COUNT(*) has no argument and accepts it.
  virtual arrow::Status ConsumeRows(int64_t num_rows, const arrow::BooleanArray* selection);
  // Adds the partial result of another state made for the same aggregate and input type.
  virtual arrow::Status Merge(const AggregateState& other) = 0;
  // The result: a one-element array of plan::ToArrow(result type).
  [[nodiscard]] virtual arrow::Result<std::shared_ptr<arrow::Array>> Finalize(
      arrow::MemoryPool* pool) const = 0;
};

// The state of `kind` over an argument of type `input` (std::nullopt for COUNT(*)) producing
// `result` (plan::AggregateCall::type); `pool` holds its temporary buffers. Invalid for a
// combination the binder never produces.
arrow::Result<std::unique_ptr<AggregateState>> MakeAggregateState(
    plan::AggKind kind, std::optional<plan::LogicalType> input, plan::LogicalType result,
    arrow::MemoryPool* pool = arrow::default_memory_pool());

}  // namespace antb1::exec
