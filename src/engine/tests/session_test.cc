#include "antb1/engine/session.h"

#include <filesystem>
#include <memory>
#include <string>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>

#include "antb1/plan/sql_status.h"

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

// The binder accepts the whole grammar; the executor answers only COUNT(*) without WHERE so far,
// so everything else is Unsupported (exit code 4), not an internal error.
TEST_F(SessionTest, QueriesTheExecutorCannotRunYetAreUnsupported) {
  auto session = Session::Make().ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("t", {path_}).ok());
  for (const char* sql :
       {"SELECT * FROM t", "SELECT AdvEngineID FROM t LIMIT 3", "SELECT SUM(AdvEngineID) FROM t",
        "SELECT COUNT(*) FROM t WHERE AdvEngineID <> 70000", "SELECT COUNT(*) FROM t LIMIT 1"}) {
    auto result = session->Execute(sql);
    ASSERT_FALSE(result.ok()) << sql;
    const auto detail = plan::GetSqlError(result.status());
    ASSERT_NE(detail, nullptr) << sql << ": " << result.status().ToString();
    EXPECT_EQ(detail->kind(), plan::SqlErrorDetail::Kind::kUnsupported) << sql;
    EXPECT_TRUE(result.status().IsNotImplemented()) << sql;
  }
  // Bind errors come first.
  auto bind = session->Execute("SELECT SUM(nope) FROM t");
  const auto detail = plan::GetSqlError(bind.status());
  ASSERT_NE(detail, nullptr) << bind.status().ToString();
  EXPECT_EQ(detail->kind(), plan::SqlErrorDetail::Kind::kBind);
  // COUNT(*) with an alias is still answered from the footers.
  auto count = session->Execute("SELECT COUNT(*) AS n FROM t");
  ASSERT_TRUE(count.ok()) << count.status().ToString();
  EXPECT_EQ(count->names, std::vector<std::string>{"n"});
}

}  // namespace
}  // namespace antb1::engine
