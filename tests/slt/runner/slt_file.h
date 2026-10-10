#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "canonical.h"

// The sqllogictest subset of antb1-slt (tests/slt/README.md):
// - statement ok | statement error [regex]: SQL lines up to a blank line.
// - query <I|R|D|T...> [nosort|rowsort|valuesort] [label]: SQL lines, "----", then the expected
//   lines up to a blank line.
// - skipif <engine> | onlyif <engine> (antb1 or duckdb): conditions of the next record.
// - pending <roadmap id> (e.g. pending J4): the next statement or query waits on that roadmap PR.
//   DuckDB runs it as usual; antb1 runs it only in the pending check (CheckPendingFile in
//   runner.h), where it must still get Unsupported. It takes no skipif or onlyif.
// - halt: stop the file (with a condition: for that engine only).
// - hash-threshold <n>: hash results with more than n values (0: never).
// - # comment; "# tol <rel>" sets the R tolerance of the next query.

namespace antb1::slt {

enum class RecordKind : std::uint8_t {
  kStatementOk,
  kStatementError,
  kQuery,
  kHalt,
  kHashThreshold
};

struct Record {
  RecordKind kind = RecordKind::kStatementOk;
  int line = 0;  // 1-based line of the header (statement/query/halt/hash-threshold)
  std::vector<std::string> skipif;
  std::vector<std::string> onlyif;
  std::string pending;  // the roadmap id of `pending <id>`, or empty
  std::string sql;
  std::string error_regex;   // statement error
  std::regex error_pattern;  // error_regex compiled (ParseSlt rejects invalid regexes)
  std::string types;         // query: one of I, R, D, T per column
  SortMode sort = SortMode::kNoSort;
  std::string label;
  std::optional<double> tolerance;  // "# tol <rel>" before the record
  int64_t hash_threshold = 0;       // hash-threshold
  std::vector<std::string> expected;
  // 0-based line indexes for rewriting: [header, sql_end) holds the header and the SQL; a query's
  // block is "----" at sql_end (if has_separator) followed by the lines up to block_end.
  std::size_t header = 0;
  std::size_t sql_end = 0;
  std::size_t block_end = 0;
  bool has_separator = false;

  // Whether the record runs on the engine called `engine` (skipif/onlyif; antb1 skips a pending
  // record, which only the pending check runs on it).
  [[nodiscard]] bool RunsOn(std::string_view engine) const;
};

struct SltFile {
  std::string path;
  std::vector<std::string> lines;
  std::vector<Record> records;
};

inline constexpr std::array<std::string_view, 2> kEngineNames{"antb1", "duckdb"};

// Whether `id` names a roadmap PR as a `pending` line writes it: an upper-case letter, digits and
// at most one lower-case letter (S3, J2b, H6b).
bool IsRoadmapId(std::string_view id);

// Why `sort` cannot check a record whose columns have the classes `types`, or std::nullopt.
// valuesort loses the columns: with an R column, every value compares within the tolerance, so
// it refuses D columns next to R ones.
std::optional<std::string> SortModeProblem(std::string_view types, SortMode sort);

// Parses the text of an .slt file; the error names path:line.
std::expected<SltFile, std::string> ParseSlt(const std::string& path, std::string_view text);

struct BlockUpdate {
  std::vector<std::string> lines;        // new expected lines
  std::optional<std::string> new_types;  // replaces the types in the query header
};

// The file text with the expected blocks of some queries replaced (record index -> update).
std::string RewriteSlt(const SltFile& file, const std::map<std::size_t, BlockUpdate>& updates);

}  // namespace antb1::slt
