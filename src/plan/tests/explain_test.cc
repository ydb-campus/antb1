#include "antb1/plan/explain.h"

#include <memory>
#include <string>
#include <string_view>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/plan/logical_plan.h"
#include "antb1/plan/optimizer.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::BindSql;
using testing::FakeTable;
using testing::MakeCatalog;

std::string ExplainSql(std::string_view sql, bool optimize = true) {
  static const Catalog catalog = MakeCatalog();
  auto plan = BindSql(sql, catalog);
  EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  if (!plan.ok()) {
    return {};
  }
  return Explain(optimize ? Optimize(*plan) : *plan);
}

TEST(ExplainTest, RowCount) {
  EXPECT_EQ(ExplainSql("SELECT COUNT(*) AS n FROM t"),
            "Output: n:BIGINT\n"
            "RowCount table=t source=fake\n");
}

TEST(ExplainTest, BoundPlanScansEveryField) {
  EXPECT_EQ(ExplainSql("SELECT COUNT(*) FROM t", /*optimize=*/false),
            "Output: count_star():BIGINT\n"
            "Aggregate COUNT(*)\n"
            "  Scan table=t source=fake columns=[i16, i32, i64, u16, h, d, s, dt, bad, "
            "\"Mixed Case\", from]\n");
}

TEST(ExplainTest, AggregatesOverAFilter) {
  EXPECT_EQ(ExplainSql("SELECT COUNT(*), SUM(i16) AS total, AVG(d), MIN(\"Mixed Case\") FROM t "
                       "WHERE i16 <> 0 AND dt >= '2013-07-01' AND d < 0.5 AND i64 > 1.5 "
                       "AND u16 < 70000 AND i32 = 0.5 AND s = 'it''s'"),
            "Output: count_star():BIGINT total:HUGEINT avg(d):DOUBLE "
            "min(\"Mixed Case\"):INTEGER\n"
            "Aggregate COUNT(*), SUM(i16), AVG(d), MIN(\"Mixed Case\")\n"
            "  Filter i16 <> 0 AND dt >= DATE '2013-07-01' AND d < 0.5 AND i64 >= 2 AND "
            "u16 IS NOT NULL AND FALSE AND s = 'it''s'\n"
            "    Scan table=t source=fake columns=[i16, i64, u16, d, s, dt, \"Mixed Case\"]\n");
}

TEST(ExplainTest, ProjectionWithLimit) {
  EXPECT_EQ(ExplainSql("SELECT * FROM ok LIMIT 10"),
            "Output: i16:SMALLINT s:VARCHAR\n"
            "Project i16, s\n"
            "  Limit 10\n"
            "    Scan table=ok source=fake columns=[i16, s]\n");
  EXPECT_EQ(ExplainSql("SELECT s AS \"The S\", I16 FROM OK WHERE i16 >= -32768.5 LIMIT 0"),
            "Output: The S:VARCHAR i16:SMALLINT\n"
            "Project s, i16\n"
            "  Limit 0\n"
            "    Filter i16 >= -32768\n"
            "      Scan table=OK source=fake columns=[i16, s]\n");
}

// The Limit moves below the Project, right above the Sort (a top-N in the executor); the Sort
// reads a column the query does not select and a hidden aggregate.
TEST(ExplainTest, SortAndOffset) {
  EXPECT_EQ(ExplainSql("SELECT s FROM ok ORDER BY i16 DESC, s NULLS FIRST LIMIT 3 OFFSET 2"),
            "Output: s:VARCHAR\n"
            "Project s\n"
            "  Limit 3 OFFSET 2\n"
            "    Sort i16 DESC NULLS LAST, s ASC NULLS FIRST\n"
            "      Scan table=ok source=fake columns=[i16, s]\n");
  EXPECT_EQ(ExplainSql("SELECT i16 FROM ok GROUP BY i16 ORDER BY COUNT(*) DESC OFFSET 1"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Limit ALL OFFSET 1\n"
            "    Sort \"count_star()\" DESC NULLS LAST\n"
            "      GroupAggregate keys=[i16] COUNT(*)\n"
            "        Scan table=ok source=fake columns=[i16]\n");
}

TEST(ExplainTest, In) {
  EXPECT_EQ(ExplainSql("SELECT i16 FROM ok WHERE i16 IN (3, 1.5, 7) AND s NOT IN ('x', 'it''s')"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter i16 IN (3, 7) AND s NOT IN ('x', 'it''s')\n"
            "    Scan table=ok source=fake columns=[i16, s]\n");
}

TEST(ExplainTest, Like) {
  EXPECT_EQ(ExplainSql("SELECT i16 FROM ok WHERE s LIKE '%it''s_%' AND s NOT LIKE ''"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter s LIKE '%it''s_%' AND s NOT LIKE ''\n"
            "    Scan table=ok source=fake columns=[i16, s]\n");
}

TEST(ExplainTest, ConstantsAndPositions) {
  EXPECT_EQ(ExplainSql("SELECT 1, 'x', s, COUNT(*) FROM ok GROUP BY 1, 3 ORDER BY 4 DESC"),
            "Output: 1:INTEGER 'x':VARCHAR s:VARCHAR count_star():BIGINT\n"
            "Project 1, 'x', s, \"count_star()\"\n"
            "  Sort \"count_star()\" DESC NULLS LAST\n"
            "    GroupAggregate keys=[s] COUNT(*)\n"
            "      Scan table=ok source=fake columns=[s]\n");
  EXPECT_EQ(ExplainSql("SELECT 2 FROM ok"),
            "Output: 2:INTEGER\n"
            "Project 2\n"
            "  Scan table=ok source=fake columns=[]\n");
}

TEST(ExplainTest, CountDistinct) {
  EXPECT_EQ(ExplainSql("SELECT COUNT(DISTINCT s), COUNT(s) FROM ok"),
            "Output: count(DISTINCT s):BIGINT count(s):BIGINT\n"
            "Aggregate COUNT(DISTINCT s), COUNT(s)\n"
            "  Scan table=ok source=fake columns=[s]\n");
}

TEST(ExplainTest, NamesAndStringsStayOnOneAsciiLine) {
  const auto table = std::make_shared<FakeTable>(
      arrow::schema({arrow::field("a\"b", arrow::binary()), arrow::field("é\n", arrow::int32())}),
      1);
  Catalog catalog;
  ASSERT_TRUE(catalog.Register("w", table).ok());
  auto plan = BindSql("SELECT \"a\"\"b\", \"é\n\" FROM w WHERE \"a\"\"b\" = 'x\ty\\z'", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(Explain(Optimize(*plan)),
            "Output: a\"b:VARCHAR \\xC3\\xA9\\x0A:INTEGER\n"
            "Project \"a\"\"b\", \"\\xC3\\xA9\\x0A\"\n"
            "  Filter \"a\"\"b\" = 'x\\x09y\\x5Cz'\n"
            "    Scan table=w source=fake columns=[\"a\"\"b\", \"\\xC3\\xA9\\x0A\"]\n");
}

TEST(ExplainTest, EmptyPlan) { EXPECT_EQ(Explain(LogicalPlan{}), "Output:\n"); }

}  // namespace
}  // namespace antb1::plan
