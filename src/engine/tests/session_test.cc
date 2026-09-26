#include "antb1/engine/session.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
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

TEST_F(SessionTest, CountStarFromPath) {
  auto session = Session::Make().ValueOrDie();
  auto result = session->Execute("SELECT COUNT(*) FROM '" + path_ + "'");
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  EXPECT_EQ(
      std::static_pointer_cast<arrow::Int64Array>(result->table->column(0)->chunk(0))->Value(0),
      10);
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
}

TEST_F(SessionTest, UnsupportedAndBindErrorsKeepTheirKinds) {
  auto session = Session::Make().ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("t", {path_}).ok());
  for (const char* sql :
       {"SELECT COUNT(*) FROM t GROUP BY AdvEngineID", "SELECT AdvEngineID FROM t ORDER BY 1",
        "SELECT DISTINCT AdvEngineID FROM t"}) {
    auto result = session->Execute(sql);
    const auto detail = plan::GetSqlError(result.status());
    ASSERT_NE(detail, nullptr) << sql << ": " << result.status().ToString();
    EXPECT_EQ(detail->kind(), plan::SqlErrorDetail::Kind::kUnsupported) << sql;
    EXPECT_TRUE(result.status().IsNotImplemented()) << sql;
  }
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

}  // namespace
}  // namespace antb1::engine
