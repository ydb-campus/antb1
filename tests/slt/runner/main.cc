// antb1-slt: the sqllogictest runner of the test harness (tests/slt/README.md).
//
//   antb1-slt run --engine antb1|duckdb --fixtures DIR --tables FILE [--redact] [--mutate KIND]
//                 FILE...
//   antb1-slt complete --fixtures DIR --tables FILE FILE...
//   antb1-slt diff --fixtures DIR --tables FILE --seed S --count N [--only I] [--table NAME]...
//                  [--redact] [--mutate KIND] [--target-percent P]
//   antb1-slt queries --fixtures DIR --tables FILE [--redact] [--only LINE] [--mutate KIND] FILE
//   antb1-slt clickbench --fixtures DIR --tables FILE --status FILE [--redact] [--only N]
//                        [--mutate KIND] QUERIES
//   antb1-slt answers --fixtures DIR --tables FILE --queries DIR --answers DIR [--only N]
//                     [--mutate KIND]
//   antb1-slt tpch --fixtures DIR --tables FILE --queries DIR --status FILE [--memory-limit SIZE]
//                  [--only N] [--mutate KIND]
//   antb1-slt version
//
// `queries` and `clickbench` run the ClickBench data tests (tests/data): see query_file.h and
// clickbench.h. `answers` checks stored answers against the DuckDB oracle (answers.h). `tpch` is
// the ratchet of the queries derived from TPC-H (ratchet.h, tests/tpch): q01.sql, q02.sql, ... of
// DIR as Q1, Q2, ...; ANTB1_TPCH_TIMES=1 adds each query's seconds and their geometric mean.
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
  // answers, tpch
  std::string queries_dir;
  std::string answers_dir;
  // tpch: the antb1 sessions' memory limit (engine::SessionOptions::memory_limit)
  std::string memory_limit;
  bool times = false;  // ANTB1_TPCH_TIMES=1
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

// The command line of this run without --redact, --show-values and --only (repro lines add their
// own --only), with --show-values when the run is redacted without --redact (`show_values`).
std::string CommandLine(std::span<char*> argv, bool show_values) {
  std::string command;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--redact" || arg == "--show-values" || arg.starts_with("--only=")) {
      continue;
    }
    if (arg == "--only") {
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
  bool tpch_times = false;      // ANTB1_TPCH_TIMES=1
};

Environment ReadEnvironment(char* const* envp) {
  Environment env;
  for (char* const* entry = envp; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view variable(*entry);
    env.github_actions = env.github_actions || variable == "GITHUB_ACTIONS=true";
    env.tpch_times = env.tpch_times || variable == "ANTB1_TPCH_TIMES=1";
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
  auto engine = DuckDbEngine::Make(tables, args.fixtures, args.temp_dir);
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
                  "DuckDB temp directory (default: <fixtures>/../slt-tmp)");
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
  const DiffOptions options{.count = args.count,
                            .only = args.only,
                            .redact = args.redact,
                            .pixi_repro = IsDiffRandomTables(args.tables),
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
};

int Tpch(const Args& args, const std::vector<TableDef>& tables, const std::string& command) {
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
  auto status = ParseRatchetStatus(*status_text, kTpch);
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
                         .status_path = args.status,
                         .command = command,
                         .clock = {}};
  if (args.times) {
    options.clock = [] {
      return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
          .count();
    };
  }
  // Each line as it comes, so the log of a run that times out ends with the query it was running.
  const RatchetStats stats = RunRatchet(*queries, *status, kTpch, antb1->get(), **oracle, options,
                                        [](std::string_view text) {
                                          std::print("{}", text);
                                          std::fflush(stdout);
                                        });
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
  auto* tpch = app.add_subcommand(
      "tpch", "Ratchet of the queries derived from TPC-H: q01.sql, ... on antb1 vs DuckDB");
  AddSetup(tpch, args);
  tpch->add_option("--queries", args.queries_dir, "Directory of q01.sql, q02.sql, ...")->required();
  tpch->add_option("--status", args.status, "The ratchet (tests/data/tpch_status.json)")
      ->required();
  tpch->add_option("--memory-limit", args.memory_limit,
                   "antb1: memory a query may use, e.g. 2GiB (bytes, KiB, MiB or GiB)")
      ->check([](const std::string& value) -> std::string {
        return ParseByteSize(value).has_value()
                   ? std::string()
                   : "expects a size such as 2GiB or 512MiB, got '" + value + "'";
      });
  tpch->add_option("--only", args.only, "Run only Q<n> (repro)");
  tpch->add_option("--mutate", args.mutate, "Corrupt antb1 results (harness self-tests)");
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
  const auto tables = LoadTables(args.tables, args.fixtures);
  if (!tables) {
    std::println(stderr, "antb1-slt: {}", tables.error());
    return kExitSetup;
  }
  // Redacted unless --show-values: tables marked `redact`, `answers` and `tpch`.
  // `tpch` too: its queries are derived from TPC-H, whatever the tables file says.
  const bool redacted_data =
      answers->parsed() || tpch->parsed() || std::ranges::any_of(*tables, &TableDef::redact);
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
    args.times = env.tpch_times;
    return Tpch(args, *tables, command);
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
