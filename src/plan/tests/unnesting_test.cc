#include "../unnesting.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/result.h>
#include <gtest/gtest.h>

#include "antb1/plan/explain.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/plan_options.h"

#include "legacy_unnesting.h"
#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::BindSql;
using testing::MakeCatalog;

// Per node in pre-order: its name in EXPLAIN, its output ids and the largest id it holds, which
// EXPLAIN cannot show.
void Walk(const LogicalNodePtr& node, std::vector<std::string>& out) {
  if (node == nullptr) {
    return;
  }
  std::string line = ExplainNode(*node) + " |";
  for (const ColumnId id : OutputIds(*node)) {
    line += " " + std::to_string(std::to_underlying(id));
  }
  line += " | max " + std::to_string(std::to_underlying(MaxColumnId(*node)));
  out.push_back(std::move(line));
  for (const LogicalNodePtr& input : InputsOf(*node)) {
    Walk(input, out);
  }
}

// While ADR 0041's SQ0 replaces ADR 0023's Path 1 and Path 2 with one rule table, every correlated
// statement of binder_test over the test catalog unnests to the same plan as before, the same ids
// minted in the same order, or to the same refusal.
TEST(UnnestingTest, TheRuleTableBuildsTodaysPlans) {
  const Catalog catalog = MakeCatalog();
  std::size_t compared = 0;
  for (const char* sql : {
           "SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 AND EXISTS (SELECT 1 FROM dec WHERE dec.i "
           "= t.i32) FULL JOIN ok ON u.i16 = ok.i16",
           "SELECT t.i16 FROM t WHERE EXISTS (SELECT * FROM u JOIN dec ON u.i32 = dec.i AND dec.b "
           "= t.i64 FULL JOIN ok ON u.i16 = ok.i16)",
           "WITH c AS (SELECT i16 FROM t) SELECT md5(s) FROM (SELECT s FROM c) AS a",
           "SELECT i16 FROM t WHERE i16 IN (SELECT i16 FROM u WHERE u.i32 = t.i32 UNION ALL SELECT "
           "i16 FROM t)",
           "SELECT i16 FROM t WHERE i16 > (SELECT MAX(i16) FROM u WHERE u.i32 = t.i32 UNION ALL "
           "SELECT MAX(i16) FROM t)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT i16 FROM u WHERE u.i32 = t.i32 UNION ALL SELECT "
           "i16 FROM t)",
           "SELECT i16 FROM t WHERE NOT EXISTS (SELECT i16 FROM u UNION ALL SELECT i16 FROM u "
           "WHERE u.i32 = t.i32)",
           "SELECT i16 FROM t GROUP BY i16 HAVING COUNT(*) > (SELECT MAX(i16) FROM u WHERE u.i16 = "
           "t.i16 UNION ALL SELECT MAX(i16) FROM t)",
           "WITH c AS (SELECT a FROM nope UNION ALL SELECT b FROM nada ORDER BY 1) SELECT x FROM "
           "missing",
           "SELECT i16 FROM t WHERE EXISTS (SELECT i16 FROM u WHERE u.i16 = t.i16 EXCEPT SELECT "
           "i16 FROM t)",
           "SELECT i16 FROM t WHERE i16 IN (SELECT i16 FROM u INTERSECT ALL SELECT i16 FROM ok "
           "WHERE ok.i16 = t.i32)",
           "SELECT d.i16 FROM t AS a, (SELECT i16 FROM u WHERE u.i32 = a.i32 UNION ALL SELECT i16 "
           "FROM u) AS d",
           "SELECT i16 FROM t WHERE i32 = (SELECT i16 FROM u WHERE u.i64 = t.i64)",
           "SELECT i16 FROM t WHERE i32 = (SELECT COUNT(*) FROM u WHERE u.i64 = t.i64 LIMIT 1)",
           "SELECT i16 FROM t WHERE i32 = (SELECT COUNT(*) FROM u WHERE u.i64 = t.i64 OFFSET 1)",
           "SELECT i16 FROM t WHERE i32 = (SELECT COUNT(*) FROM u WHERE u.i64 = t.i64 GROUP BY "
           "i16)",
           "SELECT i16 FROM t WHERE i32 = (SELECT COUNT(*) FROM u WHERE u.i64 = t.i64 HAVING "
           "COUNT(*) > 1)",
           "SELECT i16 FROM t WHERE i32 = (SELECT i16, i32 FROM u WHERE u.i64 = t.i64)",
           "SELECT nope FROM t WHERE i32 = (SELECT i16 FROM u WHERE u.i64 = t.i64)",
           "SELECT i16 FROM t WHERE i32 = (SELECT nope FROM u WHERE u.i64 = t.i64 GROUP BY i16)",
           "SELECT i16 FROM t WHERE i32 = 1 OR EXISTS (SELECT u.i16 FROM u WHERE u.i32 = t.i32)",
           "SELECT i16 FROM t WHERE i32 = 1 OR i32 > (SELECT MIN(u.i16) FROM u WHERE u.i64 = "
           "t.i64)",
           "SELECT i16 FROM t WHERE i32 > (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64) + 1",
           "SELECT i16 FROM t WHERE EXISTS (SELECT u.i32 FROM u WHERE u.i32 = t.i32 GROUP BY "
           "u.i16)",
           "SELECT nope FROM t WHERE EXISTS (SELECT u.i16 FROM u WHERE u.i32 = t.i32 GROUP BY "
           "u.i16)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT u.i16 FROM u WHERE u.i32 = t.i32 GROUP BY "
           "u.i16)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT u.i16 FROM u WHERE u.i32 = t.i32 LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT MIN(u.i16) FROM u WHERE u.i32 = t.i32)",
           "SELECT i32, COUNT(*) FROM t GROUP BY i32 HAVING EXISTS (SELECT u.i16 FROM u WHERE "
           "u.i32 = t.i32)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT nope FROM u WHERE u.i32 = t.i32 GROUP BY u.i16)",
           "SELECT i16, COUNT(*) FROM t GROUP BY i32 HAVING EXISTS (SELECT u.i16 FROM u WHERE "
           "u.i32 = t.i32)",
           "SELECT i16 FROM t WHERE nope = 1 AND EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 LIMIT "
           "1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 LIMIT 1) AND nope "
           "= 1",
           "SELECT i16 FROM t WHERE i32 IN (SELECT i16 FROM u WHERE i64 = t.i64)",
           "SELECT i16 FROM t WHERE i32 NOT IN (SELECT i16 FROM u WHERE i64 = t.i64)",
           "SELECT i16 FROM t WHERE i32 IN (SELECT i16 FROM ok WHERE i16 = i64)",
           "SELECT i16 FROM t WHERE i32 IN (SELECT t.i64 FROM u)",
           "SELECT i16 FROM t WHERE i32 IN (SELECT i16 FROM u GROUP BY i16, t.i64)",
           "SELECT i16 FROM t WHERE i32 IN (SELECT i16 FROM u ORDER BY t.i64)",
           "SELECT i16 FROM t WHERE i32 IN (SELECT i16 FROM u GROUP BY i16 HAVING MIN(i16) > "
           "t.i64)",
           "SELECT i16 FROM t WHERE i32 IN (SELECT u.i16 FROM u LEFT JOIN ok ON u.i16 = ok.i16 AND "
           "ok.i16 = t.i64)",
           "SELECT i16 FROM t WHERE i32 IN (SELECT u.i16 FROM u JOIN ok ON u.i16 = ok.i16 AND "
           "ok.i16 = t.i64)",
           "SELECT i16 FROM t WHERE i32 IN (SELECT i16 FROM u WHERE i32 IN (SELECT i16 FROM ok "
           "WHERE i16 = t.i64))",
           "SELECT i32 FROM t GROUP BY i32 HAVING MIN(i16) = (SELECT MIN(u.i16) FROM u WHERE u.i32 "
           "= t.i32)",
           "SELECT i16 FROM t GROUP BY i16 HAVING (SELECT COUNT(*) FROM u WHERE u.i32 < "
           "SUM(t.i32)) > 3",
           "SELECT i16 FROM t GROUP BY i16 HAVING (SELECT COUNT(*) FROM u WHERE u.i32 < (SELECT "
           "MAX(ok.i16) FROM ok WHERE ok.i16 > SUM(t.i32))) > 3",
           "SELECT i16 FROM t GROUP BY i16 HAVING (SELECT COUNT(*) FROM u WHERE u.i32 < SUM(u.i32 "
           "+ t.i32)) > 3",
           "SELECT i16 FROM t GROUP BY i16 HAVING COUNT(*) > (SELECT COUNT(*) FROM u WHERE "
           "SUM(u.i32 + t.i32) IN (SELECT ok.i16 FROM ok))",
           "SELECT i16 FROM t GROUP BY i16 HAVING COUNT(*) > (SELECT COUNT(*) FROM u WHERE "
           "SUM(t.i32) IN (SELECT ok.i16 FROM ok))",
           "SELECT i16 FROM t GROUP BY i16 HAVING COUNT(*) > (SELECT COUNT(*) FROM u WHERE u.i32 < "
           "(SELECT MAX(ok.i16) FROM ok WHERE ok.i16 > SUM(u.i32)))",
           "SELECT (SELECT COUNT(*) FROM u WHERE u.i32 < MAX(t.i32)) FROM t",
           "SELECT i16 FROM t GROUP BY i16 ORDER BY (SELECT COUNT(*) FROM u WHERE u.i32 < "
           "MAX(t.i32))",
           "SELECT (SELECT COUNT(*) FROM u WHERE u.i32 < MAX(t.i32)) AS n FROM t GROUP BY n",
           "SELECT i16, (SELECT SUM(u.i32 * (SELECT COUNT(*) FROM ok WHERE ok.i16 < MAX(u.i16))) "
           "FROM u) FROM t",
           "SELECT i16, (SELECT MIN(u.i32 * (SELECT COUNT(*) FROM ok WHERE ok.i16 < MAX(u.i16))) "
           "FROM u) FROM t",
           "SELECT i16 FROM t WHERE i32 = (SELECT MIN(u.i16) FROM u WHERE t.i64 IN (SELECT ok.i16 "
           "FROM ok))",
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 AND t.i32 > "
           "(SELECT MIN(ok.i16) FROM ok WHERE ok.i16 = u.i16))",
           "SELECT i16 FROM t WHERE (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64) < (SELECT "
           "MAX(ok.i16) FROM ok WHERE ok.i16 = t.i16)",
           "SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 AND u.i32 > (SELECT MIN(dec.i) FROM dec "
           "WHERE dec.i = u.i32) + 1 FULL JOIN ok ON u.i16 = ok.i16",
           "SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 AND (SELECT MIN(ok.i16) FROM ok) < (SELECT "
           "MIN(dec.i) FROM dec WHERE dec.i = u.i32) FULL JOIN ok ON u.i16 = ok.i16",
           "SELECT i16 FROM t WHERE (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64) < (SELECT "
           "MAX(nope) FROM ok WHERE ok.i16 = t.i16)",
           "SELECT i16 FROM t WHERE i32 BETWEEN (SELECT MIN(i16) FROM u WHERE u.i64 = t.i64) AND "
           "10",
           "SELECT i16 FROM t GROUP BY i16 ORDER BY (SELECT MIN(u.i32) FROM u WHERE u.i16 = t.i16)",
           "SELECT i16, CASE WHEN EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16) THEN 1 ELSE 0 END "
           "FROM t GROUP BY i16",
           "SELECT i16 FROM t WHERE i16 < 10 OR i32 IN (SELECT u.i16 FROM u WHERE u.i64 = t.i64)",
           "SELECT i16 FROM t WHERE i16 < 10 OR i32 NOT IN (SELECT u.i16 FROM u WHERE u.i64 = "
           "t.i64)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 AND (u.i32 < 0 OR "
           "t.i32 IN (SELECT i16 FROM ok)))",
           "SELECT i16 FROM t WHERE i32 > (SELECT CASE WHEN COUNT(*) IN (SELECT i16 FROM ok) THEN "
           "1 ELSE 0 END FROM u WHERE u.i64 = t.i64)",
           "SELECT i16 FROM t WHERE (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64) IN (SELECT i16 "
           "FROM ok)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT (SELECT nope FROM u) FROM u)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT (SELECT ok.i16 FROM ok WHERE ok.i16 = u.i32) "
           "FROM u)",
           "SELECT (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64) FROM t",
           "SELECT i16 FROM t ORDER BY (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64)",
           "SELECT CASE WHEN EXISTS (SELECT 1 FROM u WHERE u.i64 = t.i64) THEN 1 ELSE 0 END FROM t",
           "SELECT i16 FROM t WHERE i32 > (SELECT SUM(u.i32) + (SELECT MIN(i16) FROM ok) FROM u "
           "WHERE u.i64 = t.i64)",
           "SELECT i16 FROM t WHERE i32 < (SELECT SUM(u.i32) FROM u WHERE u.i64 = t.i64 HAVING "
           "SUM(u.i32) > (SELECT MIN(i16) FROM ok))",
           "SELECT i16 FROM t WHERE i16 IN (SELECT i32 FROM u GROUP BY i32 HAVING GROUPING(t.i16) "
           "= 0)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 GROUP BY {})",
           "SELECT i16 FROM t WHERE i16 = (SELECT COUNT(*) FROM u WHERE u.i16 = t.i16 GROUP BY {})",
           "SELECT i16 FROM t WHERE EXISTS (SELECT DISTINCT s FROM t AS u WHERE u.i32 = t.i32)",
           "SELECT i16 FROM ok WHERE i16 = (SELECT MAX(t.i16) FROM t WHERE t.i16 = ok.i16)",
           "SELECT i16 FROM t WHERE i16 = (SELECT MAX(ok.i16) FROM ok WHERE ok.i16 = t.i16)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT i16 FROM ok WHERE ok.i16 = t.i16)",
           "SELECT i16 FROM t WHERE NOT EXISTS (SELECT i16 FROM ok WHERE ok.i16 = t.i16)",
           "SELECT i16 FROM ok WHERE EXISTS (SELECT ok.* FROM u WHERE u.i16 = ok.i16)",
           "SELECT ok.i16 FROM ok JOIN (SELECT ok.* FROM u) AS d ON ok.i16 = d.i16",
           "SELECT i16 FROM ok WHERE i16 = (SELECT MAX(i16), ok.* FROM u)",
           "SELECT i16 FROM ok WHERE EXISTS (SELECT *, i32 FROM u WHERE u.i16 = ok.i16 ORDER BY 1)",
           "SELECT i16 FROM ok WHERE EXISTS (SELECT *, i32 FROM u WHERE u.i16 = ok.i16 ORDER BY "
           "99)",
           "SELECT i16 FROM ok WHERE EXISTS (SELECT x.* FROM u WHERE u.i16 = ok.i16)",
           "SELECT i16 FROM ok WHERE EXISTS (SELECT *, nope FROM u WHERE u.i16 = ok.i16)",
           "SELECT i16 FROM ok WHERE EXISTS (SELECT u.*, t.d FROM u JOIN t ON u.i16 = t.i16 WHERE "
           "u.i16 = ok.i16 ORDER BY i32)",
           "SELECT i16 FROM ok WHERE EXISTS (SELECT t.i32, u.* FROM u JOIN t ON u.i16 = t.i16 "
           "WHERE u.i16 = ok.i16 ORDER BY i32)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT *, u.i32 FROM u JOIN ok ON u.i16 = ok.i16 WHERE "
           "u.i16 = t.i16 ORDER BY 13)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT *, u.i32 FROM u JOIN ok ON u.i16 = ok.i16 WHERE "
           "u.i16 = t.i16 ORDER BY 15)",
           "SELECT i16 FROM ok WHERE EXISTS (SELECT u.*, u.i32 FROM u WHERE u.i16 = ok.i16)",
           "SELECT i16 FROM ok WHERE EXISTS (SELECT i32, * FROM u WHERE u.i16 = ok.i16 ORDER BY 1)",
           "SELECT i16 FROM ok WHERE EXISTS (SELECT *, i32 FROM u WHERE u.i16 = ok.i16 ORDER BY "
           "12)",
           "SELECT i16 FROM u WHERE EXISTS (SELECT * FROM t WHERE t.i64 = u.i64)",
           "SELECT i16 FROM u WHERE EXISTS (SELECT * FROM t, t AS t WHERE t.i64 = u.i64)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT s AS i32 FROM ok WHERE ok.i16 = i32 LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT i32 + 1, s AS i32 FROM ok LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT s FROM ok ORDER BY i32 LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT ok.s AS i64 FROM ok JOIN dec ON dec.b = i64 "
           "LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT s AS i32 FROM ok GROUP BY s HAVING MAX(i32) > 0 "
           "LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT s AS i32 FROM ok GROUP BY i32 + 1, s LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT s AS i32 FROM ok GROUP BY ROLLUP (s) HAVING "
           "GROUPING(i32) = 0)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 AND t.i64 > "
           "(SELECT MIN(ok.i16) FROM ok))",
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 AND t.i32 BETWEEN "
           "(SELECT MIN(ok.i16) FROM ok) AND 100)",
           "SELECT i16 FROM t WHERE i32 = (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64 AND u.i32 "
           "> (SELECT MIN(ok.i16) FROM ok))",
           "SELECT i16 FROM t WHERE i32 + (SELECT COUNT(*) FROM ok) > (SELECT MIN(u.i16) FROM u "
           "WHERE u.i64 = t.i64)",
           "SELECT i16 FROM t WHERE (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64) < (SELECT "
           "COUNT(*) FROM ok)",
           "SELECT i16 FROM t WHERE (SELECT COUNT(*) FROM ok) > (SELECT MIN(u.i16) FROM u WHERE "
           "u.i64 = t.i64)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE (SELECT MIN(ok.i16) FROM ok) = "
           "t.i16)",
           "SELECT i16 FROM t WHERE i32 > (SELECT SUM(u.i32) FROM u WHERE (SELECT MIN(ok.i16) FROM "
           "ok) = t.i16)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT (SELECT i16 FROM ok), u.i16 FROM u WHERE u.i64 "
           "= t.i64)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT (SELECT MIN(ok.i16) FROM ok WHERE ok.i16 = "
           "u.i32) FROM u)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT i16 + (SELECT MIN(i16) FROM ok) AS a, i16 * 2 "
           "AS b, i16 * 3 AS c FROM u ORDER BY {} LIMIT 1)",
           "SELECT i16 FROM t WHERE (i32 = 2 AND i32 > (SELECT nope FROM u)) OR i32 = 2",
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 AND t.i64 > "
           "(SELECT MIN(ok.i16) FROM ok) AND u.i32 > (SELECT MAX(ok.i16) FROM ok) + 1)",
           "SELECT i16 FROM t WHERE i32 = (SELECT MIN(u.i32) FROM u WHERE u.i64 = t.i64)",
           "SELECT i16 FROM t WHERE i32 = (SELECT 1 FROM u WHERE u.i64 = t.i64 ORDER BY COUNT(*))",
           "SELECT i16 FROM t WHERE i32 = (SELECT 1 FROM u WHERE u.i64 = t.i64 ORDER BY "
           "MAX(u.i32))",
           "SELECT i16 FROM t WHERE i32 = (SELECT 1 + 0 FROM u WHERE u.i64 = t.i64 ORDER BY "
           "MIN(u.i32))",
           "SELECT i16 FROM t WHERE i32 < (SELECT MAX(u.i32) + 1 FROM u WHERE u.i64 = t.i64)",
           "SELECT i16 FROM t WHERE i32 < (SELECT -MIN(u.i32) * 2 FROM u WHERE u.i64 = t.i64 ORDER "
           "BY MAX(u.i32))",
           "SELECT i16 FROM t WHERE i64 < (SELECT CAST(AVG(u.i32) AS BIGINT) FROM u WHERE u.i64 = "
           "t.i64)",
           "SELECT i16 FROM t WHERE i64 < (SELECT SUM(u.i32 + 1) FROM u WHERE u.i64 = t.i64)",
           "SELECT p FROM dec WHERE b < (SELECT SUM(u.i32 + 1) FROM u JOIN ok ON ok.i16 = u.i16 "
           "AND u.i32 = dec.i)",
           "SELECT p FROM dec WHERE b < (SELECT SUM(u.i32 + 1) FROM u JOIN ok ON ok.i16 = u.i16 "
           "AND u.i32 = i JOIN dec AS d2 ON d2.s16 = u.i16)",
           "SELECT t.i16 FROM t JOIN ok ON t.i16 = ok.i16 AND EXISTS (SELECT * FROM dec WHERE "
           "dec.i = x.i32) JOIN u AS x ON x.i16 = t.i16",
           "SELECT x.i16 FROM u AS x WHERE EXISTS (SELECT * FROM t JOIN ok ON t.i16 = ok.i16 AND "
           "EXISTS (SELECT * FROM dec WHERE dec.i = x.i32) JOIN u AS x ON x.i16 = t.i16)",
           "SELECT t.i16 FROM t JOIN ok ON t.i16 = ok.i16 AND EXISTS (SELECT * FROM dec WHERE "
           "dec.i = i32) JOIN u ON u.i16 = t.i16",
           "SELECT i16 FROM t WHERE (i16 < 0 OR i32 IN (SELECT i16 FROM ok)) AND i32 > (SELECT "
           "MIN(u.i16) FROM u WHERE u.i64 = t.i64)",
           "SELECT i16 FROM t WHERE (i16 < 0 OR i32 IN (SELECT i16 FROM ok)) AND EXISTS (SELECT 1 "
           "FROM u WHERE u.i64 = t.i64)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 AND (u.i32 IN "
           "(SELECT i16 FROM ok) OR t.i64 < 0))",
           "SELECT i16 FROM t WHERE i32 > (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64 AND (u.i32 "
           "IN (SELECT i16 FROM ok) OR u.i16 < 0))",
           "SELECT i16 FROM t WHERE (i16 = 2 AND i32 IN (SELECT nope FROM u)) OR i16 = 2",
       }) {
    auto plan = BindSql(sql, catalog);
    if (!plan.ok()) {
      continue;
    }
    SCOPED_TRACE(sql);
    const arrow::Result<LogicalNodePtr> now = Unnest(plan->root, kBuildSideRatio);
    const arrow::Result<LogicalNodePtr> before = legacy::Unnest(plan->root, kBuildSideRatio);
    ASSERT_EQ(now.ok(), before.ok())
        << now.status().ToString() << " / " << before.status().ToString();
    if (!now.ok()) {
      EXPECT_EQ(now.status().ToString(), before.status().ToString());
      continue;
    }
    ++compared;
    EXPECT_EQ(Explain({.root = *now, .output = plan->output}),
              Explain({.root = *before, .output = plan->output}));
    std::vector<std::string> now_nodes;
    std::vector<std::string> before_nodes;
    Walk(*now, now_nodes);
    Walk(*before, before_nodes);
    EXPECT_EQ(now_nodes, before_nodes);
  }
  EXPECT_GT(compared, 20U);
}

}  // namespace
}  // namespace antb1::plan
