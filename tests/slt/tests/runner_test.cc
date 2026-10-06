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

// One DECIMAL column (DECIMAL(12,2) by default) or another class with its type name.
ResultSet Decimals(std::vector<Row> rows, ColumnClass cls = ColumnClass::kDecimal,
                   std::string type = "DECIMAL(12,2)") {
  return ResultSet{.classes = {cls}, .type_names = {std::move(type)}, .rows = std::move(rows)};
}

EngineError Unsupported() {
  return EngineError{.kind = "unsupported", .message = "unsupported: OVER", .unsupported = true};
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
  engine.Answer("SELECT row_number() OVER () FROM t", std::unexpected(Unsupported()));
  const auto o = RunText(
      "statement error\nSELECT row_number() OVER () FROM t\n\nquery I\nSELECT row_number() OVER () "
      "FROM t\n----\n1\n",
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

// A pending record between two records that antb1 answers.
constexpr std::string_view kPendingFile =
    "query I nosort\nSELECT COUNT(*) FROM t\n----\n31337\n\n"
    "pending J2b\nquery I nosort\nSELECT COUNT(*) FROM t, u WHERE t.k = u.k\n----\n7\n\n"
    "statement ok\nSELECT COUNT(*) FROM t\n";

TEST(RunFile, Antb1SkipsPendingRecordsAndDuckDbRunsThem) {
  FakeEngine antb1("antb1");
  antb1.Answer("SELECT COUNT(*) FROM t", Ints({{"31337"}}));
  const auto a = RunText(kPendingFile, antb1);
  EXPECT_EQ(a.stats.records, 2);
  EXPECT_EQ(a.stats.passed, 2) << a.out;
  EXPECT_EQ(a.stats.skipped, 1);
  EXPECT_EQ(antb1.calls(), 2);
  FakeEngine duckdb("duckdb");
  duckdb.Answer("SELECT COUNT(*) FROM t", Ints({{"31337"}}));
  duckdb.Answer("SELECT COUNT(*) FROM t, u WHERE t.k = u.k", Ints({{"7"}}));
  const auto d = RunText(kPendingFile, duckdb);
  EXPECT_EQ(d.stats.records, 3);
  EXPECT_EQ(d.stats.passed, 3) << d.out;
  EXPECT_EQ(d.stats.skipped, 0);
}

PendingStats CheckPendingText(std::string_view text, Engine& engine, std::string& out,
                              bool redact = false) {
  return CheckPendingFile(Parse(text), engine,
                          RunOptions{.redact = redact, .repro = "    repro-command\n"}, out);
}

TEST(CheckPendingFile, PassesWhileEveryPendingRecordIsUnsupported) {
  FakeEngine antb1("antb1");
  antb1.Answer("SELECT COUNT(*) FROM t, u WHERE t.k = u.k", std::unexpected(Unsupported()));
  antb1.Answer("SELECT COUNT(*) FROM t AS semi", std::unexpected(Unsupported()));
  std::string out;
  const auto stats = CheckPendingText(
      std::string(kPendingFile) + "\npending S3\nstatement error\nSELECT COUNT(*) FROM t AS semi\n",
      antb1, out);
  EXPECT_EQ(stats.records, 2);
  EXPECT_EQ(stats.unsupported, 2);
  EXPECT_EQ(stats.failed, 0) << out;
  EXPECT_EQ(stats.by_id, (std::map<std::string, int, std::less<>>{{"J2b", 1}, {"S3", 1}}));
  EXPECT_EQ(antb1.calls(), 2);  // only the pending records run
  EXPECT_NE(out.find("t.slt: pending records=2 unsupported=2 failed=0 (J2b 1, S3 1)"),
            std::string::npos)
      << out;
}

TEST(CheckPendingFile, AnyOtherAnswerSaysRemoveTheGuard) {
  const std::string file = std::string(kPendingFile) +
                           "\npending J5\nstatement error\nSELECT nope\n\n"
                           "pending U2\nquery I nosort\nSELECT 1\n----\n1\n";
  FakeEngine antb1("antb1");
  antb1.Answer("SELECT COUNT(*) FROM t, u WHERE t.k = u.k", Ints({{"7"}, {"8"}}));
  antb1.Answer("SELECT 1", std::unexpected(EngineError{.kind = "internal",
                                                       .message = "internal: boom",
                                                       .unsupported = false,
                                                       .internal = true}));
  std::string out;
  const auto stats = CheckPendingText(file, antb1, out);  // SELECT nope: a bind error
  EXPECT_EQ(stats.records, 3);
  EXPECT_EQ(stats.unsupported, 0);
  EXPECT_EQ(stats.failed, 3);
  EXPECT_NE(out.find("FAIL t.slt:7: query I nosort [antb1]: remove the guard (J2b): antb1 answers "
                     "it now\n  antb1 answers with 2 rows."),
            std::string::npos)
      << out;
  EXPECT_NE(
      out.find("FAIL t.slt:16: statement error [antb1]: remove the guard (J5): antb1 fails "
               "with a bind error now, not Unsupported\n  bind: unknown statement SELECT nope"),
      std::string::npos)
      << out;
  EXPECT_NE(out.find("remove the guard (U2): antb1 fails with an internal error now"),
            std::string::npos)
      << out;
  EXPECT_NE(out.find("SELECT COUNT(*) FROM t, u WHERE t.k = u.k"), std::string::npos) << out;
  EXPECT_NE(out.find("repro-command"), std::string::npos) << out;
  EXPECT_NE(out.find("pending records=3 unsupported=0 failed=3 (J2b 1, J5 1, U2 1)"),
            std::string::npos)
      << out;
}

TEST(CheckPendingFile, RedactedReportPrintsNoSqlAndNoMessage) {
  FakeEngine antb1("antb1");
  antb1.Answer("SELECT COUNT(*) FROM t, u WHERE t.k = u.k", Ints({{"31338"}}));
  antb1.Answer("SELECT secret FROM t",
               std::unexpected(EngineError{.kind = "bind", .message = "bind: no column secret"}));
  std::string out;
  const auto stats = CheckPendingText(
      std::string(kPendingFile) + "\npending J5\nstatement error\nSELECT secret FROM t\n", antb1,
      out, /*redact=*/true);
  EXPECT_EQ(stats.failed, 2);
  EXPECT_NE(out.find("remove the guard (J2b): antb1 answers it now\n  rows: 1\n"),
            std::string::npos)
      << out;
  EXPECT_NE(out.find("error kind: bind"), std::string::npos) << out;
  EXPECT_EQ(out.find("SELECT"), std::string::npos) << out;
  EXPECT_EQ(out.find("secret"), std::string::npos) << out;
  EXPECT_EQ(out.find("3133"), std::string::npos) << out;
}

TEST(CheckPendingFile, StopsAtAHaltForAntb1) {
  FakeEngine antb1("antb1");
  antb1.Answer("SELECT 1", std::unexpected(Unsupported()));
  std::string out;
  const auto stats = CheckPendingText(
      "pending J2b\nstatement ok\nSELECT 1\n\nonlyif duckdb\nhalt\n\n"
      "pending J2b\nstatement ok\nSELECT 2\n\nhalt\n\npending J2b\nstatement ok\nSELECT 3\n",
      antb1, out);
  EXPECT_EQ(stats.records, 2) << out;  // SELECT 2 gets a bind error: a failure
  EXPECT_EQ(stats.failed, 1);
  EXPECT_TRUE(stats.halted);
  EXPECT_NE(out.find("t.slt:12: halt: the remaining pending records are not run on antb1"),
            std::string::npos)
      << out;
}

TEST(CompleteFile, WritesPendingRecordsFromTheOracle) {
  FakeEngine oracle("duckdb");
  FakeEngine antb1("antb1");
  oracle.Answer("SELECT COUNT(*) FROM t, u WHERE t.k = u.k", Ints({{"9"}}));
  const auto file =
      Parse("pending J2b\nquery I nosort\nSELECT COUNT(*) FROM t, u WHERE t.k = u.k\n");
  std::string text;
  std::string out;
  const auto stats = CompleteFile(file, oracle, antb1, text, out);
  EXPECT_EQ(stats.queries, 1);
  EXPECT_EQ(stats.from_antb1, 0);
  EXPECT_EQ(antb1.calls(), 0);
  EXPECT_EQ(text,
            "pending J2b\nquery I nosort\nSELECT COUNT(*) FROM t, u WHERE t.k = u.k\n----\n9\n");
}

TEST(RunFile, EveryMutationIsCaught) {
  // On an integer record and on a DECIMAL one, where a value mutation (an appended digit) only
  // changes the scale.
  const std::vector<std::pair<std::string, ResultSet>> queries = {
      {"query I nosort\nSELECT COUNT(*) FROM t\n----\n31337\n\n", Ints({{"31337"}})},
      {"query D nosort\nSELECT COUNT(*) FROM t\n----\n1234567890.12\n\n",
       Decimals({{"1234567890.12"}})}};
  for (const auto& [query, answer] : queries) {
    const std::string file =
        query + "statement ok\nSELECT COUNT(*) FROM t\n\nstatement error\nSELECT nope\n";
    for (const auto* name : {"value", "null", "drop-row", "extra-row", "extra-column", "error",
                             "unsupported", "succeed", "canary"}) {
      FakeEngine engine("antb1");
      engine.Answer("SELECT COUNT(*) FROM t", answer);
      EXPECT_EQ(RunText(file, engine).stats.failed, 0);
      const auto mutation = ParseMutation(name);
      ASSERT_TRUE(mutation.has_value()) << name;
      auto mutating = MakeMutatingEngine(engine, mutation.value_or(Mutation::kNone));
      const auto o = RunText(file, *mutating);
      EXPECT_GT(o.stats.failed, 0) << name << "\n" << o.out;
    }
  }
  EXPECT_FALSE(ParseMutation("sideways").has_value());
}

// A sum of money with ten integer digits.
constexpr std::string_view kTotal =
    "query D nosort\n"
    "SELECT total FROM t\n"
    "----\n"
    "1234567890.12\n";

TEST(RunFile, DecimalValuesCompareExactly) {
  // A wrong last digit, a wrong scale and a missing leading zero all fail.
  for (const std::string_view wrong : {"1234567890.13", "1234567890.120", "1234567890.1"}) {
    FakeEngine engine("antb1");
    engine.Answer("SELECT total FROM t", Decimals({{std::string(wrong)}}));
    const auto o = RunText(kTotal, engine);
    EXPECT_EQ(o.stats.failed, 1) << wrong;
    EXPECT_NE(o.out.find("result mismatch: values differ"), std::string::npos) << o.out;
  }
  FakeEngine zero("antb1");
  zero.Answer("SELECT total FROM t", Decimals({{"-.25"}}, ColumnClass::kDecimal, "DECIMAL(3,2)"));
  EXPECT_EQ(RunText("query D nosort\nSELECT total FROM t\n----\n-0.25\n", zero).stats.failed, 1);
  // The same wrong cent passes in an R record: the hole that D closes.
  FakeEngine real("antb1");
  real.Answer("SELECT total FROM t", Decimals({{"1234567890.13"}}, ColumnClass::kReal, "DOUBLE"));
  EXPECT_EQ(
      RunText("query R nosort\nSELECT total FROM t\n----\n1234567890.12\n", real).stats.failed, 0);
  // A DOUBLE answer to a D record fails on its column type.
  const auto o = RunText(kTotal, real);
  EXPECT_NE(o.out.find("column types differ: expected D, got R (DOUBLE)"), std::string::npos)
      << o.out;
}

TEST(RunFile, DecimalResultsAreHashedAboveTheThreshold) {
  // Unlike R results, D results are hashed, and one digit changes the hash.
  const auto right = Decimals({{"1234567890.12"}, {"-0.25"}});
  const auto block = RenderBlock(right, SortMode::kRowSort, 1);
  ASSERT_EQ(block.size(), 1U);
  EXPECT_TRUE(block[0].starts_with("2 values hashing to ")) << block[0];
  auto file = Parse("hash-threshold 1\n\nquery D rowsort\nSELECT total FROM t\n----\n1\n");
  file.records[1].expected = block;
  FakeEngine engine("antb1");
  engine.Answer("SELECT total FROM t", right);
  std::string out;
  EXPECT_EQ(RunFile(file, engine, RunOptions{}, out).failed, 0) << out;
  engine.Answer("SELECT total FROM t", Decimals({{"1234567890.13"}, {"-0.25"}}));
  out.clear();
  EXPECT_EQ(RunFile(file, engine, RunOptions{}, out).failed, 1);
  EXPECT_NE(out.find("hashed results differ"), std::string::npos) << out;
}

TEST(CompleteFile, WritesDecimalRecordsWithTheirLetterAndText) {
  FakeEngine oracle("duckdb");
  FakeEngine antb1("antb1");
  oracle.Answer("SELECT total FROM t", Decimals({{"17.00"}, {"-0.25"}}));
  const auto file = Parse("query R rowsort\nSELECT total FROM t\n----\n17\n-0.25\n");
  std::string text;
  std::string out;
  CompleteFile(file, oracle, antb1, text, out);
  EXPECT_EQ(text, "query D rowsort\nSELECT total FROM t\n----\n-0.25\n17.00\n");
  EXPECT_NE(out.find("NOTE t.slt:1: column types R -> D"), std::string::npos) << out;
}

TEST(CompleteFile, KeepsValuesortRecordsThatWouldGetRealAndDecimalColumns) {
  // valuesort cannot check a DECIMAL next to a DOUBLE (SortModeProblem): rewritten as
  // `query RD valuesort`, the record would no longer parse, so it is an error and stays as it is.
  FakeEngine oracle("duckdb");
  FakeEngine antb1("antb1");
  oracle.Answer("SELECT AVG(d), 0.5 FROM t",
                ResultSet{.classes = {ColumnClass::kReal, ColumnClass::kDecimal},
                          .type_names = {"DOUBLE", "DECIMAL(2,1)"},
                          .rows = {{"1.5", "0.5"}}});
  const std::string text = "query RR valuesort\nSELECT AVG(d), 0.5 FROM t\n----\n0.5\n1.5\n";
  std::string new_text;
  std::string out;
  const auto stats = CompleteFile(Parse(text), oracle, antb1, new_text, out);
  EXPECT_EQ(stats.errors, 1);
  EXPECT_EQ(stats.queries, 0);
  EXPECT_EQ(new_text, text);
  EXPECT_NE(out.find("ERROR t.slt:1: query RR valuesort [duckdb]: valuesort compares every value "
                     "within the R tolerance"),
            std::string::npos)
      << out;
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
