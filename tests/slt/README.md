# SQL logic tests (`antb1-slt`)

`antb1-slt` runs sqllogictest files against antb1 and checks the same expectations against DuckDB, the
test oracle. DuckDB writes the expectations (`pixi run slt-complete`); humans review the diff.

## Layout

- `cases/<area>/<file>.slt`: the test cases. Each file becomes the ctest tests `slt.<area>.<file>`
  (antb1, label `slt`) and `oracle.<area>.<file>` (DuckDB, label `oracle`).
- `selftest/`: harness self-tests. `mutate.slt` and `redact_canary.slt` must pass as they are, and must
  fail under `--mutate` (`harness.slt.mutate.<kind>`, `harness.slt.redact`). `lockdown.slt` checks the
  DuckDB lockdown. The pending records of `pending.slt` stay Unsupported, and the pending check must fail when
  `--mutate` answers them (`harness.slt.pending.mutate.<kind>`).
- `canary/`: redaction canaries that fail on purpose (`harness.slt.redact_sentinels`, `harness.diff.redact`), and
  `tables_redact.txt` and `redact_table.slt` for the self-tests of the `redact` option (`harness.*.redact_table`).
- `tables.txt`: the tables every file can query, registered identically on both engines, and their foreign keys.
- `supported_features.h`: the SQL features antb1 answers today (see "Random differential test").
- `runner/`: the runner (`antb1_slt_lib` and `antb1-slt`); `tests/`: its unit tests (label `harness`).

The tables are views over the Parquet fixtures of `tools/fixturegen` (ctest `fixtures.generate`, written to
`build/<preset>/fixtures`). Write our own queries over our own tables only: never ClickBench data or
ClickBench query text.

A table's `ref=<column>[+<column>...]:<table>.<column>[+<column>...]` options (repeatable) declare its foreign keys:
which of its columns reference, pairwise, which columns of a table of the same file, itself included. The star
schema of the join tests (`tools/fixturegen/star.h`) declares 13. A ref is a hint for the generated joins of roadmap
PR T1, not a constraint: NULL and dangling keys are intended. The loader checks the syntax and finds the table
ASCII case-insensitively (`runner/tables.h`); `metamorphic.TablesTxt.RefsJoinColumnsOfOneKind` checks that every
column exists and that each pair joins one kind of key (integers, DECIMALs, DATE or VARCHAR, never DOUBLE). The
engines and the query generator ignore refs until T1.

## Commands

```bash
pixi run test -L slt        # antb1 vs the expectations
pixi run test -L oracle     # DuckDB vs the same expectations
pixi run test -L harness    # runner unit tests, fixture checks, mutation and redaction self-tests, pending check
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
- `query <types> [nosort|rowsort|valuesort] [label]`: one letter per column, `I` integer, `R` real, `D` decimal (a
  `DECIMAL(p,s)` of any scale), `T` text (dates too). The engine's column classes must match. A record does not check
  type names: a DECIMAL's text shows its scale, but `DECIMAL(15,2)` and `DECIMAL(18,2)` print alike (the diff,
  query-file and ClickBench runs compare type names). `valuesort` refuses a record with both `R` and `D` columns: it
  loses the columns, so every value would compare within the R tolerance. Queries with the same label must return the
  same result.
- `skipif <engine>` / `onlyif <engine>` (engine `antb1` or `duckdb`) guard the next record.
- `pending <roadmap id>` (`pending J2b`) guards the next statement or query until that roadmap PR answers it: DuckDB
  runs it, antb1 only in the pending check (below). It takes no `skipif` or `onlyif`.
- `halt` stops the file (for one engine when guarded); `hash-threshold <n>` hashes results with more than
  `n` values (0: never), except results with an R column.
- `${FIXTURES}` in SQL is the fixtures directory, e.g. `FROM '${FIXTURES}/edge.parquet'`.

## Current support is the contract

An antb1 `Unsupported` answer is always a failure, also for `statement error`. Internal errors never satisfy
`statement error`. A record for SQL that antb1 does not answer yet carries a guard, which the PR that adds the
feature removes:

- `pending <roadmap id>` when a PR of the roadmap answers it. DuckDB runs the record as usual (`oracle.*` and
  `slt-complete`), and antb1's `slt.*` and `parallel.*` tests skip it. The pending check `harness.slt.pending`
  (`antb1-slt pending`) runs the pending records of every registered file on antb1, where each must still get
  Unsupported: once antb1 answers one, with rows or with an error of another kind, the check fails with
  `remove the guard (<id>)`, so the PR that answers it removes its guard and the `slt.*` tests compare its answer
  from then on. A record whose outcome another PR changes first waits on that PR: the syntax errors of
  `cases/joins/alias_words.slt`, which antb1 rejects as Unsupported until roadmap PR S3, are `pending S3`.
- `onlyif duckdb` otherwise. A section of such records is headed `# ---- DuckDB only until <deferred item> gets a
  PR ----` when an ADR defers the SQL until a query needs it, and `# ---- DuckDB only, for good: ... ----` for
  DuckDB's own error texts.

`cases/joins/` and `cases/subqueries/` hold the pending corpus of the join and subquery PRs of ADRs 0022 and 0023,
over the star schema. Its pending records stay inside what those PRs answer: every query joins all its tables through
equalities whose common type is not DOUBLE (a disconnected join graph exits 4), and none has a `NULL` literal,
`IS NULL`, `SELECT DISTINCT` or `COALESCE`, which exit 4 as well (the NULLs come from the data, and `COUNT(col)`
counts them). Results of several rows use `rowsort`, and no string `MIN` or `MAX` runs under `GROUP BY`, where
DuckDB 1.5.5 leaks. The DuckDB-only ASOF records join columns without NULLs on a build key without repeats, because
DuckDB 1.5.5 and 1.5.6 count a NULL inequality value as an ASOF match. Each file starts with unguarded records over
one table that pin its inputs, so its `slt.*` and `parallel.*` tests run before any guard goes.

## Canonical values

Both engines produce the same canonical text (`runner/canonical.h`): integers exactly (SUM is a 128-bit HUGEINT),
doubles as the shortest round-trip form, decimals as DuckDB prints them (exactly s digits after the point, and a leading
`0` only when p > s, [ADR 0021](../../docs/adr/0021-decimal-semantics.md) rule 15: `DECIMAL(15,2)` prints `17.00` and
`-0.25`, `DECIMAL(3,3)` prints `.500`), dates as `YYYY-MM-DD`, strings as bytes. Cells are escaped: `NULL`, `(empty)`
for the empty string, `\t` `\n` `\r` `\\`, and `\xHH` for control characters, invalid UTF-8 bytes and leading or
trailing spaces. `I`, `D` and `T` compare exactly; `R` compares with relative tolerance 1e-9 plus absolute 1e-12
(`# tol` overrides the relative part).

## The DuckDB oracle

DuckDB runs in memory through its C API with `threads=1`, no extension autoinstall or autoload, file
access limited to the fixtures directory, temp files under `build/`, its `late_materialization` optimizer
off, and a locked configuration. With the optimizer on, DuckDB 1.5.5 and 1.5.6 fail some filtered `LIMIT ... OFFSET`
queries with an internal error (`where/pushdown.slt`). With it off, DuckDB computes the select list of a
small `ORDER BY ... LIMIT` for every row the `WHERE` keeps, as antb1 does, so an overflow there fails in both
engines; only the order of tied rows at the edge of a `LIMIT` can differ, and every comparison accepts that.
A record runs one statement, and only `SELECT`, `EXPLAIN`, `SET` or `LOAD`: the oracle refuses anything else
(`COPY`, `ATTACH`, `EXPORT`, DDL, DML) with a `Permission Error` before it runs, so no record writes next
to the shared fixtures or changes state for later records (`selftest/lockdown.slt` checks it). It never
loads an extension either: DuckDB refuses `LOAD`, by name or by path, once external access is off, so the oracle
cannot run TPC's dbgen, the `tpch` extension of `tests/tpch`. Each
table is `CREATE VIEW <name> AS SELECT * FROM read_parquet([...], binary_as_string=true)`; tables with
the `clickbench` option replace `EventDate` with `make_date(EventDate)`.

`engine::Session` only has session-wide column overrides, so every table with an `EventDate` column must
agree on `clickbench`, and `FROM '<path>'` also reads `EventDate` as DATE on antb1 (but not on DuckDB).

## Random differential test

`antb1-slt diff` generates queries over the tables of `tables.txt`, runs each on antb1 and on DuckDB and
compares the results with the same comparator (rowsort for projections and `GROUP BY`). With a `LIMIT` or
`OFFSET` and no `ORDER BY`, any rows of the unlimited answer are right: antb1 must return as many rows as DuckDB,
each one a distinct row of DuckDB's answer to the same query without the `LIMIT` (`CompareSubset`). With `ORDER BY`,
rows with equal keys may come in any order: DuckDB runs the query with its `ORDER BY` keys appended to the select
list and without `LIMIT`/`OFFSET` (its limit grows until the run of ties at the window's end is complete), and
antb1's row `i` must be a distinct row of the run of equal keys at rank `offset + i` (`CompareOrdered`,
`runner/ordered_compare.h`). At most 2^20 rows are fetched in order: when the run at the window's end goes on
beyond them (millions of groups tied at a count, say), one more query fetches only the rows of that run whose
cells equal antb1's rows there, written as SQL literals (`D` and `T` cells compare with the column's text as DuckDB
prints it, so a DECIMAL literal is never rounded or respelled). The query files of the data tests and the ClickBench
runner check the same way (`CompareQueryAnswers`). The column types must match exactly (`BIGINT`, `HUGEINT`,
`DECIMAL(15,2)`, ...), not only their `I`/`R`/`D`/`T` class. Query `i` of seed `s` depends only on `s`, `i`, the tables
and the supported features, so one case reproduces alone.

- 75% of the queries use only the features in `supported_features.h` (`kSupportedFeatures`); the rest
  sample the full target grammar of `docs/sql-subset.md` (`--target-percent`).
- An antb1 Unsupported answer is a failure for a query that uses only supported features. Otherwise it is
  counted per missing feature and reported, not a failure; another query error there is counted as
  `rejected`.
- DuckDB runs every query: the generator writes only SQL DuckDB accepts with the semantics antb1 targets
  (typed literals, doubles that both engines parse alike, no FLOAT column because its results are DOUBLE on antb1
  only (divergence D11 in `docs/sql-subset.md`), no clickbench-typed column with `FROM '<path>'`, a DECIMAL compared
  only where DuckDB's common type keeps 38 digits (divergence D13), a DECIMAL `CASE` value only where its cast to the
  `CASE` type cannot fail), so a DuckDB error is a generator bug and fails.
- A failure prints the seed, the case index, the features, the SQL, at most 5 differing rows and the repro.

```bash
pixi run diff-random                                    # 2000 queries, random seed (printed first)
ANTB1_DIFF_SEED=7 ANTB1_DIFF_COUNT=20000 pixi run diff-random
ANTB1_DIFF_SEED=7 ANTB1_DIFF_ONLY=1234 pixi run diff-random  # one case
pixi run diff-random --list --target-percent 100        # print generated queries, run nothing
```

ctest runs `diff.random` (label `diff`) with a fixed seed and 300 queries over the tables it names, and `diff.decimal`
(over `decimals`) and `diff.star` (over the star schema of the join tests) with seeds of their own; `parallel.diff.star`
runs the queries of `diff.star` on 4 threads in 97-row batches. A slice PR that implements a
feature adds it to `kSupportedFeatures` (and new grammar to `runner/query_gen.cc`; the unit test
`harness.QueryGenerator.TargetSamplesCoverTheWholeGrammar` fails until every feature is generated). Grammar that
the parser accepts before the generator writes it waits in `kGeneratorPending` (today the FROM lists, joins, table
aliases and qualified names, which ADR 0022's T1 and T2 teach the generator); such a feature cannot be declared
supported until it leaves that set.

## Data tests: `queries` and `clickbench`

The ClickBench data tests (`tests/data`, label `data`) use two more subcommands:

- `antb1-slt queries FILE` runs our own queries (`tests/data/hits0_slice.sql`, format in
  `runner/query_file.h`) on antb1 and DuckDB. Each query declares its features; a query with only supported
  features must equal DuckDB's answer, the others must still get Unsupported.
- `antb1-slt clickbench --status FILE QUERIES` runs ClickBench's queries on antb1, DuckDB only where antb1
  answers, and compares the passing queries with the ratchet `tests/data/clickbench_status.json`
  (`runner/clickbench.h`).

`canary/canary_queries.sql` and `canary/status_pass_*.json` drive their self-tests (`harness.queries.*`,
`harness.clickbench.*`).

## The ratchet of the queries derived from TPC-H: `tpch`

`antb1-slt tpch --queries DIR --status FILE [--memory-limit SIZE]` is the same ratchet (`runner/ratchet.h`, shared
with `clickbench`) for the numbered queries of a directory (`q01.sql`, ... as Q1, ...; the loader of `answers`)
against `tests/data/tpch_status.json` (`{"pass": [...]}`). Two differences: only Unsupported is a clean failure (a
parse or bind error fails, since the queries are valid SQL), and each query is logged as `Q<n>: running` before it
runs, flushed, so the log of a run that times out names the query. `--memory-limit` takes an absolute size (`2GiB`,
`512MiB`, bytes) for the antb1 sessions. `ANTB1_TPCH_TIMES=1` adds each query's seconds and the geometric mean of
the passing queries. Its self-tests `harness.tpch.status.*` write our own numbered queries over the canary table
(marked `redact`) into the build tree.

## Stored answers: `answers`

`antb1-slt answers --queries DIR --answers DIR` checks answers that DuckDB stored next to their queries, for data,
queries and answers generated together at test time (the data derived from TPC-H). It runs the numbered queries
of one directory (`q01.sql`, `q02.sql`, ...: one statement each in the format of `runner/query_file.h`, numbered from
1 without gaps) on the DuckDB oracle and compares each result with the answer of the same number in the other
directory (`q01.csv`, ...; `runner/answers.h`). An answer is a header line and one line per row, with `|` between
fields, an empty field for NULL and nothing trimmed.

- Only the number of columns is compared, never the header's names.
- Cells compare by the oracle's column class: `I` and `T` exactly, `R` within the tolerance, `D` by value.
- Rows compare in order. Rows that are equal only in another order pass, reported as `pass (rows in another order)`.

`D` compares by value here, although `.slt` records compare DECIMAL text exactly. An `.slt` expectation is the text
of DuckDB's result, so its exact text also pins the scale (`DECIMAL(15,2)` prints `17.00`). A stored answer comes
from another writer, which may drop the trailing zeros (`17`), and the column types are not part of its text.

The output is always redacted: per query `Q<n>: pass`, or a failure with the error kind, the row counts, the first
differing row and the sha256 of both blocks (`answer` and `DuckDB`). `--mutate` corrupts DuckDB's results. The
self-tests `harness.answers.*` write our own queries over the canary table and their answers into the build tree,
because lint R009 keeps files named like `q01.sql` out of the repository.

## Redaction and other options

- `--redact` never prints values or SQL: only the record id, column types, row counts, the first
  differing row and the sha256 of each side's block (`harness.slt.redact` guards it). `diff --list`, which prints
  the generated SQL, is refused with it.
- The `redact` option of a table in the tables file (`runner/tables.h`), for data that must never be printed,
  makes every run redacted without `--redact`. `diff --list` and `complete`, which print or write values, are
  refused (`harness.*.redact_table`).
- `--show-values` lifts the redaction of `redact` tables and of `answers` for a local repro
  (`harness.answers.show_values`): the repro command of a redacted failure carries it. It is refused when
  `GITHUB_ACTIONS=true` (`harness.answers.show_values_on_ci`), because CI logs are public. An explicit `--redact`
  always wins (`harness.queries.redact_wins`).
- `--mutate <kind>` corrupts antb1 results (`value`, `null`, `drop-row`, `extra-row`, `extra-column`,
  `error`, `unsupported`, `succeed`, `canary`); the self-tests prove that each one is caught.

`--redact` and `--mutate` work for `run`, `diff`, `queries` and `clickbench`; `answers` and `tpch` take `--mutate`
(`tpch` is always redacted, whatever its tables file says), and `--show-values` works for all six. The `pixi run diff-random`
repro of a `diff` failure is printed only for the tables of that task (`tables.txt`); every failure also prints its
exact command line with `--only`.
