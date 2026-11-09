#include "../arithmetic_rewrite.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/plan/catalog.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/sql/parser.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::BindSql;
using testing::MakeCatalog;

using ColumnNames = std::map<ColumnId, std::string>;

// The names of the columns `expr` reads, by id.
void CollectNames(const Expr& expr, ColumnNames& out) {
  if (const auto* column = std::get_if<ColumnExpr>(&expr.node)) {
    out.emplace(column->id, expr.name);
  } else if (const auto* arith = std::get_if<ArithExpr>(&expr.node)) {
    CollectNames(*arith->left, out);
    CollectNames(*arith->right, out);
  } else if (const auto* negate = std::get_if<NegateExpr>(&expr.node)) {
    CollectNames(*negate->operand, out);
  } else if (const auto* cast = std::get_if<CastExpr>(&expr.node)) {
    CollectNames(*cast->operand, out);
  } else if (const auto* function = std::get_if<FunctionExpr>(&expr.node)) {
    for (const ExprPtr& arg : function->args) {
      CollectNames(*arg, out);
    }
  }
}

// The structure of an expression: an operation with its operands in order, a constant by its
// value, a cast with its type, a column by its name in `columns` (a rewritten expression keeps the
// name of the expression it replaces, a column too), anything else by its name.
std::string Shape(const Expr& expr, const ColumnNames& columns) {
  if (const auto* arith = std::get_if<ArithExpr>(&expr.node)) {
    return std::format("({} {} {})", Shape(*arith->left, columns), ToString(arith->op),
                       Shape(*arith->right, columns));
  }
  if (const auto* constant = std::get_if<ConstantExpr>(&expr.node)) {
    if (const auto* value = std::get_if<Int128>(&constant->value.value)) {
      return Int128ToString(*value);
    }
  }
  if (const auto* negate = std::get_if<NegateExpr>(&expr.node)) {
    return std::format("-({})", Shape(*negate->operand, columns));
  }
  if (const auto* cast = std::get_if<CastExpr>(&expr.node)) {
    return std::format("CAST({} AS {})", Shape(*cast->operand, columns), ToString(expr.type));
  }
  if (const auto* column = std::get_if<ColumnExpr>(&expr.node)) {
    const auto name = columns.find(column->id);
    return name == columns.end() ? expr.name : name->second;
  }
  // CASE WHEN x IS NOT NULL THEN 0 END, DuckDB's constant_or_null(0, x).
  if (const auto* c = std::get_if<CaseExpr>(&expr.node);
      c != nullptr && c->whens.size() == 1 && c->otherwise == nullptr) {
    if (const auto* when = std::get_if<PredicateExpr>(&c->whens.front()->node);
        when != nullptr && when->predicate.kind == Predicate::Kind::kIsNotNull &&
        when->operands.size() == 1) {
      return std::format("constant_or_null({}, {})", Shape(*c->thens.front(), columns),
                         Shape(*when->operands.front(), columns));
    }
  }
  return expr.name;
}

// The expression of the select item of SELECT <item> FROM t, as the binder computes it.
ExprPtr Item(std::string_view item) {
  static const Catalog catalog = MakeCatalog();
  const std::string sql = std::format("SELECT {} FROM t", item);
  auto plan = BindSql(sql, catalog);
  if (!plan.ok()) {
    ADD_FAILURE() << sql << ": " << plan.status().ToString();
    return nullptr;
  }
  const auto* compute = std::get_if<ComputeNode>(&testing::Nth(*plan, 1));
  if (compute == nullptr || compute->exprs.size() != 1) {
    ADD_FAILURE() << sql << ": no Compute of one expression below the Project";
    return nullptr;
  }
  return compute->exprs.front();
}

// DuckDB 1.5.6's ConstantOrderNormalization, ConstantFolding and ArithmeticSimplification, each
// case checked against the plan DuckDB's EXPLAIN shows for the same expression over columns of the
// same types: the same operations and operands in the same order, where DuckDB also shows the casts
// antb1's executor makes, and constant_or_null(0, x) and NULL for x * 0 and x // 0, which antb1
// computes (arithmetic_rewrite.h). The name and the type stay the written expression's.
TEST(ArithmeticRewriteTest, IntegerChainsAsDuckDbComputesThem) {
  struct Case {
    std::string_view item;
    std::string_view shape;
  };
  for (const Case& c : {
           // The constants of a chain first, in order, and folded.
           Case{.item = "i32 + 1 + 2", .shape = "(3 + i32)"},
           Case{.item = "1 + i32 + 2", .shape = "(3 + i32)"},
           Case{.item = "1 + i32 + 2 + i32 + 3", .shape = "((6 + i32) + i32)"},
           Case{.item = "i32 * 2 * 3", .shape = "(6 * i32)"},
           Case{.item = "i32 + (1 + 2) + i32", .shape = "((3 + i32) + i32)"},
           // A * inside a + chain is an operand of it, folded where it stands.
           Case{.item = "i32 + (1 * 2) + i32", .shape = "((i32 + 2) + i32)"},
           // Constants that lead the chain stay apart, and fold only among themselves.
           Case{.item = "1 + (2 + i32)", .shape = "(1 + (2 + i32))"},
           Case{.item = "(1 + 2) + (3 + i32)", .shape = "(3 + (3 + i32))"},
           // One that overflows its type stays unfolded, from its first overflow up.
           Case{.item = "i32 + 2147483647 + 1", .shape = "((2147483647 + 1) + i32)"},
           Case{.item = "i32 + 2147483640 + 7 + 1", .shape = "((2147483647 + 1) + i32)"},
           Case{.item = "i32 + 2147483647 + 1 + -1", .shape = "(((2147483647 + 1) + -1) + i32)"},
           Case{.item = "i32 + -1 + 2147483647 + 1", .shape = "(2147483647 + i32)"},
           Case{.item = "i32 * 2147483647 * 2 * 0",
                .shape = "(constant_or_null(0, (2147483647 * 2)) * i32)"},
           // In the chain's type: SMALLINT here, BIGINT where a literal or a column widens it.
           Case{.item = "i16 + 1 + 2", .shape = "(3 + i16)"},
           Case{.item = "i16 + 30000 + 30000", .shape = "((30000 + 30000) + i16)"},
           Case{.item = "i64 + 2147483647 + 1", .shape = "(2147483648 + i64)"},
           Case{.item = "i32 + 3000000000 + 1", .shape = "(3000000001 + i32)"},
           // An operation of another type is an operand, as DuckDB casts it.
           Case{.item = "2147483647 + 1 + i64", .shape = "((2147483647 + 1) + i64)"},
           Case{.item = "u16 + 1 + -1", .shape = "((u16 + 1) + -1)"},
           // - is no chain.
           Case{.item = "i32 - 1 - 2", .shape = "((i32 - 1) - 2)"},
           Case{.item = "i32 + 1 - 1", .shape = "((i32 + 1) - 1)"},
           // x + 0, x - 0, x * 1 and x // 1 are x, cast to the expression's type.
           Case{.item = "i32 + 1 + -1", .shape = "i32"},
           Case{.item = "i32 + 0", .shape = "i32"},
           Case{.item = "0 + i32", .shape = "i32"},
           Case{.item = "i32 - 0", .shape = "i32"},
           Case{.item = "0 - i32", .shape = "(0 - i32)"},
           Case{.item = "i32 * 1", .shape = "i32"},
           Case{.item = "1 * i32", .shape = "i32"},
           Case{.item = "i32 // 1", .shape = "i32"},
           Case{.item = "1 // i32", .shape = "(1 // i32)"},
           Case{.item = "u16 + 70000 + -70000", .shape = "CAST(u16 AS INTEGER)"},
           // x * 0 is DuckDB's constant_or_null(0, x), which computes x and is NULL for a NULL x,
           // and which no chain of * reads through; x // 0 computes x, then NULL.
           Case{.item = "i32 * 2 * 0", .shape = "constant_or_null(0, i32)"},
           Case{.item = "i32 * 0", .shape = "constant_or_null(0, i32)"},
           Case{.item = "(i32 * 2147483640 + 0) * 0",
                .shape = "constant_or_null(0, (i32 * 2147483640))"},
           Case{.item = "i32 // 0", .shape = "(i32 // 0)"},
           // // and % of constants fold, with a divisor that is no zero.
           Case{.item = "i32 + (4 // 2) + 1", .shape = "(3 + i32)"},
           Case{.item = "i32 + (7 % 4) + 1", .shape = "(4 + i32)"},
           // Negation and abs of constants are constants, a negation of a cast (-1::INTEGER) or of
           // an operation included; -(1) is the literal -1.
           Case{.item = "i32 + -1::INTEGER + 2", .shape = "(1 + i32)"},
           Case{.item = "i32 + -(2 * 1) + 3", .shape = "(1 + i32)"},
           Case{.item = "i32 + -(1) + 2", .shape = "(1 + i32)"},
           Case{.item = "i32 + abs(-1) + 2", .shape = "(3 + i32)"},
           // A whole constant folds.
           Case{.item = "1 + 2", .shape = "3"},
           // A cast to a value's own type is no node to DuckDB, whose binder writes none: the
           // chain reads through it, a written constant's included.
           Case{.item = "CAST(i32 + 1 AS INTEGER) + -1", .shape = "i32"},
           Case{.item = "CAST(i32 + 2147483647 AS INTEGER) + 1",
                .shape = "((2147483647 + 1) + i32)"},
           Case{.item = "i32 + 2147483647 + CAST(1 + -1 AS INTEGER)",
                .shape = "(((2147483647 + 1) + -1) + i32)"},
           Case{.item = "CAST(i32 * 2 AS INTEGER) * 0", .shape = "constant_or_null(0, i32)"},
           Case{.item = "CAST(i32 + 1 AS BIGINT) + -1",
                .shape = "(CAST((i32 + 1) AS BIGINT) + -1)"},
           // DOUBLE and DECIMAL arithmetic stays as written.
           Case{.item = "d + 1 + 2", .shape = "((d + 1) + 2)"},
       }) {
    const ExprPtr bound = Item(c.item);
    ASSERT_NE(bound, nullptr) << c.item;
    const ExprPtr rewritten = RewriteArithmetic(bound);
    ColumnNames columns;
    CollectNames(*bound, columns);
    EXPECT_EQ(Shape(*rewritten, columns), c.shape) << c.item;
    EXPECT_EQ(rewritten->name, bound->name) << c.item;
    EXPECT_EQ(rewritten->type, bound->type) << c.item;
    EXPECT_EQ(RewriteArithmetic(rewritten), rewritten) << "a fixed point: " << c.item;
  }
}

// An expression no rule changes is the same pointer, and so is a plan the pass does not change.
TEST(ArithmeticRewriteTest, NothingToRewriteIsTheSameExpression) {
  for (const std::string_view item : {"i32 + 1", "i32 + i64 + 1", "-i32", "d * 2", "i32 - 1 - 2"}) {
    const ExprPtr bound = Item(item);
    ASSERT_NE(bound, nullptr) << item;
    EXPECT_EQ(RewriteArithmetic(bound), bound) << item;
  }
}

// The optimizer rewrites what a plan computes (its rule 6): an aggregate's argument, a WHERE
// operand and a select item over a key alike, each keeping its name.
// A balanced chain of many operands, which parses shallow, would be as deep as it has operands
// once rebuilt left-deep: it stays as written where that would take the expression deeper than
// the parser lets one be, so that no walk of the plan recurses deeper than parsing does.
TEST(ArithmeticRewriteTest, AChainTooLongToRebuildStaysAsWritten) {
  const Catalog catalog = MakeCatalog();
  std::string chain = "(i32 + 1)";
  for (int i = 0; i < 9; ++i) {  // 1024 operands, 11 levels
    chain = std::format("({} + {})", chain, chain);
  }
  auto plan = BindSql(std::format("SELECT {} AS v FROM t", chain), catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString().substr(0, 200);
  const LogicalPlan optimized = Optimized(*plan);
  const auto depth = [](auto&& self, const Expr& expr) -> std::size_t {
    if (const auto* arith = std::get_if<ArithExpr>(&expr.node)) {
      return 1 + std::max(self(self, *arith->left), self(self, *arith->right));
    }
    return 1;
  };
  std::size_t deepest = 0;
  const auto visit = [&](auto&& self, const LogicalNodePtr& node) -> void {
    if (const auto* compute = std::get_if<ComputeNode>(node.get())) {
      for (const ExprPtr& expr : compute->exprs) {
        deepest = std::max(deepest, depth(depth, *expr));
      }
    }
    for (const LogicalNodePtr& input : InputsOf(*node)) {
      self(self, input);
    }
  };
  visit(visit, optimized.root);
  EXPECT_GT(deepest, 1U) << "the chain is computed";
  EXPECT_LE(deepest, sql::kMaxExpressionDepth) << "no deeper than the parser lets an expression be";
  // A short chain still regroups.
  EXPECT_EQ(Shape(*RewriteArithmetic(Item("i32 + 1 + 2")), {}), "(3 + i32)");
}

TEST(ArithmeticRewriteTest, TheOptimizerRewritesEveryComputedExpression) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT i32 + 1 + -1, SUM(i16 * 2 * 3) FROM t WHERE i32 + 2147483647 + 1 > i64 GROUP BY i32",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const LogicalPlan optimized = Optimized(*plan);
  std::map<std::string, ExprPtr> computed;
  const auto collect = [&computed](auto&& self, const LogicalNodePtr& node) -> void {
    if (node == nullptr) {
      return;
    }
    if (const auto* compute = std::get_if<ComputeNode>(node.get())) {
      for (const ExprPtr& expr : compute->exprs) {
        computed.emplace(expr->name, expr);
      }
    }
    for (const LogicalNodePtr& input : InputsOf(*node)) {
      self(self, input);
    }
  };
  collect(collect, optimized.root);
  const auto shape = [&computed](const std::string& name) {
    const auto expr = computed.find(name);
    return expr == computed.end() ? std::string("(none)") : Shape(*expr->second, {});
  };
  EXPECT_EQ(shape("((i16 * 2) * 3)"), "(6 * i16)");
  EXPECT_EQ(shape("((i32 + 2147483647) + 1)"), "((2147483647 + 1) + i32)");
  const auto key = computed.find("((i32 + 1) + -1)");
  ASSERT_NE(key, computed.end());
  EXPECT_TRUE(std::holds_alternative<ColumnExpr>(key->second->node))
      << "the select item computes nothing: it is the key i32";
}

// And what a join's residual computes.
TEST(ArithmeticRewriteTest, TheOptimizerRewritesAJoinsResidual) {
  const Catalog catalog = MakeCatalog();
  auto plan =
      BindSql("SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 AND t.i32 + 1 + 2 > u.i64", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const LogicalPlan optimized = Optimized(*plan);
  const JoinNode* join = testing::FirstJoin(optimized);
  ASSERT_NE(join, nullptr);
  ASSERT_EQ(join->residual.size(), 1U);
  const auto* condition = std::get_if<PredicateExpr>(&join->residual.front()->node);
  ASSERT_NE(condition, nullptr);
  ASSERT_FALSE(condition->operands.empty());
  EXPECT_EQ(Shape(*condition->operands.front(), {}), "(3 + t.i32)");
}

}  // namespace
}  // namespace antb1::plan
