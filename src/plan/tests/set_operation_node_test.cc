// The set-operation node (docs/adr/0039-set-operations.md) in every pass that walks a plan: its
// formula's checks, starters and counted inputs; its output and reads, ResolvePositions and
// PositionMismatch, a clone's fresh ids, EXPLAIN, the optimizer's pruning, the join order's
// estimate, and the shared sub-plans, which never hold it nor anything below it.

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

BoundColumn Column(ColumnId id, std::string name, LogicalType type = LogicalType::kSmallInt) {
  return BoundColumn{.index = 0, .id = id, .name = std::move(name), .type = type};
}

SetTerm In(std::size_t input) { return SetTerm{.kind = SetTermKind::kInput, .input = input}; }
SetTerm Op(SetTermKind kind, std::size_t operands = 2) {
  return SetTerm{.kind = kind, .input = 0, .operands = operands};
}

// A scan of fields 0 to 3 (i16, i32, i64, u16) as columns first .. first + 3, of `rows` rows.
LogicalNodePtr Scan(std::uint32_t first, std::optional<int64_t> rows = 100,
                    std::shared_ptr<FakeTable> table = nullptr) {
  if (table == nullptr) {
    table = std::make_shared<FakeTable>(AllTypesSchema(), rows);
  }
  return Node(ScanNode{
      .table = std::move(table),
      .table_name = "t",
      .fields = {0, 1, 2, 3},
      .ids = {ColumnId{first}, ColumnId{first + 1}, ColumnId{first + 2}, ColumnId{first + 3}},
      .span = {}});
}

// `formula` over scans of columns 1.., 11.., 21.. (as many as it reads), each read at its i16 and
// i32: the node's columns #90 and #91. Every index is wrong (0), for ResolvePositions to set.
SetOperationNode Over(std::vector<SetTerm> formula, std::vector<std::optional<int64_t>> rows = {}) {
  std::size_t inputs = 0;
  for (const SetTerm& term : formula) {
    inputs += term.kind == SetTermKind::kInput ? 1 : 0;
  }
  SetOperationNode node{.ids = {ColumnId{90}, ColumnId{91}}, .formula = std::move(formula)};
  for (std::size_t k = 0; k < inputs; ++k) {
    const auto first = static_cast<std::uint32_t>(1 + (10 * k));
    node.inputs.push_back(Scan(first, k < rows.size() ? rows[k] : std::optional<int64_t>(100)));
    node.columns.push_back({Column(ColumnId{first}, "i16"),
                            Column(ColumnId{first + 1}, "i32", LogicalType::kInteger)});
  }
  return node;
}

LogicalPlan Plan(LogicalNodePtr root) {
  std::vector<OutputColumn> output;
  for (const ColumnId id : OutputIds(*root)) {
    output.push_back(OutputColumn{.name = "c", .type = LogicalType::kBigInt, .id = id});
  }
  return LogicalPlan{.root = std::move(root), .output = std::move(output)};
}

// ---- the formula ----

TEST(SetOperationNodeTest, CheckSetFormulaWantsOneTreeOverTheInputsInOrder) {
  EXPECT_EQ(CheckSetFormula({In(0), In(1), Op(SetTermKind::kUnion)}, 2), std::nullopt);
  EXPECT_EQ(CheckSetFormula({In(0), Op(SetTermKind::kUnion, 1)}, 1), std::nullopt)
      << "one input, still deduplicated";
  EXPECT_NE(CheckSetFormula({}, 0), std::nullopt);
  EXPECT_NE(CheckSetFormula({In(0), In(1)}, 2), std::nullopt) << "two roots";
  EXPECT_NE(CheckSetFormula({In(0)}, 1), std::nullopt) << "an input as the root";
  EXPECT_NE(CheckSetFormula({In(1), In(0), Op(SetTermKind::kUnion)}, 2), std::nullopt);
  EXPECT_NE(CheckSetFormula({In(0), In(1), Op(SetTermKind::kUnion)}, 3), std::nullopt);
  EXPECT_NE(CheckSetFormula({In(0), In(1), Op(SetTermKind::kUnion, 3)}, 2), std::nullopt);
  EXPECT_NE(CheckSetFormula({In(0), In(1), Op(SetTermKind::kUnion, 0)}, 2), std::nullopt);
}

// The starters and the counted inputs of ADR 0039's examples.
TEST(SetOperationNodeTest, StartersAndCountedInputs) {
  struct Case {
    std::vector<SetTerm> formula;
    std::vector<bool> starters;
    std::vector<bool> counted;
    std::string text;
  };
  const std::vector<Case> cases = {
      // A INTERSECT B INTERSECT C: A alone inserts.
      {.formula = {In(0), In(1), In(2), Op(SetTermKind::kIntersect, 3)},
       .starters = {true, false, false},
       .counted = {false, false, false},
       .text = "0 INTERSECT 1 INTERSECT 2"},
      // (A UNION B) INTERSECT C: both of the first operand's.
      {.formula = {In(0), In(1), Op(SetTermKind::kUnion), In(2), Op(SetTermKind::kIntersect)},
       .starters = {true, true, false},
       .counted = {false, false, false},
       .text = "(0 UNION 1) INTERSECT 2"},
      // A UNION (B INTERSECT C)
      {.formula = {In(0), In(1), In(2), Op(SetTermKind::kIntersect), Op(SetTermKind::kUnion)},
       .starters = {true, true, false},
       .counted = {false, false, false},
       .text = "0 UNION (1 INTERSECT 2)"},
      // A EXCEPT (B EXCEPT C): C's bit matters only through B, yet B is no starter.
      {.formula = {In(0), In(1), In(2), Op(SetTermKind::kExcept), Op(SetTermKind::kExcept)},
       .starters = {true, false, false},
       .counted = {false, false, false},
       .text = "0 EXCEPT (1 EXCEPT 2)"},
      // (A INTERSECT ALL B) EXCEPT ALL C: counts everywhere.
      {.formula = {In(0), In(1), Op(SetTermKind::kIntersectAll), In(2),
                   Op(SetTermKind::kExceptAll)},
       .starters = {true, false, false},
       .counted = {true, true, true},
       .text = "(0 INTERSECT ALL 1) EXCEPT ALL 2"},
      // (A EXCEPT ALL B) UNION C: an EXCEPT ALL needs counts even for presence.
      {.formula = {In(0), In(1), Op(SetTermKind::kExceptAll), In(2), Op(SetTermKind::kUnion)},
       .starters = {true, false, true},
       .counted = {true, true, false},
       .text = "(0 EXCEPT ALL 1) UNION 2"},
      // (A INTERSECT ALL B) UNION C: presence only, so bits.
      {.formula = {In(0), In(1), Op(SetTermKind::kIntersectAll), In(2), Op(SetTermKind::kUnion)},
       .starters = {true, false, true},
       .counted = {false, false, false},
       .text = "(0 INTERSECT ALL 1) UNION 2"},
      // An ALL root reads counts.
      {.formula = {In(0), In(1), Op(SetTermKind::kIntersectAll)},
       .starters = {true, false},
       .counted = {true, true},
       .text = "0 INTERSECT ALL 1"},
  };
  for (const Case& c : cases) {
    const std::size_t inputs = c.starters.size();
    ASSERT_EQ(CheckSetFormula(c.formula, inputs), std::nullopt) << c.text;
    EXPECT_EQ(SetStarters(c.formula, inputs), c.starters) << c.text;
    EXPECT_EQ(SetCounted(c.formula, inputs), c.counted) << c.text;
    EXPECT_EQ(SetFormulaText(c.formula), c.text);
  }
}

// ---- the visitors ----

TEST(SetOperationNodeTest, OutputsNewColumnsAndReadsEveryInputsColumns) {
  const SetOperationNode node = Over({In(0), In(1), Op(SetTermKind::kExcept)});
  EXPECT_EQ(NodeName(LogicalNode(node)), "SetOperation");
  EXPECT_EQ(InputsOf(LogicalNode(node)).size(), 2U);
  EXPECT_EQ(OutputIds(LogicalNode(node)), (std::vector<ColumnId>{ColumnId{90}, ColumnId{91}}));
  EXPECT_EQ(FreeVariables(LogicalNode(node)), std::vector<ColumnId>{});
  EXPECT_EQ(MaxColumnId(LogicalNode(node)), ColumnId{91});
  const LogicalNodePtr copy = WithInputs(LogicalNode(node), {Scan(1), Scan(11)});
  EXPECT_EQ(std::get<SetOperationNode>(*copy).formula.size(), 3U);
}

TEST(SetOperationNodeTest, ResolvePositionsSetsEachInputsColumns) {
  const LogicalPlan resolved =
      ResolvePositions(Plan(Node(Over({In(0), In(1), Op(SetTermKind::kUnion)}))));
  const auto& node = std::get<SetOperationNode>(*resolved.root);
  EXPECT_EQ(node.columns[1][0].index, 0);
  EXPECT_EQ(node.columns[1][1].index, 1);
  EXPECT_EQ(PositionMismatch(resolved), std::nullopt);
}

TEST(SetOperationNodeTest, PositionMismatchNamesABrokenNode) {
  const auto broken = [](auto change) {
    SetOperationNode node = Over({In(0), In(1), Op(SetTermKind::kUnion)});
    change(node);
    return PositionMismatch(Plan(Node(std::move(node))));
  };
  EXPECT_EQ(broken([](SetOperationNode& n) { n.formula.pop_back(); }),
            "SetOperation: the formula leaves 2 terms, not one root");
  EXPECT_EQ(broken([](SetOperationNode& n) { n.columns[1][1].type = LogicalType::kBigInt; }),
            "SetOperation: column 1 is INTEGER in input 0 and BIGINT in input 1");
  EXPECT_EQ(broken([](SetOperationNode& n) { n.columns.pop_back(); }),
            "SetOperation: 1 column lists for 2 inputs");
  EXPECT_EQ(broken([](SetOperationNode& n) { n.ids[1] = ColumnId{1}; }),
            "SetOperation: column #1 is defined twice");
}

TEST(SetOperationNodeTest, ACloneMintsEveryColumnItDefines) {
  ColumnId last{91};
  const LogicalNodePtr clone =
      CloneWithFreshIds(Node(Over({In(0), In(1), Op(SetTermKind::kIntersect)})), last);
  for (const ColumnId id : OutputIds(*clone)) {
    EXPECT_GT(std::to_underlying(id), 91U);
  }
  EXPECT_EQ(PositionMismatch(ResolvePositions(Plan(clone))), std::nullopt);
}

TEST(SetOperationNodeTest, ExplainShowsTheFormulaByInput) {
  EXPECT_EQ(ExplainNode(LogicalNode(
                Over({In(0), In(1), In(2), Op(SetTermKind::kIntersect), Op(SetTermKind::kUnion)}))),
            "SetOperation inputs=3 formula=(0 UNION (1 INTERSECT 2)) columns=[i16, i32]");
}

// Every column is a key of the deduplication, so each input's scan keeps the fields the node reads,
// even with nothing above reading them.
TEST(SetOperationNodeTest, PruningKeepsEveryColumn) {
  ProjectNode project{.input = Node(Over({In(0), In(1), Op(SetTermKind::kExcept)})),
                      .columns = {Column(ColumnId{90}, "i16")},
                      .ids = {ColumnId{95}}};
  const LogicalPlan optimized = Optimized(ResolvePositions(Plan(Node(std::move(project)))));
  ASSERT_EQ(PositionMismatch(optimized), std::nullopt);
  const auto& project_out = std::get<ProjectNode>(*optimized.root);
  const auto& node = std::get<SetOperationNode>(*project_out.input);
  for (const LogicalNodePtr& input : node.inputs) {
    EXPECT_EQ(std::get<ScanNode>(*input).fields, (std::vector<int>{0, 1}));
  }
  EXPECT_EQ(node.ids.size(), 2U);
}

// A UNION the sum, an INTERSECT the smallest operand, an EXCEPT its first; unknown where an operand
// it reads is unknown, but an INTERSECT bounded by its known operands.
TEST(SetOperationNodeTest, EstimatesFollowTheFormula) {
  const std::vector<std::optional<int64_t>> rows = {100, 40};
  EXPECT_EQ(EstimateRows(LogicalNode(Over({In(0), In(1), Op(SetTermKind::kUnion)}, rows))),
            std::optional<int64_t>(140));
  EXPECT_EQ(EstimateRows(LogicalNode(Over({In(0), In(1), Op(SetTermKind::kIntersect)}, rows))),
            std::optional<int64_t>(40));
  EXPECT_EQ(EstimateRows(LogicalNode(Over({In(0), In(1), Op(SetTermKind::kExceptAll)}, rows))),
            std::optional<int64_t>(100));
  const std::vector<std::optional<int64_t>> unknown = {std::nullopt, 40};
  EXPECT_EQ(EstimateRows(LogicalNode(Over({In(0), In(1), Op(SetTermKind::kUnion)}, unknown))),
            std::nullopt);
  EXPECT_EQ(EstimateRows(LogicalNode(Over({In(0), In(1), Op(SetTermKind::kExcept)}, unknown))),
            std::nullopt);
  EXPECT_EQ(EstimateRows(LogicalNode(Over({In(0), In(1), Op(SetTermKind::kIntersect)}, unknown))),
            std::optional<int64_t>(40));
}

// Two identical set operations under a join: one group of the nodes themselves, keyed (exec
// declines to hold it, being no aggregate), and nothing below them -- their scans repeat too, but
// nothing below a set operation is collected (ADR 0039, as below a union).
TEST(SetOperationNodeTest, IsKeyedAndNothingBelowItIsShared) {
  const auto table = std::make_shared<FakeTable>(AllTypesSchema(), 100);
  const auto over = [&table](std::uint32_t first, std::uint32_t own) {
    SetOperationNode node{.ids = {ColumnId{own}, ColumnId{own + 1}},
                          .formula = {In(0), In(1), Op(SetTermKind::kIntersect)}};
    for (std::uint32_t k = 0; k < 2; ++k) {
      const std::uint32_t at = first + (10 * k);
      node.inputs.push_back(Scan(at, 100, table));
      node.columns.push_back(
          {Column(ColumnId{at}, "i16"), Column(ColumnId{at + 1}, "i32", LogicalType::kInteger)});
    }
    return Node(std::move(node));
  };
  const LogicalPlan plan = ResolvePositions(Plan(Node(JoinNode{
      .kind = JoinKind::kInner,
      .left = over(1, 50),
      .right = over(101, 150),
      .keys = {JoinKey{.left = Column(ColumnId{50}, "i16"), .right = Column(ColumnId{150}, "i16")}},
      .residual = {},
      .build = BuildSide::kRight})));
  const std::vector<SharedSubPlan> shared = SharedSubPlans(plan);
  ASSERT_EQ(shared.size(), 1U);
  for (const LogicalNodePtr& member : shared.front().members) {
    EXPECT_TRUE(std::holds_alternative<SetOperationNode>(*member)) << ExplainNode(*member);
  }
}

}  // namespace
}  // namespace antb1::plan
