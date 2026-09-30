# 11. Sorting and top-N

Date: 2026-09-27

## Status

Accepted (with the maintainer-approved plan for GROUP BY, ORDER BY and COUNT(DISTINCT))

## Context

- Most ClickBench queries that group rows then order the groups by an aggregate and keep the first `n`
  (`ORDER BY c DESC LIMIT 10`); several projections order by a column they do not select.
- DuckDB, the oracle, orders NULLs last by default for `ASC` and `DESC` alike, orders NaN above every number (and
  equal to NaN), treats `-0.0` as equal to `0.0` and compares VARCHAR by bytes. Arrow's `SortIndices` and `SelectK`
  place NaN and NULL by their own rules (NaN before or after NULL, not per direction as needed here).
- `ORDER BY` often names a select alias (`ORDER BY c` for `COUNT(*) AS c`), and it may use aggregates the select list
  does not return. DuckDB resolves an alias before a table column of the same name.
- Execution is single-threaded (ADR 0003); parallel execution over row groups is the expected next step.

## Decision

- **A logical `Sort` node below the final `Project`.** Its keys are columns of its input: table columns for a
  projection, keys and aggregates of `GroupAggregate` for a grouped query. So it can order by columns the query does
  not return and by hidden aggregates (the binder appends an `ORDER BY` aggregate that the select list lacks to the
  `GroupAggregate`; the `Project` drops it). A global aggregate has one row and gets no `Sort`.
- **`Limit` carries an offset, and a rule moves it below `Project`.** A `Project` keeps every row, so the rewrite is
  always valid; it puts the `Limit` right above the `Sort`, and the physical planner runs `Limit(Sort)` with a limit
  as one top-N operator. `LimitOperator` narrows selections instead of materializing them.
- **Our own row comparator** (`exec::RowComparator`) with DuckDB's order, one comparison function per key type
  chosen once, instead of Arrow's sort kernels.
- **One sort buffer for sort and top-N** (`exec::SortBuffer`). It keeps the input batches and sorts row references
  stably, so tied rows keep their input order and a top-N equals the window of the full sort. With a limit it keeps
  only `limit + offset` rows: once that many are held, a row that does not beat the last kept row is dropped as it
  arrives, and the buffer compacts when the candidates grow past twice that (at least 4096 rows beyond).
- **Mergeable.** `SortBuffer::Merge` appends another buffer's rows as if they came later in the input, so buffers
  filled from consecutive parts of the input (row groups) and merged in input order give the single-buffer result.
- **In memory, no spilling.** A sort without a limit holds its whole input.
- **A top-N over a partitioned GROUP BY** (amended 2026-10-01): for `Limit` over `Sort` right over a
  `GroupAggregate` that runs partitioned (ADR 0013), each of its 64 partitions keeps only its first `limit + offset`
  rows in the top-N's order (a `SortBuffer` with that keep), in parallel, as it builds them; the top-N reads at
  most 64 × `limit + offset` rows. A row in the top of all groups is in the top of its own partition, and the
  partitions come in order with their kept rows stably sorted, so the result is the same rows in the same order,
  ties included. On the full ClickBench data Q32 (100 M groups) went from 2.43 s to 1.60 s: the serial top-N over
  every group and the building of rows no one reads are gone.

## Consequences

- Follow-up: a top-N over table parts reads the columns only the result needs for the result's rows alone
  ([ADR 0016](0016-late-materialization.md)).
- `ORDER BY` on columns, aliases and aggregates, `ASC`/`DESC`, `NULLS FIRST`/`LAST`, `LIMIT` and `OFFSET` answer like
  DuckDB up to the order of tied rows.
- The test harness accepts any order of tied rows: it runs an augmented query on DuckDB (the `ORDER BY` keys
  appended to the select list, no `LIMIT`) and checks that antb1's row `i` is one of DuckDB's rows with the keys of
  rank `offset + i` (`tests/slt/runner/ordered_compare.h`).
- `ORDER BY` positions (`ORDER BY 2`) and expressions stay outside the subset.
