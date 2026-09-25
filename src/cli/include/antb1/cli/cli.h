#pragma once

#include <functional>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

#include <arrow/status.h>

namespace antb1::cli {

// Process exit codes of the `antb1` binary (docs/sql-subset.md, AGENTS.md).
enum ExitCode : int {
  kExitOk = 0,
  kExitQueryError = 1,   // parse / bind / execution error in the query
  kExitUsage = 2,        // bad command line
  kExitIo = 3,           // file missing, unreadable, not Parquet, schema mismatch
  kExitUnsupported = 4,  // valid SQL outside the supported subset
  kExitInternal = 70,    // bug: any other error
};

// Exit code for a failed query/registration status.
int ExitCodeFor(const arrow::Status& status);

// The environment RunCli touches besides its streams; tests replace it. An empty member means the
// real implementation.
struct CliHooks {
  // Seconds on a monotonic clock (query timings, `bench` load and query times).
  std::function<double()> seconds;
  // Today's date as YYYY-MM-DD in UTC (`bench`).
  std::function<std::string()> today;
  // Drops the operating system's page cache (`bench --drop-caches`): runs `sudo -n sh -c 'sync &&
  // echo 3 > /proc/sys/vm/drop_caches'` (Linux only).
  std::function<arrow::Status()> drop_caches;
};

// Runs the CLI with argv (argv[0] is the program name). `in` feeds `-c -`.
int RunCli(std::span<const std::string> args, std::istream& in, std::ostream& out,
           std::ostream& err, const CliHooks& hooks = {});

// Runs a program (looked up in PATH) with an empty environment and waits for it. IOError if it
// cannot be started, is killed or exits with a non-zero code.
arrow::Status RunCommand(const std::vector<std::string>& argv);

}  // namespace antb1::cli
