# 12. Scalar expressions

Date: 2026-09-28

## Status

Accepted (with the maintainer-approved plan for scalar expressions)

## Context

- The seven ClickBench queries antb1 did not answer all need scalar expressions: arithmetic inside `SUM`, arithmetic
  in `GROUP BY` and the select list, string functions, `CASE` and timestamp functions. The parser reads the whole
  expression grammar since the first PR of the plan (ADR 0008's update); the binder rejected every non-leaf
  expression.
- DuckDB, the oracle, types arithmetic by its operands: an integer literal that fits the other operand's type takes
  that type, two integer types the wider one, `/` is always DOUBLE. It computes integer arithmetic in that type and
  fails the query on an overflow ("Overflow in addition of INT16"). Its optimizer rewrites `SUM(x + c)` for a signed
  integer `x` and an integer constant `c` into `SUM(x) + c * COUNT(x)`, so that addition is never computed and never
  overflows.
- Operators (`Filter`, `GroupAggregate`, the aggregate states, `Sort`, `Project`) all take columns of their input by
  index. Arrow's compute functions cover checked integer arithmetic, float division and the comparisons of mixed
  numeric types.

## Decision

- **A bound expression tree, `plan::Expr`,** with the node kinds the binder answers (a column of the node's input, a
  constant, arithmetic, negation), each typed with DuckDB's rule and named with DuckDB's result name (`(a + 1)`,
  `sum((a + 1))`). Later PRs of the plan add function calls, `CASE` and timestamps as new node kinds.
  Update: function calls are a `FunctionExpr` node with a fixed list of functions (`strlen`, `regexp_replace`), each
  with its arity, argument types and result type in the binder; a `regexp_replace` pattern and replacement must be
  literals, so the evaluator compiles them once per batch with Arrow's RE2 kernel.
  Update: conditions are BOOLEAN expressions (`PredicateExpr` leaves bound and folded as `WHERE` comparisons,
  `BoolExpr` for `AND`/`OR`/`NOT`) and `CASE` a `CaseExpr`; BOOLEAN is an internal type, never a column of a table
  or a result. A `WHERE` or `HAVING` conjunct with `OR` or `NOT` is computed by a `Compute` and filtered with a new
  `IS TRUE` predicate, so `Filter` stays a conjunction over columns. `CASE` computes each branch only for its rows,
  as DuckDB does, so a guarded overflow never fails.
- **A `Compute` node** appends one column per expression to its input. The binder places one over the (filtered)
  table for `WHERE` operands, aggregate arguments and `GROUP BY` expressions, and one over the aggregation for select,
  `HAVING` and `ORDER BY` expressions over keys and aggregates. Every other operator keeps taking columns, and a
  select or `ORDER BY` expression equal to a `GROUP BY` expression is that key. A `WHERE` condition over table columns
  filters before the `Compute`, one over computed columns after it; two columns compare with a new
  `column <op> column` predicate.
- **Evaluation with Arrow's kernels where they match DuckDB** (`add_checked`, `subtract_checked`,
  `multiply_checked`, `negate_checked` in the result type, `divide` in DOUBLE, the comparison kernels), and with own
  loops where they do not: `//` and `%` (DuckDB truncates, returns NULL for a zero divisor and fails on the minimum
  divided by -1), and HUGEINT `+ - *` (Arrow's decimal kernels widen the precision past 38 digits). An overflow is an
  execution error (exit code 1), as in DuckDB. A `Compute` evaluates only the rows its input selected.
- **DuckDB's sum rewriter is reproduced in the binder**, so `SUM(x + c)` never fails where DuckDB answers: the SUM
  becomes an expression over a hidden `SUM(x)` and `COUNT(x)` in HUGEINT.
- **What stays unsupported:** DECIMAL arithmetic (a decimal literal with an integer; antb1 has no DECIMAL type),
  DATE arithmetic, negating a USMALLINT (DuckDB wraps it), `//` and `%` in HUGEINT, and arithmetic on FLOAT columns
  (antb1 reads FLOAT as DOUBLE, divergence D11).

## Consequences

- ClickBench Q29 (`SUM` of a column plus 90 constants) and Q35 (`GROUP BY` and select a column minus constants) are
  answered, checked against DuckDB by the `.slt` cases, the random differential test and the ClickBench ratchet.
- Expressions cost a materialization of the selected rows and one array per expression and batch; the aggregates and
  sorts themselves are unchanged.
- The query generator knows each integer column's data range and writes only arithmetic that cannot overflow, since
  both engines fail on an overflow and the differential test compares answers.
- HUGEINT arithmetic is limited to decimal128(38, 0), 10^38 - 1 (divergence D9), while DuckDB's HUGEINT reaches
  2^127 - 1.
