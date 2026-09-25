# SQL subset

antb1 accepts a single `SELECT` statement over Parquet tables. The goal of the first slice is a small, exactly
specified subset that behaves like DuckDB, which serves as the test oracle. Everything outside the implemented subset
is rejected cleanly with an error and a source position (normally exit code 4), never answered wrongly.

This page is the contract: a PR that changes SQL behavior updates it in the same PR.

## What works today

Only one query shape is implemented, ClickBench Q0:

```sql
SELECT COUNT(*) FROM <table> [;]
```

- `<table>` is a table registered with `--table NAME=PATH[,PATH|GLOB]` (an identifier or a `"quoted identifier"`,
  matched ASCII case-insensitively) or a string literal with a path or glob, e.g. `FROM '/data/hits_*.parquet'`.
- The answer comes from the Parquet footers (the sum of their row counts); no data pages are read. The result is one
  BIGINT column named `count_star()`, as in DuckDB.
- `--` line comments, `/* block */` comments and one trailing `;` are allowed.
- Other statements, any clause after the table (`WHERE`, `GROUP BY`, `LIMIT`, `JOIN`, ...) and any select list
  other than a single `COUNT(*)` fail with exit code 4 and point at the first unsupported token.
- Malformed SQL fails with exit code 1 (syntax error). So do, for now, a few valid forms that the parser does not
  recognize yet: an alias on `COUNT(*)`, a comma join and a subquery in `FROM`.

```bash
pixi run antb1 query -c "SELECT COUNT(*) FROM hits" --table hits=/data/clickbench/hits_0.parquet
pixi run antb1 explain -c "SELECT COUNT(*) FROM hits" --table hits=/data/clickbench/hits_0.parquet
```

Globs are allowed in the file-name part of a path only (`/data/hits_*.parquet`, not `/data/*/hits.parquet`) and
expand to a sorted file list. All files of a table must have the same schema.

## Target grammar

The slice PRs implement this grammar step by step. Keywords are case-insensitive.

```ebnf
statement   = query , [ ";" ] ;
query       = "SELECT" , select_list , "FROM" , table_ref , [ "WHERE" , predicate ] , [ "LIMIT" , integer ] ;
select_list = "*" | select_item , { "," , select_item } ;
select_item = ( agg_call | column_ref ) , [ [ "AS" ] , identifier ] ;
agg_call    = "COUNT" , "(" , "*" , ")"
            | ( "COUNT" | "SUM" | "AVG" | "MIN" | "MAX" ) , "(" , column_ref , ")" ;
table_ref   = identifier | string_literal ;
column_ref  = identifier ;
predicate   = comparison , { "AND" , comparison } ;
comparison  = column_ref , cmp_op , literal | literal , cmp_op , column_ref ;
cmp_op      = "=" | "<>" | "!=" | "<" | "<=" | ">" | ">=" ;
literal     = [ "-" ] , integer | [ "-" ] , decimal | string_literal | "DATE" , string_literal ;
```

Lexical rules: an `identifier` is a letter or `_` followed by letters, digits or `_`, or any text in double quotes
(`""` escapes a quote); a `string_literal` is text in single quotes (`''` escapes a quote); an `integer` is a
sequence of digits; a `decimal` is a number with a decimal point, an exponent or both (`1.5`, `.5`, `5.`, `1e3`).
Keywords are not reserved by the lexer. A literal-first comparison is normalized by the parser (`5 < c` becomes
`c > 5`).

Rules the binder will enforce:

- Aggregates cannot be mixed with plain columns, and `*` cannot be mixed with anything.
- SUM and AVG over VARCHAR or DATE are bind errors.
- A DATE column compared with a number is a bind error; a string literal compared with a DATE column must be
  `YYYY-MM-DD`.
- A string literal compared with a numeric column is a bind error.
- `LIMIT` requires a non-negative integer.

Outside the grammar, the slice parser will recognize common SQL and reject it with exit code 4 and a source span,
among others: `GROUP BY`, `ORDER BY`, `DISTINCT`, `HAVING`, `OFFSET`, joins, `LIKE`, `IN`, `CASE`, arithmetic,
function calls other than the five aggregates, `NULL` literals, `IS [NOT] NULL`, `OR` and `NOT`. Malformed SQL inside
the subset, such as `SELECT COUNT(*) FORM t`, is a syntax error with exit code 1.

## Types

| Parquet column | Arrow type as read | Engine type | Notes |
| --- | --- | --- | --- |
| INT32 annotated INT(16, signed) | int16 | SMALLINT | |
| INT32 | int32 | INTEGER | |
| INT64 | int64 | BIGINT | |
| INT32 annotated INT(16, unsigned) | uint16 | USMALLINT | read as DATE with `--clickbench` (EventDate) or `--column-type COL=DATE` |
| INT32 annotated DATE | date32 | DATE | |
| FLOAT | float | DOUBLE | widened on read |
| DOUBLE | double | DOUBLE | |
| BYTE_ARRAY, unannotated | binary | VARCHAR | compared byte-wise |
| BYTE_ARRAY annotated STRING (UTF8) | utf8 | VARCHAR | same engine representation as unannotated |
| DECIMAL(38, 0) | decimal128(38, 0) | HUGEINT | also the result type of integer SUM |
| anything else | – | unsupported | `antb1 schema` shows `unsupported(<type>)`; a query that uses the column is rejected |

`--column-type COL=DATE` reinterprets a USMALLINT or INTEGER column as days since 1970-01-01; `--clickbench` is a
shortcut for `EventDate`. Both apply to every table (registered or opened with `FROM 'path'`) that has a column with
exactly that name; for now `COL` is matched case-sensitively, and tables without the column are left unchanged.

## Semantics

The semantics follow DuckDB ([ADR 0004](adr/0004-types-null-overflow-semantics.md)). Items marked "slice" are
decided and documented here before the code lands.

- Identifiers: table and column names match ASCII case-insensitively, quoted identifiers included (as in DuckDB).
  Registering two tables whose names differ only in case is a usage error (exit code 2). A name that matches two
  columns differing only in case is a bind error (slice).
- Literals (slice): comparisons of a column with a literal are folded exactly at bind time, never through a lossy
  cast. A literal outside the column type's range turns the comparison into constant true or false for non-NULL
  values; NULL values still compare as NULL. A decimal literal compared with an integer column becomes an
  equivalent integer comparison: `c > 1.5` becomes `c >= 2`, and `c = 1.5` is never true.
- COUNT: `COUNT(*)` counts rows; `COUNT(col)` counts non-NULL values. Both return BIGINT.
- SUM (slice): over integer columns it accumulates in 128 bits and returns HUGEINT (decimal128(38, 0)), exactly like
  DuckDB, so it never overflows or wraps. Over DOUBLE it returns DOUBLE.
- AVG (slice): over integer columns the sum accumulates exactly in 128 bits and is divided by the count once at the
  end, so the DOUBLE result is accurate to about one ulp (the oracle tests use a tight relative tolerance). Over
  DOUBLE it returns DOUBLE.
- MIN and MAX (slice): return the input type; VARCHAR compares byte-wise and DATE chronologically. Under `WHERE` only
  the selected rows are considered.
- NULL: aggregates skip NULLs. Over zero input rows `COUNT` returns 0 and `SUM`, `AVG`, `MIN` and `MAX` return NULL.
  A predicate that evaluates to NULL rejects the row.
- VARCHAR: values are raw bytes. Unannotated BYTE_ARRAY columns (as in ClickBench) are VARCHAR and are never
  validated as UTF-8.
- Execution: single-threaded, files and row groups in order, 64Ki-row batches; results are deterministic.

## Output formats

`antb1 query --format table|csv|json` (default `table`). Every format renders values with the same canonical
formatter:

- integers (HUGEINT included) exactly; DOUBLE as the shortest round-trip form (`nan`, `inf`, `-inf` for non-finite
  values); DATE as `YYYY-MM-DD`; VARCHAR as its raw bytes;
- NULL as `NULL` in `table`, an empty field in `csv` and `null` in `json`;
- `csv` has a header row and RFC 4180 quoting, with LF line endings; `json` is an array of objects, where numbers are
  JSON numbers except HUGEINT and non-finite doubles, which are strings (exactness), and invalid UTF-8 bytes in
  strings are written as the text `\xHH`.

`--timing` prints the elapsed seconds in fixed-point notation (for example `0.003620`) as the last line on stderr.

## Exit codes

| Code | Meaning | Examples |
| --- | --- | --- |
| 0 | success | |
| 1 | query error: syntax, bind or execution error | `SELECT COUNT(*) FORM t`; an unknown table |
| 2 | usage error | unknown option; neither or both of `-c` and `-f`; a malformed `--table` or `--column-type`; a column that `--column-type` cannot read as DATE; a table name registered twice |
| 3 | I/O error | a missing or unreadable file; not a Parquet file; schemas that differ; a glob that matches nothing |
| 4 | unsupported: valid-looking SQL outside the supported subset | `WHERE` or `GROUP BY` today |
| 70 | internal error: anything else, which is a bug | an uncaught exception; an Arrow `NotImplemented` or type error without SQL context |

Exit code 4 is used only for errors that the parser or binder marks as unsupported (`SqlErrorDetail` kind
`kUnsupported`). Any other `NotImplemented` or type error from Arrow maps to 70, so a missing feature can never hide
an internal bug ([ADR 0005](adr/0005-error-boundary.md)).

Errors go to stderr as `antb1: <kind> error: <message>`, followed by the line, column and a caret marker when the
error has a source position. With `--format json` the error is one JSON object on stderr instead (command-line syntax
errors such as an unknown option are always reported as plain text by CLI11):

```json
{"error":{"kind":"bind","message":"table 'nope' does not exist","offset":21,"length":4,"line":1,"column":22}}
```

The kinds are `parse`, `unsupported`, `bind`, `io`, `execution` and `internal`.

## Divergences from DuckDB

Every intentional difference from DuckDB is registered here, with the way the tests handle it. The oracle tests
compare against DuckDB, so an unregistered difference is a bug.

| ID | Area | antb1 | DuckDB | How tests handle it |
| --- | --- | --- | --- | --- |
| – | – | none registered yet | – | – |

Known candidates, to be confirmed and registered by the PR that implements the feature:

- A string literal compared with a numeric column is a bind error in antb1; DuckDB casts the literal.
- FLOAT columns are read as DOUBLE, so values print with double precision; the oracle casts FLOAT to DOUBLE.
- Arrow's `min_max` ignores NaN, while DuckDB orders NaN above every other value; test fixtures contain no NaN.

## ClickBench status

The target of the first slice is ClickBench Q0, Q1, Q2, Q3 and Q6, run with `--clickbench`. Query numbers follow
ClickBench's DuckDB/Parquet query file (0-based); the query text itself is never committed. The data tests (added
in a later PR) compare antb1 with DuckDB on the first partition of the `hits` dataset and keep a ratchet of verified
passes in sync with this table; the PR that changes the pass set updates both.

| Query | Status | Notes |
| --- | --- | --- |
| Q0 | pass | answered from the Parquet footer row counts |
| Q1 | target | slice |
| Q2 | target | slice |
| Q3 | target | slice |
| Q6 | target | slice |
| Q19 | not a target | fits the grammar, so it may pass incidentally; recorded only if verified |
| all others | out of scope | need GROUP BY, ORDER BY, LIKE, functions or other rejected syntax; they must fail cleanly |
