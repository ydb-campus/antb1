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
// ADR 0022's rule, which the search replaced. The cases below that pin rules 1 to 5 ask for it by
// name, because it is still what orders a block of more relations than the search is bounded for,
// and what `--join-order greedy` selects.
std::vector<JoinStep> GreedyOrder(std::span<const JoinRelation> relations,
                                  std::span<const JoinEdge> edges,
                                  int64_t ratio = kBuildSideRatio) {
  return OrderJoins(relations, edges, ratio, JoinOrderSearch::kGreedy);
}

// The cheapest an order of these relations can be, by a plain search over every subset and every
// split of it. The reference the search must match: a lost or duplicated connected pair would just
// make it choose a worse plan, which nothing else would notice.
int64_t OptimalCost(std::span<const JoinRelation> relations, std::span<const JoinEdge> edges) {
  const std::size_t n = relations.size();
  const auto all = static_cast<std::uint32_t>((std::uint64_t{1} << n) - 1);
  std::vector<int64_t> best(std::size_t{1} << n, -1);
  const auto set_of = [n](std::uint32_t mask) {
    RelationSet out;
    for (std::size_t r = 0; r < n; ++r) {
      if (((mask >> r) & 1U) != 0) {
        out.set(r);
      }
    }
    return out;
  };
  const auto joined = [&edges](std::uint32_t a, std::uint32_t b) {
    return std::ranges::any_of(edges, [a, b](const JoinEdge& edge) {
      const bool a_to_b = ((a >> edge.a) & 1U) != 0 && ((b >> edge.b) & 1U) != 0;
      const bool b_to_a = ((a >> edge.b) & 1U) != 0 && ((b >> edge.a) & 1U) != 0;
      return a_to_b || b_to_a;
    });
  };
  for (std::size_t r = 0; r < n; ++r) {
    best[std::size_t{1} << r] = 0;  // a relation costs nothing: its scan is the same in any order
  }
  for (std::uint32_t mask = 1; mask <= all; ++mask) {
    if (std::popcount(mask) < 2) {
      continue;
    }
    // Every proper non-empty subset is numerically smaller, so one ascending pass suffices.
    for (std::uint32_t sub = (mask - 1) & mask; sub != 0; sub = (sub - 1) & mask) {
      const std::uint32_t other = mask ^ sub;
      if (best[sub] < 0 || best[other] < 0 || !joined(sub, other)) {
        continue;
      }
      const int64_t cost = best[sub] + best[other] + SubsetRows(relations, edges, set_of(mask)) +
                           std::min(SubsetRows(relations, edges, set_of(sub)),
                                    SubsetRows(relations, edges, set_of(other)));
      if (best[mask] < 0 || cost < best[mask]) {
        best[mask] = cost;
      }
    }
  }
  return best[all];
}

// The relations in the order they join, reconstructed from a left-deep step list: the probe, which
// is the first step's left input, then each step's right input. A step list that is not left-deep
// has no such sequence, so this also pins left-deepness -- which is what the greedy order produces.
std::vector<std::size_t> Order(const std::vector<JoinStep>& steps) {
  std::vector<std::size_t> out;
  if (steps.empty()) {
    return out;
  }
  EXPECT_FALSE(steps.front().left.step) << "the first step's left input is the probe";
  out.push_back(steps.front().left.index);
  for (std::size_t k = 0; k < steps.size(); ++k) {
    EXPECT_FALSE(steps[k].right.step) << "step " << k << " adds a relation";
    EXPECT_EQ(steps[k].left.step, k > 0) << "step " << k;
    if (k > 0) {
      EXPECT_EQ(steps[k].left.index, k - 1) << "step " << k << " joins the one before it";
    }
    out.push_back(steps[k].right.index);
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
// FROM order on a tie, and a relation of unknown rows counts as the largest. The probe is the left
// input of the first step rather than a step of its own: a step is a join, and there is one fewer
// of them than there are relations.
TEST(JoinOrderTest, TheProbeIsTheLargestRelation) {
  const std::vector<JoinEdge> chain = {Edge(0, 1), Edge(1, 2), Edge(2, 3)};
  const auto steps = GreedyOrder(Relations({10, 30, 20, 30}), chain);
  ASSERT_EQ(steps.size(), 3U);
  EXPECT_FALSE(steps.front().left.step);
  EXPECT_EQ(steps.front().left.index, 1U);
  EXPECT_FALSE(steps.front().edges.empty()) << "a join always has a key";
  const auto unknown = GreedyOrder(Relations({10, std::nullopt, 50, std::nullopt}), chain);
  ASSERT_EQ(unknown.size(), 3U);
  EXPECT_EQ(unknown.front().left.index, 1U);
  // One relation is its own tree, so there is nothing to join and no step at all.
  EXPECT_TRUE(GreedyOrder(Relations({7}), {}).empty());
  EXPECT_TRUE(GreedyOrder({}, {}).empty());
}

// The search must choose an order of the lowest cost there is. A plain search over every subset
// and every split is the reference: nothing else would notice a connected pair the enumeration
// loses, because a lost pair only makes the plan quietly worse.
TEST(JoinOrderTest, TheSearchChoosesAnOrderOfTheLowestCost) {
  struct Case {
    std::vector<std::optional<int64_t>> rows;
    std::vector<JoinEdge> edges;
    std::string_view what;
  };
  // The last is Q5's shape: a fact table, two dimensions behind it, and a cycle that closes on the
  // relation joined last, which is what makes its two-key join selective.
  for (const Case& c : std::vector<Case>{
           {.rows = {100, 10}, .edges = {Edge(0, 1, 10, 10)}, .what = "one join"},
           {.rows = {1000, 100, 10},
            .edges = {Edge(0, 1, 100, 100), Edge(1, 2, 10, 10)},
            .what = "a chain"},
           {.rows = {1000, 100, 10},
            .edges = {Edge(0, 1, 100, 100), Edge(1, 2, 10, 10), Edge(0, 2, 10, 10)},
            .what = "a triangle"},
           {.rows = {1'000'000, 10, 10, 10},
            .edges = {Edge(0, 1, 10, 10), Edge(0, 2, 10, 10), Edge(0, 3, 10, 10)},
            .what = "a star"},
           {.rows = {100, 200, 300, 400, 500},
            .edges = {Edge(0, 1, 50, 50), Edge(1, 2, 60, 60), Edge(2, 3, 70, 70),
                      Edge(3, 4, 80, 80), Edge(0, 4, 90, 90)},
            .what = "a cycle of five"},
           {.rows = {1'500'000, 2'275'919, 59'986'052, 100'000, 25, 1},
            .edges = {Edge(0, 1, 1'500'000, 1'500'000), Edge(1, 2, 15'000'000, 15'000'000),
                      Edge(2, 3, 100'000, 100'000), Edge(0, 3, 25, 25), Edge(3, 4, 25, 25),
                      Edge(4, 5, 5, 5)},
            .what = "a fact table, its dimensions, and a cycle through the last of them"}}) {
    SCOPED_TRACE(c.what);
    const std::vector<JoinRelation> relations = Relations(c.rows);
    const auto steps = OrderJoins(relations, c.edges);
    ASSERT_EQ(steps.size(), c.rows.size() - 1);
    EXPECT_EQ(OrderCost(relations, c.edges, steps), OptimalCost(relations, c.edges));
    // And it is at least as good as the rule it replaced, which is the point of having it.
    EXPECT_LE(OrderCost(relations, c.edges, steps),
              OrderCost(relations, c.edges, GreedyOrder(relations, c.edges)));
  }
}

// The rows of a subset, which is what the search compares. Within a pair of relations the largest
// domain of their edges wins; across pairs the divisors multiply.
TEST(JoinOrderTest, SubsetRowsDividesOncePerPair) {
  const auto set_of = [](std::initializer_list<std::size_t> members) {
    RelationSet out;
    for (const std::size_t r : members) {
      out.set(r);
    }
    return out;
  };
  const std::vector<JoinRelation> relations = Relations({1000, 100, 10});
  // One pair, one edge: 1000 x 100 / 100.
  EXPECT_EQ(SubsetRows(relations, std::vector{Edge(0, 1, 100, 100)}, set_of({0, 1})), 1000);
  // A subset of one relation is its own rows, and the empty subset is one row (nothing joined).
  EXPECT_EQ(SubsetRows(relations, std::vector{Edge(0, 1, 100, 100)}, set_of({1})), 100);
  EXPECT_EQ(SubsetRows(relations, std::vector{Edge(0, 1, 100, 100)}, set_of({})), 1);
  // A relation an edge does not reach multiplies in undivided: 1000 x 100 / 100 x 10.
  EXPECT_EQ(SubsetRows(relations, std::vector{Edge(0, 1, 100, 100)}, set_of({0, 1, 2})), 10000);
  // Two edges between the *same* pair are one composite key: the larger domain divides, not their
  // product, so this is 1000 x 100 / 100 rather than / (100 x 5).
  EXPECT_EQ(
      SubsetRows(relations, std::vector{Edge(0, 1, 100, 100), Edge(0, 1, 5, 5)}, set_of({0, 1})),
      1000);
  // Two edges to *different* relations do narrow twice: 1000 x 100 x 10 / (100 x 10).
  EXPECT_EQ(SubsetRows(relations, std::vector{Edge(0, 1, 100, 100), Edge(0, 2, 10, 10)},
                       set_of({0, 1, 2})),
            1000);
  // A key with no value at all empties the join, whether or not the other side has a domain: one
  // side having no value is the whole of the answer, and the unknown side cannot put rows back.
  EXPECT_EQ(SubsetRows(relations, std::vector{Edge(0, 1, 0, 100)}, set_of({0, 1})), 0);
  EXPECT_EQ(SubsetRows(relations, std::vector{Edge(0, 1, std::nullopt, 0)}, set_of({0, 1})), 0);
  EXPECT_EQ(SubsetRows(relations, std::vector{Edge(0, 1, 0, std::nullopt)}, set_of({0, 1})), 0);
  // An edge neither side of which has a domain divides by nothing, so the join reads as the product
  // of its inputs. A domain divides, so standing in the largest domain available -- the relation's
  // rows -- would make the join read as the FEWEST rows it could have rather than the most, which
  // is the one direction this estimate may not err in.
  EXPECT_EQ(SubsetRows(relations, std::vector{Edge(0, 1)}, set_of({0, 1})), 1000 * 100);
  // One side's domain is enough, and it divides alone: the unknown side cannot be fewer than the
  // known one, so the known one is the whole of what the pair's selectivity is known to be. This is
  // the case a derived table always produces, since only a table carries a domain.
  EXPECT_EQ(SubsetRows(relations, std::vector{Edge(0, 1, std::nullopt, 25)}, set_of({0, 1})),
            1000 * 100 / 25);
  EXPECT_EQ(SubsetRows(relations, std::vector{Edge(0, 1, 25, std::nullopt)}, set_of({0, 1})),
            1000 * 100 / 25);
  // A relation of unknown rows still counts as the largest, saturating rather than wrapping.
  EXPECT_EQ(
      SubsetRows(Relations({std::nullopt, std::nullopt}), std::vector{Edge(0, 1)}, set_of({0, 1})),
      kLargest);
}

// Two tight pairs joined by a weak edge: joining each pair first and then the two results carries
// far fewer rows than any chain, so the plan the search chooses is not left-deep at all. The Order
// helper above would reject it, which is the point of checking the shape directly.
TEST(JoinOrderTest, TheSearchChoosesATreeThatIsNotAChain) {
  const std::vector<JoinRelation> relations = Relations({1000, 1000, 1000, 1000});
  // 0-1 and 2-3 narrow to 1000 rows each; 1-2 narrows to nothing, so whichever chain is taken
  // carries a million rows twice, where the two pairs first carry it once.
  const std::vector<JoinEdge> edges = {Edge(0, 1, 1000, 1000), Edge(2, 3, 1000, 1000),
                                       Edge(1, 2, 1, 1)};
  const auto steps = OrderJoins(relations, edges);
  ASSERT_EQ(steps.size(), 3U);
  EXPECT_EQ(OrderCost(relations, edges, steps), OptimalCost(relations, edges));
  EXPECT_LT(OrderCost(relations, edges, steps),
            OrderCost(relations, edges, GreedyOrder(relations, edges)));
  // The root joins two results, which no chain does.
  EXPECT_TRUE(steps.back().left.step) << "the root's left input is a join";
  EXPECT_TRUE(steps.back().right.step) << "the root's right input is a join";
}

// At the cap the search runs; one relation more and the greedy rule orders the block instead. The
// graph is the pair-of-pairs above with a chain hung off it, so the two choose differently: the
// search finds the bushy root, and the greedy walk cannot.
TEST(JoinOrderTest, TheSearchGivesWayToTheGreedyRuleAboveTheCap) {
  const auto graph = [](std::size_t n) {
    std::vector<JoinEdge> edges = {Edge(0, 1, 1000, 1000), Edge(2, 3, 1000, 1000),
                                   Edge(1, 2, 1, 1)};
    for (std::size_t r = 4; r < n; ++r) {
      edges.push_back(Edge(r - 1, r, 1000, 1000));
    }
    return edges;
  };
  const auto bushy = [](const std::vector<JoinStep>& steps) {
    return std::ranges::any_of(
        steps, [](const JoinStep& step) { return step.left.step && step.right.step; });
  };
  const std::vector<JoinRelation> at_cap =
      Relations(std::vector<std::optional<int64_t>>(kSearchRelations, 1000));
  const std::vector<JoinStep> searched = OrderJoins(at_cap, graph(kSearchRelations));
  ASSERT_EQ(searched.size(), kSearchRelations - 1);
  EXPECT_TRUE(bushy(searched)) << "at the cap the search runs";

  const std::vector<JoinRelation> over_cap =
      Relations(std::vector<std::optional<int64_t>>(kSearchRelations + 1, 1000));
  const std::vector<JoinEdge> over = graph(kSearchRelations + 1);
  const std::vector<JoinStep> fell_back = OrderJoins(over_cap, over);
  ASSERT_EQ(fell_back.size(), kSearchRelations);
  EXPECT_FALSE(bushy(fell_back)) << "one relation more and the greedy rule orders it";
  EXPECT_EQ(Order(fell_back), Order(GreedyOrder(over_cap, over)));
}

// Rule 2: of the relations joined to those already joined, the smallest estimate |L| x |R| /
// max(domain) comes next, whatever its own size; every join builds on the relation it adds.
TEST(JoinOrderTest, TheSmallestEstimateJoinsNext) {
  // f(1000) joins a(100) on a key of 1000 values (f's) against 100, and b(10) on 10 values.
  const auto steps = GreedyOrder(Relations({1000, 100, 10}),
                                 std::vector{Edge(0, 1, 1000, 100), Edge(0, 2, 10, 10)});
  EXPECT_EQ(Order(steps), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(Estimates(steps), (std::vector<int64_t>{100, 100}));
  EXPECT_EQ(Keys(steps), (std::vector<std::vector<std::size_t>>{{0}, {1}}));
  for (const JoinStep& step : steps) {
    EXPECT_EQ(step.build, BuildSide::kRight);
  }
  // A relation without an edge to the joined ones waits for one: c joins only through b.
  const auto through =
      GreedyOrder(Relations({1000, 5, 100}), std::vector{Edge(1, 2), Edge(0, 2, 100, 100)});
  EXPECT_EQ(Order(through), (std::vector<std::size_t>{0, 2, 1}));
  EXPECT_EQ(Keys(through), (std::vector<std::vector<std::size_t>>{{1}, {0}}));
}

// The domains keep a many-to-many edge (few values on both sides) out of the order while a key edge
// is available (ADR 0022): f(1000) has the key edge to x(500), 500 values on both sides, and the
// many-to-many edge to m(10), 2 values on both sides, whose join gives 5000 rows. With the domains
// x comes first; the control, row counts alone (unknown domains), takes m first.
TEST(JoinOrderTest, AManyToManyEdgeWaitsWhileAKeyEdgeIsAvailable) {
  const std::vector<JoinRelation> relations = Relations({1000, 500, 10});
  const auto with_domains =
      GreedyOrder(relations, std::vector{Edge(0, 1, 500, 500), Edge(0, 2, 2, 2)});
  EXPECT_EQ(Order(with_domains), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(Estimates(with_domains), (std::vector<int64_t>{1000, 5000}));
  // The control still takes m first, on row counts alone. What it no longer does is believe that
  // doing so costs 10 rows: with no domain on either edge the steps read as the products of their
  // inputs, 10,000 and 5,000,000, which over-states the true 5,000 twofold where the old reading
  // under-stated it five-hundredfold. An estimate that reads a many-to-many join as a key lookup
  // is how the search comes to pick one.
  const auto rows_only = GreedyOrder(relations, std::vector{Edge(0, 1), Edge(0, 2)});
  EXPECT_EQ(Order(rows_only), (std::vector<std::size_t>{0, 2, 1}));
  EXPECT_EQ(Estimates(rows_only), (std::vector<int64_t>{10'000, 5'000'000}));
}

// Every edge between the relation that joins and those already joined is a key of its join, so the
// edge that closes a cycle is a second key; with several edges the largest domain divides. Ties go
// to the relation first in FROM.
TEST(JoinOrderTest, ACycleGetsASecondKey) {
  // a(1000), b(100), c(10): a-b on 100 values, b-c and a-c on 10. b and c tie at 1000 after a.
  const auto steps =
      GreedyOrder(Relations({1000, 100, 10}),
                  std::vector{Edge(0, 1, 100, 100), Edge(1, 2, 10, 10), Edge(0, 2, 10, 10)});
  EXPECT_EQ(Order(steps), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(Keys(steps), (std::vector<std::vector<std::size_t>>{{0}, {1, 2}}));
  EXPECT_EQ(Estimates(steps), (std::vector<int64_t>{1000, 1000}));
  // Two keys between the same relations: the larger domain divides (1000 x 50 / 50).
  const auto pair =
      GreedyOrder(Relations({1000, 50}), std::vector{Edge(0, 1, 5, 5), Edge(1, 0, 50, 20)});
  EXPECT_EQ(Keys(pair), (std::vector<std::vector<std::size_t>>{{0, 1}}));
  EXPECT_EQ(Estimates(pair), (std::vector<int64_t>{1000}));
}

// A relation without rows, or a key without a non-NULL value, makes the join empty: estimate 0,
// which joins first, and everything joined after it is 0 too.
TEST(JoinOrderTest, EmptyInputsEstimateZero) {
  const auto empty =
      GreedyOrder(Relations({1000, 0, 10}), std::vector{Edge(0, 1), Edge(0, 2, 10, 10)});
  EXPECT_EQ(Order(empty), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(Estimates(empty), (std::vector<int64_t>{0, 0}));
  const auto all_null =
      GreedyOrder(Relations({1000, 50, 10}), std::vector{Edge(0, 1, 0, 50), Edge(0, 2, 10, 10)});
  EXPECT_EQ(Order(all_null), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(Estimates(all_null), (std::vector<int64_t>{0, 0}));
  // A domain that falls back to an empty relation's rows is empty as well.
  const auto none = GreedyOrder(Relations({0, 0}), std::vector{Edge(0, 1)});
  EXPECT_EQ(Order(none), (std::vector<std::size_t>{0, 1}));
  EXPECT_EQ(Estimates(none), (std::vector<int64_t>{0}));
}

// Estimates are computed in 128 bits: they saturate at INT64_MAX instead of overflowing, and are at
// least 1 for inputs with rows. A relation of unknown rows counts as INT64_MAX rows.
TEST(JoinOrderTest, EstimatesSaturateAndStayPositive) {
  constexpr int64_t kHuge = 4'000'000'000'000'000'000;
  const auto huge = GreedyOrder(Relations({kHuge, kHuge}), std::vector{Edge(0, 1, 1, 1)});
  EXPECT_EQ(Estimates(huge), (std::vector<int64_t>{kLargest}));
  const auto unknown =
      GreedyOrder(Relations({std::nullopt, std::nullopt}), std::vector{Edge(0, 1)});
  EXPECT_EQ(Estimates(unknown), (std::vector<int64_t>{kLargest}));
  const auto small = GreedyOrder(Relations({10, 10}), std::vector{Edge(0, 1, 1000, 1000)});
  EXPECT_EQ(Estimates(small), (std::vector<int64_t>{1}));
  // A negative count (no valid footer has one) counts as no row.
  const auto negative = GreedyOrder(Relations({10, -5}), std::vector{Edge(0, 1, -1, 3)});
  EXPECT_EQ(Estimates(negative), (std::vector<int64_t>{0}));
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
  // A dependent join outputs at most its left input's rows, for every kind the unnesting pass
  // admits, so it reads through to that side. The pass rewrites top-down, so this is what the outer
  // of two nested dependent joins has to read to choose a build side at all (ADR 0026).
  EXPECT_EQ(EstimateRows(DependentJoinNode{.left = scan, .right = scan}),
            std::optional<int64_t>(100));
  EXPECT_EQ(EstimateRows(DependentJoinNode{
                .left = node(DependentJoinNode{.left = scan, .right = scan}), .right = scan}),
            std::optional<int64_t>(100))
      << "nested, as the pass meets them";
  EXPECT_EQ(EstimateRows(DependentJoinNode{.left = node(JoinNode{.left = scan, .right = scan}),
                                           .right = scan}),
            std::nullopt)
      << "a join whose own estimate nothing set stays unknown through it";

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
  const auto steps = GreedyOrder(Relations(rows), chain);
  ASSERT_EQ(steps.size(), kMaxRelations - 1);
  // The chain joins from its largest end, one relation and one key per step.
  EXPECT_EQ(Order(steps).front(), kMaxRelations - 1);
  for (std::size_t k = 0; k < steps.size(); ++k) {
    EXPECT_EQ(steps[k].right.index, kMaxRelations - 2 - k);
    EXPECT_EQ(steps[k].edges.size(), 1U);
  }

  std::vector<JoinEdge> clique;
  for (std::size_t a = 0; a < kMaxRelations; ++a) {
    for (std::size_t b = a + 1; b < kMaxRelations; ++b) {
      clique.push_back(Edge(a, b, 1000, 1000));
    }
  }
  const auto all =
      GreedyOrder(Relations(std::vector<std::optional<int64_t>>(kMaxRelations, 1000)), clique);
  ASSERT_EQ(all.size(), kMaxRelations - 1);
  EXPECT_EQ(Order(all).front(), 0U);
  for (std::size_t k = 0; k < all.size(); ++k) {
    EXPECT_EQ(all[k].right.index, k + 1);
    EXPECT_EQ(all[k].edges.size(), k + 1) << k;
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
  const auto steps = GreedyOrder(Relations({1000, 10, 900}), chain);
  EXPECT_EQ(Order(steps), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(Estimates(steps), (std::vector<int64_t>{10, 9}));
  // The first step adds 10 rows to 1000: the added relation stays the build side. The second adds
  // 900 rows to an estimate of 10, which is 90x and so decisive: it builds on the left instead.
  EXPECT_EQ(Builds(steps), (std::vector<BuildSide>{BuildSide::kRight, BuildSide::kLeft}));
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
    return Builds(GreedyOrder(Relations({1000, 10, rows}), edges, ratio)).back();
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
    return Builds(GreedyOrder(Relations({1000, 10, rows}), edges)).back();
  };
  // Unknown rows on the side that would build: it counts as the largest for the order, but a side
  // that may turn out to be large is exactly what must not be chosen to build on.
  EXPECT_EQ(last(std::nullopt), BuildSide::kRight);
  // An estimate of no row at all is just as untrustworthy, and an empty build is free either way.
  EXPECT_EQ(Builds(GreedyOrder(Relations({1000, 0, 1000}), edges)).back(), BuildSide::kRight);
}

TEST(JoinOrderDeathTest, TheBuildSideRatioIsAtLeastOne) {
  EXPECT_DEATH((void)GreedyOrder(Relations({1, 2}), std::vector{Edge(0, 1)}, 99),
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
      GreedyOrder(Relations({kept, 100, 7}), std::vector{Edge(0, 1, 100, 100), Edge(0, 2, 7, 7)});
  EXPECT_EQ(Order(steps), (std::vector<std::size_t>{0, 1, 2}));
  EXPECT_EQ(Builds(steps), (std::vector<BuildSide>{BuildSide::kRight, BuildSide::kRight}));
}

// The range arithmetic is guarded. Once the span is computable, a constant inside the range cannot
// overflow the subtraction -- the difference is at most the span -- so the case that can is a
// narrow range and a constant far outside it, which is where `max - constant` runs away. No Parquet
// column and no bound literal reach it today, a constant being typed as its column, so this pins
// the same defensive arithmetic KeyDomain already carries rather than a live case.
TEST(JoinOrderTest, PredicateRowsGuardsIts128BitArithmetic) {
  const ColumnRange narrow{.min = 0, .max = 10, .non_null = 100, .exact = true};
  // `>= kInt128Min` admits every value, and `max - kInt128Min` is what would overflow.
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kGe, kInt128Min), narrow, 100), 33);
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kGt, kInt128Min), narrow, 100), 33);
  // The mirror is asymmetric, because the subtrahend differs: `< kInt128Max` computes
  // `kInt128Max - min`, which fits and so admits every value exactly; `<= kInt128Max` then adds one
  // for the closed bound, and that is what overflows.
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kLt, kInt128Max), narrow, 100), 100);
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kLe, kInt128Max), narrow, 100), 33);
  // A constant just outside the range still computes exactly, so the guard does not fire early.
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kGe, -1), narrow, 100), 100);
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kLt, 11), narrow, 100), 100);
  // A range whose own span overflows is not a range at all: the span guard takes it first.
  const ColumnRange unbounded{.min = kInt128Min, .max = kInt128Max, .non_null = 100, .exact = true};
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kGe, 0), unbounded, 100), 33);
  // The product guard: with the span computable, `rows` times a numerator that wide overflows
  // before the division brings it back, and keeping every row errs toward the plan it already had.
  const ColumnRange wide{.min = 0, .max = kInt128Max - 1, .non_null = 100, .exact = true};
  EXPECT_EQ(PredicateRows(100, Compare(CompareOp::kGe, 0), wide, 100), 100);
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
  EXPECT_DEATH((void)GreedyOrder(Relations({1, 2}), std::vector{Edge(1, 1)}), "edge.a != edge.b");
  EXPECT_DEATH((void)GreedyOrder(Relations({1, 2}), std::vector{Edge(0, 2)}), "edge.b < relations");
  EXPECT_DEATH((void)FirstUnconnected(2, std::vector{Edge(3, 0)}), "edge.a < relations");
}

// OrderJoins needs connected relations: the binder checks connectivity first.
TEST(JoinOrderDeathTest, TheRelationsMustBeConnected) {
  EXPECT_DEATH((void)GreedyOrder(Relations({1, 2, 3}), std::vector{Edge(1, 2)}), "next.has_value");
}

}  // namespace
}  // namespace antb1::plan
