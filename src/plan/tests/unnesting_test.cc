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

#include "../join_order.h"
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

// The first dependent join of `node`, its kind replaced, and its mark (none by default).
LogicalNodePtr WithKind(const LogicalNodePtr& node, JoinKind kind, ColumnId mark = kNoColumnId) {
  if (const auto* dependent = std::get_if<DependentJoinNode>(node.get())) {
    DependentJoinNode out = *dependent;
    out.kind = kind;
    out.mark = mark;
    return std::make_shared<const LogicalNode>(std::move(out));
  }
  std::vector<LogicalNodePtr> inputs = InputsOf(*node);
  for (LogicalNodePtr& input : inputs) {
    if (input != nullptr && FirstDependentJoin(input) != nullptr) {
      input = WithKind(input, kind, mark);
      return WithInputs(*node, std::move(inputs));
    }
  }
  return node;
}

// The first dependent join of `node` made a mark (ADR 0041's SQ8), under a fresh id: the dependent
// join the binder plants for a correlated EXISTS under OR, NOT or CASE.
LogicalNodePtr AsMark(const LogicalNodePtr& node) {
  return WithKind(node, JoinKind::kMark, ColumnId{std::to_underlying(MaxColumnId(*node)) + 1});
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

// ADR 0041's decision 16: a dependent join is unnested by its kind's traits. A semi, anti or mark
// join takes its correlated conjuncts (lift mode), a one-row join appends a static aggregate's row
// (value mode), and every other kind -- one whose join takes no residual, or that the binder never
// makes dependent -- keeps the one refusal it has always had.
TEST(UnnestingTest, EveryKindButSemiAntiMarkAndOneRowIsRefused) {
  for (const JoinKind kind : {JoinKind::kInner, JoinKind::kLeft, JoinKind::kSemi, JoinKind::kAnti,
                              JoinKind::kNullAwareAnti, JoinKind::kOneRow, JoinKind::kFull,
                              JoinKind::kNullAwareMark, JoinKind::kMark}) {
    SCOPED_TRACE(ToString(kind));
    const std::string refusal = RefusalOf(
        kind == JoinKind::kOneRow ? kScalar : kExists, [kind](const LogicalNodePtr& root) {
          return kind == JoinKind::kMark ? AsMark(root) : WithKind(root, kind);
        });
    switch (kind) {
      case JoinKind::kSemi:
      case JoinKind::kAnti:
      case JoinKind::kOneRow:
      case JoinKind::kMark:
        EXPECT_EQ(refusal, "");
        break;
      default:
        EXPECT_EQ(refusal, "a correlated subquery is not supported");
    }
  }
}

TEST(UnnestingTest, WindowInCorrelatedInputIsAlwaysABarrier) {
  for (const std::string_view text : {kExists, kNotExists, kScalar}) {
    const std::string refusal = RefusalOf(text, [](const LogicalNodePtr& root) {
      return WithRight(root, [](const LogicalNodePtr& input) {
        return std::make_shared<const LogicalNode>(WindowNode{.input = input});
      });
    });
    EXPECT_NE(refusal.find("W11g/SQ17"), std::string::npos) << refusal;
  }
}

// ADR 0041's SQ8: a dependent mark is lifted as a semi join is -- an `=` key, an IS NOT DISTINCT
// FROM key (NULL-equal), a residual over both sides and an outer-only one (a residual, never a
// filter on the outer rows, whose marks it would lose), the inner-only conjunct left below -- into
// a mark join building on its right input. It keeps the mark's id, so that what reads the mark
// above still finds it, and every left row: its estimate is the left input's.
TEST(UnnestingTest, ALiftedMarkKeepsItsIdKeysAndResiduals) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 AND "
      "u.i64 IS NOT DISTINCT FROM t.i64 AND u.i32 > t.i32 AND t.i32 > 0 AND u.i32 < 9)",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const LogicalNodePtr marked = AsMark(plan->root);
  const ColumnId mark = FirstDependentJoin(marked)->mark;
  const arrow::Result<LogicalNodePtr> out = Unnest(marked, kBuildSideRatio);
  ASSERT_TRUE(out.ok()) << out.status().ToString();
  EXPECT_EQ(TreeText(*out),
            "Project i16\n"
            "  Join MARK build=right keys=[\"t.i16\" = \"u.i16\", \"t.i64\" IS NOT DISTINCT FROM "
            "\"u.i64\"] residual=[(u.i32 > t.i32), (t.i32 > 0)]\n"
            "    Scan table=t source=fake columns=[i16, i32, i64, u16, h, d, s, dt, bad, \"Mixed "
            "Case\", from]\n"
            "    Filter i32 < 9\n"
            "      Scan table=u source=fake columns=[i16, i32, i64, u16, h, d, s, dt, bad, \"Mixed "
            "Case\", from]\n");
  const auto* project = std::get_if<ProjectNode>(out->get());
  ASSERT_NE(project, nullptr);
  const auto* join = std::get_if<JoinNode>(project->input.get());
  ASSERT_NE(join, nullptr);
  EXPECT_NE(mark, kNoColumnId);
  EXPECT_EQ(join->mark, mark);
  ASSERT_EQ(join->keys.size(), 2U);
  EXPECT_FALSE(join->keys[0].null_equal);
  EXPECT_TRUE(join->keys[1].null_equal);
  EXPECT_EQ(join->build, BuildSide::kRight);
  EXPECT_EQ(join->estimate, EstimateRows(*join->left));
  const std::vector<ColumnId> outputs = OutputIds(*project->input);
  EXPECT_EQ(outputs.back(), mark);
}

// A dependent mark is refused where a semi join is, in an EXISTS's words: without a key.
TEST(UnnestingTest, AMarkWithoutAKeyIsRefusedAsAnExists) {
  EXPECT_EQ(
      RefusalOf("SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i32 > t.i32)", AsMark),
      "a correlated EXISTS without a key is not supported");
}

// A dependent join whose mark disagrees with its kind is a binder bug, which the pass reports as an
// internal error (exit 70) rather than leave to ResolvePositions, which would abort the process: a
// mark without its id, and a semi join with one.
TEST(UnnestingTest, ADependentJoinWhoseMarkDisagreesWithItsKindIsAnInternalError) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(kExists, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const arrow::Result<LogicalNodePtr> without =
      Unnest(WithKind(plan->root, JoinKind::kMark), kBuildSideRatio);
  ASSERT_TRUE(without.status().IsNotImplemented()) << without.status().ToString();
  EXPECT_EQ(GetSqlError(without.status()), nullptr);
  EXPECT_EQ(without.status().message(),
            "the unnesting pass met a MARK dependent join without a mark");
  const arrow::Result<LogicalNodePtr> with =
      Unnest(WithKind(plan->root, JoinKind::kSemi, FirstDependentJoin(AsMark(plan->root))->mark),
             kBuildSideRatio);
  ASSERT_TRUE(with.status().IsNotImplemented()) << with.status().ToString();
  EXPECT_EQ(with.status().message(), "the unnesting pass met a SEMI dependent join with a mark");
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
    // SQ8b also answers LIMIT and grouped EXISTS; scalar values keep their narrower rules.
    const std::string exists = RefusalOf(kExists, over(make));
    const std::string not_exists = RefusalOf(kNotExists, over(make));
    if (name == "a Sort" || name == "a Limit" || name == "a GroupAggregate") {
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

// A node on the proof's way and the input of it the walk came up from (0: its left).
struct Above {
  LogicalNode node;
  std::size_t input = 0;
};

JoinBack Proof(const std::vector<Above>& above_root_first, EmptyValue value) {
  std::vector<Ancestor> above;
  above.reserve(above_root_first.size());
  for (const Above& step : above_root_first) {
    above.push_back(Ancestor{.node = &step.node, .input = step.input});
  }
  return ProveJoinBack(above, {{kValue, std::move(value)}});
}

JoinBack Proof(const std::vector<LogicalNode>& above_root_first, EmptyValue value) {
  std::vector<Above> above;
  above.reserve(above_root_first.size());
  for (const LogicalNode& node : above_root_first) {
    above.push_back(Above{.node = node, .input = 0});
  }
  return Proof(above, std::move(value));
}

// A join of `kind` with nothing but its kind set; `key` reads the value where it is set.
LogicalNode JoinOf(JoinKind kind, bool key_reads_value = false) {
  JoinNode join;
  join.kind = kind;
  if (key_reads_value) {
    join.keys.push_back(JoinKey{.left = Bigint(kValue), .right = Bigint(kOuter)});
  }
  return join;
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

// What can pass the empty value leaves the join back LEFT: IS NULL and the DISTINCT FROM pair, a
// Compute that is not strict, a node on the way that hides the value (a Project), and nothing above
// at all.
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
  EXPECT_EQ(Proof(std::vector<LogicalNode>{}, NullValue()), JoinBack::kLeft);
}

// ADR 0041's SQ5: a Filter none of whose conjuncts rejects the empty value only drops rows, so the
// walk passes it on to a later Filter that does -- one that reads no value (which SQ0 read as a
// refusal), and one that reads it without deciding. The first Filter that decides is the one.
TEST(UnnestingTest, TheProofPassesAFilterThatDecidesNothing) {
  const EmptyValue zero = Constant{.type = LogicalType::kBigInt, .value = Int128{0}};
  EXPECT_EQ(Proof({FilterOf({Compare(kValue, CompareOp::kGt, 0)}),
                   FilterOf({Compare(kOuter, CompareOp::kGt, 0)})},
                  NullValue()),
            JoinBack::kInner);
  EXPECT_EQ(Proof({FilterOf({Compare(kValue, CompareOp::kGt, 0)}),
                   FilterOf({Compare(kValue, CompareOp::kGe, 0)})},
                  zero),
            JoinBack::kInner);
  EXPECT_EQ(Proof({FilterOf({Compare(kValue, CompareOp::kGe, 0)}),
                   FilterOf({Compare(kValue, CompareOp::kGt, 0)})},
                  zero),
            JoinBack::kInner);
  EXPECT_EQ(Proof({FilterOf({Compare(kValue, CompareOp::kGe, 0)}),
                   FilterOf({Compare(kValue, CompareOp::kEq, 0)})},
                  zero),
            JoinBack::kLeft);
  // The accepted divergence of D24: a condition that fails only over the empty value stands below
  // the one that rejects it, so an INNER join back never computes it.
  constexpr ColumnId kCast{53};
  EXPECT_EQ(Proof({FilterOf({Compare(kValue, CompareOp::kGt, 0)}),
                   FilterOf({Compare(kCast, CompareOp::kGe, 0)}),
                   ComputeNode{.input = nullptr,
                               .exprs = {std::make_shared<const Expr>(
                                   Expr{.node = CastExpr{.operand = std::make_shared<const Expr>(
                                                             Expr{.node = ColumnExpr{.id = kValue},
                                                                  .type = LogicalType::kBigInt,
                                                                  .name = "v"})},
                                        .type = LogicalType::kUSmallInt,
                                        .name = "CAST(v AS USMALLINT)"})},
                               .ids = {kCast},
                               .span = {}}},
                  zero),
            JoinBack::kInner);
}

// A join reached from its left input that pads no left row decides each left row by that row and
// its right input alone, and keeps the row's columns: the walk passes it, also where its key reads
// the value. From its right input, or where it pads its left (FULL), it stops.
TEST(UnnestingTest, TheProofPassesAJoinReachedFromItsLeft) {
  for (const JoinKind kind :
       {JoinKind::kInner, JoinKind::kLeft, JoinKind::kSemi, JoinKind::kAnti,
        JoinKind::kNullAwareAnti, JoinKind::kOneRow, JoinKind::kNullAwareMark, JoinKind::kMark}) {
    SCOPED_TRACE(ToString(kind));
    for (const bool key : {false, true}) {
      EXPECT_EQ(Proof(std::vector<Above>{{.node = FilterOf({Compare(kValue, CompareOp::kGt, 0)}),
                                          .input = 0},
                                         {.node = JoinOf(kind, key), .input = 0}},
                      NullValue()),
                JoinBack::kInner);
      EXPECT_EQ(Proof(std::vector<Above>{{.node = FilterOf({Compare(kValue, CompareOp::kGt, 0)}),
                                          .input = 0},
                                         {.node = JoinOf(kind, key), .input = 1}},
                      NullValue()),
                JoinBack::kLeft);
    }
  }
  for (const std::size_t input : {std::size_t{0}, std::size_t{1}}) {
    EXPECT_EQ(Proof(std::vector<Above>{{.node = FilterOf({Compare(kValue, CompareOp::kGt, 0)}),
                                        .input = 0},
                                       {.node = JoinOf(JoinKind::kFull), .input = input}},
                    NullValue()),
              JoinBack::kLeft);
  }
}

// Every node that is no Compute, Filter or join reached from its left stops the walk: its rows
// depend on other rows (an aggregation, a sort, a limit, a set operation) or it hides the value.
TEST(UnnestingTest, TheProofStopsAtEveryOtherNode) {
  const std::vector<LogicalNode> stops = {
      ProjectNode{}, AggregateNode{}, GroupAggregateNode{}, GroupingSetsNode{}, SortNode{},
      LimitNode{},   UnionAllNode{},  SetOperationNode{},   DependentJoinNode{}};
  for (const LogicalNode& stop : stops) {
    for (const std::size_t input : {std::size_t{0}, std::size_t{1}}) {
      EXPECT_EQ(Proof(std::vector<Above>{{.node = FilterOf({Compare(kValue, CompareOp::kGt, 0)}),
                                          .input = 0},
                                         {.node = stop, .input = input}},
                      NullValue()),
                JoinBack::kLeft)
          << ExplainNode(stop);
    }
  }
}

// What the walk meets from where a value outside a condition stands (ADR 0041's SQ5b): a select
// item's comparison over the value is a Compute, which rejects nothing, so the Project above
// decides LEFT; and an aggregate's argument over the value stops at the aggregation, below any
// Filter over the aggregation that reads the value's column.
TEST(UnnestingTest, TheProofLeavesAValueOutsideAConditionLeft) {
  const ExprPtr v = std::make_shared<const Expr>(
      Expr{.node = ColumnExpr{.id = kValue}, .type = LogicalType::kBigInt, .name = "v"});
  const ExprPtr positive = std::make_shared<const Expr>(
      Expr{.node = PredicateExpr{.predicate = Compare(kValue, CompareOp::kGt, 0), .operands = {v}},
           .type = LogicalType::kBoolean,
           .name = "(v > 0)"});
  constexpr ColumnId kPositive{52};
  EXPECT_EQ(
      Proof({ProjectNode{.input = nullptr, .columns = {}, .constants = {}, .ids = {}, .span = {}},
             ComputeNode{.input = nullptr, .exprs = {positive}, .ids = {kPositive}, .span = {}}},
            NullValue()),
      JoinBack::kLeft);
  const ExprPtr product = std::make_shared<const Expr>(Expr{
      .node = ArithExpr{.op = ArithOp::kMultiply,
                        .left = std::make_shared<const Expr>(Expr{.node = ColumnExpr{.id = kOuter},
                                                                  .type = LogicalType::kBigInt,
                                                                  .name = "o"}),
                        .right = v},
      .type = LogicalType::kBigInt,
      .name = "o * v"});
  constexpr ColumnId kProduct{53};
  EXPECT_EQ(
      Proof({FilterOf({Compare(kValue, CompareOp::kGt, 0)}), GroupAggregateNode{},
             ComputeNode{.input = nullptr, .exprs = {product}, .ids = {kProduct}, .span = {}}},
            NullValue()),
      JoinBack::kLeft);
}

// What a Compute below a passed node derives from the value is tracked across it: 2 * v is NULL
// where v is, through an inner join and a Filter that decides nothing; COALESCE(v, v) is not known.
TEST(UnnestingTest, TheProofTracksComputesAcrossPassedNodes) {
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
  const ExprPtr coalesce = std::make_shared<const Expr>(
      Expr{.node = CaseExpr{.whens = {nullptr}, .thens = {v}, .otherwise = v},
           .type = LogicalType::kBigInt,
           .name = "COALESCE(v, v)"});
  constexpr ColumnId kDerived{52};
  EXPECT_EQ(
      Proof(
          std::vector<Above>{
              {.node = FilterOf({CompareColumns(kDerived, CompareOp::kGt, kOuter)}), .input = 0},
              {.node = JoinOf(JoinKind::kInner), .input = 0},
              {.node = FilterOf({Compare(kOuter, CompareOp::kGt, 0)}), .input = 0},
              {.node =
                   ComputeNode{.input = nullptr, .exprs = {two_v}, .ids = {kDerived}, .span = {}},
               .input = 0}},
          NullValue()),
      JoinBack::kInner);
  EXPECT_EQ(Proof(
                std::vector<Above>{
                    {.node = FilterOf({Compare(kDerived, CompareOp::kGt, 0)}), .input = 0},
                    {.node = JoinOf(JoinKind::kOneRow), .input = 0},
                    {.node =
                         ComputeNode{
                             .input = nullptr, .exprs = {coalesce}, .ids = {kDerived}, .span = {}},
                     .input = 0}},
                NullValue()),
            JoinBack::kLeft);
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
// proves its INNER join back, nested and stacked ones too; a dependent join with nothing above it
// -- a plan only a test builds -- does not.
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

// The Walker tells a join's inputs apart: the same correlated COUNT under `> 0` proves INNER
// through an inner join it is the left input of, and stays LEFT as its right input. Both in one
// plan, the walk goes left first and resets the input between them.
TEST(UnnestingTest, TheWalkerTellsTheInputsOfAJoinApart) {
  const Catalog catalog = MakeCatalog();
  auto plan =
      BindSql("SELECT i16 FROM t WHERE (SELECT COUNT(*) FROM u WHERE u.i64 = t.i64) >= 0", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  LogicalNodePtr dependent = plan->root;
  while (!std::holds_alternative<DependentJoinNode>(*dependent)) {
    dependent = InputsOf(*dependent).front();
  }
  ColumnId last_id = MaxColumnId(*plan->root);
  const LogicalNodePtr other = CloneWithFreshIds(dependent, last_id);
  const auto value_of = [](const LogicalNodePtr& node) {
    return OutputIds(*std::get<DependentJoinNode>(*node).right).front();
  };
  const auto rejects = [](ColumnId value) {
    return Predicate{.kind = Predicate::Kind::kCompare,
                     .column = Bigint(value),
                     .op = CompareOp::kGt,
                     .constant = Constant{.type = LogicalType::kBigInt, .value = Int128{0}}};
  };
  const auto verdicts = [](const LogicalNodePtr& root) {
    UnnestReport report;
    const arrow::Result<LogicalNodePtr> out = Unnest(root, kBuildSideRatio, &report);
    EXPECT_TRUE(out.ok()) << out.status().ToString();
    std::vector<JoinBack> found;
    found.reserve(report.join_backs.size());
    for (const UnnestReport::Entry& entry : report.join_backs) {
      found.push_back(entry.join_back);
    }
    return found;
  };
  const auto over = [](LogicalNodePtr left, LogicalNodePtr right, std::vector<Predicate> filter) {
    JoinNode join;
    join.kind = JoinKind::kInner;
    join.left = std::move(left);
    join.right = std::move(right);
    join.build = BuildSide::kRight;
    return std::make_shared<const LogicalNode>(
        FilterNode{.input = std::make_shared<const LogicalNode>(std::move(join)),
                   .predicates = std::move(filter),
                   .span = {}});
  };
  const LogicalNodePtr scan = std::get<DependentJoinNode>(*other).left;  // fresh ids too
  EXPECT_EQ(verdicts(over(dependent, scan, {rejects(value_of(dependent))})),
            std::vector<JoinBack>{JoinBack::kInner});
  EXPECT_EQ(verdicts(over(scan, dependent, {rejects(value_of(dependent))})),
            std::vector<JoinBack>{JoinBack::kLeft});
  EXPECT_EQ(
      verdicts(over(dependent, other, {rejects(value_of(dependent)), rejects(value_of(other))})),
      (std::vector<JoinBack>{JoinBack::kInner, JoinBack::kLeft}));
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

TEST(UnnestingTest, KeyedDomainDeduplicatesFullBindingsAndRetainsEqualitySemantics) {
  for (const std::string_view op : {"=", "IS NOT DISTINCT FROM"}) {
    const auto plan = UnnestedOf(std::string("SELECT t.i16, (SELECT COUNT(*) FROM u WHERE u.i16 ") +
                                 std::string(op) + " t.i16 AND u.i32 > t.i32) FROM t");
    EXPECT_NE(plan.text.find("GroupAggregate keys=[\"t.i16\", \"t.i32\"]\n"), std::string::npos)
        << plan.text;
    EXPECT_NE(plan.text.find(std::string("keys=[\"u.i16\" ") + std::string(op) + " \"t.i16\"]"),
              std::string::npos)
        << plan.text;
    EXPECT_NE(plan.text.find("keys=[\"t.i16\" IS NOT DISTINCT FROM \"t.i16\", "
                             "\"t.i32\" IS NOT DISTINCT FROM \"t.i32\"]"),
              std::string::npos)
        << plan.text;
    EXPECT_EQ(plan.join_backs, std::vector{JoinBack::kLeft});
  }
}

TEST(UnnestingTest, ScalarDomainRequiresARepeatableSourceAndAScalarConsumer) {
  const auto unchanged = [](const LogicalNodePtr& root) { return root; };
  for (const std::string_view query :
       {"SELECT (SELECT MIN(u.i16) FROM u WHERE u.i32 > o.i32) FROM (SELECT i32 FROM t LIMIT 3) o",
        "SELECT (SELECT MIN(u.i16) FROM u WHERE u.i16 = o.i16 AND u.i32 > o.i32) "
        "FROM (SELECT i16, i32 FROM t LIMIT 3) o",
        "SELECT (SELECT MIN(u.i16) FROM u WHERE u.i16 = t.i16 AND u.i32 > t.i32) FROM t "
        "WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16)",
        "SELECT (SELECT COUNT(*) FROM u WHERE u.i16 = o.k AND u.i32 <= o.cutoff) "
        "FROM (SELECT t.i16 AS k, (SELECT MAX(u.i32) FROM u WHERE u.i16 = t.i16) + 1 AS cutoff "
        "FROM t) o",
        "SELECT i16 FROM t WHERE EXISTS (SELECT COUNT(*) FROM u WHERE u.i16 = t.i16 "
        "AND u.i32 < t.i32 HAVING COUNT(*) > 0)",
        "SELECT i32 FROM t WHERE t.i32 IN (SELECT MAX(u.i32) FROM u WHERE u.i16 = t.i16 "
        "AND u.i32 <= t.i32)"}) {
    EXPECT_NE(RefusalOf(query, unchanged).find("no inner equivalent"), std::string::npos) << query;
  }
  const auto fast = UnnestedOf("SELECT (SELECT MIN(u.i32) FROM u WHERE u.i16 = t.i16) FROM t");
  EXPECT_EQ(fast.text.find("Join INNER"), std::string::npos) << fast.text;
  EXPECT_NE(fast.text.find("keys=[\"t.i16\" = \"u.i16\"]"), std::string::npos) << fast.text;
}

TEST(UnnestingTest, StackedDomainAggregatesCloneTheOuterScanOncePerDomain) {
  for (const std::string_view anchor : {"u.i16 = t.i16 AND ", ""}) {
    std::string sql = "SELECT i16";
    for (int n = 1; n <= 4; ++n) {
      sql += ", (SELECT COUNT(*) FROM u WHERE " + std::string(anchor) +
             "u.i32 < t.i32 AND u.i64 > " + std::to_string(n) + ")";
      const auto plan = UnnestedOf(sql + " FROM t");
      const std::string_view scan = "Scan table=t ";
      int scans = 0;
      for (auto pos = plan.text.find(scan); pos != std::string::npos;
           pos = plan.text.find(scan, pos + scan.size())) {
        ++scans;
      }
      EXPECT_EQ(scans, n + 1) << plan.text;
    }
  }
}

TEST(UnnestingTest, ProjectedKeyedDomainDoesNotCloneTheEarlierScalar) {
  const auto plan = UnnestedOf(
      "SELECT o.n, (SELECT COUNT(*) FROM u WHERE u.i16 = o.k AND u.i32 <= o.cutoff) "
      "FROM (SELECT t.i16 AS k, t.i32 AS cutoff, "
      "(SELECT MAX(ok.i16) FROM ok WHERE ok.i16 = t.i16) AS n FROM t) o");
  const std::string_view scan = "Scan table=ok ";
  const auto first = plan.text.find(scan);
  ASSERT_NE(first, std::string::npos) << plan.text;
  EXPECT_EQ(plan.text.find(scan, first + scan.size()), std::string::npos) << plan.text;
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
           Case{.sql =
                    "SELECT i16 FROM t WHERE (SELECT COUNT(*) + 1 FROM u WHERE u.i64 = t.i64) > 1",
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

// ADR 0041's SQ5a: a correlated value anywhere in a condition. The proof passes Computes, Filters
// that decide nothing and joins reached from their left to the condition that rejects the empty
// value: a strict combination of a SUM is NULL there, a COUNT's 0 is not, an OR or a CASE decides
// nothing, and a second value's join, an IN's semi join or an inner ON's filter stands between.
TEST(UnnestingTest, TheProofDecidesTheJoinBackOfAValueInACondition) {
  constexpr std::string_view kSum = "(SELECT SUM(u.i32) FROM u WHERE u.i64 = t.i64)";
  constexpr std::string_view kCount = "(SELECT COUNT(*) FROM u WHERE u.i64 = t.i64)";
  constexpr std::string_view kOtherSum = "(SELECT SUM(ok.i16) FROM ok WHERE ok.i16 = t.i16)";
  constexpr std::string_view kOtherCount = "(SELECT COUNT(*) FROM ok WHERE ok.i16 = t.i16)";
  struct Case {
    std::string sql;
    std::vector<JoinBack> join_backs;
  };
  for (const Case& c : {
           Case{.sql = std::format("SELECT i16 FROM t WHERE i32 > 2 * {}", kSum),
                .join_backs = {JoinBack::kInner}},
           Case{.sql = std::format("SELECT i16 FROM t WHERE i32 > 2 * {}", kCount),
                .join_backs = {JoinBack::kLeft}},
           Case{.sql = std::format("SELECT i16 FROM t WHERE {} > 0 OR i16 = 1", kCount),
                .join_backs = {JoinBack::kLeft}},
           Case{.sql =
                    std::format("SELECT i16 FROM t WHERE CAST({} - 1 AS USMALLINT) >= 0 AND {} > 0",
                                kCount, kCount),
                .join_backs = {JoinBack::kInner}},
           Case{.sql = std::format("SELECT i16 FROM t WHERE {} + {} > 3", kSum, kOtherSum),
                .join_backs = {JoinBack::kInner, JoinBack::kInner}},
           Case{.sql = std::format("SELECT i16 FROM t WHERE {} > {}", kCount, kOtherCount),
                .join_backs = {JoinBack::kLeft, JoinBack::kLeft}},
           Case{.sql = std::format("SELECT i16 FROM t WHERE {} + 0 >= 0 AND i16 IN (SELECT ok.i16 "
                                   "FROM ok) AND {} > 0",
                                   kCount, kCount),
                .join_backs = {JoinBack::kInner}},
           Case{.sql = std::format("SELECT t.i16 FROM t JOIN ok ON ok.i16 = t.i16 AND {} + 0 >= 0 "
                                   "WHERE {} > 0",
                                   kCount, kCount),
                .join_backs = {JoinBack::kInner}},
           Case{.sql = std::format("SELECT i16 FROM t WHERE {} >= 0 AND (SELECT MAX(u.i32) FROM u) "
                                   "> 0",
                                   kCount),
                .join_backs = {JoinBack::kLeft}},
       }) {
    SCOPED_TRACE(c.sql);
    const Unnested unnested = UnnestedOf(c.sql);
    EXPECT_EQ(unnested.join_backs, c.join_backs) << unnested.text;
  }
}

// ADR 0041's SQ5b: a correlated value outside a condition. Its join back meets the block's
// aggregation, Sort or Project before any Filter that rejects its empty value, so it is LEFT: a
// select item (alone, in a CASE, under LIMIT or DISTINCT), an ORDER BY item, a GROUP BY key (next
// to ROLLUP too), an aggregate's argument (also in HAVING), a value in a correlated block's item.
// Where a condition of WHERE or an inner ON that rejects it reads the value too, the value stands
// below that condition, which decides it INNER; under OR, or as an IN probe, it stays LEFT.
TEST(UnnestingTest, TheProofDecidesTheJoinBackOfAValueOutsideConditions) {
  constexpr std::string_view kSum = "(SELECT SUM(u.i32) FROM u WHERE u.i64 = t.i64)";
  constexpr std::string_view kCount = "(SELECT COUNT(*) FROM u WHERE u.i64 = t.i64)";
  constexpr std::string_view kOtherSum = "(SELECT SUM(ok.i16) FROM ok WHERE ok.i16 = t.i16)";
  constexpr JoinBack kLeft = JoinBack::kLeft;
  constexpr JoinBack kInner = JoinBack::kInner;
  struct Case {
    std::string sql;
    std::vector<JoinBack> join_backs;
  };
  for (const Case& c : {
           Case{.sql = std::format("SELECT i16, {} FROM t", kCount), .join_backs = {kLeft}},
           Case{.sql = std::format("SELECT i16, {} FROM t", kSum), .join_backs = {kLeft}},
           Case{.sql = std::format("SELECT CASE WHEN {} > 0 THEN 1 ELSE 0 END FROM t", kSum),
                .join_backs = {kLeft}},
           Case{.sql = std::format("SELECT i16, {} FROM t LIMIT 2", kSum), .join_backs = {kLeft}},
           Case{.sql = std::format("SELECT DISTINCT {} FROM t", kSum), .join_backs = {kLeft}},
           Case{.sql = std::format("SELECT i16 FROM t ORDER BY {}", kSum), .join_backs = {kLeft}},
           Case{.sql = std::format("SELECT {} AS k, COUNT(*) FROM t GROUP BY 1", kSum),
                .join_backs = {kLeft}},
           Case{.sql = std::format("SELECT {0} AS k, i16 % 2 AS p, COUNT(*) FROM t GROUP BY {0}, "
                                   "ROLLUP (i16 % 2)",
                                   kSum),
                .join_backs = {kLeft}},
           Case{.sql = std::format("SELECT SUM(i32 * {}) FROM t", kSum), .join_backs = {kLeft}},
           Case{.sql = std::format("SELECT i16 FROM t GROUP BY i16 HAVING SUM(i32 * {}) > 0", kSum),
                .join_backs = {kLeft}},
           Case{.sql = std::format("SELECT {}, {} FROM t", kSum, kOtherSum),
                .join_backs = {kLeft, kLeft}},
           Case{.sql = std::format("SELECT i16, {0} FROM t WHERE {0} > 0", kSum),
                .join_backs = {kInner}},
           Case{.sql = std::format("SELECT i16, {0} FROM t WHERE {0} > 0 OR i16 = 1", kSum),
                .join_backs = {kLeft}},
           Case{.sql = std::format("SELECT t.i16, {0} FROM t JOIN ok ON ok.i16 = t.i16 AND {0} > 0",
                                   kSum),
                .join_backs = {kInner}},
           Case{.sql =
                    std::format("SELECT {0} AS k, COUNT(*) FROM t WHERE {0} > 0 GROUP BY 1", kSum),
                .join_backs = {kInner}},
           Case{
               .sql = std::format("SELECT i16, {0} FROM t WHERE {0} IN (SELECT i16 FROM ok)", kSum),
               .join_backs = {kLeft}},
           Case{.sql = std::format("SELECT i16, {} FROM t WHERE {} > 0", kSum, kOtherSum),
                .join_backs = {kLeft, kInner}},
           // The outer value in WHERE, and the value in its block's aggregate argument.
           Case{.sql = "SELECT i16 FROM t WHERE (SELECT MAX(u.i32 + (SELECT COUNT(*) FROM ok WHERE "
                       "ok.i16 = u.i16)) FROM u WHERE u.i64 = t.i64) > 5",
                .join_backs = {kInner, kLeft}},
           Case{.sql = "SELECT i16, (SELECT SUM(u.i32 * (SELECT COUNT(*) FROM ok WHERE ok.i16 = "
                       "u.i16)) FROM u WHERE u.i64 = t.i64) FROM t",
                .join_backs = {kLeft, kLeft}},
       }) {
    SCOPED_TRACE(c.sql);
    const Unnested unnested = UnnestedOf(c.sql);
    EXPECT_EQ(unnested.join_backs, c.join_backs) << unnested.text;
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
    const std::size_t join =
        unnested.text.find(c.join_back == JoinBack::kInner ? "Join INNER" : "Join LEFT");
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
    const std::size_t join =
        unnested.text.find(c.join_back == JoinBack::kInner ? "Join INNER" : "Join LEFT");
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
  // An IN or NOT IN conjunct of the block's HAVING is a semi or null-aware anti join over the
  // aggregate, which filters its one row: no rule takes it. The binder bound it in full first.
  const auto identity = [](const LogicalNodePtr& root) { return root; };
  for (const std::string_view in : {"IN", "NOT IN"}) {
    EXPECT_EQ(RefusalOf(std::format("SELECT i16 FROM t WHERE i32 = (SELECT COUNT(*) FROM u WHERE "
                                    "u.i64 = t.i64 HAVING COUNT(*) {} (SELECT i16 FROM ok))",
                                    in),
                        identity),
              "a correlated scalar subquery whose HAVING holds an IN subquery is not supported")
        << in;
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
