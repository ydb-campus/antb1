#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine.h"
#include "query_file.h"

// The status ratchet of a workload of numbered queries (ClickBench, clickbench.h; the queries
// derived from TPC-H, `antb1-slt tpch`). Every query runs on antb1; DuckDB runs only the queries
// antb1 answers. A query passes if antb1 answers and the answer equals DuckDB's (the comparison of
// the data tests: type names, DOUBLE within 1e-9, ORDER BY order with ties in any order, any row
// order otherwise). The other queries must fail cleanly: antb1 answers Unsupported (exit code 4)
// or, where the workload allows it, a parse or bind error. The run fails on
//   - a wrong answer (antb1 answers, DuckDB disagrees or cannot run the query),
//   - an unclean failure (an io, execution or internal error, and for a workload that does not
//     allow them a parse or bind error),
//   - any difference between the passing queries and the ratchet, the committed "pass" list of
//     the workload's status file: an unexpected pass or an unexpected fail. The PR that changes the
//     pass set updates the ratchet and the workload's status table in docs/sql-subset.md.

namespace antb1::slt {

// What differs between the workloads.
struct Workload {
  std::string_view summary;       // the summary line's prefix: CLICKBENCH, TPCH
  std::string_view commit_key;    // a required string key of the status file ("": none)
  int64_t first = 0;              // the number of the first query: Q0 or Q1
  bool rejections_clean = true;   // a parse or bind error is a clean failure
  std::string_view unclean_rule;  // why an unclean failure fails, after "antb1 fails with ...; "
  std::string_view table;         // the status table of docs/sql-subset.md, for messages
  bool running_lines = false;     // "Q<n>: running" before each query (a timeout names it)
};

// The status file: {["<commit_key>": "<sha>",] "pass": [<n>, ...]}.
struct RatchetStatus {
  std::string commit;         // the value of the commit key, if the workload has one
  std::vector<int64_t> pass;  // sorted, unique
};

std::expected<RatchetStatus, std::string> ParseRatchetStatus(std::string_view json,
                                                             const Workload& workload);

struct RatchetOptions {
  bool redact = false;
  std::optional<uint64_t> only;  // run only Q<only>
  std::string status_path;       // for messages
  std::string command;           // the antb1-slt command line without --redact and --only (repro)
  // Seconds since any fixed point (ANTB1_TPCH_TIMES): each outcome line gets the query's time and
  // the summary the geometric mean of the passing queries. Empty: no times.
  std::function<double()> clock;
};

struct RatchetStats {
  int queries = 0;
  std::vector<int64_t> passed;
  int unsupported = 0;  // clean failures: Unsupported
  int rejected = 0;     // clean failures: parse or bind errors (where the workload allows them)
  int failed = 0;       // wrong answers, unclean failures and ratchet differences
};

// Runs queries[i] as Q<workload.first + i> and writes the report through `write` as it goes (one
// call per line or block, so a caller can flush each).
RatchetStats RunRatchet(const std::vector<Statement>& queries, const RatchetStatus& status,
                        const Workload& workload, Engine& antb1, Engine& oracle,
                        const RatchetOptions& options,
                        const std::function<void(std::string_view)>& write);

}  // namespace antb1::slt
