#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include <arrow/api.h>

#include "antb1/common/int128.h"
#include "antb1/plan/logical_plan.h"

// DECIMAL kernels the executor shares (ADR 0021): DuckDB's conversion of a DECIMAL to DOUBLE and
// its casts to a DECIMAL, and comparisons of DECIMALs of any scale with each other and with
// integers, exactly by value.

namespace antb1::exec {

Int128 ToInt128(const arrow::Decimal128& d);
arrow::Decimal128 FromInt128(Int128 v);

// The values of an integer (int16, uint16, int32, int64) or decimal128 array as unscaled Int128
// (0 where NULL), with the array's scale (0 for an integer).
struct UnscaledValues {
  std::vector<Int128> values;
  int scale = 0;
};
arrow::Result<UnscaledValues> ReadUnscaled(const arrow::Array& array);

// A decimal128 array converted to DOUBLE as DuckDB converts a DECIMAL (ADR 0021 rule 8), at the
// array's own precision and scale, so a HUGEINT (decimal128(38,0)) goes through DuckDB's 128-bit
// formula; NULL stays NULL.
arrow::Result<std::shared_ptr<arrow::Array>> DecimalToDouble(const arrow::Array& values,
                                                             arrow::MemoryPool* pool);

// DuckDB's error for a value (unscaled, of logical type `from`: a DECIMAL or an integer type) that
// does not fit the DECIMAL `to` it is cast to: `Casting value "<value>" to type DECIMAL(p,s)
// failed: value is out of range!` for a DECIMAL, printed at its own scale, and `Could not cast
// value <value> to DECIMAL(p,s)` for an integer, followed by ` when casting from source column
// <column>` when `column` is not empty (DuckDB names the column it casts).
arrow::Status DecimalCastError(Int128 value, plan::LogicalType from, plan::LogicalType to,
                               std::string_view column);

// An integer or decimal128 array of logical type `from` cast to the DECIMAL `to` (decimal128 of
// exactly its width and scale) as DuckDB casts it (ADR 0021 rule 10): rescaled, rounded half away
// from zero where the scale shrinks. A value that does not fit `to`'s width, even one beyond its
// own declared width, fails with DecimalCastError; NULL stays NULL.
arrow::Result<std::shared_ptr<arrow::Array>> CastToDecimal(const arrow::Array& values,
                                                           plan::LogicalType from,
                                                           plan::LogicalType to,
                                                           std::string_view column,
                                                           arrow::MemoryPool* pool);

// left <op> right by value for integer and decimal128 arrays of any types and scales (ADR 0021
// rule 11): a BOOLEAN array, NULL where either side is NULL. It never fails on a value, even one
// beyond its declared width (a file can hold such values, up to the Int128 extremes).
arrow::Result<std::shared_ptr<arrow::Array>> CompareExact(const arrow::Array& left,
                                                          const arrow::Array& right,
                                                          plan::CompareOp op,
                                                          arrow::MemoryPool* pool);

}  // namespace antb1::exec
