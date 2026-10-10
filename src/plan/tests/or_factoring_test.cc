#include "../or_factoring.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "antb1/sql/ast.h"
#include "antb1/sql/parser.h"
#include "antb1/sql/unparse.h"

// The AST rewrite of ADR 0022's "OR factoring (J3)", on its own: the binder's use of it is pinned
// by plan.BinderTest.
namespace antb1::plan {
namespace {

// The WHERE of `SELECT 1 FROM t WHERE <text>` as the parser builds it: one element per top-level
// AND.
std::vector<sql::Expr> Where(const std::string& text) {
  auto stmt = sql::Parse("SELECT i16 FROM t WHERE " + text);
  if (!stmt) {
    ADD_FAILURE() << text << ": " << stmt.error().message;
    return {};
  }
  return stmt->where;
}

// The factored conjuncts of `text`, as canonical SQL, and what was absorbed.
struct Factored {
  std::vector<std::string> conjuncts;
  std::vector<std::string> absorbed;
};

Factored Factor(const std::string& text) {
  const std::vector<sql::Expr> where = Where(text);
  Factored out;
  for (const sql::Expr& element : where) {
    std::vector<sql::Box<sql::Expr>> owned;
    std::vector<const sql::Expr*> conjuncts;
    std::vector<const sql::Expr*> absorbed;
    FactorSharedConjuncts(element, owned, conjuncts, absorbed);
    for (const sql::Expr* c : conjuncts) {
      out.conjuncts.push_back(sql::ToSql(*c));
    }
    for (const sql::Expr* a : absorbed) {
      out.absorbed.push_back(sql::ToSql(*a));
    }
  }
  return out;
}

using Texts = std::vector<std::string>;

// A conjunct every branch shares is hoisted, and the OR of the remainders follows it.
TEST(OrFactoringTest, HoistsWhatEveryBranchShares) {
  EXPECT_EQ(Factor("(i16 = 1 AND i32 = 2) OR (i16 = 1 AND i32 = 3)").conjuncts,
            (Texts{"i16 = 1", "i32 = 2 OR i32 = 3"}));
  // Three branches, and the shared conjunct written in the middle of each.
  EXPECT_EQ(
      Factor("(i32 = 2 AND i16 = 1) OR (i16 = 1 AND i32 = 3) OR (i16 = 1 AND i32 = 4)").conjuncts,
      (Texts{"i16 = 1", "i32 = 2 OR i32 = 3 OR i32 = 4"}));
  // Two shared conjuncts, in the first branch's order.
  EXPECT_EQ(
      Factor("(i16 = 1 AND s = 'x' AND i32 = 2) OR (s = 'x' AND i16 = 1 AND i32 = 3)").conjuncts,
      (Texts{"i16 = 1", "s = 'x'", "i32 = 2 OR i32 = 3"}));
  // Only the structurally equal ones: spans differ, so the same text in another position shares.
  EXPECT_EQ(Factor("(i16 = 1 AND i32 = 2) OR (i32 = 2 AND i16 = 1)").conjuncts.size(), 2U)
      << "both conjuncts are shared, so the remainders are empty and the OR is absorbed";
}

// A branch of only shared conjuncts makes the OR true wherever they hold: the shared conjuncts are
// the whole conjunct, and the original OR is handed back for the caller to bind and discard.
TEST(OrFactoringTest, AbsorbsAnOrItsSharedConjunctsImply) {
  const Factored one = Factor("i16 = 1 OR (i16 = 1 AND i32 = 2)");
  EXPECT_EQ(one.conjuncts, (Texts{"i16 = 1"}));
  EXPECT_EQ(one.absorbed, (Texts{"i16 = 1 OR i16 = 1 AND i32 = 2"}));
  // The absorbing branch need not be first, and may be a conjunction of only shared conjuncts.
  const Factored last = Factor("(i16 = 1 AND i32 = 2) OR (i32 = 2 AND i16 = 1)");
  EXPECT_EQ(last.conjuncts, (Texts{"i16 = 1", "i32 = 2"}));
  EXPECT_EQ(last.absorbed.size(), 1U);
}

// Nothing shared, nothing to do: the element's conjuncts come back as they were.
TEST(OrFactoringTest, LeavesAnOrWithNothingSharedAlone) {
  for (const char* text : {
           "i16 = 1 OR i32 = 2",
           "(i16 = 1 AND i32 = 2) OR (i16 = 3 AND i32 = 2 AND s = 'x')",  // shared i32 = 2
           "(i16 = 1 AND i32 = 2) OR i32 = 3",
       }) {
    const Factored f = Factor(text);
    EXPECT_TRUE(f.absorbed.empty()) << text;
    EXPECT_FALSE(f.conjuncts.empty()) << text;
  }
  // No OR at all: the AND chain is flattened and nothing else happens.
  EXPECT_EQ(Factor("i16 = 1 AND i32 = 2 AND s = 'x'").conjuncts,
            (Texts{"i16 = 1", "i32 = 2", "s = 'x'"}));
  // The second case above really does share i32 = 2.
  EXPECT_EQ(Factor("(i16 = 1 AND i32 = 2) OR (i16 = 3 AND i32 = 2 AND s = 'x')").conjuncts,
            (Texts{"i32 = 2", "i16 = 1 OR i16 = 3 AND s = 'x'"}));
}

// One level only: a branch's own nested OR is a conjunct of that branch, not descended into, and a
// NOT is one opaque conjunct that never matches a bare conjunct.
TEST(OrFactoringTest, RewritesOneLevelOnly) {
  // The left branch is an AND, so the top-level OR has two branches and i16 = 1 is shared.
  // The nested OR is one conjunct of its branch, never descended into; ToSql prints no redundant
  // parentheses around it, OR being associative.
  EXPECT_EQ(Factor("(i16 = 1 AND (i32 = 2 OR s = 'x')) OR (i16 = 1 AND i32 = 3)").conjuncts,
            (Texts{"i16 = 1", "i32 = 2 OR s = 'x' OR i32 = 3"}));
  // NOT (i16 = 1 AND ...) is not the conjunct i16 = 1, so nothing is shared.
  const Factored negated = Factor("NOT (i16 = 1 AND i32 = 2) OR (i16 = 1 AND i32 = 3)");
  EXPECT_EQ(negated.conjuncts.size(), 1U);
  EXPECT_TRUE(negated.absorbed.empty());
  // A plain BETWEEN is one conjunct here, so two different bounds do not share a half.
  const Factored between =
      Factor("(i16 BETWEEN 1 AND 5 AND i32 = 2) OR (i16 BETWEEN 1 AND 9 AND i32 = 3)");
  EXPECT_EQ(between.conjuncts.size(), 1U) << "the BETWEENs differ, so only the OR remains";
}

// Structural equality is literal: an equality written the other way round is not the same conjunct.
TEST(OrFactoringTest, MatchingIsStructuralNotCommutative) {
  const Factored f = Factor("(i16 = i32 AND s = 'x') OR (i32 = i16 AND s = 'y')");
  EXPECT_EQ(f.conjuncts.size(), 1U) << "i16 = i32 and i32 = i16 are different conjuncts";
  EXPECT_TRUE(f.absorbed.empty());
}

// A conjunct a branch repeats is shared once.
TEST(OrFactoringTest, SharesARepeatedConjunctOnce) {
  EXPECT_EQ(Factor("(i16 = 1 AND i16 = 1 AND i32 = 2) OR (i16 = 1 AND i32 = 3)").conjuncts,
            (Texts{"i16 = 1", "i32 = 2 OR i32 = 3"}));
}

// Several ORs in one element are each factored, and the pointers stay valid as `owned` grows: the
// expressions live behind sql::Box, so a reallocation of the vector moves no expression.
TEST(OrFactoringTest, FactorsEveryOrOfAnElementAndKeepsPointersValid) {
  // The parser splits a top-level AND into one element each, so the whole conjunction is wrapped:
  // a parenthesized AND chain stays one element, which Conjuncts then flattens into two ORs.
  const std::vector<sql::Expr> where = Where(
      "(((i16 = 1 AND i32 = 2) OR (i16 = 1 AND i32 = 3)) AND "
      "((s = 'x' AND i64 = 4) OR (s = 'x' AND i64 = 5)))");
  ASSERT_EQ(where.size(), 1U) << "a parenthesized AND of two ORs is one element";
  std::vector<sql::Box<sql::Expr>> owned;
  std::vector<const sql::Expr*> conjuncts;
  std::vector<const sql::Expr*> absorbed;
  FactorSharedConjuncts(where.front(), owned, conjuncts, absorbed);
  ASSERT_EQ(conjuncts.size(), 4U);
  EXPECT_EQ(owned.size(), 2U);
  std::vector<std::string> texts;
  for (const sql::Expr* c : conjuncts) {
    texts.push_back(sql::ToSql(*c));  // every pointer still readable after both pushes
  }
  EXPECT_EQ(texts, (Texts{"i16 = 1", "i32 = 2 OR i32 = 3", "s = 'x'", "i64 = 4 OR i64 = 5"}));
}

// Factoring is idempotent: its output, re-parsed, factors to itself.
TEST(OrFactoringTest, IsIdempotent) {
  const Factored once = Factor("(i16 = 1 AND i32 = 2) OR (i16 = 1 AND i32 = 3)");
  std::string again;
  for (const std::string& c : once.conjuncts) {
    again += (again.empty() ? "" : " AND ") + ("(" + c + ")");
  }
  EXPECT_EQ(Factor(again).conjuncts, once.conjuncts);
}

}  // namespace
}  // namespace antb1::plan
