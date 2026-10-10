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

// The conjuncts of `element` with the conjuncts every branch of a top-level OR shares factored out,
// appended to `conjuncts`; `owned` keeps the expressions the rewrite builds alive, so `conjuncts`
// points into `element` or into `owned` and stays valid as `owned` grows (sql::Box holds its value
// behind a pointer).
//
// One level only: each conjunct of `element` whose root is an OR is flattened into branches, each
// branch into its own AND chain, and a conjunct of the first branch is shared when every other
// branch has one structurally equal to it (sql::EqualIgnoringSpans). The shared conjuncts come
// first, then the OR of what is left of each branch. A branch's own nested OR is not descended
// into, and a NOT is one opaque conjunct.
//
// When a branch keeps nothing, the shared conjuncts imply the whole OR, so only they are emitted
// and `absorbed` receives the original OR. The caller must still bind every expression in
// `absorbed`, and discard the result: that is what keeps a bind error in a dropped branch a bind
// error (ADR 0022's "DuckDB's bind errors stay bind errors"), which dropping it unbound would lose.
void FactorSharedConjuncts(const sql::Expr& element, std::vector<sql::Box<sql::Expr>>& owned,
                           std::vector<const sql::Expr*>& conjuncts,
                           std::vector<const sql::Expr*>& absorbed);

}  // namespace antb1::plan
