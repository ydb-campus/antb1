#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <arrow/status.h>

#include "antb1/cli/cli.h"
#include "antb1/engine/session.h"

// Private to the cli module: helpers shared by cli.cc and bench.cc.

namespace antb1::cli {

// The error kind of a failed status: parse, unsupported, bind, io, execution or internal (the
// kinds of the error report and of `bench`).
std::string_view ErrorKind(const arrow::Status& status);

// A JSON string literal, escaped like `--format json` values (engine::JsonEscape): always valid
// UTF-8, whatever the bytes of an identifier, a path or a --machine name.
std::string JsonString(std::string_view s);

// ---- antb1 bench: ClickBench-format runs (docs/benchmarks.md) ----

struct BenchSettings {
  std::string queries_file;
  int tries = 3;
  bool drop_caches = false;  // before the first try of every query (else the run is lukewarm)
  std::string machine;       // empty: this host (DefaultMachine())
  std::string git_sha;       // empty: unknown (null in the JSON)
  std::string out;           // the JSON file, or "-" for stdout
};

struct BenchQuery {
  std::vector<std::optional<double>> seconds;  // one per try; all std::nullopt when it failed
  arrow::Status error;                         // why it failed (OK when it ran)
};

struct BenchReport {
  std::string date;
  std::string machine;
  std::string git_sha;
  bool cold = false;  // page cache dropped before every query's first try
  int tries = 0;
  int64_t batch_size = 0;
  double load_time = 0;
  int64_t data_size = 0;
  std::vector<BenchQuery> queries;
};

// The queries of a ClickBench query file: one per line; empty and blank lines are skipped.
std::vector<std::string> ReadBenchQueries(std::string_view text);

// "<OS> <architecture>, <n> CPUs" of this host.
std::string DefaultMachine();

// ClickBench's result JSON: system, date, machine, cluster_size, proprietary, hardware, tuned,
// tags, load_time, data_size, concurrent_qps, concurrent_error_ratio, result (one array of tries
// per query, null for every try of a query that failed) and an "antb1" object with the build, the
// cache state and the failures.
std::string ClickBenchJson(const BenchReport& report);

// Runs `antb1 bench` on a session whose tables are registered (load_time and data_size already in
// `report`): every query `tries` times, timing Session::Execute. Progress lines go to `err` (query
// numbers, timings and error kinds only: never query text or results). Returns the exit code: 0
// when every query ran or failed as out of the subset (parse, bind or unsupported), else the exit
// code of the first other failure; the JSON is written in every case.
int RunBench(engine::Session& session, const BenchSettings& settings, BenchReport report,
             const CliHooks& hooks, std::ostream& out, std::ostream& err);

}  // namespace antb1::cli
