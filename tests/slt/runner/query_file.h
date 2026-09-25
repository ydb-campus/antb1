#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine.h"
#include "supported_features.h"

// SQL query files for `antb1-slt queries` (our own queries, e.g. tests/data/hits0_slice.sql) and
// `antb1-slt clickbench` (ClickBench's queries.sql, fetched at run time and never committed):
//   - a statement is one or more lines, the last one ending with ';', which terminates it and is
//   not
//     part of the query (a ';' that ends a line inside a string literal is not supported);
//   - between statements, blank lines and lines starting with `--` are comments; the comment
//     `-- features: <name>, <name>...` declares the SQL features (FeatureName() in
//     supported_features.h) of the next statement.

namespace antb1::slt {

struct Statement {
  int line = 0;                        // 1-based line of its first line
  std::string sql;                     // its lines joined with '\n', without the terminating ';'
  std::optional<FeatureSet> features;  // from a `-- features:` comment
};

std::expected<std::vector<Statement>, std::string> ParseSqlFile(std::string_view path,
                                                                std::string_view text);

// The feature with this FeatureName(), if any.
std::optional<Feature> FeatureByName(std::string_view name);

// `antb1-slt queries FILE`: every statement of FILE (each must declare its features) runs on antb1
// and on the DuckDB oracle.
//   - features all in `supported`: antb1 must answer, and its answer must equal DuckDB's;
//   - otherwise the query is pending: antb1 must answer Unsupported (counted), or fail with another
//     query error (parse, bind: counted as rejected, like the target-grammar samples of `diff`);
//     if it answers, the answer must equal DuckDB's and the query fails until its features are
//     declared in supported_features.h;
//   - DuckDB must answer every query (the file must be valid DuckDB SQL); an antb1 internal error
//     is always a failure.
// Projections (columns or *) compare as rowsort; a projection with LIMIT compares row counts only.
struct QueryFileOptions {
  bool redact = false;
  std::optional<uint64_t> only_line;  // run only the statement that starts on this line
  std::string command;  // the antb1-slt command line without --redact and --only (repro)
};

struct QueryFileStats {
  int queries = 0;
  int compared = 0;  // antb1 answered, equal to DuckDB
  int pending = 0;   // Unsupported answers to queries with undeclared features
  int rejected = 0;  // other query errors for queries with undeclared features
  int failed = 0;
};

QueryFileStats RunQueryFile(std::string_view path, const std::vector<Statement>& statements,
                            FeatureSet supported, Engine& antb1, Engine& oracle,
                            const QueryFileOptions& options, std::string& out);

}  // namespace antb1::slt
