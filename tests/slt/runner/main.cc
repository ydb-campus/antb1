// antb1-slt: the sqllogictest runner of the test harness (tests/slt/README.md).
//
//   antb1-slt run --engine antb1|duckdb --fixtures DIR --tables FILE [--redact] [--mutate KIND]
//                 FILE...
//   antb1-slt complete --fixtures DIR --tables FILE FILE...
//   antb1-slt pending --fixtures DIR --tables FILE [--redact] [--mutate KIND] FILE...
//   antb1-slt diff --fixtures DIR --tables FILE --seed S --count N [--only I] [--table NAME]...
//                  [--redact] [--mutate KIND] [--target-percent P]
//   antb1-slt queries --fixtures DIR --tables FILE [--redact] [--only LINE] [--mutate KIND] FILE
//   antb1-slt clickbench --fixtures DIR --tables FILE --status FILE [--redact] [--only N]
//                        [--mutate KIND] QUERIES
//   antb1-slt answers --fixtures DIR --tables FILE --queries DIR --answers DIR [--only N]
//                     [--mutate KIND]
//   antb1-slt tpch --fixtures DIR --tables FILE --queries DIR --status FILE [--memory-limit SIZE]
//                  [--oracle-memory-limit SIZE] [--only N] [--shard K/N] [--costs FILE]
//                  [--mutate KIND]
//   antb1-slt tpcds ... (the options of tpch)
//   antb1-slt version
//
// `pending` is the pending check: antb1 must still answer every `pending <roadmap id>` record of
// FILEs with Unsupported (runner.h). `queries` and `clickbench` run the ClickBench data tests
// (tests/data): see query_file.h and clickbench.h. `answers` checks stored answers against the
// DuckDB oracle (answers.h). `tpch` is the ratchet of the queries derived from TPC-H (ratchet.h,
// tests/tpch): q01.sql, q02.sql, ... of DIR as Q1, Q2, ...; --costs adds each query's seconds and
// antb1's peak memory, and writes them to FILE (RatchetCosts); --oracle-memory-limit caps DuckDB's
// memory, beyond which it spills to the temp dir. `tpcds` is the same for the queries derived from
// TPC-DS (tests/tpcds).
//
// Redaction: --redact, a table with the `redact` option (tables.h) and `answers` print no values
// and no SQL. --show-values lifts the last two for a local repro; it is refused when
// GITHUB_ACTIONS=true, as CI logs are public.
//
// Exit codes: 0 every record (query) passed, 1 failures, 2 usage or .slt syntax error, 3 setup
// error (tables, fixtures, engine), 70 internal error.

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>

#include "antb1/engine/session.h"

#include "answers.h"
#include "antb1_engine.h"
#include "clickbench.h"
#include "difftest.h"
#include "engine.h"
#include "query_file.h"
#include "query_gen.h"
#include "ratchet.h"
#include "runner.h"
#include "slt_file.h"
#include "supported_features.h"
#include "tables.h"

#ifdef ANTB1_SLT_HAVE_DUCKDB
#include "duckdb_engine.h"
#endif

namespace antb1::slt {
namespace {

namespace fs = std::filesystem;

constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitSetup = 3;

struct Args {
  std::string engine = "antb1";
  std::string fixtures;
  std::string tables;
  std::string temp_dir;
  std::string test_name;
  std::string mutate = "none";
  bool redact = false;
  bool show_values = false;
  // antb1 engine::SessionOptions; --same-as-threads also runs every query with that many threads
  // and requires identical results.
  int threads = 1;
  int64_t batch_size = engine::SessionOptions{}.batch_size;
  std::optional<int> same_as_threads;
  std::vector<std::string> files;
  // diff
  uint64_t seed = 0;
  uint64_t count = 0;
  std::optional<uint64_t> only;
  std::vector<std::string> only_tables;
  unsigned target_percent = 25;
  bool list = false;
  // queries, clickbench
  std::string query_file;
  std::string status;
  // answers, tpch, tpcds
  std::string queries_dir;
  std::string answers_dir;
  // tpch, tpcds: the antb1 sessions' memory limit (engine::SessionOptions::memory_limit) and the
  // DuckDB oracle's (DuckDbEngine::Make)
  std::string memory_limit;
  std::string oracle_memory_limit;
  std::string shard;  // tpch, tpcds: "K/N", the K-th of N slices of the queries (RatchetShard)
  std::string costs;  // tpch, tpcds: the costs file (RatchetCosts), or empty
};

std::string ShellQuote(std::string_view arg) {
  const bool plain = !arg.empty() && arg.find_first_not_of(
                                         "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                                         "0123456789_./=:,+-@%") == std::string_view::npos;
  if (plain) {
    return std::string(arg);
  }
  std::string out = "'";
  for (const char c : arg) {
    out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  }
  return out + "'";
}

// The command line of this run without --redact, --show-values, --only (repro lines add their
// own --only) and --costs (a repro must not overwrite the run's costs file), with --show-values
// when the run is redacted without --redact (`show_values`).
std::string CommandLine(std::span<char*> argv, bool show_values) {
  std::string command;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--redact" || arg == "--show-values" || arg.starts_with("--only=") ||
        arg.starts_with("--costs=")) {
      continue;
    }
    if (arg == "--only" || arg == "--costs") {
      ++i;
      continue;
    }
    command += (command.empty() ? "" : " ") + ShellQuote(arg);
  }
  return show_values ? command + " --show-values" : command;
}

// What antb1-slt reads from main's environment `envp` (std::getenv is not thread-safe).
struct Environment {
  bool github_actions = false;  // GITHUB_ACTIONS=true
};

Environment ReadEnvironment(char* const* envp) {
  Environment env;
  for (char* const* entry = envp; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view variable(*entry);
    env.github_actions = env.github_actions || variable == "GITHUB_ACTIONS=true";
  }
  return env;
}

// A memory size in bytes: digits with an optional KiB, MiB or GiB suffix; std::nullopt for
// anything else (an absolute size, so the run does not depend on the host's memory).
std::optional<int64_t> ParseByteSize(std::string_view text) {
  int64_t unit = 1;
  for (const auto& [suffix, bytes] :
       {std::pair{std::string_view("KiB"), int64_t{1024}},
        std::pair{std::string_view("MiB"), int64_t{1024} * 1024},
        std::pair{std::string_view("GiB"), int64_t{1024} * 1024 * 1024}}) {
    if (text.ends_with(suffix)) {
      text.remove_suffix(suffix.size());
      unit = bytes;
      break;
    }
  }
  int64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  int64_t bytes = 0;
  if (error != std::errc{} || end != text.data() + text.size() || value < 1 ||
      __builtin_mul_overflow(value, unit, &bytes)) {
    return std::nullopt;
  }
  return bytes;
}

// The tables file of `pixi run diff-random` (scripts/diff-random.sh).
bool IsDiffRandomTables(const std::string& tables) {
  const std::string path = fs::path(tables).lexically_normal().generic_string();
  return path == "tests/slt/tables.txt" || path.ends_with("/tests/slt/tables.txt");
}

// The command line of the run (CommandLine), plus the ctest invocation when known.
std::string Repro(const std::string& command_line, const Args& args) {
  std::string command = "    " + command_line + "\n";
  if (!args.test_name.empty()) {
    std::string regex;
    for (const char c : args.test_name) {
      regex += c == '.' ? std::string("\\.") : std::string(1, c);
    }
    command += std::format("    pixi run test -R '^{}$' --output-on-failure\n", regex);
  }
  return command;
}

std::expected<std::string, std::string> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(std::format("antb1-slt: cannot read '{}'", path));
  }
  return std::string(std::istreambuf_iterator<char>(in), {});
}

std::expected<void, std::string> WriteFile(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) {
      return std::unexpected(std::format("antb1-slt: cannot write '{}'", tmp));
    }
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    return std::unexpected(std::format("antb1-slt: cannot replace '{}': {}", path, ec.message()));
  }
  return {};
}

std::expected<std::unique_ptr<Engine>, std::string> MakeEngine(std::string_view name,
                                                               const std::vector<TableDef>& tables,
                                                               const Args& args) {
  if (name == "antb1") {
    engine::SessionOptions options;
    options.threads = args.threads;
    options.batch_size = args.batch_size;
    if (!args.memory_limit.empty()) {
      options.memory_limit = ParseByteSize(args.memory_limit);
    }
    auto engine = Antb1Engine::Make(tables, options);
    if (!engine) {
      return std::unexpected(engine.error());
    }
    if (!args.same_as_threads.has_value()) {
      return std::unique_ptr<Engine>(std::move(*engine));
    }
    options.threads = *args.same_as_threads;
    auto reference = Antb1Engine::Make(tables, options);
    if (!reference) {
      return std::unexpected(reference.error());
    }
    return MakeSameResultEngine(std::move(*engine), std::move(*reference),
                                std::format("{} thread(s)", *args.same_as_threads));
  }
#ifdef ANTB1_SLT_HAVE_DUCKDB
  auto engine = DuckDbEngine::Make(
      tables, args.fixtures, args.temp_dir,
      args.oracle_memory_limit.empty() ? std::nullopt : ParseByteSize(args.oracle_memory_limit));
  if (!engine) {
    return std::unexpected(engine.error());
  }
  return std::unique_ptr<Engine>(std::move(*engine));
#else
  (void)args;
  return std::unexpected(
      "antb1-slt was built without DuckDB (configure with ANTB1_WITH_DUCKDB=ON)");
#endif
}

struct Loaded {
  std::string text;
  SltFile file;
};

std::expected<Loaded, std::string> Load(const std::string& path, const Args& args) {
  auto text = ReadFile(path);
  if (!text) {
    return std::unexpected(text.error());
  }
  auto file = ParseSlt(path, *text);
  if (!file) {
    return std::unexpected(file.error());
  }
  SubstituteVariables(*file, args.fixtures);
  return Loaded{.text = std::move(*text), .file = std::move(*file)};
}

int Run(const Args& args, const std::vector<TableDef>& tables, const std::string& command) {
  const auto mutation = ParseMutation(args.mutate);
  if (!mutation.has_value()) {
    std::println(stderr, "antb1-slt: unknown --mutate kind '{}' (known: {})", args.mutate,
                 kMutationNames);
    return kExitUsage;
  }
  if (*mutation != Mutation::kNone && args.engine != "antb1") {
    std::println(stderr, "antb1-slt: --mutate corrupts antb1 results; use it with --engine antb1");
    return kExitUsage;
  }
  const RunOptions options{.redact = args.redact, .repro = Repro(command, args)};
  RunStats total;
  for (const auto& path : args.files) {
    auto loaded = Load(path, args);
    if (!loaded) {
      std::println(stderr, "{}", loaded.error());
      return kExitUsage;
    }
    auto engine = MakeEngine(args.engine, tables, args);
    if (!engine) {
      std::println(stderr, "antb1-slt: {}", engine.error());
      return kExitSetup;
    }
    std::unique_ptr<Engine> mutating;
    if (*mutation != Mutation::kNone) {
      mutating = MakeMutatingEngine(**engine, *mutation);
    }
    std::string out;
    const RunStats stats = RunFile(loaded->file, mutating ? *mutating : **engine, options, out);
    std::print("{}", out);
    total.records += stats.records;
    total.failed += stats.failed;
    total.skipped += stats.skipped;
    total.unsupported += stats.unsupported;
  }
  std::println("SLT: {} engine={} files={} records={} failed={} skipped={} unsupported={}",
               total.failed == 0 ? "PASS" : "FAIL", args.engine, args.files.size(), total.records,
               total.failed, total.skipped, total.unsupported);
  return total.failed == 0 ? 0 : kExitFailed;
}

int Complete(const Args& args, const std::vector<TableDef>& tables) {
  int errors = 0;
  for (const auto& path : args.files) {
    auto loaded = Load(path, args);
    if (!loaded) {
      std::println(stderr, "{}", loaded.error());
      return kExitUsage;
    }
    auto oracle = MakeEngine("duckdb", tables, args);
    auto antb1 = MakeEngine("antb1", tables, args);
    if (!oracle || !antb1) {
      std::println(stderr, "antb1-slt: {}", !oracle ? oracle.error() : antb1.error());
      return kExitSetup;
    }
    std::string text;
    std::string out;
    const CompleteStats stats = CompleteFile(loaded->file, **oracle, **antb1, text, out);
    std::print("{}", out);
    const bool changed = text != loaded->text;
    if (changed) {
      if (auto written = WriteFile(path, text); !written) {
        std::println(stderr, "{}", written.error());
        return kExitSetup;
      }
    }
    std::println("antb1-slt: completed {}: {} queries ({} from antb1), {}", path, stats.queries,
                 stats.from_antb1, changed ? "rewritten" : "unchanged");
    errors += stats.errors;
  }
  if (errors > 0) {
    std::println("antb1-slt: {} record(s) could not be completed (see ERROR lines)", errors);
    return kExitFailed;
  }
  return 0;
}

void AddSetup(CLI::App* cmd, Args& args) {
  cmd->add_option("--fixtures", args.fixtures, "Fixtures directory (antb1-fixturegen output)")
      ->required();
  cmd->add_option("--tables", args.tables, "Table definitions (tests/slt/tables.txt)")->required();
  cmd->add_option("--temp-dir", args.temp_dir,
                  "DuckDB temp directory, and tpch and tpcds's local refusal report (default: "
                  "<fixtures>/../slt-tmp)");
  cmd->add_option("--test-name", args.test_name, "ctest name, for the repro line");
  cmd->add_option("--threads", args.threads, "antb1: threads of the engine (default 1)")
      ->check(CLI::Range(1, engine::Session::kMaxThreads));
  cmd->add_option("--batch-size", args.batch_size, "antb1: rows per batch (default 65536)")
      ->check(CLI::Range(int64_t{1}, int64_t{1'073'741'824}));
  cmd->add_option("--same-as-threads", args.same_as_threads,
                  "antb1: also run every query with this many threads; results must be identical")
      ->check(CLI::Range(1, engine::Session::kMaxThreads));
  cmd->add_flag("--show-values", args.show_values,
                "Print values and SQL also for tables marked `redact` and for `answers` (local "
                "repro only; refused when GITHUB_ACTIONS=true)");
}

void AddCommon(CLI::App* cmd, Args& args) {
  AddSetup(cmd, args);
  cmd->add_option("files", args.files, ".slt files")->required();
}

int Diff(const Args& args, std::vector<TableDef> tables, const std::string& command) {
  const auto mutation = ParseMutation(args.mutate);
  if (!mutation.has_value()) {
    std::println(stderr, "antb1-slt: unknown --mutate kind '{}' (known: {})", args.mutate,
                 kMutationNames);
    return kExitUsage;
  }
  if (!args.only_tables.empty()) {
    std::vector<TableDef> selected;
    for (const auto& name : args.only_tables) {
      const auto it = std::ranges::find(tables, name, &TableDef::name);
      if (it == tables.end()) {
        std::println(stderr, "antb1-slt: --table {}: no such table in {}", name, args.tables);
        return kExitUsage;
      }
      selected.push_back(*it);
    }
    tables = std::move(selected);
  }
  // The seed goes out first: it must be known even if the run crashes.
  std::println("antb1-slt diff: seed={} count={} target-percent={} supported features: {}",
               args.seed, args.only.has_value() ? 1 : args.count, args.target_percent,
               kSupportedFeatures.Names());
  std::fflush(stdout);
  auto gen_tables = LoadGenTables(tables);
  if (!gen_tables) {
    std::println(stderr, "antb1-slt: {}", gen_tables.error());
    return kExitSetup;
  }
  auto generator = QueryGenerator::Make(
      std::move(*gen_tables), args.seed,
      GeneratorOptions{.supported = kSupportedFeatures, .target_percent = args.target_percent});
  if (!generator) {
    std::println(stderr, "antb1-slt: {}", generator.error());
    return kExitSetup;
  }
  if (args.list) {
    const uint64_t first = args.only.value_or(0);
    const uint64_t last = args.only.has_value() ? first + 1 : args.count;
    for (uint64_t i = first; i < last; ++i) {
      const GeneratedQuery q = generator->Generate(i);
      std::println("{}\t{}\t{}", i, q.features.Names(), q.sql);
    }
    return 0;
  }
  auto antb1 = MakeEngine("antb1", tables, args);
  auto oracle = MakeEngine("duckdb", tables, args);
  if (!antb1 || !oracle) {
    std::println(stderr, "antb1-slt: {}", !antb1 ? antb1.error() : oracle.error());
    return kExitSetup;
  }
  std::unique_ptr<Engine> mutating;
  if (*mutation != Mutation::kNone) {
    mutating = MakeMutatingEngine(**antb1, *mutation);
  }
  std::string pixi_args;
  for (const auto& name : args.only_tables) {
    pixi_args += " --table " + name;
  }
  const DiffOptions options{.count = args.count,
                            .only = args.only,
                            .redact = args.redact,
                            .pixi_repro = IsDiffRandomTables(args.tables),
                            .pixi_args = pixi_args,
                            .command = command};
  std::string out;
  const DiffStats stats =
      RunDiff(*generator, mutating ? *mutating : **antb1, **oracle, options, out);
  std::print("{}", out);
  return stats.failed == 0 ? 0 : kExitFailed;
}

// The antb1 engine and, with --mutate, the corrupting wrapper the run must use instead.
struct Antb1Under {
  std::unique_ptr<Engine> engine;
  std::unique_ptr<Engine> mutating;
  [[nodiscard]] Engine& get() const { return mutating ? *mutating : *engine; }
};

std::expected<Antb1Under, std::string> MakeAntb1(const std::vector<TableDef>& tables,
                                                 const Args& args) {
  const auto mutation = ParseMutation(args.mutate);
  if (!mutation.has_value()) {
    return std::unexpected(
        std::format("unknown --mutate kind '{}' (known: {})", args.mutate, kMutationNames));
  }
  auto engine = MakeEngine("antb1", tables, args);
  if (!engine) {
    return std::unexpected(engine.error());
  }
  Antb1Under out{.engine = std::move(*engine), .mutating = nullptr};
  if (*mutation != Mutation::kNone) {
    out.mutating = MakeMutatingEngine(*out.engine, *mutation);
  }
  return out;
}

// The pending check (CheckPendingFile): the `pending <roadmap id>` records of every file on antb1.
int Pending(const Args& args, const std::vector<TableDef>& tables, const std::string& command) {
  if (!ParseMutation(args.mutate).has_value()) {
    std::println(stderr, "antb1-slt: unknown --mutate kind '{}' (known: {})", args.mutate,
                 kMutationNames);
    return kExitUsage;
  }
  const RunOptions options{.redact = args.redact, .repro = Repro(command, args)};
  PendingStats total;
  int with_pending = 0;
  for (const auto& path : args.files) {
    auto loaded = Load(path, args);
    if (!loaded) {
      std::println(stderr, "{}", loaded.error());
      return kExitUsage;
    }
    if (std::ranges::none_of(loaded->file.records,
                             [](const Record& r) { return !r.pending.empty(); })) {
      continue;
    }
    ++with_pending;
    auto antb1 = MakeAntb1(tables, args);
    if (!antb1) {
      std::println(stderr, "antb1-slt: {}", antb1.error());
      return kExitSetup;
    }
    std::string out;
    const PendingStats stats = CheckPendingFile(loaded->file, antb1->get(), options, out);
    std::print("{}", out);
    total.records += stats.records;
    total.unsupported += stats.unsupported;
    total.failed += stats.failed;
    for (const auto& [id, count] : stats.by_id) {
      total.by_id[id] += count;
    }
  }
  std::string ids;
  for (const auto& [id, count] : total.by_id) {
    ids += std::format(" {}={}", id, count);
  }
  std::println("PENDING: {} files={} with_pending={} records={} unsupported={} failed={}{}",
               total.failed == 0 ? "PASS" : "FAIL", args.files.size(), with_pending, total.records,
               total.unsupported, total.failed, ids.empty() ? "" : " by id:" + ids);
  return total.failed == 0 ? 0 : kExitFailed;
}

std::expected<std::vector<Statement>, std::string> LoadStatements(const std::string& path) {
  auto text = ReadFile(path);
  if (!text) {
    return std::unexpected(text.error());
  }
  auto statements = ParseSqlFile(path, *text);
  if (!statements) {
    return std::unexpected("antb1-slt: " + statements.error());
  }
  return statements;
}

int Queries(const Args& args, const std::vector<TableDef>& tables, const std::string& command) {
  auto statements = LoadStatements(args.query_file);
  if (!statements) {
    std::println(stderr, "{}", statements.error());
    return kExitUsage;
  }
  for (const auto& s : *statements) {
    if (!s.features.has_value()) {
      std::println(stderr, "antb1-slt: {}:{}: no `-- features:` line before the query",
                   args.query_file, s.line);
      return kExitUsage;
    }
  }
  auto antb1 = MakeAntb1(tables, args);
  auto oracle = MakeEngine("duckdb", tables, args);
  if (!antb1 || !oracle) {
    std::println(stderr, "antb1-slt: {}", !antb1 ? antb1.error() : oracle.error());
    return kExitSetup;
  }
  const QueryFileOptions options{.redact = args.redact, .only_line = args.only, .command = command};
  std::string out;
  const QueryFileStats stats = RunQueryFile(args.query_file, *statements, kSupportedFeatures,
                                            antb1->get(), **oracle, options, out);
  std::print("{}", out);
  return stats.failed == 0 ? 0 : kExitFailed;
}

int ClickBench(const Args& args, const std::vector<TableDef>& tables, const std::string& command) {
  auto queries = LoadStatements(args.query_file);
  if (!queries) {
    std::println(stderr, "{}", queries.error());
    return kExitSetup;
  }
  auto status_text = ReadFile(args.status);
  if (!status_text) {
    std::println(stderr, "{}", status_text.error());
    return kExitSetup;
  }
  auto status = ParseClickBenchStatus(*status_text);
  if (!status) {
    std::println(stderr, "antb1-slt: {}: {}", args.status, status.error());
    return kExitUsage;
  }
  auto antb1 = MakeAntb1(tables, args);
  auto oracle = MakeEngine("duckdb", tables, args);
  if (!antb1 || !oracle) {
    std::println(stderr, "antb1-slt: {}", !antb1 ? antb1.error() : oracle.error());
    return kExitSetup;
  }
  const ClickBenchOptions options{
      .redact = args.redact, .only = args.only, .status_path = args.status, .command = command};
  std::string out;
  const ClickBenchStats stats =
      RunClickBench(*queries, *status, antb1->get(), **oracle, options, out);
  std::print("{}", out);
  return stats.failed == 0 ? 0 : kExitFailed;
}

// The workload of the queries derived from TPC-H: Q1 to Q22, and only Unsupported is a clean
// failure (a parse or bind error means the engine misreads valid SQL).
constexpr Workload kTpch{
    .summary = "TPCH",
    .commit_key = "",
    .first = 1,
    .rejections_clean = false,
    .unclean_rule =
        "a query derived from TPC-H that antb1 does not answer must fail with "
        "Unsupported (exit code 4)",
    .table = "the table of the queries derived from TPC-H",
    .running_lines = true,
    .disclaimer =
        "This workload is derived from the TPC-H Benchmark and is not comparable to published "
        "TPC-H "
        "Benchmark results, as this implementation does not comply with all requirements of the "
        "TPC-H Benchmark.",
};

// The workload of the queries derived from TPC-DS: Q1 to Q99, with the same rule.
constexpr Workload kTpcds{
    .summary = "TPCDS",
    .commit_key = "",
    .first = 1,
    .rejections_clean = false,
    .unclean_rule =
        "a query derived from TPC-DS that antb1 does not answer must fail with "
        "Unsupported (exit code 4)",
    .table = "the table of the queries derived from TPC-DS",
    .running_lines = true,
    .disclaimer =
        "This workload is derived from the TPC-DS Benchmark and is not comparable to published "
        "TPC-DS Benchmark results, as this implementation does not comply with all requirements of "
        "the TPC-DS Benchmark.",
};

// The ratchet of a numbered workload (tpch, tpcds): q01.sql, ... of --queries against --status.
// Seconds of the steady clock since any fixed point.
double SteadySeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int NumberedWorkload(const Args& args, const std::vector<TableDef>& tables,
                     const std::string& command, const Workload& workload, bool github_actions) {
  const double started = SteadySeconds();  // the costs' wall_seconds include the setup
  auto queries = LoadNumberedQueries(args.queries_dir, args.redact);
  if (!queries) {
    std::println(stderr, "antb1-slt: {}", queries.error());
    return kExitSetup;
  }
  auto status_text = ReadFile(args.status);
  if (!status_text) {
    std::println(stderr, "{}", status_text.error());
    return kExitSetup;
  }
  auto status = ParseRatchetStatus(*status_text, workload);
  if (!status) {
    std::println(stderr, "antb1-slt: {}: {}", args.status, status.error());
    return kExitUsage;
  }
  auto antb1 = MakeAntb1(tables, args);
  auto oracle = MakeEngine("duckdb", tables, args);
  if (!antb1 || !oracle) {
    std::println(stderr, "antb1-slt: {}", !antb1 ? antb1.error() : oracle.error());
    return kExitSetup;
  }
  RatchetOptions options{.redact = args.redact,
                         .only = args.only,
                         .shard = {},
                         .status_path = args.status,
                         .command = command,
                         .clock = {}};
  if (!args.shard.empty()) {
    const std::optional<RatchetShard> shard = ParseRatchetShard(args.shard);
    if (!shard.has_value()) {
      std::println(stderr, "antb1-slt: --shard expects K/N with 1 <= K <= N <= 99, got '{}'",
                   args.shard);
      return kExitUsage;
    }
    options.shard = *shard;
  }
  // The costs file is truncated first and every record flushed, so a run that times out keeps the
  // queries it finished and names the one it was running.
  std::ofstream costs_file;
  if (!args.costs.empty()) {
    const fs::path path(args.costs);
    std::error_code ec;
    if (path.has_parent_path()) {
      fs::create_directories(path.parent_path(), ec);
    }
    costs_file.open(path, std::ios::out | std::ios::trunc);
    if (!costs_file) {
      std::println(stderr, "antb1-slt: cannot write the costs file {}", args.costs);
      return kExitSetup;
    }
    options.clock = SteadySeconds;
    options.costs = RatchetCosts{
        .write =
            [&costs_file](std::string_view line) {
              costs_file << line;
              costs_file.flush();
            },
        .test = args.test_name,
        .threads = args.threads,
        .same_as_threads = args.same_as_threads,
        .memory_limit = args.memory_limit.empty() ? std::nullopt : ParseByteSize(args.memory_limit),
        .started = started};
  }
  // Why antb1 refuses each query it refuses, a local report (PlanRefusalReport): the log names the
  // file, never what it holds.
  std::error_code ec;
  const RefusalReport report = PlanRefusalReport(github_actions, args.only.has_value(),
                                                 fs::absolute(args.temp_dir, ec), args.fixtures);
  std::ofstream refusals_file;
  std::string refusals_note = report.skipped;
  if (report.skipped.empty()) {
    fs::create_directories(report.file.parent_path(), ec);
    refusals_file.open(report.file, std::ios::out | std::ios::trunc);
    if (!refusals_file) {
      std::println(stderr, "antb1-slt: cannot write the refusals file {}", report.file.string());
      return kExitSetup;
    }
    refusals_file << std::format(
        "# Why antb1 refuses the queries derived from {}: its messages quote the query text. Local "
        "only: never commit, paste or upload them (ADR 0006).\n",
        workload.summary == "TPCDS" ? "TPC-DS" : "TPC-H");
    options.refusals = [&refusals_file](std::string_view line) {
      refusals_file << line;
      refusals_file.flush();
    };
    refusals_note = report.file.string();
  }
  options.show_refusals = !args.redact;  // --show-values: a local repro
  // Each line as it comes, so the log of a run that times out ends with the query it was running.
  const RatchetStats stats = RunRatchet(*queries, *status, workload, antb1->get(), **oracle,
                                        options, [](std::string_view text) {
                                          std::print("{}", text);
                                          std::fflush(stdout);
                                        });
  std::println("refusals: {}", refusals_note);
  return stats.failed == 0 ? 0 : kExitFailed;
}

int Answers(const Args& args, const std::vector<TableDef>& tables, const std::string& command) {
  const auto mutation = ParseMutation(args.mutate);
  if (!mutation.has_value()) {
    std::println(stderr, "antb1-slt: unknown --mutate kind '{}' (known: {})", args.mutate,
                 kMutationNames);
    return kExitUsage;
  }
  auto queries = LoadAnswerQueries(args.queries_dir, args.answers_dir, args.redact);
  if (!queries) {
    std::println(stderr, "antb1-slt: {}", queries.error());
    return kExitSetup;
  }
  auto oracle = MakeEngine("duckdb", tables, args);
  if (!oracle) {
    std::println(stderr, "antb1-slt: {}", oracle.error());
    return kExitSetup;
  }
  std::unique_ptr<Engine> mutating;
  if (*mutation != Mutation::kNone) {
    mutating = MakeMutatingEngine(**oracle, *mutation);
  }
  const AnswersOptions options{.redact = args.redact, .only = args.only, .command = command};
  std::string out;
  const AnswersStats stats = RunAnswers(*queries, mutating ? *mutating : **oracle, options, out);
  std::print("{}", out);
  return stats.failed == 0 ? 0 : kExitFailed;
}

int Main(std::span<char*> argv, const Environment& env) {
  const bool github_actions = env.github_actions;
  CLI::App app{"antb1-slt: sqllogictest runner (antb1 engine, DuckDB oracle)", "antb1-slt"};
  app.require_subcommand(1);
  Args args;
  auto* run = app.add_subcommand("run", "Check the expected results of FILEs on one engine");
  AddCommon(run, args);
  run->add_option("--engine", args.engine, "Engine to run")
      ->check(CLI::IsMember({"antb1", "duckdb"}));
  run->add_flag("--redact", args.redact,
                "Never print values or SQL (only ids, types, counts, sha256)");
  run->add_option("--mutate", args.mutate, "Corrupt antb1 results (harness self-tests)");
  auto* complete =
      app.add_subcommand("complete", "Rewrite the expected results of FILEs from DuckDB");
  AddCommon(complete, args);
  auto* pending = app.add_subcommand(
      "pending", "Check that antb1 still answers the `pending` records of FILEs with Unsupported");
  AddCommon(pending, args);
  pending->add_flag("--redact", args.redact,
                    "Never print values or SQL (only ids, error kinds, row counts)");
  pending->add_option("--mutate", args.mutate, "Corrupt antb1 results (harness self-tests)");
  auto* diff = app.add_subcommand(
      "diff", "Random differential test: generated queries on antb1 vs the DuckDB oracle");
  AddSetup(diff, args);
  diff->add_option("--seed", args.seed, "Seed of the query generator")->required();
  diff->add_option("--count", args.count, "Number of queries (indexes 0..count-1)")->required();
  diff->add_option("--only", args.only, "Run only the query with this index (repro)");
  diff->add_option("--table", args.only_tables, "Generate queries over this table only")
      ->type_name("NAME");
  diff->add_option("--target-percent", args.target_percent,
                   "Share of queries drawn from the full target grammar (default 25)")
      ->check(CLI::Range(0U, 100U));
  diff->add_flag("--redact", args.redact,
                 "Never print values or SQL (only ids, features, types, counts, sha256)");
  diff->add_option("--mutate", args.mutate, "Corrupt antb1 results (harness self-tests)");
  diff->add_flag("--list", args.list,
                 "Print the generated queries (index, features, SQL); run nothing");
  auto* queries = app.add_subcommand(
      "queries", "Run the queries of FILE on antb1 and DuckDB and compare them (data tests)");
  AddSetup(queries, args);
  queries->add_option("file", args.query_file, "Query file (a `-- features:` line per query)")
      ->required();
  queries->add_flag("--redact", args.redact,
                    "Never print values or SQL (only ids, features, types, counts, sha256)");
  queries->add_option("--only", args.only, "Run only the query that starts on this line (repro)");
  queries->add_option("--mutate", args.mutate, "Corrupt antb1 results (harness self-tests)");
  auto* clickbench = app.add_subcommand(
      "clickbench", "ClickBench status: run QUERIES on antb1, compare with DuckDB and the ratchet");
  AddSetup(clickbench, args);
  clickbench->add_option("--status", args.status, "The ratchet (tests/data/clickbench_status.json)")
      ->required();
  clickbench->add_option("queries", args.query_file, "ClickBench's queries.sql")->required();
  clickbench->add_flag(
      "--redact", args.redact,
      "Never print values or query text (only query numbers, types, counts, sha256)");
  clickbench->add_option("--only", args.only, "Run only Q<n> (repro)");
  clickbench->add_option("--mutate", args.mutate, "Corrupt antb1 results (harness self-tests)");
  auto* answers = app.add_subcommand(
      "answers", "Check stored answers (qNN.csv) against DuckDB's results to the queries qNN.sql");
  AddSetup(answers, args);
  answers->add_option("--queries", args.queries_dir, "Directory of q01.sql, q02.sql, ...")
      ->required();
  answers->add_option("--answers", args.answers_dir, "Directory of q01.csv, q02.csv, ...")
      ->required();
  answers->add_option("--only", args.only, "Run only Q<n> (repro)");
  answers->add_option("--mutate", args.mutate, "Corrupt DuckDB's results (harness self-tests)");
  // tpch and tpcds: the ratchets of the numbered workloads, with the same options.
  const auto add_numbered = [&app, &args](const std::string& name, const std::string& workload,
                                          const std::string& status_file) {
    auto* cmd = app.add_subcommand(name, "Ratchet of the queries derived from " + workload +
                                             ": q01.sql, ... on antb1 vs DuckDB");
    AddSetup(cmd, args);
    cmd->add_option("--queries", args.queries_dir, "Directory of q01.sql, q02.sql, ...")
        ->required();
    cmd->add_option("--status", args.status, "The ratchet (" + status_file + ")")->required();
    const auto size = [](const std::string& value) -> std::string {
      return ParseByteSize(value).has_value()
                 ? std::string()
                 : "expects a size such as 2GiB or 512MiB, got '" + value + "'";
    };
    cmd->add_option("--memory-limit", args.memory_limit,
                    "antb1: memory a query may use, e.g. 2GiB (bytes, KiB, MiB or GiB)")
        ->check(size);
    cmd->add_option("--oracle-memory-limit", args.oracle_memory_limit,
                    "DuckDB: memory the oracle may use before it spills to --temp-dir, e.g. 1GiB "
                    "(bytes, KiB, MiB or GiB; default: DuckDB's, 80% of the host's memory)")
        ->check(size);
    cmd->add_option("--only", args.only, "Run only Q<n> (repro)");
    cmd->add_option(
        "--shard", args.shard,
        "Run only slice K of N (K/N): the queries at index K - 1, K - 1 + N, ... of the "
        "file, from 0, checked against the ratchet alone");
    cmd->add_option("--mutate", args.mutate, "Corrupt antb1 results (harness self-tests)");
    cmd->add_option("--costs", args.costs,
                    "Print each query's seconds and antb1's peak memory, and write them to this "
                    "file as JSON lines (truncated first)");
    return cmd;
  };
  const auto* tpch = add_numbered("tpch", "TPC-H", "tests/data/tpch_status.json");
  const auto* tpcds = add_numbered("tpcds", "TPC-DS", "tests/data/tpcds_status.json");
  const auto* version = app.add_subcommand("version", "Print the DuckDB version the oracle uses");
  try {
    app.parse(static_cast<int>(argv.size()), argv.data());
  } catch (const CLI::ParseError& e) {
    return app.exit(e) == 0 ? 0 : kExitUsage;
  }
  if (args.show_values && github_actions) {
    std::println(stderr,
                 "antb1-slt: --show-values prints values and query text; it is refused when "
                 "GITHUB_ACTIONS=true, as CI logs are public");
    return kExitUsage;
  }
  if (version->parsed()) {
#ifdef ANTB1_SLT_HAVE_DUCKDB
    std::println("antb1-slt: DuckDB oracle {}", DuckDbEngine::Version());
#else
    std::println("antb1-slt: built without DuckDB");
#endif
    return 0;
  }
  std::error_code ec;
  args.fixtures = fs::absolute(args.fixtures, ec).lexically_normal().string();
  if (!args.fixtures.empty() && args.fixtures.back() == '/') {
    args.fixtures.pop_back();
  }
  if (args.temp_dir.empty()) {
    args.temp_dir = (fs::path(args.fixtures).parent_path() / "slt-tmp").string();
  }
  if (tpch->parsed() || tpcds->parsed()) {
    // An earlier run's costs and refusal report go before any setup, on GitHub Actions too, so that
    // a run that fails leaves none behind that would read as its own.
    if (!args.costs.empty()) {
      fs::remove(args.costs, ec);
    }
    fs::remove(PlanRefusalReport(false, false, fs::absolute(args.temp_dir, ec), args.fixtures).file,
               ec);
  }
  const auto tables = LoadTables(args.tables, args.fixtures);
  if (!tables) {
    std::println(stderr, "antb1-slt: {}", tables.error());
    return kExitSetup;
  }
  // Redacted unless --show-values: tables marked `redact`, `answers`, `tpch` and `tpcds`.
  // `tpch` and `tpcds` too: their queries are derived from TPC-H and TPC-DS, whatever the tables
  // file says.
  const bool redacted_data = answers->parsed() || tpch->parsed() || tpcds->parsed() ||
                             std::ranges::any_of(*tables, &TableDef::redact);
  if (redacted_data && complete->parsed()) {
    std::println(stderr,
                 "antb1-slt: complete writes values into .slt files; it is refused for tables "
                 "marked `redact` in {}",
                 args.tables);
    return kExitUsage;
  }
  args.redact = args.redact || (redacted_data && !args.show_values);
  if (diff->parsed() && args.list && args.redact) {
    std::println(stderr,
                 "antb1-slt: diff --list prints the generated SQL, which holds values of the "
                 "tables; it is refused in a redacted run (--redact, or tables marked `redact` "
                 "without --show-values)");
    return kExitUsage;
  }
  const std::string command = CommandLine(argv, redacted_data);
  if (diff->parsed()) {
    return Diff(args, *tables, command);
  }
  if (pending->parsed()) {
    return Pending(args, *tables, command);
  }
  if (queries->parsed()) {
    return Queries(args, *tables, command);
  }
  if (clickbench->parsed()) {
    return ClickBench(args, *tables, command);
  }
  if (answers->parsed()) {
    return Answers(args, *tables, command);
  }
  if (tpch->parsed()) {
    return NumberedWorkload(args, *tables, command, kTpch, github_actions);
  }
  if (tpcds->parsed()) {
    return NumberedWorkload(args, *tables, command, kTpcds, github_actions);
  }
  return complete->parsed() ? Complete(args, *tables) : Run(args, *tables, command);
}

}  // namespace
}  // namespace antb1::slt

int main(int argc, char** argv, char* const* envp) {
  try {
    return antb1::slt::Main(std::span(argv, static_cast<std::size_t>(argc)),
                            antb1::slt::ReadEnvironment(envp));
  } catch (const std::exception& e) {
    std::fputs("antb1-slt: internal error: ", stderr);  // C stdio: cannot throw again
    std::fputs(e.what(), stderr);
    std::fputs("\n", stderr);
  } catch (...) {
    std::fputs("antb1-slt: internal error\n", stderr);
  }
  return 70;
}
