# 16. Late materialization of top-N columns

Date: 2026-09-30

## Status

Proposed

## Context

- **The gap:** a query that returns every column of a wide table, with a selective filter, then ORDER BY … LIMIT,
  was 8.6× slower than DuckDB on the full ClickBench data (Q23: 3.9 s against 0.46 s).
- **`antb1 explain --analyze`** (ADR 0015) shows why: the part pipeline decodes every column of the table (105) in
  every row group (325), 502 s of CPU summed over the parts. The filter and the top-N merge cost almost nothing next
  to that, and the result is a handful of rows.
- **The columns that matter:** a top-N needs only the columns its filters, computations and keys read. The others are
  needed only for the rows of the result.
- **No row-level reads in Arrow 25:** its Parquet reader cannot read chosen rows of a row group; there is no row-range
  API. A part (row group) can be read for any subset of columns.
- **The policy question:** late materialization is not always a win. When most parts own a row of the result, it
  reads those parts' late columns twice. Who decides?

## Decision

- **Where:** the physical planner, for a Limit over a Sort over a part pipeline made of Filter and Compute nodes over a
  Scan. The logical plan, the optimizer and plain EXPLAIN are unchanged.
- **Which columns are late:** a scan column is late when no Filter predicate, no Compute expression and no sort key
  reads it. Filter and Compute pass the scan's columns through at their positions. So nothing above the scan has to be
  renumbered.
- **The rule, decided by the planner from metadata:** the top-N reads columns late when all of these hold:
  - some column is late;
  - `limit + offset ≤ kept parts / 2`;
  - `limit + offset ≤ 65536`.

  At most `limit + offset` parts own a row of the result, so at least half of the late decoding is saved. The kept parts
  come after statistics pruning.

  A cost-based choice waits for column statistics (sizes, selectivity). This rule needs none and never costs more than
  reading half of the parts' late columns once more.
- **The narrow scan (`LateScan`):**
  - Every part's scan reads only the early columns.
  - Each late column is an `arrow::NullArray` of the batch's length, which has no buffers.
  - The first late column carries instead each row's id, `(part number << 32) | position in the part`.
  - The part top-N buffers keep these narrow rows, with the same sort and tie order as today: input order within a part,
    then part order. So the chosen rows and their order are unchanged.
  - With filter pushdown ([ADR 0020](0020-filter-pushdown.md)), the narrow scan applies the pushed predicates on its
    early columns, and the table reports the positions of the rows that pass, from which it makes their ids.
- **The fetch:** after the merge, the operator reads the late columns for the rows of the output window
  `[offset, offset + limit)`:
  - It groups the rows by part.
  - It runs one task per part and late column on the executor. Each task reads that column of that part and takes the
    rows.
  - It puts every value back in its row's place and gives the row-id column its real values.

  The column-level tasks keep a wide fetch parallel: one row group's 105 columns take about 1.5 s of CPU on one thread.
- **Profile:** `explain --analyze` shows `late=N columns` in the PartTopN detail, and the metrics `late_columns`,
  `late_parts` and `late_fetch`.

## Consequences

- **Wide, selective top-N queries:** they read their early columns for every part, and the other columns only for
  the few parts that own a result row.
- **Results:** they are byte-identical to the plain top-N on any number of threads (tests compare them). Only which
  columns are read, and when, changes.
- **Files:** the late columns are read in a second pass. A file changed between the passes is an IOError: the Parquet
  table checks the footer bytes on every read.
- **Scope:**
  - A Project inside the part pipeline, a serial top-N (not over parts), and ORDER BY without LIMIT keep reading every
    column.
  - Filters over computed columns are unaffected: they are early by construction.
  - Joins (amended 2026-10-02, [ADR 0022](0022-joins-and-query-blocks.md)): late materialization declines over a part
    pipeline that holds a hash-join probe, which then reads every column, until the "late reads for joins" item below
    adds probe-side late columns. Late build-side columns would need row ids for each table.
- **Later:**
  - statistics-driven choices;
  - late materialization after a filter without an ORDER BY (a LIMIT alone already stops early);
  - late reads for joins, probe-side late columns first.

## Alternatives considered

- **Row-level Parquet reads** (page indexes, row ranges): not in Arrow 25's reader API. They would only make the fetch
  cheaper, and the fetch is small already.
- **A narrow scan with renumbered columns:** it would need every Filter, Compute and key renumbered, and back again for
  the output. The NULL placeholders keep every position.
- **A row id beside the batch (a side channel):** every operator would have to carry it through selections and
  materialization.
- **Always late, or a user setting:** always late loses when most parts own a result row. A setting moves the decision
  to the user without information.
