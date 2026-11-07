#include "difftest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
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

// The model of these tests, independent of what tests/slt/supported_features.h declares today: the
// "good" antb1 answers exactly SELECT COUNT(*) [[AS] alias] FROM t, in any lexical form.
constexpr FeatureSet kCountStarOnly = {
    Feature::kCountStar,        Feature::kAlias,       Feature::kTableName,
    Feature::kTablePath,        Feature::kKeywordCase, Feature::kIdentifierCase,
    Feature::kQuotedIdentifier, Feature::kLayout,      Feature::kSemicolon,
};

bool UsesOnlyCountStar(const std::string& sql) {
  std::string lower = sql;
  std::ranges::transform(lower, lower.begin(), [](char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
  });
  return lower.contains("count") && lower.contains('*') && !lower.contains(',') &&
         std::ranges::none_of(std::to_array<std::string_view>(
                                  {"where", "limit", "offset", "group", "order", "distinct"}),
                              [&](std::string_view word) { return lower.contains(word); });
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
  auto gen = QueryGenerator::Make(Tables(), 42,
                                  {.supported = kCountStarOnly, .target_percent = target_percent});
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

// An assertion inside DuckDB, as DuckDbEngine reports it: a DuckDB bug.
ExecResult InternalError() {
  return std::unexpected(EngineError{.kind = "INTERNAL Error",
                                     .message = "INTERNAL Error: Failed to bind column reference",
                                     .internal = true});
}

// A case DuckDB fails with an internal error asks the retries in order and is compared with the
// first that answers; the next case asks the oracle again. Counted and listed, not a failure.
TEST(RunDiff, AnOracleInternalErrorAsksTheRetriesInOrder) {
  const auto gen = Generator(0);
  int oracle_calls = 0;
  int first_calls = 0;
  int second_calls = 0;
  ScriptedEngine antb1("antb1", GoodAntb1);
  ScriptedEngine oracle("duckdb", [&](const std::string& /*sql*/) {
    ++oracle_calls;
    return InternalError();
  });
  ScriptedEngine first("duckdb", [&](const std::string& /*sql*/) {
    ++first_calls;
    return InternalError();
  });
  ScriptedEngine second("duckdb", [&](const std::string& sql) {
    ++second_calls;
    return Oracle(sql);
  });
  const std::array<Engine*, 2> retries = {&first, &second};
  std::string out;
  const DiffStats stats = RunDiff(gen, antb1, oracle, {.count = 20}, out, retries);
  EXPECT_EQ(stats.failed, 0U) << out;
  EXPECT_EQ(stats.compared, 20U);
  EXPECT_EQ(stats.oracle_retried, 20U);
  EXPECT_EQ(oracle_calls, 20) << "every case asks the oracle first";
  EXPECT_EQ(first_calls, 20);
  EXPECT_EQ(second_calls, 20);
  EXPECT_NE(out.find("NOTE: DuckDB failed 20 queries with an internal error, a DuckDB bug, and "
                     "answered with fewer optimizers instead (first cases: 0 1 2 3 4; see one "
                     "with ANTB1_DIFF_SEED=42 ANTB1_DIFF_ONLY=<case> pixi run diff-random)"),
            std::string::npos)
      << out;
  EXPECT_NE(out.find("DIFF: PASS seed=42 queries=20 failed=0"), std::string::npos) << out;
}

// Where no retry answers, the case fails as a DuckDB bug, never as a generator bug.
TEST(RunDiff, AnInternalErrorNoRetryAnswersIsADuckDbBug) {
  const auto gen = Generator(0);
  ScriptedEngine antb1("antb1", GoodAntb1);
  ScriptedEngine oracle("duckdb", [](const std::string& /*sql*/) { return InternalError(); });
  ScriptedEngine retry("duckdb", [](const std::string& /*sql*/) { return InternalError(); });
  const std::array<Engine*, 1> retries = {&retry};
  std::string out;
  DiffStats stats = RunDiff(gen, antb1, oracle, {.count = 3}, out, retries);
  EXPECT_EQ(stats.failed, 3U);
  EXPECT_EQ(stats.oracle_retried, 0U) << "no retry answered";
  EXPECT_NE(out.find("DuckDB fails with an internal error, and its retries with fewer optimizers "
                     "fail (a DuckDB bug, not a generator bug)"),
            std::string::npos)
      << out;
  EXPECT_EQ(out.find("generator bug:"), std::string::npos) << out;

  out.clear();
  stats = RunDiff(gen, antb1, oracle, {.count = 1}, out);
  EXPECT_EQ(stats.failed, 1U);
  EXPECT_NE(out.find("DuckDB fails with an internal error (a DuckDB bug, not a generator bug)"),
            std::string::npos)
      << out;
}

// Any other DuckDB error is still a generator bug, and asks no retry.
TEST(RunDiff, AnotherOracleErrorIsAGeneratorBugAndAsksNoRetry) {
  const auto gen = Generator(0);
  int retry_calls = 0;
  ScriptedEngine antb1("antb1", GoodAntb1);
  ScriptedEngine oracle("duckdb", [](const std::string& /*sql*/) -> ExecResult {
    return std::unexpected(
        EngineError{.kind = "Binder Error", .message = "Binder Error: no such column"});
  });
  ScriptedEngine retry("duckdb", [&](const std::string& sql) {
    ++retry_calls;
    return Oracle(sql);
  });
  const std::array<Engine*, 1> retries = {&retry};
  std::string out;
  const DiffStats stats = RunDiff(gen, antb1, oracle, {.count = 1}, out, retries);
  EXPECT_EQ(stats.failed, 1U);
  EXPECT_EQ(retry_calls, 0);
  EXPECT_NE(out.find("DuckDB rejects the generated SQL (a generator bug"), std::string::npos)
      << out;
}

// A database DuckDB invalidated is a DuckDB bug too, and asks the retries like an internal error.
TEST(RunDiff, AFatalOracleErrorAsksTheRetries) {
  const auto gen = Generator(0);
  ScriptedEngine antb1("antb1", GoodAntb1);
  ScriptedEngine oracle("duckdb", [](const std::string& /*sql*/) -> ExecResult {
    return std::unexpected(EngineError{
        .kind = "FATAL Error",
        .message = "FATAL Error: Failed: database has been invalidated because of a previous fatal "
                   "error",
        .internal = true});
  });
  ScriptedEngine retry("duckdb", Oracle);
  const std::array<Engine*, 1> retries = {&retry};
  std::string out;
  const DiffStats stats = RunDiff(gen, antb1, oracle, {.count = 3}, out, retries);
  EXPECT_EQ(stats.failed, 0U) << out;
  EXPECT_EQ(stats.oracle_retried, 3U);
}

// The harness's own error of the oracle (a column type it cannot read) is the generator's or the
// harness's bug, not DuckDB's, and asks no retry.
TEST(RunDiff, TheOraclesHarnessErrorIsNoDuckDbBug) {
  const auto gen = Generator(0);
  int retry_calls = 0;
  ScriptedEngine antb1("antb1", GoodAntb1);
  ScriptedEngine oracle("duckdb", [](const std::string& /*sql*/) -> ExecResult {
    return std::unexpected(EngineError{.kind = "harness",
                                       .message = "harness: column 0 has a DuckDB type the "
                                                  "canonical formatter does not support",
                                       .internal = true});
  });
  ScriptedEngine retry("duckdb", [&](const std::string& sql) {
    ++retry_calls;
    return Oracle(sql);
  });
  const std::array<Engine*, 1> retries = {&retry};
  std::string out;
  const DiffStats stats = RunDiff(gen, antb1, oracle, {.count = 1}, out, retries);
  EXPECT_EQ(stats.failed, 1U);
  EXPECT_EQ(retry_calls, 0);
  EXPECT_NE(out.find("DuckDB rejects the generated SQL (a generator bug"), std::string::npos)
      << out;
  EXPECT_EQ(out.find("DuckDB bug"), std::string::npos) << out;
}

// A retry that fails otherwise (here out of memory) leaves the case a DuckDB bug, reported with the
// internal error first and the retry's error after it.
TEST(RunDiff, AnotherErrorOfARetryKeepsTheInternalOne) {
  const auto gen = Generator(0);
  ScriptedEngine antb1("antb1", GoodAntb1);
  ScriptedEngine oracle("duckdb", [](const std::string& /*sql*/) { return InternalError(); });
  ScriptedEngine retry("duckdb", [](const std::string& /*sql*/) -> ExecResult {
    return std::unexpected(EngineError{.kind = "Out of Memory Error",
                                       .message = "Out of Memory Error: could not allocate"});
  });
  const std::array<Engine*, 1> retries = {&retry};
  std::string out;
  const DiffStats stats = RunDiff(gen, antb1, oracle, {.count = 1}, out, retries);
  EXPECT_EQ(stats.failed, 1U);
  EXPECT_EQ(stats.oracle_retried, 0U);
  const std::size_t internal = out.find("INTERNAL Error: Failed to bind column reference");
  const std::size_t last = out.find("the last retry: Out of Memory Error: could not allocate");
  EXPECT_NE(out.find("DuckDB fails with an internal error, and its retries"), std::string::npos)
      << out;
  ASSERT_NE(internal, std::string::npos) << out;
  ASSERT_NE(last, std::string::npos) << out;
  EXPECT_LT(internal, last);
  EXPECT_EQ(out.find("generator bug:"), std::string::npos) << out;
}

// The retry that answered a case answers the comparison's further queries too: a LIMIT without
// ORDER BY whose rows differ from DuckDB's is compared with the unlimited query's rows, which the
// retry gives, while the oracle that failed is asked only the case's first query. A case whose rows
// are wrong says that a retry answered it.
TEST(RunDiff, TheRetryThatAnsweredAnswersTheFurtherQueries) {
  const FeatureSet limited = {Feature::kColumns, Feature::kIntegerColumns, Feature::kTableName,
                              Feature::kLimit};
  auto gen = QueryGenerator::Make(Tables(), 7, {.supported = limited, .target_percent = 0});
  ASSERT_TRUE(gen.has_value()) << gen.error();
  int oracle_calls = 0;
  int retry_calls = 0;
  int limits = 0;
  ScriptedEngine oracle("duckdb", [&](const std::string& /*sql*/) {
    ++oracle_calls;
    return InternalError();
  });
  // One row of the three under a LIMIT, all three without one.
  ScriptedEngine retry("duckdb", [&](const std::string& sql) {
    ++retry_calls;
    return ExecResult(sql.contains("LIMIT") ? Ints({"1"}) : Ints({"1", "2", "3"}));
  });
  const std::array<Engine*, 1> retries = {&retry};
  const auto run = [&](std::string antb1_row) {
    oracle_calls = 0;
    retry_calls = 0;
    limits = 0;
    ScriptedEngine antb1("antb1", [&](const std::string& sql) {
      const bool limit = sql.contains("LIMIT");
      limits += limit ? 1 : 0;
      return ExecResult(limit ? Ints({antb1_row}) : Ints({"1", "2", "3"}));
    });
    DiffRun r;
    r.stats = RunDiff(*gen, antb1, oracle, {.count = 40}, r.out, retries);
    return r;
  };
  DiffRun r = run("2");
  EXPECT_EQ(r.stats.failed, 0U) << r.out;
  EXPECT_GT(limits, 0) << "the generator writes LIMIT";
  EXPECT_EQ(oracle_calls, 40);
  EXPECT_EQ(retry_calls, 40 + limits) << "each LIMIT's unlimited query goes to the retry";
  r = run("9");
  EXPECT_EQ(r.stats.failed, static_cast<uint64_t>(limits)) << r.out;
  EXPECT_NE(r.out.find(" (DuckDB answered with fewer optimizers, after an internal error)"),
            std::string::npos)
      << r.out;
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
    if (kCountStarOnly.Has(static_cast<Feature>(f))) {
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

// A shard (antb1-slt diff --shard K/N) runs the indexes i % N == K - 1 of the run, so the N slices
// together run each query of the seed exactly once; --only runs its query whatever the slice.
TEST(RunDiff, ShardsRunInterleavedSlicesOfTheSameQueries) {
  const auto gen = Generator(0);
  const auto mismatch = [](const std::string&) { return ExecResult(Ints({"13"})); };
  std::vector<std::string> cases;
  for (const uint64_t shard : {0U, 1U}) {
    const DiffRun r = Diff(gen, mismatch, Oracle,
                           {.count = 7,
                            .shard_index = shard,
                            .shard_count = 2,
                            .command = "antb1-slt diff --seed 42",
                            .max_reports = 100});
    EXPECT_EQ(r.stats.queries, shard == 0 ? 4U : 3U);
    for (uint64_t i = 0; i < 7; ++i) {
      const bool ran = r.out.contains(std::format("FAIL diff case {} ", i));
      EXPECT_EQ(ran, i % 2 == shard) << "case " << i << " of slice " << shard;
      if (ran) {
        cases.push_back(std::format("{}", i));
      }
    }
  }
  EXPECT_EQ(cases.size(), 7U) << "every query once";
  const DiffRun only =
      Diff(gen, mismatch, Oracle, {.count = 7, .only = 2, .shard_index = 1, .shard_count = 2});
  EXPECT_EQ(only.stats.queries, 1U);
  EXPECT_TRUE(only.out.contains("FAIL diff case 2 ")) << only.out;
}

// A run over some of the tables of `pixi run diff-random` (`--table`) reproduces a case with the
// same filter, so the repro generates the query that failed.
TEST(RunDiff, SomeTablesReproduceWithTheirFilter) {
  const auto gen = Generator(0);
  const DiffRun r = Diff(gen, [](const std::string&) { return ExecResult(Ints({"13"})); }, Oracle,
                         {.count = 1, .pixi_args = " --table decimals"});
  EXPECT_EQ(r.stats.failed, 1U);
  EXPECT_NE(
      r.out.find("ANTB1_DIFF_SEED=42 ANTB1_DIFF_ONLY=0 pixi run diff-random --table decimals\n"),
      std::string::npos)
      << r.out;
}

// Over other tables than those of `pixi run diff-random`, only the command line reproduces a case.
TEST(RunDiff, OtherTablesReproduceWithTheCommandLineOnly) {
  const auto gen = Generator(40);
  const auto parse_error = [](const std::string& sql) {
    return UsesOnlyCountStar(sql) ? ExecResult(Ints({"13"}))
                                  : ExecResult(std::unexpected(EngineError{
                                        .kind = "parse", .message = "parse: expected FROM"}));
  };
  DiffOptions options{.count = 50, .pixi_repro = false, .command = "antb1-slt diff --seed 42"};
  DiffRun r = Diff(gen, parse_error, Oracle, options);
  EXPECT_GT(r.stats.failed, 0U);
  EXPECT_GT(r.stats.rejected, 0U);
  EXPECT_NE(r.out.find("  repro:\n    antb1-slt diff --seed 42 --only "), std::string::npos)
      << r.out;
  EXPECT_NE(r.out.find("; see one with antb1-slt diff --seed 42 --only <case>)"), std::string::npos)
      << r.out;
  EXPECT_FALSE(r.out.contains("pixi run diff-random")) << r.out;
  // Without a command line there is nothing to print.
  options.command.clear();
  r = Diff(gen, parse_error, Oracle, options);
  EXPECT_FALSE(r.out.contains("repro")) << r.out;
  EXPECT_FALSE(r.out.contains("see one with")) << r.out;
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
