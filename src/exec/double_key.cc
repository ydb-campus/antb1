#include "double_key.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

#include <arrow/api.h>

namespace antb1::exec {

arrow::Result<std::shared_ptr<arrow::Array>> NormalizeDoubleKey(
    const std::shared_ptr<arrow::Array>& column, arrow::MemoryPool* pool) {
  const auto& values = static_cast<const arrow::DoubleArray&>(*column);
  const auto needs_change = [](double v) {
    return (v == 0.0 && std::signbit(v)) ||
           (std::isnan(v) &&
            std::bit_cast<std::uint64_t>(v) !=
                std::bit_cast<std::uint64_t>(std::numeric_limits<double>::quiet_NaN()));
  };
  bool change = false;
  for (std::int64_t i = 0; i < values.length() && !change; ++i) {
    change = values.IsValid(i) && needs_change(values.Value(i));
  }
  if (!change) {
    return column;
  }
  arrow::DoubleBuilder builder(pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(values.length()));
  for (std::int64_t i = 0; i < values.length(); ++i) {
    if (values.IsNull(i)) {
      builder.UnsafeAppendNull();
      continue;
    }
    double v = values.Value(i);
    if (std::isnan(v)) {
      v = std::numeric_limits<double>::quiet_NaN();
    } else if (v == 0.0) {
      v = 0.0;  // -0.0 == 0.0
    }
    builder.UnsafeAppend(v);
  }
  return builder.Finish();
}

}  // namespace antb1::exec
