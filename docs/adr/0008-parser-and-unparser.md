# 8. Hand-written parser with an unparser and a round-trip property

Date: 2026-09-25

## Status

Accepted

## Context

- The supported SQL subset is small (one `SELECT` with aggregates, simple predicates and `LIMIT`) but must reject
  everything else precisely: a clean "unsupported" error with the position of the offending token, never a crash or
  a silent misparse.
- The `sql` module must stay free of Arrow and heavy dependencies so that it can be fuzzed on its own.
- Parser generators (bison, ANTLR) and borrowed parsers (DuckDB's or PostgreSQL's) bring build complexity, large
  grammars far beyond the subset and error messages that are hard to control.

## Decision

- The lexer and a recursive-descent parser are hand-written in `src/sql/`. They return
  `std::expected<SelectStatement, sql::ParseError>`; every AST node carries the source span it came from.
- Keywords are matched case-insensitively and are not reserved by the lexer. Constructs outside the implemented
  subset that the parser recognizes (for example `JOIN`, window functions) produce `kUnsupported` errors with
  the span of the first offending token.
- A canonical unparser, `sql::ToSql`, renders an AST as SQL text with upper-case keywords and single spaces, quoting
  identifiers only where the source quoted them.
- Properties that tests (and, later, a fuzzer) check for every accepted input:
  - round trip: `Parse(ToSql(ast))` equals `ast` ignoring spans (`EqualIgnoringSpans`);
  - idempotence: `ToSql(Parse(ToSql(ast)))` equals `ToSql(ast)`;
  - any input either parses or fails with an error; it never crashes or triggers undefined behavior.
- The parser grows with the subset described in [sql-subset.md](../sql-subset.md); every extension adds parser,
  unparser and round-trip tests in the same PR.

## Consequences

- Error messages and spans are fully under our control, and the grammar stays exactly as large as the subset.
- The round-trip property gives the fuzzer a strong oracle without any expected outputs.
- Every new construct needs code in three places (lexer or parser, AST, unparser), which is acceptable for a small
  grammar; a larger SQL surface in the future may need a new ADR.
- Update (scalar expressions): the parser accepts the whole expression grammar of
  [sql-subset.md](../sql-subset.md#grammar) ahead of the engine, and the binder rejects what it does not answer yet
  with `kUnsupported` at the same kind of span. Expression trees make the parser recursive; an expression depth limit
  (256 levels) bounds that recursion and every recursive walk of the tree.
- Update (joins and subqueries, 2026-10-02): [ADR 0022](0022-joins-and-query-blocks.md) is the new ADR that the
  larger SQL surface needs. It designs the grammar of joins and nested query blocks:
  - the FROM clause is a flat list of items, each with its connector (a comma, `CROSS JOIN`, `INNER JOIN ... ON` or
    `LEFT JOIN ... ON`), not a recursive join tree; nested joins (parenthesized ones, or a JOIN whose ON follows a
    later JOIN) stay unsupported;
  - the words that DuckDB refuses as an implicit table alias (`SEMI`, `ANTI`, `ASOF`, `POSITIONAL` and others) are
    never read as aliases: each gets its own `kUnsupported` error, or a syntax error where DuckDB gives one;
  - `sql::ToSql` prints every table alias as a quoted `AS "a"`, so DuckDB never reads an alias as a keyword;
  - nested query blocks (derived tables, CTEs and subqueries) count against the 256-level depth limit, together with
    the expressions around them;
  - every canonical form is SQL that DuckDB parses with the same meaning, because the oracle tests send `sql::ToSql`
    output to DuckDB.

  `JOIN` stops being the standard example of a recognized but unsupported construct: window functions replace it in
  the tests and goldens (roadmap PR S0).
- Update (FROM lists, 2026-10-05, roadmap PR S3): the parser accepts ADR 0022's flat FROM list (commas, `CROSS JOIN`,
  `[INNER] JOIN ... ON`, `LEFT [OUTER] JOIN ... ON`), table aliases and qualified names `t.x`; the binder rejects them
  with `kUnsupported` before any table resolves, until the binder answers them. Only a qualified name inside the
  arguments of a call with the wrong number of arguments is no such error: that call stays a bind error.
  - The 49 words that DuckDB 1.5.5 refuses as table aliases and antb1 does not reserve stay unreserved (they remain
    column, table and function names), but are never read as aliases. Their meaning depends on the place: after a FROM
    item and after an `ON` condition each is `kUnsupported` where DuckDB gives it a meaning there (told by the next one
    or two tokens: `SEMI JOIN`, `ASOF LEFT JOIN`, `AT (`, `TABLESAMPLE 10%`, ...), and a syntax error anywhere else
    after a FROM item or an `ON` condition, as in DuckDB. A harness test compares every DuckDB keyword as a table alias
    with DuckDB itself, so that a DuckDB update that gives a word a meaning is noticed.
  - A table alias may also be one of the reserved words `BETWEEN`, `EXISTS`, `INTERVAL` and `OVER`, with or without
    `AS`, and after `AS` a non-empty string literal, as in DuckDB. The longest keyword is now `AUTHORIZATION` (13
    letters).
  - The join keywords left the table of unsupported clauses: where DuckDB has no join (`SELECT 1 JOIN u`, a `JOIN`
    after `WHERE`, a bare `OUTER`) they are syntax errors. `GLOB` and `AT TIME ZONE` became unsupported operators in
    every expression, so they are never read as aliases either.
  - `sql::ToSql` prints every alias quoted after `AS`, `JOIN` as `INNER JOIN`, `LEFT OUTER JOIN` as `LEFT JOIN` and
    qualifiers as written. Copying, comparing, printing and destroying the list are loops, never recursions.
  - Update (2026-10-10, roadmap PR J2b): the binder answers commas, `CROSS JOIN`, `[INNER] JOIN ... ON`, table
    aliases and qualified names; only `LEFT [OUTER] JOIN ... ON` is still rejected with `kUnsupported`, until J6.
  - Update (2026-10-12, roadmap PR J6): the binder answers `LEFT [OUTER] JOIN ... ON` too, so every connector the
    flat FROM list carries is now bound. The other join kinds the parser recognises and rejects by name (`RIGHT`,
    `FULL`, `SEMI`, `ANTI`, `NATURAL`, `ASOF`, `POSITIONAL`, `JOIN ... USING`) are unchanged.
- Update (derived tables and WITH lists, 2026-10-09, roadmap PR S4a): a query is an optional WITH list and a block, and
  a derived table and each CTE hold a query of their own (a `sql::SelectStatement` in a heap box), so the AST nests
  statements. Every walk of it (copying, comparing, printing, `sql::Depth`, destruction) recurses once per nested
  query, which the depth limit bounds: a nested query is one level below its statement. The WITH list, the FROM list and
  column alias lists are walked in loops. `sql::ToSql` quotes CTE names and column aliases
  (`WITH "c"("x") AS (...)`). The binder rejects both forms with `kUnsupported` before any table resolves, until it
  answers them.
- Update (subqueries in expressions, 2026-10-12, roadmap PR S4b): one `sql::SubqueryExpr` holds all three forms --
  a scalar subquery used as a value, `[NOT] IN (query)` and `EXISTS (query)` -- with a `Kind`, since every walk over
  an expression treats them alike. `NOT EXISTS` is the prefix `NOT` over the `EXISTS` node, so the node itself is
  never negated. This puts a statement inside an *expression*, where S4a only put one inside a statement, so the
  walks now recurse through both kinds of edge, interleaved; the depth limit still bounds them, counting a
  subquery's query one level below its node. A `(` in an expression opens a nested query only for a query's own
  first words, so `((a + 1))` stays an expression in two pairs of parentheses and the inner pair of
  `((SELECT ...))` is the subquery; a parenthesized query that starts with neither `SELECT` nor `WITH` is refused at
  its first token, as in FROM. The binder refuses all three forms with `kUnsupported` at their own operator, before
  any name resolves.
