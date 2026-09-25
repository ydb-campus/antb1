// antb1-fuzz-replay: runs input files through the SQL round-trip property (sql_parser_property.h)
// without libFuzzer, so the committed corpus and the regressions are checked by every build (GCC
// too). ctest fuzz.replay.sql_parser (label fuzz-replay) replays fuzz/corpus/sql_parser and
// fuzz/regressions.
//
//   antb1-fuzz-replay [--mutate-unparse=suffix|whitespace] PATH...
//
// A PATH is a file or a directory; directories are read recursively in sorted order, skipping
// README.md and dotfiles. --mutate-unparse is the harness self-test: it replays with a broken
// unparser, so the property must report violations:
//   suffix      appends "_mutated": the text no longer parses, or parses to another table name;
//   whitespace  appends a space when the statement did not start at offset 0 (leading whitespace or
//               a comment): the round trip holds, but ToSql is no longer idempotent.
//
// Exit codes: 0 the property holds for every input, 1 violations, 2 usage or I/O error (a missing
// path, an unreadable file, no input at all), 70 internal error.

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "antb1/sql/ast.h"
#include "antb1/sql/unparse.h"

#include "sql_parser_property.h"

namespace antb1::fuzz {
namespace {

namespace fs = std::filesystem;

constexpr int kExitViolations = 1;
constexpr int kExitUsage = 2;

// The broken unparsers of the self-test (--mutate-unparse).
std::string SuffixToSql(const sql::SelectStatement& stmt) { return sql::ToSql(stmt) + "_mutated"; }
std::string WhitespaceToSql(const sql::SelectStatement& stmt) {
  return sql::ToSql(stmt) + (stmt.span.offset == 0 ? "" : " ");
}

constexpr std::string_view kUsage =
    "usage: antb1-fuzz-replay [--mutate-unparse=suffix|whitespace] PATH...";

bool Skipped(const fs::path& path) {
  const std::string name = path.filename().string();
  return name.starts_with('.') || name == "README.md";
}

// Appends the input files of `path` (a file, or the files under a directory) in sorted order.
bool CollectInputs(const fs::path& path, std::vector<fs::path>& inputs) {
  std::error_code ec;
  const fs::file_status status = fs::status(path, ec);
  if (ec || !fs::exists(status)) {
    std::println(stderr, "antb1-fuzz-replay: {}: no such file or directory", path.string());
    return false;
  }
  if (!fs::is_directory(status)) {
    inputs.push_back(path);
    return true;
  }
  std::vector<fs::path> found;
  for (auto it = fs::recursive_directory_iterator(path, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (Skipped(it->path())) {
      if (it->is_directory()) {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (it->is_regular_file()) {
      found.push_back(it->path());
    }
  }
  if (ec) {
    std::println(stderr, "antb1-fuzz-replay: {}: {}", path.string(), ec.message());
    return false;
  }
  std::ranges::sort(found);
  inputs.insert(inputs.end(), found.begin(), found.end());
  return true;
}

std::optional<std::string> ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  if (in.bad()) {
    return std::nullopt;
  }
  return data;
}

int Run(std::span<const std::string_view> args) {
  Unparser unparse = &sql::ToSql;
  std::vector<fs::path> inputs;
  bool any_path = false;
  for (const std::string_view arg : args) {
    if (arg == "--mutate-unparse=suffix") {
      unparse = &SuffixToSql;
    } else if (arg == "--mutate-unparse=whitespace") {
      unparse = &WhitespaceToSql;
    } else if (arg.starts_with('-')) {
      std::println(stderr, "antb1-fuzz-replay: unknown option '{}'\n{}", arg, kUsage);
      return kExitUsage;
    } else {
      any_path = true;
      if (!CollectInputs(fs::path(arg), inputs)) {
        return kExitUsage;
      }
    }
  }
  if (!any_path) {
    std::println(stderr, "{}", kUsage);
    return kExitUsage;
  }
  if (inputs.empty()) {
    std::println(stderr, "antb1-fuzz-replay: no input files found");
    return kExitUsage;
  }

  std::size_t violations = 0;
  for (const fs::path& path : inputs) {
    // Printed first, so a crash in the parser (ASan/UBSan report) names its input.
    std::println("replay {}", path.string());
    std::fflush(stdout);
    const std::optional<std::string> data = ReadFile(path);
    if (!data) {
      std::println(stderr, "antb1-fuzz-replay: {}: cannot read the file", path.string());
      return kExitUsage;
    }
    if (const std::string violation = SqlParserPropertyViolation(*data, unparse);
        !violation.empty()) {
      ++violations;
      std::println("VIOLATION {}: {}\n  input: {}", path.string(), violation, Printable(*data));
    }
  }
  if (violations > 0) {
    std::println("fuzz replay: FAIL inputs={} violations={}", inputs.size(), violations);
    return kExitViolations;
  }
  std::println("fuzz replay: PASS inputs={}", inputs.size());
  return 0;
}

}  // namespace
}  // namespace antb1::fuzz

int main(int argc, char** argv) {
  try {
    const std::span<char*> raw(argv, static_cast<std::size_t>(argc));
    const std::span<char*> rest = raw.empty() ? raw : raw.subspan(1);
    const std::vector<std::string_view> args(rest.begin(), rest.end());
    return antb1::fuzz::Run(args);
  } catch (const std::exception& e) {
    std::fputs("antb1-fuzz-replay: internal error: ", stderr);  // C stdio: cannot throw again
    std::fputs(e.what(), stderr);
    std::fputs("\n", stderr);
  } catch (...) {
    std::fputs("antb1-fuzz-replay: internal error\n", stderr);
  }
  return 70;
}
