// Column ids (ADR 0022): the ids each node outputs, plan::ResolvePositions, which sets every column
// index from the ids, and plan::PositionMismatch, which names the first broken invariant.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/plan/logical_plan.h"
#include "antb1/plan/optimizer.h"
#include "antb1/plan/types.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::AllTypesSchema;
using testing::FakeTable;

LogicalNodePtr Node(LogicalNode node) {
  return std::make_shared<const LogicalNode>(std::move(node));
}

BoundColumn Column(ColumnId id, int index) {
  return BoundColumn{.index = index, .id = id, .name = "c", .type = LogicalType::kBigInt};
}

ExprPtr Leaf(ColumnId id, int index) {
  return std::make_shared<const Expr>(Expr{
      .node = ColumnExpr{.index = index, .id = id}, .type = LogicalType::kBigInt, .name = "x"});
}

constexpr ColumnId kA{1};
constexpr ColumnId kB{2};
constexpr ColumnId kC{3};

// Scan of fields 0, 1 and 2 of a table, as columns #1, #2 and #3.
LogicalNodePtr Scan(std::vector<ColumnId> ids = {kA, kB, kC}) {
  return Node(ScanNode{.table = std::make_shared<FakeTable>(AllTypesSchema(), 100),
                       .table_name = "t",
                       .fields = {0, 1, 2},
                       .ids = std::move(ids),
                       .span = {}});
}

LogicalPlan Plan(LogicalNodePtr root) {
  std::vector<OutputColumn> output;
  for (const ColumnId id : OutputIds(*root)) {
    output.push_back(OutputColumn{.name = "c", .type = LogicalType::kBigInt, .id = id});
  }
  return LogicalPlan{.root = std::move(root), .output = std::move(output)};
}

// A Filter over the Scan reading `column`.
LogicalPlan FilterPlan(BoundColumn column) {
  return Plan(Node(FilterNode{
      .input = Scan(),
      .predicates = {Predicate{.kind = Predicate::Kind::kIsNotNull, .column = std::move(column)}},
      .span = {}}));
}

TEST(ColumnIdsTest, OutputIdsOfEveryNode) {
  const LogicalNodePtr scan = Scan();
  EXPECT_EQ(OutputIds(*scan), (std::vector<ColumnId>{kA, kB, kC}));
  EXPECT_EQ(OutputIds(FilterNode{.input = scan, .predicates = {}, .span = {}}), OutputIds(*scan));
  EXPECT_EQ(OutputIds(ComputeNode{.input = scan,
                                  .exprs = {Leaf(kA, 0), Leaf(kB, 1)},
                                  .ids = {ColumnId{4}, ColumnId{5}},
                                  .span = {}}),
            (std::vector<ColumnId>{kA, kB, kC, ColumnId{4}, ColumnId{5}}));
  EXPECT_EQ(OutputIds(ProjectNode{.input = scan,
                                  .columns = {Column(kC, 2), Column(kA, 0)},
                                  .constants = {},
                                  .ids = {ColumnId{6}, ColumnId{7}},
                                  .span = {}}),
            (std::vector<ColumnId>{ColumnId{6}, ColumnId{7}}));
  const AggregateCall count{.kind = AggKind::kCountStar, .id = ColumnId{8}};
  const AggregateCall sum{.kind = AggKind::kSum,
                          .arg = Column(kB, 1),
                          .type = LogicalType::kHugeInt,
                          .id = ColumnId{9}};
  EXPECT_EQ(OutputIds(AggregateNode{.input = scan, .aggregates = {count, sum}, .span = {}}),
            (std::vector<ColumnId>{ColumnId{8}, ColumnId{9}}));
  EXPECT_EQ(OutputIds(GroupAggregateNode{.input = scan,
                                         .keys = {Column(kA, 0)},
                                         .key_ids = {ColumnId{10}},
                                         .aggregates = {sum},
                                         .span = {}}),
            (std::vector<ColumnId>{ColumnId{10}, ColumnId{9}}));
  EXPECT_EQ(
      OutputIds(SortNode{.input = scan, .keys = {SortKey{.column = Column(kB, 1)}}, .span = {}}),
      OutputIds(*scan));
  EXPECT_EQ(OutputIds(LimitNode{.input = scan, .limit = 1, .offset = 0, .span = {}}),
            OutputIds(*scan));
  EXPECT_EQ(OutputIds(RowCountNode{.table = nullptr, .table_name = "t", .id = ColumnId{11}}),
            (std::vector<ColumnId>{ColumnId{11}}));
}

TEST(ColumnIdsTest, ResolvePositionsSetsEveryIndexFromTheIds) {
  // Every reference starts at a wrong index. A PredicateExpr's own column is local to its
  // operands (index 0) and keeps it.
  const auto condition = std::make_shared<const Expr>(
      Expr{.node = PredicateExpr{.predicate = Predicate{.kind = Predicate::Kind::kIsNotNull,
                                                        .column = Column(kNoColumnId, 0)},
                                 .operands = {Leaf(kB, 7)}},
           .type = LogicalType::kBoolean,
           .name = "b"});
  const auto when = std::make_shared<const Expr>(
      Expr{.node = CaseExpr{.whens = {condition}, .thens = {Leaf(kA, 7)}, .otherwise = nullptr},
           .type = LogicalType::kBigInt,
           .name = "w"});
  const ColumnId computed{4};
  const ColumnId key{5};
  const ColumnId total{6};
  const LogicalNodePtr filter =
      Node(FilterNode{.input = Scan(),
                      .predicates = {Predicate{.kind = Predicate::Kind::kCompareColumns,
                                               .column = Column(kC, 0),
                                               .other = Column(kA, 2)}},
                      .span = {}});
  const LogicalNodePtr compute =
      Node(ComputeNode{.input = filter, .exprs = {when}, .ids = {computed}, .span = {}});
  const LogicalNodePtr group = Node(GroupAggregateNode{
      .input = compute,
      .keys = {Column(computed, 0)},
      .key_ids = {key},
      .aggregates = {AggregateCall{.kind = AggKind::kSum, .arg = Column(kB, 0), .id = total}},
      .span = {}});
  const LogicalNodePtr sort =
      Node(SortNode{.input = group, .keys = {SortKey{.column = Column(total, 0)}}, .span = {}});
  const LogicalNodePtr project = Node(ProjectNode{
      .input = sort,
      .columns = {Column(key, 1), Column(kNoColumnId, -1), Column(total, 0)},
      .constants = {std::nullopt, Constant{.type = LogicalType::kBigInt, .value = Int128{1}},
                    std::nullopt},
      .ids = {ColumnId{7}, ColumnId{8}, ColumnId{9}},
      .span = {}});
  const LogicalPlan plan =
      Plan(Node(LimitNode{.input = project, .limit = 5, .offset = 0, .span = {}}));
  EXPECT_EQ(PositionMismatch(plan), "Filter: column #3 is at 2, not 0");

  const LogicalPlan resolved = ResolvePositions(plan);
  EXPECT_EQ(PositionMismatch(resolved), std::nullopt);
  const auto& limit = std::get<LimitNode>(*resolved.root);
  const auto& p = std::get<ProjectNode>(*limit.input);
  EXPECT_EQ(p.columns[0].index, 0);
  EXPECT_EQ(p.columns[1].index, -1) << "a constant";
  EXPECT_EQ(p.columns[2].index, 1);
  const auto& s = std::get<SortNode>(*p.input);
  EXPECT_EQ(s.keys[0].column.index, 1);
  const auto& g = std::get<GroupAggregateNode>(*s.input);
  EXPECT_EQ(g.keys[0].index, 3);
  EXPECT_EQ(g.aggregates[0].arg.value_or(BoundColumn{}).index, 1);
  const auto& c = std::get<ComputeNode>(*g.input);
  const auto& cases = std::get<CaseExpr>(c.exprs[0]->node);
  EXPECT_EQ(std::get<ColumnExpr>(cases.thens[0]->node).index, 0);
  const auto& predicate = std::get<PredicateExpr>(cases.whens[0]->node);
  EXPECT_EQ(std::get<ColumnExpr>(predicate.operands[0]->node).index, 1);
  EXPECT_EQ(predicate.predicate.column.value_or(BoundColumn{.index = -1}).index, 0)
      << "local to the operands";
  const auto& f = std::get<FilterNode>(*c.input);
  EXPECT_EQ(f.predicates[0].column.value_or(BoundColumn{}).index, 2);
  EXPECT_EQ(f.predicates[0].other.value_or(BoundColumn{.index = -1}).index, 0);
  EXPECT_EQ(f.input, std::get<FilterNode>(*filter).input) << "the Scan has no references";

  // A resolved plan is its own resolution: every node keeps its pointer.
  EXPECT_EQ(ResolvePositions(resolved).root, resolved.root);
  EXPECT_EQ(ResolvePositions(LogicalPlan{}).root, nullptr);
  EXPECT_EQ(PositionMismatch(LogicalPlan{}), std::nullopt);
}

TEST(ColumnIdsTest, MapColumnsKeepsUnchangedExpressions) {
  const auto sum = std::make_shared<const Expr>(
      Expr{.node = ArithExpr{.op = ArithOp::kAdd, .left = Leaf(kA, 0), .right = Leaf(kB, 1)},
           .type = LogicalType::kBigInt,
           .name = "s"});
  EXPECT_EQ(MapColumns(sum, [](const ColumnExpr& column) { return column; }), sum);
  const ExprPtr moved = MapColumns(sum, [](const ColumnExpr& column) {
    return ColumnExpr{.index = column.index + 1, .id = column.id};
  });
  ASSERT_NE(moved, sum);
  const auto& arith = std::get<ArithExpr>(moved->node);
  EXPECT_EQ(std::get<ColumnExpr>(arith.left->node).index, 1);
  EXPECT_EQ(std::get<ColumnExpr>(arith.right->node).index, 2);
  EXPECT_EQ(std::get<ColumnExpr>(arith.right->node).id, kB);
}

TEST(ColumnIdsTest, PositionMismatchNamesTheFirstBrokenInvariant) {
  EXPECT_EQ(PositionMismatch(FilterPlan(Column(kNoColumnId, 0))),
            "Filter: a reference without an id");
  EXPECT_EQ(PositionMismatch(FilterPlan(Column(ColumnId{99}, 0))),
            "Filter: column #99 is not in its input");
  // A Project passing #1 through twice outputs it twice.
  const LogicalNodePtr twice = Node(ProjectNode{.input = Scan(),
                                                .columns = {Column(kA, 0), Column(kA, 0)},
                                                .constants = {},
                                                .ids = {kA, kA},
                                                .span = {}});
  EXPECT_EQ(
      PositionMismatch(Plan(Node(FilterNode{
          .input = twice,
          .predicates = {Predicate{.kind = Predicate::Kind::kIsNotNull, .column = Column(kA, 0)}},
          .span = {}}))),
      "Filter: column #1 is in its input twice");
  EXPECT_EQ(PositionMismatch(Plan(Scan({kA, kA, kC}))), "Scan: column #1 is defined twice");
  EXPECT_EQ(PositionMismatch(Plan(Scan({kA, kB}))), "Scan: 2 ids for 3 fields");
  EXPECT_EQ(PositionMismatch(Plan(Node(ComputeNode{
                .input = Scan(), .exprs = {Leaf(kA, 0)}, .ids = {kNoColumnId}, .span = {}}))),
            "Compute: a column without an id");
  EXPECT_EQ(PositionMismatch(Plan(
                Node(ComputeNode{.input = Scan(), .exprs = {Leaf(kA, 0)}, .ids = {}, .span = {}}))),
            "Compute: 0 ids for 1 expressions");
  EXPECT_EQ(
      PositionMismatch(Plan(Node(ProjectNode{
          .input = Scan(), .columns = {Column(kA, 0)}, .constants = {}, .ids = {}, .span = {}}))),
      "Project: 0 ids for 1 columns");
  EXPECT_EQ(PositionMismatch(Plan(Node(GroupAggregateNode{
                .input = Scan(), .keys = {Column(kA, 0)}, .key_ids = {}, .aggregates = {}}))),
            "GroupAggregate: 0 ids for 1 keys");
  EXPECT_EQ(PositionMismatch(Plan(Node(AggregateNode{
                .input = Scan(), .aggregates = {AggregateCall{.kind = AggKind::kCountStar}}}))),
            "Aggregate: a column without an id");
  EXPECT_EQ(PositionMismatch(Plan(Node(RowCountNode{.table = nullptr, .table_name = "t"}))),
            "RowCount: a column without an id");
  EXPECT_EQ(PositionMismatch(Plan(Node(SortNode{
                .input = Scan(), .keys = {SortKey{.column = Column(kC, 0)}}, .span = {}}))),
            "Sort: column #3 is at 2, not 0");
  // The plan's output columns are the root's.
  LogicalPlan wrong_output = Plan(Scan());
  wrong_output.output[1].id = ColumnId{9};
  EXPECT_EQ(PositionMismatch(wrong_output), "output: column 1 is #9, but the root outputs #2");
  wrong_output.output.pop_back();
  EXPECT_EQ(PositionMismatch(wrong_output), "output: 2 columns, but the root outputs 3");
}

TEST(ColumnIdsTest, ResolvesAggregateArguments) {
  const LogicalPlan plan =
      Plan(Node(AggregateNode{.input = Scan(),
                              .aggregates = {AggregateCall{.kind = AggKind::kSum,
                                                           .arg = Column(kC, 0),
                                                           .type = LogicalType::kHugeInt,
                                                           .id = ColumnId{4}}},
                              .span = {}}));
  EXPECT_EQ(PositionMismatch(plan), "Aggregate: column #3 is at 2, not 0");
  const LogicalPlan resolved = ResolvePositions(plan);
  EXPECT_EQ(std::get<AggregateNode>(*resolved.root).aggregates[0].arg.value_or(BoundColumn{}).index,
            2);
  EXPECT_EQ(ResolvePositions(resolved).root, resolved.root);
}

// A broken invariant comes before an index difference, and the first broken invariant (bottom-up)
// before the others.
TEST(ColumnIdsTest, PositionMismatchReportsBrokenInvariantsFirst) {
  const LogicalNodePtr misplaced = FilterPlan(Column(kC, 0)).root;  // #3 is at 2, not 0
  EXPECT_EQ(PositionMismatch(Plan(Node(ProjectNode{.input = misplaced,
                                                   .columns = {Column(kA, 0)},
                                                   .constants = {},
                                                   .ids = {},
                                                   .span = {}}))),
            "Project: 0 ids for 1 columns");
  EXPECT_EQ(PositionMismatch(
                Plan(Node(FilterNode{.input = Scan({kA, kA, kC}),
                                     .predicates = {Predicate{.kind = Predicate::Kind::kIsNotNull,
                                                              .column = Column(ColumnId{99}, 0)}},
                                     .span = {}}))),
            "Scan: column #1 is defined twice");
}

TEST(ColumnIdsDeathTest, BrokenPlansAbort) {
  EXPECT_DEATH(ResolvePositions(FilterPlan(Column(ColumnId{99}, 0))),
               "Filter: column #99 is not in its input");
  EXPECT_DEATH(CheckPositions(FilterPlan(Column(kC, 0))), "Filter: column #3 is at 2, not 0");
  // Optimize checks the positions its rules computed before it resolves them.
  EXPECT_DEATH(Optimize(FilterPlan(Column(kC, 0))), "Filter: column #3 is at 2, not 0");
}

}  // namespace
}  // namespace antb1::plan
