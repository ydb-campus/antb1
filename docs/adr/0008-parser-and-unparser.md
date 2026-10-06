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
