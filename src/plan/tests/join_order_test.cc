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
