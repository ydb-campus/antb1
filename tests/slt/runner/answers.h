#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine.h"
#include "query_file.h"
#include "result_diff.h"

// `antb1-slt answers`: stored answers checked against the DuckDB oracle. A directory of numbered
// queries (q01.sql, q02.sql, ...: one statement each, in the query_file.h format, numbered from 1
// without gaps) runs on the locked-down oracle, and each result is compared with the answer text
// of the same number in another directory (q01.csv, ...). The answers are DuckDB's own: tests that
// generate data, queries and answers at test time (the data derived from TPC-H, tests/tpch) check
// with it that the oracle reads the generated files as the generator meant them.
//
// Answer text: a header line, then one line per row, fields separated by '|', lines ended by '\n'
// (the last one's is optional). An empty field is NULL, and nothing is trimmed. A value with '|' or
// a line break cannot be written: its line has the wrong number of fields.
//
// The comparison is that of the slt records (result_diff.h) with the oracle's column classes on
// both sides: the number of columns (the header's names are never compared), then the cells, I
// and T exactly, D by value (NormalizeDecimal: the answer text may drop trailing fraction zeros),
// R within the relative tolerance of 1e-9. Rows compare in order; rows equal only in another order
// pass, and are reported as such. An empty string on the oracle's side counts as NULL, as in the
// answer text.
//
// The output is redacted unless `redact` is off: per query the number and pass, or the failure,
// the error kind, row counts, the first differing row and the sha256 of each side's block.

namespace antb1::slt {

struct AnswerQuery {
  int number = 0;           // n of q<nn>.sql
  std::string query_path;   // for messages
  std::string answer_path;  // for messages
  Statement statement;
  std::string answer;  // the answer text
};

// Reads q01.sql, q02.sql, ... of `queries_dir` and the answers of the same numbers in
// `answers_dir`. Errors name files and numbers only; with `redact` they never quote a file.
std::expected<std::vector<AnswerQuery>, std::string> LoadAnswerQueries(
    const std::filesystem::path& queries_dir, const std::filesystem::path& answers_dir,
    bool redact);

// Reads q01.sql, q02.sql, ... of `dir`: numbered from 1 without gaps, one statement each (the
// queries of `antb1-slt tpch`). Errors as LoadAnswerQueries's.
std::expected<std::vector<Statement>, std::string> LoadNumberedQueries(
    const std::filesystem::path& dir, bool redact);

struct AnswerTable {
  std::size_t columns = 0;  // fields of the header line
  std::vector<std::vector<std::optional<std::string>>> rows;
};

// Parses answer text (see above). Errors carry line numbers and counts only.
std::expected<AnswerTable, std::string> ParseAnswerText(std::string_view text);

// The one spelling of a decimal literal [-]digits[.digits] (digits on at least one side of the
// '.'): no leading zeros in the integer part ("0" if it is empty), no trailing zeros in the
// fraction, no '.' without a fraction, no "-0". "17.00" -> "17", "-.50" -> "-0.5". std::nullopt
// for any other text.
std::optional<std::string> NormalizeDecimal(std::string_view text);

struct AnswerMatch {
  bool reordered = false;  // equal only with the rows in another order
};

// Compares the oracle's result with a stored answer (see above). The discrepancy is that of the
// comparison in order; its sides are the answer (expected) and DuckDB (actual).
std::expected<AnswerMatch, Discrepancy> CompareWithAnswer(const ResultSet& oracle,
                                                          const AnswerTable& answer);

// The side labels of the reports of `answers`.
inline constexpr SideLabels kAnswerLabels{.expected = "answer", .actual = "DuckDB"};

struct AnswersOptions {
  bool redact = true;
  std::optional<uint64_t> only;  // run only Q<only>
  std::string command;  // the antb1-slt command line that prints values, without --only (repro)
};

struct AnswersStats {
  int queries = 0;
  int passed = 0;
  int reordered = 0;  // passed with the rows in another order
  int failed = 0;
};

// Runs `queries` on `oracle` and appends the report, ending with "ANSWERS: PASS|FAIL ...", to
// `out`.
AnswersStats RunAnswers(const std::vector<AnswerQuery>& queries, Engine& oracle,
                        const AnswersOptions& options, std::string& out);

}  // namespace antb1::slt
