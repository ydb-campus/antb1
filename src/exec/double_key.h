#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include <arrow/array/array_base.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/type_fwd.h>

// Private to exec: DOUBLE values as keys of an arrow::compute::Grouper (GROUP BY keys and the
// values of COUNT(DISTINCT)).

namespace antb1::exec {

// A DOUBLE key column with -0.0 as 0.0 and every NaN as one NaN, so that the grouper, which
// compares bytes, groups them as DuckDB does. The column itself when nothing needs to change.
arrow::Result<std::shared_ptr<arrow::Array>> NormalizeDoubleKey(
    const std::shared_ptr<arrow::Array>& column, arrow::MemoryPool* pool);

// Sparse local grouping inspects only accepted rows. Materialization is needed only when one
// of those values has a different representation from its canonical key.
bool NeedsDoubleKeyNormalization(const arrow::DoubleArray& values,
                                 std::span<const std::uint32_t> rows);
arrow::Result<std::shared_ptr<arrow::Array>> SelectDoubleKeys(const arrow::DoubleArray& values,
                                                              std::span<const std::uint32_t> rows,
                                                              arrow::MemoryPool* pool);

}  // namespace antb1::exec
