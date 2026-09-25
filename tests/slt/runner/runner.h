#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "engine.h"
#include "slt_file.h"

namespace antb1::slt {

// Result corruptions for harness self-tests (--mutate): the runner must report every one as FAIL.
enum class Mutation : std::uint8_t {
  kNone,
  kValue,        // change the first value of the first row
  kNull,         // replace the first value of the first row with NULL
  kDropRow,      // drop the last row
  kExtraRow,     // duplicate the first row (or add a row of NULLs)
  kExtraColumn,  // append an integer column
  kError,        // every statement fails with an execution error
  kUnsupported,  // every statement fails as Unsupported
  kSucceed,      // failing statements succeed with an empty result
  kCanary,       // replace the first value of the first row with kCanaryValue (redaction canaries)
};

// The value kCanary writes: a redacted report must never print it.
inline constexpr std::string_view kCanaryValue = "ANTB1_CANARY_VALUE";

std::optional<Mutation> ParseMutation(std::string_view name);
inline constexpr std::string_view kMutationNames =
    "value, null, drop-row, extra-row, extra-column, error, unsupported, succeed, canary";

// Wraps `inner` (which must outlive the result) and corrupts its results.
std::unique_ptr<Engine> MakeMutatingEngine(Engine& inner, Mutation mutation);

// Replaces ${FIXTURES} in the SQL of every record with the fixtures directory.
void SubstituteVariables(SltFile& file, std::string_view fixtures_dir);

struct RunOptions {
  bool redact = false;  // never print values or SQL text (data tests; see tests/slt/README.md)
  std::string repro;    // command lines that reproduce the run, printed with every failure
};

struct RunStats {
  int records = 0;
  int passed = 0;
  int failed = 0;
  int skipped = 0;      // skipif/onlyif
  int unsupported = 0;  // antb1 Unsupported results (each one is also a failure)
  bool halted = false;
};

// Runs the records of `file` that apply to engine.name() and appends the report (failures and a
// summary line) to `out`. An antb1 Unsupported result is a failure: tests reflect current support.
RunStats RunFile(const SltFile& file, Engine& engine, const RunOptions& options, std::string& out);

struct CompleteStats {
  int queries = 0;     // expected blocks written
  int from_antb1 = 0;  // of which written by antb1 (records DuckDB does not run): review them
  int errors = 0;      // records that could not be completed (reported in `out`)
};

// Writes the expected results of every query from the oracle (records it runs) or from antb1
// (records only antb1 runs, flagged for review in `out`); statements are checked, not rewritten.
// `new_text` receives the rewritten file.
CompleteStats CompleteFile(const SltFile& file, Engine& oracle, Engine& antb1,
                           std::string& new_text, std::string& out);

}  // namespace antb1::slt
