#pragma once

#include <iosfwd>
#include <span>
#include <string>

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

// Runs the CLI with argv (argv[0] is the program name). `in` feeds `-c -`.
int RunCli(std::span<const std::string> args, std::istream& in, std::ostream& out,
           std::ostream& err);

}  // namespace antb1::cli
