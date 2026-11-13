#include "../unnesting.h"

#include <cstddef>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/result.h>
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/plan/explain.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/plan_options.h"
#include "antb1/plan/sql_status.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::BindSql;
using testing::MakeCatalog;

// The first dependent join of `node`, its right input rewritten by `f`, the rest as it was.
LogicalNodePtr WithRight(const LogicalNodePtr& node,
                         const std::function<LogicalNodePtr(const LogicalNodePtr&)>& f) {
  if (const auto* dependent = std::get_if<DependentJoinNode>(node.get())) {
    DependentJoinNode out = *dependent;
    out.right = f(out.right);
    return std::make_shared<const LogicalNode>(std::move(out));
  }
  std::vector<LogicalNodePtr> inputs = InputsOf(*node);
  for (LogicalNodePtr& input : inputs) {
    if (input != nullptr && FirstDependentJoin(input) != nullptr) {
      input = WithRight(input, f);
      return WithInputs(*node, std::move(inputs));
    }
  }
  return node;
}

// The first dependent join of `node`, its kind replaced.
LogicalNodePtr WithKind(const LogicalNodePtr& node, JoinKind kind) {
  if (const auto* dependent = std::get_if<DependentJoinNode>(node.get())) {
    DependentJoinNode out = *dependent;
    out.kind = kind;
    return std::make_shared<const LogicalNode>(std::move(out));
  }
  std::vector<LogicalNodePtr> inputs = InputsOf(*node);
  for (LogicalNodePtr& input : inputs) {
    if (input != nullptr && FirstDependentJoin(input) != nullptr) {
      input = WithKind(input, kind);
      return WithInputs(*node, std::move(inputs));
    }
  }
  return node;
}

// `node`'s correlated filter -- the first Filter over a Compute -- rewritten by `f`.
LogicalNodePtr WithFilter(const LogicalNodePtr& node,
                          const std::function<FilterNode(const FilterNode&)>& f) {
  if (const auto* filter = std::get_if<FilterNode>(node.get());
      filter != nullptr && std::holds_alternative<ComputeNode>(*filter->input)) {
    return std::make_shared<const LogicalNode>(f(*filter));
  }
  std::vector<LogicalNodePtr> inputs = InputsOf(*node);
  if (inputs.size() != 1) {
    return node;
  }
  inputs.front() = WithFilter(inputs.front(), f);
  return WithInputs(*node, std::move(inputs));
}

// `node`'s static aggregate, each call rewritten by `f`.
LogicalNodePtr WithAggregate(const LogicalNodePtr& node,
                             const std::function<AggregateCall(const AggregateCall&)>& f) {
  if (const auto* aggregate = std::get_if<AggregateNode>(node.get())) {
    AggregateNode out = *aggregate;
    for (AggregateCall& call : out.aggregates) {
      call = f(call);
    }
    return std::make_shared<const LogicalNode>(std::move(out));
  }
  std::vector<LogicalNodePtr> inputs = InputsOf(*node);
  if (inputs.size() != 1) {
    return node;
  }
  inputs.front() = WithAggregate(inputs.front(), f);
  return WithInputs(*node, std::move(inputs));
}

// A subtree as EXPLAIN shows a plan's nodes: one line per node, inputs indented below it.
std::string TreeText(const LogicalNodePtr& node, std::size_t depth = 0) {
  if (node == nullptr) {
    return "";
  }
  std::string text = std::string(2 * depth, ' ') + ExplainNode(*node) + "\n";
  for (const LogicalNodePtr& input : InputsOf(*node)) {
    text += TreeText(input, depth + 1);
  }
  return text;
}

constexpr std::string_view kExists =
    "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 AND u.i32 > 0)";
constexpr std::string_view kNotExists =
    "SELECT i16 FROM t WHERE NOT EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16)";
constexpr std::string_view kScalar =
    "SELECT i16 FROM t WHERE i32 > (SELECT MAX(u.i32) FROM u WHERE u.i64 = t.i64)";

// The refusal Unnest gives the bound `sql` with `change` applied to its plan, or "" where it
// answers.
std::string RefusalOf(std::string_view sql,
                      const std::function<LogicalNodePtr(const LogicalNodePtr&)>& change) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(sql, catalog);
  EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  if (!plan.ok()) {
    return "<bind error>";
  }
  const arrow::Result<LogicalNodePtr> out = Unnest(change(plan->root), kBuildSideRatio);
  if (out.ok()) {
    return "";
  }
  EXPECT_NE(GetSqlError(out.status()), nullptr) << out.status().ToString();
  return out.status().message();
}

// ADR 0041's decision 16: a dependent join is unnested by its kind's traits. A semi or anti join
// takes its correlated conjuncts (lift mode), a one-row join appends a static aggregate's row
// (value mode), and every other kind -- one whose join takes no residual, or that the binder never
// makes dependent -- keeps the one refusal it has always had.
TEST(UnnestingTest, EveryKindButSemiAntiAndOneRowIsRefused) {
  for (const JoinKind kind :
       {JoinKind::kInner, JoinKind::kLeft, JoinKind::kSemi, JoinKind::kAnti,
        JoinKind::kNullAwareAnti, JoinKind::kOneRow, JoinKind::kFull, JoinKind::kNullAwareMark}) {
    SCOPED_TRACE(ToString(kind));
    const std::string refusal =
        RefusalOf(kind == JoinKind::kOneRow ? kScalar : kExists,
                  [kind](const LogicalNodePtr& root) { return WithKind(root, kind); });
    switch (kind) {
      case JoinKind::kSemi:
      case JoinKind::kAnti:
      case JoinKind::kOneRow:
        EXPECT_EQ(refusal, "");
        break;
      default:
        EXPECT_EQ(refusal, "a correlated subquery is not supported");
    }
  }
}

// Decision 17: the descent has a rule per node type, and each mode declines the nodes it has no
// rule for yet with its shape message, whatever the node: one the binder never puts on the way
// (a LIMIT, a grouping, grouping sets, a join, a union, a set operation, a second dependent join,
// a bare leaf), or a mode's own misfit (an aggregate under EXISTS, a sort over a scalar's
// aggregate). Each node stands directly under the dependent join, above the block.
TEST(UnnestingTest, EachModeRefusesTheNodesItHasNoRuleFor) {
  const auto over = [](const std::function<LogicalNode(const LogicalNodePtr&)>& make) {
    return [make](const LogicalNodePtr& root) {
      return WithRight(root, [&make](const LogicalNodePtr& right) {
        return std::make_shared<const LogicalNode>(make(right));
      });
    };
  };
  const std::vector<std::pair<std::string, std::function<LogicalNode(const LogicalNodePtr&)>>>
      nodes = {
          {"a Limit",
           [](const LogicalNodePtr& in) {
             return LogicalNode(LimitNode{.input = in, .limit = 1, .offset = 0, .span = {}});
           }},
          {"a Sort",
           [](const LogicalNodePtr& in) {
             return LogicalNode(SortNode{.input = in, .keys = {}, .span = {}});
           }},
          {"an Aggregate",
           [](const LogicalNodePtr& in) {
             return LogicalNode(AggregateNode{.input = in, .aggregates = {}, .span = {}});
           }},
          {"a GroupAggregate",
           [](const LogicalNodePtr& in) {
             return LogicalNode(GroupAggregateNode{
                 .input = in, .keys = {}, .key_ids = {}, .aggregates = {}, .span = {}});
           }},
          {"a GroupingSets",
           [](const LogicalNodePtr& in) {
             return LogicalNode(GroupingSetsNode{.input = in,
                                                 .keys = {},
                                                 .key_ids = {},
                                                 .sets = {{}},
                                                 .aggregates = {},
                                                 .grouping = {},
                                                 .span = {}});
           }},
          {"a Join",
           [](const LogicalNodePtr& in) {
             ColumnId last = MaxColumnId(*in);
             return LogicalNode(JoinNode{.kind = JoinKind::kInner,
                                         .left = in,
                                         .right = CloneWithFreshIds(in, last),
                                         .keys = {},
                                         .residual = {},
                                         .build = BuildSide::kRight,
                                         .span = {}});
           }},
          {"a second DependentJoin",
           [](const LogicalNodePtr& in) {
             ColumnId last = MaxColumnId(*in);
             return LogicalNode(DependentJoinNode{.kind = JoinKind::kSemi,
                                                  .left = in,
                                                  .right = CloneWithFreshIds(in, last),
                                                  .span = {}});
           }},
          {"a SetOperation",
           [](const LogicalNodePtr& in) {
             ColumnId last = MaxColumnId(*in);
             return LogicalNode(SetOperationNode{.inputs = {in, CloneWithFreshIds(in, last)},
                                                 .columns = {},
                                                 .ids = {},
                                                 .formula = {},
                                                 .span = {}});
           }},
          {"a UnionAll",
           [](const LogicalNodePtr& in) {
             return LogicalNode(UnionAllNode{.inputs = {in}, .columns = {}, .ids = {}, .span = {}});
           }},
  };
  constexpr std::string_view kLift = "over a block that is not a plain FROM and WHERE";
  constexpr std::string_view kValue =
      "over a block that is not an ungrouped aggregate over its correlation";
  for (const auto& [name, make] : nodes) {
    SCOPED_TRACE(name);
    // Lift mode drops a Sort above the filter, as it drops the select list; anything else above
    // it is refused.
    const std::string exists = RefusalOf(kExists, over(make));
    const std::string not_exists = RefusalOf(kNotExists, over(make));
    if (name == "a Sort") {
      EXPECT_EQ(exists, "");
      EXPECT_EQ(not_exists, "");
    } else {
      EXPECT_EQ(exists, std::format("a correlated EXISTS {} is not supported", kLift));
      EXPECT_EQ(not_exists, std::format("a correlated NOT EXISTS {} is not supported", kLift));
    }
    // Value mode admits nothing over its static aggregate but the maps.
    EXPECT_EQ(RefusalOf(kScalar, over(make)),
              std::format("a correlated scalar subquery {} is not supported", kValue));
  }
  // A leaf in place of the block, and a join or a union over it: no correlated filter on the way.
  const Catalog catalog = MakeCatalog();
  auto leaf = BindSql("SELECT i16 FROM ok", catalog);
  ASSERT_TRUE(leaf.ok()) << leaf.status().ToString();
  const auto replaced = [&leaf](const LogicalNodePtr& root) {
    return WithRight(root, [&leaf](const LogicalNodePtr& right) {
      ColumnId last = MaxColumnId(*right);
      return CloneWithFreshIds(leaf->root, last);
    });
  };
  EXPECT_EQ(RefusalOf(kExists, replaced),
            std::format("a correlated EXISTS {} is not supported", kLift));
  EXPECT_EQ(RefusalOf(kScalar, replaced),
            std::format("a correlated scalar subquery {} is not supported", kValue));
  const auto counted = [](const LogicalNodePtr& root) {
    return WithRight(root, [](const LogicalNodePtr& /*right*/) {
      return std::make_shared<const LogicalNode>(
          RowCountNode{.table = nullptr, .table_name = "t", .id = ColumnId{900}, .span = {}});
    });
  };
  EXPECT_EQ(RefusalOf(kExists, counted),
            std::format("a correlated EXISTS {} is not supported", kLift));
  EXPECT_EQ(RefusalOf(kScalar, counted),
            std::format("a correlated scalar subquery {} is not supported", kValue));
}

// Decision 17's first row: each conjunct of the correlated filter is a kIsTrue over a column its
// Compute defines; any other is refused in each mode's words.
TEST(UnnestingTest, AConjunctTheFilterRuleDoesNotRecognizeIsRefused) {
  const auto unrecognized = [](const LogicalNodePtr& root) {
    return WithRight(root, [](const LogicalNodePtr& right) {
      return WithFilter(right, [](const FilterNode& filter) {
        FilterNode out = filter;
        out.predicates.front().kind = Predicate::Kind::kIsNotNull;
        return out;
      });
    });
  };
  EXPECT_EQ(RefusalOf(kExists, unrecognized),
            "a correlated EXISTS whose correlated conjunct the pass does not recognize is not "
            "supported");
  EXPECT_EQ(RefusalOf(kScalar, unrecognized),
            "a correlated scalar subquery whose correlated conjunct the pass does not recognize is "
            "not supported");
  // Two defects: the checks keep their order, the aggregate's calls before the conjuncts, so the
  // refusal is the calls'. The single value is the one call the rule refuses, and only a plan built
  // by hand puts one under a dependent join.
  const auto single_value = [&unrecognized](const LogicalNodePtr& root) {
    return WithRight(unrecognized(root), [](const LogicalNodePtr& right) {
      return WithAggregate(right, [](AggregateCall call) {
        call.kind = AggKind::kSingleValue;
        return call;
      });
    });
  };
  EXPECT_EQ(RefusalOf(kScalar, single_value),
            "a correlated scalar subquery whose aggregate is a single value is not supported");
}

// Decision 19: a static aggregate's empty values. COUNT in each spelling, and a sum of counts, is
// 0; every other call NULL; each of the call's type.
TEST(UnnestingTest, EmptyValuesFollowDecisionNineteen) {
  for (const AggKind kind :
       {AggKind::kCountStar, AggKind::kCount, AggKind::kSum, AggKind::kAvg, AggKind::kMin,
        AggKind::kMax, AggKind::kCountDistinct, AggKind::kSumCounts, AggKind::kStddevSamp,
        AggKind::kStddevPop, AggKind::kVarSamp, AggKind::kVarPop, AggKind::kSingleValue}) {
    const LogicalType type = kind == AggKind::kAvg ? LogicalType::kDouble : LogicalType::kBigInt;
    const EmptyValue value = EmptyValueOf(AggregateCall{.kind = kind, .arg = {}, .type = type});
    const Constant* constant = value.has_value() ? &*value : nullptr;
    ASSERT_NE(constant, nullptr);
    EXPECT_EQ(constant->type, type);
    const bool count = kind == AggKind::kCountStar || kind == AggKind::kCount ||
                       kind == AggKind::kCountDistinct || kind == AggKind::kSumCounts;
    if (count) {
      EXPECT_EQ(std::get<Int128>(constant->value), Int128{0});
    } else {
      EXPECT_TRUE(std::holds_alternative<std::monostate>(constant->value));
    }
  }
}

// ---- decision 20: the consumer proof ----

constexpr ColumnId kValue{50};  // the column the dependent join appends
constexpr ColumnId kOuter{51};  // an outer column

BoundColumn Bigint(ColumnId id) {
  return BoundColumn{.id = id, .name = "c", .type = LogicalType::kBigInt};
}

LogicalNode FilterOf(std::vector<Predicate> predicates) {
  return FilterNode{.input = nullptr, .predicates = std::move(predicates), .span = {}};
}

Predicate Compare(ColumnId column, CompareOp op, int64_t constant) {
  return Predicate{.kind = Predicate::Kind::kCompare,
                   .column = Bigint(column),
                   .op = op,
                   .constant = Constant{.type = LogicalType::kBigInt, .value = Int128{constant}}};
}

Predicate CompareColumns(ColumnId column, CompareOp op, ColumnId other) {
  return Predicate{.kind = Predicate::Kind::kCompareColumns,
                   .column = Bigint(column),
                   .other = Bigint(other),
                   .op = op};
}

JoinBack Proof(const std::vector<LogicalNode>& above_root_first, EmptyValue value) {
  std::vector<const LogicalNode*> above;
  above.reserve(above_root_first.size());
  for (const LogicalNode& node : above_root_first) {
    above.push_back(&node);
  }
  return ProveJoinBack(above, {{kValue, std::move(value)}});
}

EmptyValue NullValue() { return Constant{.type = LogicalType::kBigInt, .value = std::monostate{}}; }

// A comparison of the value is true for no row where the value is NULL, whatever its other side:
// the INNER join back loses no row SQL keeps.
TEST(UnnestingTest, TheProofAcceptsAComparisonOverAnEmptyValueOfNull) {
  EXPECT_EQ(Proof({FilterOf({CompareColumns(kOuter, CompareOp::kGt, kValue)})}, NullValue()),
            JoinBack::kInner);
  EXPECT_EQ(Proof({FilterOf({Compare(kValue, CompareOp::kLt, 3)})}, NullValue()), JoinBack::kInner);
  // Through a strict Compute: 2 * v is NULL where v is.
  const ExprPtr v = std::make_shared<const Expr>(
      Expr{.node = ColumnExpr{.id = kValue}, .type = LogicalType::kBigInt, .name = "v"});
  const ExprPtr two_v = std::make_shared<const Expr>(Expr{
      .node = ArithExpr{.op = ArithOp::kMultiply,
                        .left = std::make_shared<const Expr>(Expr{
                            .node = ConstantExpr{.value = Constant{.type = LogicalType::kBigInt,
                                                                   .value = Int128{2}}},
                            .type = LogicalType::kBigInt,
                            .name = "2"}),
                        .right = v},
      .type = LogicalType::kBigInt,
      .name = "2 * v"});
  constexpr ColumnId kTwoV{52};
  EXPECT_EQ(Proof({FilterOf({CompareColumns(kTwoV, CompareOp::kGt, kOuter)}),
                   ComputeNode{.input = nullptr, .exprs = {two_v}, .ids = {kTwoV}, .span = {}}},
                  NullValue()),
            JoinBack::kInner);
}

// What can pass the empty value, or reads none of it, leaves the join back LEFT: IS NULL and the
// DISTINCT FROM pair, a Compute that is not strict, a node on the way that is not a Compute, a
// Filter that reads no value, and nothing above at all.
TEST(UnnestingTest, TheProofRefusesWhatCanPassTheEmptyValue) {
  EXPECT_EQ(
      Proof({FilterOf({Predicate{.kind = Predicate::Kind::kIsNull, .column = Bigint(kValue)}})},
            NullValue()),
      JoinBack::kLeft);
  for (const Predicate::Kind kind :
       {Predicate::Kind::kIsDistinct, Predicate::Kind::kIsNotDistinct}) {
    EXPECT_EQ(Proof({FilterOf({Predicate{
                        .kind = kind, .column = Bigint(kValue), .other = Bigint(kOuter)}})},
                    NullValue()),
              JoinBack::kLeft);
  }
  const ExprPtr v = std::make_shared<const Expr>(
      Expr{.node = ColumnExpr{.id = kValue}, .type = LogicalType::kBigInt, .name = "v"});
  const ExprPtr coalesce = std::make_shared<const Expr>(
      Expr{.node = CaseExpr{.whens = {nullptr}, .thens = {v}, .otherwise = v},
           .type = LogicalType::kBigInt,
           .name = "COALESCE(v, v)"});
  constexpr ColumnId kCoalesced{52};
  EXPECT_EQ(
      Proof({FilterOf({Compare(kCoalesced, CompareOp::kGt, 0)}),
             ComputeNode{.input = nullptr, .exprs = {coalesce}, .ids = {kCoalesced}, .span = {}}},
            NullValue()),
      JoinBack::kLeft);
  EXPECT_EQ(
      Proof({FilterOf({Compare(kValue, CompareOp::kGt, 0)}),
             ProjectNode{.input = nullptr, .columns = {}, .constants = {}, .ids = {}, .span = {}}},
            NullValue()),
      JoinBack::kLeft);
  EXPECT_EQ(Proof({FilterOf({Compare(kValue, CompareOp::kGt, 0)}),
                   FilterOf({Compare(kOuter, CompareOp::kGt, 0)})},
                  NullValue()),
            JoinBack::kLeft);
  EXPECT_EQ(Proof({}, NullValue()), JoinBack::kLeft);
}

// An empty value that is no NULL -- COUNT's 0, which SQ6 answers -- is decided by the comparison
// over it: `v > 0` is false for 0, `v >= 0` true, and `v > o.x` depends on the outer row.
TEST(UnnestingTest, TheProofReadsAConstantEmptyValue) {
  const EmptyValue zero = Constant{.type = LogicalType::kBigInt, .value = Int128{0}};
  EXPECT_EQ(Proof({FilterOf({Compare(kValue, CompareOp::kGt, 0)})}, zero), JoinBack::kInner);
  EXPECT_EQ(Proof({FilterOf({Compare(kValue, CompareOp::kGe, 0)})}, zero), JoinBack::kLeft);
  EXPECT_EQ(Proof({FilterOf({CompareColumns(kValue, CompareOp::kGt, kOuter)})}, zero),
            JoinBack::kLeft);
}

// Every correlated scalar subquery of SUM, AVG, MIN or MAX under a comparison that rejects NULL
// proves its INNER join back, nested and stacked ones too; a dependent join with nothing above it --
// a plan only a test builds -- does not.
TEST(UnnestingTest, EveryAnsweredCorrelatedScalarPassesTheProof) {
  const Catalog catalog = MakeCatalog();
  for (const std::string_view sql : {
           kScalar,
           std::string_view("SELECT i16 FROM t WHERE i32 > (SELECT SUM(u.i32) * 2 FROM u WHERE "
                            "u.i64 = t.i64)"),
           std::string_view("SELECT i16 FROM t WHERE i32 > (SELECT MAX(u.i32) FROM u WHERE u.i64 = "
                            "t.i64) AND i16 < (SELECT MIN(u.i16) FROM u WHERE u.i32 = t.i32)"),
           std::string_view("SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 "
                            "AND u.i32 > (SELECT MAX(ok.i16) FROM ok WHERE ok.i16 = u.i16))"),
           std::string_view("SELECT i16 FROM t WHERE i32 > (SELECT MAX(u.i32) FROM u WHERE u.i64 = "
                            "t.i64 AND EXISTS (SELECT 1 FROM ok WHERE ok.i16 = u.i16))"),
       }) {
    SCOPED_TRACE(sql);
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << plan.status().ToString();
    UnnestReport report;
    const arrow::Result<LogicalNodePtr> out = Unnest(plan->root, kBuildSideRatio, &report);
    ASSERT_TRUE(out.ok()) << out.status().ToString();
    EXPECT_FALSE(report.join_backs.empty());
    for (const UnnestReport::Entry& entry : report.join_backs) {
      EXPECT_EQ(entry.join_back, JoinBack::kInner);
    }
  }
  // The dependent join alone, no consumer above it: the proof fails, and the join back is LEFT.
  auto plan = BindSql(kScalar, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  LogicalNodePtr alone = plan->root;
  while (!std::holds_alternative<DependentJoinNode>(*alone)) {
    alone = InputsOf(*alone).front();
  }
  UnnestReport report;
  const arrow::Result<LogicalNodePtr> out = Unnest(alone, kBuildSideRatio, &report);
  ASSERT_TRUE(out.ok()) << out.status().ToString();
  ASSERT_EQ(report.join_backs.size(), 1U);
  EXPECT_EQ(report.join_backs.front().join_back, JoinBack::kLeft);
  EXPECT_NE(TreeText(*out).find("Join LEFT build=right"), std::string::npos) << TreeText(*out);
}

// The plan Unnest makes of `sql`, as EXPLAIN shows it, and the verdict of each join back.
struct Unnested {
  std::string text;
  std::vector<JoinBack> join_backs;
};
Unnested UnnestedOf(std::string_view sql) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(sql, catalog);
  EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  if (!plan.ok()) {
    return {};
  }
  UnnestReport report;
  const arrow::Result<LogicalNodePtr> out = Unnest(plan->root, kBuildSideRatio, &report);
  EXPECT_TRUE(out.ok()) << sql << ": " << out.status().ToString();
  if (!out.ok()) {
    return {};
  }
  Unnested unnested{.text = TreeText(*out), .join_backs = {}};
  for (const UnnestReport::Entry& entry : report.join_backs) {
    unnested.join_backs.push_back(entry.join_back);
  }
  return unnested;
}

// Decision 20's proof decides the join back (ADR 0041's SQ6): INNER only where the comparison
// rejects the subquery's empty value. COUNT's 0 passes `= 0` and fails `> 0`; a strict combination
// of MAX keeps its NULL. The proof folds no constants, so `COUNT(*) + 1 > 1`, which rejects the 1
// the empty row gives, still joins back LEFT, as COALESCE and CASE items do: correct, only slower.
TEST(UnnestingTest, TheProofDecidesTheJoinBack) {
  struct Case {
    std::string_view sql;
    JoinBack join_back;
  };
  for (const Case& c : {
           Case{.sql = "SELECT i16 FROM t WHERE (SELECT COUNT(*) FROM u WHERE u.i64 = t.i64) > 0",
                .join_back = JoinBack::kInner},
           Case{.sql = "SELECT i16 FROM t WHERE (SELECT COUNT(*) FROM u WHERE u.i64 = t.i64) = 0",
                .join_back = JoinBack::kLeft},
           Case{.sql = "SELECT i16 FROM t WHERE i32 > (SELECT MAX(u.i32) * 2 FROM u WHERE u.i64 = "
                       "t.i64)",
                .join_back = JoinBack::kInner},
           Case{.sql = "SELECT i16 FROM t WHERE (SELECT COUNT(*) + 1 FROM u WHERE u.i64 = t.i64) > 1",
                .join_back = JoinBack::kLeft},
           Case{.sql = "SELECT i16 FROM t WHERE (SELECT COALESCE(SUM(u.i32), 0) FROM u WHERE u.i64 "
                       "= t.i64) > 0",
                .join_back = JoinBack::kLeft},
           Case{.sql = "SELECT i16 FROM t WHERE (SELECT CASE WHEN MAX(u.i32) > 0 THEN 1 ELSE 2 END "
                       "FROM u WHERE u.i64 = t.i64) = 1",
                .join_back = JoinBack::kLeft},
       }) {
    SCOPED_TRACE(c.sql);
    const Unnested unnested = UnnestedOf(c.sql);
    ASSERT_EQ(unnested.join_backs.size(), 1U) << unnested.text;
    EXPECT_EQ(unnested.join_backs.front(), c.join_back) << unnested.text;
    EXPECT_NE(unnested.text.find(c.join_back == JoinBack::kInner ? "Join INNER" : "Join LEFT"),
              std::string::npos)
        << unnested.text;
  }
}

// On a LEFT join back a count is computed under a fresh id and read above the join through
// COALESCE(fresh, 0) under its own, so the comparison above reads 0 for an outer row with no group;
// the join builds the aggregate, which it pads.
TEST(UnnestingTest, TheLeftJoinBackReadsACountThroughCoalesce) {
  const Unnested unnested =
      UnnestedOf("SELECT i16 FROM t WHERE (SELECT COUNT(*) FROM u WHERE u.i64 = t.i64) = 0");
  const std::string& text = unnested.text;
  const std::size_t coalesce = text.find("Compute COALESCE(count_star(), 0)");
  const std::size_t join = text.find("Join LEFT build=right");
  const std::size_t group = text.find("GroupAggregate");
  ASSERT_NE(coalesce, std::string::npos) << text;
  ASSERT_NE(join, std::string::npos) << text;
  ASSERT_NE(group, std::string::npos) << text;
  EXPECT_LT(coalesce, join) << "above the join: " << text;
  EXPECT_LT(join, group) << text;
}

// The select list moves above a LEFT join back, so a padded row computes it over the empty values:
// a constant item keeps its constant (1 over ORDER BY COUNT(*)), which the comparison then passes.
TEST(UnnestingTest, TheSelectItemMovesAboveALeftJoinBack) {
  const Unnested unnested = UnnestedOf(
      "SELECT i16 FROM t WHERE 1 = (SELECT 1 FROM u WHERE u.i64 = t.i64 ORDER BY COUNT(*))");
  const std::string& text = unnested.text;
  ASSERT_EQ(unnested.join_backs, std::vector<JoinBack>{JoinBack::kLeft}) << text;
  const std::size_t item = text.find("Compute 1");
  const std::size_t join = text.find("Join LEFT build=right");
  ASSERT_NE(item, std::string::npos) << text;
  ASSERT_NE(join, std::string::npos) << text;
  EXPECT_LT(item, join) << "the constant is computed above the join: " << text;
}

// A HAVING inside the block decides its value over the empty row too (decision 19): NULL where it
// rejects that row, the item where it keeps it, and not known otherwise. So COUNT(*) > 1 makes the
// value NULL there and `= 0` rejects it, INNER; COUNT(*) < 5 keeps the count's 0, which `= 0`
// passes, LEFT; a computed condition is not known, LEFT. Either way the HAVING is a CASE over the
// value, never a filter: a binding whose group it rejects still reads NULL (decision 17), so the
// condition's column stands above the group on both join backs. A HAVING folded to FALSE reads no
// column and rejects every row.
TEST(UnnestingTest, AHavingIsACaseOnBothJoinBacks) {
  struct Case {
    std::string_view having;
    JoinBack join_back;
  };
  for (const Case& c : {
           Case{.having = "COUNT(*) > 1", .join_back = JoinBack::kInner},
           Case{.having = "COUNT(*) < 5", .join_back = JoinBack::kLeft},
           Case{.having = "COUNT(*) % 2 = 0", .join_back = JoinBack::kLeft},
           Case{.having = "MAX(u.i16) > 100000", .join_back = JoinBack::kInner},
       }) {
    const std::string sql = std::format(
        "SELECT i16 FROM t WHERE (SELECT COUNT(*) FROM u WHERE u.i64 = t.i64 HAVING {}) = 0",
        c.having);
    SCOPED_TRACE(sql);
    const Unnested unnested = UnnestedOf(sql);
    ASSERT_EQ(unnested.join_backs, std::vector<JoinBack>{c.join_back}) << unnested.text;
    const std::size_t having = unnested.text.find("Compute HAVING");
    const std::size_t join = unnested.text.find(c.join_back == JoinBack::kInner ? "Join INNER"
                                                                                : "Join LEFT");
    const std::size_t group = unnested.text.find("GroupAggregate");
    ASSERT_NE(having, std::string::npos) << unnested.text;
    ASSERT_NE(join, std::string::npos) << unnested.text;
    ASSERT_NE(group, std::string::npos) << unnested.text;
    EXPECT_EQ(unnested.text.find("Filter \"count_star()\" >"), std::string::npos)
        << "no filter over the group: " << unnested.text;
    if (c.join_back == JoinBack::kInner) {
      EXPECT_LT(join, having) << "over the group, below the join: " << unnested.text;
    } else {
      EXPECT_LT(having, join) << "above the join: " << unnested.text;
    }
    EXPECT_LT(having, group) << unnested.text;
  }
}

// An uncorrelated value over the block's aggregate, from the slot after the aggregation, is a
// one-row join that moves up with the maps (decision 21): over the group below an INNER join back,
// above a LEFT one, where a binding without rows combines the empty value with it -- in a select
// item and in a HAVING alike.
TEST(UnnestingTest, AnUncorrelatedOneRowJoinMovesUpWithTheItem) {
  struct Case {
    std::string_view sql;
    JoinBack join_back;
  };
  for (const Case& c : {
           Case{.sql = "SELECT i16 FROM t WHERE i32 > (SELECT SUM(u.i32) + (SELECT MIN(i16) FROM "
                       "ok) FROM u WHERE u.i64 = t.i64)",
                .join_back = JoinBack::kInner},
           Case{.sql = "SELECT i16 FROM t WHERE i32 < (SELECT SUM(u.i32) FROM u WHERE u.i64 = "
                       "t.i64 HAVING SUM(u.i32) > (SELECT MIN(i16) FROM ok))",
                .join_back = JoinBack::kInner},
           Case{.sql = "SELECT i16 FROM t WHERE (SELECT COUNT(*) + (SELECT MIN(i16) FROM ok) FROM "
                       "u WHERE u.i64 = t.i64) = 1",
                .join_back = JoinBack::kLeft},
       }) {
    SCOPED_TRACE(c.sql);
    const Unnested unnested = UnnestedOf(c.sql);
    ASSERT_EQ(unnested.join_backs, std::vector<JoinBack>{c.join_back}) << unnested.text;
    const std::size_t one_row = unnested.text.find("Join ONE-ROW");
    const std::size_t join = unnested.text.find(c.join_back == JoinBack::kInner ? "Join INNER"
                                                                                : "Join LEFT");
    const std::size_t group = unnested.text.find("GroupAggregate");
    ASSERT_NE(one_row, std::string::npos) << unnested.text;
    ASSERT_NE(join, std::string::npos) << unnested.text;
    ASSERT_NE(group, std::string::npos) << unnested.text;
    if (c.join_back == JoinBack::kInner) {
      EXPECT_LT(join, one_row) << "over the group, below the join: " << unnested.text;
    } else {
      EXPECT_LT(one_row, join) << "above the join: " << unnested.text;
    }
    EXPECT_LT(one_row, group) << unnested.text;
  }
  // A one-row join whose right input reads an outer column has no rule: it would need unnesting on
  // both inputs (SQ8, SQ10).
  const auto correlated_one_row = [](const LogicalNodePtr& root) {
    return WithRight(root, [](const LogicalNodePtr& right) {
      return std::make_shared<const LogicalNode>(JoinNode{.kind = JoinKind::kOneRow,
                                                          .left = right,
                                                          .right = right,
                                                          .keys = {},
                                                          .residual = {},
                                                          .build = BuildSide::kRight,
                                                          .estimate = std::nullopt,
                                                          .span = {}});
    });
  };
  EXPECT_EQ(RefusalOf(kScalar, correlated_one_row),
            "a correlated scalar subquery over a block that is not an ungrouped aggregate over its "
            "correlation is not supported");
}

}  // namespace
}  // namespace antb1::plan
