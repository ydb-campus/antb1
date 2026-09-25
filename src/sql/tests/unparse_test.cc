#include "antb1/sql/unparse.h"

#include <gtest/gtest.h>

#include "antb1/sql/parser.h"

namespace antb1::sql {
namespace {

TEST(UnparseTest, RoundTripsCountStar) {
  for (const char* sql : {"select count(*) from events", "SELECT COUNT(*) FROM \"Events\";",
                          "SELECT COUNT(*) FROM 'a''b.parquet'"}) {
    auto stmt = Parse(sql);
    ASSERT_TRUE(stmt.has_value()) << sql;
    const std::string canonical = ToSql(*stmt);
    auto again = Parse(canonical);
    ASSERT_TRUE(again.has_value()) << canonical;
    EXPECT_TRUE(EqualIgnoringSpans(*stmt, *again)) << canonical;
    EXPECT_EQ(ToSql(*again), canonical);
  }
}

TEST(UnparseTest, RendersFullAst) {
  SelectStatement stmt;
  stmt.items.push_back(SelectItem{
      .expr = AggregateCall{.kind = AggKind::kSum, .arg = ColumnRef{.name = "a"}}, .alias = "s"});
  stmt.from = TableRef{.kind = TableRef::Kind::kName, .name = "t"};
  stmt.where.push_back(Comparison{
      .column = ColumnRef{.name = "b"},
      .op = CompareOp::kGe,
      .literal = Literal{.kind = Literal::Kind::kInteger, .negative = true, .text = "5"}});
  stmt.limit = 10;
  EXPECT_EQ(ToSql(stmt), "SELECT SUM(a) AS \"s\" FROM t WHERE b >= -5 LIMIT 10");
}

}  // namespace
}  // namespace antb1::sql
