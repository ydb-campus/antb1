#include "antb1/plan/shared_sub_plans.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/plan/catalog.h"
#include "antb1/plan/explain.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"
#include "antb1/plan/types.h"

#include "../predicate_domain.h"
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
std::vector<std::string> GroupsOf(const LogicalPlan& plan) {
  std::vector<std::string> out;
  for (const SharedSubPlan& shared : SharedSubPlans(plan)) {
    const std::string line = ExplainNode(*shared.members.front());
    out.push_back(line.substr(0, line.find(' ')) + " " + std::to_string(shared.members.size()) +
                  "x" + std::to_string(shared.fields));
  }
  return out;
}

std::vector<std::string> Groups(std::string_view sql) { return GroupsOf(Planned(sql)); }

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
  // Casts of one column to DECIMALs that differ only in scale or width compute different values
  // (ADR 0037's CT2 rounds); an expression's key is its full type, not the type's kind.
  EXPECT_EQ(
      Groups("SELECT COUNT(*) FROM "
             "(SELECT CAST(p AS DECIMAL(15,1)) AS x, COUNT(*) AS n FROM dec GROUP BY 1) AS a, "
             "(SELECT CAST(p AS DECIMAL(15,0)) AS x, COUNT(*) AS n FROM dec GROUP BY 1) AS b "
             "WHERE a.n = b.n"),
      (std::vector<std::string>{"Scan 2x1"}))
      << "casts to DECIMAL(15,1) and DECIMAL(15,0) must not be shared";
  EXPECT_EQ(
      Groups("SELECT COUNT(*) FROM "
             "(SELECT CAST(p AS DECIMAL(16,2)) AS x, COUNT(*) AS n FROM dec GROUP BY 1) AS a, "
             "(SELECT CAST(p AS DECIMAL(17,2)) AS x, COUNT(*) AS n FROM dec GROUP BY 1) AS b "
             "WHERE a.n = b.n"),
      (std::vector<std::string>{"Scan 2x1"}))
      << "nor casts to DECIMAL(16,2) and DECIMAL(17,2)";
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

TEST(SharedSubPlansTest, CostsTheJointDomainOfFilteredGroupingKeys) {
  constexpr int64_t kTickets = kMaxSharedResultBytes / (int64_t{5} * 36);
  auto readings =
      std::make_shared<testing::FakeTable>(arrow::schema({arrow::field("ticket", arrow::int64()),
                                                          arrow::field("sensor", arrow::int64())}),
                                           2'000'000'000);
  readings->WithStats(0, 0, PartStats{.min = 1, .max = kTickets, .rows = 2'000'000'000});
  auto sensors = std::make_shared<testing::FakeTable>(
      arrow::schema({arrow::field("id", arrow::int64()), arrow::field("band", arrow::int64()),
                     arrow::field("slot", arrow::int64())}),
      1000);
  sensors->WithStats(0, 0, PartStats{.min = 1, .max = 1000, .rows = 1000})
      .WithStats(0, 1, PartStats{.min = 1, .max = 100, .rows = 1000});
  Catalog catalog;
  ASSERT_TRUE(catalog.Register("readings", readings).ok());
  ASSERT_TRUE(catalog.Register("sensors", sensors).ok());

  // One band admits any of four slots; another admits only one. Five pairs fit the cap
  // with 32 bytes of values and four bytes reserved for validity per group.
  // A sixth pair or one more ticket pushes the result past the admission limit.
  constexpr std::string_view kFivePairs = "d.band = 2 OR (d.band = 4 AND d.slot = 3)";
  constexpr std::string_view kInner = "readings AS r JOIN sensors AS d ON r.sensor = d.id";
  const auto expect_sharing = [&](std::string_view from, std::string_view filter,
                                  int64_t null_slots, bool expected) {
    SCOPED_TRACE(std::format("{} with filter={}, NULL slots={}", from, filter, null_slots));
    sensors->WithStats(0, 2, PartStats{.min = 1, .max = 4, .null_count = null_slots, .rows = 1000});
    const auto bound = BindSql(
        std::format("WITH g AS (SELECT r.ticket, d.band, d.slot, COUNT(*) AS n FROM {} AND ({}) "
                    "GROUP BY r.ticket, d.band, d.slot) "
                    "SELECT a.ticket, a.band, a.slot, a.n, b.n, c.n FROM g AS a "
                    "JOIN g AS b ON a.ticket = b.ticket JOIN g AS c ON a.ticket = c.ticket",
                    from, filter),
        catalog);
    ASSERT_TRUE(bound.ok()) << bound.status().ToString();
    const auto groups = SharedSubPlans(*bound);
    ASSERT_EQ(groups.size(), 1U) << Explain(*bound);
    ASSERT_EQ(groups.front().members.size(), 3U);
    int exhausted = 0;
    EXPECT_FALSE(WorthSharing(groups.front(), exhausted))
        << "the original group estimate exceeds the byte cap";
    EXPECT_EQ(WorthSharing(groups.front()), expected) << Explain(*bound);
  };
  expect_sharing(kInner, kFivePairs, 0, true);
  expect_sharing(kInner, kFivePairs, 1, false);  // NULL in the first arm admits a sixth pair.
  expect_sharing(kInner, "d.band = 2 OR d.slot = 3", 0, false);  // Every band can match.
  expect_sharing("readings AS r LEFT JOIN sensors AS d ON r.sensor = d.id", kFivePairs, 0, false);
  // Padding on a sibling branch can similarly add a NULL ticket.
  expect_sharing(
      "sensors AS p LEFT JOIN readings AS r ON p.id = r.sensor "
      "JOIN sensors AS d ON p.id = d.id",
      kFivePairs, 0, false);
  readings->WithStats(
      0, 0, PartStats{.min = 1, .max = kTickets, .null_count = 10, .rows = 2'000'000'000});
  expect_sharing(kInner, kFivePairs, 0, false);
}

TEST(SharedSubPlansTest, RequiresATableWideBoundForKeysOutsideTheFilter) {
  constexpr int64_t kPartRows = 1'000'000;
  constexpr int kParts = 20;
  auto tickets = std::make_shared<testing::FakeTable>(
      arrow::schema({arrow::field("id", arrow::int64()), arrow::field("label", arrow::int64()),
                     arrow::field("region", arrow::int64())}),
      kPartRows * kParts);
  tickets->WithParts(std::vector<int64_t>(kParts, kPartRows));
  for (int part = 0; part < kParts; ++part) {
    // The twenty parts may have disjoint IDs.
    tickets->WithDistinctCount(part, 0, kPartRows).WithDistinctCount(part, 2, 1);
  }
  auto labels = std::make_shared<testing::FakeTable>(
      arrow::schema({arrow::field("id", arrow::int64()), arrow::field("band", arrow::int64())}),
      20'000'000);
  labels->WithStats(0, 1, PartStats{.min = 1, .max = 100, .rows = 20'000'000});
  Catalog catalog;
  ASSERT_TRUE(catalog.Register("tickets", tickets).ok());
  ASSERT_TRUE(catalog.Register("labels", labels).ok());
  const auto bound = BindSql(
      "WITH g AS (SELECT t.id, t.region, d.band, COUNT(*) AS n FROM tickets t JOIN labels d "
      "ON t.label = d.id AND d.band = 1 GROUP BY t.id, t.region, d.band) "
      "SELECT a.id, a.region, a.band, a.n, b.n FROM g a JOIN g b ON a.id = b.id",
      catalog);
  ASSERT_TRUE(bound.ok()) << bound.status().ToString();
  const auto groups = SharedSubPlans(*bound);
  ASSERT_EQ(groups.size(), 1U);
  int exhausted = 0;
  ASSERT_FALSE(WorthSharing(groups.front(), exhausted)) << "the initial estimate exceeds the cap";
  EXPECT_FALSE(WorthSharing(groups.front()));
  // Each part now contributes at most (150,000 + NULL) IDs times (one region + NULL).
  // Both keys come from the same row, so there are twenty sets of tuples.
  for (int part = 0; part < kParts; ++part) {
    tickets->WithDistinctCount(part, 0, 150'000);
  }
  EXPECT_FALSE(WorthSharing(groups.front(), exhausted));
  EXPECT_TRUE(WorthSharing(groups.front()));
}

TEST(SharedSubPlansTest, ReservesValidityBytesForNullableAggregateResults) {
  constexpr int64_t kRows = 100'000'000;
  constexpr int64_t kGroups = kMaxSharedResultBytes / 32;
  auto table = std::make_shared<testing::FakeTable>(
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("band", arrow::int64()),
                     arrow::field("v", arrow::int64())}),
      kRows);
  table->WithStats(0, 1, PartStats{.min = 1, .max = 100, .rows = kRows})
      .WithStats(0, 2, PartStats{.min = 1, .max = 10, .null_count = kRows / 2, .rows = kRows});
  Catalog catalog;
  ASSERT_TRUE(catalog.Register("t", table).ok());
  const auto bound = BindSql(
      "WITH g AS (SELECT k, band, MIN(v) AS lo, MAX(v) AS hi FROM t WHERE band = 1 "
      "GROUP BY k, band) SELECT a.k, a.band, a.lo, a.hi, b.lo FROM g a JOIN g b ON a.k = b.k",
      catalog);
  ASSERT_TRUE(bound.ok()) << bound.status().ToString();
  const auto groups = SharedSubPlans(*bound);
  ASSERT_EQ(groups.size(), 1U);
  // The four BIGINT value buffers alone can fill the cap. A group with only NULL v values
  // and another with a value require validity bitmaps for MIN and MAX.
  table->WithStats(0, 0, PartStats{.min = 1, .max = kGroups, .rows = kRows});
  EXPECT_FALSE(WorthSharing(groups.front()));
  table->WithStats(0, 0, PartStats{.min = 1, .max = kGroups / 2, .rows = kRows});
  int exhausted = 0;
  EXPECT_FALSE(WorthSharing(groups.front(), exhausted));
  EXPECT_TRUE(WorthSharing(groups.front())) << "the smaller filtered result fits with validity";
}

TEST(SharedSubPlansTest, RequiresTheWholeDisjunctionWithinThePlanningBudget) {
  const auto catalog = MakeCatalog();
  for (const bool restrict_both_arms : {false, true}) {
    const std::string query =
        "SELECT i64 FROM t WHERE i64 = 17 OR " +
        std::string(restrict_both_arms ? "(i32 IS NULL AND i64 IN (26, 27))" : "i32 IS NULL");
    const auto bound = BindSql(query, catalog);
    ASSERT_TRUE(bound.ok()) << bound.status().ToString();
    const auto* project = std::get_if<ProjectNode>(bound->root.get());
    ASSERT_NE(project, nullptr) << Explain(*bound);
    const auto* filter = std::get_if<FilterNode>(project->input.get());
    ASSERT_NE(filter, nullptr) << Explain(*bound);
    const std::vector keys{
        PredicateDomainKey{.id = project->columns.front().id, .max_values = std::nullopt}};
    int budget = 2;
    EXPECT_EQ(PredicateTupleBound(*filter, keys, budget), std::nullopt);
    EXPECT_EQ(budget, 0);
    budget = kPredicateDomainNodeBudget;
    EXPECT_EQ(PredicateTupleBound(*filter, keys, budget),
              restrict_both_arms ? std::optional<int64_t>(3) : std::nullopt)
        << Explain(*bound);
  }
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

// ---- UnionAll (ADR 0036) ----

LogicalNodePtr Node(LogicalNode node) {
  return std::make_shared<const LogicalNode>(std::move(node));
}

// GROUP BY field 2 of `table` with COUNT(*), its columns minted from `first`: a union's branch.
LogicalNodePtr Grouped(const std::string& table, std::uint32_t first) {
  return Node(GroupAggregateNode{
      .input = Node(
          ScanNode{.table = std::make_shared<testing::FakeTable>(testing::AllTypesSchema(), 100),
                   .table_name = table,
                   .fields = {2},
                   .ids = {ColumnId{first}},
                   .span = {}}),
      .keys = {BoundColumn{.id = ColumnId{first}, .name = "i64", .type = LogicalType::kBigInt}},
      .key_ids = {ColumnId{first + 1}},
      .aggregates = {AggregateCall{.kind = AggKind::kCountStar, .id = ColumnId{first + 2}}},
      .span = {}});
}

// The union of a branch over t and one over u (u's first with `swapped`), its columns minted from
// `first`: what a CTE whose body is a UNION ALL binds to at each reference.
LogicalNodePtr UnionOfGroups(std::uint32_t first, bool swapped, bool sorted) {
  std::vector<LogicalNodePtr> inputs = {Grouped("t", first), Grouped("u", first + 10)};
  if (sorted) {
    // A Sort has no key, so neither has the union above it.
    inputs[0] =
        Node(SortNode{.input = inputs[0],
                      .keys = {SortKey{.column = BoundColumn{.id = ColumnId{first + 1},
                                                             .type = LogicalType::kBigInt}}},
                      .span = {}});
  }
  if (swapped) {
    std::swap(inputs[0], inputs[1]);
  }
  UnionAllNode node{.inputs = inputs, .columns = {}, .ids = {}, .span = {}};
  for (const LogicalNodePtr& input : inputs) {
    std::vector<BoundColumn>& columns = node.columns.emplace_back();
    for (const ColumnId id : OutputIds(*input)) {
      columns.push_back(BoundColumn{.id = id, .name = "c", .type = LogicalType::kBigInt});
    }
  }
  node.ids = {ColumnId{first + 20}, ColumnId{first + 21}};
  return Node(std::move(node));
}

// Two references to such a CTE, joined on their first columns.
LogicalPlan JoinOfUnions(bool swapped, bool sorted = false) {
  const LogicalNodePtr join = Node(JoinNode{
      .kind = JoinKind::kInner,
      .left = UnionOfGroups(100, false, sorted),
      .right = UnionOfGroups(200, swapped, sorted),
      .keys = {JoinKey{.left = BoundColumn{.id = ColumnId{120}, .type = LogicalType::kBigInt},
                       .right = BoundColumn{.id = ColumnId{220}, .type = LogicalType::kBigInt}}},
      .span = {}});
  std::vector<OutputColumn> output;
  for (const ColumnId id : OutputIds(*join)) {
    output.push_back(OutputColumn{.name = "c", .type = LogicalType::kBigInt, .id = id});
  }
  return ResolvePositions(LogicalPlan{.root = join, .output = std::move(output)});
}

// A union repeated whole is one group, headed by the union, which exec declines to hold
// (WorthSharing); nothing under it is a member, so the aggregate-topped branches are never
// reported on their own. Held one by one, a branch of that shape can outgrow
// MaterializedTable::kMaxBytes at large scale and fail the query instead of answering it.
TEST(SharedSubPlansTest, AUnionRepeatedWholeIsOneGroupAndItsBranchesAreNotShared) {
  EXPECT_EQ(GroupsOf(JoinOfUnions(/*swapped=*/false)), (std::vector<std::string>{"UnionAll 2x2"}));
}

// A union's rows come in its inputs' order, so the same branches in another order are not the same
// sub-plan: the unions are not grouped -- and the branches, which do repeat, are not either, since
// nothing under a union is a member.
TEST(SharedSubPlansTest, UnionsOfTheSameBranchesInAnotherOrderAreNotGrouped) {
  EXPECT_TRUE(GroupsOf(JoinOfUnions(/*swapped=*/true)).empty());
}

// A union whose branch holds a node without a key has no key itself, and its branches are still no
// members: what keeps them unshared is where they are, not what the union above them holds.
TEST(SharedSubPlansTest, NothingUnderAUnionWithoutAKeyIsAMember) {
  EXPECT_TRUE(GroupsOf(JoinOfUnions(/*swapped=*/false, /*sorted=*/true)).empty());
}

// A union of one input, as the optimizer leaves a union whose other branches the conditions on it
// rule out (ADR 0036's X7a), columns minted from `first`.
LogicalNodePtr UnionOfOneGroup(std::uint32_t first) {
  const LogicalNodePtr branch = Grouped("t", first);
  UnionAllNode node{.inputs = {branch}, .columns = {{}}, .ids = {}, .span = {}};
  for (const ColumnId id : OutputIds(*branch)) {
    node.columns[0].push_back(BoundColumn{.id = id, .name = "c", .type = LogicalType::kBigInt});
  }
  node.ids = {ColumnId{first + 20}, ColumnId{first + 21}};
  return Node(std::move(node));
}

LogicalPlan JoinOf(LogicalNodePtr left, LogicalNodePtr right, ColumnId left_key,
                   ColumnId right_key) {
  const LogicalNodePtr join = Node(JoinNode{
      .kind = JoinKind::kInner,
      .left = std::move(left),
      .right = std::move(right),
      .keys = {JoinKey{.left = BoundColumn{.id = left_key, .type = LogicalType::kBigInt},
                       .right = BoundColumn{.id = right_key, .type = LogicalType::kBigInt}}},
      .span = {}});
  std::vector<OutputColumn> output;
  for (const ColumnId id : OutputIds(*join)) {
    output.push_back(OutputColumn{.name = "c", .type = LogicalType::kBigInt, .id = id});
  }
  return ResolvePositions(LogicalPlan{.root = join, .output = std::move(output)});
}

// A union pruned to one branch stays a union, so two references of a CTE that keep the same branch
// are one group headed by the union, which exec declines to hold, and their aggregate-topped
// branch is no member: before ADR 0037's PF5 gives the cap a fallback, holding it could fail the
// query at a large scale factor.
TEST(SharedSubPlansTest, AUnionOfOneInputIsStillTheGroupsHead) {
  EXPECT_EQ(
      GroupsOf(JoinOf(UnionOfOneGroup(100), UnionOfOneGroup(200), ColumnId{120}, ColumnId{220})),
      (std::vector<std::string>{"UnionAll 2x1"}));
}

// The same aggregate-topped branch outside any union is a sub-plan of its own, never one group with
// the branch under a union of one input.
TEST(SharedSubPlansTest, ABranchUnderAUnionOfOneInputIsNoMember) {
  EXPECT_TRUE(
      GroupsOf(JoinOf(UnionOfOneGroup(100), Grouped("t", 200), ColumnId{120}, ColumnId{201}))
          .empty());
  // The control: outside any union, the two are one group.
  EXPECT_FALSE(
      GroupsOf(JoinOf(Grouped("t", 100), Grouped("t", 200), ColumnId{101}, ColumnId{201})).empty());
}

LogicalNodePtr ScanOf(const std::string& table, std::uint32_t id) {
  return Node(
      ScanNode{.table = std::make_shared<testing::FakeTable>(testing::AllTypesSchema(), 100),
               .table_name = table,
               .fields = {2},
               .ids = {ColumnId{id}},
               .span = {}});
}

// An inner join of `left` and `right` on their first columns.
LogicalNodePtr JoinedOnFirst(const LogicalNodePtr& left, const LogicalNodePtr& right) {
  return Node(JoinNode{
      .kind = JoinKind::kInner,
      .left = left,
      .right = right,
      .keys = {JoinKey{
          .left = BoundColumn{.id = OutputIds(*left).front(), .type = LogicalType::kBigInt},
          .right = BoundColumn{.id = OutputIds(*right).front(), .type = LogicalType::kBigInt}}},
      .span = {}});
}

// GROUP BY the first column of `input` with COUNT(*), its columns minted from `first`.
LogicalNodePtr GroupedOn(const LogicalNodePtr& input, std::uint32_t first) {
  return Node(GroupAggregateNode{
      .input = input,
      .keys = {BoundColumn{
          .id = OutputIds(*input).front(), .name = "c", .type = LogicalType::kBigInt}},
      .key_ids = {ColumnId{first}},
      .aggregates = {AggregateCall{.kind = AggKind::kCountStar, .id = ColumnId{first + 1}}},
      .span = {}});
}

LogicalPlan PlanOf(const LogicalNodePtr& root) {
  std::vector<OutputColumn> output;
  for (const ColumnId id : OutputIds(*root)) {
    output.push_back(OutputColumn{.name = "c", .type = LogicalType::kBigInt, .id = id});
  }
  return ResolvePositions(LogicalPlan{.root = root, .output = std::move(output)});
}

// A copy of a repeated sub-plan under a union neither is a member nor reports a group: the
// sub-plan's other two copies lie inside the members of a larger group, which is the one reported,
// and a group nested wholly in it would be computed and held for no consumer.
TEST(SharedSubPlansTest, ACopyUnderAUnionReportsNoNestedGroup) {
  // X groups a scan of big; G groups X joined to a scan of small, and is referenced twice; a third
  // copy of X is a branch of a union that occurs once.
  const auto g = [](std::uint32_t first) {
    return GroupedOn(JoinedOnFirst(Grouped("big", first), ScanOf("small", first + 5)), first + 10);
  };
  UnionAllNode u{.inputs = {Grouped("big", 300), Grouped("other", 310)},
                 .columns = {},
                 .ids = {ColumnId{320}, ColumnId{321}},
                 .span = {}};
  for (const LogicalNodePtr& input : u.inputs) {
    std::vector<BoundColumn>& columns = u.columns.emplace_back();
    for (const ColumnId id : OutputIds(*input)) {
      columns.push_back(BoundColumn{.id = id, .name = "c", .type = LogicalType::kBigInt});
    }
  }
  const LogicalNodePtr root = JoinedOnFirst(JoinedOnFirst(g(100), g(200)), Node(std::move(u)));
  EXPECT_EQ(GroupsOf(PlanOf(root)), (std::vector<std::string>{"GroupAggregate 2x2"}));
}

// A group above a union is found as any other: an aggregation of a union, repeated, is one group
// headed by the aggregation -- which the gate may hold, since it reduces.
TEST(SharedSubPlansTest, AGroupAboveAUnionIsFound) {
  const LogicalNodePtr root = JoinedOnFirst(GroupedOn(UnionOfGroups(100, false, false), 400),
                                            GroupedOn(UnionOfGroups(200, false, false), 500));
  EXPECT_EQ(GroupsOf(PlanOf(root)), (std::vector<std::string>{"GroupAggregate 2x2"}));
}

// **The two sides of a self-join are not one place.** A table's field is one origin wherever it
// is read, so these two sub-plans -- one join, grouped by `a.i64` in one and by `b.i64` in the
// other, which give different groups -- once keyed as one, and exec answered one from the other's
// result. Each reference now carries its path, which tells the two sides apart.
TEST(SharedSubPlansTest, TheSidesOfASelfJoinAreNotOnePlace) {
  const std::vector<std::string> groups = Groups(
      "SELECT COUNT(*) FROM "
      "(SELECT a.i64 AS g, COUNT(*) AS n FROM t AS a, t AS b WHERE a.i32 = b.i32 AND b.i16 > 3 "
      "GROUP BY a.i64) AS s1, "
      "(SELECT b.i64 AS g, COUNT(*) AS n FROM t AS a, t AS b WHERE a.i32 = b.i32 AND b.i16 > 3 "
      "GROUP BY b.i64) AS s2 "
      "WHERE s1.g = s2.g");
  // Not even the joins under them: one outputs `a.i64` and the other `b.i64`, so neither's output
  // holds the other's, and one result could not serve both. Matched by origin alone, `b.i64`
  // would have been read from `a.i64`'s place.
  EXPECT_TRUE(groups.empty()) << groups.front();
}

// **Nor are two aggregations' counts one column.** COUNT(*) has no argument, so every COUNT(*)
// has one origin whatever it counts: a sum of one side's count and a sum of the other's once
// keyed as one. The path of each read tells them apart, and the join they share is what is
// reported.
TEST(SharedSubPlansTest, TwoAggregationsCountsAreNotOneColumn) {
  constexpr std::string_view kCounts =
      "(SELECT i64 AS k, COUNT(*) AS n FROM t GROUP BY i64) AS x, "
      "(SELECT i64 AS k, COUNT(*) AS n FROM u GROUP BY i64) AS y WHERE x.k = y.k GROUP BY x.k";
  const std::vector<std::string> groups =
      Groups(std::format("SELECT COUNT(*) FROM (SELECT x.k, SUM(x.n) AS s FROM {}) AS p, "
                         "(SELECT x.k, SUM(y.n) AS s FROM {}) AS q WHERE p.k = q.k",
                         kCounts, kCounts));
  ASSERT_EQ(groups.size(), 1U);
  EXPECT_EQ(groups[0].substr(0, groups[0].find(' ')), "Join") << groups[0];
}

// A join on a NULL-equal key matches NULL to NULL, which the same join on an `=` key does not
// (ADR 0037's OJ5): the two compute different rows and are not one sub-plan, while the scans under
// them still are. The control: two joins whose keys agree are one group, headed by the join.
TEST(SharedSubPlansTest, AJoinOnANullEqualKeyIsNotOneWithTheJoinOnAnEqualOne) {
  const auto joined = [](std::uint32_t first, bool null_equal) {
    JoinNode join = std::get<JoinNode>(*JoinedOnFirst(ScanOf("t", first), ScanOf("u", first + 1)));
    join.keys.front().null_equal = null_equal;
    return Node(std::move(join));
  };
  for (const bool first : {false, true}) {
    for (const bool second : {false, true}) {
      SCOPED_TRACE(std::format("NULL-equal: {} and {}", first, second));
      const std::vector<std::string> groups =
          GroupsOf(PlanOf(JoinedOnFirst(joined(10, first), joined(20, second))));
      ASSERT_FALSE(groups.empty());
      const bool joins = std::ranges::any_of(
          groups, [](const std::string& group) { return group.starts_with("Join "); });
      EXPECT_EQ(joins, first == second) << groups.front();
    }
  }
}

// A mark join's mark (ADR 0041's SQ7a) has its origin in the join's keys: a CTE that computes over
// one, referenced twice, is one sub-plan up to its Project, while two marks over other probes of
// the same tables are not one column, so what reads them is not shared.
TEST(SharedSubPlansTest, AMarkIsOneColumnOnlyOverTheSameKeys) {
  EXPECT_EQ(Groups("WITH g AS (SELECT i64, CASE WHEN i32 IN (SELECT i16 FROM u) THEN 1 ELSE 0 END "
                   "AS m FROM t) SELECT COUNT(*) FROM g AS a, g AS b WHERE a.i64 = b.i64 AND "
                   "a.m = b.m"),
            (std::vector<std::string>{"Project 2x3"}))
      << "the CTE's Project heads it, its mark join and the Compute that reads the mark inside";
  EXPECT_EQ(Groups("SELECT COUNT(*) FROM (SELECT i64, CASE WHEN i32 IN (SELECT i16 FROM u) THEN 1 "
                   "ELSE 0 END AS m FROM t) AS a, (SELECT i64, CASE WHEN i16 IN (SELECT i16 FROM "
                   "u) THEN 1 ELSE 0 END AS m FROM t) AS b WHERE a.i64 = b.i64 AND a.m = b.m"),
            (std::vector<std::string>{"Project 2x1"}))
      << "only the set's sub-plan, read by both marks, is one";
}

// A correlated EXISTS's mark (ADR 0041's SQ8a) has its origin in the mark join's kind, keys and
// residuals: a CTE that computes over one, referenced twice, is one sub-plan up to its Project,
// while two marks of one subtree whose residuals differ are not one column, so what reads them is
// not shared.
TEST(SharedSubPlansTest, ACorrelatedMarkIsOneColumnOnlyOverTheSameResiduals) {
  EXPECT_EQ(
      Groups("WITH g AS (SELECT i64, CASE WHEN EXISTS (SELECT 1 FROM u WHERE u.i64 = t.i64 AND "
             "u.i32 > t.i32) THEN 1 ELSE 0 END AS m FROM t) SELECT COUNT(*) FROM g AS a, g AS b "
             "WHERE a.i64 = b.i64 AND a.m = b.m"),
      (std::vector<std::string>{"Project 2x4"}))
      << "the CTE's Project heads it, its mark join and the Compute that reads the mark inside";
  EXPECT_EQ(
      Groups("SELECT COUNT(*) FROM (SELECT i64, CASE WHEN EXISTS (SELECT 1 FROM u WHERE u.i64 = "
             "t.i64 AND u.i32 > t.i32) THEN 1 ELSE 0 END AS m FROM t) AS a, (SELECT i64, CASE "
             "WHEN EXISTS (SELECT 1 FROM u WHERE u.i64 = t.i64 AND u.i32 < t.i32) THEN 1 ELSE 0 "
             "END AS m FROM t) AS b WHERE a.i64 = b.i64 AND a.m = b.m"),
      (std::vector<std::string>{"Scan 2x2", "Scan 2x2"}))
      << "only the scans under the two marks are one";
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
