// antb1-fixture-digest: the logical digest of the Parquet fixtures (fixture_digest.h).
//
//   antb1-fixture-digest <fixtures dir>                        print the digest
//   antb1-fixture-digest --check <digest file> <fixtures dir>  exit 1 unless they agree
//   antb1-fixture-digest --write <digest file> <fixtures dir>  rewrite the digest file
//
// Exit codes: 0 ok, 1 digest mismatch, 2 usage, 3 I/O error, 70 internal error.

#include <cstddef>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fixture_digest.h"

namespace {

constexpr int kExitMismatch = 1;
constexpr int kExitUsage = 2;
constexpr int kExitIo = 3;

int Run(std::span<char*> argv) {
  const std::vector<std::string_view> args(argv.begin() + 1, argv.end());
  const bool print = args.size() == 1 && !args[0].starts_with('-');
  const bool check = args.size() == 3 && args[0] == "--check";
  const bool write = args.size() == 3 && args[0] == "--write";
  if (!print && !check && !write) {
    std::println(stderr,
                 "usage: antb1-fixture-digest <fixtures dir>\n"
                 "       antb1-fixture-digest --check|--write <digest file> <fixtures dir>");
    return kExitUsage;
  }
  const std::string dir(args.back());
  auto digests = antb1::harness::DigestDirectory(dir);
  if (!digests.ok()) {
    std::println(stderr, "antb1-fixture-digest: {}", digests.status().ToString());
    return kExitIo;
  }
  const std::string text = antb1::harness::DigestText(*digests);
  if (!check && !write) {
    std::print("{}", text);
    return 0;
  }
  const std::string digest_file(args[1]);
  if (write) {
    std::ofstream out(digest_file, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) {
      std::println(stderr, "antb1-fixture-digest: cannot write '{}'", digest_file);
      return kExitIo;
    }
    std::println("antb1-fixture-digest: wrote {} ({} files)", digest_file, digests->size());
    return 0;
  }
  std::ifstream in(digest_file, std::ios::binary);
  if (!in) {
    std::println(stderr, "antb1-fixture-digest: cannot read '{}'", digest_file);
    return kExitIo;
  }
  const std::string expected(std::istreambuf_iterator<char>(in), {});
  const auto diffs = antb1::harness::CompareDigests(expected, *digests);
  for (const auto& d : diffs) {
    std::println("  {}", d);
  }
  if (!diffs.empty()) {
    std::println(
        "antb1-fixture-digest: FAIL: the fixtures in {} differ from {} ({} difference(s)).\n"
        "  The generator (tools/fixturegen) must write the same data on every platform. After an\n"
        "  intended change: antb1-fixture-digest --write {} {}",
        dir, digest_file, diffs.size(), digest_file, dir);
    return kExitMismatch;
  }
  std::println("antb1-fixture-digest: PASS: {} files match {}", digests->size(), digest_file);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return Run(std::span(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& e) {
    std::fputs("antb1-fixture-digest: internal error: ", stderr);  // C stdio: cannot throw again
    std::fputs(e.what(), stderr);
    std::fputs("\n", stderr);
  } catch (...) {
    std::fputs("antb1-fixture-digest: internal error\n", stderr);
  }
  return 70;
}
