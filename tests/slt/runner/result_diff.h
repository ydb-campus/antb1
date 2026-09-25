#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "canonical.h"
#include "engine.h"

// Comparison of antb1's answer with the DuckDB oracle's answer to the same query, and the failure
// report shared by `antb1-slt diff`, `queries` and `clickbench`. Answers compare like slt records
// (canonical.h): the column classes and the engine type names must be equal (e.g. an integer SUM
// is HUGEINT, as in DuckDB), I and T cells exactly, R cells with a relative tolerance of 1e-9.
//
// A redacted report (data tests) never prints SQL, values or error messages: only error kinds,
// column types, row counts, the first differing row and the sha256 of each side's canonical block.

namespace antb1::slt {

// Why antb1's answer is wrong, or why a query could not be compared.
struct Discrepancy {
  std::string what;       // one line; safe to print in redacted mode
  std::string detail;     // unredacted details (error messages)
  std::string redacted;   // details that are safe to print in redacted mode (error kinds)
  bool mismatch = false;  // the result blocks below differ
  std::string types;      // I/R/T per column (R compares with a tolerance)
  std::vector<std::string> expected;  // DuckDB's block
  std::vector<std::string> actual;    // antb1's block
  std::optional<std::size_t> first_row;
};

// "I (BIGINT)": the class letters and the engine type names of a result.
std::string ColumnTypes(const ResultSet& result);

// std::nullopt if antb1's answer equals the oracle's. `row_count_only`: compare only the number of
// rows (a projection with LIMIT, where any n rows are right).
std::optional<Discrepancy> CompareAnswers(const ResultSet& oracle, const ResultSet& antb1,
                                          SortMode sort, bool row_count_only);

// A failure caused by an engine error: `what` is the one-line summary.
Discrepancy ErrorDiscrepancy(std::string what, const EngineError& error);

// Appends the body of a failure report, each line indented by two spaces. Redacted: the error kind,
// the row counts, the first differing row and both sha256 hashes. Otherwise: the details, `sql`
// and at most 5 differing rows.
void AppendDiscrepancy(const Discrepancy& d, std::string_view sql, SortMode sort, bool redact,
                       std::string& out);

}  // namespace antb1::slt
