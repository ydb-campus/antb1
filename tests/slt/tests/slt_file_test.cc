#include "slt_file.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "canonical.h"

namespace antb1::slt {
namespace {

constexpr std::string_view kFile =
    "# comment\n"
    "statement ok\n"
    "SELECT 1\n"
    "\n"
    "onlyif duckdb\n"
    "statement error ^Catalog Error: .*\n"
    "SELECT * FROM nope\n"
    "\n"
    "hash-threshold 8\n"
    "\n"
    "skipif antb1\n"
    "# tol 1e-6\n"
    "query IR rowsort lbl\n"
    "SELECT a,\n"
    "  b FROM t\n"
    "----\n"
    "1\t2.5\n"
    "3\t4.5\n"
    "\n"
    "query T\n"
    "SELECT s FROM t\n"
    "\n"
    "halt\n";

TEST(ParseSlt, ReadsEveryDirective) {
  auto file = ParseSlt("f.slt", kFile);
  ASSERT_TRUE(file.has_value()) << file.error();
  ASSERT_EQ(file->records.size(), 6U);
  EXPECT_EQ(file->records[5].kind, RecordKind::kHalt);
  const auto& ok = file->records[0];
  EXPECT_EQ(ok.kind, RecordKind::kStatementOk);
  EXPECT_EQ(ok.line, 2);
  EXPECT_EQ(ok.sql, "SELECT 1");
  const auto& error = file->records[1];
  EXPECT_EQ(error.kind, RecordKind::kStatementError);
  EXPECT_EQ(error.error_regex, "^Catalog Error: .*");
  EXPECT_EQ(error.onlyif, (std::vector<std::string>{"duckdb"}));
  EXPECT_FALSE(error.RunsOn("antb1"));
  EXPECT_TRUE(error.RunsOn("duckdb"));
  EXPECT_EQ(file->records[2].kind, RecordKind::kHashThreshold);
  EXPECT_EQ(file->records[2].hash_threshold, 8);
  const auto& query = file->records[3];
  EXPECT_EQ(query.kind, RecordKind::kQuery);
  EXPECT_EQ(query.types, "IR");
  EXPECT_EQ(query.sort, SortMode::kRowSort);
  EXPECT_EQ(query.label, "lbl");
  EXPECT_EQ(query.tolerance, 1e-6);
  EXPECT_EQ(query.sql, "SELECT a,\n  b FROM t");
  EXPECT_EQ(query.expected, (std::vector<std::string>{"1\t2.5", "3\t4.5"}));
  EXPECT_TRUE(query.has_separator);
  EXPECT_FALSE(query.RunsOn("antb1"));
  const auto& bare = file->records[4];
  EXPECT_EQ(bare.sort, SortMode::kNoSort);
  EXPECT_FALSE(bare.has_separator);
  EXPECT_TRUE(bare.expected.empty());
}

TEST(ParseSlt, RejectsMalformedFiles) {
  const std::map<std::string, std::string> cases = {
      {"frobnicate\n", "f.slt:1: unknown or malformed directive 'frobnicate'"},
      {"query X\nSELECT 1\n",
       "f.slt:1: expected 'query <I|R|T...> [nosort|rowsort|valuesort] [label]'"},
      {"query I sideways\nSELECT 1\n",
       "f.slt:1: unknown sort mode 'sideways' (nosort, rowsort or valuesort)"},
      {"onlyif mysql\nstatement ok\nSELECT 1\n",
       "f.slt:1: expected 'onlyif antb1' or 'onlyif duckdb'"},
      {"onlyif duckdb\n\nstatement ok\nSELECT 1\n",
       "f.slt:2: skipif/onlyif must be followed directly by a record"},
      {"# tol 1e-6\nstatement ok\nSELECT 1\n", "f.slt:2: '# tol' must precede a query record"},
      {"# tol 2\nquery R\nSELECT 1\n",
       "f.slt:1: expected '# tol <relative tolerance>' with 0 < tolerance < 1"},
      {"statement ok\n\n", "f.slt:1: record without SQL"},
      {"statement ok\nSELECT 1\n----\n1\n", "f.slt:3: statement records have no result block"},
      {"hash-threshold -1\n", "f.slt:1: expected 'hash-threshold <n>' with n >= 0"},
      {"onlyif antb1\n", "f.slt:1: condition or '# tol' without a record at the end of the file"},
  };
  for (const auto& [text, message] : cases) {
    auto file = ParseSlt("f.slt", text);
    ASSERT_FALSE(file.has_value()) << text;
    EXPECT_EQ(file.error(), message) << text;
  }
  auto bad_regex = ParseSlt("f.slt", "statement error ([\nSELECT 1\n");
  ASSERT_FALSE(bad_regex.has_value());
  EXPECT_TRUE(bad_regex.error().starts_with("f.slt:1: invalid error regex")) << bad_regex.error();
}

TEST(RewriteSlt, ReplacesBlocksAndKeepsEverythingElse) {
  auto file = ParseSlt("f.slt", kFile);
  ASSERT_TRUE(file.has_value());
  EXPECT_EQ(RewriteSlt(*file, {}), kFile);

  std::map<std::size_t, BlockUpdate> updates;
  updates[3] = BlockUpdate{.lines = {"7\t8.5"}, .new_types = std::nullopt};
  updates[4] = BlockUpdate{.lines = {"x", "y"}, .new_types = "TT"};
  const std::string text = RewriteSlt(*file, updates);
  EXPECT_NE(text.find("----\n7\t8.5\n\nquery TT\nSELECT s FROM t\n----\nx\ny\n\nhalt\n"),
            std::string::npos)
      << text;
  EXPECT_EQ(text.find("3\t4.5"), std::string::npos);

  auto again = ParseSlt("f.slt", text);
  ASSERT_TRUE(again.has_value()) << again.error();
  EXPECT_EQ(again->records[4].types, "TT");
  EXPECT_EQ(again->records[4].expected, (std::vector<std::string>{"x", "y"}));
  EXPECT_EQ(RewriteSlt(*again, updates), text);  // idempotent
}

}  // namespace
}  // namespace antb1::slt
