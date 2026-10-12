# 25. The VALUES table constructor, postponed

Date: 2026-10-12

## Status

Postponed (the design is recorded so the shape is agreed; no roadmap PR implements it, and the
grammar keeps rejecting it with exit code 4)

## Context

- **What the form is.** `VALUES` is SQL's table constructor: a literal table written inline, as a
  list of parenthesised row constructors. It appears in two positions — in `FROM`
  (`FROM (VALUES (1), (2)) AS v(x)`, two rows in DuckDB) and as the body of a subquery
  (`x IN (VALUES (1), (2))`), where it stands exactly where a `SELECT` would.
- **It is not the `IN` list antb1 already answers.** `x IN (1, 2)` is a list of scalar expressions
  inside the `IN` operator (`sql::InExpr`); `x IN (VALUES (1), (2))` is a one-column, two-row table
  compared against `x`. Over the star fixtures both answer **140**, so for literals the two are a
  pure spelling difference with no expressive gain. That coincidence is the whole reason the form
  looks like a small addition.
- **It is not ADR 0022's `block` either.** That grammar's `block` is a `SELECT` with an optional
  `WITH` list. `VALUES` is its own production, and it composes with set operations:
  `x IN (VALUES (1), (2) UNION SELECT 3)` answers 210 in DuckDB. Supporting it therefore means a
  table-expression node, not a `Kind` on ADR 0022's subquery node.
- **Where antb1 stands today.** In `FROM` and as a CTE's query the form is recognised and refused:
  `StartsOtherQuery` (`src/sql/parser.cc`) knows `FROM`, `TABLE` and `VALUES` before `(` as the
  parenthesised queries DuckDB takes that start with no `SELECT` or `WITH`, and docs/sql-subset.md
  records `(VALUES (1))` as exit code 4. The guard on the following `(` is needed because `values`
  is otherwise a name: `FROM (values JOIN u ON ...)` is a join of a table called `values`.
- **What is wrong today, and stays wrong.** In *expression* position nothing reaches that check, so
  `x IN (VALUES (1), (2))` falls through to the function-call parser and reports
  `function VALUES() is not supported` — the right exit code with a misleading message.
  - Update (2026-10-12, roadmap PR S4b): this paragraph first said S4b would fix the message by
    routing its two new `(` sites through `StartsOtherQuery`. It cannot, and S4b does not. `VALUES`
    is a *name* in an expression (divergence D21), so `VALUES(1)` is a call there; the unparser
    always parenthesises a negation, so claiming the word would print `-VALUES(a)` as
    `-(VALUES(a))` and read it back as a query, which costs the canonical form its round trip. Only
    a reserved word opens a nested query in an expression — `SELECT`, `WITH`, `FROM`, `TABLE` — and
    the misleading message stays until the form is answered. In `FROM`, where `VALUES` is no name,
    it is still refused as a query.
- **Nothing depends on it.** No ClickBench query and none of the 22 queries derived from TPC-H uses
  `VALUES`. There is no `Feature` for it in `tests/slt/supported_features.h`, so it has no
  `kGeneratorPending` entry and no two-sided gate: shipping it without an id would be the one kind of
  grammar growth that set exists to prevent.

## Decision

- **Postpone it.** The grammar keeps refusing `VALUES` in every position with exit code 4, and
  docs/sql-subset.md keeps saying so. No `Feature`, no `.slt` answers, no ratchet movement.
- **When it is wanted, it is a table expression, not a subquery kind.** The shape agreed here:
  - a `sql::ValuesTable { std::vector<std::vector<Expr>> rows; SourceSpan span; }` node, and a FROM
    source and a subquery body that can hold either a `SelectStatement` or one of these — so the two
    positions share one production, as they do in SQL;
  - every row must have the same arity, which is a parse error otherwise (as in DuckDB);
  - a column's type is the common type of its column of literals, by the same rules a `CASE` uses
    (ADR 0004 and ADR 0021), so a column mixing an integer and a decimal is DECIMAL and one mixing
    an integer and a string is an error rather than a silent cast;
  - the binder plans it as a one-batch `ScanNode` substitute — a new `LiteralNode` holding the
    materialised rows — which the executor streams once, so it needs no new operator;
  - `sql::ToSql` prints `VALUES (...), (...)`, and the depth limit counts its expressions as any
    other.
- **Row constructors are a separate id, and come first if both are wanted.**
  `(a, b) IN (VALUES (1, 2))` answers 0 in DuckDB and needs a row-constructor expression and
  row-wise comparison semantics, which are their own design; `VALUES` of one column is useful
  without them, so neither blocks the other.

## Consequences

- No query answer changes and no gate moves while this is postponed. The only code this ADR touches
  today is prose.
- Once implemented, `x IN (VALUES ...)` would answer wherever `x IN (...)` answers, so it would gain
  nothing for a list of literals. Its value is portability — SQL written for a dialect that requires
  the constructor — and, with more than one column, a literal dimension table in `FROM` without a
  Parquet file, which is what makes it worth recording rather than rejecting outright.
- A `Feature` (`kValuesTable`) would enter `kGeneratorPending` with the id, so the generator must
  learn to write it before the id can declare it supported, as every other grammar id does.

## Alternatives considered

- **Answer it as a `Kind` of ADR 0022's subquery node.** The node would hold either a statement or a
  row list, and every walk over `sql::ExprNode` would carry that distinction. It also would not
  cover `FROM (VALUES ...)`, which is the same production in SQL and the more useful of the two
  positions. Rejected: it buys the less valuable half at the cost of a baggier node.
- **Rewrite `x IN (VALUES (r1), (r2))` into `x IN (r1, r2)` in the parser.** It is exact for a
  single column of literals, needs no new node and no binder work. Rejected: it is a lie about the
  grammar — `VALUES ... UNION SELECT ...` cannot be rewritten, nor can a multi-column constructor in
  `FROM`, so the rewrite would have to refuse most of the form anyway, and a reader of the AST would
  no longer see what was written.
- **Add `VALUES` to the parser's rejected-keyword table**, beside `SEMI` and `EXISTS`. Rejected:
  `values` is a legal unquoted name, so a bare keyword entry would break `FROM values` and
  `FROM (values JOIN u ON ...)`. The existing lookahead on `(` is what makes the refusal precise,
  and S4b reuses it.
- **Answer `FROM (VALUES ...)` only, and keep the subquery position refused.** Rejected: the two are
  one production, and splitting them would leave the grammar's two halves disagreeing about what a
  parenthesised query is, which is the inconsistency S4b removes.

## Plan

One roadmap PR, after the work of ADR 0022 and ADR 0023, since both touch the same grammar and
binder paths:

- **V1, feat(sql,plan): the VALUES table constructor.** The `ValuesTable` node and its walks, the
  shared FROM-source and subquery-body production, the arity and common-type rules, the
  `LiteralNode` the binder plans it as, `kValuesTable` out of `kGeneratorPending` with the generator
  writing it, and `.slt` cases for both positions and for the arity and type errors. Row
  constructors, if wanted, are their own id before it.
