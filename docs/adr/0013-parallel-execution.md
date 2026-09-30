# 13. Parallel execution over row groups

Date: 2026-09-28

## Status

Proposed

## Context

- **Execution is single-threaded** (ADR 0003): files and row groups are read in order, in 64Ki-row batches. ADR 0003
  leaves intra-query parallelism open as a question, "for example morsel-driven scheduling".
- **Cost on the full ClickBench data (100 files) is on one core.** Q28 (a regex per row) takes about 326 s. Q18, Q27 and
  Q39 take 25-28 s. Scan-heavy queries scale with the data, not with the machine.
- **Granularity:** the full ClickBench data has 325 row groups in 100 files (from its Parquet metadata), from about 1
  thousand to 800 thousand rows each. That is enough parts for tens of threads, but they are uneven.
- **The states were designed for this.**
  - `AggregateState::Merge`, `GroupedAggregateState::Merge(other, group_map)` and `SortBuffer::Merge` exist and are
    unit-tested. No operator calls them yet.
  - ADR 0010: partial states over parts of the input, merged in input order, give the single-pass result. The one
    exception is DOUBLE sums, whose rounding follows the part boundaries.
  - ADR 0011 says the same for sort buffers.
- **Nothing blocks sharing the process across threads.**
  - There are no static caches or `thread_local` state in `src/`.
  - Arrow's function registry and default memory pool are thread-safe.
  - `LikePattern` is immutable.
  - Operator instances hold run state, so they are single-thread objects.
- **Arrow 25 already provides the scheduler.** `arrow::internal::ThreadPool`/`Executor` (`arrow/util/thread_pool.h`) are
  part of `libarrow`, which `exec` and `engine` already link, so no new dependency is needed.
- **Tests can already accept any result order.** They compare grouped results, tied rows and LIMIT-without-ORDER-BY
  results without regard to order.
  - The docs still promise "results are deterministic".
  - The hermetic suites pin one engine thread.
  - AGENTS.md asks for a maintainer's approval before tests get threads.

## Decision

- **The unit of work is a part, one row group of one file.**
  - `plan::Table` gains:
    - `num_parts()` and `part_rows(part)`: the parts in file and row-group order, with row counts;
    - `ScanPart(part, fields, batch_size)`: a reader of that row group's fields.
  - `ParquetTable` keeps each file's `FileMetaData` from `Open`, so opening a part does not re-read the footer. It opens
    a `FileReader` per part, which outlives its reader as today.
  - `MemoryTable` and the test tables are one part.
- **The plan splits at its first blocking operator.**
  - The pipeline below it is `Scan <- [Filter] <- [Compute] <- [Project]`. It is built once per part by the physical
    planner over `ScanPart`, and runs on the pool.
  - The sink is the blocking operator:
    - `Aggregate`: `AggregateState`s;
    - `GroupAggregate`: a grouper and `GroupedAggregateState`s;
    - `Sort`: a `SortBuffer`, with top-N `keep`;
    - for a plan without one, an ordered collector with the `Limit`.
  - The sink takes a partial result per part. Everything above the sink stays serial, over its small input, except
    a `Compute` (`ParallelComputeOperator`, amended 2026-09-30): over a large aggregation (millions of groups, as
    when GROUP BY keys computed from other keys move above it, ADR 0018) it computes its batches on the pool, in
    order, so its output and its first error are those of the serial one. It follows the part scheduler's memory
    rules: a window of batches that halves under pressure and an out-of-memory retry alone.
  - **Every scan runs through parts, also on one thread:** with one thread, each part runs on the calling thread when
    the merge reaches it. That is the same code and the same partial results, so answers are byte-identical for any
    thread count, DOUBLE sums included.
  - Until a blocking operator has its own sink, it reads the pipeline's batches in part order from a part union: the
    pipeline below it already runs in parallel (for example the expressions below a GROUP BY), and its own work stays
    serial.
- **Partial results are merged in part order, whatever the thread count and schedule.**
  - A part's partial result is merged into the global one once every earlier part's result is merged.
  - Results that finish early wait in a window, bounded at twice the thread count. The scheduler does not start a part
    beyond the window.
  - So the answer is a function of the parts only, never of timing: the same on 1 thread and on 64.
  - GROUP BY merges map each part's groups through the global grouper (its unique keys, normalized as today). The
    first-seen key of a group stays the one from the earliest part.
  - An ordered projection emits each part's batches in part order.
  - `Limit` above a projection, or top-N `keep`, stops scheduling new parts once the window is decided, as the serial
    scan stops today.
- **Threads:**
  - `SessionOptions::threads` sets the size of a `ThreadPool` the `Session` owns (none for 1 thread).
  - The CLI's `--threads` defaults to the machine's hardware threads (`std::thread::hardware_concurrency()`), as in
    DuckDB, whose `threads` setting defaults to all hardware threads. The maintainer chose this on 2026-09-29. The
    library default stays 1, so embedders and the test harnesses are single-threaded unless they ask; the `parallel`
    label uses 4.
  - The CLI gets `--threads` for `query` and `bench`, and `antb1 bench` records the count in its JSON. `explain`
    prints the logical plan and executes nothing, so it has no `--threads`.
  - Arrow compute kernels keep running inline (no nested parallelism), each worker with its own `compute::ExecContext`.
  - I/O stays synchronous inside a part (no pre-buffering), so memory is about one row group per busy thread plus the
    window.
- **Errors:**
  - The first failing part in part order decides the query's error, as the serial scan would have met it first.
  - Parts after a failure are not started; parts already running finish or stop at their next batch.
  - Errors from a later part than one that already succeeded are dropped. No error is invented or lost relative to the
    serial order.
- **Semantics that change, documented in docs/sql-subset.md:**
  - DOUBLE `SUM` and `AVG` add per row group, then the row-group sums in order. The value can differ from today's one
    running sum by rounding, still deterministically. The oracle tests compare DOUBLE with a tolerance. The
    maintainer accepted this on 2026-09-29.
  - Group order and tie order stay unspecified. They are deterministic for a given input, now also across thread counts.

## Towards joins (amendment, 2026-09-29)

The maintainer asked for this design to be ready for TPC-H: hash joins, and correlated subqueries.

- **A physical plan becomes a DAG of pipelines.** A pipeline has a source (a table's parts, or a finished sink's
  output), streaming operators (`Filter`, `Compute`, `Project`, later a hash-join probe) and a sink (aggregate,
  grouped aggregate, sort/top-N, the ordered collector, later a hash-join build). A pipeline runs once the sinks it
  reads are finished: a probe pipeline after its builds.
- **A table read by two pipelines is scanned twice, never buffered** (a self-join, a CTE used twice, a decorrelated
  subquery over the outer table). Only the sinks hold data.
- **Order stays deterministic:** a build holds its rows in part order and a probe keeps the probe side's part order,
  so a join's output does not depend on the thread count either.
- **Correlated subqueries** are decorrelated by the binder into semi, anti and aggregate joins; the executor never
  runs a subquery per row.
- **Memory:** the window bounds what is in flight: up to 2 × threads parts' partial states or, under a part union
  (a projection, or a blocking operator without its own sink yet), their whole output, because a part hands on its
  batches only when it is finished. A hash-join build is one table shared read-only by the probe threads, built on
  the smaller side. The session memory limit (`exec::MemoryBudget`: a counting memory pool plus reservations for the
  operators' own containers; the window halves under pressure and widens again without, and a part that runs out
  of memory next to others runs again alone; a `memory` error, exit code 1) came
  right after the first parallel PR. Spilling (grace hash join, partitioned GROUP BY, external sort) comes after
  joins, in its own ADR; it never changes a value, only the row order where SQL leaves it open.

## Consequences

- **Speed:** scan, filter, expression and aggregation work scales with cores. The merges are serial.
  - Merge cost is small for global aggregates and top-N.
  - It grows with the number of groups times parts for high-cardinality GROUP BY (Q32, Q33). A partitioned (radix) merge
    is the follow-up if profiling shows it matters.
- **Cost of the ordered merge:** merging costs the same work in any order. For global aggregates it is one addition
  per aggregate and part; for GROUP BY it is one insertion per partial group. Keeping the order adds only waiting: a
  part that finished early waits for the earlier ones. Other threads keep working on later parts until the window is
  full, so time is lost only when an early part is much slower than those after it. The full data's row groups range
  from about 1 to 800 thousand rows.
- **Load balance, in this order** (the maintainer's choice, 2026-09-29):
  1. Ship the ordered merge with a window of 2× the threads, and measure idle time and query times with
     `antb1 bench` on the full data at several thread counts.
  2. If threads idle, split large row groups into batch ranges (a part is then a row group and a range of its
     batches), so that parts are even. The ordered merge does not change.
  3. Only if that is not enough, merge out of order for queries whose results do not depend on order: integer sums,
     counts, MIN and MAX. The order-dependent results keep the ordered merge: DOUBLE sums, the first-seen spelling of
     a DOUBLE key, and the output order of groups, ties and LIMIT without ORDER BY. Output order would then come from
     sorting the groups at the end, so answers stay the same for any thread count.
- **Memory:** it grows with the window and the per-part partial states. High-cardinality grouping holds partial group
  tables for at most the window's parts.
- **Tests:**
  - A new `parallel` label runs the `.slt` and differential suites with 4 threads. It checks their answers against
    DuckDB, and checks that the output is byte-for-byte equal to 1 thread over parts of the fixtures.
  - The fixtures gain multi-row-group files so that parts exist.
  - ThreadSanitizer (`pixi run tsan`) covers these tests.
  - The label is part of the regular `pixi run test` and `pixi run check`, with a fixed count of 4 threads, fixed
    seeds and no timing assertions. `pixi run tsan` (nightly, advisory) covers it as well. The maintainer approved
    these threads in tests (AGENTS.md asks before adding them) on 2026-09-29. The preset and task changes this needs
    (`CMakePresets.json`, `pixi.toml`) are protected paths and come for approval with the PR that adds the thread
    pool.
- **ADR 0003** moves to Accepted for parallelism once this ADR is accepted. ADRs 0006 and 0010, and architecture.md's
  "one thread" statements, are updated in the PRs that change them.

## Plan (one PR each, measured with `antb1 bench` on the full data)

1. **io** (#42): `Table::num_parts`/`part_rows`/`ScanPart`, `FileMetaData` reuse. No behavior change. (The fixtures
   already had multi-row-group files.)
2. **engine and exec:** the pool, `--threads`, the part pipelines with the global-aggregate sink, the ordered part
   union with LIMIT early stop, the ordered merge window and error order. Tests with the `parallel` label.
3. **engine and exec:** the session memory limit (`--memory-limit`, default 80% of physical memory).
4. **exec:** the GROUP BY sink (grouper merge through group maps, first-seen keys by part), then its
   radix-partitioned merge: 64 partitions by key hash (a constant, not the thread count), each merging the parts in
   part order, the partitions in parallel. Amended 2026-09-30: each partition merges in its own lane
   (`exec::PartitionLanes`) instead of all partitions of a part finishing before the next part starts. A slow
   partition then holds back no other: on Q32 the per-part waits cost 3.6× the mean partition time. Then the
   partitions' rows are built in parallel, as many at a time as threads. Amended again 2026-09-30: a part whose
   first 4096 rows hardly reduce (more groups than 3/4 of them) sends its other rows straight to the partitions,
   which aggregate them after the part's own groups. Part order and the results stay as before; plans with a
   DOUBLE SUM or AVG (their rounding follows the parts) or a HUGEINT one (its overflow check follows the order of
   the additions) keep the parts' own tables.
5. **exec:** the top-N sink (every part keeps its first rows, merged in part order). A full `ORDER BY` without
   `LIMIT` still sorts the part union on one thread; a k-way merge of sorted parts is its follow-up.
6. **io and exec** (follow-up): skipping parts by their footer statistics (integer-valued columns, predicates
   directly on the scan), before the parts are scheduled.
7. **exec** (follow-up, [ADR 0014](0014-two-level-aggregation.md)): `COUNT(DISTINCT)` in two levels, with the
   heavy keys' groups spread over the partitions.
8. **Docs:** ADRs 0003, 0006 and 0010 and the status of this ADR (semantics, architecture.md and benchmarks change
   with the PRs above).

## Alternatives considered

- **Per-thread partial states merged as threads finish:** less merging, but results depend on the schedule (DOUBLE sums,
  first-seen DOUBLE keys, group order). Rejected: the oracle and bug reports need answers that don't depend on thread
  timing.
- **Static contiguous ranges of parts per thread:** deterministic for a thread count but not across counts, and it
  load-balances poorly across uneven row groups.
- **Exchange operators and push-based pipelines (Volcano exchange, DuckDB-style push):** more general, for joins and
  deeper plans, but antb1's plans have one scan and at most one blocking operator below small serial tops. Revisit with
  joins (ADR 0003's pull-versus-push question).
