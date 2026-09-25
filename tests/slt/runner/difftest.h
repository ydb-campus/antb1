#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine.h"
#include "query_gen.h"
#include "supported_features.h"

// `antb1-slt diff`: the random differential test. Generated queries (query_gen.h) run on antb1
// and on the DuckDB oracle; the results are compared with the slt comparator (canonical.h:
// canonical text, R columns with a relative tolerance of 1e-9, rowsort for projections).
//
// Per query:
//   - antb1 Unsupported: a failure if the query uses only supported features; otherwise counted
//     (per feature outside the supported set) and not a failure;
//   - another antb1 query error (parse, bind, io, execution) for a query outside the supported
//     set: counted as "rejected" and listed, not a failure (the target grammar promises
//     Unsupported there; the sql unit tests own that contract);
//   - any other antb1 error (internal errors always), a DuckDB error, different column types
//     (the engine type names, e.g. HUGEINT vs BIGINT, not only I/R/T) or different results: a
//     failure. The generator only writes SQL that DuckDB accepts, so the
//     oracle runs every query, also those antb1 does not support (a DuckDB error is a generator
//     bug).
// A failure prints the seed, the case index, the features, the SQL, at most 5 differing rows and
// the repro `ANTB1_DIFF_SEED=<seed> ANTB1_DIFF_ONLY=<index> pixi run diff-random`. With `redact`
// it prints no SQL and no values: only features, column types, row counts, the first differing
// row and the sha256 of each side's canonical block.

namespace antb1::slt {

struct DiffOptions {
  uint64_t count = 0;
  std::optional<uint64_t> only;  // run just this query index
  bool redact = false;
  std::string command;        // the antb1-slt command line without --redact and --only (repro)
  uint64_t max_reports = 10;  // failures reported in full; the rest are only counted
};

struct DiffStats {
  uint64_t queries = 0;
  uint64_t supported = 0;         // queries that use only supported features
  uint64_t target = 0;            // queries drawn from the full target grammar
  uint64_t compared = 0;          // antb1 answered and was compared with DuckDB
  uint64_t unsupported = 0;       // antb1 Unsupported answers outside the supported set
  uint64_t answered_outside = 0;  // compared queries that use features outside the supported set
  uint64_t rejected = 0;          // antb1 query errors (not Unsupported) outside the supported set
  std::vector<uint64_t> rejected_cases;  // the first few indexes, for the summary
  uint64_t failed = 0;
  // For every Unsupported answer: +1 for each of its features outside the supported set.
  std::array<uint64_t, kFeatureCount> unsupported_by_feature{};
};

// Runs the queries [0, options.count), or just options.only, and appends the report (failures and
// summary lines, ending with "DIFF: PASS|FAIL ...") to `out`.
DiffStats RunDiff(const QueryGenerator& generator, Engine& antb1, Engine& oracle,
                  const DiffOptions& options, std::string& out);

}  // namespace antb1::slt
