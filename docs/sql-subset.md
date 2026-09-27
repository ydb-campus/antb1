# SQL subset

antb1 accepts a single `SELECT` statement over Parquet tables. The goal of the first slice is a small, exactly
specified subset that behaves like DuckDB, which serves as the test oracle. Everything outside the implemented subset
is rejected cleanly with an error and a source position (normally exit code 4), never answered wrongly.

This page is the contract: a PR that changes SQL behavior updates it in the same PR.

## What works today

Every query of the [grammar](#grammar) below runs: global and grouped (`GROUP BY`) aggregates, `COUNT(DISTINCT ...)`
included, projections (`*` or columns), a `WHERE` conjunction of `column <op> literal` comparisons,
`column [NOT] LIKE 'pattern'` and `column [NOT] IN (literal, ...)`, `ORDER BY`, `LIMIT` and `OFFSET`, over one table
of Parquet files. This covers 35 of the 43 ClickBench queries (see [ClickBench status](#clickbench-status)).

```sql
SELECT COUNT(*), SUM(ResolutionWidth) AS width, AVG(UserID), MAX(EventDate) FROM hits WHERE IsMobile = 1
SELECT WatchID, URL FROM hits WHERE RegionID < 300 AND SearchPhrase <> '' LIMIT 10
SELECT COUNT(*) FROM events WHERE url LIKE '%shop%' AND title NOT LIKE 'Promo_%'
SELECT RegionID, COUNT(*) AS n, AVG(ResolutionWidth) FROM hits WHERE IsMobile = 1 GROUP BY RegionID
SELECT OS, COUNT(*) AS n FROM hits GROUP BY OS ORDER BY n DESC, MAX(EventDate) NULLS FIRST LIMIT 10 OFFSET 5
SELECT * FROM '/data/hits_*.parquet' LIMIT 5
```

- The table is registered with `--table NAME=PATH[,PATH|GLOB]` (an identifier or a `"quoted identifier"`, matched
  ASCII case-insensitively) or given as a string literal with a path or glob, e.g. `FROM '/data/hits_*.parquet'`.
- Only the columns a query references are decoded. `COUNT(*)` without `WHERE` is answered from the Parquet footers
  (no data page is read); under `WHERE` it counts the rows the filter selects without copying them.
- Result names and types follow DuckDB ([Binding](#binding)); values follow the [Semantics](#semantics) below.
- `--` line comments, `/* block */` comments and one trailing `;` are allowed.
- SQL outside the grammar (`HAVING`, `JOIN`, `OR`, functions, ...) fails with exit code 4 and points at the first
  unsupported token. Malformed SQL (a syntax error) and SQL that is wrong for the table (a bind error) fail with exit
  code 1.

```bash
pixi run antb1 query -f query.sql --table hits=/data/clickbench/hits_0.parquet --clickbench
pixi run antb1 explain -f query.sql --table hits=/data/clickbench/hits_0.parquet --clickbench
```

Globs are allowed in the file-name part of a path only (`/data/hits_*.parquet`, not `/data/*/hits.parquet`) and
expand to a sorted file list. All files of a table must have the same schema (divergence D1 below).

## Grammar

The parser accepts this grammar, the binder checks it against the tables and the executor runs it. Keywords are
case-insensitive.

```ebnf
statement   = query , [ ";" ] ;
query       = "SELECT" , select_list , "FROM" , table_ref , [ "WHERE" , predicate ] ,
              [ "GROUP" , "BY" , column_ref , { "," , column_ref } ] ,
              [ "ORDER" , "BY" , order_item , { "," , order_item } ] ,
              [ limit_offset ] ;
limit_offset = "LIMIT" , integer , [ "OFFSET" , integer ] | "OFFSET" , integer , [ "LIMIT" , integer ] ;
select_list = "*" | select_item , { "," , select_item } ;
select_item = ( agg_call | column_ref ) , [ [ "AS" ] , identifier ] ;
agg_call    = "COUNT" , "(" , "*" , ")"
            | "COUNT" , "(" , "DISTINCT" , column_ref , ")"
            | ( "COUNT" | "SUM" | "AVG" | "MIN" | "MAX" ) , "(" , column_ref , ")" ;
order_item  = ( agg_call | column_ref ) , [ "ASC" | "DESC" ] , [ "NULLS" , ( "FIRST" | "LAST" ) ] ;
table_ref   = identifier | string_literal ;
column_ref  = identifier ;
predicate   = comparison , { "AND" , comparison } ;
comparison  = column_ref , cmp_op , literal | literal , cmp_op , column_ref
            | column_ref , [ "NOT" ] , "LIKE" , string_literal
            | column_ref , [ "NOT" ] , "IN" , "(" , literal , { "," , literal } , ")" ;
cmp_op      = "=" | "<>" | "!=" | "<" | "<=" | ">" | ">=" ;
literal     = [ "-" ] , integer | [ "-" ] , decimal | string_literal | "DATE" , string_literal ;
```

Lexical rules: an `identifier` is a letter or `_` followed by letters, digits or `_`, or any text in double quotes
(`""` escapes a quote); a `string_literal` is text in single quotes (`''` escapes a quote); an `integer` is a
sequence of digits; a `decimal` is a number with a decimal point, an exponent or both (`1.5`, `.5`, `5.`, `1e3`).
Keywords are not reserved by the lexer. A literal-first comparison is normalized by the parser (`5 < c` becomes
`c > 5`).

`GROUP BY` and `ORDER BY` positions (`ORDER BY 2`), `ALL`, constants and expressions are rejected by the parser, and
so are `SUM`, `AVG`, `MIN` and `MAX` with `DISTINCT`.

Outside the grammar, the parser recognizes common SQL and rejects it with exit code 4 and a source span, among others:
`SELECT DISTINCT`, `HAVING`, joins, `ILIKE`, `LIKE ... ESCAPE`, `LIKE` outside `WHERE` or with a column as the
pattern, `IN` with a subquery, a column in its list or a literal on its left, `CASE`, arithmetic, function calls other
than the five aggregates, `NULL` literals, `IS [NOT] NULL`, `OR` and `NOT`. Malformed SQL inside the subset, such as
`SELECT COUNT(*) FORM t`, is a syntax error with exit code 1.

## Binding

The binder (`plan::Bind`) resolves the statement against the table and builds the logical plan. A bind error has exit
code 1 and points at the offending name, call or literal; the first error in query order wins (the table, then the
select list, `WHERE`, the `GROUP BY` names, the grouping rule below, `LIMIT`, `OFFSET` and the `ORDER BY` items).

- Names: table and column names match ASCII case-insensitively, quoted identifiers included (as in DuckDB). An
  unknown table or column is a bind error, and so is a name that matches two columns differing only in case. A
  column of an unsupported type fails with exit code 4 wherever it is referenced (by `SELECT *` too).
- Select list: `*` alone, plain columns, or aggregates. Without `GROUP BY`, aggregates (in the select list or in
  `ORDER BY`) cannot be mixed with plain columns: a bind error at the first plain column (or at `*`).
- `GROUP BY`: each name is a table column, or else the alias of a plain column of the select list (as in DuckDB; an
  alias of an aggregate is a bind error); a key repeated, also in another spelling, is one key, and a key need not be
  selected. Every plain column of the select list, and every column of `SELECT *`, must be a key: otherwise a bind
  error at that column (`column 'b' must appear in the GROUP BY clause or be inside an aggregate function`).
- Result types: `COUNT(*)`, `COUNT(col)` and `COUNT(DISTINCT col)` are BIGINT; `SUM` of an integer column is
  HUGEINT and of a DOUBLE column DOUBLE; `AVG` is DOUBLE; `MIN` and `MAX` have the type of their column. `SUM` and
  `AVG` of a VARCHAR or DATE column are bind errors.
- Result names follow DuckDB: a plain column is named as declared in the table (`SELECT regionid` gives
  `RegionID`); an aggregate is named `count_star()`, `count(x)`, `count(DISTINCT x)`, `sum(x)`, `avg(x)`, `min(x)`
  or `max(x)`, with the argument as written in the query, double-quoted when it is not a plain identifier or is a
  reserved word (`sum("from")`). An alias replaces the name.
- `ORDER BY`: a name is the alias of a select item first (the last item with that alias, as in DuckDB; also when a
  table column has the same name), else a table column; an aggregate call is computed like a select-list aggregate.
  A projection may order by any column of the table, selected or not. A grouped query orders by its keys and
  aggregates (an aggregate the select list lacks is computed and not returned); any other column is a bind error
  (`column 'b' must appear in the GROUP BY clause or be inside an aggregate function`). An aggregate query without
  `GROUP BY` has one row, so it may order by aggregates only. A key that repeats an earlier one changes nothing.
- `LIMIT n` and `OFFSET m` take integers from 0 to 9223372036854775807, in either order.

Literals in `WHERE` must fit the column's type; any other combination is a bind error that points at the literal:

| Column type | Literals | Compared as |
| --- | --- | --- |
| SMALLINT, INTEGER, BIGINT, USMALLINT, HUGEINT | integer, decimal | exactly, after folding (below) |
| DOUBLE | integer, decimal | the nearest double, as in DuckDB for a DOUBLE column; beyond the double range `inf` or `-inf`, below the smallest subnormal `0`. A column stored as FLOAT is compared as DuckDB compares it: an integer or DECIMAL literal becomes the FLOAT that DuckDB casts it to, with DuckDB's rounding (`0.1` is `0.1F`; `16777217.5` and some long spellings of `0.1`, such as 16 or 24 decimals, are not the nearest FLOAT, and HUGEINT literals are rounded through a double), beyond the FLOAT range `inf` or `-inf`; a number DuckDB types as DOUBLE compares with the nearest double |
| VARCHAR | string | bytes; `LIKE` and `NOT LIKE` take a string pattern (only a VARCHAR column: LIKE on another type is a bind error, as in DuckDB) |
| DATE | string, `DATE` string | a date written exactly `YYYY-MM-DD` (years 0000 to 9999) that exists in the calendar |

A comparison of an integer column with a number is folded exactly at bind time, never through a lossy cast:

- A decimal that is not an integer becomes the nearest integer on the side the comparison keeps: `c > 1.5` and
  `c >= 1.5` become `c >= 2`, `c < -1.5` and `c <= -1.5` become `c <= -2`. `c = 1.5` is never true and `c <> 1.5` is
  true for every value. Decimals with an integer value compare as that integer (`c = 2.0`, `c > 1e3`).
- A number that DuckDB types as DOUBLE, one with an exponent (`1e3`) or a decimal of more than 38 digits (leading
  zeros count), is first rounded to the nearest double, as in DuckDB, and that double is folded exactly:
  `c = 1.0000000000000000000001e0` is `c = 1` (divergence D7 for BIGINT values beyond 2^53).
- A number outside the column type's range (SMALLINT -32768 to 32767, USMALLINT 0 to 65535, INTEGER and BIGINT
  their 32-bit and 64-bit ranges, HUGEINT -(10^38 - 1) to 10^38 - 1) makes the comparison true for every value or
  for none, depending on the operator: `smallint_col < 40000` is always true, `usmallint_col = -1` never.
- A comparison that is true for every value still rejects NULL: EXPLAIN shows it as `c IS NOT NULL`. One that is
  never true shows as `FALSE` and reads no column.

## Logical plans and EXPLAIN

A bound query is a tree of logical nodes: `Scan` (reads fields of the table), `Filter` (the `WHERE` conjunction),
`Project` (`*` or the plain columns), `Aggregate` (the aggregates, one output row) or `GroupAggregate` (`GROUP BY`:
the keys, then the aggregates, one row per group; a `Project` above it restores the select order), `Sort`
(`ORDER BY`, below the `Project`, so it can use columns and aggregates the query does not return) and `Limit` (with
its offset). A rule optimizer then rewrites it:

- `Limit` moves below `Project`: a `Project` keeps every row, so the `Limit` copies only the rows it keeps and ends
  up right above a `Sort`, which the executor runs as a top-N (it keeps only `limit + offset` rows while it reads);
- projection pruning: `Scan` reads only the columns that the nodes above it use (none for a bare `COUNT(*)`);
- `COUNT(*)` alone without `WHERE`, over a table whose row count is known without scanning (every Parquet table),
  becomes `RowCount`, answered from the footers.

`antb1 explain` prints the optimized plan: an `Output:` line with the result names and types, then one line per
node from the root down, each input indented by two more spaces. Names that are not plain identifiers are
double-quoted, and bytes outside printable ASCII are written as `\xHH`:

```text
Output: count_star():BIGINT width:HUGEINT
Aggregate COUNT(*), SUM(ResolutionWidth)
  Filter IsMobile = 1 AND UserAgent >= 2 AND RegionID IS NOT NULL
    Scan table=t source=parquet(files=1, rows=10000) columns=[RegionID, UserAgent, ResolutionWidth, IsMobile]
```

```text
Output: RegionID:INTEGER c:BIGINT
Project RegionID, c
  Limit 10 OFFSET 5
    Sort c DESC NULLS LAST, "max(EventTime)" ASC NULLS FIRST
      GroupAggregate keys=[RegionID] COUNT(*), MAX(EventTime)
        Scan table=t source=parquet(files=1, rows=10000) columns=[EventTime, RegionID]
```

## Types

| Parquet column | Arrow type as read | Engine type | Notes |
| --- | --- | --- | --- |
| INT32 annotated INT(16, signed) | int16 | SMALLINT | |
| INT32 | int32 | INTEGER | |
| INT64 | int64 | BIGINT | |
| INT32 annotated INT(16, unsigned) | uint16 | USMALLINT | read as DATE with `--clickbench` (EventDate) or `--column-type COL=DATE` |
| INT32 annotated DATE | date32 | DATE | |
| FLOAT | float | DOUBLE | widened exactly on read; `WHERE` compares like DuckDB (see Binding), results are DOUBLE (divergence D11) |
| DOUBLE | double | DOUBLE | |
| BYTE_ARRAY, unannotated | binary | VARCHAR | compared byte-wise |
| BYTE_ARRAY annotated STRING (UTF8) | utf8 | VARCHAR | same engine representation as unannotated |
| DECIMAL(38, 0) | decimal128(38, 0) | HUGEINT | also the result type of integer SUM |
| anything else | – | unsupported | `antb1 schema` shows `unsupported(<type>)`; a query that uses the column is rejected |

`--column-type COL=DATE` reinterprets a USMALLINT or INTEGER column as days since 1970-01-01; `--clickbench` is a
shortcut for `EventDate`. Both apply to every table (registered or opened with `FROM 'path'`) that has a column with
that name, matched ASCII case-insensitively like every column name (all columns it matches must be readable as DATE),
and tables without the column are left unchanged. The test oracle handles `FROM 'path'` differently (divergence D2
below).

## Semantics

The semantics follow DuckDB ([ADR 0004](adr/0004-types-null-overflow-semantics.md)).

- Identifiers: table and column names match ASCII case-insensitively, quoted identifiers included (as in DuckDB).
  Registering two tables whose names differ only in case is a usage error (exit code 2). A name that matches two
  columns differing only in case is a bind error.
- Literals: comparisons of a column with a literal are folded exactly at bind time, never through a lossy cast
  ([Binding](#binding)). A literal outside the column type's range turns the comparison into constant true or false
  for non-NULL values; NULL values still compare as NULL. A decimal literal compared with an integer column becomes
  an equivalent integer comparison: `c > 1.5` becomes `c >= 2`, and `c = 1.5` is never true.
- WHERE: the comparisons are evaluated with Arrow's comparison kernels and combined with Kleene AND; a row passes
  only when every comparison is true, so a comparison that is NULL rejects it. VARCHAR compares byte-wise and DATE
  chronologically. A predicate folded to never-true reads no data at all.
- IN: `c IN (v1, v2, ...)` is `c = v1 OR c = v2 OR ...` with Kleene logic, and `c NOT IN (...)` its negation, so a
  NULL value rejects the row for both. Each value is typed and folded exactly like `c = v` ([Binding](#binding)); a
  value that no column value can equal (out of the column type's range, or not an integer for an integer column) is
  dropped, and without values `IN` is `FALSE` and `NOT IN` is `IS NOT NULL`. DuckDB gives the list one type: when a
  number in it types as DOUBLE (an exponent, or more than 38 digits), every number is read as a double, so an integer
  column compares with the nearest doubles (divergence D7) and a FLOAT column in DOUBLE (`f IN (0.1, 1e0)` does not
  match the FLOAT `0.1`, while `f IN (0.1, 2)` does). NaN is not equal to NaN, as for `=`
  (divergence D10).
- LIKE: `%` matches any sequence of characters (none included), `_` exactly one character, and every other byte
  itself; the match is case-sensitive and there is no escape character (`\` is a literal byte), as in DuckDB. A
  character is a UTF-8 sequence; in bytes that are not UTF-8 (which DuckDB refuses to read as VARCHAR) each byte is
  one character. `NOT LIKE` is the negation; a NULL value rejects the row for both. A pattern of only `%` folds:
  `LIKE` to `IS NOT NULL`, `NOT LIKE` to `FALSE`. Patterns without `_` are matched by their literal segments (a
  prefix, a suffix and substrings in order), the others by backtracking.
- COUNT: `COUNT(*)` counts rows; `COUNT(col)` counts non-NULL values; `COUNT(DISTINCT col)` counts distinct non-NULL
  values, with DOUBLE `-0.0` equal to `0.0` and every NaN one value (as DuckDB groups them) and VARCHAR by bytes. All
  return BIGINT, 0 over no values.
- SUM: over integer columns it accumulates in 128 bits and returns HUGEINT (decimal128(38, 0)), exactly like
  DuckDB, so it never overflows or wraps; Arrow's 64-bit `sum` kernel is never used. A sum outside HUGEINT's range
  (only possible over a HUGEINT column) is an execution error (divergence D9). Over DOUBLE it returns DOUBLE, adding
  the values in row order.
- AVG: over integer columns the sum accumulates exactly in 128 bits and is divided by the count once at the end, so
  the DOUBLE result is accurate to about one ulp (the oracle tests use a tight relative tolerance). Over DOUBLE it
  returns DOUBLE.
- MIN and MAX: return the input type; VARCHAR compares byte-wise and DATE chronologically. They use Arrow's
  `min_max` kernel, over only the selected rows under `WHERE` (divergence D10 for NaN).
- NULL: aggregates skip NULLs. Over zero input rows `COUNT` returns 0 and `SUM`, `AVG`, `MIN` and `MAX` return NULL.
  A predicate that evaluates to NULL rejects the row.
- GROUP BY: one row per distinct combination of the keys; the aggregates of a group are exactly those of the same
  rows without `GROUP BY`. NULL is a key value (all NULLs form one group). A DOUBLE key groups `-0.0` with `0.0` and
  every NaN together, as DuckDB does, and the group shows the key as first seen. Over zero input rows there is no
  group, so no row. Groups come in no particular order (deterministic in antb1, not the order of first appearance),
  and the tests compare grouped results without regard to order; with `LIMIT` any groups are a right answer.
- ORDER BY: rows in the order of the first key, ties broken by the next ones. `ASC` is the default; NULLs come last
  unless `NULLS FIRST` is written, for `ASC` and `DESC` alike (DuckDB's default). VARCHAR orders by bytes, DATE
  chronologically, and DOUBLE with `-0.0` equal to `0.0` and NaN above every number (equal to NaN), as in DuckDB.
  Rows with equal keys come in no particular order (antb1 keeps their input order); the tests accept any order of
  tied rows, and any of the tied rows at the edges of a `LIMIT`/`OFFSET` window.
- LIMIT and OFFSET: `OFFSET m` skips the first `m` rows, then `LIMIT n` keeps at most `n`. With `ORDER BY` that is a
  window of the order. Without it, the first rows in file and row group order: the scan stops as soon as the window
  is out. `LIMIT 0` returns no row, also for an aggregate. Without `ORDER BY` a projection returns its rows in file
  order too, but SQL does not promise an order, and the tests compare such results without regard to order (with
  `LIMIT` or `OFFSET`, any rows of the full answer are right).
- VARCHAR: values are raw bytes. Unannotated BYTE_ARRAY columns (as in ClickBench) are VARCHAR and are never
  validated as UTF-8.
- Execution: single-threaded, files and row groups in order, 64Ki-row batches; results are deterministic.

## Output formats

`antb1 query --format table|csv|json` (default `table`). Every format renders values with the same canonical
formatter:

- integers (HUGEINT included) exactly; DOUBLE as the shortest round-trip form (`nan`, `inf`, `-inf` for non-finite
  values); DATE as `YYYY-MM-DD`, and like DuckDB outside years 1 to 9999: `YYYY-MM-DD (BC)` before year 1, more year
  digits after 9999, `infinity` and `-infinity` for DuckDB's sentinel day numbers; VARCHAR as its raw bytes;
- NULL as `NULL` in `table`, an empty field in `csv` and `null` in `json`;
- `csv` has a header row and RFC 4180 quoting, with LF line endings; `json` is an array of objects, where numbers are
  JSON numbers except HUGEINT and non-finite doubles, which are strings (exactness), and every byte of ill-formed
  UTF-8 in strings (RFC 3629, so also overlong forms, surrogates and code points above U+10FFFF) is written as the
  text `\xHH` with lowercase hex digits, so the output is always valid UTF-8; `table` and `csv` write the raw bytes.

`--timing` prints the elapsed seconds in fixed-point notation (for example `0.003620`) as the last line on stderr.

## Exit codes

| Code | Meaning | Examples |
| --- | --- | --- |
| 0 | success | |
| 1 | query error: syntax, bind or execution error | `SELECT COUNT(*) FORM t`; an unknown table or column; `SUM` of a VARCHAR column; a `SUM` outside HUGEINT's range |
| 2 | usage error | unknown option; neither or both of `-c` and `-f`; a malformed `--table` or `--column-type`; a column that `--column-type` cannot read as DATE; a table name registered twice |
| 3 | I/O error | a missing or unreadable file; not a Parquet file; schemas that differ; a glob that matches nothing |
| 4 | unsupported: valid-looking SQL outside the supported subset | `HAVING`; `OR`; a function call; `ORDER BY 2`; `SUM(DISTINCT ...)`; a column of an unsupported type |
| 70 | internal error: anything else, which is a bug | an uncaught exception; an Arrow `NotImplemented` or type error without SQL context |

Exit code 4 is used only for errors that the parser, the binder or the physical planner marks as unsupported
(`SqlErrorDetail` kind `kUnsupported`). Any other `NotImplemented` or type error from Arrow maps to 70, so a
missing feature can never hide an internal bug ([ADR 0005](adr/0005-error-boundary.md)).

Errors go to stderr as `antb1: <kind> error: <message>`, followed by the line, column and a caret marker when the
error has a source position. With `--format json` the error is one JSON object on stderr instead (command-line syntax
errors such as an unknown option are always reported as plain text by CLI11):

```json
{"error":{"kind":"bind","message":"table 'nope' does not exist","offset":21,"length":4,"line":1,"column":22}}
```

The kinds are `parse`, `unsupported`, `bind`, `io`, `execution`, `internal` and, for command-line errors (exit code 2),
`usage`. Strings in the error object (and in the `antb1 bench` report) are escaped like `--format json` values, so it is
valid UTF-8 whatever bytes the SQL holds.

## Divergences from DuckDB

Every intentional difference from DuckDB is registered here, with the way the tests handle it. The oracle tests
compare against DuckDB, so an unregistered difference is a bug.

| ID | Area | antb1 | DuckDB | How tests handle it |
| --- | --- | --- | --- | --- |
| D1 | Files with different schemas | a table (or a `FROM '<glob>'`) whose files have different Parquet schemas is an I/O error, exit code 3: `schema of '<file>' differs from '<first file>'` | `read_parquet` over the same files reads them and answers | an `onlyif antb1` record in `tests/slt/cases/basic/errors.slt`; `integration.ParquetErrors.*` (mismatched and mixed schemas); the CLI golden `io_schema_mismatch`; every table in `tests/slt/tables.txt` has a single schema |
| D2 | Column-type overrides | `--clickbench` and `--column-type COL=DATE` apply to every table with that column, including tables opened with `FROM '<path>'` | the oracle applies `make_date(EventDate)` only to the named tables with the `clickbench` option in `tests/slt/tables.txt`; `FROM '<path>'` reads the raw integers | the runner rejects a table list where some tables with an `EventDate` column have the option and others do not; the random generator never reads an overridden column through `FROM '<path>'`; `.slt` records read `EventDate` only through table names |
| D3 | Literal types | a string literal compared with a numeric column is a bind error | casts the string to the column's type | `onlyif antb1` records in `tests/slt/cases/basic/bind_errors.slt`; the query generator writes numbers for numeric columns |
| D4 | Literal types | a number or a `DATE` literal compared with a VARCHAR column is a bind error | casts the column's values at run time (a conversion error unless every value converts) | as D3; the generator writes strings for VARCHAR columns |
| D5 | Date literals | a date must be written exactly `YYYY-MM-DD` | also accepts `2013-7-1`, surrounding spaces and a time of day | as D3; the generator writes `YYYY-MM-DD` |
| D6 | AVG of DATE | `AVG` of a DATE column is a bind error | returns a TIMESTAMP | as D3; the generator averages numeric columns only |
| D7 | DOUBLE literals and BIGINT | a number that DuckDB types as DOUBLE (an exponent, or more than 38 digits) is rounded to the nearest double like in DuckDB, then compared exactly with the integer column; in an `IN` list with such a number every value is rounded so | converts BIGINT (and HUGEINT) values to DOUBLE for the comparison, so values beyond 2^53 compare rounded: `i64 >= 9223372036854775808e0` holds for `9223372036854775807` | the `.slt` records with such literals avoid BIGINT values beyond 2^53 (`tests/slt/cases/where/folding.slt`); `plan.ApproximateNumbers/FoldThroughBinderTest.*` pins antb1's folding; the generator writes no exponents |
| D8 | Result names | an aggregate's argument is quoted when it is not a plain identifier or is a reserved word | also quotes non-reserved keywords (`sum("year")`) | the tests compare values and types, not names |
| D9 | HUGEINT range | HUGEINT is decimal128(38, 0): a `SUM` outside -(10^38 - 1) to 10^38 - 1 is an execution error (exit code 1). An integer SUM over BIGINT or smaller types cannot reach it | HUGEINT holds -(2^127 - 1) to 2^127 - 1 | no fixture has a HUGEINT column; `exec.AggregateStateTest.HugeIntSumIsCheckedAgainstTheRange` checks the error |
| D10 | NaN | MIN and MAX ignore NaN like Arrow's `min_max`, whatever the batch and file boundaries: they return NaN only when every selected non-NULL value is NaN (so only MAX over NaN and other values differs from DuckDB). Arrow's comparison kernels follow IEEE 754: NaN compares unequal to everything, so `d > 1` and `d >= 1` are false for NaN, and `d IN (...)` never matches it | orders NaN above every other value and equal to itself: MIN and MAX return NaN when it is the extreme, `d > 1` is true for NaN | the fixtures contain no NaN (fixturegen builds doubles from integer ratios); `exec.AggregateStateTest.MinMaxOfDoublesIgnoreNaNInEveryBatchSplit` and `engine.SessionTest.MinMaxIgnoreNaNAcrossBatchesAndFiles` pin antb1's MIN and MAX |
| D11 | FLOAT columns | read as DOUBLE (widened exactly): results of FLOAT columns are DOUBLE and print with double precision; `WHERE` compares like DuckDB (see Binding) | keeps FLOAT (`MIN`, `MAX` and projections return FLOAT) | the random generator never references a FLOAT column (`ColumnOf` in `tests/slt/runner/query_gen.cc`, `harness.LoadGenTables.SkipsFloatColumns`); `tests/slt/cases/where/float.slt` selects only other columns, and `engine.SessionTest.FloatColumnsCompareLikeDuckDb` pins that results stay DOUBLE |
| D12 | Long numbers against DOUBLE | a number compared with a DOUBLE column is the correctly rounded nearest double | converts a DECIMAL literal (at most 38 digits) or a HUGEINT literal to DOUBLE in two steps when its digits exceed 2^53, which can be one ulp off (`9007199254740993.5`) | the generator only writes decimals of at most 2^53 in their digits with at most 22 decimals, where both round the same (`ExactDecimalDouble` in `tests/slt/runner/query_gen.cc`) |
| D13 | Decimals with many digits against integer columns | compared exactly | compares in a DECIMAL whose width is capped at 38 digits: when the column type's digits plus the literal's decimals exceed 38, a column value with too many integer digits fails the query with a conversion error (`i16 = 1.0000000000000000000000000000000000001` over the value -32768) | the `.slt` records and the generator keep literals short enough; `plan.Binder/FoldThroughBinderTest.*` covers the exact folding |

## ClickBench status

The target of the first slice was ClickBench Q0, Q1, Q2, Q3 and Q6, run with `--clickbench`; `GROUP BY`, `ORDER BY`
and `OFFSET` added the queries listed below them. Query numbers follow
ClickBench's DuckDB/Parquet query file at ClickBench commit `5a56398c975bfd9f328f544894bcb92533ed134c` (0-based); the
query text itself is never committed. The data test `data.clickbench.status` (`pixi run test-data`, CI job
`clickbench-hits0`) runs every query on antb1 over the first partition of the `hits` dataset, compares the answers
with DuckDB and fails when the passing queries differ from the ratchet `tests/data/clickbench_status.json`. `pass`
below means exactly the ratchet (`pixi run lint` compares them); the PR that changes the pass set updates both
([testing.md](testing.md#the-clickbench-ratchet)). Every other query must fail cleanly (exit code 4, or a parse or
bind error); today all of them answer Unsupported with exit code 4.

| Query | Status | Notes |
| --- | --- | --- |
| Q0 | pass | `COUNT(*)`: answered from the Parquet footer row counts |
| Q1 | pass | `COUNT(*)` under a `WHERE` comparison: the true count of the filter's selection |
| Q2 | pass | `SUM` (HUGEINT), `COUNT(*)` and `AVG` in one scan of the referenced columns |
| Q3 | pass | `AVG` of a BIGINT column: exact 128-bit sum, one division |
| Q4 | pass | `COUNT(DISTINCT)` of a BIGINT column over the whole table |
| Q5 | pass | `COUNT(DISTINCT)` of a VARCHAR column over the whole table |
| Q6 | pass | `MIN` and `MAX` of `EventDate` read as DATE (`--clickbench`) |
| Q7 | pass | `GROUP BY` one column under `WHERE`, ordered by the count descending, no `LIMIT` |
| Q8 | pass | `GROUP BY` one column with `COUNT(DISTINCT)`, ordered by it descending, top-N |
| Q9 | pass | `GROUP BY` one column with `SUM`, `COUNT(*)`, `AVG` and `COUNT(DISTINCT)`, ordered by the count, top-N |
| Q10 | pass | `GROUP BY` one column under `WHERE` with `COUNT(DISTINCT)`, ordered by it descending, top-N |
| Q11 | pass | `GROUP BY` two columns under `WHERE` with `COUNT(DISTINCT)`, ordered by it descending, top-N |
| Q12 | pass | `GROUP BY` one column under `WHERE`, ordered by the count descending, top-N |
| Q13 | pass | `GROUP BY` one column under `WHERE` with `COUNT(DISTINCT)` of another column, ordered by it descending, top-N |
| Q14 | pass | `GROUP BY` two columns under `WHERE`, ordered by the count descending, top-N |
| Q15 | pass | `GROUP BY` one column, ordered by the count descending, top-N |
| Q16 | pass | `GROUP BY` two columns, ordered by the count descending, top-N |
| Q17 | pass | `GROUP BY` two columns with `COUNT(*)` and `LIMIT` without `ORDER BY`: any groups are a right answer, compared as a subset of DuckDB's unlimited answer |
| Q19 | pass | not a target: a projection under a `WHERE` comparison; fits the grammar and passes incidentally |
| Q20 | pass | `COUNT(*)` under `LIKE '%...%'` |
| Q21 | pass | `LIKE '%...%'` and a comparison, `GROUP BY` one column with `MIN` and `COUNT(*)`, ordered by the count, top-N |
| Q22 | pass | `LIKE` and `NOT LIKE` and a comparison, `GROUP BY` one column with `MIN`, `COUNT(*)` and `COUNT(DISTINCT)`, ordered by the count, top-N |
| Q23 | pass | `SELECT *` under `LIKE '%...%'`, ordered by a column, top-N |
| Q24 | pass | a projection under `WHERE`, ordered by a column it does not select, top-N |
| Q25 | pass | a projection under `WHERE`, ordered by the column it selects, top-N |
| Q26 | pass | a projection under `WHERE`, ordered by a column it does not select and then by the one it selects, top-N |
| Q30 | pass | `GROUP BY` two columns under `WHERE` with `COUNT(*)`, `SUM` and `AVG`, ordered by the count, top-N |
| Q31 | pass | `GROUP BY` two columns under `WHERE` with `COUNT(*)`, `SUM` and `AVG`, ordered by the count, top-N |
| Q32 | pass | `GROUP BY` two columns with `COUNT(*)`, `SUM` and `AVG`, no `WHERE`, ordered by the count, top-N: many groups tie at the cut |
| Q33 | pass | `GROUP BY` one column, ordered by the count descending, top-N |
| Q36 | pass | `GROUP BY` one column under a `WHERE` conjunction, ordered by the alias of the count, top-N |
| Q37 | pass | `GROUP BY` one column under a `WHERE` conjunction, ordered by the alias of the count, top-N |
| Q38 | pass | `GROUP BY` one column under a `WHERE` conjunction, ordered by the alias of the count, a window with `OFFSET` |
| Q40 | pass | a `WHERE` conjunction with `IN` over two values, `GROUP BY` two columns, ordered by the count, a window with `OFFSET` |
| Q41 | pass | `GROUP BY` two columns under a `WHERE` conjunction, ordered by the alias of the count, a window with `OFFSET` |
| all others | out of scope | need functions, expressions, HAVING, `GROUP BY` positions or other rejected syntax; they fail cleanly with exit code 4 |
