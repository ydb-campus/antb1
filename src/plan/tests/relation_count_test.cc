#include "../relation_count.h"

#include <cstddef>
#include <format>
#include <string>
#include <vector>

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

// The first FROM item of every block the statement reaches, in order: a readable stand-in for the
// blocks themselves, since each block of these queries reads one table nothing else does.
std::vector<std::string> ReachedTables(const std::string& sql) {
  auto stmt = sql::Parse(sql);
  if (!stmt) {
    ADD_FAILURE() << sql << ": " << stmt.error().message;
    return {};
  }
  std::vector<std::string> names;
  for (const sql::SelectStatement* block : ReachableBlocks(*stmt)) {
    const sql::TableRef* ref = block->from.front().table();
    names.emplace_back(ref == nullptr ? "<derived>" : ref->name);
  }
  return names;
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

// A subquery in an expression counts like a derived table: itself plus its body, wherever it is
// written. The limit therefore bounds the plan a subquery conjunct expands to as well (ADR 0022).
TEST(RelationCountTest, CountsASubqueryAsOnePlusItsBody) {
  EXPECT_EQ(Count("SELECT 1 FROM t WHERE a IN (SELECT a FROM u)"), 3U);
  EXPECT_EQ(Count("SELECT 1 FROM t WHERE a NOT IN (SELECT a FROM u, v)"), 4U);
  EXPECT_EQ(Count("SELECT 1 FROM t WHERE a = (SELECT MAX(a) FROM u)"), 3U);
  EXPECT_EQ(Count("SELECT 1 FROM t WHERE EXISTS (SELECT 1 FROM u)"), 3U);
  // Two conjuncts are two subqueries; one in each other clause counts too.
  EXPECT_EQ(Count("SELECT 1 FROM t WHERE a IN (SELECT a FROM u) AND b IN (SELECT b FROM v)"), 5U);
  EXPECT_EQ(Count("SELECT (SELECT MAX(a) FROM u) FROM t"), 3U);
  EXPECT_EQ(Count("SELECT 1 FROM t JOIN u ON a IN (SELECT a FROM v)"), 4U);
  EXPECT_EQ(Count("SELECT a FROM t GROUP BY a HAVING COUNT(*) > (SELECT COUNT(*) FROM u)"), 3U);
  EXPECT_EQ(Count("SELECT a FROM t ORDER BY (SELECT MAX(a) FROM u)"), 3U);
  // Nested: the inner subquery belongs to the outer one's body, and one in the IN operand to the
  // expression that holds it.
  EXPECT_EQ(Count("SELECT 1 FROM t WHERE a IN (SELECT a FROM u WHERE b IN (SELECT b FROM v))"), 5U);
  EXPECT_EQ(Count("SELECT 1 FROM t WHERE (SELECT MAX(a) FROM v) IN (SELECT a FROM u)"), 5U);
  // A derived table's own WHERE subquery, and a subquery over a derived table.
  EXPECT_EQ(Count("SELECT 1 FROM (SELECT a FROM t WHERE a IN (SELECT a FROM u)) AS s"), 4U);
  EXPECT_EQ(Count("SELECT 1 FROM t WHERE a IN (SELECT a FROM (SELECT a FROM u) AS s)"), 4U);
}

// A CTE referenced only from inside a subquery is reached through it, and still counts once per
// reference: the subquery's query sees the CTEs of the block that holds it.
TEST(RelationCountTest, ReachesACteThroughASubquery) {
  EXPECT_EQ(Count("WITH c AS (SELECT a FROM t, u) SELECT 1 FROM v WHERE a IN (SELECT a FROM c)"),
            5U);
  EXPECT_EQ(Count("WITH c AS (SELECT a FROM t, u) SELECT 1 FROM c WHERE a IN (SELECT a FROM c)"),
            7U);
  // Unreferenced from either place: still nothing.
  EXPECT_EQ(Count("WITH c AS (SELECT a FROM t, u) SELECT 1 FROM v WHERE a IN (SELECT a FROM w)"),
            3U);
}

// The same doubling chain, referenced only from a subquery in WHERE: the limit must still reject it
// before any sub-plan is bound, and it reports at the subquery's operator -- a WHERE subquery has
// no FROM item of the outermost block to point at.
TEST(RelationCountTest, RejectsTheDoublingChainBehindASubquery) {
  std::string sql = "WITH c0 AS (SELECT 1 FROM t)";
  for (int i = 1; i <= 30; ++i) {
    sql += std::format(", c{} AS (SELECT 1 FROM c{} AS x, c{} AS y)", i, i - 1, i - 1);
  }
  sql += " SELECT 1 FROM u WHERE a IN (SELECT a FROM c30)";
  EXPECT_TRUE(Exceeds(sql));
  EXPECT_EQ(Count(sql), kMaxRelations + 1) << "saturated, not 2^30";
  auto stmt = sql::Parse(sql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  const auto over = RelationsExceedLimit(*stmt);
  ASSERT_TRUE(over.has_value());
  ASSERT_TRUE(over->span.has_value()) << "no FROM item holds the overflow";
  EXPECT_EQ(sql.substr(over->span->offset, over->span->length), "IN");
}

// A FROM item still wins when it crosses the limit first, even with a subquery in the same block:
// the FROM list is walked before the expressions, as the binder reports in query order.
TEST(RelationCountTest, ReportsAFromItemBeforeASubqueryOfTheSameBlock) {
  std::string sql = "SELECT 1 FROM (SELECT 1 FROM t1";
  for (std::size_t i = 2; i <= kMaxRelations; ++i) {
    sql += std::format(", t{}", i);
  }
  sql += ") AS s WHERE a IN (SELECT a FROM u)";
  auto stmt = sql::Parse(sql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  const auto over = RelationsExceedLimit(*stmt);
  ASSERT_TRUE(over.has_value());
  EXPECT_FALSE(over->span.has_value()) << "the derived table, not the subquery";
  EXPECT_EQ(over->item, 0U);
}

// Every block a statement reaches, so that CheckSupported checks a subquery's block too: a block's
// FROM list comes before its own expressions, and a CTE is reached once however often it is used.
TEST(RelationCountTest, ReachesTheBlockOfEverySubquery) {
  EXPECT_EQ(ReachedTables("SELECT 1 FROM t"), std::vector<std::string>{"t"});
  EXPECT_EQ(ReachedTables("SELECT 1 FROM t WHERE a IN (SELECT a FROM u)"),
            (std::vector<std::string>{"t", "u"}));
  EXPECT_EQ(ReachedTables("SELECT (SELECT 1 FROM u) FROM t WHERE a IN (SELECT a FROM v)"),
            (std::vector<std::string>{"t", "u", "v"}))
      << "the select item's subquery precedes WHERE's";
  EXPECT_EQ(ReachedTables("SELECT 1 FROM (SELECT 1 FROM u) AS s WHERE a IN (SELECT a FROM v)"),
            (std::vector<std::string>{"<derived>", "u", "v"}))
      << "the FROM list's blocks precede the expressions'";
  EXPECT_EQ(ReachedTables("SELECT 1 FROM t WHERE a IN (SELECT a FROM u WHERE b = (SELECT 1 "
                          "FROM v))"),
            (std::vector<std::string>{"t", "u", "v"}));
  EXPECT_EQ(ReachedTables("WITH c AS (SELECT 1 FROM w) SELECT 1 FROM c WHERE a IN (SELECT a "
                          "FROM c)"),
            (std::vector<std::string>{"c", "w", "c"}))
      << "the CTE body once, the subquery's own block always";
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
