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

struct WindowPrefixPiece {
  std::uint8_t* values;
  std::uint8_t* validity;
  int64_t nulls;
};
struct WindowPrefixTarget {
  std::span<const std::size_t> first;
  std::span<WindowPrefixPiece> pieces;
  std::size_t width;
  std::size_t batch_rows;
};

// Evaluate one bounded range of a sorted layout, carrying state between ranges. starts[i]
// resets the state before row i. arrays caches the argument array of each retained input chunk
// (empty for COUNT(*)). Input chunks must outlive the state and every range. Dispatch is per
// call/range; typed loops emit one prefix per row without accessing RecordBatch columns.
arrow::Result<std::shared_ptr<arrow::Array>> WindowPrefixes(
    const plan::WindowCall& call, std::span<const arrow::Array* const> arrays,
    std::span<const SortBuffer::RowRef> rows, std::span<const std::uint8_t> starts,
    WindowPrefixState& state, arrow::MemoryPool* pool, WindowPrefixTarget* target = nullptr);

}  // namespace antb1::exec
