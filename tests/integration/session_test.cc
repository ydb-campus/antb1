// engine::Session end to end over every fixture table (label integration).

#include "antb1/engine/session.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>
#include <parquet/column_reader.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>

#include "antb1/exec/profile.h"

#ifdef ANTB1_INTEGRATION_HAVE_DUCKDB
#include <duckdb.h>
#endif

#include "antb1/plan/sql_status.h"
#include "antb1/plan/types.h"

#include "integration_util.h"

namespace antb1::integration {
namespace {

struct FixtureTable {
  std::string_view name;
  std::string_view file;  // relative to the fixtures directory; may be a glob
  int64_t rows;
  bool has_event_date;
};

constexpr auto kTables = std::to_array<FixtureTable>({
    {.name = "hits_like", .file = "hits_like.parquet", .rows = 10'000, .has_event_date = true},
    {.name = "hits_like_nulls",
     .file = "hits_like_nulls.parquet",
     .rows = 10'000,
     .has_event_date = true},
    {.name = "hits_like_split",
     .file = "hits_like_split/part-*.parquet",
     .rows = 10'000,
     .has_event_date = true},
    {.name = "hits_like_required",
     .file = "hits_like_required.parquet",
     .rows = 10'000,
     .has_event_date = true},
    {.name = "edge", .file = "edge.parquet", .rows = 12, .has_event_date = false},
    {.name = "empty", .file = "empty.parquet", .rows = 0, .has_event_date = true},
});

TEST(Session, CountStarOverEveryFixtureTable) {
  auto session = NewSession();
  ASSERT_NE(session, nullptr);
  for (const auto& t : kTables) {
    ASSERT_TRUE(session->RegisterParquet(std::string(t.name), {Fixture(t.file)}).ok()) << t.name;
    const std::string sql = "SELECT COUNT(*) FROM " + std::string(t.name);
    auto result = session->Execute(sql);
    ASSERT_TRUE(result.ok()) << t.name << ": " << result.status().ToString();
    EXPECT_EQ(result->names, std::vector<std::string>{"count_star()"});
    EXPECT_EQ(result->types, std::vector<plan::LogicalType>{plan::LogicalType::kBigInt});
    auto count = Count(*session, sql);
    ASSERT_TRUE(count.ok()) << count.status().ToString();
    EXPECT_EQ(*count, t.rows) << t.name;
  }
}

TEST(Session, CountStarFromPaths) {
  auto session = NewSession();
  ASSERT_NE(session, nullptr);
  for (const auto& t : kTables) {
    auto count = Count(*session, FromPath(Fixture(t.file)));
    ASSERT_TRUE(count.ok()) << t.name << ": " << count.status().ToString();
    EXPECT_EQ(*count, t.rows) << t.name;
  }
}

TEST(Session, NamesMatchCaseInsensitivelyAlsoWhenQuoted) {
  auto session = NewSession();
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(session->RegisterParquet("Hits_Like", {Fixture("hits_like.parquet")}).ok());
  for (const char* sql :
       {"SELECT COUNT(*) FROM hits_like", "select count(*) from HITS_LIKE",
        "SELECT COUNT(*) FROM \"hits_like\"", "SELECT COUNT(*) FROM \"HITS_LIKE\";"}) {
    auto count = Count(*session, sql);
    ASSERT_TRUE(count.ok()) << sql << ": " << count.status().ToString();
    EXPECT_EQ(*count, 10'000) << sql;
  }
  EXPECT_FALSE(session->RegisterParquet("HITS_LIKE", {Fixture("edge.parquet")}).ok())
      << "a second table with the same name (ignoring case) must be rejected";
}

TEST(Session, ClickBenchOverrideReadsEventDateAsDate) {
  for (const bool clickbench : {false, true}) {
    auto session = NewSession(clickbench);
    ASSERT_NE(session, nullptr);
    for (const auto& t : kTables) {
      ASSERT_TRUE(session->RegisterParquet(std::string(t.name), {Fixture(t.file)}).ok()) << t.name;
      const auto table = session->catalog().Find(t.name);
      ASSERT_NE(table, nullptr) << t.name;
      const auto field = table->schema()->GetFieldByName("EventDate");
      ASSERT_EQ(field != nullptr, t.has_event_date) << t.name;
      if (field != nullptr) {
        EXPECT_EQ(field->type()->id(), clickbench ? arrow::Type::DATE32 : arrow::Type::UINT16)
            << t.name << " clickbench=" << clickbench;
      }
    }
  }
}

TEST(Session, ExplainShowsTheRowCountPlan) {
  auto session = NewSession();
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(
      session->RegisterParquet("hits_like_split", {Fixture("hits_like_split/part-*.parquet")})
          .ok());
  auto text = session->Explain("SELECT COUNT(*) FROM hits_like_split");
  ASSERT_TRUE(text.ok()) << text.status().ToString();
  EXPECT_NE(text->find("RowCount table=hits_like_split"), std::string::npos) << *text;
  EXPECT_NE(text->find("files=4"), std::string::npos) << *text;
  EXPECT_NE(text->find("rows=10000"), std::string::npos) << *text;
}

TEST(Session, ErrorKindsAtTheBoundary) {
  auto session = NewSession();
  ASSERT_NE(session, nullptr);
  ASSERT_TRUE(session->RegisterParquet("hits_like", {Fixture("hits_like.parquet")}).ok());
  const auto kind = [&](const char* sql) {
    const auto status = session->Execute(sql).status();
    const auto detail = plan::GetSqlError(status);
    EXPECT_NE(detail, nullptr) << sql << ": " << status.ToString();
    return detail != nullptr ? detail->kind() : plan::SqlErrorDetail::Kind::kParse;
  };
  EXPECT_EQ(kind("SELECT COUNT(*) FROM"), plan::SqlErrorDetail::Kind::kParse);
  EXPECT_EQ(kind("SELECT COUNT(*) FROM no_such_table"), plan::SqlErrorDetail::Kind::kBind);
  EXPECT_EQ(kind("SELECT SUM(AdvEngineID) FROM hits_like GROUP BY lower(URL)"),
            plan::SqlErrorDetail::Kind::kUnsupported);
  EXPECT_EQ(kind("SELECT row_number() OVER () FROM no_such_table"),
            plan::SqlErrorDetail::Kind::kUnsupported)
      << "unsupported SQL is rejected before any name is resolved";
  EXPECT_EQ(kind("SELECT SUM(AdvEngineID) FROM hits_like GROUP BY 1"),
            plan::SqlErrorDetail::Kind::kBind)
      << "GROUP BY 1 refers to the aggregate";
  EXPECT_TRUE(session->Execute(FromPath(Fixture("no_such_file.parquet"))).status().IsIOError());
  // The session stays usable after every error.
  auto count = Count(*session, "SELECT COUNT(*) FROM hits_like");
  ASSERT_TRUE(count.ok()) << count.status().ToString();
  EXPECT_EQ(*count, 10'000);
}

// A build holds only the columns read above its join (ADR 0026's C8a), **through the whole chain**:
// SQL, the binder, Prune's set, the physical planner's verification of it and the build itself. The
// `cols_dropped` metric is the only place that chain is observable, and it is reachable only from a
// session -- which is why this guard lives here rather than beside either half.
//
// It earned the place. The planner refuses the whole set when an id in it is not one the join
// outputs, and the demand Prune carries is passed down both branches of every join, so at a nested
// join it also holds an ancestor's other branch's ids: the set was refused on exactly the joins it
// is worth the most on, every plan still printed the same, every answer was right, and 2,887 tests
// passed. A query of three relations is the smallest shape that has a nested join at all.
TEST(Session, ANestedJoinsBuildDropsWhatNothingReadsAboveIt) {
  auto session = engine::Session::Make().ValueOrDie();
  // One fixture under three names: a self-join needs no second file and the shape is what matters.
  for (const std::string_view name : {"a", "b", "c"}) {
    ASSERT_TRUE(session->RegisterParquet(std::string(name), {Fixture("hits_like.parquet")}).ok())
        << name;
  }
  // The lower join is a.WatchID = b.WatchID and the upper one joins c on a.UserID -- both keys are
  // all but unique in this fixture, so neither join multiplies its input. Only a.RegionID is read
  // above, so b.RegionID reaches a build through the Filter that needs it and dies there.
  const auto profile = session->ExplainAnalyze(
      "SELECT a.RegionID, COUNT(*) FROM a, b, c "
      "WHERE a.WatchID = b.WatchID AND a.UserID = c.UserID AND b.RegionID > 100 "
      "GROUP BY a.RegionID");
  ASSERT_TRUE(profile.ok()) << profile.status().ToString();
  ASSERT_NE(profile->root, nullptr);
  // Every HashBuild of the run with its depth, so that the assertion names which one dropped: the
  // deeper build is the nested join's, and a plan whose join order flipped would fail here rather
  // than keep passing while covering nothing.
  std::vector<std::pair<int, int64_t>> builds;  // depth, cols_dropped
  const auto walk = [&](const auto& self, const exec::ProfileNode& node, int depth) -> void {
    if (node.name() == "HashBuild") {
      int64_t dropped = 0;
      for (const exec::ProfileMetric& metric : node.metrics()) {
        if (metric.name == "cols_dropped") {
          dropped = metric.value;
        }
      }
      builds.emplace_back(depth, dropped);
    }
    for (const exec::ProfileNode* child : node.children()) {
      self(self, *child, depth + 1);
    }
  };
  walk(walk, *profile->root, 0);
  ASSERT_EQ(builds.size(), 2U) << "one per join";
  std::ranges::sort(builds);
  EXPECT_EQ(builds.front().second, 0)
      << "the upper join's build: every column of c is a key or read";
  EXPECT_EQ(builds.back().second, 1)
      << "the nested join's build dropped nothing: the retain set did not survive the chain";
}

#ifdef ANTB1_INTEGRATION_HAVE_DUCKDB
// A VARCHAR column read so that a pushed predicate can be applied to it, and by nothing above the
// scan, is read as dictionary indices and never copied into a batch (ADR 0020). **On a file whose
// column chunks carry no `encoding_stats`**, which is the case the change exists for and the only
// one no other test can reach: Arrow exposes a dictionary only for a chunk it proves fully
// dictionary-encoded and proves it from that optional field alone, so its own door declines here
// and `io` decides from the required `encodings` instead.
//
// Arrow's writer always emits `encoding_stats`, so the file is written by the test oracle, which
// never does -- the same way scripts/static_gates.py makes its smoke-test files.
TEST(Session, ADictionaryPredicateOnAFileWithNoEncodingStats) {
  const auto dir = std::filesystem::path(::testing::TempDir()) / "antb1_no_encoding_stats";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const std::string path = (dir / "t.parquet").string();
  {
    duckdb_database db = nullptr;
    duckdb_connection conn = nullptr;
    ASSERT_EQ(duckdb_open(nullptr, &db), DuckDBSuccess);
    ASSERT_EQ(duckdb_connect(db, &conn), DuckDBSuccess);
    // Four distinct values over 20,000 rows: a dictionary that never outgrows its page, and a
    // numeric column that is read above the scan so that the batches carry something.
    // One thread: a test adds no parallelism of its own (AGENTS.md's hermetic rules).
    const std::string sql =
        "SET threads TO 1; COPY (SELECT ('v' || (i % 4))::VARCHAR AS s, i::BIGINT AS n FROM "
        "range(20000) t(i)) TO '" +
        path + "' (FORMAT parquet, ROW_GROUP_SIZE 5000)";
    duckdb_result result;
    const auto state = duckdb_query(conn, sql.c_str(), &result);
    duckdb_destroy_result(&result);
    duckdb_disconnect(&conn);
    duckdb_close(&db);
    ASSERT_EQ(state, DuckDBSuccess) << "the oracle writes the file";
  }
  // The situation, asserted rather than assumed: no encoding_stats, so Arrow's door declines.
  {
    auto reader = parquet::ParquetFileReader::OpenFile(path, /*memory_map=*/false);
    ASSERT_GT(reader->metadata()->num_row_groups(), 0);
    const auto chunk = reader->metadata()->RowGroup(0)->ColumnChunk(0);
    EXPECT_TRUE(chunk->encoding_stats().empty()) << "this writer emits none";
    auto group = reader->RowGroup(0);
    EXPECT_NE(group->ColumnWithExposeEncoding(0, parquet::ExposedEncoding::DICTIONARY)
                  ->GetExposedEncoding(),
              parquet::ExposedEncoding::DICTIONARY)
        << "Arrow declines without encoding_stats, which is what this test is about";
  }
  auto session = engine::Session::Make().ValueOrDie();
  ASSERT_TRUE(session->RegisterParquet("t", {path}).ok());
  const std::string sql = "SELECT SUM(n) FROM t WHERE s = 'v1'";
  const auto result = session->Execute(sql);
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  ASSERT_EQ(result->table->num_rows(), 1);
  // i % 4 == 1 over 0..19999: 1 + 5 + ... + 19997, which is 5000 terms averaging 9999.
  const auto sum = std::static_pointer_cast<arrow::Int64Array>(result->table->column(0)->chunk(0));
  ASSERT_EQ(sum->length(), 1);
  EXPECT_EQ(sum->Value(0), 49'995'000);

  const auto profile = session->ExplainAnalyze(sql);
  ASSERT_TRUE(profile.ok()) << profile.status().ToString();
  ASSERT_NE(profile->root, nullptr);
  const exec::ProfileNode* scan = FindNode(*profile->root, "Scan");
  ASSERT_NE(scan, nullptr);
  EXPECT_EQ(MetricOf(*scan, "cols_filter_only"), 1)
      << "s is read to be compared and nothing above the scan reads it";
  EXPECT_GT(MetricOf(*scan, "dict_pieces").value_or(0), 0)
      << "its predicate was asked once per distinct value, which Arrow's door alone would refuse";
}
#endif  // ANTB1_INTEGRATION_HAVE_DUCKDB

}  // namespace
}  // namespace antb1::integration
