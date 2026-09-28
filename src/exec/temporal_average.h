#pragma once

#include <cstdint>
#include <limits>

#include <arrow/array.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include "antb1/common/int128.h"

// AVG of DATE and TIMESTAMP values as DuckDB computes it (docs/sql-subset.md): over the
// microseconds of each TIMESTAMP, or of each DATE's midnight, into a TIMESTAMP.

namespace antb1::exec {

inline constexpr int64_t kMicrosPerDay = 86'400'000'000;

// The microseconds of row `row` of a DATE or TIMESTAMP array (not NULL): a DATE's midnight, its
// infinities as the TIMESTAMP infinities (+-INT64_MAX); a DATE beyond the TIMESTAMP range is an
// error, as DuckDB's cast to TIMESTAMP is.
inline arrow::Result<int64_t> TemporalMicros(const arrow::Array& values, int64_t row) {
  if (values.type_id() == arrow::Type::TIMESTAMP) {
    return static_cast<const arrow::TimestampArray&>(values).Value(row);
  }
  if (values.type_id() != arrow::Type::DATE32) {
    return arrow::Status::Invalid("AVG of ", values.type()->ToString(), " as a TIMESTAMP");
  }
  const int32_t days = static_cast<const arrow::Date32Array&>(values).Value(row);
  constexpr int32_t kInfinity = std::numeric_limits<int32_t>::max();
  if (days == kInfinity || days == -kInfinity) {
    return days > 0 ? std::numeric_limits<int64_t>::max() : -std::numeric_limits<int64_t>::max();
  }
  int64_t micros = 0;
  if (__builtin_mul_overflow(int64_t{days}, kMicrosPerDay, &micros)) {
    return arrow::Status::ExecutionError("AVG: a DATE (", days,
                                         " days) is outside the TIMESTAMP range");
  }
  return micros;
}

// DuckDB's average of `count` > 0 microseconds summing to `sum`: sum / count truncated, one more
// when twice the remainder exceeds the count. So a positive average rounds to the nearest
// microsecond (halves down) and a negative one toward zero, as in DuckDB.
inline int64_t AverageMicros(Int128 sum, int64_t count) {
  Int128 quotient = sum / count;
  if (2 * (sum % count) > count) {
    ++quotient;
  }
  return static_cast<int64_t>(quotient);  // an average of int64 values is one
}

}  // namespace antb1::exec
