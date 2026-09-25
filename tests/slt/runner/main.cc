// antb1-slt: the sqllogictest runner of the test harness (tests/slt/README.md).
//
//   antb1-slt run --engine antb1|duckdb --fixtures DIR --tables FILE [--redact] [--mutate KIND]
//                 FILE...
//   antb1-slt complete --fixtures DIR --tables FILE FILE...
//   antb1-slt diff --fixtures DIR --tables FILE --seed S --count N [--only I] [--table NAME]...
//                  [--redact] [--mutate KIND] [--target-percent P]
//   antb1-slt version
//
// Exit codes: 0 every record (query) passed, 1 failures, 2 usage or .slt syntax error, 3 setup
// error (tables, fixtures, engine), 70 internal error.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
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

#include "antb1_engine.h"
#include "difftest.h"
#include "engine.h"
#include "query_gen.h"
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
  std::vector<std::string> files;
  // diff
  uint64_t seed = 0;
  uint64_t count = 0;
  std::optional<uint64_t> only;
  std::vector<std::string> only_tables;
  unsigned target_percent = 25;
  bool list = false;
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

// The command line of this run without --redact and --only (diff repro lines add their own).
std::string CommandLine(std::span<char*> argv) {
  std::string command;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--redact" || arg.starts_with("--only=")) {
      continue;
    }
    if (arg == "--only") {
      ++i;
      continue;
    }
    command += (command.empty() ? "" : " ") + ShellQuote(arg);
  }
  return command;
}

// The command line of this run without --redact, plus the ctest invocation when known.
std::string Repro(std::span<char*> argv, const Args& args) {
  std::string command = "    " + CommandLine(argv) + "\n";
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
    auto engine = Antb1Engine::Make(tables);
    if (!engine) {
      return std::unexpected(engine.error());
    }
    return std::unique_ptr<Engine>(std::move(*engine));
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

int Run(const Args& args, const std::vector<TableDef>& tables, std::span<char*> argv) {
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
  const RunOptions options{.redact = args.redact, .repro = Repro(argv, args)};
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
}

void AddCommon(CLI::App* cmd, Args& args) {
  AddSetup(cmd, args);
  cmd->add_option("files", args.files, ".slt files")->required();
}

int Diff(const Args& args, std::vector<TableDef> tables, std::span<char*> argv) {
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
  const DiffOptions options{
      .count = args.count, .only = args.only, .redact = args.redact, .command = CommandLine(argv)};
  std::string out;
  const DiffStats stats =
      RunDiff(*generator, mutating ? *mutating : **antb1, **oracle, options, out);
  std::print("{}", out);
  return stats.failed == 0 ? 0 : kExitFailed;
}

int Main(std::span<char*> argv) {
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
  const auto* version = app.add_subcommand("version", "Print the DuckDB version the oracle uses");
  try {
    app.parse(static_cast<int>(argv.size()), argv.data());
  } catch (const CLI::ParseError& e) {
    return app.exit(e) == 0 ? 0 : kExitUsage;
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
  if (diff->parsed()) {
    return Diff(args, *tables, argv);
  }
  return complete->parsed() ? Complete(args, *tables) : Run(args, *tables, argv);
}

}  // namespace
}  // namespace antb1::slt

int main(int argc, char** argv) {
  try {
    return antb1::slt::Main(std::span(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& e) {
    std::fputs("antb1-slt: internal error: ", stderr);  // C stdio: cannot throw again
    std::fputs(e.what(), stderr);
    std::fputs("\n", stderr);
  } catch (...) {
    std::fputs("antb1-slt: internal error\n", stderr);
  }
  return 70;
}
