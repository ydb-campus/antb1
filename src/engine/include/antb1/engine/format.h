#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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

// The body of a JSON string (without the quotes): quotes, backslashes and control characters
// escaped, and every byte of an ill-formed UTF-8 sequence written as the text \xHH, so the result
// is always valid UTF-8. Also used by the CLI's error object and bench report.
std::string JsonEscape(std::string_view s);

}  // namespace antb1::engine
