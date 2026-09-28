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
    - `Parts()`: the parts in file and row-group order, with row counts;
    - `ScanPart(part, fields, batch_size)`: a reader of that row group's fields.
  - `ParquetTable` keeps each file's `FileMetaData` from `Open`, so opening a part does not re-read the footer. It opens
    a `FileReader` per part, which outlives its reader as today.
  - `MemoryTable` and the test tables are one part.
  - `Scan` stays the serial path.
- **The plan splits at its first blocking operator.**
  - The pipeline below it is `Scan <- [Filter] <- [Compute] <- [Project]`. It is built once per part by the physical
    planner over `ScanPart`, and runs on the pool.
  - The sink is the blocking operator:
    - `Aggregate`: `AggregateState`s;
    - `GroupAggregate`: a grouper and `GroupedAggregateState`s;
    - `Sort`: a `SortBuffer`, with top-N `keep`;
    - for a plan without one, an ordered collector with the `Limit`.
  - The sink takes a partial result per part. Everything above the sink stays serial, over its small input.
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
  - `SessionOptions::threads` sets the size of a `ThreadPool` the `Session` owns; 1 means the serial path.
  - The default is the machine's hardware threads (`std::thread::hardware_concurrency()`), as in DuckDB, whose
    `threads` setting defaults to all hardware threads. The maintainer chose this on 2026-09-29. The hermetic test
    presets pin 1, except the `parallel` label, which pins 4.
  - The CLI gets `--threads` for `query`, `explain` and `bench`, and `antb1 bench` records the count in its JSON.
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

1. **io:** `Table::Parts`/`ScanPart`, `FileMetaData` reuse, multi-row-group fixtures. No behavior change.
2. **engine and exec:** the pool, `--threads`, the part pipeline with the global-aggregate sink and the ordered merge
   window, and error order. Tests with the `parallel` label (after approval).
3. **exec:** the GROUP BY sink (grouper merge through group maps, first-seen keys by part).
4. **exec:** the sort/top-N sink and the ordered projection with LIMIT early stop.
5. **Docs:** semantics (DOUBLE sums per row group), architecture.md, ADRs 0003, 0006 and 0010, and benchmarks.

## Alternatives considered

- **Per-thread partial states merged as threads finish:** less merging, but results depend on the schedule (DOUBLE sums,
  first-seen DOUBLE keys, group order). Rejected: the oracle and bug reports need answers that don't depend on thread
  timing.
- **Static contiguous ranges of parts per thread:** deterministic for a thread count but not across counts, and it
  load-balances poorly across uneven row groups.
- **Exchange operators and push-based pipelines (Volcano exchange, DuckDB-style push):** more general, for joins and
  deeper plans, but antb1's plans have one scan and at most one blocking operator below small serial tops. Revisit with
  joins (ADR 0003's pull-versus-push question).
