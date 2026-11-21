#include "double_key.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>

#include <arrow/api.h>

#include "antb1/common/narrow.h"

namespace antb1::exec {

namespace {

bool NeedsChange(double v) {
  return (v == 0.0 && std::signbit(v)) ||
         (std::isnan(v) &&
          std::bit_cast<std::uint64_t>(v) !=
              std::bit_cast<std::uint64_t>(std::numeric_limits<double>::quiet_NaN()));
}

arrow::Result<std::shared_ptr<arrow::Array>> BuildDoubleKeys(const arrow::DoubleArray& values,
                                                             std::int64_t count, const auto& row_at,
                                                             arrow::MemoryPool* pool) {
  arrow::DoubleBuilder builder(pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(count));
  for (std::int64_t i = 0; i < count; ++i) {
    const auto row = row_at(i);
    if (values.IsNull(row)) {
      builder.UnsafeAppendNull();
      continue;
    }
    double v = values.Value(row);
    if (std::isnan(v)) {
      v = std::numeric_limits<double>::quiet_NaN();
    } else if (v == 0.0) {
      v = 0.0;  // -0.0 == 0.0
    }
    builder.UnsafeAppend(v);
  }
  return builder.Finish();
}

}  // namespace

arrow::Result<std::shared_ptr<arrow::Array>> NormalizeDoubleKey(
    const std::shared_ptr<arrow::Array>& column, arrow::MemoryPool* pool) {
  const auto& values = static_cast<const arrow::DoubleArray&>(*column);
  for (std::int64_t i = 0; i < values.length(); ++i) {
    if (values.IsValid(i) && NeedsChange(values.Value(i))) {
      return BuildDoubleKeys(values, values.length(), std::identity{}, pool);
    }
  }
  return column;
}

bool NeedsDoubleKeyNormalization(const arrow::DoubleArray& values,
                                 std::span<const std::uint32_t> rows) {
  return std::ranges::any_of(rows, [&](std::uint32_t row) {
    return values.IsValid(row) && NeedsChange(values.Value(row));
  });
}

arrow::Result<std::shared_ptr<arrow::Array>> SelectDoubleKeys(const arrow::DoubleArray& values,
                                                              std::span<const std::uint32_t> rows,
                                                              arrow::MemoryPool* pool) {
  return BuildDoubleKeys(
      values, Narrow<std::int64_t>(rows.size()),
      [&](std::int64_t i) { return rows[static_cast<std::size_t>(i)]; }, pool);
}

}  // namespace antb1::exec
