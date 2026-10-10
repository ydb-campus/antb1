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

// The factored conjuncts of `text`, as canonical SQL: `conjuncts` are the ones that reach the plan,
// `absorbed` the ORs kept only to be bound, and `order` every one of them as it was emitted.
struct Factored {
  std::vector<std::string> conjuncts;
  std::vector<std::string> absorbed;
  std::vector<std::string> order;
};

Factored Factor(const std::string& text) {
  const std::vector<sql::Expr> where = Where(text);
  Factored out;
  for (const sql::Expr& element : where) {
    std::vector<sql::Box<sql::Expr>> owned;
    std::vector<FactoredConjunct> conjuncts;
    FactorSharedConjuncts(element, owned, conjuncts);
    for (const FactoredConjunct& c : conjuncts) {
      std::string sql = sql::ToSql(*c.expr);
      out.order.push_back(c.bind_only ? "bind-only: " + sql : sql);
      (c.bind_only ? out.absorbed : out.conjuncts).push_back(std::move(sql));
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
  // Three branches, one of which keeps nothing: that branch absorbs the other two, whose remainders
  // are dropped with it rather than kept as an OR.
  const Factored three = Factor("i16 = 1 OR (i16 = 1 AND i32 = 2) OR (i16 = 1 AND i32 = 3)");
  EXPECT_EQ(three.conjuncts, (Texts{"i16 = 1"}));
  EXPECT_EQ(three.absorbed.size(), 1U);
  EXPECT_EQ(three.order,
            (Texts{"i16 = 1", "bind-only: i16 = 1 OR i16 = 1 AND i32 = 2 OR i16 = 1 AND i32 = 3"}));
}

// Everything is emitted where it was written, so the caller reports the element's first error: an
// absorbed OR stays in its own place instead of moving ahead of the conjuncts before it.
TEST(OrFactoringTest, KeepsTheOrderOfTheElement) {
  EXPECT_EQ(Factor("i32 = 9 AND (i16 = 1 OR i16 = 1 AND i32 = 2)").order,
            (Texts{"i32 = 9", "i16 = 1", "bind-only: i16 = 1 OR i16 = 1 AND i32 = 2"}))
      << "the OR follows i32 = 9, which is written before it";
  // A conjunct written after the OR stays after it.
  EXPECT_EQ(Factor("(i16 = 1 OR i16 = 1 AND i32 = 2) AND i32 = 9").order,
            (Texts{"i16 = 1", "bind-only: i16 = 1 OR i16 = 1 AND i32 = 2", "i32 = 9"}));
}

// Nothing shared, nothing to do: the element's conjuncts come back exactly as they were.
TEST(OrFactoringTest, LeavesAnOrWithNothingSharedAlone) {
  struct Case {
    std::string_view text;
    Texts conjuncts;
  };
  for (const Case& c : {
           // Two bare comparisons share nothing; the OR is one conjunct, unchanged.
           Case{.text = "i16 = 1 OR i32 = 2", .conjuncts = {"i16 = 1 OR i32 = 2"}},
           // One branch is a bare comparison the other's AND chain does not contain.
           Case{.text = "(i16 = 1 AND i32 = 2) OR i32 = 3",
                .conjuncts = {"i16 = 1 AND i32 = 2 OR i32 = 3"}},
           // Three branches where only the first two share: all three keep the OR.
           Case{.text = "(i16 = 1 AND i32 = 2) OR (i16 = 1 AND i32 = 3) OR s = 'x'",
                .conjuncts = {"i16 = 1 AND i32 = 2 OR i16 = 1 AND i32 = 3 OR s = 'x'"}},
           // No OR at all: the AND chain is flattened and nothing else happens.
           Case{.text = "i16 = 1 AND i32 = 2 AND s = 'x'",
                .conjuncts = {"i16 = 1", "i32 = 2", "s = 'x'"}},
       }) {
    const Factored f = Factor(std::string(c.text));
    EXPECT_EQ(f.conjuncts, c.conjuncts) << c.text;
    EXPECT_TRUE(f.absorbed.empty()) << c.text;
  }
  // A conjunct two branches share is still factored when the rest of them differs in length.
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
  EXPECT_EQ(between.conjuncts.size(), 1U) << "the two bounds differ, so only the OR remains";
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
  std::vector<FactoredConjunct> conjuncts;
  FactorSharedConjuncts(where.front(), owned, conjuncts);
  ASSERT_EQ(conjuncts.size(), 4U);
  EXPECT_EQ(owned.size(), 2U);
  std::vector<std::string> texts;
  texts.reserve(conjuncts.size());
  for (const FactoredConjunct& c : conjuncts) {
    texts.push_back(sql::ToSql(*c.expr));  // every pointer still readable after both pushes
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
