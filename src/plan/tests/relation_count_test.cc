#include "../relation_count.h"

#include <cstddef>
#include <format>
#include <string>

#include <gtest/gtest.h>

#include "antb1/sql/parser.h"

// The relation count of ADR 0022's limit, on its own: the binder's use of it is pinned by
// plan.FromLists/BindErrorTest.
namespace antb1::plan {
namespace {

std::size_t Count(const std::string& sql) {
  auto stmt = sql::Parse(sql);
  if (!stmt) {
    ADD_FAILURE() << sql << ": " << stmt.error().message;
    return 0;
  }
  return CountRelations(*stmt);
}

bool Exceeds(const std::string& sql) {
  auto stmt = sql::Parse(sql);
  if (!stmt) {
    ADD_FAILURE() << sql << ": " << stmt.error().message;
    return false;
  }
  return RelationsExceedLimit(*stmt).has_value();
}

// A FROM list of tables and paths counts one per item, whatever they name.
TEST(RelationCountTest, CountsEveryFromItem) {
  EXPECT_EQ(Count("SELECT 1 FROM t"), 1U);
  EXPECT_EQ(Count("SELECT 1 FROM t, u, v"), 3U);
  EXPECT_EQ(Count("SELECT 1 FROM t JOIN u ON t.a = u.a JOIN v ON u.a = v.a"), 3U);
  EXPECT_EQ(Count("SELECT 1 FROM 'a.parquet', 'b.parquet'"), 2U);
}

// A derived table is itself plus its body, so nesting adds one level at a time.
TEST(RelationCountTest, CountsADerivedTableAsOnePlusItsBody) {
  EXPECT_EQ(Count("SELECT 1 FROM (SELECT 1 FROM t) AS s"), 2U);
  EXPECT_EQ(Count("SELECT 1 FROM (SELECT 1 FROM t, u) AS s"), 3U);
  EXPECT_EQ(Count("SELECT 1 FROM (SELECT 1 FROM (SELECT 1 FROM t) AS i) AS o"), 3U);
  EXPECT_EQ(Count("SELECT 1 FROM (SELECT 1 FROM t) AS a, (SELECT 1 FROM u) AS b"), 4U);
}

// A CTE counts where it is referenced, once per reference, and not at all when it is not.
TEST(RelationCountTest, CountsACteOncePerReference) {
  EXPECT_EQ(Count("WITH c AS (SELECT 1 FROM t) SELECT 1 FROM c"), 2U);
  EXPECT_EQ(Count("WITH c AS (SELECT 1 FROM t) SELECT 1 FROM c AS a, c AS b"), 4U);
  EXPECT_EQ(Count("WITH c AS (SELECT 1 FROM t, u, v) SELECT 1 FROM c"), 4U);
  // Unreferenced: DuckDB never binds it, so it expands to nothing.
  EXPECT_EQ(Count("WITH c AS (SELECT 1 FROM t, u, v) SELECT 1 FROM t"), 1U);
  // A CTE hides a catalog table of the same name (rule 8), so `t` is the CTE here.
  EXPECT_EQ(Count("WITH t AS (SELECT 1 FROM u, v) SELECT 1 FROM t"), 3U);
  // A string FROM item names a CTE before it names a path.
  EXPECT_EQ(Count("WITH \"a.parquet\" AS (SELECT 1 FROM u, v) SELECT 1 FROM 'a.parquet'"), 3U);
}

// A CTE's body sees the CTEs before it in its own list, never itself and never a later one.
TEST(RelationCountTest, ACteBodySeesOnlyTheCtesBeforeIt) {
  EXPECT_EQ(Count("WITH a AS (SELECT 1 FROM t, u), b AS (SELECT 1 FROM a) SELECT 1 FROM b"), 4U);
  // `a`'s body names `b`, which is declared after it: that is a table, not the CTE.
  EXPECT_EQ(Count("WITH a AS (SELECT 1 FROM b), b AS (SELECT 1 FROM t, u) SELECT 1 FROM a"), 2U);
}

// The chain the ADR names: each CTE joins the previous one with itself, so the count doubles per
// level. It is rejected long before 2^30 bodies are counted -- and the walk itself stays cheap,
// since each body is counted once and every sum saturates.
TEST(RelationCountTest, RejectsTheDoublingChainOfCtes) {
  std::string sql = "WITH c0 AS (SELECT 1 FROM t)";
  for (int i = 1; i <= 30; ++i) {
    sql += std::format(", c{} AS (SELECT 1 FROM c{} AS x, c{} AS y)", i, i - 1, i - 1);
  }
  sql += " SELECT 1 FROM c30";
  EXPECT_TRUE(Exceeds(sql));
  EXPECT_EQ(Count(sql), kMaxRelations + 1) << "saturated, not 2^30";
  // The doubling, level by level: a reference costs 1 + its body, so a body of two references to
  // the level below costs 2 * (1 + previous). c0's body is 1, so the references cost 2, 5, 11, 23,
  // 47 and the outer block's reference to c4 is the 47. Five levels more cross the limit.
  std::string four = "WITH c0 AS (SELECT 1 FROM t)";
  for (int i = 1; i <= 4; ++i) {
    four += std::format(", c{} AS (SELECT 1 FROM c{} AS x, c{} AS y)", i, i - 1, i - 1);
  }
  four += " SELECT 1 FROM c4";
  EXPECT_FALSE(Exceeds(four));
  EXPECT_EQ(Count(four), 47U);
  std::string nine = "WITH c0 AS (SELECT 1 FROM t)";
  for (int i = 1; i <= 9; ++i) {
    nine += std::format(", c{} AS (SELECT 1 FROM c{} AS x, c{} AS y)", i, i - 1, i - 1);
  }
  nine += " SELECT 1 FROM c9";
  EXPECT_TRUE(Exceeds(nine));
}

// The limit, and which FROM item is reported: the first whose expansion crosses it, so a flat list
// of 257 tables reports at the 257th exactly as it did before sub-plans existed.
TEST(RelationCountTest, ReportsTheFirstItemThatCrossesTheLimit) {
  std::string flat = "SELECT 1 FROM t1";
  for (std::size_t i = 2; i <= kMaxRelations; ++i) {
    flat += std::format(", t{}", i);
  }
  EXPECT_FALSE(Exceeds(flat)) << "256 is the limit, not one past it";
  auto stmt = sql::Parse(flat + std::format(", t{}", kMaxRelations + 1));
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  const auto over = RelationsExceedLimit(*stmt);
  ASSERT_TRUE(over.has_value());
  EXPECT_EQ(over.value_or(RelationOverflow{}).item, kMaxRelations)
      << "the 257th item, counting from 0";
  // A derived table that expands past the limit is reported at the item that holds it, even when
  // the FROM list is short.
  std::string wide = "SELECT 1 FROM u, (SELECT 1 FROM t1";
  for (std::size_t i = 2; i <= kMaxRelations; ++i) {
    wide += std::format(", t{}", i);
  }
  wide += ") AS s";
  auto nested = sql::Parse(wide);
  ASSERT_TRUE(nested.has_value()) << nested.error().message;
  const auto over_nested = RelationsExceedLimit(*nested);
  ASSERT_TRUE(over_nested.has_value());
  EXPECT_EQ(over_nested.value_or(RelationOverflow{}).item, 1U)
      << "the derived table, not the table before it";
}

// An unreferenced CTE of more relations than the limit costs nothing, because it is never walked.
TEST(RelationCountTest, AnUnreferencedCteDoesNotCount) {
  std::string sql = "WITH big AS (SELECT 1 FROM t1";
  for (std::size_t i = 2; i <= kMaxRelations + 44; ++i) {
    sql += std::format(", t{}", i);
  }
  sql += ") SELECT 1 FROM u";
  EXPECT_FALSE(Exceeds(sql));
  EXPECT_EQ(Count(sql), 1U);
}

}  // namespace
}  // namespace antb1::plan
