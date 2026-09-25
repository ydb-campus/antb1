# 5. Error boundary and exit codes

Date: 2026-09-25

## Status

Accepted

## Context

- The front end (`common`, `sql`) is kept free of Arrow so that it builds fast and can be fuzzed in isolation.
  Everything from `plan` upward works with Arrow types and Arrow's `Status`/`Result` error model.
- Arrow and Parquet report some errors with C++ exceptions (`parquet::ParquetException`), and CLI11 reports parse
  errors with exceptions.
- Users, scripts and the ClickBench harness need to tell apart "your query is wrong", "this is not supported yet",
  "the file is broken" and "antb1 has a bug". An unsupported feature must never hide an internal error.

## Decision

- `common` and `sql` return `std::expected<T, E>`; the parser's error is `sql::ParseError` with a kind (syntax or
  unsupported), a message and a source span.
- `plan` is the boundary: `plan::ToArrowStatus` converts a `sql::ParseError` into an `arrow::Status` that carries a
  `plan::SqlErrorDetail` (kind parse, unsupported or bind, plus the span). From `plan` upward every function returns
  `arrow::Status` or `arrow::Result<T>`.
- No exception crosses a module boundary. `io` catches Parquet exceptions and returns `IOError`; `cli` handles CLI11
  exceptions; the `main` function catches anything left and exits with 70.
- `ANTB1_CHECK` and `ANTB1_DCHECK` abort on broken invariants (programming errors) and are never used for user
  input.
- The CLI maps errors to exit codes:

  | Code | Meaning |
  | --- | --- |
  | 0 | success |
  | 1 | query error: parse, bind or execution error |
  | 2 | usage error |
  | 3 | I/O error |
  | 4 | unsupported: only an error that carries `SqlErrorDetail` kind `kUnsupported` |
  | 70 | internal error: every other failure, including Arrow `NotImplemented` or `TypeError` without SQL context |

- Errors print to stderr as `antb1: <kind> error: <message>` with line, column and a caret marker when a span is
  known, or as one JSON object with `--format json`.

## Consequences

- The Arrow-free front end can be tested and fuzzed without linking Arrow.
- Error spans survive the boundary, so every parse, bind and unsupported error points at the offending SQL text.
- A missing Arrow kernel registration or a dispatch bug surfaces as exit code 70, not as a harmless-looking
  "unsupported", which keeps the ClickBench "fails cleanly" check honest.
- New error kinds must be mapped in `cli` and covered by tests of the exit-code mapping.
