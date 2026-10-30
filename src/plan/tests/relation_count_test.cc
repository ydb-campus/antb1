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
    if (!block->set_operation.empty()) {
      names.emplace_back("<compound>");  // its blocks hold its FROM items (ADR 0036)
      continue;
    }
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
  const RelationOverflow overflow = over.value_or(RelationOverflow{});
  ASSERT_TRUE(overflow.span.has_value()) << "no FROM item holds the overflow";
  const SourceSpan span = overflow.span.value_or(SourceSpan{});
  EXPECT_EQ(sql.substr(span.offset, span.length), "IN");
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
  const RelationOverflow overflow = over.value_or(RelationOverflow{});
  EXPECT_FALSE(overflow.span.has_value()) << "the derived table, not the subquery";
  EXPECT_EQ(overflow.item, 0U);
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
  // Referenced from the subquery alone: the body is reached through it, in the frame of the block
  // that holds the subquery, so a check over the result sees it.
  EXPECT_EQ(ReachedTables("WITH c AS (SELECT 1 FROM w) SELECT 1 FROM t WHERE a IN (SELECT a "
                          "FROM c)"),
            (std::vector<std::string>{"t", "c", "w"}));
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

// A compound's blocks count as blocks do (ADR 0036), each in the compound's frame: a derived one
// counts one plus the sum of its blocks, and a CTE the blocks share counts once per reference.
TEST(RelationCountTest, CountsACompoundsBlocks) {
  EXPECT_EQ(Count("SELECT a FROM t UNION ALL SELECT b FROM u, v"), 3U);
  EXPECT_EQ(Count("SELECT 1 FROM (SELECT a FROM t UNION ALL SELECT b FROM u) s"), 3U);
  EXPECT_EQ(Count("WITH c AS (SELECT a FROM t, u) SELECT a FROM c UNION ALL SELECT a FROM c"), 6U);
  EXPECT_EQ(Count("SELECT a FROM t WHERE a IN (SELECT b FROM u UNION ALL SELECT c FROM v)"), 4U);
  // Three blocks: each one counts, not only the first two.
  EXPECT_EQ(Count("SELECT a FROM t UNION ALL SELECT b FROM u UNION ALL SELECT c FROM v, w, x"), 5U);
  // A CTE whose query is a compound: one for the reference, plus its blocks'.
  EXPECT_EQ(Count("WITH c AS (SELECT a FROM t UNION ALL SELECT b FROM u) SELECT 1 FROM c"), 3U);
  // The compound's own ORDER BY: its subquery counts one plus its body, after the blocks.
  EXPECT_EQ(Count("SELECT a FROM t UNION ALL SELECT b FROM u ORDER BY (SELECT MAX(c) FROM v)"), 4U);
}

// A compound's blocks come before it, since its own clauses follow them in the query; a CTE the
// blocks share is reached once, through its first reference.
TEST(RelationCountTest, ReachesEveryBlockOfACompoundBeforeIt) {
  EXPECT_EQ(ReachedTables("SELECT a FROM t UNION ALL SELECT b FROM (SELECT c FROM u) s UNION ALL "
                          "SELECT d FROM v"),
            (std::vector<std::string>{"t", "<derived>", "u", "v", "<compound>"}));
  EXPECT_EQ(
      ReachedTables("WITH c AS (SELECT a FROM w) SELECT a FROM c UNION ALL SELECT a FROM c, x"),
      (std::vector<std::string>{"c", "w", "c", "<compound>"}));
  EXPECT_EQ(ReachedTables("SELECT a FROM t WHERE a IN (SELECT b FROM u UNION ALL SELECT c FROM v)"),
            (std::vector<std::string>{"t", "u", "v", "<compound>"}));
  // A subquery of the compound's own ORDER BY comes after the compound, as a block's subqueries do.
  EXPECT_EQ(
      ReachedTables("SELECT a FROM t UNION ALL SELECT b FROM u ORDER BY (SELECT MAX(c) FROM v)"),
      (std::vector<std::string>{"t", "u", "<compound>", "v"}));
}

// The outermost compound has no FROM item of its own to index: the item of a block that carries the
// count past the limit is reported by span -- its connector, or the item when it is its block's
// first.
TEST(RelationCountTest, ReportsTheItemOfACompoundsBlockThatCrossesTheLimit) {
  std::string sql = "SELECT 1 FROM t1";
  for (std::size_t i = 2; i <= 200; ++i) {
    sql += std::format(", t{}", i);
  }
  sql += " UNION ALL SELECT 1 FROM u1";
  for (std::size_t i = 2; i <= 60; ++i) {
    sql += std::format(", u{}", i);
  }
  auto stmt = sql::Parse(sql);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  const auto over = RelationsExceedLimit(*stmt);
  ASSERT_TRUE(over.has_value());
  const SourceSpan span = over.value_or(RelationOverflow{}).span.value_or(SourceSpan{});
  EXPECT_EQ(sql.substr(span.offset, span.length), ",") << "the 257th item: u57's comma";
  EXPECT_EQ(sql.substr(span.offset, 5), ", u57");
  // The item that crosses the limit is its block's first.
  std::string first = sql.substr(0, sql.find(" UNION ALL")) + std::format(", t{}", 201);
  for (std::size_t i = 202; i <= 256; ++i) {
    first += std::format(", t{}", i);
  }
  first += " UNION ALL SELECT 1 FROM 'p.parquet'";
  auto at_first = sql::Parse(first);
  ASSERT_TRUE(at_first.has_value()) << at_first.error().message;
  const auto over_first = RelationsExceedLimit(*at_first);
  ASSERT_TRUE(over_first.has_value());
  const SourceSpan item = over_first.value_or(RelationOverflow{}).span.value_or(SourceSpan{});
  EXPECT_EQ(first.substr(item.offset, item.length), "'p.parquet'");
  // Three blocks: the total runs on across all of them, so after t and u the 257th relation is the
  // third block's v255, reported at its comma.
  std::string third = "SELECT 1 FROM t UNION ALL SELECT 1 FROM u UNION ALL SELECT 1 FROM v1";
  for (std::size_t i = 2; i <= 255; ++i) {
    third += std::format(", v{}", i);
  }
  auto at_third = sql::Parse(third);
  ASSERT_TRUE(at_third.has_value()) << at_third.error().message;
  const auto over_third = RelationsExceedLimit(*at_third);
  ASSERT_TRUE(over_third.has_value());
  const SourceSpan comma = over_third.value_or(RelationOverflow{}).span.value_or(SourceSpan{});
  EXPECT_EQ(third.substr(comma.offset, comma.length), ",");
  EXPECT_EQ(third.substr(comma.offset, 6), ", v255");
}

// A subquery that carries a compound's count past the limit is reported at its operator, by span,
// whether a block holds it or the compound's own ORDER BY does, which counts after every block.
TEST(RelationCountTest, ReportsTheSubqueryOfACompoundThatCrossesTheLimit) {
  std::string first_block = "SELECT 1 FROM t1";
  for (std::size_t i = 2; i <= 254; ++i) {
    first_block += std::format(", t{}", i);
  }
  // 254 relations in the first block and u the 255th: the subquery in the second block's WHERE
  // counts 2 more, so its IN carries the total to 257.
  const std::string in_block =
      first_block + " UNION ALL SELECT 1 FROM u WHERE a IN (SELECT b FROM v)";
  auto stmt = sql::Parse(in_block);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  const auto over = RelationsExceedLimit(*stmt);
  ASSERT_TRUE(over.has_value());
  const SourceSpan span = over.value_or(RelationOverflow{}).span.value_or(SourceSpan{});
  EXPECT_EQ(in_block.substr(span.offset, span.length), "IN");
  EXPECT_EQ(span.offset, in_block.find("IN (SELECT b FROM v)"));
  // The blocks hold 255 and the compound's ORDER BY subquery 2 more: reported at its '('.
  const std::string ordered =
      first_block + " UNION ALL SELECT 1 FROM u ORDER BY (SELECT MAX(a) FROM v)";
  auto in_order_by = sql::Parse(ordered);
  ASSERT_TRUE(in_order_by.has_value()) << in_order_by.error().message;
  const auto over_ordered = RelationsExceedLimit(*in_order_by);
  ASSERT_TRUE(over_ordered.has_value());
  const SourceSpan open = over_ordered.value_or(RelationOverflow{}).span.value_or(SourceSpan{});
  EXPECT_EQ(ordered.substr(open.offset, open.length), "(");
  EXPECT_EQ(open.offset, ordered.find("(SELECT MAX(a) FROM v)")) << "the subquery's, not MAX's";
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
