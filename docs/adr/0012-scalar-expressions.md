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
  literals, so the evaluator compiles them once per batch with Arrow's RE2 kernel. DuckDB replaces the first match
  of the whole value (`RE2::Replace`) where Arrow's `replace_substring_regex` replaces every match, and with one
  replacement re-matches the pattern against the matched substring alone, where `\b`, `\B`, `^` and `$` see the
  wrong neighbours; so a pattern is wrapped as `^(\C*?)(pattern)` -- RE2's own leftmost unanchored search, `\C`
  being any byte -- with the text before the match in group 1 and the replacement's groups shifted by two.
  Update (2026-10-18): **a pattern already anchored at `^` is not wrapped.** A leading `^` matches in one place
  only, so the lazy `\C*?` before it can match nothing: `^(\C*?)(^P)` accepts exactly what `^P` accepts, with group
  1 always empty, and the anchor already limits the kernel to one replacement. What the wrapper costs is RE2's
  literal prefix -- with `\C*?` first it cannot reject a value that could never match without trying every
  position, where `^https?://` first rejects on the first byte. So it is free on values that match and dear on
  values that do not: over 200 K values through the kernel, dropping it is 0.93x when all of them match, **0.29x
  when none do**, and 0.79x over the 82/18 mix the ClickBench data holds (14.7 M of its 81.0 M non-empty Referer
  rows do not match q28's pattern). End to end q28 went 3.330 s to 2.740 s, **0.823x** -- 0.59 s off the largest
  query of the 43. The binder's rejections (`\Q` in a pattern, `\8` and `\9` in a replacement) stay unconditional,
  so which form the evaluator picks is invisible apart from the speed.
  Update (2026-10-18): **a trailing `.*$` is rewritten to `\C*$`.** The match is replaced along with
  everything else, so the tail's bytes are thrown away and all that scan establishes is that they are
  characters and that none of them is a newline; `\C*`, RE2's any byte, establishes nothing and skips
  bytes instead of decoding characters. Over 977,975 Referer values that is 0.444 us a value against
  **0.197** -- the speed of running RE2 in Latin-1 with `dot_nl`, but through Arrow's own kernel, so
  with no new dependency. (`(?s:.*)$` is 0.389: the win is the byte-wise skip, not the newline flag.)
  The two tails read a value the same way only where it is a whole number of characters and holds no
  newline, so the rewrite is guarded on both, off the batch's distinct values before the regex runs:
  UTF-8 in one pass over all of them (plus a test that no value starts on a continuation byte, since
  the concatenation can validate where a value alone does not), and a newline **per value**, not per
  batch. Per batch was the first form and it gave back 57% of the win: one newline among a batch's
  ~20 K distinct values takes the rewrite off every one of them, and about 2,200 of ClickBench's
  100 M Referer values hold one -- enough to reach more than half the batches. The values that do
  hold one run the pattern as written and go back in place with `replace_with_mask`. That is
  correctness and not only speed: for an unanchored pattern the two tails answer differently there,
  since `.*$` cannot match across the newline and the leftmost match therefore starts after it,
  where `\C*$` matches from the front.
  The pattern must also start the tail where a character starts, which whole values do not give on
  their own: `\C`, which consumes one byte, and `\B`, which is a word boundary over bytes and so
  holds between the bytes of a character, both leave the cursor inside one, and there `.*$` fails --
  no lone continuation byte is a character -- where `\C*$` eats the rest of it. `\B.*$` over a value
  of `a` and U+00E9 answered `61 C3 58`, a result that is not valid UTF-8 from input that is. Those
  two are the only constructs that can do it: `\b` cannot, since no byte of a multi-byte character
  is an ASCII word character, so it sees non-word on both sides and fails; `^`, `$`, `\A`, `\z` and
  their `(?m)` forms are all at a character boundary; and everything that matches a rune consumes it
  whole. So a pattern holding an unescaped `\C` or `\B` keeps the slower tail, and the rest of the
  pattern test is deliberately conservative -- the last three characters, with an even number of
  backslashes before the dot -- so `(.*)$` and `.{0,}$` keep it too, and a tail whose group the
  replacement reads is never rewritten. End to end
  q28 went 2.78 s to 1.90 s, **0.68x**, and the 43 from 20.04 s to 19.16 s with no query more than
  3% slower.
  Rejected on the way there, so that it is not planned twice: **memoising the replacement across the batches of a
  part.** The evaluator dictionary-encodes each batch so RE2 runs once per distinct value *of that batch*, and
  widening that scope looked worth 4x. Instrumented on q28 it is not: 25,806,416 calls today against the 23,266,159
  a part-scoped memo would make and 19,720,796 for a query-wide one -- **1.11x and 1.31x**, because the per-batch
  dictionary already reaches 3.14 rows a call. The 4x came from extrapolating one row group that is 13.6% distinct
  where the file is 24%.
  Update (2026-10-13, ADR 0023's S5): the list is `strlen`, `regexp_replace`, `toDateTime`, `date_trunc` and
  `substring` (with its alias `substr`), and an arity may now be a range: `substring` takes two or three arguments.
  `substring` is also the first function whose arguments after the value are **evaluated per row** rather than
  constants, so `FunctionSpec` carries their kind and `FunctionExpr::args` holds bound expressions for them; the
  evaluator walks UTF-8 codepoints itself, since Arrow's slice kernels take a constant window.
  Update: conditions are BOOLEAN expressions (`PredicateExpr` leaves bound and folded as `WHERE` comparisons,
  `BoolExpr` for `AND`/`OR`/`NOT`) and `CASE` a `CaseExpr`; BOOLEAN is an internal type, never a column of a table
  or a result. A `WHERE` or `HAVING` conjunct with `OR` or `NOT` is computed by a `Compute` and filtered with a new
  `IS TRUE` predicate, so `Filter` stays a conjunction over columns. `CASE` computes each branch only for its rows,
  as DuckDB does, so a guarded overflow never fails.
  Update: TIMESTAMP (Arrow timestamp[us]) is a computed type, produced by `toDateTime` (ClickBench's DuckDB macro
  `epoch_ms(t * 1000)`, bound as that expression; the oracle defines the macro the same way) and `date_trunc`, and
  read by `EXTRACT`; it sorts, groups and aggregates like an integer and prints as DuckDB prints it.
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
  Update (2026-10-02): DECIMAL arithmetic is specified by [ADR 0021](0021-decimal-semantics.md). Neither of DuckDB's
  integer-only rewrites, the sum rewriter above and constant moving in comparisons, applies to DECIMAL.
  (Later PRs implemented that specification: DECIMAL columns, literals and `+ - * / // %` with integers and with
  each other all answer today; the other four constructs of the list above are still unsupported.)

## Consequences

- ClickBench Q29 (`SUM` of a column plus 90 constants) and Q35 (`GROUP BY` and select a column minus constants) are
  answered, checked against DuckDB by the `.slt` cases, the random differential test and the ClickBench ratchet.
- Expressions cost a materialization of the selected rows and one array per expression and batch; the aggregates and
  sorts themselves are unchanged.
- The query generator knows each integer column's data range and writes only arithmetic that cannot overflow, since
  both engines fail on an overflow and the differential test compares answers.
- HUGEINT arithmetic is limited to decimal128(38, 0), 10^38 - 1 (divergence D9), while DuckDB's HUGEINT reaches
  2^127 - 1.
