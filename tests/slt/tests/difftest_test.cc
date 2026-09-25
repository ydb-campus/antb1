#include "difftest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "engine.h"
#include "query_gen.h"
#include "supported_features.h"

namespace antb1::slt {
namespace {

class ScriptedEngine final : public Engine {
 public:
  ScriptedEngine(std::string name, std::function<ExecResult(const std::string&)> answer)
      : name_(std::move(name)), answer_(std::move(answer)) {}

  [[nodiscard]] std::string_view name() const override { return name_; }
  ExecResult Execute(const std::string& sql) override { return answer_(sql); }

 private:
  std::string name_;
  std::function<ExecResult(const std::string&)> answer_;
};

ResultSet Ints(std::vector<std::string> values) {
  ResultSet r{.classes = {ColumnClass::kInteger}, .type_names = {"BIGINT"}, .rows = {}};
  for (auto& v : values) {
    r.rows.push_back({std::move(v)});
  }
  return r;
}

bool UsesOnlyCountStar(const std::string& sql) {
  std::string lower = sql;
  std::ranges::transform(lower, lower.begin(), [](char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  });
  return lower.contains("count") && lower.contains('*') && !lower.contains("where") &&
         !lower.contains(',') && !lower.contains("limit") && !lower.contains(" a1");
}

ExecResult Unsupported() {
  return std::unexpected(
      EngineError{.kind = "unsupported", .message = "unsupported: nope", .unsupported = true});
}

// The oracle answers every query with 12; the "good" antb1 answers COUNT(*)-only queries likewise.
ExecResult Oracle(const std::string& /*sql*/) { return Ints({"12"}); }
ExecResult GoodAntb1(const std::string& sql) {
  return UsesOnlyCountStar(sql) ? ExecResult(Ints({"12"})) : Unsupported();
}

std::vector<GenTable> Tables() {
  GenTable t{.name = "t", .path = "/data/t.parquet", .rows = 12, .columns = {}};
  t.columns = {{.name = "c", .kind = ValueKind::kInteger, .min = 0, .max = 100, .samples = {"42"}}};
  return {t};
}

QueryGenerator Generator(unsigned target_percent) {
  auto gen = QueryGenerator::Make(
      Tables(), 42, {.supported = kSupportedFeatures, .target_percent = target_percent});
  EXPECT_TRUE(gen.has_value()) << gen.error();
  return *std::move(gen);
}

struct DiffRun {
  DiffStats stats;
  std::string out;
};

DiffRun Diff(const QueryGenerator& gen, const std::function<ExecResult(const std::string&)>& antb1,
             const std::function<ExecResult(const std::string&)>& oracle,
             const DiffOptions& options) {
  ScriptedEngine a("antb1", antb1);
  ScriptedEngine o("duckdb", oracle);
  DiffRun r;
  r.stats = RunDiff(gen, a, o, options, r.out);
  return r;
}

TEST(RunDiff, AgreeingEnginesPassAndUnsupportedTargetQueriesAreCounted) {
  const auto gen = Generator(40);
  const DiffRun r = Diff(gen, GoodAntb1, Oracle, {.count = 300});
  EXPECT_EQ(r.stats.failed, 0U) << r.out;
  EXPECT_EQ(r.stats.queries, 300U);
  EXPECT_EQ(r.stats.compared + r.stats.unsupported, 300U);
  EXPECT_GT(r.stats.target, 0U);
  EXPECT_GT(r.stats.unsupported, 0U);
  EXPECT_EQ(r.stats.compared, r.stats.supported);
  for (std::size_t f = 0; f < kFeatureCount; ++f) {
    if (kSupportedFeatures.Has(static_cast<Feature>(f))) {
      EXPECT_EQ(r.stats.unsupported_by_feature[f], 0U) << FeatureName(static_cast<Feature>(f));
    }
  }
  EXPECT_NE(r.out.find("DIFF: PASS seed=42 queries=300 failed=0"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("Unsupported answers (not failures) by feature"), std::string::npos);
}

TEST(RunDiff, MismatchPrintsSeedCaseSqlRowsAndRepro) {
  const auto gen = Generator(0);
  const DiffRun r = Diff(gen, [](const std::string&) { return ExecResult(Ints({"13"})); }, Oracle,
                         {.count = 5, .command = "antb1-slt diff --seed 42", .max_reports = 1});
  EXPECT_EQ(r.stats.failed, 5U);
  EXPECT_NE(r.out.find("FAIL diff case 0 (seed 42, supported features): result mismatch"),
            std::string::npos)
      << r.out;
  EXPECT_NE(r.out.find("  SQL:\n    "), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("row 0: DuckDB 12\n"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("antb1  13\n"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("ANTB1_DIFF_SEED=42 ANTB1_DIFF_ONLY=0 pixi run diff-random"),
            std::string::npos);
  EXPECT_NE(r.out.find("antb1-slt diff --seed 42 --only 0"), std::string::npos);
  EXPECT_NE(r.out.find("... 4 more failure(s) not shown"), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("DIFF: FAIL"), std::string::npos);
}

TEST(RunDiff, EngineTypesMustMatchNotOnlyTheColumnClass) {
  const auto gen = Generator(0);
  const DiffRun r = Diff(gen,
                         [](const std::string&) {
                           ResultSet narrower = Ints({"12"});
                           narrower.type_names = {"INTEGER"};
                           return ExecResult(std::move(narrower));
                         },
                         Oracle, {.count = 3});
  EXPECT_EQ(r.stats.failed, 3U) << r.out;
  EXPECT_NE(r.out.find("column types differ: DuckDB I (BIGINT), antb1 I (INTEGER)"),
            std::string::npos)
      << r.out;
}

TEST(RunDiff, RedactedReportsPrintNoSqlAndNoValues) {
  const auto gen = Generator(0);
  const DiffRun r = Diff(gen, [](const std::string&) { return ExecResult(Ints({"987654"})); },
                         Oracle, {.count = 3, .redact = true});
  EXPECT_EQ(r.stats.failed, 3U);
  EXPECT_NE(r.out.find("sha256: DuckDB "), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("rows: DuckDB 1, antb1 1; first differing row: 0"), std::string::npos);
  for (const std::string_view secret : {"987654", "12\n", "SQL:", "count(", "COUNT(", "/data/"}) {
    EXPECT_EQ(r.out.find(secret), std::string::npos) << secret << " in\n" << r.out;
  }
}

TEST(RunDiff, ErrorsAndUnsupportedAnswers) {
  const auto gen = Generator(40);
  // Unsupported everywhere: a failure exactly for the queries that use only supported features.
  DiffRun r = Diff(gen, [](const std::string&) { return Unsupported(); }, Oracle, {.count = 200});
  EXPECT_EQ(r.stats.failed, r.stats.supported);
  EXPECT_EQ(r.stats.unsupported, 200U - r.stats.supported);
  EXPECT_NE(r.out.find("antb1 reports Unsupported for a query that uses only supported features"),
            std::string::npos);
  // Another query error outside the supported set is counted as rejected, never as a pass.
  const auto parse_error = [](const std::string& sql) {
    return UsesOnlyCountStar(sql) ? ExecResult(Ints({"12"}))
                                  : ExecResult(std::unexpected(EngineError{
                                        .kind = "parse", .message = "parse: expected FROM"}));
  };
  r = Diff(gen, parse_error, Oracle, {.count = 200});
  EXPECT_EQ(r.stats.failed, 0U) << r.out;
  EXPECT_GT(r.stats.rejected, 0U);
  EXPECT_NE(r.out.find("NOTE: antb1 rejected"), std::string::npos) << r.out;
  // Internal errors always fail; an oracle error is a generator bug.
  r = Diff(gen,
           [](const std::string&) {
             return ExecResult(std::unexpected(
                 EngineError{.kind = "internal", .message = "internal: boom", .internal = true}));
           },
           Oracle, {.count = 50});
  EXPECT_EQ(r.stats.failed, 50U);
  r = Diff(gen, GoodAntb1,
           [](const std::string&) {
             return ExecResult(std::unexpected(
                 EngineError{.kind = "Parser Error", .message = "Parser Error: x"}));
           },
           {.count = 50});
  EXPECT_EQ(r.stats.failed, 50U);
  EXPECT_NE(r.out.find("DuckDB rejects the generated SQL"), std::string::npos);
}

TEST(RunDiff, OnlyRunsOneCaseAndProjectionsCompareAtMostFiveRows) {
  const FeatureSet projections = {Feature::kColumns, Feature::kIntegerColumns, Feature::kTableName};
  auto gen = QueryGenerator::Make(Tables(), 9, {.supported = projections, .target_percent = 0});
  ASSERT_TRUE(gen.has_value()) << gen.error();
  const auto rows = [](int first) {
    return [first](const std::string&) {
      std::vector<std::string> v;
      v.reserve(9);
      for (int i = 0; i < 9; ++i) {
        v.push_back(std::to_string(first + i));
      }
      return ExecResult(Ints(std::move(v)));
    };
  };
  const DiffRun r = Diff(*gen, rows(100), rows(200), {.count = 100, .only = 7});
  EXPECT_EQ(r.stats.queries, 1U);
  EXPECT_EQ(r.stats.failed, 1U);
  EXPECT_NE(r.out.find("FAIL diff case 7 "), std::string::npos) << r.out;
  EXPECT_NE(r.out.find("row 4:"), std::string::npos) << r.out;
  EXPECT_EQ(r.out.find("row 5:"), std::string::npos) << "at most 5 differing rows\n" << r.out;
  // rowsort: the same rows in another order are equal.
  const auto reversed = [](const std::string&) { return ExecResult(Ints({"3", "2", "1"})); };
  const auto ordered = [](const std::string&) { return ExecResult(Ints({"1", "2", "3"})); };
  EXPECT_EQ(Diff(*gen, reversed, ordered, {.count = 20}).stats.failed, 0U);
}

}  // namespace
}  // namespace antb1::slt
