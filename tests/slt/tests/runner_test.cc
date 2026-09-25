#include "runner.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "canonical.h"
#include "engine.h"
#include "slt_file.h"

namespace antb1::slt {
namespace {

using Row = std::vector<std::optional<std::string>>;

class FakeEngine final : public Engine {
 public:
  explicit FakeEngine(std::string name) : name_(std::move(name)) {}

  [[nodiscard]] std::string_view name() const override { return name_; }
  ExecResult Execute(const std::string& sql) override {
    ++calls_;
    const auto it = answers_.find(sql);
    if (it == answers_.end()) {
      return std::unexpected(
          EngineError{.kind = "bind", .message = "bind: unknown statement " + sql});
    }
    return it->second;
  }

  void Answer(const std::string& sql, ExecResult result) {
    answers_.insert_or_assign(sql, std::move(result));
  }
  [[nodiscard]] int calls() const { return calls_; }

 private:
  std::string name_;
  std::map<std::string, ExecResult> answers_;
  int calls_ = 0;
};

ResultSet Ints(std::vector<Row> rows) {
  return ResultSet{
      .classes = {ColumnClass::kInteger}, .type_names = {"BIGINT"}, .rows = std::move(rows)};
}

EngineError Unsupported() {
  return EngineError{
      .kind = "unsupported", .message = "unsupported: GROUP BY", .unsupported = true};
}

SltFile Parse(std::string_view text) {
  auto file = ParseSlt("t.slt", text);
  EXPECT_TRUE(file.has_value()) << file.error();
  return file.value_or(SltFile{});
}

struct Outcome {
  RunStats stats;
  std::string out;
};

Outcome RunText(std::string_view text, Engine& engine, bool redact = false) {
  Outcome o;
  o.stats = RunFile(Parse(text), engine,
                    RunOptions{.redact = redact, .repro = "    repro-command\n"}, o.out);
  return o;
}

constexpr std::string_view kCount =
    "query I nosort\n"
    "SELECT COUNT(*) FROM t\n"
    "----\n"
    "31337\n";

TEST(RunFile, PassingQueryAndStatements) {
  FakeEngine engine("antb1");
  engine.Answer("SELECT COUNT(*) FROM t", Ints({{"31337"}}));
  const auto o =
      RunText(std::string(kCount) +
                  "\nstatement ok\nSELECT COUNT(*) FROM t\n\nstatement error\nSELECT nope\n"
                  "\nstatement error ^bind: unknown\nSELECT nope\n",
              engine);
  EXPECT_EQ(o.stats.records, 4);
  EXPECT_EQ(o.stats.passed, 4) << o.out;
  EXPECT_EQ(o.stats.failed, 0);
  EXPECT_NE(o.out.find("records=4 passed=4 failed=0 skipped=0 unsupported=0"), std::string::npos)
      << o.out;
}

TEST(RunFile, MismatchPrintsSqlValuesAndRepro) {
  FakeEngine engine("antb1");
  engine.Answer("SELECT COUNT(*) FROM t", Ints({{"31338"}}));
  const auto o = RunText(kCount, engine);
  EXPECT_EQ(o.stats.failed, 1);
  EXPECT_NE(o.out.find("FAIL t.slt:1: query I nosort [antb1]: result mismatch: values differ"),
            std::string::npos)
      << o.out;
  EXPECT_NE(o.out.find("SELECT COUNT(*) FROM t"), std::string::npos);
  EXPECT_NE(o.out.find("31337"), std::string::npos);
  EXPECT_NE(o.out.find("31338"), std::string::npos);
  EXPECT_NE(o.out.find("repro-command"), std::string::npos);
}

TEST(RunFile, RedactedMismatchPrintsNoValuesAndNoSql) {
  FakeEngine engine("antb1");
  engine.Answer("SELECT COUNT(*) FROM t", Ints({{"31338"}}));
  const auto o = RunText(kCount, engine, /*redact=*/true);
  EXPECT_EQ(o.stats.failed, 1);
  EXPECT_NE(o.out.find("FAIL t.slt:1: query I nosort [antb1]: result mismatch"), std::string::npos)
      << o.out;
  EXPECT_NE(o.out.find("rows: expected 1, actual 1; first differing row: 0"), std::string::npos)
      << o.out;
  EXPECT_NE(o.out.find("sha256: expected "), std::string::npos);
  EXPECT_EQ(o.out.find("3133"), std::string::npos) << o.out;
  EXPECT_EQ(o.out.find("SELECT"), std::string::npos) << o.out;
}

TEST(RunFile, RedactedErrorsPrintOnlyTheKind) {
  FakeEngine engine("antb1");
  engine.Answer(
      "SELECT secret_column FROM t",
      std::unexpected(EngineError{.kind = "bind", .message = "bind: no column secret_column"}));
  const auto o = RunText("statement ok\nSELECT secret_column FROM t\n", engine, /*redact=*/true);
  EXPECT_EQ(o.stats.failed, 1);
  EXPECT_NE(o.out.find("error kind: bind"), std::string::npos) << o.out;
  EXPECT_EQ(o.out.find("secret"), std::string::npos) << o.out;
}

TEST(RunFile, UnsupportedIsAFailureEvenForStatementError) {
  FakeEngine engine("antb1");
  engine.Answer("SELECT a FROM t GROUP BY a", std::unexpected(Unsupported()));
  const auto o = RunText(
      "statement error\nSELECT a FROM t GROUP BY a\n\nquery I\nSELECT a FROM t GROUP BY "
      "a\n----\n1\n",
      engine);
  EXPECT_EQ(o.stats.failed, 2);
  EXPECT_EQ(o.stats.unsupported, 2);
  EXPECT_NE(o.out.find("antb1 reports Unsupported"), std::string::npos) << o.out;
  EXPECT_NE(o.out.find("onlyif duckdb"), std::string::npos) << o.out;
}

TEST(RunFile, InternalErrorsAndRegexMismatchesFailStatementError) {
  FakeEngine engine("antb1");
  engine.Answer("SELECT 1", std::unexpected(EngineError{.kind = "internal",
                                                        .message = "internal: boom",
                                                        .unsupported = false,
                                                        .internal = true}));
  engine.Answer("SELECT 2", std::unexpected(EngineError{.kind = "bind", .message = "bind: other"}));
  const auto o =
      RunText("statement error\nSELECT 1\n\nstatement error ^parse:\nSELECT 2\n", engine);
  EXPECT_EQ(o.stats.failed, 2) << o.out;
  EXPECT_NE(o.out.find("the error does not match the expected regex"), std::string::npos) << o.out;
}

TEST(RunFile, ConditionsHaltAndTypeChecks) {
  FakeEngine engine("antb1");
  engine.Answer("SELECT COUNT(*) FROM t", Ints({{"31337"}}));
  const auto o = RunText(
      "onlyif duckdb\nstatement ok\nSELECT 1\n\n"
      "skipif antb1\nstatement ok\nSELECT 1\n\n"
      "query T\nSELECT COUNT(*) FROM t\n----\n31337\n\n"
      "query II\nSELECT COUNT(*) FROM t\n----\n31337\n\n"
      "onlyif antb1\nhalt\n\n"
      "statement ok\nSELECT never\n",
      engine);
  EXPECT_EQ(o.stats.skipped, 2);
  EXPECT_EQ(o.stats.records, 2);
  EXPECT_EQ(o.stats.failed, 2);
  EXPECT_TRUE(o.stats.halted);
  EXPECT_NE(o.out.find("column types differ: expected T, got I (BIGINT)"), std::string::npos)
      << o.out;
  EXPECT_NE(o.out.find("column types differ: expected II, got I"), std::string::npos) << o.out;
  EXPECT_EQ(engine.calls(), 2);
}

TEST(RunFile, HashThresholdAndLabels) {
  FakeEngine engine("antb1");
  engine.Answer("SELECT a FROM t", Ints({{"1"}, {"2"}, {"3"}}));
  engine.Answer("SELECT b FROM t", Ints({{"3"}, {"1"}, {"2"}}));
  engine.Answer("SELECT c FROM t", Ints({{"4"}, {"1"}, {"2"}}));
  RunStats stats;
  std::string out;
  auto file = Parse(
      "hash-threshold 2\n\n"
      "query I rowsort L\nSELECT a FROM t\n----\n3 values hashing to "
      "0000000000000000000000000000000000000000000000000000000000000000\n\n"
      "query I rowsort L\nSELECT b FROM t\n----\n1\n2\n3\n\n"
      "hash-threshold 0\n\n"
      "query I rowsort L\nSELECT c FROM t\n----\n1\n2\n4\n");
  // Fill in the real hash of "1\n2\n3\n" so only the label check of the third query fails.
  file.records[1].expected = RenderBlock(Ints({{"1"}, {"2"}, {"3"}}), SortMode::kRowSort, 2);
  file.records[2].expected = file.records[1].expected;
  stats = RunFile(file, engine, RunOptions{}, out);
  EXPECT_EQ(stats.passed, 2) << out;
  EXPECT_EQ(stats.failed, 1);
  EXPECT_NE(out.find("result differs from the earlier query labelled 'L'"), std::string::npos)
      << out;
}

TEST(RunFile, EveryMutationIsCaught) {
  constexpr std::string_view kFile =
      "query I nosort\nSELECT COUNT(*) FROM t\n----\n31337\n\n"
      "statement ok\nSELECT COUNT(*) FROM t\n\n"
      "statement error\nSELECT nope\n";
  for (const auto* name : {"value", "null", "drop-row", "extra-row", "extra-column", "error",
                           "unsupported", "succeed", "canary"}) {
    FakeEngine engine("antb1");
    engine.Answer("SELECT COUNT(*) FROM t", Ints({{"31337"}}));
    EXPECT_EQ(RunText(kFile, engine).stats.failed, 0);
    const auto mutation = ParseMutation(name);
    ASSERT_TRUE(mutation.has_value()) << name;
    auto mutating = MakeMutatingEngine(engine, mutation.value_or(Mutation::kNone));
    const auto o = RunText(kFile, *mutating);
    EXPECT_GT(o.stats.failed, 0) << name << "\n" << o.out;
  }
  EXPECT_FALSE(ParseMutation("sideways").has_value());
}

TEST(CompleteFile, WritesBlocksFromTheOracleAndFlagsAntb1OnlyRecords) {
  FakeEngine oracle("duckdb");
  FakeEngine antb1("antb1");
  oracle.Answer("SELECT COUNT(*) FROM t", Ints({{"7"}}));
  oracle.Answer("SELECT x FROM t", ResultSet{.classes = {ColumnClass::kText},
                                             .type_names = {"VARCHAR"},
                                             .rows = {{"a\tb"}, {""}}});
  antb1.Answer("SELECT COUNT(*) FROM u", Ints({{"8"}}));
  const auto file = Parse(
      "query I nosort\nSELECT COUNT(*) FROM t\n----\n1\n\n"
      "query I rowsort\nSELECT x FROM t\n\n"
      "onlyif antb1\nquery I nosort\nSELECT COUNT(*) FROM u\n\n"
      "statement error\nSELECT COUNT(*) FROM t\n");
  std::string text;
  std::string out;
  const auto stats = CompleteFile(file, oracle, antb1, text, out);
  EXPECT_EQ(stats.queries, 3);
  EXPECT_EQ(stats.from_antb1, 1);
  EXPECT_EQ(stats.errors, 1);
  EXPECT_EQ(text,
            "query I nosort\nSELECT COUNT(*) FROM t\n----\n7\n\n"
            "query T rowsort\nSELECT x FROM t\n----\n(empty)\na\\tb\n\n"
            "onlyif antb1\nquery I nosort\nSELECT COUNT(*) FROM u\n----\n8\n\n"
            "statement error\nSELECT COUNT(*) FROM t\n");
  EXPECT_NE(out.find("NOTE t.slt:6: column types I -> T"), std::string::npos) << out;
  EXPECT_NE(out.find("REVIEW t.slt:10: onlyif antb1"), std::string::npos) << out;
  EXPECT_NE(out.find("ERROR t.slt:13: statement error [duckdb]: statement succeeded"),
            std::string::npos)
      << out;
}

}  // namespace
}  // namespace antb1::slt
