#include "query_file.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "engine.h"
#include "supported_features.h"

namespace antb1::slt {
namespace {

class ScriptedEngine final : public Engine {
 public:
  ScriptedEngine(std::string name, std::function<ExecResult(const std::string&)> answer)
      : name_(std::move(name)), answer_(std::move(answer)) {}

  [[nodiscard]] std::string_view name() const override { return name_; }
  ExecResult Execute(const std::string& sql) override {
    ++calls;
    return answer_(sql);
  }

  int calls = 0;

 private:
  std::string name_;
  std::function<ExecResult(const std::string&)> answer_;
};

ResultSet Int(std::string value) {
  return ResultSet{
      .classes = {ColumnClass::kInteger}, .type_names = {"BIGINT"}, .rows = {{std::move(value)}}};
}

ExecResult Error(const std::string& kind, bool unsupported = false, bool internal = false) {
  return std::unexpected(EngineError{.kind = kind,
                                     .message = kind + ": SECRET_MESSAGE",
                                     .unsupported = unsupported,
                                     .internal = internal});
}

TEST(ParseSqlFile, StatementsFeaturesAndLines) {
  const std::string text =
      "-- a comment\n"
      "\n"
      "-- features: count_star, table_name\n"
      "SELECT COUNT(*) FROM t;\n"
      "\n"
      "select count(*)\n"
      "from T -- rows\n"
      "  ;  \n"
      "-- features: sum integer_columns\n"
      "SELECT SUM(c) FROM t;\n";
  const auto statements = ParseSqlFile("f.sql", text);
  ASSERT_TRUE(statements.has_value()) << statements.error();
  ASSERT_EQ(statements->size(), 3U);
  EXPECT_EQ((*statements)[0].line, 4);
  EXPECT_EQ((*statements)[0].sql, "SELECT COUNT(*) FROM t");
  EXPECT_EQ((*statements)[0].features,
            std::optional(FeatureSet{Feature::kCountStar, Feature::kTableName}));
  EXPECT_EQ((*statements)[1].line, 6);
  EXPECT_EQ((*statements)[1].sql, "select count(*)\nfrom T -- rows");
  EXPECT_FALSE((*statements)[1].features.has_value());
  EXPECT_EQ((*statements)[2].line, 10);
  EXPECT_EQ((*statements)[2].features,
            std::optional(FeatureSet{Feature::kSum, Feature::kIntegerColumns}));
}

TEST(ParseSqlFile, Errors) {
  EXPECT_EQ(ParseSqlFile("f.sql", "SELECT 1;\nSELECT 2\n").error(),
            "f.sql:2: the query does not end with ';'");
  EXPECT_EQ(ParseSqlFile("f.sql", "-- features: nope\nSELECT 1;\n").error(),
            "f.sql:1: unknown feature 'nope' (the names are FeatureName() in "
            "tests/slt/supported_features.h)");
  EXPECT_EQ(ParseSqlFile("f.sql", "-- features:\nSELECT 1;\n").error(),
            "f.sql:1: `-- features:` lists no feature");
  EXPECT_EQ(ParseSqlFile("f.sql", "SELECT 1;\n-- features: sum\n").error(),
            "f.sql:2: `-- features:` is not followed by a query");
  EXPECT_EQ(ParseSqlFile("f.sql", "-- features: sum\n-- features: avg\nSELECT 1;\n").error(),
            "f.sql:2: a second `-- features:` line before a query");
}

TEST(FeatureByName, EveryFeatureRoundTrips) {
  for (std::size_t i = 0; i < kFeatureCount; ++i) {
    const auto f = static_cast<Feature>(i);
    EXPECT_EQ(FeatureByName(FeatureName(f)), std::optional(f));
  }
  EXPECT_EQ(FeatureByName("no_such_feature"), std::nullopt);
}

struct FileRun {
  QueryFileStats stats;
  std::string out;
};

FileRun RunOne(FeatureSet features, std::function<ExecResult(const std::string&)> antb1,
               std::function<ExecResult(const std::string&)> oracle, bool redact = false) {
  ScriptedEngine a("antb1", std::move(antb1));
  ScriptedEngine o("duckdb", std::move(oracle));
  const std::vector<Statement> statements = {
      {.line = 7, .sql = "SELECT SECRET_SQL FROM t", .features = features}};
  FileRun run;
  run.stats =
      RunQueryFile("q.sql", statements, {Feature::kCountStar, Feature::kTableName}, a, o,
                   {.redact = redact, .only_line = std::nullopt, .command = "cmd"}, run.out);
  return run;
}

constexpr FeatureSet kSupported = {Feature::kCountStar, Feature::kTableName};
constexpr FeatureSet kPendingFeatures = {Feature::kSum, Feature::kTableName};

TEST(RunQueryFile, SupportedQueriesMustEqualDuckDb) {
  const auto oracle = [](const std::string&) { return ExecResult(Int("5")); };
  const FileRun ok = RunOne(kSupported, oracle, oracle);
  EXPECT_EQ(ok.stats.compared, 1);
  EXPECT_EQ(ok.stats.failed, 0);
  EXPECT_TRUE(ok.out.ends_with(
      "QUERIES: PASS file=q.sql queries=1 compared=1 pending=0 rejected=0 failed=0\n"))
      << ok.out;

  const FileRun wrong =
      RunOne(kSupported, [](const std::string&) { return ExecResult(Int("6")); }, oracle);
  EXPECT_EQ(wrong.stats.failed, 1);
  EXPECT_TRUE(wrong.out.contains("FAIL q.sql:7: result mismatch")) << wrong.out;
  EXPECT_TRUE(wrong.out.contains("SECRET_SQL")) << "unredacted reports show the SQL";
  EXPECT_TRUE(wrong.out.contains("cmd --only 7")) << wrong.out;

  const FileRun unsupported =
      RunOne(kSupported, [](const std::string&) { return Error("unsupported", true); }, oracle);
  EXPECT_EQ(unsupported.stats.failed, 1);
  EXPECT_TRUE(unsupported.out.contains("antb1 reports Unsupported")) << unsupported.out;
}

TEST(RunQueryFile, PendingQueriesNeedUnsupportedOrARejection) {
  const auto oracle = [](const std::string&) { return ExecResult(Int("5")); };
  const FileRun pending = RunOne(
      kPendingFeatures, [](const std::string&) { return Error("unsupported", true); }, oracle);
  EXPECT_EQ(pending.stats.pending, 1);
  EXPECT_EQ(pending.stats.failed, 0);
  const FileRun rejected =
      RunOne(kPendingFeatures, [](const std::string&) { return Error("parse"); }, oracle);
  EXPECT_EQ(rejected.stats.rejected, 1);
  EXPECT_EQ(rejected.stats.failed, 0);
  for (const std::string kind : {"io", "execution"}) {
    const FileRun unclean =
        RunOne(kPendingFeatures, [&kind](const std::string&) { return Error(kind); }, oracle);
    EXPECT_EQ(unclean.stats.failed, 1) << kind << " errors are not clean rejections";
    EXPECT_TRUE(unclean.out.contains("a pending query must fail cleanly")) << unclean.out;
  }
  const FileRun internal = RunOne(
      kPendingFeatures, [](const std::string&) { return Error("internal", false, true); }, oracle);
  EXPECT_EQ(internal.stats.failed, 1) << "an internal error is always a failure";
  const FileRun answered = RunOne(kPendingFeatures, oracle, oracle);
  EXPECT_EQ(answered.stats.failed, 1);
  EXPECT_TRUE(answered.out.contains("Add them to kSupportedFeatures")) << answered.out;
  const FileRun duckdb_error = RunOne(
      kPendingFeatures, [](const std::string&) { return Error("unsupported", true); },
      [](const std::string&) { return Error("Parser Error"); });
  EXPECT_EQ(duckdb_error.stats.failed, 1) << "DuckDB must accept every query of the file";
}

TEST(RunQueryFile, RedactedReportsHoldNoSqlValuesOrMessages) {
  const FileRun run = RunOne(
      kSupported, [](const std::string&) { return ExecResult(Int("SECRET_VALUE_A")); },
      [](const std::string&) { return ExecResult(Int("SECRET_VALUE_B")); }, /*redact=*/true);
  EXPECT_EQ(run.stats.failed, 1);
  EXPECT_TRUE(run.out.contains("sha256: DuckDB")) << run.out;
  EXPECT_FALSE(run.out.contains("SECRET")) << run.out;
  const FileRun error = RunOne(
      kSupported, [](const std::string&) { return Error("execution"); },
      [](const std::string&) { return ExecResult(Int("1")); }, /*redact=*/true);
  EXPECT_TRUE(error.out.contains("error kind: execution")) << error.out;
  EXPECT_FALSE(error.out.contains("SECRET")) << error.out;
}

TEST(RunQueryFile, OnlySelectsALine) {
  ScriptedEngine a("antb1", [](const std::string&) { return ExecResult(Int("1")); });
  ScriptedEngine o("duckdb", [](const std::string&) { return ExecResult(Int("1")); });
  const std::vector<Statement> statements = {
      {.line = 3, .sql = "SELECT COUNT(*) FROM t", .features = kSupported},
      {.line = 9, .sql = "SELECT COUNT(*) FROM u", .features = kSupported}};
  std::string out;
  const auto stats = RunQueryFile("q.sql", statements, kSupported, a, o,
                                  {.redact = false, .only_line = 9, .command = "cmd"}, out);
  EXPECT_EQ(stats.queries, 1);
  EXPECT_EQ(a.calls, 1);
  std::string missing;
  const auto none = RunQueryFile("q.sql", statements, kSupported, a, o,
                                 {.redact = false, .only_line = 4, .command = "cmd"}, missing);
  EXPECT_EQ(none.failed, 1);
  EXPECT_TRUE(missing.contains("no query starts on line 4")) << missing;
}

}  // namespace
}  // namespace antb1::slt
