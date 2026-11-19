#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include <arrow/api.h>

#include "antb1/common/int128.h"
#include "antb1/exec/sort.h"
#include "antb1/plan/logical_plan.h"

#include "variance.h"

namespace antb1::exec {

// One running aggregate's state. Extremes refer to retained input, including VARCHAR values;
// there are no per-row string copies. Copying a state permits a smaller-chunk retry on capacity.
struct WindowPrefixState {
  Int128 exact_sum = 0;
  double sum = 0;
  int64_t count = 0;
  Moments moments;
  std::optional<SortBuffer::RowRef> best;
};

// Evaluate one bounded range of a sorted layout, carrying state between ranges. starts[i]
// resets the state before row i. Dispatch is per call/range; typed loops emit one prefix per row.
arrow::Result<std::shared_ptr<arrow::Array>> WindowPrefixes(
    const plan::WindowCall& call, const arrow::RecordBatchVector& input,
    std::span<const SortBuffer::RowRef> rows, std::span<const std::uint8_t> starts,
    WindowPrefixState& state, arrow::MemoryPool* pool);

}  // namespace antb1::exec
