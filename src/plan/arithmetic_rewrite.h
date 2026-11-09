#pragma once

#include "antb1/plan/logical_plan.h"

// Integer arithmetic as DuckDB 1.5.6 computes it, so that an overflow fails the same queries
// (docs/sql-subset.md's rewrites of DuckDB's optimizer). Private to the plan module: the optimizer
// runs it over every expression a plan computes, and the binder over the operand of a comparison
// and the argument of a SUM before it moves their constants (MoveConstants, the sum rewrite), as
// DuckDB's own constant moving and sum rewriter see these expressions only after it.
namespace antb1::plan {

// `expr` rewritten by the three rules of DuckDB's expression rewriter that decide which integer
// additions and multiplications it computes, applied as it applies them: top down, a node's rules
// in this order until none fires before its children, and passes until no rule fires anywhere. They
// read no cast of an integer value to its own type, which DuckDB's binder never writes (it returns
// the value itself) and antb1's keeps for GROUP BY to match: such a cast is its operand.
//  1. ConstantOrderNormalization: a chain of `+` (or of `*`) of an integer type -- the nodes of one
//     operator and one type that are operands of each other; every other node is an operand of the
//     chain, as are those of another type, which DuckDB reaches through a cast -- with at least two
//     constant operands, one of them after an operand that is no constant, is rebuilt left-deep
//     with the constant operands first: x + 1 + y + 2 is ((1 + 2) + x) + y. A constant is any
//     operand that reads no column, whether or not it folds. A chain whose left-deep form would
//     take the expression deeper than the parser lets one be (sql::kMaxExpressionDepth) stays as
//     it is.
//  2. ConstantFolding: an integer expression that reads no column becomes its value, of its type,
//     where its evaluation succeeds. One that overflows its type stays as it is -- DuckDB leaves
//     it unfolded -- and is computed, failing on the first row that computes it: so
//     x + 2147483647 + 1 fails, as (2147483647 + 1) + x, where it once computed x + 2147483647.
//     Only literals and + - * // %, negation, casts and abs over them are evaluated here; any other
//     constant (a function of a string, a CASE of constants, a quotient by zero) is computed
//     instead, to the same value, and is none to the rules that need a constant's value: rule 3 and
//     the binder's constant moving and sum rewrite (divergence D33).
//  3. ArithmeticSimplification: x + 0, 0 + x, x - 0, x * 1, 1 * x and x // 1 of an integer type
//     become x (cast to the expression's type where x is of another one), so x + 1 + -1 computes
//     nothing: (1 + -1) + x, then 0 + x, then x. x * 0 and 0 * x become DuckDB's
//     constant_or_null(0, x), which computes x and is NULL for a NULL x, and which no chain of *
//     reads through. Its x // 0, NULL without computing x, and its rule for a NULL constant operand
//     are not reproduced: antb1 computes x // 0, to NULL, and folds no constant to NULL, so that
//     (x + 1) // 0, or an operation with 2 // 0 as an operand, computes its other operand where
//     DuckDB does not.
// The result keeps `expr`'s name, type and values; only the rows an overflow fails on can differ,
// and they become DuckDB's. The same pointer where no rule fires.
ExprPtr RewriteArithmetic(const ExprPtr& expr);

// One top-down pass of RewriteArithmetic: each node's rules until none fires, then its children.
// What DuckDB's rewriter gives a comparison's operand between two of its constant moves, since its
// MoveConstants changes the comparison in place and ends the pass before the operand is visited
// (Binder::MoveConstants). Keeps `expr`'s name and type; the same pointer where no rule fires.
ExprPtr RewriteArithmeticPass(const ExprPtr& expr);

}  // namespace antb1::plan
