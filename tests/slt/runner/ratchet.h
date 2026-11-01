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
//   - an unclean failure (an io, execution, memory or internal error, and for a workload that does
//     not allow them a parse or bind error),
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
  // Printed first and after the summary of a run with costs (RatchetCosts): ADR 0006's disclaimer
  // for the numbers of a workload derived from a TPC benchmark ("": none).
  std::string_view disclaimer;
};

// The status file: {["<commit_key>": "<sha>",] "pass": [<n>, ...]}.
struct RatchetStatus {
  std::string commit;         // the value of the commit key, if the workload has one
  std::vector<int64_t> pass;  // sorted, unique
};

std::expected<RatchetStatus, std::string> ParseRatchetStatus(std::string_view json,
                                                             const Workload& workload);

// One of `count` disjoint slices of the queries (antb1-slt --shard): the query at index i of the
// file, from 0, is in slice i % count, so every slice holds every count-th query and the slices
// stay even in size and in how far apart their queries are. A run reports and checks its slice
// alone.
struct RatchetShard {
  int64_t index = 0;  // from 0
  int64_t count = 1;
};

// "K/N", 1 <= K <= N <= 99, as the slice of RatchetShard (K counts from 1 on the command line,
// where --shard takes it); std::nullopt for anything else.
std::optional<RatchetShard> ParseRatchetShard(std::string_view text);

// The costs of a run (antb1-slt tpch and tpcds --costs, for ADR 0037's SF 1 budget): with them,
// each outcome line gets the query's seconds (antb1, DuckDB and the comparison), antb1's own
// seconds and its peak memory where antb1 answered, the workload's disclaimer comes first and after
// the summary, and `write` gets the costs file, one JSON object per call (the caller flushes each):
//   {"record": "run", "workload", "test", "shard", "threads", "same_as_threads",
//    "memory_limit_bytes"}                                  first
//   {"record": "running", "query"}                          before each query
//   {"record": "query", "query", "outcome", "ratchet", "seconds", "antb1_seconds",
//    "peak_memory_bytes"}                                   after it
//   {"record": "end", "queries", "passed", "failed", "wall_seconds"}   last
// "outcome" is the label of the outcome line (pass, unsupported, wrong answer, DuckDB error,
// "<kind> error"), "ratchet" is ok, unexpected pass, unexpected fail or fail, and the numbers
// antb1 did not give are null. A run that ends early (a timeout) has no "end" record.
struct RatchetCosts {
  std::function<void(std::string_view)> write;
  std::string test;                     // the ctest name (--test-name)
  int threads = 1;                      // antb1's --threads
  std::optional<int> same_as_threads;   // --same-as-threads
  std::optional<int64_t> memory_limit;  // --memory-limit, in bytes
  double started = 0;                   // the clock when the run began (wall_seconds)
};

struct RatchetOptions {
  bool redact = false;
  std::optional<uint64_t> only;  // run only Q<only>
  RatchetShard shard;            // run only the queries of this slice
  std::string status_path;       // for messages
  // The antb1-slt command line of the repro lines (CommandLine in main.cc): without --redact,
  // --only (each repro line adds its own) and --costs (a repro must not overwrite the run's costs
  // file); with --show-values when the run is redacted.
  std::string command;
  // Seconds since any fixed point: each outcome line gets the query's time and the summary the
  // geometric mean of the passing queries. Empty: no times. Set whenever `costs` is.
  std::function<double()> clock;
  std::optional<RatchetCosts> costs;
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
