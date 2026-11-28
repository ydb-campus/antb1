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

// Canonical text of one value: integers exactly, DECIMAL(p, s) with s fraction digits as DuckDB
// prints it (17.00, .500), doubles as the shortest round-trip form, dates as YYYY-MM-DD, VARCHAR
// as raw bytes, NULL as "NULL". Shared with the test oracle
// comparator.
std::string FormatValue(const arrow::Array& column, int64_t row, plan::LogicalType type);

// Renders a whole result: kTable (aligned, for humans), kCsv (RFC 4180, header row), kJson (array
// of objects; numbers as JSON numbers except HUGEINT and DECIMAL, which are strings to keep
// exactness).
arrow::Result<std::string> FormatResult(const QueryResult& result, OutputFormat format);

enum class ProfileFormat { kText, kJson };

// Renders a query profile (`antb1 explain --analyze`, docs/adr/0015-query-profiles.md). kText: the
// "Output:" line, a "Total:" line (time, rows, peak memory, threads), then one line per physical
// operator, root first, linked as EXPLAIN links a logical plan (`├─` / `└─`, with `│ ` under an
// earlier input). Each line is the operator's name, its logical node's EXPLAIN text, and in
// brackets its rows, time (and self time when its inputs ran on the same thread), runs (per part)
// and metrics. A HashBuild is drawn under its HashJoin, before the probe when the join builds on
// the left and after it when the join builds on the right. A shared producer is drawn in full
// under the first consumer in that order; later consumers keep their own metrics and no subtree.
// When sharing is dropped at runtime, the original plan replaces that shared plan wherever it sits,
// with the attempt's `shared` and `shared_fallbacks` metrics on the replacement. kJson: the same
// operators as one object, times in nanoseconds, memory in bytes, with each HashBuild left on the
// operator that prepares it. The shared producer is an input of the same first consumer. A dropped
// attempt is the original plan object, in place of the shared plan.
std::string FormatProfile(const QueryProfile& profile, ProfileFormat format);

// Renders a physical plan nobody ran (`antb1 explain --physical`, Session::ExplainPhysical): the
// "Output:" line, then one line per physical operator exactly as FormatProfile's kText writes it
// but without the brackets. What is in them -- rows, times, metrics -- is what a run measures, and
// the decisions the planner made are in the lines themselves: part pipelines, hoisted builds,
// pushed predicates (ADR 0020) and ADR 0029's key filter candidates.
std::string FormatPhysicalPlan(std::string_view output, const exec::ProfileNode& root);

// The body of a JSON string (without the quotes): quotes, backslashes and control characters
// escaped, and every byte of an ill-formed UTF-8 sequence written as the text \xHH, so the result
// is always valid UTF-8. Also used by the CLI's error object and bench report.
std::string JsonEscape(std::string_view s);

}  // namespace antb1::engine
