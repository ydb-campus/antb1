# SQL logic tests (`antb1-slt`)

`antb1-slt` runs sqllogictest files against antb1 and checks the same expectations against DuckDB, the
test oracle. DuckDB writes the expectations (`pixi run slt-complete`); humans review the diff.

## Layout

- `cases/<area>/<file>.slt`: the test cases. Each file becomes the ctest tests `slt.<area>.<file>`
  (antb1, label `slt`) and `oracle.<area>.<file>` (DuckDB, label `oracle`).
- `selftest/`: harness self-tests. `mutate.slt` and `redact_canary.slt` must pass as they are, and must
  fail under `--mutate` (`harness.slt.mutate.<kind>`, `harness.slt.redact`). `lockdown.slt` checks the
  DuckDB lockdown.
- `canary/`: redaction canaries that fail on purpose (`harness.slt.redact_sentinels`, `harness.diff.redact`).
- `tables.txt`: the tables every file can query, registered identically on both engines.
- `supported_features.h`: the SQL features antb1 answers today (see "Random differential test").
- `runner/`: the runner (`antb1_slt_lib` and `antb1-slt`); `tests/`: its unit tests (label `harness`).

The tables are views over the Parquet fixtures of `tools/fixturegen` (ctest `fixtures.generate`, written to
`build/<preset>/fixtures`). Write our own queries over our own tables only: never ClickBench data or
ClickBench query text.

## Commands

```bash
pixi run test -L slt        # antb1 vs the expectations
pixi run test -L oracle     # DuckDB vs the same expectations
pixi run test -L harness    # runner unit tests, fixture checks, mutation and redaction self-tests
pixi run slt-complete       # rewrite every expected block from DuckDB, then review `git diff`
```

A failure prints the file and line, the SQL, the expected and actual blocks, and a repro command.

## File format

Records are separated by blank lines.

```text
# A comment. "# tol 1e-6" sets the relative tolerance of R columns in the next query.

statement ok
SELECT COUNT(*) FROM hits_like

statement error ^bind:
SELECT COUNT(*) FROM no_such_table

onlyif duckdb
query T valuesort
SELECT s FROM edge
----
(empty)
NULL
a
```

In an expected block each row is one line; the cells of a row are separated by a tab.

- `statement ok` / `statement error [regex]`: the statement must succeed / fail. The regex (ECMAScript) is
  searched in the error text: antb1 reports `<kind>: <message>` with kind `parse`, `bind`, `io` or
  `execution`; DuckDB reports its own text (`Catalog Error: ...`). A regex usually needs `onlyif`.
- `query <types> [nosort|rowsort|valuesort] [label]`: one letter per column, `I` integer, `R` real, `T` text
  (dates too). The engine's column types must match. Queries with the same label must return the same result.
- `skipif <engine>` / `onlyif <engine>` (engine `antb1` or `duckdb`) guard the next record.
- `halt` stops the file (for one engine when guarded); `hash-threshold <n>` hashes results with more than
  `n` values (0: never), except results with an R column.
- `${FIXTURES}` in SQL is the fixtures directory, e.g. `FROM '${FIXTURES}/edge.parquet'`.

## Current support is the contract

An antb1 `Unsupported` answer is always a failure, also for `statement error`. A record for SQL that antb1
does not support yet carries `onlyif duckdb`; the PR that adds the feature removes the guard. Internal
errors never satisfy `statement error`.

## Canonical values

Both engines produce the same canonical text (`runner/canonical.h`): integers exactly (SUM is a 128-bit
HUGEINT), doubles as the shortest round-trip form, dates as `YYYY-MM-DD`, strings as bytes. Cells are
escaped: `NULL`, `(empty)` for the empty string, `\t` `\n` `\r` `\\`, and `\xHH` for control characters,
invalid UTF-8 bytes and leading or trailing spaces. `I` and `T` compare exactly; `R` compares with
relative tolerance 1e-9 plus absolute 1e-12 (`# tol` overrides the relative part).

## The DuckDB oracle

DuckDB runs in memory through its C API with `threads=1`, no extension autoinstall or autoload, file
access limited to the fixtures directory, temp files under `build/`, and a locked configuration. A record
runs one statement, and only `SELECT`, `EXPLAIN`, `SET` or `LOAD`: the oracle refuses anything else
(`COPY`, `ATTACH`, `EXPORT`, DDL, DML) with a `Permission Error` before it runs, so no record writes next
to the shared fixtures or changes state for later records (`selftest/lockdown.slt` checks it). Each
table is `CREATE VIEW <name> AS SELECT * FROM read_parquet([...], binary_as_string=true)`; tables with
the `clickbench` option replace `EventDate` with `make_date(EventDate)`.

`engine::Session` only has session-wide column overrides, so every table with an `EventDate` column must
agree on `clickbench`, and `FROM '<path>'` also reads `EventDate` as DATE on antb1 (but not on DuckDB).

## Random differential test

`antb1-slt diff` generates queries over the tables of `tables.txt`, runs each on antb1 and on DuckDB and
compares the results with the same comparator (rowsort for projections; for a projection with LIMIT only the
column types and the row count). The column types must match exactly (`BIGINT`, `HUGEINT`, ...), not only
their `I`/`R`/`T` class. Query `i` of seed `s` depends only on `s`, `i`, the tables and the supported
features, so one case reproduces alone.

- 75% of the queries use only the features in `supported_features.h` (`kSupportedFeatures`); the rest
  sample the full target grammar of `docs/sql-subset.md` (`--target-percent`).
- An antb1 Unsupported answer is a failure for a query that uses only supported features. Otherwise it is
  counted per missing feature and reported, not a failure; another query error there is counted as
  `rejected`.
- DuckDB runs every query: the generator writes only SQL DuckDB accepts with the semantics antb1 targets
  (typed literals, doubles that both engines parse alike, no clickbench-typed column with `FROM '<path>'`),
  so a DuckDB error is a generator bug and fails.
- A failure prints the seed, the case index, the features, the SQL, at most 5 differing rows and the repro.

```bash
pixi run diff-random                                    # 2000 queries, random seed (printed first)
ANTB1_DIFF_SEED=7 ANTB1_DIFF_COUNT=20000 pixi run diff-random
ANTB1_DIFF_SEED=7 ANTB1_DIFF_ONLY=1234 pixi run diff-random  # one case
pixi run diff-random --list --target-percent 100        # print generated queries, run nothing
```

ctest runs `diff.random` (label `diff`) with a fixed seed and 300 queries. A slice PR that implements a
feature adds it to `kSupportedFeatures` (and new grammar to `runner/query_gen.cc`; the unit test
`harness.QueryGenerator.TargetSamplesCoverTheWholeGrammar` fails until every feature is generated).

## Options for data tests

- `--redact` never prints values or SQL: only the record id, column types, row counts, the first
  differing row and the sha256 of each side's block (`harness.slt.redact` guards it).
- `--mutate <kind>` corrupts antb1 results (`value`, `null`, `drop-row`, `extra-row`, `extra-column`,
  `error`, `unsupported`, `succeed`, `canary`); the self-tests prove that each one is caught.

Both options work for `run` and `diff`.
