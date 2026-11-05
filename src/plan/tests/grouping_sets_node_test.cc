// The grouping-sets node (docs/adr/0038-grouping-sets.md) in every pass that walks a plan: its
// output and its reads, ResolvePositions and PositionMismatch, a clone's fresh ids, EXPLAIN, the
// optimizer's pruning, the join order's estimate and the shared sub-plans, which never share it.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/plan/explain.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/optimizer.h"
#include "antb1/plan/shared_sub_plans.h"
#include "antb1/plan/types.h"

#include "../join_order.h"
#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::AllTypesSchema;
using testing::FakeTable;

LogicalNodePtr Node(LogicalNode node) {
  return std::make_shared<const LogicalNode>(std::move(node));
}

BoundColumn Column(ColumnId id, int index, std::string name) {
  return BoundColumn{
      .index = index, .id = id, .name = std::move(name), .type = LogicalType::kBigInt};
}

// A scan of fields 0 to 3 of a table of 100 rows (or of an unknown count), as columns first ..
// first + 3.
LogicalNodePtr Scan(int first = 1, std::optional<int64_t> rows = 100,
                    std::shared_ptr<FakeTable> table = nullptr) {
  if (table == nullptr) {
    table = std::make_shared<FakeTable>(AllTypesSchema(), rows);
  }
  std::vector<ColumnId> ids;
  ids.reserve(4);
  for (int i = 0; i < 4; ++i) {
    ids.push_back(ColumnId{static_cast<std::uint32_t>(first + i)});
  }
  return Node(ScanNode{.table = std::move(table),
                       .table_name = "t",
                       .fields = {0, 1, 2, 3},
                       .ids = std::move(ids),
                       .span = {}});
}

// ROLLUP (a, b) with SUM(v) and GROUPING(a, b) over the scan of columns `first`..: a and b its
// first two columns, v its fourth; its own columns from `own` on. Every index is wrong (0), for
// ResolvePositions to set.
GroupingSetsNode Rollup(LogicalNodePtr input, std::uint32_t first = 1, std::uint32_t own = 10) {
  return GroupingSetsNode{
      .input = std::move(input),
      .keys = {Column(ColumnId{first}, 0, "a"), Column(ColumnId{first + 1}, 0, "b")},
      .key_ids = {ColumnId{own}, ColumnId{own + 1}},
      .sets = {{0, 1}, {0}, {}},
      .aggregates = {AggregateCall{.kind = AggKind::kSum,
                                   .arg = Column(ColumnId{first + 3}, 0, "v"),
                                   .type = LogicalType::kHugeInt,
                                   .id = ColumnId{own + 2}}},
      .grouping = {GroupingCall{.args = {0, 1}, .id = ColumnId{own + 3}}},
      .span = {}};
}

LogicalPlan Plan(LogicalNodePtr root) {
  std::vector<OutputColumn> output;
  for (const ColumnId id : OutputIds(*root)) {
    output.push_back(OutputColumn{.name = "c", .type = LogicalType::kBigInt, .id = id});
  }
  return LogicalPlan{.root = std::move(root), .output = std::move(output)};
}

TEST(GroupingSetsNodeTest, OutputsItsKeysCallsAndGroupingColumns) {
  const GroupingSetsNode node = Rollup(Scan());
  EXPECT_EQ(NodeName(LogicalNode(node)), "GroupingSets");
  EXPECT_EQ(InputsOf(LogicalNode(node)).size(), 1U);
  EXPECT_EQ(OutputIds(LogicalNode(node)),
            (std::vector<ColumnId>{ColumnId{10}, ColumnId{11}, ColumnId{12}, ColumnId{13}}));
  EXPECT_EQ(FreeVariables(LogicalNode(node)), std::vector<ColumnId>{});
  EXPECT_EQ(MaxColumnId(LogicalNode(node)), ColumnId{13});
  const LogicalNodePtr copy = WithInputs(LogicalNode(node), {Scan()});
  EXPECT_EQ(std::get<GroupingSetsNode>(*copy).sets, node.sets);
}

TEST(GroupingSetsNodeTest, ResolvePositionsSetsTheKeysAndArguments) {
  const LogicalPlan resolved = ResolvePositions(Plan(Node(Rollup(Scan()))));
  const auto& node = std::get<GroupingSetsNode>(*resolved.root);
  EXPECT_EQ(node.keys[0].index, 0);
  EXPECT_EQ(node.keys[1].index, 1);
  EXPECT_EQ(node.aggregates[0].arg.value_or(BoundColumn{}).index, 3);  // it has one
  EXPECT_EQ(PositionMismatch(resolved), std::nullopt);
}

TEST(GroupingSetsNodeTest, PositionMismatchNamesABrokenNode) {
  const auto broken = [](auto change) {
    GroupingSetsNode node = Rollup(Scan());
    change(node);
    return PositionMismatch(Plan(Node(std::move(node))));
  };
  EXPECT_EQ(broken([](GroupingSetsNode& n) { n.sets.clear(); }), "GroupingSets: no sets");
  EXPECT_EQ(broken([](GroupingSetsNode& n) { n.sets[0] = {0, 2}; }),
            "GroupingSets: key 2 of a set, of 2 keys");
  EXPECT_EQ(broken([](GroupingSetsNode& n) { n.sets[0] = {1, 0}; }),
            "GroupingSets: a set's keys are not ascending and distinct");
  EXPECT_EQ(broken([](GroupingSetsNode& n) { n.sets[0] = {0, 0}; }),
            "GroupingSets: a set's keys are not ascending and distinct");
  EXPECT_EQ(broken([](GroupingSetsNode& n) { n.key_ids.pop_back(); }),
            "GroupingSets: 1 ids for 2 keys");
  EXPECT_EQ(broken([](GroupingSetsNode& n) { n.grouping[0].args.clear(); }),
            "GroupingSets: GROUPING of 0 arguments");
  EXPECT_EQ(broken([](GroupingSetsNode& n) { n.grouping[0].args = {0, 5}; }),
            "GroupingSets: GROUPING of key 5, of 2 keys");
  EXPECT_EQ(broken([](GroupingSetsNode& n) { n.grouping[0].id = ColumnId{10}; }),
            "GroupingSets: column #10 is defined twice");
}

TEST(GroupingSetsNodeTest, ACloneMintsEveryColumnItDefines) {
  ColumnId last{13};
  const LogicalNodePtr clone = CloneWithFreshIds(Node(Rollup(Scan())), last);
  const std::vector<ColumnId> ids = OutputIds(*clone);
  ASSERT_EQ(ids.size(), 4U);
  for (const ColumnId id : ids) {
    EXPECT_GT(std::to_underlying(id), 13U);
  }
  EXPECT_EQ(ids.back(), last);
  EXPECT_EQ(PositionMismatch(ResolvePositions(Plan(clone))), std::nullopt);
}

TEST(GroupingSetsNodeTest, ExplainNamesTheSetsByTheirKeys) {
  EXPECT_EQ(ExplainNode(LogicalNode(Rollup(Scan()))),
            "GroupingSets keys=[a, b] sets=[(a, b), (a), ()] SUM(v) GROUPING(a, b)");
}

// The scan under it keeps the fields of its keys and arguments only.
TEST(GroupingSetsNodeTest, PruningKeepsItsKeysAndArguments) {
  const LogicalPlan optimized = Optimized(Plan(Node(Rollup(Scan()))));
  ASSERT_EQ(PositionMismatch(optimized), std::nullopt);
  const auto& node = std::get<GroupingSetsNode>(*optimized.root);
  const auto& scan = std::get<ScanNode>(*node.input);
  EXPECT_EQ(scan.fields, (std::vector<int>{0, 1, 3}));
  EXPECT_EQ(OutputIds(*optimized.root),
            (std::vector<ColumnId>{ColumnId{10}, ColumnId{11}, ColumnId{12}, ColumnId{13}}));
}

// Every input row's group in every set, at most, and the empty set's row over no input.
TEST(GroupingSetsNodeTest, EstimatesARowPerInputRowAndSet) {
  EXPECT_EQ(EstimateRows(LogicalNode(Rollup(Scan()))), std::optional<int64_t>(300));
  EXPECT_EQ(EstimateRows(LogicalNode(Rollup(Scan(1, 0)))), std::optional<int64_t>(3));
  EXPECT_EQ(EstimateRows(LogicalNode(Rollup(Scan(1, std::nullopt)))), std::nullopt);
}

// Two identical grouping-sets sub-plans under a join are not shared (not yet): only their scans
// are reported.
TEST(GroupingSetsNodeTest, IsNotAShareableSubPlan) {
  const auto table = std::make_shared<FakeTable>(AllTypesSchema(), 100);
  const LogicalNodePtr left = Node(Rollup(Scan(1, 100, table), 1, 10));
  const LogicalNodePtr right = Node(Rollup(Scan(21, 100, table), 21, 30));
  const LogicalPlan plan =
      ResolvePositions(Plan(Node(JoinNode{.kind = JoinKind::kInner,
                                          .left = left,
                                          .right = right,
                                          .keys = {JoinKey{.left = Column(ColumnId{10}, 0, "a"),
                                                           .right = Column(ColumnId{30}, 0, "a")}},
                                          .residual = {},
                                          .build = BuildSide::kRight})));
  const std::vector<SharedSubPlan> shared = SharedSubPlans(plan);
  ASSERT_FALSE(shared.empty());
  for (const SharedSubPlan& group : shared) {
    EXPECT_TRUE(std::holds_alternative<ScanNode>(*group.members.front()))
        << ExplainNode(*group.members.front());
  }
}

}  // namespace
}  // namespace antb1::plan
