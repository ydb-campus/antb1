// Column ids (ADR 0022): the ids each node outputs, plan::ResolvePositions, which sets every column
// index from the ids, and plan::PositionMismatch, which names the first broken invariant.

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/plan/explain.h"
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

// Optimize's rules find columns by their ids and leave positions to ResolvePositions, its last
// step: an index that is not its column's position is set to it.
TEST(ColumnIdsTest, OptimizeResolvesStaleIndexes) {
  const LogicalPlan optimized = Optimized(FilterPlan(Column(kC, 0)));  // #3 is at 2, not 0
  const auto* filter = std::get_if<FilterNode>(optimized.root.get());
  ASSERT_NE(filter, nullptr);
  EXPECT_EQ(filter->predicates.at(0).column.value_or(BoundColumn{}).index, 2);
  EXPECT_EQ(PositionMismatch(optimized), std::nullopt);
}

constexpr ColumnId kD{4};
constexpr ColumnId kE{5};
constexpr ColumnId kF{6};

// A BOOLEAN residual comparing columns `a` and `b` (operand-local predicate columns 0 and 1).
ExprPtr Residual(ColumnId a, ColumnId b, int index = 0) {
  return std::make_shared<const Expr>(
      Expr{.node = PredicateExpr{.predicate = Predicate{.kind = Predicate::Kind::kCompareColumns,
                                                        .column = Column(kNoColumnId, 0),
                                                        .other = Column(kNoColumnId, 1),
                                                        .op = CompareOp::kLt},
                                 .operands = {Leaf(a, index), Leaf(b, index)}},
           .type = LogicalType::kBoolean,
           .name = "r"});
}

// A join of Scan() (#1, #2, #3) and a second Scan (#4, #5, #6) on #2 = #5, every index at 0.
JoinNode Join(JoinKind kind, BuildSide build = BuildSide::kRight) {
  return JoinNode{.kind = kind,
                  .left = Scan(),
                  .right = Scan({kD, kE, kF}),
                  .keys = {JoinKey{.left = Column(kB, 0), .right = Column(kE, 0)}},
                  .residual = {},
                  .build = build,
                  .span = {}};
}

TEST(ColumnIdsTest, OutputIdsOfEveryJoinKind) {
  const std::vector<ColumnId> left = {kA, kB, kC};
  const std::vector<ColumnId> both = {kA, kB, kC, kD, kE, kF};
  EXPECT_EQ(OutputIds(Join(JoinKind::kInner)), both);
  EXPECT_EQ(OutputIds(Join(JoinKind::kLeft)), both);
  EXPECT_EQ(OutputIds(Join(JoinKind::kOneRow)), both);
  EXPECT_EQ(OutputIds(Join(JoinKind::kSemi)), left);
  EXPECT_EQ(OutputIds(Join(JoinKind::kAnti)), left);
  EXPECT_EQ(OutputIds(Join(JoinKind::kNullAwareAnti)), left);
}

// A dependent join outputs its inputs' ids by its kind's rule, exactly as the join of that kind.
TEST(ColumnIdsTest, OutputIdsOfEveryDependentJoinKind) {
  const auto dependent = [](JoinKind kind) {
    return DependentJoinNode{.kind = kind, .left = Scan(), .right = Scan({kD, kE, kF}), .span = {}};
  };
  EXPECT_EQ(OutputIds(dependent(JoinKind::kSemi)), (std::vector<ColumnId>{kA, kB, kC}));
  EXPECT_EQ(OutputIds(dependent(JoinKind::kAnti)), (std::vector<ColumnId>{kA, kB, kC}));
  EXPECT_EQ(OutputIds(dependent(JoinKind::kOneRow)),
            (std::vector<ColumnId>{kA, kB, kC, kD, kE, kF}));
}

// A plan's free variables (ADR 0023): the ids it reads that none of its own nodes defines. A
// subquery is correlated exactly when its plan has some.
TEST(ColumnIdsTest, FreeVariablesAreTheIdsReadButNotDefined) {
  // A plan that reads only its own Scan's columns has none, whatever it computes above them.
  const LogicalNodePtr own = Node(ComputeNode{
      .input = FilterPlan(Column(kA, 0)).root, .exprs = {Leaf(kB, 1)}, .ids = {ColumnId{4}}});
  EXPECT_TRUE(FreeVariables(*own).empty());

  // A Filter reading #9, which nothing below defines.
  const LogicalNodePtr reads_outer =
      Node(FilterNode{.input = Scan(),
                      .predicates = {Predicate{.kind = Predicate::Kind::kCompareColumns,
                                               .column = Column(kA, 0),
                                               .other = Column(ColumnId{9}, 0)}},
                      .span = {}});
  EXPECT_EQ(FreeVariables(*reads_outer), (std::vector<ColumnId>{ColumnId{9}}));

  // Sorted and without repeats, over every kind of reference: a Compute's expression, a Sort key,
  // an aggregate's argument and a join's key and residual.
  const LogicalNodePtr many = Node(SortNode{
      .input = Node(AggregateNode{
          .input = Node(ComputeNode{.input = Scan(),
                                    .exprs = {Leaf(ColumnId{30}, 0), Leaf(ColumnId{20}, 0)},
                                    .ids = {ColumnId{40}, ColumnId{41}}}),
          .aggregates = {AggregateCall{
              .kind = AggKind::kSum, .arg = Column(ColumnId{20}, 0), .id = ColumnId{42}}}}),
      .keys = {SortKey{.column = Column(ColumnId{42}, 0)}},
      .span = {}});
  EXPECT_EQ(FreeVariables(*many), (std::vector<ColumnId>{ColumnId{20}, ColumnId{30}}));

  // A dependent join binds its left input's ids for its right input, so a right input that reads
  // them has no free variables left at the dependent join.
  const LogicalNodePtr bound = Node(
      DependentJoinNode{.kind = JoinKind::kSemi, .left = Scan(), .right = reads_outer, .span = {}});
  EXPECT_EQ(FreeVariables(*reads_outer), (std::vector<ColumnId>{ColumnId{9}}));
  EXPECT_EQ(FreeVariables(*bound), (std::vector<ColumnId>{ColumnId{9}}))
      << "#9 is defined by neither input, so it stays free of the whole";
  const LogicalNodePtr reads_left =
      Node(FilterNode{.input = Scan({ColumnId{4}, ColumnId{5}, ColumnId{6}}),
                      .predicates = {Predicate{.kind = Predicate::Kind::kCompareColumns,
                                               .column = Column(ColumnId{4}, 0),
                                               .other = Column(kB, 0)}},
                      .span = {}});
  EXPECT_EQ(FreeVariables(*reads_left), (std::vector<ColumnId>{kB}));
  EXPECT_TRUE(
      FreeVariables(*Node(DependentJoinNode{
                        .kind = JoinKind::kSemi, .left = Scan(), .right = reads_left, .span = {}}))
          .empty())
      << "the left input defines #2";

  // `mark` is defined by the dependent join itself, so a node above that reads it is not free.
  EXPECT_TRUE(
      FreeVariables(*Node(FilterNode{.input = Node(DependentJoinNode{.kind = JoinKind::kSemi,
                                                                     .left = Scan(),
                                                                     .right = Scan({ColumnId{4}}),
                                                                     .mark = ColumnId{50},
                                                                     .span = {}}),
                                     .predicates = {Predicate{.kind = Predicate::Kind::kIsNotNull,
                                                              .column = Column(ColumnId{50}, 0)}},
                                     .span = {}}))
          .empty());
}

// The first shape of the output-id contract (ADR 0023): a dependent join binds its left input's ids
// inside its right input, so a node of the right input may read a column that no input of its own
// outputs -- a free variable. It resolves to its position among the ids the dependent join bound,
// and the same reference outside the right input is still "not in its input".
TEST(ColumnIdsTest, ResolvePositionsBindsLeftIdsInsideTheRightInput) {
  // The right input filters on #2, a column of the left input, which its own Scan does not output.
  const auto right_reading = [](ColumnId outer, int index) {
    return Node(FilterNode{.input = Scan({ColumnId{4}, ColumnId{5}, ColumnId{6}}),
                           .predicates = {Predicate{.kind = Predicate::Kind::kIsNotNull,
                                                    .column = Column(outer, index)}},
                           .span = {}});
  };
  const LogicalPlan plan = Plan(Node(DependentJoinNode{
      .kind = JoinKind::kSemi, .left = Scan(), .right = right_reading(kB, 7), .span = {}}));
  EXPECT_EQ(PositionMismatch(plan), std::optional<std::string>("Filter: column #2 is at 1, not 7"));

  const LogicalPlan resolved = ResolvePositions(plan);
  EXPECT_EQ(PositionMismatch(resolved), std::nullopt) << "resolving twice changes nothing";
  // #2 is the second of the left input's ids, so the free variable's index is 1.
  const auto& join = std::get<DependentJoinNode>(*resolved.root);
  EXPECT_EQ(std::get<FilterNode>(*join.right).predicates[0].column.value_or(BoundColumn{}).index,
            1);

  // The binding ends with the right input: the same reference under the left input is unresolvable.
  EXPECT_EQ(PositionMismatch(Plan(Node(DependentJoinNode{.kind = JoinKind::kSemi,
                                                         .left = right_reading(ColumnId{9}, 0),
                                                         .right = Scan({ColumnId{4}}),
                                                         .span = {}}))),
            std::optional<std::string>("Filter: column #9 is not in its input"));
}

// The second shape of the output-id contract (ADR 0023): a two-input node may also define a column
// of its own, which goes last. No kind sets `mark` yet, so only a hand-built plan has one.
TEST(ColumnIdsTest, OutputIdsOfADependentJoinEndWithItsOwnColumn) {
  const ColumnId mark{12};
  const auto marked = [](JoinKind kind) {
    return DependentJoinNode{
        .kind = kind, .left = Scan(), .right = Scan({kD, kE, kF}), .mark = mark, .span = {}};
  };
  EXPECT_EQ(OutputIds(marked(JoinKind::kSemi)), (std::vector<ColumnId>{kA, kB, kC, mark}));
  EXPECT_EQ(OutputIds(marked(JoinKind::kOneRow)),
            (std::vector<ColumnId>{kA, kB, kC, kD, kE, kF, mark}));
  // A Join defines no column of its own, so the same ids without the marker.
  EXPECT_EQ(OutputIds(Join(JoinKind::kSemi)), (std::vector<ColumnId>{kA, kB, kC}));
}

// Keys resolve against their own input; residual columns against the left ids, then the right ids.
TEST(ColumnIdsTest, ResolvePositionsResolvesJoinKeysPerInputAndResidualsOverBoth) {
  JoinNode join = Join(JoinKind::kInner);
  join.keys = {JoinKey{.left = Column(kC, 0), .right = Column(kF, 0)},
               JoinKey{.left = Column(kA, 7), .right = Column(kD, 7)}};
  join.residual = {Residual(kF, kB)};
  // Over the join's output, a Filter reads a right column.
  const LogicalPlan plan = Plan(Node(FilterNode{
      .input = Node(join),
      .predicates = {Predicate{.kind = Predicate::Kind::kIsNotNull, .column = Column(kE, 0)}},
      .span = {}}));
  EXPECT_EQ(PositionMismatch(plan), "Join: column #3 is at 2, not 0");

  const LogicalPlan resolved = ResolvePositions(plan);
  EXPECT_EQ(PositionMismatch(resolved), std::nullopt);
  const auto& filter = std::get<FilterNode>(*resolved.root);
  EXPECT_EQ(filter.predicates[0].column.value_or(BoundColumn{}).index, 4);
  const auto& j = std::get<JoinNode>(*filter.input);
  EXPECT_EQ(j.keys[0].left.index, 2);
  EXPECT_EQ(j.keys[0].right.index, 2) << "the position in the right input";
  EXPECT_EQ(j.keys[1].left.index, 0);
  EXPECT_EQ(j.keys[1].right.index, 0);
  const auto& residual = std::get<PredicateExpr>(j.residual[0]->node);
  EXPECT_EQ(std::get<ColumnExpr>(residual.operands[0]->node).index, 5) << "after the left input";
  EXPECT_EQ(std::get<ColumnExpr>(residual.operands[1]->node).index, 1);
  EXPECT_EQ(residual.predicate.other.value_or(BoundColumn{}).index, 1) << "operand-local";
  EXPECT_EQ(j.left, join.left) << "the Scans have no references";
  EXPECT_EQ(j.right, join.right);
  EXPECT_EQ(ResolvePositions(resolved).root, resolved.root);

  // Over a semi join, the right input's columns are gone.
  join.kind = JoinKind::kSemi;
  join.residual.clear();
  EXPECT_EQ(
      PositionMismatch(Plan(Node(FilterNode{
          .input = Node(join),
          .predicates = {Predicate{.kind = Predicate::Kind::kIsNotNull, .column = Column(kE, 0)}},
          .span = {}}))),
      "Filter: column #5 is not in its input");
}

TEST(ColumnIdsTest, EveryJoinKindResolves) {
  for (const JoinKind kind : {JoinKind::kInner, JoinKind::kLeft, JoinKind::kSemi, JoinKind::kAnti,
                              JoinKind::kNullAwareAnti}) {
    JoinNode join = Join(kind);
    join.residual = {Residual(kC, kF)};
    const LogicalPlan resolved = ResolvePositions(Plan(Node(join)));
    EXPECT_EQ(PositionMismatch(resolved), std::nullopt) << ToString(kind);
  }
  JoinNode one_row = Join(JoinKind::kOneRow);
  one_row.keys.clear();
  EXPECT_EQ(PositionMismatch(ResolvePositions(Plan(Node(one_row)))), std::nullopt);
  for (const JoinKind kind : {JoinKind::kInner, JoinKind::kLeft, JoinKind::kSemi, JoinKind::kAnti,
                              JoinKind::kNullAwareAnti}) {
    EXPECT_EQ(PositionMismatch(ResolvePositions(Plan(Node(Join(kind, BuildSide::kLeft))))),
              std::nullopt)
        << "every kind but ONE-ROW may build on either input: " << ToString(kind);
  }
}

TEST(ColumnIdsTest, PositionMismatchNamesBrokenJoins) {
  JoinNode types = Join(JoinKind::kInner);
  types.keys[0].right.type = LogicalType::kInteger;
  EXPECT_EQ(PositionMismatch(Plan(Node(types))),
            "Join: key #2 = #5 has two types, BIGINT and INTEGER");

  JoinNode keyless = Join(JoinKind::kInner);
  keyless.keys.clear();
  EXPECT_EQ(PositionMismatch(Plan(Node(keyless))), "Join: INNER join without keys");
  keyless.kind = JoinKind::kNullAwareAnti;
  EXPECT_EQ(PositionMismatch(Plan(Node(keyless))), "Join: NULL-AWARE ANTI join without keys");
  EXPECT_EQ(PositionMismatch(Plan(Node(Join(JoinKind::kOneRow)))),
            "Join: ONE-ROW join with 1 keys");

  JoinNode one_row = Join(JoinKind::kOneRow, BuildSide::kLeft);
  one_row.keys.clear();
  EXPECT_EQ(PositionMismatch(Plan(Node(one_row))), "Join: ONE-ROW join builds on its left input");

  JoinNode residual = Join(JoinKind::kInner);
  residual.residual = {Leaf(kA, 0)};
  EXPECT_EQ(PositionMismatch(Plan(Node(residual))), "Join: a residual of type BIGINT");
  residual.residual = {Residual(kA, ColumnId{99})};
  EXPECT_EQ(PositionMismatch(Plan(Node(residual))), "Join: column #99 is not in its input");

  JoinNode crossed = Join(JoinKind::kInner);
  crossed.keys[0].right = Column(kB, 1);  // a left column as the right key
  EXPECT_EQ(PositionMismatch(Plan(Node(crossed))), "Join: column #2 is not in its input");
  crossed = Join(JoinKind::kInner);
  crossed.keys[0].left = Column(kE, 1);  // a right column as the left key
  EXPECT_EQ(PositionMismatch(Plan(Node(crossed))), "Join: column #5 is not in its input");

  JoinNode twice = Join(JoinKind::kInner);
  twice.right = Scan({kD, kE, kA});  // #1 in both inputs
  EXPECT_EQ(PositionMismatch(Plan(Node(twice))), "Scan: column #1 is defined twice");
}

// ---- CloneWithFreshIds ----

// The reason the function exists: a plan is a tree, so a pass that needs one subtree in two places
// cannot reach the same node twice. The same `Scan` under both inputs of a join is not a plan --
// PositionMismatch names it -- and the clone is what makes the pair legal.
TEST(ColumnIdsTest, ACloneIsWhatLetsOneSubtreeBeReadTwice) {
  const LogicalNodePtr shared = Scan();
  EXPECT_EQ(PositionMismatch(
                Plan(Node(JoinNode{.kind = JoinKind::kInner,
                                   .left = shared,
                                   .right = shared,
                                   .keys = {JoinKey{.left = Column(kB, 1), .right = Column(kB, 1)}},
                                   .span = {}}))),
            "Scan: column #1 is defined twice");

  ColumnId last = MaxColumnId(*shared);
  const LogicalNodePtr clone = CloneWithFreshIds(shared, last);
  const std::vector<ColumnId> cloned = OutputIds(*clone);
  ASSERT_EQ(cloned.size(), 3U);
  EXPECT_EQ(PositionMismatch(Plan(Node(
                JoinNode{.kind = JoinKind::kInner,
                         .left = shared,
                         .right = clone,
                         .keys = {JoinKey{.left = Column(kB, 1), .right = Column(cloned[1], 1)}},
                         .span = {}}))),
            std::nullopt);
}

// Every id the subtree defines is minted again, every read of one follows it, and `last_id` ends
// past them all. The shape does not move: EXPLAIN prints columns by name, so the clone's text is
// the original's.
TEST(ColumnIdsTest, ACloneMintsEveryDefineAndFollowsEveryRead) {
  const LogicalNodePtr original = Node(ProjectNode{
      .input = Node(GroupAggregateNode{
          .input = Node(ComputeNode{
              .input = Scan(), .exprs = {Leaf(kA, 0)}, .ids = {ColumnId{10}}, .span = {}}),
          .keys = {Column(ColumnId{10}, 3)},
          .key_ids = {ColumnId{11}},
          .aggregates = {AggregateCall{
              .kind = AggKind::kSum, .arg = Column(kB, 1), .id = ColumnId{12}}},
          .span = {}}),
      .columns = {Column(ColumnId{11}, 0), Column(ColumnId{12}, 1)},
      .constants = {},
      .ids = {ColumnId{13}, ColumnId{14}},
      .span = {}});
  const LogicalPlan plan = Plan(original);
  ASSERT_EQ(PositionMismatch(plan), std::nullopt);

  ColumnId last = MaxColumnId(*original);
  const ColumnId before = last;
  const LogicalNodePtr clone = CloneWithFreshIds(original, last);
  ASSERT_NE(clone, nullptr);

  // Nothing is shared, and the ids are the ones this minted.
  const std::vector<ColumnId> originals = OutputIds(*original);
  const std::vector<ColumnId> cloned = OutputIds(*clone);
  ASSERT_EQ(cloned.size(), originals.size());
  for (const ColumnId id : cloned) {
    EXPECT_GT(std::to_underlying(id), std::to_underlying(before)) << "not a fresh id";
    EXPECT_EQ(std::ranges::find(originals, id), originals.end()) << "shared with the original";
  }
  EXPECT_EQ(MaxColumnId(*clone), last) << "last_id ends at the largest id the clone holds";
  EXPECT_GT(std::to_underlying(last), std::to_underlying(before));

  // The reads followed their defines: the clone resolves on its own, which it could not do if a
  // key or an argument still named a column of the original.
  EXPECT_EQ(PositionMismatch(Plan(clone)), std::nullopt);
  EXPECT_EQ(Explain(Plan(clone)), Explain(plan)) << "the same plan, read by other names";
}

// A Project column that keeps the id it reads passes it through and defines nothing, which is the
// rule ResolvePositions applies. The clone has to pass the *renamed* id through rather than mint a
// second definition of it: minting would leave the clone's Project outputting an id that nothing
// above it was renamed to, and the consumer above would read a column its input does not have --
// an abort, which is the very thing this function exists to prevent.
TEST(ColumnIdsTest, AClonePassesThroughAProjectsOwnPassThroughIds) {
  const LogicalNodePtr passes = Node(ProjectNode{.input = Scan(),
                                                 .columns = {Column(kA, 0), Column(kB, 1)},
                                                 .constants = {},
                                                 .ids = {kA, kB},  // the ids it reads: passed on
                                                 .span = {}});
  const LogicalNodePtr reader = Node(FilterNode{
      .input = passes,
      .predicates = {Predicate{.kind = Predicate::Kind::kIsNotNull, .column = Column(kB, 1)}},
      .span = {}});
  ASSERT_EQ(PositionMismatch(Plan(reader)), std::nullopt);

  ColumnId last = MaxColumnId(*reader);
  const LogicalNodePtr clone = CloneWithFreshIds(reader, last);
  EXPECT_EQ(PositionMismatch(Plan(clone)), std::nullopt) << Explain(Plan(clone));
  const std::vector<ColumnId> cloned = OutputIds(*clone);
  ASSERT_EQ(cloned.size(), 2U);
  for (const ColumnId id : cloned) {
    EXPECT_NE(id, kA);
    EXPECT_NE(id, kB);
  }
  EXPECT_EQ(Explain(Plan(clone)), Explain(Plan(reader)));
}

// An id the subtree reads but does not define is read from outside it, so it is left alone: that is
// what keeps a clone of a dependent join's left input binding the right input's references, and a
// clone of any subtree reading the columns above it.
TEST(ColumnIdsTest, ACloneKeepsTheIdsItReadsFromOutside) {
  const LogicalNodePtr reads_outer =
      Node(FilterNode{.input = Scan(),
                      .predicates = {Predicate{.kind = Predicate::Kind::kCompareColumns,
                                               .column = Column(kA, 0),
                                               .other = Column(ColumnId{9}, 0)}},
                      .span = {}});
  ColumnId last = MaxColumnId(*reads_outer);
  const LogicalNodePtr clone = CloneWithFreshIds(reads_outer, last);
  EXPECT_EQ(FreeVariables(*clone), (std::vector<ColumnId>{ColumnId{9}}));
  EXPECT_EQ(FreeVariables(*clone), FreeVariables(*reads_outer));
}

// Every node kind, including the ones that define a column other than a scan's: a RowCount's own
// id, a dependent join's mark, and an aggregate call's result.
TEST(ColumnIdsTest, ACloneCoversEveryNodeThatDefinesAColumn) {
  // A RowCount defines its own id and has no input, so it is the only column minted.
  const LogicalNodePtr row_count = Node(RowCountNode{
      .table = std::make_shared<FakeTable>(AllTypesSchema(), 100), .id = ColumnId{20}});
  auto last = ColumnId{20};
  EXPECT_EQ(OutputIds(*CloneWithFreshIds(row_count, last)), (std::vector<ColumnId>{ColumnId{21}}));
  EXPECT_EQ(last, ColumnId{21});

  const LogicalNodePtr dependent = Node(DependentJoinNode{.kind = JoinKind::kSemi,
                                                          .left = Scan(),
                                                          .right = Scan({kD}),
                                                          .mark = ColumnId{30},
                                                          .span = {}});
  last = ColumnId{30};
  const std::vector<ColumnId> cloned = OutputIds(*CloneWithFreshIds(dependent, last));
  ASSERT_FALSE(cloned.empty());
  EXPECT_EQ(cloned.back(), last) << "the mark is minted too";
  EXPECT_EQ(std::ranges::find(cloned, ColumnId{30}), cloned.end());

  // An aggregate call's result, which is minted after its input's columns rather than before.
  const LogicalNodePtr global =
      Node(AggregateNode{.input = Scan(),
                         .aggregates = {AggregateCall{
                             .kind = AggKind::kSum, .arg = Column(kB, 1), .id = ColumnId{40}}},
                         .span = {}});
  last = ColumnId{40};
  const LogicalNodePtr cloned_global = CloneWithFreshIds(global, last);
  EXPECT_EQ(OutputIds(*cloned_global), (std::vector<ColumnId>{last}));
  EXPECT_EQ(std::to_underlying(last), 44) << "three scan columns, then the call";
  EXPECT_EQ(PositionMismatch(Plan(cloned_global)), std::nullopt);

  EXPECT_EQ(CloneWithFreshIds(nullptr, last), nullptr);
}

TEST(ColumnIdsDeathTest, BrokenPlansAbort) {
  EXPECT_DEATH(ResolvePositions(FilterPlan(Column(ColumnId{99}, 0))),
               "Filter: column #99 is not in its input");
  // Optimize is [[nodiscard]] since ADR 0023's unnesting pass; the abort is what is under test.
  EXPECT_DEATH(static_cast<void>(Optimize(FilterPlan(Column(ColumnId{99}, 0)))),
               "Filter: column #99 is not in its input");
  JoinNode keyless = Join(JoinKind::kSemi);
  keyless.keys.clear();
  EXPECT_DEATH(static_cast<void>(Optimize(Plan(Node(keyless)))), "Join: SEMI join without keys");
}

}  // namespace
}  // namespace antb1::plan
