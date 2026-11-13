#include "../unnesting.h"

#include <cstddef>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/result.h>
#include <gtest/gtest.h>

#include "antb1/plan/explain.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/plan_options.h"
#include "antb1/plan/sql_status.h"

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

// The first dependent join of `node`, its right input rewritten by `f`, the rest as it was.
LogicalNodePtr WithRight(const LogicalNodePtr& node,
                         const std::function<LogicalNodePtr(const LogicalNodePtr&)>& f) {
  if (const auto* dependent = std::get_if<DependentJoinNode>(node.get())) {
    DependentJoinNode out = *dependent;
    out.right = f(out.right);
    return std::make_shared<const LogicalNode>(std::move(out));
  }
  std::vector<LogicalNodePtr> inputs = InputsOf(*node);
  for (LogicalNodePtr& input : inputs) {
    if (input != nullptr && FirstDependentJoin(input) != nullptr) {
      input = WithRight(input, f);
      return WithInputs(*node, std::move(inputs));
    }
  }
  return node;
}

// The first dependent join of `node`, its kind replaced.
LogicalNodePtr WithKind(const LogicalNodePtr& node, JoinKind kind) {
  if (const auto* dependent = std::get_if<DependentJoinNode>(node.get())) {
    DependentJoinNode out = *dependent;
    out.kind = kind;
    return std::make_shared<const LogicalNode>(std::move(out));
  }
  std::vector<LogicalNodePtr> inputs = InputsOf(*node);
  for (LogicalNodePtr& input : inputs) {
    if (input != nullptr && FirstDependentJoin(input) != nullptr) {
      input = WithKind(input, kind);
      return WithInputs(*node, std::move(inputs));
    }
  }
  return node;
}

// `node`'s correlated filter -- the first Filter over a Compute -- rewritten by `f`.
LogicalNodePtr WithFilter(const LogicalNodePtr& node,
                          const std::function<FilterNode(const FilterNode&)>& f) {
  if (const auto* filter = std::get_if<FilterNode>(node.get());
      filter != nullptr && std::holds_alternative<ComputeNode>(*filter->input)) {
    return std::make_shared<const LogicalNode>(f(*filter));
  }
  std::vector<LogicalNodePtr> inputs = InputsOf(*node);
  if (inputs.size() != 1) {
    return node;
  }
  inputs.front() = WithFilter(inputs.front(), f);
  return WithInputs(*node, std::move(inputs));
}

constexpr std::string_view kExists =
    "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 AND u.i32 > 0)";
constexpr std::string_view kNotExists =
    "SELECT i16 FROM t WHERE NOT EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16)";
constexpr std::string_view kScalar =
    "SELECT i16 FROM t WHERE i32 > (SELECT MAX(u.i32) FROM u WHERE u.i64 = t.i64)";

// The refusal Unnest gives the bound `sql` with `change` applied to its plan, or "" where it
// answers.
std::string RefusalOf(std::string_view sql,
                      const std::function<LogicalNodePtr(const LogicalNodePtr&)>& change) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(sql, catalog);
  EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  if (!plan.ok()) {
    return "<bind error>";
  }
  const arrow::Result<LogicalNodePtr> out = Unnest(change(plan->root), kBuildSideRatio);
  if (out.ok()) {
    return "";
  }
  EXPECT_NE(GetSqlError(out.status()), nullptr) << out.status().ToString();
  return out.status().message();
}

// ADR 0041's decision 16: a dependent join is unnested by its kind's traits. A semi or anti join
// takes its correlated conjuncts (lift mode), a one-row join appends a static aggregate's row
// (value mode), and every other kind -- one whose join takes no residual, or that the binder never
// makes dependent -- keeps the one refusal it has always had.
TEST(UnnestingTest, EveryKindButSemiAntiAndOneRowIsRefused) {
  for (const JoinKind kind :
       {JoinKind::kInner, JoinKind::kLeft, JoinKind::kSemi, JoinKind::kAnti,
        JoinKind::kNullAwareAnti, JoinKind::kOneRow, JoinKind::kFull, JoinKind::kNullAwareMark}) {
    SCOPED_TRACE(ToString(kind));
    const std::string refusal =
        RefusalOf(kind == JoinKind::kOneRow ? kScalar : kExists,
                  [kind](const LogicalNodePtr& root) { return WithKind(root, kind); });
    switch (kind) {
      case JoinKind::kSemi:
      case JoinKind::kAnti:
      case JoinKind::kOneRow:
        EXPECT_EQ(refusal, "");
        break;
      default:
        EXPECT_EQ(refusal, "a correlated subquery is not supported");
    }
  }
}

// Decision 17: the descent has a rule per node type, and each mode declines the nodes it has no
// rule for yet with its shape message, whatever the node: one the binder never puts on the way
// (a LIMIT, a grouping, a join, a union, a second dependent join, a bare leaf), or a mode's own
// misfit (an aggregate under EXISTS, a sort over a scalar's aggregate).
TEST(UnnestingTest, EachModeRefusesTheNodesItHasNoRuleFor) {
  const auto over = [](const std::function<LogicalNode(const LogicalNodePtr&)>& make) {
    return [make](const LogicalNodePtr& root) {
      return WithRight(root, [&make](const LogicalNodePtr& right) {
        return std::make_shared<const LogicalNode>(make(right));
      });
    };
  };
  const std::vector<std::pair<std::string, std::function<LogicalNode(const LogicalNodePtr&)>>>
      nodes = {
          {"a Limit",
           [](const LogicalNodePtr& in) {
             return LogicalNode(LimitNode{.input = in, .limit = 1, .offset = 0, .span = {}});
           }},
          {"a Sort",
           [](const LogicalNodePtr& in) {
             return LogicalNode(SortNode{.input = in, .keys = {}, .span = {}});
           }},
          {"an Aggregate",
           [](const LogicalNodePtr& in) {
             return LogicalNode(AggregateNode{.input = in, .aggregates = {}, .span = {}});
           }},
          {"a GroupAggregate",
           [](const LogicalNodePtr& in) {
             return LogicalNode(GroupAggregateNode{
                 .input = in, .keys = {}, .key_ids = {}, .aggregates = {}, .span = {}});
           }},
  };
  constexpr std::string_view kLift = "over a block that is not a plain FROM and WHERE";
  constexpr std::string_view kValue =
      "over a block that is not an ungrouped aggregate over its correlation";
  for (const auto& [name, make] : nodes) {
    SCOPED_TRACE(name);
    // Lift mode drops a Sort above the filter, as it drops the select list; anything else above
    // it is refused.
    const std::string exists = RefusalOf(kExists, over(make));
    const std::string not_exists = RefusalOf(kNotExists, over(make));
    if (name == "a Sort") {
      EXPECT_EQ(exists, "");
      EXPECT_EQ(not_exists, "");
    } else {
      EXPECT_EQ(exists, std::format("a correlated EXISTS {} is not supported", kLift));
      EXPECT_EQ(not_exists, std::format("a correlated NOT EXISTS {} is not supported", kLift));
    }
    // Value mode admits nothing over its static aggregate but the maps.
    EXPECT_EQ(RefusalOf(kScalar, over(make)),
              std::format("a correlated scalar subquery {} is not supported", kValue));
  }
  // A leaf in place of the block, and a join or a union over it: no correlated filter on the way.
  const Catalog catalog = MakeCatalog();
  auto leaf = BindSql("SELECT i16 FROM ok", catalog);
  ASSERT_TRUE(leaf.ok()) << leaf.status().ToString();
  const auto replaced = [&leaf](const LogicalNodePtr& root) {
    return WithRight(root, [&leaf](const LogicalNodePtr& right) {
      ColumnId last = MaxColumnId(*right);
      return CloneWithFreshIds(leaf->root, last);
    });
  };
  EXPECT_EQ(RefusalOf(kExists, replaced),
            std::format("a correlated EXISTS {} is not supported", kLift));
  EXPECT_EQ(RefusalOf(kScalar, replaced),
            std::format("a correlated scalar subquery {} is not supported", kValue));
  const auto unioned = [](const LogicalNodePtr& root) {
    return WithRight(root, [](const LogicalNodePtr& right) {
      return std::make_shared<const LogicalNode>(
          UnionAllNode{.inputs = {right}, .columns = {}, .ids = {}, .span = {}});
    });
  };
  EXPECT_EQ(RefusalOf(kExists, unioned),
            std::format("a correlated EXISTS {} is not supported", kLift));
}

// Decision 17's first row: each conjunct of the correlated filter is a kIsTrue over a column its
// Compute defines; any other is refused in each mode's words.
TEST(UnnestingTest, AConjunctTheFilterRuleDoesNotRecognizeIsRefused) {
  const auto unrecognized = [](const LogicalNodePtr& root) {
    return WithRight(root, [](const LogicalNodePtr& right) {
      return WithFilter(right, [](const FilterNode& filter) {
        FilterNode out = filter;
        out.predicates.front().kind = Predicate::Kind::kIsNotNull;
        return out;
      });
    });
  };
  EXPECT_EQ(RefusalOf(kExists, unrecognized),
            "a correlated EXISTS whose correlated conjunct the pass does not recognize is not "
            "supported");
  EXPECT_EQ(RefusalOf(kScalar, unrecognized),
            "a correlated scalar subquery whose correlated conjunct the pass does not recognize is "
            "not supported");
}

// Decision 19: a static aggregate's empty values. COUNT in each spelling, and a sum of counts, is
// 0; every other call NULL; each of the call's type.
TEST(UnnestingTest, EmptyValuesFollowDecisionNineteen) {
  for (const AggKind kind :
       {AggKind::kCountStar, AggKind::kCount, AggKind::kSum, AggKind::kAvg, AggKind::kMin,
        AggKind::kMax, AggKind::kCountDistinct, AggKind::kSumCounts, AggKind::kStddevSamp,
        AggKind::kStddevPop, AggKind::kVarSamp, AggKind::kVarPop, AggKind::kSingleValue}) {
    const LogicalType type = kind == AggKind::kAvg ? LogicalType::kDouble : LogicalType::kBigInt;
    const EmptyValue value = EmptyValueOf(AggregateCall{.kind = kind, .arg = {}, .type = type});
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->type, type);
    const bool count = kind == AggKind::kCountStar || kind == AggKind::kCount ||
                       kind == AggKind::kCountDistinct || kind == AggKind::kSumCounts;
    if (count) {
      EXPECT_EQ(std::get<Int128>(value->value), Int128{0});
    } else {
      EXPECT_TRUE(std::holds_alternative<std::monostate>(value->value));
    }
  }
}

}  // namespace
}  // namespace antb1::plan
