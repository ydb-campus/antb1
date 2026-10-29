#pragma once

#include <memory>

#include <arrow/array/array_base.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>

// Private to exec: DOUBLE values as keys of a table that compares their bytes (GROUP BY keys, whose
// key table is antb1's own or arrow::compute::Grouper, and the values of COUNT(DISTINCT), a
// Grouper's).

namespace antb1::exec {

// A DOUBLE key column with -0.0 as 0.0 and every NaN as one NaN, so that a table that compares
// bytes groups them as DuckDB does. The column itself when nothing needs to change.
arrow::Result<std::shared_ptr<arrow::Array>> NormalizeDoubleKey(
    const std::shared_ptr<arrow::Array>& column, arrow::MemoryPool* pool);

}  // namespace antb1::exec
