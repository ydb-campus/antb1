#include "../aggregate_placement.h"

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/plan/catalog.h"
#include "antb1/plan/explain.h"
#include "antb1/plan/logical_plan.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::BindSql;
using testing::MakeCatalog;

LogicalPlan Planned(std::string_view sql) {
  static const Catalog catalog = MakeCatalog();
  auto plan = BindSql(sql, catalog);
  EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  return plan.ok() ? Optimized(*plan) : LogicalPlan{};
}

// Each placement as "<side> <pre-grouping size>+<from the grouping set>[ padding]", so a case reads
// as the three things a rewrite would need: which input to group, how wide the pre-grouping is, and
// whether the final step has to pad an unmatched row.
std::vector<std::string> Placements(std::string_view sql) {
  std::vector<std::string> out;
  for (const AggregatePlacement& p : AggregatePlacements(Planned(sql))) {
    out.push_back(std::string(p.left_is_aggregated ? "left " : "right ") +
                  std::to_string(p.pre_grouping.size()) + "+" +
                  std::to_string(p.grouping_from_aggregated_side) +
                  (p.needs_identity ? " padding" : ""));
  }
  return out;
}

// The shape ADR 0032 is written for: the aggregates read one input, the grouping key comes from the
// other, and the pre-grouping is the join key alone. `ok` holds the aggregated side.
TEST(AggregatePlacementTest, FindsTheShapeTheAdrIsWrittenFor) {
  EXPECT_EQ(Placements("SELECT t.i16, SUM(ok.i16) FROM t, ok WHERE t.i32 = ok.i16 GROUP BY t.i16"),
            (std::vector<std::string>{"right 1+0"}));
}

// An aggregate over an expression of two columns of one input. This is the shape of one of the two
// queries the ADR measures, and it is why a Compute between the aggregate and the join has to be
// looked through rather than refused: the aggregate's argument is the Compute's own column, and
// only the expression says which input computed it.
TEST(AggregatePlacementTest, LooksThroughAComputeToTheColumnsAnExpressionReads) {
  EXPECT_EQ(Placements("SELECT t.i16, SUM(ok.i16 * 2) FROM t, ok WHERE t.i32 = ok.i16 "
                       "GROUP BY t.i16"),
            (std::vector<std::string>{"right 1+0"}));
}

// Condition 3's second half, which is the one that changes aggregate *values* when it is dropped: a
// grouping column from the aggregated side must join the pre-grouping, so the count rises to two.
TEST(AggregatePlacementTest, AGroupingColumnOfTheAggregatedSideJoinsThePreGrouping) {
  EXPECT_EQ(Placements("SELECT t.i16, ok.s, SUM(ok.i16) FROM t, ok WHERE t.i32 = ok.i16 "
                       "GROUP BY t.i16, ok.s"),
            (std::vector<std::string>{"right 2+1"}));
}

// The aggregated side is the null-supplying input, so an unmatched row of the preserved side has no
// partial to read and the final step has to supply the aggregate over the empty set.
TEST(AggregatePlacementTest, ReportsPaddingWhenTheAggregatedSideCanBeMissing) {
  EXPECT_EQ(Placements("SELECT t.i16, SUM(ok.i16) FROM t LEFT JOIN ok ON t.i32 = ok.i16 "
                       "GROUP BY t.i16"),
            (std::vector<std::string>{"right 1+0 padding"}));
}

// The preserved side aggregated instead: the join invents no row of `t`, so nothing has to be
// padded even though the join is the same outer one.
TEST(AggregatePlacementTest, NoPaddingWhenThePreservedSideIsTheAggregatedOne) {
  EXPECT_EQ(Placements("SELECT ok.i16, SUM(t.i32) FROM t LEFT JOIN ok ON t.i32 = ok.i16 "
                       "GROUP BY ok.i16"),
            (std::vector<std::string>{"left 1+0"}));
}

// The note a reader sees, pinned here because it is the whole of what G1 ships: the pass is
// internal and `EXPLAIN` is its only consumer.
TEST(AggregatePlacementTest, SaysSoInExplain) {
  const std::string plan =
      Explain(Planned("SELECT t.i16, ok.s, SUM(ok.i16) FROM t LEFT JOIN ok ON t.i32 = ok.i16 GROUP "
                      "BY t.i16, ok.s"));
  EXPECT_NE(plan.find("[groupable below the right of join, 2 keys, 1 from the grouping set, "
                      "padding]"),
            std::string::npos)
      << plan;
}

// COUNT(*) beside a call that names a side, on the null-supplying input. Reported, and the point
// of the case is the identity the note stands for: the written query answers 1 for a preserved row
// with no match, because the join gave it one padded row to count, where every other kind answers
// what it answers over no rows at all.
TEST(AggregatePlacementTest, CountStarIsTheOneWhosePaddedValueIsNotEmpty) {
  EXPECT_EQ(
      Placements("SELECT t.i16, COUNT(*), COUNT(ok.i16) FROM t LEFT JOIN ok ON t.i32 = ok.i16 "
                 "GROUP BY t.i16"),
      (std::vector<std::string>{"right 1+0 padding"}));
}

// A join key whose two types differ: the narrower side is widened by a Compute on its own branch,
// so the column the aggregated side groups by is below the join after all and the placement stands.
// Worth a case because the opposite is easy to assume.
TEST(AggregatePlacementTest, AWidenedJoinKeyIsStillBelowTheJoin) {
  EXPECT_EQ(Placements("SELECT t.i16, SUM(ok.i16) FROM t, ok WHERE t.i32 = ok.i16 GROUP BY t.i16"),
            (std::vector<std::string>{"right 1+0"}));
}

// A grouping column of the aggregated side that a Compute above the join defines. The pre-grouping
// names the column that computed it, which exists below the join, rather than the one the grouping
// set names, which does not.
TEST(AggregatePlacementTest, AComputedGroupingColumnNamesWhatComputedIt) {
  EXPECT_EQ(Placements("SELECT ok.i16 + 1, SUM(ok.i16) FROM t, ok WHERE t.i16 = ok.i16 "
                       "GROUP BY ok.i16 + 1"),
            (std::vector<std::string>{"right 1+0"}));
}

// Every refusal. Each reports nothing, and reporting nothing is always safe -- but each is here
// because it is a shape that would otherwise look legal.
TEST(AggregatePlacementTest, Refusals) {
  struct Case {
    std::string_view sql;
    std::string_view why;
  };
  for (const Case& c : {
           // An aggregate reading both inputs cannot be computed on either alone (condition 1).
           Case{.sql = "SELECT t.i16, SUM(t.i32 + ok.i16) FROM t, ok WHERE t.i32 = ok.i16 "
                       "GROUP BY t.i16",
                .why = "the aggregate's expression mixes the two inputs"},
           // Two aggregates, one per input: no single side carries both (condition 1).
           Case{.sql = "SELECT t.i16, SUM(t.i32), SUM(ok.i16) FROM t, ok WHERE t.i32 = ok.i16 "
                       "GROUP BY t.i16",
                .why = "the calls read different inputs"},
           // COUNT(DISTINCT) does not decompose unless the distinct column enters the
           // pre-grouping, which would make the pre-grouping a key of its side (condition 2).
           Case{.sql = "SELECT t.i16, COUNT(DISTINCT ok.i16) FROM t, ok WHERE t.i32 = ok.i16 "
                       "GROUP BY t.i16",
                .why = "COUNT(DISTINCT) is not decomposable"},
           // Nothing names a side: both inputs would be legal and this pass does not choose.
           Case{.sql = "SELECT t.i16, COUNT(*) FROM t, ok WHERE t.i32 = ok.i16 GROUP BY t.i16",
                .why = "every call is COUNT(*)"},
           // A global aggregate: a bound one is an AggregateNode and not a grouping at all, so
           // this reaches nothing -- kept because it is the shape a reader will ask about.
           Case{.sql = "SELECT SUM(ok.i16) FROM t, ok WHERE t.i32 = ok.i16", .why = "no GROUP BY"},
           // A semi join, whose left side a placement WOULD be legal on: refused for now, and the
           // refusal is pinned so that admitting it is a visible change rather than a silent one.
           Case{.sql = "SELECT t.i16, SUM(t.i32) FROM t WHERE t.i32 IN (SELECT i16 FROM ok) "
                       "GROUP BY t.i16",
                .why = "a semi join is refused conservatively"},
           // A residual decides which pairs match on something other than the keys, and the
           // pre-grouping destroys the per-row values it reads (condition 4).
           Case{.sql =
                    "SELECT t.i16, SUM(ok.i16) FROM t, ok WHERE t.i32 = ok.i16 AND t.i64 > ok.i16 "
                    "GROUP BY t.i16",
                .why = "the join carries a residual"},
           // A grouping column reading both inputs separates rows no pre-grouping of one input can.
           Case{.sql = "SELECT t.i32 + ok.i16, SUM(ok.i16) FROM t, ok WHERE t.i32 = ok.i16 "
                       "GROUP BY t.i32 + ok.i16",
                .why = "the grouping column reads both inputs"},
           // A Filter and a Project between the aggregate and the join. Both are refused, and the
           // pair is what a bound plan produces: a conjunct over one relation is pushed BELOW the
           // join, so a Filter reaches this position only above a derived table's Project. That is
           // why no case here isolates the Filter -- nothing produces one on its own.
           Case{.sql = "SELECT d.a, SUM(d.b) FROM (SELECT t.i16 AS a, ok.i16 AS b FROM t, ok "
                       "WHERE t.i32 = ok.i16) AS d WHERE d.b > 2 GROUP BY d.a",
                .why = "a Filter and a Project sit between the aggregate and the join"},
           // One input, so there is no join to exchange across.
           Case{.sql = "SELECT i16, SUM(i32) FROM t GROUP BY i16", .why = "no join"},
       }) {
    EXPECT_EQ(Placements(c.sql), (std::vector<std::string>{})) << c.why << ": " << c.sql;
  }
}

}  // namespace
}  // namespace antb1::plan
