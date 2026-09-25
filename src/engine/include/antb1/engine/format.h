#pragma once

#include <cstdint>
#include <string>

#include <arrow/array.h>
#include <arrow/result.h>

#include "antb1/engine/session.h"
#include "antb1/plan/types.h"

namespace antb1::engine {

enum class OutputFormat { kTable, kCsv, kJson };

// Canonical text of one value: integers exactly, doubles as the shortest round-trip form,
// dates as YYYY-MM-DD, VARCHAR as raw bytes, NULL as "NULL". Shared with the test oracle
// comparator.
std::string FormatValue(const arrow::Array& column, int64_t row, plan::LogicalType type);

// Renders a whole result: kTable (aligned, for humans), kCsv (RFC 4180, header row), kJson (array
// of objects; numbers as JSON numbers except HUGEINT, which is a string to keep exactness).
arrow::Result<std::string> FormatResult(const QueryResult& result, OutputFormat format);

}  // namespace antb1::engine
