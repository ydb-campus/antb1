#include "../join_order.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"
#include "antb1/plan/types.h"

#include "../scope.h"
#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::FakeTable;

constexpr int64_t kLargest = std::numeric_limits<int64_t>::max();

std::vector<JoinRelation> Relations(const std::vector<std::optional<int64_t>>& rows) {
  std::vector<JoinRelation> out;
  out.reserve(rows.size());
  for (const std::optional<int64_t>& r : rows) {
    out.push_back(JoinRelation{.rows = r});
  }
  return out;
}

JoinEdge Edge(std::size_t a, std::size_t b, std::optional<int64_t> a_domain = std::nullopt,
              std::optional<int64_t> b_domain = std::nullopt) {
  return JoinEdge{.a = a, .b = b, .a_domain = a_domain, .b_domain = b_domain};
}

// The relations of the steps, in order.
std::vector<std::size_t> Order(const std::vector<JoinStep>& steps) {
  std::vector<std::size_t> out;
  out.reserve(steps.size());
  for (const JoinStep& step : steps) {
    out.push_back(step.relation);
  }
  return out;
}

std::vector<int64_t> Estimates(const std::vector<JoinStep>& steps) {
  std::vector<int64_t> out;
  out.reserve(steps.size());
  for (const JoinStep& step : steps) {
    out.push_back(step.estimate);
  }
  return out;
}

std::vector<std::vector<std::size_t>> Keys(const std::vector<JoinStep>& steps) {
  std::vector<std::vector<std::size_t>> out;
  out.reserve(steps.size());
  for (const JoinStep& step : steps) {
    out.push_back(step.edges);
  }
  return out;
}

// Rule 1 of the join order: the probe is the relation with the most rows, the first of them in
// FROM order on a tie, and a relation of unknown rows counts as the largest. It is the first step,
// without keys, and estimated at its rows.
TEST(JoinOrderTest, TheProbeIsTheLargestRelation) {
  const std::vector<JoinEdge> chain = {Edge(0, 1), Edge(1, 2), Edge(2, 3)};
  const auto steps = OrderJoins(Relations({10, 30, 20, 30}), chain);
  ASSERT_EQ(steps.size(), 4U);
  EXPECT_EQ(steps.front().relation, 1U);
  EXPECT_TRUE(steps.front().edges.empty());
  EXPECT_EQ(steps.front().estimate, 30);
  const auto unknown = OrderJoins(Relations({10, std::nullopt, 50, std::nullopt}), chain);
  ASSERT_EQ(unknown.size(), 4U);
  EXPECT_EQ(unknown.front().relation, 1U);
  EXPECT_EQ(unknown.front().estimate, kLargest);
  const auto one = OrderJoins(Relations({7}), {});
  ASSERT_EQ(one.size(), 1U);
  EXPECT_EQ(one.front().relation, 0U);
  EXPECT_EQ(one.front().estimate, 7);
  EXPECT_TRUE(OrderJoins({}, {}).empty());
}

// Rule 2: of the relations joined to those already joined, the smallest estimate |L| x |R| /
// max(domain) comes next, whatever its own size; every join builds on the relation it adds.
TEST(JoinOrderTest, TheSmallestEstimateJoinsNext) {
  // f(1000) joins a(100) on a key of 1000 values (f's) against 100, and b(10) on 10 values.
  const auto steps = OrderJoins(Relations({1000, 100, 10}),
                                std::vector{Edge(0, 1, 1000, 100), Edge(0, 2, 10, 10)});
  EXPECT_EQ(Order(steps), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(Estimates(steps), (std::vector<int64_t>{1000, 100, 100}));
  EXPECT_EQ(Keys(steps), (std::vector<std::vector<std::size_t>>{{}, {0}, {1}}));
  for (const JoinStep& step : steps) {
    EXPECT_EQ(step.build, BuildSide::kRight);
  }
  // A relation without an edge to the joined ones waits for one: c joins only through b.
  const auto through =
      OrderJoins(Relations({1000, 5, 100}), std::vector{Edge(1, 2), Edge(0, 2, 100, 100)});
  EXPECT_EQ(Order(through), (std::vector<std::size_t>{0, 2, 1}));
  EXPECT_EQ(Keys(through), (std::vector<std::vector<std::size_t>>{{}, {1}, {0}}));
}

// The domains keep a many-to-many edge (few values on both sides) out of the order while a key edge
// is available (ADR 0022): f(1000) has the key edge to x(500), 500 values on both sides, and the
// many-to-many edge to m(10), 2 values on both sides, whose join gives 5000 rows. With the domains
// x comes first; the control, row counts alone (unknown domains), takes m first.
TEST(JoinOrderTest, AManyToManyEdgeWaitsWhileAKeyEdgeIsAvailable) {
  const std::vector<JoinRelation> relations = Relations({1000, 500, 10});
  const auto with_domains =
      OrderJoins(relations, std::vector{Edge(0, 1, 500, 500), Edge(0, 2, 2, 2)});
  EXPECT_EQ(Order(with_domains), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(Estimates(with_domains), (std::vector<int64_t>{1000, 1000, 5000}));
  const auto rows_only = OrderJoins(relations, std::vector{Edge(0, 1), Edge(0, 2)});
  EXPECT_EQ(Order(rows_only), (std::vector<std::size_t>{0, 2, 1}));
  EXPECT_EQ(Estimates(rows_only), (std::vector<int64_t>{1000, 10, 5}));
}

// Every edge between the relation that joins and those already joined is a key of its join, so the
// edge that closes a cycle is a second key; with several edges the largest domain divides. Ties go
// to the relation first in FROM.
TEST(JoinOrderTest, ACycleGetsASecondKey) {
  // a(1000), b(100), c(10): a-b on 100 values, b-c and a-c on 10. b and c tie at 1000 after a.
  const auto steps =
      OrderJoins(Relations({1000, 100, 10}),
                 std::vector{Edge(0, 1, 100, 100), Edge(1, 2, 10, 10), Edge(0, 2, 10, 10)});
  EXPECT_EQ(Order(steps), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(Keys(steps), (std::vector<std::vector<std::size_t>>{{}, {0}, {1, 2}}));
  EXPECT_EQ(Estimates(steps), (std::vector<int64_t>{1000, 1000, 1000}));
  // Two keys between the same relations: the larger domain divides (1000 x 50 / 50).
  const auto pair =
      OrderJoins(Relations({1000, 50}), std::vector{Edge(0, 1, 5, 5), Edge(1, 0, 50, 20)});
  EXPECT_EQ(Keys(pair), (std::vector<std::vector<std::size_t>>{{}, {0, 1}}));
  EXPECT_EQ(Estimates(pair), (std::vector<int64_t>{1000, 1000}));
}

// A relation without rows, or a key without a non-NULL value, makes the join empty: estimate 0,
// which joins first, and everything joined after it is 0 too.
TEST(JoinOrderTest, EmptyInputsEstimateZero) {
  const auto empty =
      OrderJoins(Relations({1000, 0, 10}), std::vector{Edge(0, 1), Edge(0, 2, 10, 10)});
  EXPECT_EQ(Order(empty), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(Estimates(empty), (std::vector<int64_t>{1000, 0, 0}));
  const auto all_null =
      OrderJoins(Relations({1000, 50, 10}), std::vector{Edge(0, 1, 0, 50), Edge(0, 2, 10, 10)});
  EXPECT_EQ(Order(all_null), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(Estimates(all_null), (std::vector<int64_t>{1000, 0, 0}));
  // A domain that falls back to an empty relation's rows is empty as well.
  const auto none = OrderJoins(Relations({0, 0}), std::vector{Edge(0, 1)});
  EXPECT_EQ(Order(none), (std::vector<std::size_t>{0, 1}));
  EXPECT_EQ(Estimates(none), (std::vector<int64_t>{0, 0}));
}

// Estimates are computed in 128 bits: they saturate at INT64_MAX instead of overflowing, and are at
// least 1 for inputs with rows. A relation of unknown rows counts as INT64_MAX rows.
TEST(JoinOrderTest, EstimatesSaturateAndStayPositive) {
  constexpr int64_t kHuge = 4'000'000'000'000'000'000;
  const auto huge = OrderJoins(Relations({kHuge, kHuge}), std::vector{Edge(0, 1, 1, 1)});
  EXPECT_EQ(Estimates(huge), (std::vector<int64_t>{kHuge, kLargest}));
  const auto unknown = OrderJoins(Relations({std::nullopt, std::nullopt}), std::vector{Edge(0, 1)});
  EXPECT_EQ(Estimates(unknown), (std::vector<int64_t>{kLargest, kLargest}));
  const auto small = OrderJoins(Relations({10, 10}), std::vector{Edge(0, 1, 1000, 1000)});
  EXPECT_EQ(Estimates(small), (std::vector<int64_t>{10, 1}));
  // A negative count (no valid footer has one) counts as no row.
  const auto negative = OrderJoins(Relations({10, -5}), std::vector{Edge(0, 1, -1, 3)});
  EXPECT_EQ(Estimates(negative), (std::vector<int64_t>{10, 0}));
}

TEST(JoinOrderTest, FirstUnconnected) {
  EXPECT_EQ(FirstUnconnected(0, {}), std::nullopt);
  EXPECT_EQ(FirstUnconnected(1, {}), std::nullopt);
  EXPECT_EQ(FirstUnconnected(2, {}), std::optional<std::size_t>(1));
  EXPECT_EQ(FirstUnconnected(3, std::vector{Edge(0, 1)}), std::optional<std::size_t>(2));
  EXPECT_EQ(FirstUnconnected(3, std::vector{Edge(1, 2)}), std::optional<std::size_t>(1));
  EXPECT_EQ(FirstUnconnected(4, std::vector{Edge(2, 3), Edge(0, 2), Edge(3, 1)}), std::nullopt);
  EXPECT_EQ(FirstUnconnected(4, std::vector{Edge(0, 3), Edge(3, 0), Edge(1, 2)}),
            std::optional<std::size_t>(1));
}

// A dv_id-like key: a range of 2^32 values over 96 non-NULL rows.
FakeTable& Sparse(FakeTable& table, int field) {
  return table.WithParts({50, 50})
      .WithStats(0, field,
                 PartStats{.min = 1000, .max = 4'294'968'296, .null_count = 2, .rows = 50})
      .WithStats(1, field, PartStats{.min = 2000, .max = 3000, .null_count = 2, .rows = 50});
}

// KeyDomain: the integer range when every part has exact statistics, capped at the non-NULL rows
// (a sparse key is not 2^32 values); else the largest distinct-count hint when every part has one;
// else unknown.
TEST(JoinOrderTest, KeyDomains) {
  const auto schema = arrow::schema({arrow::field("k", arrow::int64())});
  FakeTable sparse(schema, 100);
  EXPECT_EQ(KeyDomain(Sparse(sparse, 0), 0), std::optional<int64_t>(96));
  FakeTable dense(schema, 40);
  dense.WithParts({10, 30})
      .WithStats(0, 0, PartStats{.min = 1, .max = 10, .null_count = 0, .rows = 10})
      .WithStats(1, 0, PartStats{.min = 5, .max = 40, .null_count = 20, .rows = 30});
  EXPECT_EQ(KeyDomain(dense, 0), std::optional<int64_t>(20));  // min(40 values, 20 non-NULL rows)
  dense.WithStats(1, 0, PartStats{.min = 5, .max = 40, .null_count = 0, .rows = 30});
  EXPECT_EQ(KeyDomain(dense, 0), std::optional<int64_t>(40));
  // A part of NULLs only adds nothing; NULLs only everywhere is an empty domain.
  FakeTable nulls(schema, 20);
  nulls.WithParts({10, 10})
      .WithStats(0, 0,
                 PartStats{.min = std::nullopt, .max = std::nullopt, .null_count = 10, .rows = 10})
      .WithStats(1, 0, PartStats{.min = -3, .max = 1, .null_count = 3, .rows = 10});
  EXPECT_EQ(KeyDomain(nulls, 0), std::optional<int64_t>(5));
  nulls.WithStats(
      1, 0, PartStats{.min = std::nullopt, .max = std::nullopt, .null_count = 10, .rows = 10});
  EXPECT_EQ(KeyDomain(nulls, 0), std::optional<int64_t>(0));
  // A part without statistics (or with values but no range): the hints, when every part has one.
  FakeTable hinted(schema, 20);
  hinted.WithParts({10, 10})
      .WithStats(0, 0, PartStats{.min = 1, .max = 1000, .null_count = 0, .rows = 10})
      .WithDistinctCount(0, 0, 7)
      .WithDistinctCount(1, 0, 9);
  EXPECT_EQ(KeyDomain(hinted, 0), std::optional<int64_t>(9));
  hinted.WithStats(1, 0, PartStats{.min = std::nullopt, .max = 5, .null_count = 0, .rows = 10});
  EXPECT_EQ(KeyDomain(hinted, 0), std::optional<int64_t>(9));
  FakeTable partial(schema, 20);
  partial.WithParts({10, 10}).WithDistinctCount(1, 0, 4);
  EXPECT_EQ(KeyDomain(partial, 0), std::nullopt);
  EXPECT_EQ(KeyDomain(FakeTable(schema, 5), 0), std::nullopt);  // one part, nothing known
  FakeTable no_parts(schema, 0);
  EXPECT_EQ(KeyDomain(no_parts.WithParts({}), 0), std::optional<int64_t>(0));
}

// KeyDomain over statistics that no Parquet footer gives antb1: a range of 128-bit values (HUGEINT)
// wider than an Int128 saturates, and the non-NULL rows cap it; a part whose min exceeds its max
// has no exact statistics; a part of negative rows adds no value.
TEST(JoinOrderTest, KeyDomainsOfExtremeStatistics) {
  const auto schema = arrow::schema({arrow::field("k", arrow::int64())});
  const Int128 widest = PowerOfTen(38) - 1;  // HUGEINT's 38 digits
  FakeTable wide(schema, 20);
  wide.WithParts({10, 10})
      .WithStats(0, 0, PartStats{.min = -widest, .max = 0, .null_count = 0, .rows = 10})
      .WithStats(1, 0, PartStats{.min = 1, .max = widest, .null_count = 4, .rows = 10});
  EXPECT_EQ(KeyDomain(wide, 0), std::optional<int64_t>(16));
  FakeTable extreme(schema, 10);
  extreme.WithParts({10}).WithStats(
      0, 0, PartStats{.min = kInt128Min, .max = kInt128Max, .null_count = 0, .rows = 10});
  EXPECT_EQ(KeyDomain(extreme, 0), std::optional<int64_t>(10));
  extreme.WithStats(0, 0, PartStats{.min = 0, .max = kInt128Max, .null_count = 0, .rows = 10});
  EXPECT_EQ(KeyDomain(extreme, 0), std::optional<int64_t>(10));

  FakeTable reversed(schema, 20);
  reversed.WithParts({10, 10})
      .WithStats(0, 0, PartStats{.min = 1, .max = 5, .null_count = 0, .rows = 10})
      .WithStats(1, 0, PartStats{.min = 9, .max = 3, .null_count = 0, .rows = 10})
      .WithDistinctCount(0, 0, 4)
      .WithDistinctCount(1, 0, 6);
  EXPECT_EQ(KeyDomain(reversed, 0), std::optional<int64_t>(6));
  FakeTable reversed_alone(schema, 10);
  reversed_alone.WithParts({10}).WithStats(
      0, 0, PartStats{.min = 9, .max = 3, .null_count = 0, .rows = 10});
  EXPECT_EQ(KeyDomain(reversed_alone, 0), std::nullopt);

  FakeTable negative(schema, 10);
  negative.WithParts({10, -3})
      .WithStats(0, 0, PartStats{.min = 1, .max = 50, .null_count = 0, .rows = 10})
      .WithStats(1, 0, PartStats{.min = 1, .max = 50, .null_count = -5, .rows = -3});
  EXPECT_EQ(KeyDomain(negative, 0), std::optional<int64_t>(10));
}

// RelationRows and EstimateRows: a table's exact row count; for a sub-plan, a Scan's table's rows
// through every single-input node, one row for an ungrouped aggregate and a RowCount, and unknown
// for a join.
TEST(JoinOrderTest, RowsOfRelationsAndSubPlans) {
  const auto table = std::make_shared<FakeTable>(testing::AllTypesSchema(), 100);
  ColumnIdSource ids;
  const Binding t =
      Binding::OfTable("t", TableSource{.table = table, .table_name = "t", .span = {}}, ids);
  EXPECT_EQ(RelationRows(t), std::optional<int64_t>(100));
  const Binding unknown = Binding::OfTable(
      "u",
      TableSource{.table = std::make_shared<FakeTable>(testing::AllTypesSchema(), std::nullopt),
                  .table_name = "u",
                  .span = {}},
      ids);
  EXPECT_EQ(RelationRows(unknown), std::nullopt);

  const LogicalNodePtr scan = t.Node();
  const auto node = [](auto n) { return std::make_shared<const LogicalNode>(std::move(n)); };
  for (const LogicalNodePtr& above :
       {node(FilterNode{.input = scan}), node(ComputeNode{.input = scan}),
        node(ProjectNode{.input = scan}), node(GroupAggregateNode{.input = scan}),
        node(SortNode{.input = scan}), node(LimitNode{.input = scan, .limit = 3})}) {
    EXPECT_EQ(EstimateRows(*above), std::optional<int64_t>(100)) << NodeName(*above);
  }
  EXPECT_EQ(EstimateRows(*scan), std::optional<int64_t>(100));
  EXPECT_EQ(EstimateRows(RowCountNode{.table = table}), std::optional<int64_t>(1));
  EXPECT_EQ(EstimateRows(JoinNode{.left = scan, .right = scan}), std::nullopt);
  EXPECT_EQ(EstimateRows(DependentJoinNode{.left = scan, .right = scan}), std::nullopt);

  const ColumnId count = ids.Next();
  const LogicalNodePtr aggregate =
      node(AggregateNode{.input = node(FilterNode{.input = scan}),
                         .aggregates = {AggregateCall{.kind = AggKind::kCountStar, .id = count}}});
  const Binding sub = Binding::OfPlan(
      "sub", aggregate, {BindingColumn{.name = "n", .id = count, .type = LogicalType::kBigInt}});
  EXPECT_EQ(RelationRows(sub), std::optional<int64_t>(1));
}

// The relation limit's worst cases stay cheap: a chain of 256 relations joins from its largest end,
// one key per step, and a clique of 256 relations takes every edge to the joined ones as keys.
TEST(JoinOrderTest, TwoHundredFiftySixRelations) {
  std::vector<std::optional<int64_t>> rows;
  std::vector<JoinEdge> chain;
  for (std::size_t r = 0; r < kMaxRelations; ++r) {
    rows.emplace_back(1000 + static_cast<int64_t>(r));
    if (r > 0) {
      chain.push_back(Edge(r - 1, r));
    }
  }
  EXPECT_EQ(FirstUnconnected(kMaxRelations, chain), std::nullopt);
  const auto steps = OrderJoins(Relations(rows), chain);
  ASSERT_EQ(steps.size(), kMaxRelations);
  for (std::size_t k = 0; k < steps.size(); ++k) {
    EXPECT_EQ(steps[k].relation, kMaxRelations - 1 - k);
    EXPECT_EQ(steps[k].edges.size(), k == 0 ? 0U : 1U);
  }

  std::vector<JoinEdge> clique;
  for (std::size_t a = 0; a < kMaxRelations; ++a) {
    for (std::size_t b = a + 1; b < kMaxRelations; ++b) {
      clique.push_back(Edge(a, b, 1000, 1000));
    }
  }
  const auto all =
      OrderJoins(Relations(std::vector<std::optional<int64_t>>(kMaxRelations, 1000)), clique);
  ASSERT_EQ(all.size(), kMaxRelations);
  for (std::size_t k = 0; k < all.size(); ++k) {
    EXPECT_EQ(all[k].relation, k);
    EXPECT_EQ(all[k].edges.size(), k);
    EXPECT_EQ(all[k].estimate, 1000);
  }
}

// The sides of the steps, in order.
std::vector<BuildSide> Builds(const std::vector<JoinStep>& steps) {
  std::vector<BuildSide> out;
  out.reserve(steps.size());
  for (const JoinStep& step : steps) {
    out.push_back(step.build);
  }
  return out;
}

// Rule 5 (ADR 0026): a step builds on the relation it adds, unless that relation is more than the
// ratio larger than the relations joined so far, when it builds on them instead. The first step is
// the probe and joins nothing, so its side is the default.
TEST(JoinOrderTest, TheBuildSideIsTheAddedRelationUnlessItIsDecisivelyLarger) {
  // 1000 probes; 10 then 900 join it. One edge of unknown domain, so each estimate is the product
  // over the larger side's rows: joining 10 leaves 1000, and 900 against that is far more than a
  // quarter of it, so the second step builds on the relations joined so far instead.
  const std::vector<JoinEdge> chain = {Edge(0, 1, 1000, 1000), Edge(0, 2, 1000, 1000)};
  const auto steps = OrderJoins(Relations({1000, 10, 900}), chain);
  EXPECT_EQ(Order(steps), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(Estimates(steps), (std::vector<int64_t>{1000, 10, 9}));
  // Step 1 adds 10 rows to 1000: the added relation stays the build side. Step 2 adds 900 rows to
  // an estimate of 10, which is 90x and so decisive: it builds on the left instead.
  EXPECT_EQ(Builds(steps),
            (std::vector<BuildSide>{BuildSide::kRight, BuildSide::kRight, BuildSide::kLeft}));
}

// A near-tie keeps the plan as it was: the ratio exists so that an estimate that is wrong by a
// little cannot move a join that is already right.
//
// Only a third relation can put the larger side on the build: with two, rule 1 makes the larger of
// them the probe, so the relation each step adds is already the smaller one. Here 1000 probes, a
// relation of 10 rows joins it down to an estimate of 10, and the last relation is compared against
// that 10 -- which is how a join of a shrinking pipeline to a whole table arises in a real query.
TEST(JoinOrderTest, TheBuildSideKeepsTheAddedRelationOnANearTie) {
  const std::vector<JoinEdge> edges = {Edge(0, 1, 1000, 1000), Edge(0, 2, 1000, 1000)};
  const auto last = [&edges](int64_t rows, int64_t ratio = kBuildSideRatio) {
    return Builds(OrderJoins(Relations({1000, 10, rows}), edges, ratio)).back();
  };
  EXPECT_EQ(last(40), BuildSide::kRight);  // 40 against 10 is 4.0x: not more than the ratio
  EXPECT_EQ(last(41), BuildSide::kLeft);   // 4.1x is
  // At a ratio of 100 hundredths the smaller side always builds, which is ADR 0022's own rule.
  EXPECT_EQ(last(11, 100), BuildSide::kLeft);
  EXPECT_EQ(last(10, 100), BuildSide::kRight);  // equal rows are no reason to move the build
  // A larger ratio trusts the estimates less: at 10x, 4.1x is no longer decisive.
  EXPECT_EQ(last(41, 1000), BuildSide::kRight);
}

// Unknown rows count as the largest for the order (rule 1), but must never decide a build side:
// building on a side that turns out to be large is the mistake the rule exists to avoid.
TEST(JoinOrderTest, AnUnknownOrEmptyEstimateKeepsTheAddedRelationAsTheBuildSide) {
  const std::vector<JoinEdge> edges = {Edge(0, 1, 1000, 1000), Edge(0, 2, 1000, 1000)};
  const auto last = [&edges](std::optional<int64_t> rows) {
    return Builds(OrderJoins(Relations({1000, 10, rows}), edges)).back();
  };
  // Unknown rows on the side that would build: it counts as the largest for the order, but a side
  // that may turn out to be large is exactly what must not be chosen to build on.
  EXPECT_EQ(last(std::nullopt), BuildSide::kRight);
  // An estimate of no row at all is just as untrustworthy, and an empty build is free either way.
  EXPECT_EQ(Builds(OrderJoins(Relations({1000, 0, 1000}), edges)).back(), BuildSide::kRight);
}

TEST(JoinOrderDeathTest, TheBuildSideRatioIsAtLeastOne) {
  EXPECT_DEATH((void)OrderJoins(Relations({1, 2}), std::vector{Edge(0, 1)}, 99),
               "build_side_ratio >= 100");
}

// The build-side rule itself (ADR 0026), which both the join order and the unnesting pass call so
// that they cannot drift. The left input is the side already joined, the right the side added.
TEST(JoinOrderTest, ChooseBuildSideFlipsOnlyOnADecisiveDifference) {
  EXPECT_EQ(ChooseBuildSide(100, 399), BuildSide::kRight);  // 3.99x: not more than the default 4
  EXPECT_EQ(ChooseBuildSide(100, 401), BuildSide::kLeft);
  EXPECT_EQ(ChooseBuildSide(100, 100), BuildSide::kRight);
  EXPECT_EQ(ChooseBuildSide(400, 100), BuildSide::kRight);  // a smaller right never moves a build
  // At 100 hundredths the smaller side always builds, which is ADR 0022's own rule; a larger ratio
  // trusts the estimates less.
  EXPECT_EQ(ChooseBuildSide(100, 101, 100), BuildSide::kLeft);
  EXPECT_EQ(ChooseBuildSide(100, 100, 100), BuildSide::kRight);
  EXPECT_EQ(ChooseBuildSide(100, 401, 1000), BuildSide::kRight);
  // Unknown on either side, and an estimate of no row, keep the build where it was.
  EXPECT_EQ(ChooseBuildSide(std::nullopt, 1'000'000), BuildSide::kRight);
  EXPECT_EQ(ChooseBuildSide(100, std::nullopt), BuildSide::kRight);
  EXPECT_EQ(ChooseBuildSide(0, 1'000'000), BuildSide::kRight);
  EXPECT_EQ(ChooseBuildSide(100, -1), BuildSide::kRight);
  // 128-bit arithmetic: the largest counts must not overflow into the wrong answer.
  EXPECT_EQ(ChooseBuildSide(1, kLargest), BuildSide::kLeft);
  EXPECT_EQ(ChooseBuildSide(kLargest, kLargest), BuildSide::kRight);
  EXPECT_EQ(ChooseBuildSide(kLargest, 1), BuildSide::kRight);
}

TEST(JoinOrderDeathTest, ChooseBuildSideNeedsARatioOfAtLeastOne) {
  EXPECT_DEATH((void)ChooseBuildSide(1, 2, 99), "build_side_ratio >= 100");
}

// A join's rows are estimated from the estimate the join order left on the node: the domains rule 3
// divides by are not reachable from a plan node, so nothing downstream could recompute it, and a
// join whose estimate was never set stays unknown.
TEST(JoinOrderTest, EstimateRowsReadsAJoinsOwnEstimate) {
  const auto schema = arrow::schema({arrow::field("k", arrow::int64())});
  const auto scan = [&schema](int64_t rows) {
    return std::make_shared<const LogicalNode>(
        ScanNode{.table = std::make_shared<FakeTable>(schema, rows),
                 .table_name = "t",
                 .fields = {0},
                 .ids = {ColumnId{1}},
                 .span = {}});
  };
  JoinNode join{.kind = JoinKind::kInner, .left = scan(1000), .right = scan(10)};
  EXPECT_EQ(EstimateRows(LogicalNode{join}), std::nullopt);  // nothing estimated it
  join.estimate = 250;
  EXPECT_EQ(EstimateRows(LogicalNode{join}), std::optional<int64_t>(250));
  // A single-input node above it passes the estimate through as an upper bound, which is what lets
  // a pass above a subquery's aggregate see the join below it.
  const auto over = std::make_shared<const LogicalNode>(join);
  EXPECT_EQ(EstimateRows(LogicalNode{GroupAggregateNode{.input = over}}),
            std::optional<int64_t>(250));
}

// The footer statistics one column's estimates read (ADR 0026): exact only when every part has
// them, since a part without them could hold any value.
TEST(JoinOrderTest, ColumnRanges) {
  // Two fields: `k` has statistics in every part, `d` is a DECIMAL and so has none anywhere
  // (ADR 0021), which is the case that matters -- an unrelated DECIMAL column in the same scan.
  const auto schema = arrow::schema(
      {arrow::field("k", arrow::int64()), arrow::field("d", arrow::decimal128(15, 2))});
  FakeTable table(schema, 40);
  table.WithParts({10, 30})
      .WithStats(0, 0, PartStats{.min = 1, .max = 10, .null_count = 0, .rows = 10})
      .WithStats(1, 0, PartStats{.min = 5, .max = 40, .null_count = 6, .rows = 30});
  const ColumnRange range = ColumnRangeOf(table, 0);
  EXPECT_TRUE(range.exact);
  EXPECT_EQ(range.min, std::optional<Int128>(1));
  EXPECT_EQ(range.max, std::optional<Int128>(40));
  EXPECT_EQ(range.non_null, 34);  // 10 + (30 - 6)
  // A column whose type has no statistics at all (DECIMAL, VARCHAR: ADR 0021) is not exact, so
  // every predicate on it takes a default factor.
  EXPECT_FALSE(ColumnRangeOf(table, 1).exact);
  EXPECT_EQ(ColumnRangeOf(table, 1).non_null, 0);
  // Nor is a column one of whose parts has none.
  FakeTable partial(schema, 40);
  partial.WithParts({10, 30}).WithStats(
      0, 0, PartStats{.min = 1, .max = 10, .null_count = 0, .rows = 10});
  EXPECT_FALSE(ColumnRangeOf(partial, 0).exact);
  // A part of NULLs only contributes nothing and keeps the column exact.
  FakeTable nulls(schema, 20);
  nulls.WithParts({10, 10})
      .WithStats(0, 0,
                 PartStats{.min = std::nullopt, .max = std::nullopt, .null_count = 10, .rows = 10})
      .WithStats(1, 0, PartStats{.min = 3, .max = 7, .null_count = 0, .rows = 10});
  const ColumnRange only_nulls = ColumnRangeOf(nulls, 0);
  EXPECT_TRUE(only_nulls.exact);
  EXPECT_EQ(only_nulls.min, std::optional<Int128>(3));
  EXPECT_EQ(only_nulls.non_null, 10);
  // A table without parts has nothing to read.
  EXPECT_FALSE(ColumnRangeOf(FakeTable(schema, 40).WithParts({}), 0).exact);
}

Predicate Compare(CompareOp op, Int128 value) {
  return Predicate{.kind = Predicate::Kind::kCompare,
                   .column = BoundColumn{},
                   .op = op,
                   .constant = Constant{.type = LogicalType::kBigInt, .value = value}};
}

// A column of 100 rows whose values run 1 to 100, one row each.
ColumnRange Hundred() { return ColumnRange{.min = 1, .max = 100, .non_null = 100, .exact = true}; }

// PredicateRows over a range the footers give exactly: the part of the range the comparison admits.
TEST(JoinOrderTest, PredicateRowsFromAnExactRange) {
  const ColumnRange range = Hundred();
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kLt, 51), range, 100), 50);  // 1..50
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kLe, 50), range, 100), 50);
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kGt, 50), range, 100), 50);  // 51..100
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kGe, 51), range, 100), 50);
  // A constant outside the range admits all of it or none of it, and never more rows than there
  // are.
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kLt, 1000), range, 100), 100);
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kGt, 1000), range, 100), 0);
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kLt, -5), range, 100), 0);
  // Equality divides by the domain; inequality keeps the rest.
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kEq, 7), range, 100), 1);
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kNe, 7), range, 100), 99);
  // An estimate never reaches no row from a comparison that admits part of the range: a relation
  // estimated at nothing would be ordered first and stop every later estimate.
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kEq, 7), range, 1000), 1);
  EXPECT_EQ(PredicateRows(0, Compare(CompareOp::kEq, 7), range, 100), 0);
}

// A column whose footer range excludes the constant decides the predicate outright: an equality
// matches no row, and its complement excludes none. Without this a column of one distinct value
// estimates `col <> other` at almost no rows, which orders that relation first and then builds on
// the wrong side of its joins -- a plan strictly worse than the unfiltered footer count gave.
TEST(JoinOrderTest, PredicateRowsUsesTheRangeForEqualityAndItsComplement) {
  // 100 rows whose only value is 1, as a constant column's footers report it.
  const ColumnRange one{.min = 1, .max = 1, .non_null = 100, .exact = true};
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kEq, 7), one, 1), 0);    // cannot match
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kNe, 7), one, 1), 100);  // excludes nothing
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kEq, 1), one, 1), 100);  // the only value
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kNe, 1), one, 1), 1);    // floored, not 0
  // The complement never reaches no row even where the match saturates, because a relation
  // estimated at no row is ordered first and makes every later estimate 0 as well.
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kNe, 50), Hundred(), 1), 1);
  Predicate in{.kind = Predicate::Kind::kIn, .column = BoundColumn{}};
  const auto value = [](Int128 v) { return Constant{.type = LogicalType::kBigInt, .value = v}; };
  in.values = {value(7), value(8)};
  EXPECT_EQ(PredicateRows(100, in, one, 1), 0);  // no listed value is in the range
  in.kind = Predicate::Kind::kNotIn;
  EXPECT_EQ(PredicateRows(100, in, one, 1), 100);
  // A value inside the range counts; one outside does not.
  in.values = {value(1), value(8)};
  EXPECT_EQ(PredicateRows(100, in, one, 1), 1);  // NOT IN (1, 8): only 1 can match
  in.kind = Predicate::Kind::kIn;
  EXPECT_EQ(PredicateRows(100, in, one, 1), 100);  // IN (1, 8): 1 matches every row
  // Without an exact range no value can be excluded, so the factors apply as before.
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kNe, 7), ColumnRange{}, std::nullopt), 90);
}

// The whole point of the complement floor, at the level the orderer sees: a filter that keeps every
// row must not make its relation look empty, because an empty estimate is ordered first and then
// every later estimate and build side in the block follows from it.
TEST(JoinOrderTest, AKeepEverythingFilterDoesNotCollapseTheOrder) {
  const ColumnRange one{.min = 1, .max = 1, .non_null = 100'000, .exact = true};
  const int64_t kept = PredicateRows(100'000, Compare(CompareOp::kNe, 0), one, 1);
  EXPECT_EQ(kept, 100'000);
  // 100,000 rows against a 100-row dimension and a 7-row one: the big relation still probes, and
  // each step still builds on the relation it adds.
  const auto steps =
      OrderJoins(Relations({kept, 100, 7}), std::vector{Edge(0, 1, 100, 100), Edge(0, 2, 7, 7)});
  EXPECT_EQ(Order(steps), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(Builds(steps),
            (std::vector<BuildSide>{BuildSide::kRight, BuildSide::kRight, BuildSide::kRight}));
}

// Without statistics every shape takes its documented default factor, and the shapes the footers
// never cover take one always.
TEST(JoinOrderTest, PredicateRowsFromTheDefaultFactors) {
  const ColumnRange none;
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kEq, 7), none, std::nullopt), 10);  // 1 in 10
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kNe, 7), none, std::nullopt), 90);
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kLt, 7), none, std::nullopt), 33);
  // A domain is enough for equality even with no range to compare against.
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kEq, 7), none, 4), 25);
  Predicate in{.kind = Predicate::Kind::kIn, .column = BoundColumn{}};
  in.values = {Constant{}, Constant{}, Constant{}};
  EXPECT_EQ(PredicateRows(100, in, none, 10), 30);  // 3 of 10 values
  EXPECT_EQ(PredicateRows(100, in, none, 2), 100);  // never more rows than there are
  in.kind = Predicate::Kind::kNotIn;
  EXPECT_EQ(PredicateRows(100, in, none, 10), 70);
  // A pattern match, two columns compared and a computed BOOLEAN have no statistics to read.
  EXPECT_EQ(PredicateRows(100, Predicate{.kind = Predicate::Kind::kLike, .column = BoundColumn{}},
                          Hundred(), 100),
            25);
  EXPECT_EQ(
      PredicateRows(100, Predicate{.kind = Predicate::Kind::kNotLike, .column = BoundColumn{}},
                    Hundred(), 100),
      75);  // the complement of LIKE's quarter
  EXPECT_EQ(PredicateRows(100, Predicate{.kind = Predicate::Kind::kCompareColumns}, Hundred(), 100),
            50);
  EXPECT_EQ(PredicateRows(100, Predicate{.kind = Predicate::Kind::kIsTrue, .column = BoundColumn{}},
                          Hundred(), 100),
            50);
  // IS NOT NULL reads the null count when it is exact, and a default factor otherwise.
  const Predicate not_null{.kind = Predicate::Kind::kIsNotNull, .column = BoundColumn{}};
  EXPECT_EQ(PredicateRows(100, not_null, ColumnRange{.non_null = 60, .exact = true}, 100), 60);
  EXPECT_EQ(PredicateRows(100, not_null, none, std::nullopt), 90);
  // A folded conjunct that is true for no row keeps none, whatever the statistics say.
  EXPECT_EQ(PredicateRows(100, Predicate{.kind = Predicate::Kind::kFalse}, Hundred(), 100), 0);
}

TEST(JoinOrderDeathTest, EdgesJoinTwoRelationsOfTheBlock) {
  EXPECT_DEATH((void)OrderJoins(Relations({1, 2}), std::vector{Edge(1, 1)}), "edge.a != edge.b");
  EXPECT_DEATH((void)OrderJoins(Relations({1, 2}), std::vector{Edge(0, 2)}), "edge.b < relations");
  EXPECT_DEATH((void)FirstUnconnected(2, std::vector{Edge(3, 0)}), "edge.a < relations");
}

// OrderJoins needs connected relations: the binder checks connectivity first.
TEST(JoinOrderDeathTest, TheRelationsMustBeConnected) {
  EXPECT_DEATH((void)OrderJoins(Relations({1, 2, 3}), std::vector{Edge(1, 2)}), "next.has_value");
}

}  // namespace
}  // namespace antb1::plan
