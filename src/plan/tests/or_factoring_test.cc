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
  const std::vector<const sql::Expr*> siblings = Conjuncts(where);
  for (const sql::Expr& element : where) {
    std::vector<sql::Box<sql::Expr>> owned;
    std::vector<FactoredConjunct> conjuncts;
    FactorSharedConjuncts(element, siblings, owned, conjuncts);
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
  // The remainder OR constrains one column in every branch, so it also implies an IN list.
  EXPECT_EQ(Factor("(i16 = 1 AND i32 = 2) OR (i16 = 1 AND i32 = 3)").conjuncts,
            (Texts{"i16 = 1", "i32 = 2 OR i32 = 3", "i32 IN (2, 3)"}));
  // Three branches, and the shared conjunct written in the middle of each.
  EXPECT_EQ(
      Factor("(i32 = 2 AND i16 = 1) OR (i16 = 1 AND i32 = 3) OR (i16 = 1 AND i32 = 4)").conjuncts,
      (Texts{"i16 = 1", "i32 = 2 OR i32 = 3 OR i32 = 4", "i32 IN (2, 3, 4)"}));
  // Two shared conjuncts, in the first branch's order.
  EXPECT_EQ(
      Factor("(i16 = 1 AND s = 'x' AND i32 = 2) OR (s = 'x' AND i16 = 1 AND i32 = 3)").conjuncts,
      (Texts{"i16 = 1", "s = 'x'", "i32 = 2 OR i32 = 3", "i32 IN (2, 3)"}));
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
           // Nothing is shared, yet every branch bounds i32, so the OR implies an IN list.
           Case{.text = "(i16 = 1 AND i32 = 2) OR i32 = 3",
                .conjuncts = {"i16 = 1 AND i32 = 2 OR i32 = 3", "i32 IN (2, 3)"}},
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
            (Texts{"i32 = 2", "i16 = 1 OR i16 = 3 AND s = 'x'", "i16 IN (1, 3)"}));
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
  EXPECT_EQ(between.conjuncts, (Texts{"i16 BETWEEN 1 AND 5 AND i32 = 2 OR i16 BETWEEN 1 AND 9 AND "
                                      "i32 = 3",
                                      "i32 IN (2, 3)"}))
      << "the two bounds differ, so the OR remains, and i32 is bounded in both branches";
}

// Structural equality is literal: an equality written the other way round is not the same conjunct.
TEST(OrFactoringTest, MatchingIsStructuralNotCommutative) {
  const Factored f = Factor("(i16 = i32 AND s = 'x') OR (i32 = i16 AND s = 'y')");
  EXPECT_EQ(f.conjuncts.size(), 2U) << "i16 = i32 and i32 = i16 are different conjuncts, so only "
                                       "the OR and the IN list s implies remain";
  EXPECT_EQ(f.conjuncts.back(), "s IN ('x', 'y')");
  EXPECT_TRUE(f.absorbed.empty());
}

// A conjunct a branch repeats is shared once.
TEST(OrFactoringTest, SharesARepeatedConjunctOnce) {
  EXPECT_EQ(Factor("(i16 = 1 AND i16 = 1 AND i32 = 2) OR (i16 = 1 AND i32 = 3)").conjuncts,
            (Texts{"i16 = 1", "i32 = 2 OR i32 = 3", "i32 IN (2, 3)"}));
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
  FactorSharedConjuncts(where.front(), Conjuncts(where), owned, conjuncts);
  ASSERT_EQ(conjuncts.size(), 6U) << "each OR contributes its shared conjunct, its remainder and "
                                     "the IN list that remainder implies";
  EXPECT_EQ(owned.size(), 4U) << "two folded remainders and two derived IN lists";
  std::vector<std::string> texts;
  texts.reserve(conjuncts.size());
  for (const FactoredConjunct& c : conjuncts) {
    texts.push_back(sql::ToSql(*c.expr));  // every pointer still readable after both pushes
  }
  EXPECT_EQ(texts, (Texts{"i16 = 1", "i32 = 2 OR i32 = 3", "i32 IN (2, 3)", "s = 'x'",
                          "i64 = 4 OR i64 = 5", "i64 IN (4, 5)"}));
}

// The IN list a top-level OR implies, beside the OR itself (ADR 0026's "filters implied by ORs"):
// where every branch bounds one column to literals, their union holds for every row the OR keeps,
// so a single-column IN can reach that column's Scan where the OR -- spanning two relations, or
// mixing columns -- cannot. Redundant by construction, so it can only change the plan.
TEST(OrFactoringTest, DerivesTheInListEveryBranchImplies) {
  // The shape the workload has: each branch pins a different value of the same two columns.
  EXPECT_EQ(
      Factor("(a = 'FRANCE' AND b = 'GERMANY') OR (a = 'GERMANY' AND b = 'FRANCE')").conjuncts,
      (Texts{"a = 'FRANCE' AND b = 'GERMANY' OR a = 'GERMANY' AND b = 'FRANCE'",
             "a IN ('FRANCE', 'GERMANY')", "b IN ('GERMANY', 'FRANCE')"}));
  // An IN in a branch contributes its whole list, and a repeated value is kept once.
  EXPECT_EQ(Factor("(a IN (1, 2) AND b = 9) OR (a = 2 AND b = 8)").conjuncts,
            (Texts{"a IN (1, 2) AND b = 9 OR a = 2 AND b = 8", "a IN (1, 2)", "b IN (9, 8)"}));
  // A literal on the left of the equality is the same constraint.
  EXPECT_EQ(Factor("(1 = a AND b = 9) OR (a = 2 AND b = 8)").conjuncts.at(1), "a IN (1, 2)");
}

// What it must not derive, since a derived conjunct is only sound when every branch bounds the
// column: one branch that leaves it free makes the union no constraint at all.
TEST(OrFactoringTest, DerivesNothingWhereABranchLeavesTheColumnFree) {
  const auto conjuncts = [](const std::string& text) { return Factor(text).conjuncts.size(); };
  // The second branch says nothing about `a`, so `a IN (...)` would reject rows the OR keeps.
  EXPECT_EQ(conjuncts("(a = 1 AND b = 2) OR b = 3"), 2U) << "only `b` is bounded by both branches";
  EXPECT_EQ(Factor("(a = 1 AND b = 2) OR b = 3").conjuncts.back(), "b IN (2, 3)");
  // Bounds that are not equalities bound nothing: a range, an inequality and a NOT IN.
  EXPECT_EQ(conjuncts("(a = 1 AND b = 2) OR (a > 5 AND b = 3)"), 2U);
  EXPECT_EQ(conjuncts("(a = 1 AND b = 2) OR (a <> 5 AND b = 3)"), 2U);
  EXPECT_EQ(conjuncts("(a = 1 AND b = 2) OR (a NOT IN (5, 6) AND b = 3)"), 2U);
  // A column compared with another column is no constraint to a literal set.
  EXPECT_EQ(conjuncts("(a = b AND c = 2) OR (a = 1 AND c = 3)"), 2U);
  // Mixing literal kinds would make the derived IN a bind error on a query that binds today.
  EXPECT_EQ(conjuncts("(a = 1 AND c = 2) OR (a = 'x' AND c = 3)"), 2U);
  // An IN list with a constant integer expression contributes none of its values, whatever their
  // order: its literals alone would derive `a IN (1, 3)` and drop the rows where `a` is 7.
  EXPECT_EQ(conjuncts("a IN (1, 1 + 6) OR a = 3"), 1U);
  EXPECT_EQ(conjuncts("a IN (1 + 6, 1) OR a = 3"), 1U);
  EXPECT_EQ(conjuncts("(a IN (1, 1 + 6) AND b = 2) OR (a = 3 AND b = 4)"), 2U);
  EXPECT_EQ(Factor("(a IN (1, 1 + 6) AND b = 2) OR (a = 3 AND b = 4)").conjuncts.back(),
            "b IN (2, 4)");
  // One value for every branch is the shared-conjunct path's, not this one's: no IN is added.
  EXPECT_EQ(Factor("(a = 1 AND b = 2) OR (a = 1 AND b = 3)").conjuncts,
            (Texts{"a = 1", "b = 2 OR b = 3", "b IN (2, 3)"}));
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
