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
  subset that the parser recognizes (for example `GROUP BY`, `ORDER BY`, `JOIN`) produce `kUnsupported` errors with
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
