#pragma once

#include <vector>

#include "antb1/sql/ast.h"

// Factoring the conjuncts that every branch of an OR shares
// (docs/adr/0022-joins-and-query-blocks.md, "OR factoring (J3)"): `(A AND X) OR (A AND Y)` is `A
// AND (X OR Y)`, so a shared equality can become a join key and a shared single-relation conjunct
// can reach its scan. An AST rewrite, run on each WHERE and inner-ON element before the binder
// classifies its conjuncts. Private to the plan module: the binder and its tests include it.

namespace antb1::plan {

// The conjuncts of a predicate, with parenthesized AND chains flattened; appended to `out`.
void Conjuncts(const sql::Expr& expr, std::vector<const sql::Expr*>& out);

// The conjuncts of every element of a WHERE or HAVING predicate, in order.
std::vector<const sql::Expr*> Conjuncts(const std::vector<sql::Expr>& predicate);

// One conjunct the factoring leaves behind.
struct FactoredConjunct {
  const sql::Expr* expr;
  // An OR the shared conjuncts imply, kept only so that the caller can bind it: see below.
  bool bind_only = false;
};

// The conjuncts of `element` with the conjuncts every branch of a top-level OR shares factored out,
// appended to `out` in the order they are written, so that the first error a caller reports is the
// first one in the element. `owned` keeps the expressions the rewrite builds alive, so each `expr`
// points into `element` or into `owned` and stays valid as `owned` grows (sql::Box holds its value
// behind a pointer).
//
// One level only: each conjunct of `element` whose root is an OR is flattened into branches, each
// branch into its own AND chain, and a conjunct of the first branch is shared when every other
// branch has one structurally equal to it (sql::EqualIgnoringSpans). An OR contributes the shared
// conjuncts, then the OR of what is left of each branch. A branch's own nested OR is not descended
// into, and a NOT is one opaque conjunct.
//
// When a branch keeps nothing, the shared conjuncts imply the whole OR, so what the OR contributes
// after them is the original OR with `bind_only` set. The caller must bind it like any other
// conjunct and discard the result: that is what keeps a bind error in a dropped branch a bind error
// (ADR 0022's "DuckDB's bind errors stay bind errors"), which dropping it unbound would lose.
void FactorSharedConjuncts(const sql::Expr& element, std::vector<sql::Box<sql::Expr>>& owned,
                           std::vector<FactoredConjunct>& out);

}  // namespace antb1::plan
