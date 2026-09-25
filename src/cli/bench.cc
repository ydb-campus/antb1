// `antb1 bench`: runs a query file several times and writes ClickBench's result JSON
// (docs/benchmarks.md).

#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arrow/status.h>
#include <arrow/util/config.h>

#include "antb1/cli/cli.h"
#include "antb1/common/version.h"
#include "antb1/engine/session.h"

#include "cli_internal.h"
#include <sys/utsname.h>

namespace antb1::cli {
namespace {

std::string_view Trim(std::string_view s) {
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

// Plain fixed-point seconds (never an exponent), as ClickBench's tables expect.
std::string Seconds(double seconds) { return std::format("{:.6f}", seconds); }

// Queries that fail because they are outside the subset; any other failure is reported by the
// exit code.
bool OutOfSubset(const arrow::Status& status) {
  const std::string_view kind = ErrorKind(status);
  return kind == "unsupported" || kind == "parse" || kind == "bind";
}

arrow::Status WriteReport(const std::string& path, const std::string& json, std::ostream& out) {
  if (path == "-") {
    out << json;
    out.flush();
    return arrow::Status::OK();
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << json;
  file.close();
  if (!file) {
    return arrow::Status::IOError("cannot write '", path, "'");
  }
  return arrow::Status::OK();
}

}  // namespace

std::vector<std::string> ReadBenchQueries(std::string_view text) {
  std::vector<std::string> queries;
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    const std::string_view line = Trim(text.substr(0, end));
    if (!line.empty()) {
      queries.emplace_back(line);
    }
    text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
  }
  return queries;
}

std::string DefaultMachine() {
  utsname host{};
  const unsigned cpus = std::thread::hardware_concurrency();
  if (::uname(&host) != 0) {
    return std::format("unknown, {} CPUs", cpus);
  }
  return std::format("{} {}, {} CPUs", static_cast<const char*>(host.sysname),
                     static_cast<const char*>(host.machine), cpus);
}

std::string ClickBenchJson(const BenchReport& report) {
  std::string out = "{\n";
  out += "  \"system\": \"antb1\",\n";
  out += std::format("  \"date\": {},\n", JsonString(report.date));
  out += std::format("  \"machine\": {},\n", JsonString(report.machine));
  out += "  \"cluster_size\": 1,\n";
  out += "  \"proprietary\": \"no\",\n";
  out += "  \"hardware\": \"cpu\",\n";
  out += "  \"tuned\": \"no\",\n";
  out += "  \"tags\": [\"C++\", \"column-oriented\", \"embedded\", \"stateless\"],\n";
  out += std::format("  \"load_time\": {},\n", Seconds(report.load_time));
  out += std::format("  \"data_size\": {},\n", report.data_size);
  out += "  \"concurrent_qps\": null,\n";
  out += "  \"concurrent_error_ratio\": null,\n";
  out += "  \"result\": [";
  std::string failures;
  for (std::size_t q = 0; q < report.queries.size(); ++q) {
    const BenchQuery& query = report.queries[q];
    out += q == 0 ? "\n    [" : ",\n    [";
    for (std::size_t t = 0; t < query.seconds.size(); ++t) {
      out += t == 0 ? "" : ", ";
      const std::optional<double>& seconds = query.seconds[t];
      out += seconds.has_value() ? Seconds(*seconds) : "null";
    }
    out += ']';
    if (!query.error.ok()) {
      failures += std::format(R"({}{{"query": {}, "kind": {}}})", failures.empty() ? "" : ", ", q,
                              JsonString(ErrorKind(query.error)));
    }
  }
  out += report.queries.empty() ? "],\n" : "\n  ],\n";
  out += "  \"antb1\": {\n";
  out += std::format("    \"version\": {},\n", JsonString(Version()));
  out += std::format("    \"git_sha\": {},\n",
                     report.git_sha.empty() ? "null" : JsonString(report.git_sha));
  out += std::format("    \"compiler\": {},\n", JsonString(CompilerVersion()));
  out += std::format("    \"arrow\": {},\n", JsonString(ARROW_VERSION_STRING));
  out += std::format("    \"build_type\": {},\n", JsonString(BuildType()));
  out += std::format("    \"cache\": \"{}\",\n", report.cold ? "cold" : "lukewarm");
  out += std::format("    \"tries\": {},\n", report.tries);
  out += std::format("    \"batch_size\": {},\n", report.batch_size);
  out += std::format("    \"failed\": [{}]\n", failures);
  out += "  }\n}\n";
  return out;
}

int RunBench(engine::Session& session, const BenchSettings& settings, BenchReport report,
             const CliHooks& hooks, std::ostream& out, std::ostream& err) {
  std::ifstream file(settings.queries_file, std::ios::binary);
  if (!file) {
    err << std::format("antb1: io error: cannot read the query file '{}'\n", settings.queries_file);
    return kExitIo;
  }
  const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  const std::vector<std::string> queries = ReadBenchQueries(text);
  report.tries = settings.tries;
  report.cold = settings.drop_caches;
  std::optional<arrow::Status> first_failure;
  for (std::size_t q = 0; q < queries.size(); ++q) {
    BenchQuery result;
    if (settings.drop_caches) {
      if (const arrow::Status dropped = hooks.drop_caches(); !dropped.ok()) {
        err << "antb1: io error: cannot drop the page cache: " << dropped.message() << '\n';
        return kExitIo;
      }
    }
    for (int t = 0; t < settings.tries && result.error.ok(); ++t) {
      const double start = hooks.seconds();
      auto answer = session.Execute(queries[q]);
      const double elapsed = hooks.seconds() - start;
      if (answer.ok()) {
        result.seconds.emplace_back(elapsed);
      } else {
        result.error = answer.status();
      }
    }
    std::string line = std::format("Q{}:", q);
    if (result.error.ok()) {
      for (const auto& s : result.seconds) {
        line += " " + Seconds(s.value_or(0));
      }
    } else {
      result.seconds.assign(static_cast<std::size_t>(settings.tries), std::nullopt);
      line += std::format(" {} error", ErrorKind(result.error));
      if (!first_failure.has_value() && !OutOfSubset(result.error)) {
        first_failure = result.error;
      }
    }
    err << line << '\n';
    report.queries.push_back(std::move(result));
  }
  if (const arrow::Status written = WriteReport(settings.out, ClickBenchJson(report), out);
      !written.ok()) {
    err << "antb1: io error: " << written.message() << '\n';
    return kExitIo;
  }
  std::size_t answered = 0;
  for (const BenchQuery& q : report.queries) {
    answered += q.error.ok() ? 1U : 0U;
  }
  err << std::format("bench: {} queries, {} answered, {} failed; {} ({})\n", queries.size(),
                     answered, queries.size() - answered,
                     settings.out == "-" ? "JSON on stdout" : "wrote " + settings.out,
                     report.cold ? "cold: page cache dropped before each query" : "lukewarm");
  return first_failure.has_value() ? ExitCodeFor(*first_failure) : kExitOk;
}

}  // namespace antb1::cli
