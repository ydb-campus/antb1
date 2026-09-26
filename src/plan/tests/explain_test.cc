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
            "Limit 10\n"
            "  Project i16, s\n"
            "    Scan table=ok source=fake columns=[i16, s]\n");
  EXPECT_EQ(ExplainSql("SELECT s AS \"The S\", I16 FROM OK WHERE i16 >= -32768.5 LIMIT 0"),
            "Output: The S:VARCHAR i16:SMALLINT\n"
            "Limit 0\n"
            "  Project s, i16\n"
            "    Filter i16 >= -32768\n"
            "      Scan table=OK source=fake columns=[i16, s]\n");
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
