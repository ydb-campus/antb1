#include "antb1/plan/shared_sub_plans.h"

#include <cstddef>
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
  EXPECT_TRUE(plan.ok()) << plan.status().ToString();
  return plan.ok() ? Optimized(*plan) : LogicalPlan{};
}

// Which node heads each group, with how many members and how many fields: "Join 2x3". The head is
// what matters as much as the count -- two references to one table almost always repeat their
// *scan*, so a test that only counted groups would pass while the node it cares about went
// unshared.
std::vector<std::string> Groups(std::string_view sql) {
  std::vector<std::string> out;
  for (const SharedSubPlan& shared : SharedSubPlans(Planned(sql))) {
    const std::string line = ExplainNode(*shared.members.front());
    out.push_back(line.substr(0, line.find(' ')) + " " + std::to_string(shared.members.size()) +
                  "x" + std::to_string(shared.fields));
  }
  return out;
}

// A CTE referenced twice is bound twice (ADR 0022), so the plan holds the subtree twice and this
// finds it, outermost node first. The shape ADR 0030 is built around.
TEST(SharedSubPlansTest, FindsTheSubPlanTwoCteReferencesRepeat) {
  EXPECT_EQ(Groups("WITH g AS (SELECT i64 AS k, COUNT(*) AS n FROM t GROUP BY i64) "
                   "SELECT COUNT(*) FROM g AS a, g AS b WHERE a.k = b.k"),
            (std::vector<std::string>{"Project 2x1"}))
      << "the Project heads it, not the Scan or the GroupAggregate inside it";
}

// The case exact structural equality misses, and the one this ADR was amended for: two sub-plans
// whose scans read different fields. Selecting fields does not change which rows a scan returns, so
// they compute the same rows and the shared one reads the union.
//
// `a.j` is used by the outer query and `b` has no such column, so projection pruning leaves a's
// scan of t reading two fields and b's reading one -- the difference is driven from *outside* the
// repeated sub-plan, which is exactly how it arises in the queries the ADR measures.
TEST(SharedSubPlansTest, FindsASubPlanWhoseScansReadDifferentFields) {
  EXPECT_EQ(Groups("SELECT COUNT(*) FROM "
                   "(SELECT t.i64 AS k, t.i32 AS j FROM t, u WHERE t.i64 = u.i64) AS a, "
                   "(SELECT t.i64 AS k FROM t, u WHERE t.i64 = u.i64) AS b "
                   "WHERE a.k = b.k AND a.j > 0"),
            (std::vector<std::string>{"Join 2x3"}))
      << "the join is shared though one side's scan of t reads a field the other's does not: "
         "two fields of t and one of u";
}

// The half that keeps answers right. In each of these the nodes that differ must not be shared --
// but the scans below them still repeat, and reporting those is correct, so what is asserted is
// *which* node heads the group rather than that there is none.
TEST(SharedSubPlansTest, SharesNoNodeWhoseContentDiffers) {
  EXPECT_EQ(Groups("SELECT COUNT(*) FROM (SELECT i64 FROM t WHERE i32 > 0) AS a, "
                   "(SELECT i64 FROM t WHERE i32 > 1) AS b WHERE a.i64 = b.i64"),
            (std::vector<std::string>{"Scan 2x2"}))
      << "a different constant: the filters are not shared, the scans under them are";
  EXPECT_TRUE(Groups("SELECT COUNT(*) FROM (SELECT i64 FROM t WHERE i32 > 0) AS a, "
                     "(SELECT i64 FROM t WHERE i16 > 0) AS b WHERE a.i64 = b.i64")
                  .empty())
      << "a different column, and nothing at all is reported: the filters differ, and the scans "
         "below them read {i64, i32} and {i64, i16} -- overlapping but neither inside the other, "
         "so no one member's result could serve both and the group is not reported";
  EXPECT_TRUE(Groups("SELECT COUNT(*) FROM t AS a, u AS b WHERE a.i64 = b.i64").empty())
      << "a different table shares nothing at all";
  // A DECIMAL constant against the column's own type and a DOUBLE one, which ADR 0021 rule 11
  // compares differently. These two are told apart by their text alone (ToString renders them
  // "1.50" and "1.5"), so this pins the behaviour without exercising ConstantKey's type component
  // -- see the comment on ConstantKey for why that component is there regardless. No SQL of this
  // subset spells one literal at two types, so the case it guards is not reachable to write a test
  // for: a predicate's constant is typed as its column, and an integer literal has one type.
  EXPECT_EQ(Groups("SELECT COUNT(*) FROM (SELECT p FROM dec WHERE p > 1.5) AS a, "
                   "(SELECT p FROM dec WHERE p > 1.5e0) AS b WHERE a.p = b.p"),
            (std::vector<std::string>{"Scan 2x1"}))
      << "two constants of different types must not be shared";
}

// An ordered or limited sub-plan is never a member: a shared result cannot hand two consumers an
// order without deciding whose wins. The scans below them repeat and are reported, which is the
// point -- refusing the Sort costs the report nothing it could have used.
TEST(SharedSubPlansTest, SharesNoOrderedOrLimitedSubPlan) {
  const std::vector<std::string> groups = Groups(
      "SELECT COUNT(*) FROM (SELECT i64 FROM t ORDER BY i64 LIMIT 5) AS a, "
      "(SELECT i64 FROM t ORDER BY i64 LIMIT 5) AS b WHERE a.i64 = b.i64");
  EXPECT_EQ(groups, (std::vector<std::string>{"Scan 2x1"}));
  for (const std::string& group : groups) {
    EXPECT_EQ(group.find("Sort"), std::string::npos);
    EXPECT_EQ(group.find("Limit"), std::string::npos);
  }
}

// Three references give one group of three, not three groups of two.
TEST(SharedSubPlansTest, GroupsEveryMemberTogether) {
  EXPECT_EQ(Groups("WITH g AS (SELECT i64 AS k, COUNT(*) AS n FROM t GROUP BY i64) "
                   "SELECT COUNT(*) FROM g AS a, g AS b, g AS c "
                   "WHERE a.k = b.k AND b.k = c.k"),
            (std::vector<std::string>{"Project 3x1"}));
}

// A bare repeated scan is reported. Whether it is worth materialising is the cost gate's question,
// and the cost gate is E2's: a pass that reports must not also decide.
TEST(SharedSubPlansTest, ReportsABareRepeatedScan) {
  EXPECT_EQ(Groups("SELECT COUNT(*) FROM t AS a, t AS b WHERE a.i64 = b.i64"),
            (std::vector<std::string>{"Scan 2x1"}));
}

TEST(SharedSubPlansTest, FindsNothingWhereNothingRepeats) {
  EXPECT_TRUE(Groups("SELECT i64 FROM t WHERE i32 > 0").empty());
  EXPECT_TRUE(Groups("SELECT COUNT(*) FROM t").empty());
  EXPECT_TRUE(SharedSubPlans(LogicalPlan{}).empty()) << "and no root at all";
}

// The same plan gives the same groups in the same order, which EXPLAIN's goldens depend on.
TEST(SharedSubPlansTest, IsDeterministic) {
  static constexpr std::string_view kSql =
      "WITH g AS (SELECT i64 AS k, COUNT(*) AS n FROM t GROUP BY i64) "
      "SELECT COUNT(*) FROM g AS a, g AS b WHERE a.k = b.k";
  const std::vector<std::string> first = Groups(kSql);
  ASSERT_FALSE(first.empty());
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(Groups(kSql), first);
  }
}

}  // namespace
}  // namespace antb1::plan
