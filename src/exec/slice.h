#pragma once

#include <cstdint>
#include <memory>

#include <arrow/result.h>
#include <arrow/type_fwd.h>

namespace antb1::exec {

// A zero-copy slice that lets heap allocation failures reach the caller's error boundary.
arrow::Result<std::shared_ptr<arrow::Array>> SliceColumn(const arrow::Array& array, int64_t offset,
                                                         int64_t length);
arrow::Result<std::shared_ptr<arrow::RecordBatch>> SliceBatch(
    const std::shared_ptr<arrow::RecordBatch>& batch, int64_t start, int64_t length);

}  // namespace antb1::exec
