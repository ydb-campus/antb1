// antb1-fixturegen: writes the deterministic Parquet fixtures of the test harness, or compares the
// Parquet schema of a file with the hits-like schema.
//
//   antb1-fixturegen <outdir>                              write every fixture into <outdir>
//   antb1-fixturegen --check-schema [--redact] <file>...   exit 0 if every <file> has the hits-like
//                                                          schema, else 1
//
// --check-schema also checks real ClickBench partitions (ctest data.hits0.schema). Its report holds
// only column names and Parquet types, never values; --redact, which every data test passes, is
// accepted for that convention and changes nothing.
// Exit codes: 0 ok, 1 schema mismatch, 2 usage, 3 I/O error.

#include <algorithm>
#include <cstdio>
#include <exception>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fixtures.h"
#include "hits_schema.h"

namespace {

constexpr int kExitMismatch = 1;
constexpr int kExitUsage = 2;
constexpr int kExitIo = 3;

int Usage() {
  std::println(stderr,
               "usage: antb1-fixturegen <outdir>\n"
               "       antb1-fixturegen --check-schema [--redact] <parquet file>...");
  return kExitUsage;
}

int Generate(const std::string& dir) {
  auto written = antb1::fixturegen::WriteAllFixtures(dir);
  if (!written.ok()) {
    std::println(stderr, "antb1-fixturegen: {}", written.status().ToString());
    return kExitIo;
  }
  for (const auto& f : *written) {
    std::println("  {:<36} {:>6} rows {:>2} row group(s)", f.path, f.rows, f.row_groups);
  }
  std::println("antb1-fixturegen: wrote {} files to {}", written->size(), dir);
  return 0;
}

int CheckSchema(const std::string& path) {
  auto diffs = antb1::fixturegen::CheckHitsSchema(path);
  if (!diffs.ok()) {
    std::println(stderr, "antb1-fixturegen: {}", diffs.status().ToString());
    return kExitIo;
  }
  for (const auto& d : *diffs) {
    std::println("  {}", d);
  }
  if (!diffs->empty()) {
    std::println("antb1-fixturegen: {}: {} difference(s) from the hits-like schema", path,
                 diffs->size());
    return kExitMismatch;
  }
  std::println("antb1-fixturegen: {}: matches the hits-like schema ({} columns)", path,
               antb1::fixturegen::kHitsColumnCount);
  return 0;
}

int Run(std::span<char*> argv) {
  std::vector<std::string_view> args(argv.begin() + 1, argv.end());
  if (args.size() == 1 && !args[0].starts_with('-')) {
    return Generate(std::string(args[0]));
  }
  if (args.empty() || args[0] != "--check-schema") {
    return Usage();
  }
  args.erase(args.begin());
  if (!args.empty() && args[0] == "--redact") {
    args.erase(args.begin());
  }
  if (args.empty() || std::ranges::any_of(args, [](auto a) { return a.starts_with('-'); })) {
    return Usage();
  }
  int rc = 0;
  for (const auto file : args) {
    rc = std::max(rc, CheckSchema(std::string(file)));
  }
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return Run(std::span(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& e) {
    std::fputs("antb1-fixturegen: internal error: ", stderr);  // C stdio: cannot throw again
    std::fputs(e.what(), stderr);
    std::fputs("\n", stderr);
  } catch (...) {
    std::fputs("antb1-fixturegen: internal error\n", stderr);
  }
  return 70;
}
