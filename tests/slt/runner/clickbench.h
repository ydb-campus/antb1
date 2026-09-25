#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine.h"
#include "query_file.h"

// `antb1-slt clickbench`: the ClickBench status ratchet (ctest data.clickbench.status, label data).
//
// Every query of ClickBench's queries.sql (fetched at run time, never committed; Q<n> is the
// 0-based statement number) runs on antb1 against the `hits` table. DuckDB runs only the queries
// antb1 answers. A query passes if antb1 answers and the answer equals DuckDB's (rowsort, the slt
// comparator). The other queries must fail cleanly: antb1 answers Unsupported (exit code 4) or a
// parse or bind error. The run fails on
//   - a wrong answer (antb1 answers, DuckDB disagrees or cannot run the query),
//   - an unclean failure (an io, execution or internal error),
//   - any difference between the passing queries and the ratchet, the committed "pass" list of
//     tests/data/clickbench_status.json: an unexpected pass or an unexpected fail. The PR that
//     changes the pass set updates the ratchet and the status table in docs/sql-subset.md.

namespace antb1::slt {

// tests/data/clickbench_status.json: {"clickbench_commit": "<sha>", "pass": [<n>, ...]}.
struct ClickBenchStatus {
  std::string commit;
  std::vector<int64_t> pass;  // sorted, unique
};

std::expected<ClickBenchStatus, std::string> ParseClickBenchStatus(std::string_view json);

struct ClickBenchOptions {
  bool redact = false;
  std::optional<uint64_t> only;  // run only Q<only>
  std::string status_path;       // for messages
  std::string command;           // the antb1-slt command line without --redact and --only (repro)
};

struct ClickBenchStats {
  int queries = 0;
  std::vector<int64_t> passed;
  int unsupported = 0;  // clean failures: Unsupported
  int rejected = 0;     // clean failures: parse or bind errors
  int failed = 0;       // wrong answers, unclean failures and ratchet differences
};

ClickBenchStats RunClickBench(const std::vector<Statement>& queries, const ClickBenchStatus& status,
                              Engine& antb1, Engine& oracle, const ClickBenchOptions& options,
                              std::string& out);

}  // namespace antb1::slt
