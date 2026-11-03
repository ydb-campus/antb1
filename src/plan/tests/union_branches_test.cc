#include "../union_branches.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/plan/branch_truth.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

LogicalNodePtr Node(LogicalNode node) {
  return std::make_shared<const LogicalNode>(std::move(node));
}

// A Scan of AllTypesSchema's i16 and i32 as columns `first` and `first + 1`.
LogicalNodePtr Scan(std::uint32_t first) {
  return Node(ScanNode{.table = std::make_shared<testing::FakeTable>(testing::AllTypesSchema(), 10),
                       .table_name = "t" + std::to_string(first),
                       .fields = {0, 1},
                       .ids = {ColumnId{first}, ColumnId{first + 1}},
                       .span = {}});
}

BoundColumn Column(ColumnId id, LogicalType type, std::string name = "c") {
  return BoundColumn{.index = 0, .id = id, .name = std::move(name), .type = type};
}

Constant Number(Int128 value, LogicalType type = LogicalType::kInteger) {
  return Constant{.type = type, .value = value};
}

Constant Text(std::string value) {
  return Constant{.type = LogicalType::kVarchar, .value = std::move(value)};
}

// A Project over `input` of its column `column` (id `out`) and the constant `tag` (id `out + 1`).
LogicalNodePtr Tagged(LogicalNodePtr input, const BoundColumn& column, const Constant& tag,
                      std::uint32_t out) {
  return Node(ProjectNode{.input = std::move(input),
                          .columns = {column, Column(kNoColumnId, tag.type, "tag")},
                          .constants = {std::nullopt, tag},
                          .ids = {ColumnId{out}, ColumnId{out + 1}},
                          .span = {}});
}

// A Compute over `input` casting its column `id` (of type `from`) to `to`, as column `out`.
LogicalNodePtr Cast(LogicalNodePtr input, ColumnId id, LogicalType from, LogicalType to,
                    std::uint32_t out) {
  const auto operand =
      std::make_shared<const Expr>(Expr{.node = ColumnExpr{.index = 0, .id = id}, .type = from});
  return Node(ComputeNode{.input = std::move(input),
                          .exprs = {std::make_shared<const Expr>(
                              Expr{.node = CastExpr{.operand = operand}, .type = to})},
                          .ids = {ColumnId{out}},
                          .span = {}});
}

// Column `id`'s constant through the nodes that keep it: a Filter, a Sort, a Limit and a Compute
// that defines another column.
TEST(UnionBranchesTest, FindsAConstantThroughTheNodesThatKeepIt) {
  const LogicalNodePtr project =
      Tagged(Scan(1), Column(ColumnId{1}, LogicalType::kSmallInt), Text("a"), 10);
  const ColumnId tag{11};
  ASSERT_EQ(ConstantColumn(project, tag).value_or(Constant{}).value, Text("a").value);
  LogicalNodePtr top = Node(FilterNode{
      .input = project,
      .predicates = {Predicate{.kind = Predicate::Kind::kIsNotNull,
                               .column = Column(ColumnId{10}, LogicalType::kSmallInt)}}});
  top =
      Node(SortNode{.input = top, .keys = {SortKey{.column = Column(tag, LogicalType::kVarchar)}}});
  top = Node(LimitNode{.input = top, .limit = 3});
  top = Cast(top, ColumnId{10}, LogicalType::kSmallInt, LogicalType::kInteger, 20);
  const std::optional<Constant> found = ConstantColumn(top, tag);
  ASSERT_TRUE(found.has_value());
  const Constant value = found.value_or(Constant{});
  EXPECT_EQ(value.type, LogicalType::kVarchar);
  EXPECT_EQ(value.value, Text("a").value);
  EXPECT_EQ(ConstantColumn(top, ColumnId{10}), std::nullopt) << "a column, not a constant";
  EXPECT_EQ(ConstantColumn(top, ColumnId{20}), std::nullopt) << "a cast of a column";
}

// The NULL item of a branch is a NULL constant that the binder computes below the union (ADR
// 0037's NL3), and a WHERE above the union rules that branch out: NULL is never true.
TEST(UnionBranchesTest, FindsAndRulesOutTheNullItemOfABranch) {
  const Constant null{.type = LogicalType::kVarchar, .value = std::monostate{}};
  const LogicalNodePtr branch =
      Node(ComputeNode{.input = Scan(1),
                       .exprs = {std::make_shared<const Expr>(
                           Expr{.node = ConstantExpr{.value = null}, .type = null.type})},
                       .ids = {ColumnId{5}},
                       .span = {}});
  const std::optional<Constant> found = ConstantColumn(branch, ColumnId{5});
  ASSERT_TRUE(found.has_value());
  EXPECT_TRUE(std::holds_alternative<std::monostate>(found.value_or(Constant{}).value));
  const UnionAllNode node{
      .inputs = {Tagged(Scan(10), Column(ColumnId{10}, LogicalType::kSmallInt), Text("a"), 20),
                 branch},
      .columns = {{Column(ColumnId{21}, LogicalType::kVarchar)},
                  {Column(ColumnId{5}, LogicalType::kVarchar)}},
      .ids = {ColumnId{30}},
      .span = {}};
  const LogicalNodePtr rewritten = FilterIntoUnionBranches(Node(
      FilterNode{.input = Node(node),
                 .predicates = {Predicate{.kind = Predicate::Kind::kCompare,
                                          .column = Column(ColumnId{30}, LogicalType::kVarchar),
                                          .op = CompareOp::kNe,
                                          .constant = Text("b")}}}));
  const auto& out = std::get<UnionAllNode>(*rewritten);
  ASSERT_EQ(out.inputs.size(), 1U) << "the NULL branch is ruled out";
  EXPECT_EQ(out.inputs[0], node.inputs[0]);
}

// A constant reaches the union's type through the binder's widening casts, as they compute it:
// the same value in a wider integer, multiplied up into a DECIMAL.
TEST(UnionBranchesTest, WidensAConstantAsTheBindersCastsDo) {
  struct Case {
    Constant constant;
    LogicalType to;
    Int128 widened;
  };
  const LogicalType d122 = LogicalType::Decimal(12, 2);
  const LogicalType d32 = LogicalType::Decimal(3, 2);
  for (const Case& c : {
           Case{.constant = Number(-7, LogicalType::kSmallInt),
                .to = LogicalType::kInteger,
                .widened = -7},
           Case{.constant = Number(65'535, LogicalType::kUSmallInt),
                .to = LogicalType::kInteger,
                .widened = 65'535},
           Case{.constant = Number(7), .to = LogicalType::kBigInt, .widened = 7},
           Case{.constant = Number(7), .to = LogicalType::kHugeInt, .widened = 7},
           Case{.constant = Number(7), .to = d122, .widened = 700},
           Case{.constant = Number(15, LogicalType::Decimal(2, 1)), .to = d32, .widened = 150},
           Case{.constant = Number(PowerOfTen(37), LogicalType::kHugeInt),
                .to = LogicalType::Decimal(38, 0),
                .widened = PowerOfTen(37)},
       }) {
    SCOPED_TRACE(ToString(c.constant.type) + " to " + ToString(c.to));
    const LogicalNodePtr project =
        Node(ProjectNode{.input = Scan(1),
                         .columns = {Column(kNoColumnId, c.constant.type)},
                         .constants = {c.constant},
                         .ids = {ColumnId{10}},
                         .span = {}});
    const std::optional<Constant> found =
        ConstantColumn(Cast(project, ColumnId{10}, c.constant.type, c.to, 20), ColumnId{20});
    ASSERT_TRUE(found.has_value());
    const Constant value = found.value_or(Constant{});
    EXPECT_EQ(value.type, c.to);
    EXPECT_EQ(value.value, Constant{.value = c.widened}.value);
  }
}

// No constant where it is not certain: a Project's column, a narrowing cast, a cast of a constant
// rather than a column, a cast whose operand's type is not the constant's, and every node that
// ends the search.
TEST(UnionBranchesTest, FindsNoConstantWhereItIsUnsure) {
  const LogicalNodePtr bigint =
      Node(ProjectNode{.input = Scan(1),
                       .columns = {Column(kNoColumnId, LogicalType::kBigInt)},
                       .constants = {Number(7, LogicalType::kBigInt)},
                       .ids = {ColumnId{10}},
                       .span = {}});
  EXPECT_EQ(
      ConstantColumn(Cast(bigint, ColumnId{10}, LogicalType::kBigInt, LogicalType::kInteger, 20),
                     ColumnId{20}),
      std::nullopt)
      << "a narrowing";
  EXPECT_EQ(
      ConstantColumn(Cast(bigint, ColumnId{10}, LogicalType::kInteger, LogicalType::kBigInt, 20),
                     ColumnId{20}),
      std::nullopt)
      << "an operand typed otherwise than the constant";
  const auto constant_operand = std::make_shared<const Expr>(
      Expr{.node = ConstantExpr{.value = Number(7)}, .type = LogicalType::kInteger});
  const LogicalNodePtr cast_of_constant = Node(ComputeNode{
      .input = bigint,
      .exprs = {std::make_shared<const Expr>(
          Expr{.node = CastExpr{.operand = constant_operand}, .type = LogicalType::kBigInt})},
      .ids = {ColumnId{20}},
      .span = {}});
  EXPECT_EQ(ConstantColumn(cast_of_constant, ColumnId{20}), std::nullopt);

  const LogicalNodePtr scan = Scan(1);
  EXPECT_EQ(ConstantColumn(scan, ColumnId{1}), std::nullopt);
  const LogicalNodePtr aggregate = Node(
      AggregateNode{.input = bigint,
                    .aggregates = {AggregateCall{.kind = AggKind::kMax,
                                                 .arg = Column(ColumnId{10}, LogicalType::kBigInt),
                                                 .type = LogicalType::kBigInt,
                                                 .id = ColumnId{30}}}});
  EXPECT_EQ(ConstantColumn(aggregate, ColumnId{30}), std::nullopt) << "an aggregate";
  const LogicalNodePtr join =
      Node(JoinNode{.kind = JoinKind::kLeft, .left = Scan(40), .right = bigint});
  EXPECT_EQ(ConstantColumn(join, ColumnId{10}), std::nullopt) << "a LEFT JOIN pads NULLs";
  EXPECT_EQ(ConstantColumn(nullptr, ColumnId{10}), std::nullopt);
}

// Three branches over i16: tagged 'a', tagged 'b', and with i16 itself (no constant), as columns
// #71 (x) and #72 (tag) of the union.
UnionAllNode ThreeBranches() {
  const LogicalNodePtr a =
      Tagged(Scan(1), Column(ColumnId{1}, LogicalType::kSmallInt), Text("a"), 10);
  const LogicalNodePtr b =
      Tagged(Scan(3), Column(ColumnId{3}, LogicalType::kSmallInt), Text("b"), 20);
  const LogicalNodePtr c = Node(ProjectNode{.input = Scan(5),
                                            .columns = {Column(ColumnId{5}, LogicalType::kSmallInt),
                                                        Column(ColumnId{6}, LogicalType::kVarchar)},
                                            .constants = {},
                                            .ids = {ColumnId{30}, ColumnId{31}},
                                            .span = {}});
  return UnionAllNode{.inputs = {a, b, c},
                      .columns = {{Column(ColumnId{10}, LogicalType::kSmallInt),
                                   Column(ColumnId{11}, LogicalType::kVarchar)},
                                  {Column(ColumnId{20}, LogicalType::kSmallInt),
                                   Column(ColumnId{21}, LogicalType::kVarchar)},
                                  {Column(ColumnId{30}, LogicalType::kSmallInt),
                                   Column(ColumnId{31}, LogicalType::kVarchar)}},
                      .ids = {ColumnId{71}, ColumnId{72}},
                      .span = {}};
}

Predicate TagIs(CompareOp op, std::string tag) {
  return Predicate{.kind = Predicate::Kind::kCompare,
                   .column = Column(ColumnId{72}, LogicalType::kVarchar, "tag"),
                   .op = op,
                   .constant = Text(std::move(tag))};
}

TEST(UnionBranchesTest, ReadsTheBranchThroughTheUnionsColumns) {
  const UnionAllNode node = ThreeBranches();
  const Predicate is_a = TagIs(CompareOp::kEq, "a");
  EXPECT_EQ(TruthInBranch(node, 0, is_a), BranchTruth::kAlways);
  EXPECT_EQ(TruthInBranch(node, 1, is_a), BranchTruth::kNever);
  EXPECT_EQ(TruthInBranch(node, 2, is_a), BranchTruth::kUnknown) << "no constant in that branch";
  EXPECT_EQ(TruthInBranch(node, 3, is_a), BranchTruth::kUnknown) << "no such branch";
  Predicate foreign = is_a;
  foreign.column = Column(ColumnId{11}, LogicalType::kVarchar);
  EXPECT_EQ(TruthInBranch(node, 0, foreign), BranchTruth::kUnknown)
      << "a column the union does not output";
}

// Three branches over i16 tagged 'a', 'b' and 'c', as columns #71 (x) and #72 (tag).
UnionAllNode ThreeTagged() {
  return UnionAllNode{
      .inputs = {Tagged(Scan(1), Column(ColumnId{1}, LogicalType::kSmallInt), Text("a"), 10),
                 Tagged(Scan(3), Column(ColumnId{3}, LogicalType::kSmallInt), Text("b"), 20),
                 Tagged(Scan(5), Column(ColumnId{5}, LogicalType::kSmallInt), Text("c"), 30)},
      .columns = {{Column(ColumnId{10}, LogicalType::kSmallInt),
                   Column(ColumnId{11}, LogicalType::kVarchar)},
                  {Column(ColumnId{20}, LogicalType::kSmallInt),
                   Column(ColumnId{21}, LogicalType::kVarchar)},
                  {Column(ColumnId{30}, LogicalType::kSmallInt),
                   Column(ColumnId{31}, LogicalType::kVarchar)}},
      .ids = {ColumnId{71}, ColumnId{72}},
      .span = {}};
}

// A branch ruled out is gone, wherever it stands, and the others keep their order, their inputs and
// their columns, the condition decided in them; the union keeps its ids.
TEST(UnionBranchesTest, PrunesTheFirstTheMiddleAndTheLastBranch) {
  const UnionAllNode node = ThreeTagged();
  const LogicalNodePtr union_all = Node(node);
  struct Case {
    std::string ruled_out;
    std::vector<std::size_t> kept;
  };
  for (const Case& c :
       {Case{.ruled_out = "a", .kept = {1, 2}}, Case{.ruled_out = "b", .kept = {0, 2}},
        Case{.ruled_out = "c", .kept = {0, 1}}}) {
    SCOPED_TRACE("tag <> '" + c.ruled_out + "'");
    const LogicalNodePtr rewritten = FilterIntoUnionBranches(
        Node(FilterNode{.input = union_all, .predicates = {TagIs(CompareOp::kNe, c.ruled_out)}}));
    const auto& out = std::get<UnionAllNode>(*rewritten);
    EXPECT_EQ(out.ids, node.ids);
    ASSERT_EQ(out.inputs.size(), c.kept.size());
    ASSERT_EQ(out.columns.size(), c.kept.size());
    for (std::size_t i = 0; i < c.kept.size(); ++i) {
      const std::size_t k = c.kept[i];
      EXPECT_EQ(out.columns[i][0].id, node.columns[k][0].id);
      EXPECT_EQ(out.inputs[i], node.inputs[k]) << "decided: the branch is as it was";
    }
  }
}

// What a branch's constants do not decide goes on top of that branch, read in the branch's own
// column, with no set of needed columns (Prune computes it).
TEST(UnionBranchesTest, PutsWhatItCannotDecideOnTopOfTheBranch) {
  const UnionAllNode node = ThreeBranches();
  const LogicalNodePtr rewritten = FilterIntoUnionBranches(
      Node(FilterNode{.input = Node(node), .predicates = {TagIs(CompareOp::kGt, "a")}}));
  const auto& out = std::get<UnionAllNode>(*rewritten);
  ASSERT_EQ(out.inputs.size(), 2U) << "'a' > 'a' rules the first out";
  EXPECT_EQ(out.inputs[0], node.inputs[1]) << "'b' > 'a': decided";
  const auto& filter = std::get<FilterNode>(*out.inputs[1]);
  EXPECT_EQ(filter.input, node.inputs[2]);
  ASSERT_EQ(filter.predicates.size(), 1U);
  const BoundColumn column = filter.predicates[0].column.value_or(BoundColumn{});
  EXPECT_EQ(column.id, ColumnId{31}) << "the branch's own column";
  EXPECT_EQ(column.type, LogicalType::kVarchar);
  EXPECT_EQ(filter.needed_above, std::nullopt);
}

// A comparison of two columns goes on top of each branch with both its sides read in that
// branch's own columns.
TEST(UnionBranchesTest, MovesAComparisonOfTwoColumnsWithBothItsSides) {
  const auto branch = [](std::uint32_t first, std::uint32_t out) {
    return Node(ProjectNode{.input = Scan(first),
                            .columns = {Column(ColumnId{first}, LogicalType::kSmallInt),
                                        Column(ColumnId{first + 1}, LogicalType::kSmallInt)},
                            .constants = {},
                            .ids = {ColumnId{out}, ColumnId{out + 1}},
                            .span = {}});
  };
  const UnionAllNode node{.inputs = {branch(1, 10), branch(3, 20)},
                          .columns = {{Column(ColumnId{10}, LogicalType::kSmallInt, "x"),
                                       Column(ColumnId{11}, LogicalType::kSmallInt, "y")},
                                      {Column(ColumnId{20}, LogicalType::kSmallInt, "x"),
                                       Column(ColumnId{21}, LogicalType::kSmallInt, "y")}},
                          .ids = {ColumnId{71}, ColumnId{72}},
                          .span = {}};
  const Predicate x_above_y{.kind = Predicate::Kind::kCompareColumns,
                            .column = Column(ColumnId{71}, LogicalType::kSmallInt, "x"),
                            .other = Column(ColumnId{72}, LogicalType::kSmallInt, "y"),
                            .op = CompareOp::kGt};
  const LogicalNodePtr rewritten =
      FilterIntoUnionBranches(Node(FilterNode{.input = Node(node), .predicates = {x_above_y}}));
  const auto& out = std::get<UnionAllNode>(*rewritten);
  ASSERT_EQ(out.inputs.size(), 2U);
  for (std::size_t k = 0; k < out.inputs.size(); ++k) {
    SCOPED_TRACE("branch " + std::to_string(k));
    const auto& filter = std::get<FilterNode>(*out.inputs[k]);
    EXPECT_EQ(filter.input, node.inputs[k]);
    ASSERT_EQ(filter.predicates.size(), 1U);
    const Predicate& moved = filter.predicates[0];
    EXPECT_EQ(moved.kind, Predicate::Kind::kCompareColumns);
    EXPECT_EQ(moved.op, CompareOp::kGt);
    EXPECT_EQ(moved.column.value_or(BoundColumn{}).id, node.columns[k][0].id);
    EXPECT_EQ(moved.other.value_or(BoundColumn{}).id, node.columns[k][1].id);
  }
}

// None left: FALSE over a union of the first branch. Running the pass again changes nothing.
TEST(UnionBranchesTest, AUnionWithNoBranchLeftReadsNothing) {
  UnionAllNode node = ThreeBranches();
  node.inputs.pop_back();
  node.columns.pop_back();
  const LogicalNodePtr rewritten = FilterIntoUnionBranches(
      Node(FilterNode{.input = Node(node), .predicates = {TagIs(CompareOp::kEq, "z")}}));
  const auto& filter = std::get<FilterNode>(*rewritten);
  ASSERT_EQ(filter.predicates.size(), 1U);
  EXPECT_EQ(filter.predicates[0].kind, Predicate::Kind::kFalse);
  const auto& one = std::get<UnionAllNode>(*filter.input);
  ASSERT_EQ(one.inputs.size(), 1U);
  EXPECT_EQ(one.inputs[0], node.inputs[0]);
  EXPECT_EQ(one.ids, node.ids);
  EXPECT_EQ(FilterIntoUnionBranches(rewritten), rewritten) << "a FALSE filter stays as it is";
}

// A filter that reads a column the union does not output, or that holds a FALSE, is left alone.
TEST(UnionBranchesTest, LeavesAFilterItCannotMove) {
  const LogicalNodePtr union_all = Node(ThreeBranches());
  Predicate foreign = TagIs(CompareOp::kEq, "a");
  foreign.column = Column(ColumnId{11}, LogicalType::kVarchar);
  for (const std::vector<Predicate>& predicates :
       {std::vector<Predicate>{foreign},
        std::vector<Predicate>{TagIs(CompareOp::kEq, "a"),
                               Predicate{.kind = Predicate::Kind::kFalse}},
        std::vector<Predicate>{}}) {
    const LogicalNodePtr filter = Node(FilterNode{.input = union_all, .predicates = predicates});
    EXPECT_EQ(FilterIntoUnionBranches(filter), filter);
  }
}

// A union directly under a branch has no constant of its own, so the condition goes on top of it,
// and from there on into its branches: the outer union keeps that branch alone, and the inner one
// keeps its 'a' branch (decided) and the one without a constant (filtered).
TEST(UnionBranchesTest, ConditionsReachAUnionDirectlyUnderABranch) {
  const LogicalNodePtr inner = Node(ThreeBranches());
  const LogicalNodePtr other =
      Tagged(Scan(7), Column(ColumnId{7}, LogicalType::kSmallInt), Text("b"), 40);
  const LogicalNodePtr outer =
      Node(UnionAllNode{.inputs = {inner, other},
                        .columns = {{Column(ColumnId{71}, LogicalType::kSmallInt),
                                     Column(ColumnId{72}, LogicalType::kVarchar)},
                                    {Column(ColumnId{40}, LogicalType::kSmallInt),
                                     Column(ColumnId{41}, LogicalType::kVarchar)}},
                        .ids = {ColumnId{81}, ColumnId{82}},
                        .span = {}});
  const LogicalNodePtr rewritten = FilterIntoUnionBranches(Node(FilterNode{
      .input = outer,
      .predicates = {Predicate{.kind = Predicate::Kind::kCompare,
                               .column = Column(ColumnId{82}, LogicalType::kVarchar, "tag"),
                               .op = CompareOp::kEq,
                               .constant = Text("a")}}}));
  const auto& top = std::get<UnionAllNode>(*rewritten);
  ASSERT_EQ(top.inputs.size(), 1U) << "the outer 'b' branch is ruled out";
  const auto& kept = std::get<UnionAllNode>(*top.inputs[0]);
  const auto& original = std::get<UnionAllNode>(*inner);
  ASSERT_EQ(kept.inputs.size(), 2U) << "the inner 'b' branch is ruled out";
  EXPECT_EQ(kept.inputs[0], original.inputs[0]);
  EXPECT_EQ(std::get<FilterNode>(*kept.inputs[1]).input, original.inputs[2]);
  EXPECT_EQ(FilterIntoUnionBranches(rewritten), rewritten) << "nothing more to move";
}

}  // namespace
}  // namespace antb1::plan
