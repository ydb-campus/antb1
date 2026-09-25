#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <arrow/result.h>
#include <arrow/type_fwd.h>

namespace antb1::plan {

// Engine-level (SQL) types; see docs/sql-subset.md for the mapping from Parquet/Arrow.
enum class LogicalType : std::uint8_t {
  kSmallInt,   // int16
  kInteger,    // int32
  kBigInt,     // int64
  kUSmallInt,  // uint16 (ClickBench EventDate before the DATE override)
  kHugeInt,    // 128-bit integer result of integer SUM: arrow decimal128(38, 0)
  kDouble,     // float64 (float32 columns are widened)
  kVarchar,    // bytes; compared byte-wise (unannotated Parquet BYTE_ARRAY included)
  kDate,       // date32
};

std::string_view ToString(LogicalType type);

// The Arrow type the engine uses for a logical type (VARCHAR is always arrow::binary()).
std::shared_ptr<arrow::DataType> ToArrow(LogicalType type);

// Maps a storage type to a logical type; NotImplemented (with a clear message) for unsupported
// types.
arrow::Result<LogicalType> FromArrow(const arrow::DataType& type);

bool IsInteger(LogicalType type);
bool IsNumeric(LogicalType type);

}  // namespace antb1::plan
