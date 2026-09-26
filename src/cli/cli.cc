#include "antb1/cli/cli.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <istream>
#include <iterator>
#include <memory>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <arrow/api.h>
#include <arrow/util/config.h>

#include "antb1/common/source_span.h"
#include "antb1/common/version.h"
#include "antb1/engine/format.h"
#include "antb1/engine/session.h"
#include "antb1/plan/sql_status.h"
#include "antb1/plan/types.h"

#include "cli_internal.h"
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>

namespace antb1::cli {

std::string_view ErrorKind(const arrow::Status& status) {
  if (auto detail = plan::GetSqlError(status)) {
    switch (detail->kind()) {
      case plan::SqlErrorDetail::Kind::kParse:
        return "parse";
      case plan::SqlErrorDetail::Kind::kUnsupported:
        return "unsupported";
      case plan::SqlErrorDetail::Kind::kBind:
        return "bind";
    }
  }
  if (status.IsIOError()) {
    return "io";
  }
  if (status.IsExecutionError() || status.IsInvalid()) {
    return "execution";
  }
  return "internal";
}

std::string JsonString(std::string_view s) { return "\"" + engine::JsonEscape(s) + "\""; }

namespace {

struct Inputs {
  std::vector<std::string> tables;        // NAME=PATH[,PATH...]
  std::vector<std::string> column_types;  // COLUMN=DATE
  bool clickbench = false;
  std::string command;  // -c
  std::string file;     // -f
  std::string format = "table";
  bool timing = false;
  BenchSettings bench;
};

// The real implementations of the hooks that `hooks` leaves empty.
CliHooks WithDefaults(CliHooks hooks) {
  if (!hooks.seconds) {
    hooks.seconds = [] {
      return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
          .count();
    };
  }
  if (!hooks.today) {
    hooks.today = [] {
      const auto today = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
      return std::format("{:%F}", std::chrono::year_month_day(today));
    };
  }
  if (!hooks.drop_caches) {
    hooks.drop_caches = [] {
      return RunCommand({"sudo", "-n", "sh", "-c", "sync && echo 3 > /proc/sys/vm/drop_caches"});
    };
  }
  return hooks;
}

// kind_override replaces the status-derived kind (used for command-line errors: "usage").
void ReportError(const arrow::Status& status, std::string_view sql, bool json, std::ostream& err,
                 std::string_view kind_override = {}) {
  const auto detail = plan::GetSqlError(status);
  const std::string& message = status.message();
  const std::string_view kind = kind_override.empty() ? ErrorKind(status) : kind_override;
  if (json) {
    std::string obj =
        std::format(R"({{"error":{{"kind":"{}","message":{})", kind, JsonString(message));
    if (detail) {
      const auto lc = ToLineColumn(sql, detail->span());
      obj += std::format(R"(,"offset":{},"length":{},"line":{},"column":{})", detail->span().offset,
                         detail->span().length, lc.line, lc.column);
    }
    err << obj << "}}\n";
    return;
  }
  err << "antb1: " << kind << " error: " << message << '\n';
  if (detail) {
    const auto lc = ToLineColumn(sql, detail->span());
    std::size_t line_start = detail->span().offset;
    while (line_start > 0 && sql[line_start - 1] != '\n') {
      --line_start;
    }
    std::size_t line_end = sql.find('\n', line_start);
    if (line_end == std::string_view::npos) {
      line_end = sql.size();
    }
    err << std::format("  at line {}, column {}\n", lc.line, lc.column);
    err << "  " << sql.substr(line_start, line_end - line_start) << '\n';
    err << "  " << std::string(lc.column - 1, ' ')
        << std::string(std::max<std::size_t>(detail->span().length, 1), '^') << '\n';
  }
}

arrow::Result<std::string> ReadSql(const Inputs& in, std::istream& stdin_stream) {
  if (in.command.empty() == in.file.empty()) {
    return arrow::Status::Invalid("pass exactly one of -c SQL, -c - (stdin) or -f FILE");
  }
  if (in.command == "-") {
    return std::string(std::istreambuf_iterator<char>(stdin_stream), {});
  }
  if (!in.command.empty()) {
    return in.command;
  }
  std::ifstream file(in.file, std::ios::binary);
  if (!file) {
    return arrow::Status::IOError("cannot read SQL file '", in.file, "'");
  }
  return std::string(std::istreambuf_iterator<char>(file), {});
}

arrow::Result<std::unique_ptr<engine::Session>> MakeSession(const Inputs& in) {
  engine::SessionOptions options;
  if (in.clickbench) {
    options.default_overrides.emplace_back("EventDate", plan::LogicalType::kDate);
  }
  for (const auto& spec : in.column_types) {
    const auto eq = spec.find('=');
    if (eq == std::string::npos || eq == 0 || spec.substr(eq + 1) != "DATE") {
      return arrow::Status::Invalid("--column-type expects COLUMN=DATE, got '", spec, "'");
    }
    options.default_overrides.emplace_back(spec.substr(0, eq), plan::LogicalType::kDate);
  }
  ARROW_ASSIGN_OR_RAISE(auto session, engine::Session::Make(options));
  for (const auto& spec : in.tables) {
    const auto eq = spec.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == spec.size()) {
      return arrow::Status::Invalid("--table expects NAME=PATH[,PATH...], got '", spec, "'");
    }
    std::vector<std::string> paths;
    std::stringstream list(spec.substr(eq + 1));
    for (std::string p; std::getline(list, p, ',');) {
      if (!p.empty()) {
        paths.push_back(p);
      }
    }
    ARROW_RETURN_NOT_OK(session->RegisterParquet(spec.substr(0, eq), paths));
  }
  return session;
}

void AddTableOptions(CLI::App* cmd, Inputs& in) {
  cmd->add_option("--table", in.tables, "Register NAME=PATH[,PATH|GLOB...] (Parquet)")
      ->type_name("SPEC");
  cmd->add_option("--column-type", in.column_types, "Read COLUMN as another type (COLUMN=DATE)")
      ->type_name("SPEC");
  cmd->add_flag("--clickbench", in.clickbench, "ClickBench conventions (EventDate is a DATE)");
}

void AddSqlOptions(CLI::App* cmd, Inputs& in) {
  cmd->add_option("-c,--command", in.command, "SQL text, or - to read it from stdin");
  cmd->add_option("-f,--file", in.file, "File with the SQL text (use it for SQL with quotes)");
}

int Fail(const arrow::Status& status, std::string_view sql, const Inputs& in, std::ostream& err) {
  ReportError(status, sql, in.format == "json", err);
  return ExitCodeFor(status);
}

// `antb1 bench` once the tables are registered (in load_time seconds).
int Bench(engine::Session& session, const Inputs& in, double load_time, const CliHooks& hooks,
          std::ostream& out, std::ostream& err) {
#ifndef __linux__
  if (in.bench.drop_caches) {
    err << "antb1: usage error: --drop-caches is only supported on Linux\n";
    return kExitUsage;
  }
#endif
  BenchReport report{.date = hooks.today(),
                     .machine = in.bench.machine.empty() ? DefaultMachine() : in.bench.machine,
                     .git_sha = in.bench.git_sha,
                     .batch_size = engine::SessionOptions{}.batch_size,
                     .load_time = load_time};
  for (const auto& spec : in.tables) {
    const auto table = session.catalog().Find(spec.substr(0, spec.find('=')));
    report.data_size += table == nullptr ? 0 : table->data_size().value_or(0);
  }
  return RunBench(session, in.bench, std::move(report), hooks, out, err);
}

}  // namespace

int ExitCodeFor(const arrow::Status& status) {
  if (status.ok()) {
    return kExitOk;
  }
  if (auto detail = plan::GetSqlError(status)) {
    return detail->kind() == plan::SqlErrorDetail::Kind::kUnsupported ? kExitUnsupported
                                                                      : kExitQueryError;
  }
  if (status.IsIOError()) {
    return kExitIo;
  }
  if (status.IsExecutionError() || status.IsInvalid()) {
    return kExitQueryError;
  }
  return kExitInternal;
}

int RunCli(std::span<const std::string> args, std::istream& in_stream, std::ostream& out,
           std::ostream& err, const CliHooks& hooks) {
  const CliHooks h = WithDefaults(hooks);
  CLI::App app{"antb1: experimental SQL engine over Parquet files", "antb1"};
  app.require_subcommand(1);
  Inputs in;

  auto* query = app.add_subcommand("query", "Run a query and print the result");
  AddSqlOptions(query, in);
  AddTableOptions(query, in);
  query->add_option("--format", in.format, "Output format")
      ->check(CLI::IsMember({"table", "csv", "json"}));
  query->add_flag("--timing", in.timing, "Print elapsed seconds as the last stderr line");

  auto* explain = app.add_subcommand("explain", "Print the logical plan of a query");
  AddSqlOptions(explain, in);
  AddTableOptions(explain, in);

  auto* schema = app.add_subcommand("schema", "Print the columns of the registered tables");
  AddTableOptions(schema, in);

  auto* bench = app.add_subcommand(
      "bench", "Run a query file (one query per line) and write ClickBench's result JSON");
  bench->add_option("--queries", in.bench.queries_file, "Query file: one query per line")
      ->required()
      ->type_name("FILE");
  AddTableOptions(bench, in);
  bench->add_option("--tries", in.bench.tries, "Runs of every query")
      ->default_val(3)
      ->check(CLI::Range(1, 1000));
  bench->add_option("--out", in.bench.out, "Result JSON file, - for stdout")
      ->required()
      ->type_name("FILE");
  bench->add_option("--machine", in.bench.machine, "Machine description (default: this host)");
  bench->add_option("--git-sha", in.bench.git_sha, "Commit of the build, recorded in the JSON");
  bench->add_flag("--drop-caches", in.bench.drop_caches,
                  "Drop the page cache (sudo -n) before the first try of every query (Linux)");

  const auto* version = app.add_subcommand("version", "Print version information");

  std::vector<const char*> argv;
  argv.reserve(args.size());
  for (const auto& a : args) {
    argv.push_back(a.c_str());
  }
  try {
    app.parse(static_cast<int>(argv.size()), argv.data());
  } catch (const CLI::ParseError& e) {
    const int code = app.exit(e, out, err);
    return code == 0 ? kExitOk : kExitUsage;
  }

  if (version->parsed()) {
    out << std::format("antb1 {} (Apache Arrow {})\n", Version(), ARROW_VERSION_STRING);
    return kExitOk;
  }

  const double start = h.seconds();
  std::string sql;
  if (!schema->parsed() && !bench->parsed()) {
    auto text = ReadSql(in, in_stream);
    if (!text.ok()) {
      const bool io = text.status().IsIOError();
      ReportError(text.status(), "", in.format == "json", err, io ? "io" : "usage");
      return io ? kExitIo : kExitUsage;
    }
    sql = std::move(*text);
  }
  auto session = MakeSession(in);
  if (!session.ok()) {
    const bool io = session.status().IsIOError();
    ReportError(session.status(), "", in.format == "json", err, io ? "io" : "usage");
    return io ? kExitIo : kExitUsage;
  }

  if (bench->parsed()) {
    return Bench(**session, in, h.seconds() - start, h, out, err);
  }

  if (schema->parsed()) {
    for (const auto& spec : in.tables) {
      const std::string name = spec.substr(0, spec.find('='));
      const auto table = (*session)->catalog().Find(name);
      out << name << '\n';
      for (const auto& field : table->schema()->fields()) {
        auto logical = plan::FromArrow(*field->type());
        out << "  " << field->name() << '\t'
            << (logical.ok() ? std::string(plan::ToString(*logical))
                             : "unsupported(" + field->type()->ToString() + ")")
            << '\n';
      }
    }
    return kExitOk;
  }

  if (explain->parsed()) {
    auto text = (*session)->Explain(sql);
    if (!text.ok()) {
      return Fail(text.status(), sql, in, err);
    }
    out << *text;
    return kExitOk;
  }

  auto result = (*session)->Execute(sql);
  if (!result.ok()) {
    return Fail(result.status(), sql, in, err);
  }
  auto format = engine::OutputFormat::kTable;
  if (in.format == "csv") {
    format = engine::OutputFormat::kCsv;
  } else if (in.format == "json") {
    format = engine::OutputFormat::kJson;
  }
  auto text = engine::FormatResult(*result, format);
  if (!text.ok()) {
    return Fail(text.status(), sql, in, err);
  }
  out << *text;
  out.flush();
  if (in.timing) {
    err << std::format("{:.6f}\n", h.seconds() - start);  // plain fixed-point (ClickBench)
  }
  return kExitOk;
}

arrow::Status RunCommand(const std::vector<std::string>& argv) {
  if (argv.empty()) {
    return arrow::Status::Invalid("no command to run");
  }
  std::vector<std::string> storage = argv;  // posix_spawnp takes mutable strings
  std::vector<char*> args;
  args.reserve(storage.size() + 1);
  for (std::string& a : storage) {
    args.push_back(a.data());
  }
  args.push_back(nullptr);
  std::vector<char*> no_environment = {nullptr};
  pid_t pid = 0;
  const int spawned =
      ::posix_spawnp(&pid, args[0], nullptr, nullptr, args.data(), no_environment.data());
  if (spawned != 0) {
    return arrow::Status::IOError("cannot run '", argv[0],
                                  "': ", std::generic_category().message(spawned));
  }
  siginfo_t info{};
  while (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED) != 0) {
    if (errno != EINTR) {
      return arrow::Status::IOError("cannot wait for '", argv[0], "'");
    }
  }
  if (info.si_code != CLD_EXITED) {
    return arrow::Status::IOError("'", argv[0], "' was killed");
  }
  if (info.si_status != 0) {
    return arrow::Status::IOError("'", argv[0], "' failed with exit code ", info.si_status);
  }
  return arrow::Status::OK();
}

}  // namespace antb1::cli
