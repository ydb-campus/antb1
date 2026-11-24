#include "antb1/engine/session.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>

#include "antb1/engine/format.h"
#include "antb1/plan/sql_status.h"
#include "antb1/plan/types.h"

namespace antb1::engine {
namespace {

namespace fs = std::filesystem;

class SessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::path(::testing::TempDir()) / "antb1_engine" /
           ::testing::UnitTest::GetInstance()->current_test_info()->name();
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    path_ = (dir_ / "t.parquet").string();
    arrow::Int16Builder a;
    arrow::UInt16Builder d;
    for (int16_t i = 0; i < 10; ++i) {
      ASSERT_TRUE(a.Append(i).ok());
      ASSERT_TRUE(d.Append(19000).ok());
    }
    auto table = arrow::Table::Make(arrow::schema({arrow::field("AdvEngineID", arrow::int16()),
                                                   arrow::field("EventDate", arrow::uint16())}),
                                    {a.Finish().ValueOrDie(), d.Finish().ValueOrDie()});
    auto out = arrow::io::FileOutputStream::Open(path_).ValueOrDie();
    ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, 4).ok());
    ASSERT_TRUE(out->Close().ok());
  }
  void TearDown() override { fs::remove_all(dir_); }

  fs::path dir_;
  std::string path_;
};

TEST_F(SessionTest, RankingAndRegroupingReuseTheSameGroupingBuckets) {
  auto session = Session::Make().ValueOrDie();
  // More logical partitions than hash buckets, nullable keys and peers across input parts.
  arrow::Int64Builder tenants;
  arrow::Int64Builder items;
  for (int64_t i = 0; i < 768; ++i) {
    if (i % 9 == 0) {
      ASSERT_TRUE(tenants.AppendNull().ok());
    } else {
      ASSERT_TRUE(tenants.Append(i % 48).ok());
    }
    ASSERT_TRUE(items.Append(i % 11).ok());
  }
  const auto table = arrow::Table::Make(
      arrow::schema({arrow::field("tenant", arrow::int64()), arrow::field("item", arrow::int64())}),
      {tenants.Finish().ValueOrDie(), items.Finish().ValueOrDie()});
  const auto path = (dir_ / "ranked.parquet").string();
  const auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
  ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, 64).ok());
  ASSERT_TRUE(out->Close().ok());
  ASSERT_TRUE(session->RegisterParquet("events", {path}).ok());

  for (const auto* grouping :
       {"GROUP BY item, tenant", "GROUP BY GROUPING SETS ((item,tenant),(tenant),(item),())"}) {
    for (const auto* value : {"COUNT(*)", "SUM(CAST(item AS DECIMAL(26,0)))"}) {
      const std::string grouped =
          std::format("SELECT item, tenant AS k, {} AS n FROM events {}", value, grouping);
      // Negation preserves partition equality over these small integers. Computing it puts
      // a boundary between the grouping and the window, giving an independent serial control.
      const auto ranked = [&](std::string_view key) {
        return std::format(
            "SELECT k,item,n,RANK() OVER (PARTITION BY {} ORDER BY n DESC) AS r "
            "FROM ({}) g",
            key, grouped);
      };
      const auto compare = [&](const std::string& query, const std::string& control) {
        const auto actual = session->Execute(query);
        const auto expected = session->Execute(control);
        ASSERT_TRUE(actual.ok()) << actual.status().ToString();
        ASSERT_TRUE(expected.ok()) << expected.status().ToString();
        EXPECT_TRUE(actual->table->Equals(*expected->table)) << query;
      };
      const auto query = ranked("k");
      const auto control = ranked("-k");
      const std::string ordered = " ORDER BY k,item,n,r";
      ASSERT_NO_FATAL_FAILURE(compare(query + ordered, control + ordered));
      const auto regroup = [](const std::string& input) {
        return "SELECT k,r,COUNT(*) FROM (" + input + ") w WHERE r <= 4 GROUP BY k,r ORDER BY k,r";
      };
      ASSERT_NO_FATAL_FAILURE(compare(regroup(query), regroup(control)));
      const auto profile = session->ExplainAnalyze(regroup(query));
      ASSERT_TRUE(profile.ok()) << profile.status().ToString();
      const auto text = FormatProfile(*profile, ProfileFormat::kJson);
      EXPECT_NE(text.find("\"rank_buckets\":"), std::string::npos);
      EXPECT_NE(text.find("\"group_buckets\":"), std::string::npos);
      if (std::string_view(grouping).contains("GROUPING SETS")) {
        // Only (tenant) is complete within root buckets. (item) and () span buckets.
        EXPECT_TRUE(text.contains("\"colocated_sets\":1")) << text;
      }
      const auto serial = session->ExplainAnalyze(control);
      ASSERT_TRUE(serial.ok()) << serial.status().ToString();
      EXPECT_EQ(FormatProfile(*serial, ProfileFormat::kJson).find("\"rank_buckets\":"),
                std::string::npos);
    }
  }
}

TEST_F(SessionTest, WideDecimalRollupUsesTheVerifiedJoinMultiplicity) {
  auto session = Session::Make().ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("events", {path_}).ok());
  // Ten input rows fit the DECIMAL(37,0) proof. The other join pairs every row with all ten.
  for (const bool unique : {true, false}) {
    const auto* field = unique ? "AdvEngineID" : "EventDate";
    const auto sql = std::format(
        "SELECT a.AdvEngineID AS k, SUM(CAST(b.AdvEngineID AS DECIMAL(37,0))) AS v "
        "FROM events a LEFT JOIN events b ON a.{} = b.{} GROUP BY ROLLUP(a.AdvEngineID) "
        "ORDER BY k NULLS LAST",
        field, field);
    const auto result = session->Execute(sql);
    ASSERT_TRUE(result.ok()) << result.status();
    ASSERT_EQ(result->types.back(), plan::LogicalType::Decimal(38, 0));
    ASSERT_EQ(result->table->num_rows(), 11);
    const auto& sums = result->table->column(1);
    for (int64_t i = 0; i < 10; ++i) {
      EXPECT_EQ(sums->GetScalar(i).ValueOrDie()->ToString(), std::to_string(unique ? i : 45));
    }
    EXPECT_EQ(sums->GetScalar(10).ValueOrDie()->ToString(), unique ? "45" : "450");
    const auto profile = session->ExplainAnalyze(sql);
    ASSERT_TRUE(profile.ok()) << profile.status();
    const auto json = FormatProfile(*profile, ProfileFormat::kJson);
    const auto visits = std::format("\"local_rows\":{}", unique ? 10 : 200);
    EXPECT_TRUE(json.contains(visits + ",") || json.contains(visits + "}")) << json;
  }
}

TEST_F(SessionTest, RollupChecksEveryWideDecimalCoalesceArm) {
  auto session = Session::Make().ValueOrDie();
  const auto type = arrow::decimal128(26, 0);
  arrow::Decimal128Builder amounts(type);
  arrow::Int64Builder keys;
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(keys.Append(i % 2).ok());
    ASSERT_TRUE(amounts.Append(arrow::Decimal128(i)).ok());
  }
  const auto schema = arrow::schema({arrow::field("k", arrow::int64()), arrow::field("raw", type)});
  const auto data =
      arrow::Table::Make(schema, {keys.Finish().ValueOrDie(), amounts.Finish().ValueOrDie()});
  const auto path = (dir_ / "decimal.parquet").string();
  const auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
  ASSERT_TRUE(parquet::arrow::WriteTable(*data, arrow::default_memory_pool(), out, 8).ok());
  ASSERT_TRUE(out->Close().ok());
  ASSERT_TRUE(session->RegisterParquet("samples", {path}).ok());
  // The product and zero have proven bounds. A same-type cast of the file column does not.
  for (const bool checked : {true, false}) {
    const auto sql = std::format(
        "SELECT k, SUM(x) FROM (SELECT k, COALESCE(CAST(k AS DECIMAL(20,0)) * "
        "CAST(k AS DECIMAL(6,0)), {}) AS x FROM samples) s GROUP BY ROLLUP(k)",
        checked ? "0" : "CAST(raw AS DECIMAL(26,0))");
    const auto profile = session->ExplainAnalyze(sql);
    ASSERT_TRUE(profile.ok()) << profile.status();
    const auto json = FormatProfile(*profile, ProfileFormat::kJson);
    const auto visits = std::format("\"local_rows\":{}", checked ? 8 : 16);
    EXPECT_TRUE(json.contains(visits + ",") || json.contains(visits + "}")) << json;
  }
}

TEST_F(SessionTest, WideDecimalRollupKeepsIntermediateOverflowBeforeCancellation) {
  auto session = Session::Make().ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("events", {path_}).ok());
  // Individual fine groups fit. The grand total overflows on its second row before cancellation.
  const auto result = session->Execute(
      "SELECT k,SUM(v) FROM (SELECT AdvEngineID % 2 AS k, "
      "CASE WHEN AdvEngineID < 2 THEN CAST(90000000000000000000000000000000000000 "
      "AS DECIMAL(38,0)) ELSE CAST(-90000000000000000000000000000000000000 AS DECIMAL(38,0)) "
      "END AS v FROM events WHERE AdvEngineID < 3) t GROUP BY ROLLUP(k) LIMIT 1");
  EXPECT_FALSE(result.ok());
  EXPECT_TRUE(result.status().message().contains("SUM overflow")) << result.status();
}

TEST_F(SessionTest, CountStarFromRegisteredTable) {
  auto session = Session::Make().ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("events", {path_}).ok());
  auto result = session->Execute("SELECT COUNT(*) FROM events;");
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  ASSERT_EQ(result->table->num_rows(), 1);
  auto col = std::static_pointer_cast<arrow::Int64Array>(result->table->column(0)->chunk(0));
  EXPECT_EQ(col->Value(0), 10);
  EXPECT_EQ(result->names, std::vector<std::string>{"count_star()"});
}

// A union whose first block the condition on it rules out (ADR 0036's X7a) names and types its
// columns as that first block does: the block left, which calls its column z, does not rename it.
TEST_F(SessionTest, AUnionPrunedOfItsFirstBlockKeepsItsNamesAndTypes) {
  auto session = Session::Make().ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("events", {path_}).ok());
  constexpr std::string_view kSql =
      "SELECT * FROM (SELECT 1 AS k FROM events UNION ALL SELECT 3000000000 AS z FROM events) AS v "
      "WHERE k > 5";
  const auto plan = session->Explain(kSql);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_NE(plan->find("UnionAll inputs=1 columns=[z]"), std::string::npos)
      << "the first block is ruled out:\n"
      << *plan;
  auto result = session->Execute(kSql);
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  EXPECT_EQ(result->names, std::vector<std::string>{"k"});
  EXPECT_EQ(result->types, std::vector<plan::LogicalType>{plan::LogicalType::kBigInt});
  EXPECT_EQ(result->table->schema()->field(0)->name(), "k");
  EXPECT_EQ(result->table->num_rows(), 10) << "the second block's rows, all of them";
}

TEST_F(SessionTest, CountStarFromPath) {
  auto session = Session::Make().ValueOrDie();
  auto result = session->Execute("SELECT COUNT(*) FROM '" + path_ + "'");
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  EXPECT_EQ(
      std::static_pointer_cast<arrow::Int64Array>(result->table->column(0)->chunk(0))->Value(0),
      10);
}

// Filtering a derived join/group must not suppress an error in a join residual evaluated before
// that filter. Integer multiplication overflows on the fixture's matches.
TEST_F(SessionTest, GroupKeyPushdownKeepsJoinResidualErrors) {
  auto session = Session::Make().ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("events", {path_}).ok());
  for (const char* kind : {"JOIN", "LEFT JOIN"}) {
    for (const bool grouped : {false, true}) {
      const std::string query = std::format(
          "SELECT k FROM (SELECT a.AdvEngineID AS k{} FROM events a {} events b "
          "ON a.EventDate = b.EventDate AND a.AdvEngineID * b.AdvEngineID * "
          "4611686018427387904 > b.AdvEngineID{}) g WHERE k = 0",
          grouped ? ", COUNT(*) AS n" : "", kind, grouped ? " GROUP BY a.AdvEngineID" : "");
      const auto result = session->Execute(query);
      EXPECT_TRUE(result.status().IsExecutionError()) << query << "\n"
                                                      << result.status().ToString();
    }
  }
}

TEST_F(SessionTest, GroupKeyFilterPreservesOverflowInDiscardedDecimalRows) {
  // The second row fits the physical INT32 but exceeds its declared precision. Doubling
  // it overflows DECIMAL(5,1), even though the consumer only wants the first group.
  auto session = Session::Make().ValueOrDie();
  const auto type = arrow::decimal128(4, 1);
  arrow::Decimal128Builder amounts(type);
  arrow::Int32Builder keys;
  ASSERT_TRUE(keys.AppendValues({1, 2}).ok());
  ASSERT_TRUE(amounts.Append(arrow::Decimal128(10)).ok());
  ASSERT_TRUE(amounts.Append(arrow::Decimal128(60000)).ok());
  const auto table = arrow::Table::Make(
      arrow::schema({arrow::field("k", arrow::int32()), arrow::field("v", type)}),
      {keys.Finish().ValueOrDie(), amounts.Finish().ValueOrDie()});
  const auto path = (dir_ / "amounts.parquet").string();
  const auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
  parquet::WriterProperties::Builder props;
  props.enable_store_decimal_as_integer();
  ASSERT_TRUE(
      parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, 1, props.build()).ok());
  ASSERT_TRUE(out->Close().ok());
  ASSERT_TRUE(session->RegisterParquet("samples", {path}).ok());
  const auto result = session->Execute(
      "SELECT n FROM (SELECT k, MAX((v + v) / 3) AS n FROM samples GROUP BY k) g WHERE k = 1");
  EXPECT_TRUE(result.status().IsExecutionError()) << result.status();
  EXPECT_NE(result.status().ToString().find("Overflow"), std::string::npos) << result.status();
}

TEST_F(SessionTest, GroupKeyInferenceDoesNotSkipFailingSourceComputations) {
  auto session = Session::Make().ValueOrDie();
  const auto write = [&](const std::string& name, int64_t key, int64_t rows) {
    arrow::Int64Builder k;
    arrow::Int64Builder j;
    for (int64_t i = 0; i < rows; ++i) {
      ASSERT_TRUE(k.Append(key).ok());
      ASSERT_TRUE(j.Append(0).ok());
    }
    const auto table = arrow::Table::Make(
        arrow::schema({arrow::field("k", arrow::int64()), arrow::field("j", arrow::int64())}),
        {k.Finish().ValueOrDie(), j.Finish().ValueOrDie()});
    const auto path = (dir_ / (name + ".parquet")).string();
    const auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
    ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, 4).ok());
    ASSERT_TRUE(out->Close().ok());
    ASSERT_TRUE(session->RegisterParquet(name, {path}).ok());
  };
  ASSERT_NO_FATAL_FAILURE(write("a", 4, 2));
  ASSERT_NO_FATAL_FAILURE(write("b", 5, 1));
  // Two keys avoid the existing single-key aggregate runtime filter. Inferring b.k = 4 would
  // empty the right build and skip the overflowing computation on the left entirely.
  std::vector<std::string> queries = {
      "SELECT g.n FROM (SELECT k, j, COUNT(*) * 4611686018427387904 AS n FROM a "
      "WHERE k = 4 GROUP BY k, j) AS g JOIN b ON g.k = b.k AND g.j = b.j",
      "SELECT g.n FROM (SELECT k, j, MAX(k * 4611686018427387904) AS n FROM a "
      "GROUP BY k, j HAVING k = 4) AS g JOIN b ON g.k = b.k AND g.j = b.j",
      // Direct movement through a derived join must preserve the same source barriers, both
      // when inferring an empty build and when the moved predicate itself empties the build.
      "SELECT s.n FROM (SELECT c.k, c.j, c.n FROM "
      "(SELECT k, j, k * 4611686018427387904 AS n FROM a) c "
      "JOIN b ON c.k = b.k AND c.j = b.j) s WHERE s.k = 4",
      "SELECT s.n FROM (SELECT b.k, c.n FROM "
      "(SELECT k, j, k * 4611686018427387904 AS n FROM a) c "
      "JOIN b ON c.k = b.k AND c.j = b.j) s WHERE s.k = 4"};
  // A new fact above an argument computation, or a sibling of a join, must retain the barrier.
  for (const char* kind : {"JOIN", "LEFT JOIN"}) {
    queries.push_back(
        std::format("SELECT s.n FROM (SELECT g.k, g.j, c.n FROM "
                    "(SELECT k, j, COUNT(*) AS q FROM a WHERE k = 4 GROUP BY k, j) g {} "
                    "(SELECT k, j, k * 4611686018427387904 AS n FROM a) c "
                    "ON g.k = c.k AND g.j = c.j) s JOIN b ON s.k = b.k AND s.j = b.j",
                    kind));
  }
  for (const auto& query : queries) {
    const auto result = session->Execute(query);
    EXPECT_TRUE(result.status().IsExecutionError()) << query << "\n" << result.status().ToString();
  }
}

TEST_F(SessionTest, InferredJoinKeysPreserveCheckedAggregateErrorsOnEitherInput) {
  arrow::Int64Builder keys;
  arrow::Int64Builder tags;
  const auto type = arrow::decimal128(38, 0);
  arrow::Decimal128Builder values(type);
  ASSERT_TRUE(keys.AppendValues({1, 2, 2}).ok());
  ASSERT_TRUE(tags.AppendValues({0, 0, 0}).ok());
  ASSERT_TRUE(values.Append(arrow::Decimal128(1)).ok());
  const auto largest = arrow::Decimal128::GetMaxValue(38);
  ASSERT_TRUE(values.Append(largest).ok());
  ASSERT_TRUE(values.Append(largest).ok());
  const auto table = arrow::Table::Make(
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("j", arrow::int64()),
                     arrow::field("v", type)}),
      {keys.Finish().ValueOrDie(), tags.Finish().ValueOrDie(), values.Finish().ValueOrDie()});
  const auto path = (dir_ / "wide.parquet").string();
  const auto file = arrow::io::FileOutputStream::Open(path).ValueOrDie();
  ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), file, 4).ok());
  ASSERT_TRUE(file->Close().ok());

  SessionOptions options;
  options.build_side_ratio = 100'000;  // Build on the right; two keys avoid the runtime key filter.
  auto session = Session::Make(options).ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("t", {path}).ok());
  for (const char* query :
       {// Inferring k=2 on the right would empty the build and skip the overflowing probe.
        "SELECT a.n FROM (SELECT k, j, SUM(v) AS n FROM t WHERE k=2 GROUP BY k,j) a "
        "JOIN (SELECT k,j FROM t WHERE k=1) b ON a.k=b.k AND a.j=b.j",
        // Inferring k=1 below the right aggregate would exclude its overflowing group.
        "SELECT b.n FROM (SELECT k,j,COUNT(*) AS n FROM t WHERE k=1 GROUP BY k,j) a "
        "JOIN (SELECT k,j,SUM(v) AS n FROM t GROUP BY k,j) b ON a.k=b.k AND a.j=b.j"}) {
    SCOPED_TRACE(query);
    const auto result = session->Execute(query);
    EXPECT_TRUE(result.status().IsExecutionError()) << result.status().ToString();
    EXPECT_NE(result.status().ToString().find("overflow"), std::string::npos);
  }
  // A consumer's own grouping-key predicate may still exclude that group.
  const auto direct =
      session->Execute("SELECT n FROM (SELECT k, SUM(v) AS n FROM t GROUP BY k) g WHERE k=1");
  ASSERT_TRUE(direct.ok()) << direct.status().ToString();
  EXPECT_EQ(direct->table->num_rows(), 1);
}

TEST_F(SessionTest, ClickBenchOverrideAppliesOnlyWhenColumnExists) {
  SessionOptions options;
  options.default_overrides.emplace_back("eventdate",
                                         plan::LogicalType::kDate);  // case-insensitive
  auto session = Session::Make(options).ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("events", {path_}).ok());
  const auto table = session->catalog().Find("EVENTS");
  ASSERT_NE(table, nullptr);
  EXPECT_EQ(table->schema()->GetFieldByName("EventDate")->type()->id(), arrow::Type::DATE32);
}

TEST_F(SessionTest, ErrorsCarryKinds) {
  auto session = Session::Make().ValueOrDie();
  auto parse = session->Execute("SELEC 1");
  ASSERT_FALSE(parse.ok());
  ASSERT_NE(plan::GetSqlError(parse.status()), nullptr);

  auto missing_table = session->Execute("SELECT COUNT(*) FROM nope");
  EXPECT_EQ(plan::GetSqlError(missing_table.status())->kind(), plan::SqlErrorDetail::Kind::kBind);

  auto io = session->Execute("SELECT COUNT(*) FROM '/nonexistent/x.parquet'");
  EXPECT_TRUE(io.status().IsIOError());
  EXPECT_EQ(plan::GetSqlError(io.status()), nullptr);
}

TEST_F(SessionTest, ExplainShowsRowCount) {
  auto session = Session::Make().ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("t", {path_}).ok());
  auto text = session->Explain("SELECT COUNT(*) FROM t");
  ASSERT_TRUE(text.ok()) << text.status().ToString();
  EXPECT_EQ(*text,
            "Output: count_star():BIGINT\nRowCount table=t source=parquet(files=1, rows=10)\n");
}

// ExplainAnalyze runs the query (the rows of Execute, dropped) and profiles every physical
// operator: the text has the Output and Total lines, then one line per operator with its rows and
// time in brackets; the JSON is one object; errors are those of Execute. The same counts on 1 and
// 4 threads.
TEST_F(SessionTest, ExplainAnalyzeProfilesTheQuery) {
  const std::string sql =
      "SELECT AdvEngineID, COUNT(*) FROM t WHERE AdvEngineID > 2 GROUP BY AdvEngineID "
      "ORDER BY AdvEngineID LIMIT 5";
  std::string counts;
  for (const int threads : {1, 4}) {
    auto session = Session::Make({.threads = threads}).ValueOrDie();
    ASSERT_TRUE(session->RegisterParquet("t", {path_}).ok());
    const auto result = session->Execute(sql);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    const auto profile = session->ExplainAnalyze(sql);
    ASSERT_TRUE(profile.ok()) << profile.status().ToString();
    EXPECT_EQ(profile->rows, result->table->num_rows());
    EXPECT_EQ(profile->threads, threads);
    EXPECT_GT(profile->peak_memory, 0);
    EXPECT_EQ(profile->output, "Output: AdvEngineID:SMALLINT count_star():BIGINT");

    const std::string text = FormatProfile(*profile, ProfileFormat::kText);
    std::vector<std::string> lines;
    for (std::size_t start = 0; start < text.size();) {
      const std::size_t end = text.find('\n', start);
      lines.push_back(text.substr(start, end - start));
      start = end + 1;
    }
    ASSERT_GE(lines.size(), 5U) << text;
    EXPECT_EQ(lines[0], profile->output);
    EXPECT_TRUE(lines[1].starts_with("Total: time=")) << lines[1];
    EXPECT_NE(lines[1].find(std::format(" rows=5 peak_memory=")), std::string::npos) << lines[1];
    std::string these;
    for (std::size_t i = 2; i < lines.size(); ++i) {
      EXPECT_NE(lines[i].find("  [rows="), std::string::npos) << lines[i];
      EXPECT_TRUE(lines[i].ends_with(']')) << lines[i];
      // The operator and its rows, which do not depend on the threads.
      const std::size_t rows = lines[i].find("[rows=");
      these += lines[i].substr(0, lines[i].find_first_not_of(' ')) +
               lines[i].substr(lines[i].find_first_not_of(' '),
                               lines[i].find(' ', lines[i].find_first_not_of(' ')) -
                                   lines[i].find_first_not_of(' ')) +
               " " + lines[i].substr(rows, lines[i].find(' ', rows) - rows) + "\n";
    }
    if (counts.empty()) {
      counts = these;
      EXPECT_NE(counts.find("PartGroupAggregate [rows=7"), std::string::npos) << counts;
    } else {
      EXPECT_EQ(these, counts);
    }

    const std::string json = FormatProfile(*profile, ProfileFormat::kJson);
    EXPECT_TRUE(json.starts_with(R"({"output":"Output: AdvEngineID:SMALLINT)")) << json;
    EXPECT_NE(json.find(std::format(R"("rows":5,"peak_memory_bytes":)")), std::string::npos);
    EXPECT_NE(json.find(std::format(R"("threads":{},"plan":{{"name":)", threads)),
              std::string::npos)
        << json;
    EXPECT_TRUE(json.ends_with("]}}\n")) << json;

    EXPECT_EQ(session->ExplainAnalyze("SELECT nope FROM t").status().ToString(),
              session->Execute("SELECT nope FROM t").status().ToString());
  }
}

// `explain --physical` is the plan `explain --analyze` profiles, without what only a run can
// measure: strip the brackets off the profile's text and the two are the same tree, line for line.
// It is the comparison that keeps the two renderings from drifting apart, and the query is one
// whose scan a run would have to open parts of.
TEST_F(SessionTest, ExplainPhysicalIsTheProfiledPlanWithoutTheCounters) {
  const std::string sql =
      "SELECT AdvEngineID, COUNT(*) FROM t WHERE AdvEngineID > 2 GROUP BY AdvEngineID "
      "ORDER BY AdvEngineID LIMIT 5";
  auto session = Session::Make({.threads = 4}).ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("t", {path_}).ok());
  const auto physical = session->ExplainPhysical(sql);
  ASSERT_TRUE(physical.ok()) << physical.status().ToString();
  // The planner's decisions are what the lines are for: this one pushes its predicate into the
  // scan, which the logical plan of Explain cannot show.
  EXPECT_NE(physical->find("pushed predicate"), std::string::npos) << *physical;

  const auto profile = session->ExplainAnalyze(sql);
  ASSERT_TRUE(profile.ok()) << profile.status().ToString();
  std::string stripped;
  const std::string text = FormatProfile(*profile, ProfileFormat::kText);
  for (std::size_t start = 0; start < text.size();) {
    const std::size_t end = text.find('\n', start);
    const std::string line = text.substr(start, end - start);
    start = end == std::string::npos ? text.size() : end + 1;
    if (line.starts_with("Total: ")) {
      continue;  // a run's own line, which a plan has nothing to put in
    }
    const std::size_t counters = line.find("  [rows=");
    stripped += line.substr(0, counters == std::string::npos ? line.size() : counters) + '\n';
  }
  EXPECT_EQ(*physical, stripped);
}

// Explain shows the optimized plan of every bound query: pruned scans, folded literals.
TEST_F(SessionTest, ExplainShowsTheOptimizedPlan) {
  SessionOptions options;
  options.default_overrides.emplace_back("EventDate", plan::LogicalType::kDate);
  auto session = Session::Make(options).ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("t", {path_}).ok());
  auto text = session->Explain(
      "SELECT MIN(EventDate) AS first, SUM(advengineid) FROM t WHERE AdvEngineID > 0.5 AND "
      "EventDate < '2013-07-16'");
  ASSERT_TRUE(text.ok()) << text.status().ToString();
  EXPECT_EQ(*text,
            "Output: first:DATE sum(advengineid):HUGEINT\n"
            "Aggregate MIN(EventDate), SUM(AdvEngineID)\n"
            "  Filter AdvEngineID >= 1 AND EventDate < DATE '2013-07-16'\n"
            "    Scan table=t source=parquet(files=1, rows=10) columns=[AdvEngineID, EventDate]\n");
}

// The rows of a result as canonical text (engine::FormatValue), one vector per row.
std::vector<std::vector<std::string>> Rows(const QueryResult& result) {
  const auto& table = *result.table;
  std::vector<std::vector<std::string>> rows(static_cast<std::size_t>(table.num_rows()));
  for (int c = 0; c < table.num_columns(); ++c) {
    std::size_t row = 0;
    for (const auto& chunk : table.column(c)->chunks()) {
      for (int64_t i = 0; i < chunk->length(); ++i, ++row) {
        rows.at(row).push_back(
            FormatValue(*chunk, i, result.types.at(static_cast<std::size_t>(c))));
      }
    }
  }
  return rows;
}

// Every query shape of the grammar runs; results carry the output names (also as the table's
// field names) and types.
TEST_F(SessionTest, ExecutesAggregatesProjectionsFiltersAndLimits) {
  SessionOptions options;
  options.default_overrides.emplace_back("EventDate", plan::LogicalType::kDate);
  options.batch_size = 3;
  auto session = Session::Make(options).ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("t", {path_}).ok());

  auto aggregates = session->Execute(
      "SELECT COUNT(*), SUM(AdvEngineID) AS s, AVG(AdvEngineID), MIN(EventDate), "
      "MAX(AdvEngineID), COUNT(EventDate) FROM t WHERE AdvEngineID >= 2");
  ASSERT_TRUE(aggregates.ok()) << aggregates.status().ToString();
  EXPECT_EQ(aggregates->names,
            (std::vector<std::string>{"count_star()", "s", "avg(AdvEngineID)", "min(EventDate)",
                                      "max(AdvEngineID)", "count(EventDate)"}));
  EXPECT_EQ(aggregates->types, (std::vector<plan::LogicalType>{
                                   plan::LogicalType::kBigInt, plan::LogicalType::kHugeInt,
                                   plan::LogicalType::kDouble, plan::LogicalType::kDate,
                                   plan::LogicalType::kSmallInt, plan::LogicalType::kBigInt}));
  EXPECT_EQ(aggregates->table->schema()->field(1)->name(), "s");
  EXPECT_EQ(Rows(*aggregates),
            (std::vector<std::vector<std::string>>{{"8", "44", "5.5", "2022-01-08", "9", "8"}}));

  auto projection = session->Execute(
      "SELECT AdvEngineID, EventDate AS d FROM t WHERE AdvEngineID <> 1 AND 5 > AdvEngineID "
      "LIMIT 3");
  ASSERT_TRUE(projection.ok()) << projection.status().ToString();
  EXPECT_EQ(Rows(*projection), (std::vector<std::vector<std::string>>{
                                   {"0", "2022-01-08"}, {"2", "2022-01-08"}, {"3", "2022-01-08"}}));

  auto star = session->Execute("SELECT * FROM t WHERE AdvEngineID > 8.5");
  ASSERT_TRUE(star.ok()) << star.status().ToString();
  EXPECT_EQ(Rows(*star), (std::vector<std::vector<std::string>>{{"9", "2022-01-08"}}));

  // No row passes: COUNT is 0, the other aggregates are NULL; a projection has no rows.
  auto none = session->Execute(
      "SELECT COUNT(*), COUNT(AdvEngineID), SUM(AdvEngineID), MAX(EventDate) FROM t "
      "WHERE AdvEngineID > 100000");
  ASSERT_TRUE(none.ok()) << none.status().ToString();
  EXPECT_EQ(Rows(*none), (std::vector<std::vector<std::string>>{{"0", "0", "NULL", "NULL"}}));
  auto empty = session->Execute("SELECT AdvEngineID FROM t WHERE AdvEngineID = 1.5");
  ASSERT_TRUE(empty.ok()) << empty.status().ToString();
  EXPECT_EQ(empty->table->num_rows(), 0);
  EXPECT_EQ(empty->names, std::vector<std::string>{"AdvEngineID"});
}

// Writes `values` to a Parquet file as `d` (DOUBLE) and `f` (FLOAT, widened to DOUBLE on read),
// with `k` (SMALLINT) numbering the rows from `first_k`, in row groups of 2 rows.
void WriteDoubles(const std::string& path, const std::vector<double>& values, int16_t first_k) {
  arrow::Int16Builder k;
  arrow::DoubleBuilder d;
  arrow::FloatBuilder f;
  int16_t next = first_k;
  for (const double v : values) {
    ASSERT_TRUE(k.Append(next++).ok());
    ASSERT_TRUE(d.Append(v).ok());
    ASSERT_TRUE(f.Append(static_cast<float>(v)).ok());
  }
  auto table = arrow::Table::Make(
      arrow::schema({arrow::field("k", arrow::int16()), arrow::field("d", arrow::float64()),
                     arrow::field("f", arrow::float32())}),
      {k.Finish().ValueOrDie(), d.Finish().ValueOrDie(), f.Finish().ValueOrDie()});
  auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
  ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, 2).ok());
  ASSERT_TRUE(out->Close().ok());
}

// D10: MIN and MAX ignore NaN and are NaN only when every selected value is NaN, whatever the
// batch size, the file order and the rows WHERE keeps (FLOAT too, read as DOUBLE).
TEST_F(SessionTest, MinMaxIgnoreNaNAcrossBatchesAndFiles) {
  constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
  const std::string a = (dir_ / "a.parquet").string();
  const std::string b = (dir_ / "b.parquet").string();
  const std::string c = (dir_ / "c.parquet").string();
  ASSERT_NO_FATAL_FAILURE(WriteDoubles(a, {kNaN, kNaN, kNaN, 5}, 0));  // k 0 to 3
  ASSERT_NO_FATAL_FAILURE(WriteDoubles(b, {2, kNaN}, 4));              // k 4 and 5
  ASSERT_NO_FATAL_FAILURE(WriteDoubles(c, {kNaN, kNaN}, 6));           // k 6 and 7
  const std::vector<std::pair<std::string, std::vector<std::string>>> queries = {
      {"", {"2", "5", "2", "5"}},
      {" WHERE k <> 4", {"5", "5", "5", "5"}},  // keeps only the NaN of b
      {" WHERE k <> 3 AND k <> 4", {"nan", "nan", "nan", "nan"}},
  };
  for (const int64_t batch_size : {1, 2, 3, 64 * 1024}) {
    for (const auto& files :
         std::vector<std::vector<std::string>>{{a, b, c}, {c, b, a}, {b, c, a}}) {
      SessionOptions options;
      options.batch_size = batch_size;
      auto session = Session::Make(options).ValueOrDie();
      ASSERT_TRUE(session->RegisterParquet("t", files).ok());
      for (const auto& [where, expected] : queries) {
        auto result = session->Execute("SELECT MIN(d), MAX(d), MIN(f), MAX(f) FROM t" + where);
        ASSERT_TRUE(result.ok()) << result.status().ToString();
        EXPECT_EQ(Rows(*result), std::vector<std::vector<std::string>>{expected})
            << "batch size " << batch_size << ", files from " << files.front() << where;
      }
    }
  }
}

// A FLOAT column is widened exactly to DOUBLE, but WHERE compares it like DuckDB: an integer or
// DECIMAL literal becomes the FLOAT DuckDB casts it to (0.1F = 0.1 and 16777216F = 16777217 hold);
// a number DuckDB types as DOUBLE (1e-1) compares in DOUBLE. A DOUBLE column holding the same
// values still compares with the nearest double. Results stay DOUBLE (divergence D11).
TEST_F(SessionTest, FloatColumnsCompareLikeDuckDb) {
  const std::string path = (dir_ / "floats.parquet").string();
  arrow::FloatBuilder f;
  arrow::DoubleBuilder d;
  for (const float v : {0.1F, 0.2F, 16777216.0F}) {
    ASSERT_TRUE(f.Append(v).ok());
    ASSERT_TRUE(d.Append(static_cast<double>(v)).ok());
  }
  ASSERT_TRUE(f.AppendNull().ok());
  ASSERT_TRUE(d.AppendNull().ok());
  const auto table = arrow::Table::Make(
      arrow::schema({arrow::field("f", arrow::float32()), arrow::field("d", arrow::float64())}),
      {f.Finish().ValueOrDie(), d.Finish().ValueOrDie()});
  auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
  ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, 4).ok());
  ASSERT_TRUE(out->Close().ok());

  auto session = Session::Make().ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("floats", {path}).ok());
  auto count = [&session](const std::string& where) {
    auto result = session->Execute("SELECT COUNT(*) FROM floats WHERE " + where);
    return result.ok() ? Rows(*result).at(0).at(0) : result.status().ToString();
  };
  // DuckDB's answers.
  EXPECT_EQ(count("f > 0.1"), "2");
  EXPECT_EQ(count("f = 0.1"), "1");
  EXPECT_EQ(count("0.1 >= f"), "1");
  EXPECT_EQ(count("f = 16777217"), "1");
  EXPECT_EQ(count("f < 16777217"), "2");
  EXPECT_EQ(count("f > 1e-1"), "3");
  EXPECT_EQ(count("f < 16777217e0"), "3");
  // The DOUBLE column: the nearest double, as before.
  EXPECT_EQ(count("d = 0.1"), "0");
  EXPECT_EQ(count("d > 0.1"), "3");
  EXPECT_EQ(count("d = 16777217"), "0");

  auto values = session->Execute("SELECT f FROM floats WHERE f < 1");
  ASSERT_TRUE(values.ok()) << values.status().ToString();
  EXPECT_EQ(values->types, std::vector<plan::LogicalType>{plan::LogicalType::kDouble});
  EXPECT_EQ(Rows(*values), (std::vector<std::vector<std::string>>{{"0.10000000149011612"},
                                                                  {"0.20000000298023224"}}));

  // Arithmetic on FLOAT columns would compute in DOUBLE, not in FLOAT as DuckDB does: unsupported
  // (divergence D11), and so is comparing them with another expression. MIN and MAX keep FLOAT,
  // so a HAVING alias of one compares in FLOAT.
  for (const char* sql : {"SELECT f + 1 FROM floats", "SELECT -f FROM floats",
                          "SELECT COUNT(*) FROM floats WHERE f < d",
                          "SELECT CASE WHEN f > 0 THEN f END FROM floats"}) {
    auto result = session->Execute(sql);
    const auto detail = plan::GetSqlError(result.status());
    ASSERT_NE(detail, nullptr) << sql << ": " << result.status().ToString();
    EXPECT_EQ(detail->kind(), plan::SqlErrorDetail::Kind::kUnsupported) << sql;
  }
  auto having = session->Execute("SELECT MAX(f) AS m FROM floats HAVING m = 0.2");
  ASSERT_TRUE(having.ok()) << having.status().ToString();
  EXPECT_EQ(Rows(*having), (std::vector<std::vector<std::string>>{}));
  auto grouped = session->Execute("SELECT f, COUNT(*) FROM floats GROUP BY f HAVING f = 0.1");
  ASSERT_TRUE(grouped.ok()) << grouped.status().ToString();
  EXPECT_EQ(Rows(*grouped).size(), 1U);
}

TEST_F(SessionTest, UnsupportedAndBindErrorsKeepTheirKinds) {
  constexpr auto kRecursive =
      "WITH RECURSIVE countdown(n) AS (SELECT COUNT(*) FROM (SELECT * FROM t LIMIT 3) AS seed "
      "UNION ALL SELECT n - 1 FROM countdown WHERE n > 0) SELECT COUNT(*) FROM countdown";
  auto session = Session::Make().ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("t", {path_}).ok());
  // Column nope does not exist (SUM(nope) below is a bind error), but unsupported SQL is
  // rejected before any name is resolved, and so are the join kinds that still parse and wait
  // (table nope is not registered either; a LEFT JOIN is answered from roadmap PR J6 on).
  for (const char* sql : {kRecursive, "SELECT AdvEngineID FROM t ORDER BY md5(AdvEngineID)",
                          "SELECT SUM(DISTINCT AdvEngineID) FROM t",
                          "SELECT DISTINCT ON (AdvEngineID) AdvEngineID FROM t",
                          "SELECT COUNT(*) FROM t semi JOIN nope ON t.AdvEngineID = nope.x",
                          "SELECT COUNT(*) FROM (SELECT md5(AdvEngineID) FROM t)"}) {
    auto result = session->Execute(sql);
    const auto detail = plan::GetSqlError(result.status());
    ASSERT_NE(detail, nullptr) << sql << ": " << result.status().ToString();
    EXPECT_EQ(detail->kind(), plan::SqlErrorDetail::Kind::kUnsupported) << sql;
    EXPECT_TRUE(result.status().IsNotImplemented()) << sql;
  }
  // An inner join and a comma join bind, so their tables resolve: a missing one is a bind error,
  // not an unsupported query.
  for (const char* sql :
       {"SELECT nope FROM t, nope", "SELECT COUNT(*) FROM t JOIN nope ON t.AdvEngineID = nope.x"}) {
    auto result = session->Execute(sql);
    const auto detail = plan::GetSqlError(result.status());
    ASSERT_NE(detail, nullptr) << sql << ": " << result.status().ToString();
    EXPECT_EQ(detail->kind(), plan::SqlErrorDetail::Kind::kBind) << sql;
    EXPECT_EQ(result.status().message(), "table 'nope' does not exist") << sql;
  }
  // A sub-plan binds from J4 on, so a missing column inside one is that block's bind error.
  {
    auto result = session->Execute("SELECT COUNT(*) FROM (SELECT nope FROM t)");
    const auto detail = plan::GetSqlError(result.status());
    ASSERT_NE(detail, nullptr) << result.status().ToString();
    EXPECT_EQ(detail->kind(), plan::SqlErrorDetail::Kind::kBind);
    EXPECT_EQ(result.status().message(), "column 'nope' does not exist");
  }
  // An unreferenced CTE is never bound, as DuckDB never binds one, so a broken body answers.
  {
    auto result = session->Execute("WITH c AS (SELECT nope FROM nope) SELECT COUNT(*) FROM t");
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(Rows(*result), (std::vector<std::vector<std::string>>{{"10"}}));
  }
  // A table alias and a qualified name answer from J2b-2 on, so they are no longer rejected: an
  // explicit alias, an implicit one, and a qualifier of the table's own name.
  for (const char* sql : {"SELECT COUNT(*) FROM t AS a", "SELECT COUNT(*) FROM t over",
                          "SELECT COUNT(*) FROM t AS a WHERE a.AdvEngineID = a.AdvEngineID"}) {
    auto result = session->Execute(sql);
    ASSERT_TRUE(result.ok()) << sql << ": " << result.status().ToString();
    EXPECT_EQ(Rows(*result), (std::vector<std::vector<std::string>>{{"10"}})) << sql;
  }
  auto qualified = session->Execute("SELECT t.AdvEngineID FROM t");
  ASSERT_TRUE(qualified.ok()) << qualified.status().ToString();
  EXPECT_EQ(qualified->names, std::vector<std::string>{"AdvEngineID"});
  EXPECT_EQ(Rows(*qualified).size(), 10U);
  auto bind = session->Execute("SELECT SUM(nope) FROM t");
  const auto detail = plan::GetSqlError(bind.status());
  ASSERT_NE(detail, nullptr) << bind.status().ToString();
  EXPECT_EQ(detail->kind(), plan::SqlErrorDetail::Kind::kBind);
  // COUNT(*) with an alias is still answered from the footers.
  auto count = session->Execute("SELECT COUNT(*) AS n FROM t");
  ASSERT_TRUE(count.ok()) << count.status().ToString();
  EXPECT_EQ(count->names, std::vector<std::string>{"n"});
  EXPECT_EQ(Rows(*count), (std::vector<std::vector<std::string>>{{"10"}}));
}

// An unsupported correlation exits 4 from every entry point, not 1 and not 70 (ADR 0023's U1).
// Before Optimize could fail, only Execute rejected what the optimizer rejects; now `explain` and
// `explain --analyze` reject exactly what `query` does, since all three bind and optimize through
// one path.
TEST_F(SessionTest, CorrelatedSubqueriesAreUnsupportedInEveryEntryPoint) {
  auto session = Session::Make().ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("t", {path_}).ok());
  ASSERT_TRUE(session->RegisterParquet("u", {path_}).ok());
  for (const char* sql : {
           // A correlation the unnesting refuses: an outer column with no inner equivalent, which
           // only the domain D answers (ADR 0041's SQ9).
           "SELECT COUNT(*) FROM t WHERE AdvEngineID = "
           "(SELECT MIN(u.AdvEngineID) FROM u WHERE u.AdvEngineID < t.AdvEngineID)",
           // The shapes the binder itself rejects, each with its own rule.
           "SELECT COUNT(*) FROM t WHERE AdvEngineID > 0 OR AdvEngineID IN "
           "(SELECT u.AdvEngineID FROM u WHERE u.AdvEngineID = t.AdvEngineID)",
           "SELECT COUNT(*) FROM t WHERE AdvEngineID IN (SELECT t.AdvEngineID FROM u)",
           // A lateral reference: the derived table's enclosing scope is the FROM items before it,
           // not a query block's, so ADR 0022 refuses it (nothing pinned this before).
           "SELECT COUNT(*) FROM t, (SELECT u.AdvEngineID FROM u "
           "WHERE u.AdvEngineID = t.AdvEngineID) AS d",
       }) {
    const auto statuses = {session->Execute(sql).status(), session->Explain(sql).status(),
                           session->ExplainPhysical(sql).status(),
                           session->ExplainAnalyze(sql).status()};
    for (const arrow::Status& status : statuses) {
      const auto detail = plan::GetSqlError(status);
      ASSERT_NE(detail, nullptr) << sql << ": " << status.ToString();
      EXPECT_EQ(detail->kind(), plan::SqlErrorDetail::Kind::kUnsupported) << sql;
      EXPECT_TRUE(status.IsNotImplemented()) << sql << ": " << status.ToString();
    }
  }
  // An uncorrelated scalar subquery still answers, and so do the correlated forms the unnesting
  // pass now rewrites (roadmap ids U2 and U3; a correlated COUNT joined back LEFT, ADR 0041's SQ6),
  // through all three entry points.
  for (const char* answered :
       {"SELECT COUNT(*) FROM t WHERE AdvEngineID = (SELECT MIN(u.AdvEngineID) FROM u)",
        "SELECT COUNT(*) FROM t WHERE 0 = "
        "(SELECT COUNT(*) FROM u WHERE u.AdvEngineID = t.AdvEngineID)",
        "SELECT COUNT(*) FROM t WHERE AdvEngineID = "
        "(SELECT MIN(u.AdvEngineID) FROM u WHERE u.AdvEngineID = t.AdvEngineID)",
        "SELECT COUNT(*) FROM t WHERE AdvEngineID IN "
        "(SELECT u.AdvEngineID FROM u WHERE u.AdvEngineID = t.AdvEngineID)",
        "SELECT COUNT(*) FROM t WHERE AdvEngineID NOT IN "
        "(SELECT u.AdvEngineID FROM u WHERE u.AdvEngineID = t.AdvEngineID)",
        "SELECT COUNT(*) FROM t WHERE EXISTS "
        "(SELECT * FROM u WHERE u.AdvEngineID = t.AdvEngineID)"}) {
    EXPECT_TRUE(session->Execute(answered).ok()) << answered;
    EXPECT_TRUE(session->Explain(answered).ok()) << answered;
    EXPECT_TRUE(session->ExplainPhysical(answered).ok()) << answered;
    EXPECT_TRUE(session->ExplainAnalyze(answered).ok()) << answered;
  }
}

// The build-side ratio is checked, and it reaches the join order: the same query gets a different
// build side at a different ratio, and the same answer at every one of them (ADR 0026). Three
// bindings of the 10-row table join on one key, and the equality on `a` leaves the relations joined
// so far estimated at one row, so the relation added last is decisively larger than them.
TEST_F(SessionTest, TheBuildSideRatioReachesTheJoinOrderAndKeepsEveryAnswer) {
  for (const int64_t ratio : {int64_t{99}, int64_t{0}, int64_t{-1}}) {
    SessionOptions options;
    options.build_side_ratio = ratio;
    const auto made = Session::Make(options);
    ASSERT_TRUE(made.status().IsInvalid()) << ratio;
    EXPECT_NE(made.status().message().find("build-side ratio"), std::string::npos) << ratio;
  }
  const std::string sql =
      "SELECT COUNT(*) FROM t a, t b, t c "
      "WHERE a.AdvEngineID = b.AdvEngineID AND b.AdvEngineID = c.AdvEngineID "
      "AND a.AdvEngineID = 5";
  const auto lefts = [](const std::string& plan) {
    std::size_t n = 0;
    for (std::size_t at = plan.find("build=left"); at != std::string::npos;
         at = plan.find("build=left", at + 1)) {
      ++n;
    }
    return n;
  };
  std::optional<std::vector<std::vector<std::string>>> expected;
  std::map<int64_t, std::size_t> left_builds;
  for (const int64_t ratio : {int64_t{100}, int64_t{400}, int64_t{1'000'000}}) {
    SessionOptions options;
    options.build_side_ratio = ratio;
    auto session = Session::Make(options);
    ASSERT_TRUE(session.ok()) << ratio << ": " << session.status().ToString();
    ASSERT_TRUE((*session)->RegisterParquet("t", {path_}).ok());
    auto plan = (*session)->Explain(sql);
    ASSERT_TRUE(plan.ok()) << ratio << ": " << plan.status().ToString();
    left_builds[ratio] = lefts(*plan);
    auto result = (*session)->Execute(sql);
    ASSERT_TRUE(result.ok()) << ratio << ": " << result.status().ToString();
    if (!expected.has_value()) {
      expected = Rows(*result);
    } else {
      EXPECT_EQ(Rows(*result), *expected) << "ratio " << ratio;  // the answer never moves
    }
  }
  // Building on the smaller side always finds one here; trusting the estimates only beyond a
  // millionfold finds none. That difference is the whole chain -- SessionOptions to BindOptions to
  // OrderJoins -- so stubbing any link of it out fails this.
  EXPECT_GT(left_builds[100], 0U);
  EXPECT_EQ(left_builds[1'000'000], 0U);
  auto implicit = Session::Make();
  ASSERT_TRUE(implicit.ok()) << implicit.status().ToString();
  ASSERT_TRUE((*implicit)->RegisterParquet("t", {path_}).ok());
  const auto default_plan = (*implicit)->Explain(sql);
  ASSERT_TRUE(default_plan.ok()) << default_plan.status().ToString();
  EXPECT_EQ(lefts(*default_plan), left_builds[100]);
  const auto default_result = (*implicit)->Execute(sql);
  ASSERT_TRUE(default_result.ok()) << default_result.status().ToString();
  if (expected.has_value()) {
    EXPECT_EQ(Rows(*default_result), *expected);
  } else {
    FAIL() << "No expected result was collected from the explicit-ratio sessions";
  }
}

TEST_F(SessionTest, AnInSubqueryKeepsDuplicateRowsAndNullSemanticsWithEitherBuildSide) {
  const auto left_path = dir_ / "left.parquet";
  const auto right_path = dir_ / "right.parquet";
  arrow::Int32Builder left_keys;
  arrow::Int64Builder payload;
  ASSERT_TRUE(left_keys.AppendValues({1, 1, 2, 4}).ok());
  ASSERT_TRUE(left_keys.AppendNull().ok());
  ASSERT_TRUE(payload.AppendValues({10, 11, 20, 40, 50}).ok());
  const auto left = arrow::Table::Make(
      arrow::schema({arrow::field("k", arrow::int32()), arrow::field("v", arrow::int64())}),
      {left_keys.Finish().ValueOrDie(), payload.Finish().ValueOrDie()});
  arrow::Int64Builder right_keys;
  ASSERT_TRUE(right_keys.AppendValues({1, 1, 3}).ok());
  for (int64_t key = 10; key < 40; ++key) {
    ASSERT_TRUE(right_keys.Append(key).ok());
  }
  ASSERT_TRUE(right_keys.AppendNull().ok());
  const auto right = arrow::Table::Make(arrow::schema({arrow::field("k", arrow::int64())}),
                                        {right_keys.Finish().ValueOrDie()});
  for (const auto& [path, table] : {std::pair{left_path, left}, std::pair{right_path, right}}) {
    const auto output = arrow::io::FileOutputStream::Open(path.string()).ValueOrDie();
    ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), output, 2).ok());
    ASSERT_TRUE(output->Close().ok());
  }
  constexpr std::string_view kIn =
      "SELECT a.v FROM a WHERE a.k IN (SELECT b.k FROM b) ORDER BY a.v";
  for (const int64_t ratio : {int64_t{100}, int64_t{1'000'000}}) {
    auto session = Session::Make({.build_side_ratio = ratio});
    ASSERT_TRUE(session.ok()) << session.status().ToString();
    ASSERT_TRUE((*session)->RegisterParquet("a", {left_path.string()}).ok());
    ASSERT_TRUE((*session)->RegisterParquet("b", {right_path.string()}).ok());
    const auto plan = (*session)->Explain(kIn);
    ASSERT_TRUE(plan.ok()) << plan.status().ToString();
    EXPECT_EQ(plan->contains("SEMI build=left"), ratio == 100);
    for (const std::string_view sql : std::vector<std::string_view>{
             kIn,
             "SELECT a.v FROM a WHERE a.k + 0 IN (SELECT b.k FROM b) ORDER BY a.v",
             "SELECT a.v FROM a WHERE a.k = ANY (SELECT b.k FROM b) ORDER BY a.v",
         }) {
      const auto result = (*session)->Execute(sql);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_EQ(Rows(*result), (std::vector<std::vector<std::string>>{{"10"}, {"11"}}));
    }
    for (const std::string_view sql : {
             "SELECT a.v FROM a WHERE a.k IN (SELECT b.k FROM b WHERE b.k < 0)",
             "SELECT a.v FROM a WHERE a.k NOT IN (SELECT b.k FROM b)",
         }) {
      const auto result = (*session)->Execute(sql);
      ASSERT_TRUE(result.ok()) << result.status().ToString();
      EXPECT_TRUE(Rows(*result).empty());
    }
    const auto result = (*session)->Execute(
        "SELECT a.v FROM a WHERE a.k NOT IN (SELECT b.k FROM b WHERE b.k < 0) ORDER BY a.v");
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(Rows(*result),
              (std::vector<std::vector<std::string>>{{"10"}, {"11"}, {"20"}, {"40"}, {"50"}}));
  }
}

// The thread count is checked; the row groups of a query run on the session's threads, and the
// result is the same for any number of them.
TEST_F(SessionTest, ThreadsDoNotChangeResults) {
  for (const int threads : {0, -1, Session::kMaxThreads + 1}) {
    SessionOptions options;
    options.threads = threads;
    EXPECT_TRUE(Session::Make(options).status().IsInvalid()) << threads;
  }
  const std::vector<std::string> queries = {
      "SELECT COUNT(*), SUM(AdvEngineID), AVG(AdvEngineID) FROM t WHERE AdvEngineID > 1",
      "SELECT AdvEngineID, AdvEngineID * 2 FROM t WHERE AdvEngineID <> 5",
      "SELECT AdvEngineID FROM t LIMIT 3 OFFSET 4",
      "SELECT AdvEngineID % 3 AS k, COUNT(*) FROM t GROUP BY k",
      "SELECT AdvEngineID FROM t ORDER BY AdvEngineID DESC LIMIT 4",
  };
  std::vector<std::vector<std::vector<std::string>>> expected;
  for (const int threads : {1, 4}) {
    SessionOptions options;
    options.threads = threads;
    options.batch_size = 3;
    auto session = Session::Make(options).ValueOrDie();
    ASSERT_TRUE(session->RegisterParquet("t", {path_}).ok());
    for (std::size_t i = 0; i < queries.size(); ++i) {
      auto result = session->Execute(queries[i]);
      ASSERT_TRUE(result.ok()) << queries[i] << ": " << result.status().ToString();
      if (threads == 1) {
        expected.push_back(Rows(*result));
      } else {
        EXPECT_EQ(Rows(*result), expected[i]) << queries[i];
      }
    }
  }
  EXPECT_EQ(expected[2], (std::vector<std::vector<std::string>>{{"4"}, {"5"}, {"6"}}));
}

// The memory limit: a query that needs more fails with OutOfMemory; one that fits gives the same
// result as without a limit; a result keeps its memory valid after the session is gone.
TEST_F(SessionTest, ConditionalPreaggregationRetriesWithLessAvailableMemory) {
  const auto facts = (dir_ / "facts.parquet").string();
  arrow::Int16Builder keys;
  arrow::Int64Builder amounts;
  for (int i = 0; i < 32768; ++i) {
    ASSERT_TRUE(keys.Append(static_cast<int16_t>(i % 1024)).ok());
    ASSERT_TRUE(amounts.Append(i % 5).ok());
  }
  const auto table =
      arrow::Table::Make(arrow::schema({arrow::field("keycol", arrow::int16()),
                                        arrow::field("amount", arrow::int64())}),
                         {keys.Finish().ValueOrDie(), amounts.Finish().ValueOrDie()});
  const auto file = arrow::io::FileOutputStream::Open(facts).ValueOrDie();
  ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), file, 4096).ok());
  ASSERT_TRUE(file->Close().ok());
  constexpr int64_t kLimit = int64_t{16} * 1024 * 1024;
  auto session = Session::Make({.batch_size = 256, .memory_limit = kLimit}).ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("labels", {path_}).ok());
  ASSERT_TRUE(session->RegisterParquet("facts", {facts}).ok());

  const auto query = [](std::string_view measure) {
    return std::format(
        "SELECT d.EventDate, SUM(CASE WHEN d.AdvEngineID>0 THEN {} END) AS total "
        "FROM (SELECT keycol,amount FROM facts UNION ALL SELECT keycol,amount "
        "FROM facts) f JOIN labels d ON d.AdvEngineID=f.keycol GROUP BY d.EventDate",
        measure);
  };
  // Negating twice on this small fixture computes the same answer and keeps the original plan.
  const auto control = session->Execute(query("-(-f.amount)"));
  ASSERT_TRUE(control.ok()) << control.status();
  const auto optimized = session->ExplainAnalyze(query("f.amount"));
  ASSERT_TRUE(optimized.ok()) << optimized.status();
  constexpr int64_t kSlack = 8192;
  ASSERT_GT(optimized->peak_memory,
            control->peak_memory + kSlack + control->memory->bytes_allocated());
  // The planner sees the session limit. Outstanding Arrow buffers reduce the headroom to that
  // of the original plan. Execute and ExplainAnalyze must both succeed on their second attempt.
  const int64_t hold = kLimit - control->memory->bytes_allocated() - control->peak_memory - kSlack;
  ASSERT_GT(hold, 0);
  const auto held = arrow::AllocateBuffer(hold, control->memory.get());
  ASSERT_TRUE(held.ok()) << held.status();
  {
    const auto actual = session->Execute(query("f.amount"));
    ASSERT_TRUE(actual.ok()) << actual.status();
    EXPECT_TRUE(actual->table->Equals(*control->table));
  }
  const auto profile = session->ExplainAnalyze(query("f.amount"));
  ASSERT_TRUE(profile.ok()) << profile.status();
  EXPECT_EQ(profile->rows, control->table->num_rows());
}

TEST_F(SessionTest, MemoryLimit) {
  for (const int64_t limit : {int64_t{0}, int64_t{-5}}) {
    SessionOptions options;
    options.memory_limit = limit;
    EXPECT_TRUE(Session::Make(options).status().IsInvalid()) << limit;
  }
  const std::string sql = "SELECT AdvEngineID % 3 AS k, COUNT(*) FROM t GROUP BY k ORDER BY k";
  const auto run = [&](std::optional<int64_t> limit) {
    SessionOptions options;
    options.memory_limit = limit;
    options.threads = 2;
    auto session = Session::Make(options).ValueOrDie();
    EXPECT_TRUE(session->RegisterParquet("t", {path_}).ok());
    return session->Execute(sql);  // the session is destroyed before the result
  };
  const auto unlimited = run(std::nullopt);
  ASSERT_TRUE(unlimited.ok()) << unlimited.status().ToString();
  EXPECT_EQ(Rows(*unlimited),
            (std::vector<std::vector<std::string>>{{"0", "4"}, {"1", "3"}, {"2", "3"}}));
  const auto fits = run(int64_t{1024} * 1024 * 1024);
  ASSERT_TRUE(fits.ok()) << fits.status().ToString();
  EXPECT_EQ(Rows(*fits), Rows(*unlimited));
  ASSERT_NE(fits->memory, nullptr);
  EXPECT_GT(fits->memory->bytes_allocated(), 0);  // the result's buffers
  const auto tiny = run(64);
  EXPECT_TRUE(tiny.status().IsOutOfMemory()) << tiny.status().ToString();
  EXPECT_NE(tiny.status().message().find("memory limit of 64 bytes"), std::string::npos);
}

// QueryResult::peak_memory: each Execute starts its peak from the bytes held at the time. With one
// thread and no earlier result alive, that is the query's own peak, the same on every run and
// within the limit; a result the caller still holds adds its bytes to the next query's peak.
TEST_F(SessionTest, ExecuteReportsThePeakMemoryOfTheQuery) {
  SessionOptions options;
  options.memory_limit = int64_t{1024} * 1024 * 1024;
  auto session = Session::Make(options).ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("t", {path_}).ok());
  const std::string sorted = "SELECT AdvEngineID, EventDate FROM t ORDER BY AdvEngineID DESC";
  const std::string count = "SELECT COUNT(*) FROM t";  // the row count of the file's metadata
  const auto peak = [&](const std::string& sql) {      // the result is dropped on return
    const auto result = session->Execute(sql);
    EXPECT_TRUE(result.ok()) << result.status().ToString();
    return result.ok() ? result->peak_memory : -1;
  };
  const int64_t sorted_peak = peak(sorted);
  EXPECT_GT(sorted_peak, 0);
  EXPECT_LE(sorted_peak, *options.memory_limit);
  EXPECT_EQ(peak(sorted), sorted_peak);
  const auto profile = session->ExplainAnalyze(sorted);  // the same count, no pool allocations
  ASSERT_TRUE(profile.ok()) << profile.status().ToString();
  EXPECT_EQ(profile->peak_memory, sorted_peak);
  const int64_t count_peak = peak(count);
  EXPECT_GT(count_peak, 0);  // its builder's capacity
  EXPECT_LT(count_peak, sorted_peak);

  const auto held = session->Execute(sorted);
  ASSERT_TRUE(held.ok()) << held.status().ToString();
  const int64_t held_bytes = held->memory->bytes_allocated();
  EXPECT_GT(held_bytes, 0);
  // The peak, not the bytes still held at the end: the sort's input and buffers came first.
  EXPECT_GT(sorted_peak, held_bytes);
  const auto next = session->Execute(count);
  ASSERT_TRUE(next.ok()) << next.status().ToString();
  EXPECT_EQ(next->peak_memory, held_bytes + count_peak);
}

// A query that fails in its first row group while later ones hold buffers of the session's
// memory on the pool's threads: destroying the session right away must wait for those threads
// before the memory goes (ASan checks the frees).
TEST_F(SessionTest, DestroyedRightAfterAFailedParallelQuery) {
  for (int i = 0; i < 20; ++i) {
    SessionOptions options;
    options.threads = 4;
    options.memory_limit = int64_t{1024} * 1024 * 1024;
    auto session = Session::Make(options).ValueOrDie();
    ASSERT_TRUE(session->RegisterParquet("t", {path_}).ok());
    // 2 * 2^62 overflows BIGINT in the first row group (AdvEngineID 0..3).
    const auto failed = session->Execute("SELECT AdvEngineID * 4611686018427387904 FROM t");
    EXPECT_TRUE(failed.status().IsExecutionError()) << failed.status().ToString();
  }
}

// Row groups ruled out by their statistics are skipped without changing any answer: a file sorted
// by id in row groups of 100 rows, with a NULL-only row group in `v`.
TEST_F(SessionTest, SkippedRowGroupsDoNotChangeAnswers) {
  const std::string sorted = (dir_ / "sorted.parquet").string();
  {
    arrow::Int64Builder id;
    arrow::Int64Builder v;
    for (int64_t i = 0; i < 1000; ++i) {
      ASSERT_TRUE(id.Append(i).ok());
      ASSERT_TRUE((i >= 300 && i < 400 ? v.AppendNull() : v.Append(i % 7)).ok());
    }
    auto table = arrow::Table::Make(
        arrow::schema({arrow::field("id", arrow::int64()), arrow::field("v", arrow::int64())}),
        {id.Finish().ValueOrDie(), v.Finish().ValueOrDie()});
    auto out = arrow::io::FileOutputStream::Open(sorted).ValueOrDie();
    ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out, 100).ok());
    ASSERT_TRUE(out->Close().ok());
  }
  for (const int threads : {1, 4}) {
    SessionOptions options;
    options.threads = threads;
    auto session = Session::Make(options).ValueOrDie();
    ASSERT_TRUE(session->RegisterParquet("s", {sorted}).ok());
    const auto rows = [&](const std::string& sql) {
      auto result = session->Execute(sql);
      EXPECT_TRUE(result.ok()) << sql << ": " << result.status().ToString();
      return result.ok() ? Rows(*result) : std::vector<std::vector<std::string>>{};
    };
    using R = std::vector<std::vector<std::string>>;
    EXPECT_EQ(rows("SELECT COUNT(*), MIN(id), MAX(id) FROM s WHERE id >= 250 AND id < 260"),
              (R{{"10", "250", "259"}}));
    EXPECT_EQ(rows("SELECT id FROM s WHERE id IN (5, 777, 5000) ORDER BY id"), (R{{"5"}, {"777"}}));
    EXPECT_EQ(rows("SELECT COUNT(*) FROM s WHERE id > 999"), (R{{"0"}}));
    EXPECT_EQ(rows("SELECT COUNT(*) FROM s WHERE v = 3 AND id >= 300 AND id < 400"), (R{{"0"}}));
    EXPECT_EQ(rows("SELECT COUNT(*), SUM(id) FROM s WHERE v >= 0"), (R{{"900", "464550"}}));
    EXPECT_EQ(rows("SELECT id FROM s WHERE id <> 0 ORDER BY id LIMIT 2"), (R{{"1"}, {"2"}}));
    // GROUP BY, top-N and COUNT(DISTINCT) over kept parts only, and over none.
    EXPECT_EQ(
        rows("SELECT v, COUNT(*) FROM s WHERE id >= 250 AND id < 260 GROUP BY v ORDER BY v"),
        (R{{"0", "2"}, {"1", "1"}, {"2", "1"}, {"3", "1"}, {"4", "1"}, {"5", "2"}, {"6", "2"}}));
    EXPECT_EQ(rows("SELECT id FROM s WHERE id >= 850 ORDER BY id DESC LIMIT 2"),
              (R{{"999"}, {"998"}}));
    EXPECT_EQ(rows("SELECT COUNT(DISTINCT v) FROM s WHERE id >= 850"), (R{{"7"}}));
    EXPECT_EQ(rows("SELECT v, COUNT(*) FROM s WHERE id > 999 GROUP BY v"), R{});
    EXPECT_EQ(rows("SELECT id FROM s WHERE id > 999 ORDER BY id LIMIT 3"), R{});
    EXPECT_EQ(rows("SELECT COUNT(DISTINCT v) FROM s WHERE id > 999"), (R{{"0"}}));
    // Filters on v alone (table field 1 is the scan's column 0) and next to a computed condition.
    EXPECT_EQ(rows("SELECT COUNT(*) FROM s WHERE v = 3"), (R{{"129"}}));
    EXPECT_EQ(rows("SELECT COUNT(*) FROM s WHERE v >= 7"), (R{{"0"}}));
    EXPECT_EQ(rows("SELECT COUNT(*) FROM s WHERE id + v > 5 AND id < 50"), (R{{"47"}}));
  }
}

TEST_F(SessionTest, SupportedWindowsAcrossSessionEntryPoints) {
  auto session = Session::Make().ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("t", {path_}).ok());
  constexpr const char* kRowsPrefixExists =
      "SELECT AdvEngineID FROM t WHERE EXISTS (SELECT COUNT(*) OVER (ROWS UNBOUNDED PRECEDING) "
      "FROM t)";
  constexpr const char* kUnusedRowsPrefix =
      "WITH unused AS (SELECT COUNT(*) OVER (ROWS UNBOUNDED PRECEDING) FROM no_such_table) "
      "SELECT COUNT(*) FROM t";
  for (const char* text :
       {"SELECT SUM(AdvEngineID) OVER (ROWS UNBOUNDED PRECEDING) FROM t WHERE AdvEngineID < 0",
        "SELECT COUNT(*) OVER (ROWS UNBOUNDED PRECEDING) FROM t LIMIT 0",
        "SELECT COUNT(*) FROM t ORDER BY SUM(COUNT(*)) OVER (ROWS UNBOUNDED PRECEDING)",
        kRowsPrefixExists,
        kUnusedRowsPrefix,
        "SELECT RANK() OVER () FROM t LIMIT 0",
        "SELECT COUNT(*) FROM t ORDER BY RANK() OVER ()",
        "SELECT RANK() OVER (ORDER BY AdvEngineID DESC NULLS FIRST) FROM t",
        "SELECT COUNT(*) FROM (SELECT RANK() OVER () AS x FROM t) AS q",
        "SELECT AdvEngineID FROM t WHERE EXISTS (SELECT RANK() OVER () FROM t)",
        "SELECT CASE WHEN AdvEngineID > 0 THEN 1 ELSE RANK() OVER () END FROM t",
        "WITH unused AS (SELECT RANK() OVER () FROM no_such_table) SELECT COUNT(*) FROM t",
        "SELECT COUNT(*) FROM t UNION ALL SELECT RANK() OVER () FROM t",
        "SELECT COUNT(*) OVER () FROM t",
        "SELECT COUNT(*) FROM t ORDER BY COUNT(*) OVER () LIMIT 0",
        "SELECT COUNT(*) FROM t ORDER BY SUM(COUNT(*)) OVER ()",
        "SELECT COUNT(*) FROM (SELECT COUNT(*) OVER () AS n FROM t) AS q",
        "SELECT AdvEngineID FROM t WHERE EXISTS (SELECT COUNT(*) OVER () FROM t)",
        "SELECT COUNT(*) FROM t UNION ALL SELECT COUNT(*) OVER () FROM t",
        "WITH unused AS (SELECT COUNT(*) OVER () FROM t) SELECT COUNT(*) FROM t"}) {
    SCOPED_TRACE(text);
    EXPECT_TRUE(session->Execute(text).ok());
    EXPECT_TRUE(session->Explain(text).ok());
    EXPECT_TRUE(session->ExplainPhysical(text).ok());
    EXPECT_TRUE(session->ExplainAnalyze(text).ok());
  }
  const auto output = session->Execute("SELECT COUNT(*) FROM t ORDER BY SUM(COUNT(*)) OVER ()");
  ASSERT_TRUE(output.ok()) << output.status();
  EXPECT_EQ(output->table->num_columns(), 1);
  EXPECT_EQ(output->table->num_rows(), 1);
  const auto physical = session->ExplainPhysical("SELECT COUNT(*) OVER () FROM t");
  ASSERT_TRUE(physical.ok()) << physical.status();
  EXPECT_NE(physical->find("Window"), std::string::npos);
}

TEST_F(SessionTest, WindowExecutionBoundaryCoversAllEntryPoints) {
  constexpr auto kUnusedDeferred =
      "WITH unused AS (SELECT SUM(AdvEngineID) OVER (ORDER BY AdvEngineID) FROM t) SELECT "
      "COUNT(*) FROM t";
  auto session = Session::Make().ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("t", {path_}).ok());
  for (const char* text :
       {"SELECT DENSE_RANK() OVER () FROM t LIMIT 0",
        "SELECT COUNT(*) FROM t ORDER BY DENSE_RANK() OVER ()",
        "SELECT SUM(AdvEngineID) OVER (ROWS 1 PRECEDING) FROM t WHERE AdvEngineID < 0",
        "SELECT COUNT(*) FROM (SELECT DENSE_RANK() OVER () AS x FROM t) AS q",
        "SELECT AdvEngineID FROM t WHERE EXISTS (SELECT DENSE_RANK() OVER () FROM t)",
        "SELECT CASE WHEN AdvEngineID > 0 THEN 1 ELSE DENSE_RANK() OVER () END FROM t",
        "WITH unused AS (SELECT DENSE_RANK() OVER () FROM no_such_table) SELECT COUNT(*) FROM t",
        kUnusedDeferred,
        "WITH unused AS (SELECT AdvEngineID FROM t WINDOW w AS ()) SELECT COUNT(*) FROM t",
        "SELECT COUNT(*) FROM t UNION ALL SELECT DENSE_RANK() OVER () FROM t"}) {
    SCOPED_TRACE(text);
    for (const arrow::Status& status :
         {session->Execute(text).status(), session->Explain(text).status(),
          session->ExplainPhysical(text).status(), session->ExplainAnalyze(text).status()}) {
      ASSERT_FALSE(status.ok());
      const auto detail = plan::GetSqlError(status);
      ASSERT_NE(detail, nullptr) << status;
      EXPECT_EQ(detail->kind(), plan::SqlErrorDetail::Kind::kUnsupported) << status;
      EXPECT_NE(status.message().find('W'), std::string::npos) << status;
    }
  }
}

TEST_F(SessionTest, DeferredWindowOperandsInUnusedCtesStayUnsupported) {
  auto session = Session::Make().ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("t", {path_}).ok());
  const std::vector<std::pair<std::string, std::string>> operands{
      {"COUNT(AdvEngineID = 1) OVER ()", "W11b"},
      {"COUNT(*) OVER (PARTITION BY AdvEngineID = 1)", "W11b"},
      {"SUM(sin(AdvEngineID)) OVER ()", "W11e"},
      {"RANK() OVER (PARTITION BY AdvEngineID = 1)", "W11b"},
      {"RANK() OVER (ORDER BY AdvEngineID = 1)", "W11b"},
      {"RANK() OVER (ORDER BY sin(AdvEngineID))", "W11e"}};
  for (const auto& [expression, owner] : operands) {
    const std::string text =
        "WITH unused AS (SELECT " + expression + " FROM no_such_table) SELECT COUNT(*) FROM t";
    SCOPED_TRACE(text);
    for (const arrow::Status& status :
         {session->Execute(text).status(), session->Explain(text).status(),
          session->ExplainPhysical(text).status(), session->ExplainAnalyze(text).status()}) {
      ASSERT_FALSE(status.ok());
      const auto detail = plan::GetSqlError(status);
      ASSERT_NE(detail, nullptr) << status;
      EXPECT_EQ(detail->kind(), plan::SqlErrorDetail::Kind::kUnsupported) << status;
      EXPECT_NE(status.message().find(owner), std::string::npos) << status;
    }
  }
}

}  // namespace
}  // namespace antb1::engine
