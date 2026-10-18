# 29. Dynamic join filters: the build side's keys at the probe's scan

Date: 2026-10-18

## Status

Proposed (draft: the measurement below is taken, the design is not built)

## Context

- **The gap this addresses.** On the 22 queries derived from TPC-H at SF 100, 14 threads, best of three, antb1's
  geomean is 2.409 s against DuckDB's 1.117 s (**2.16x**) and the totals are 70.2 s against 30.3 s (2.31x). Seven
  queries carry 61% of the 39.8 s deficit, and the two worst run 3.9x and 4.5x. That ratio has been flat for three
  orders of magnitude of scale — 2.6x at SF 0.1, 2.9x at SF 1, 2.7-2.9x at SF 10, 2.7x at SF 100 on 4 threads — so it
  is a constant factor and not something volume makes worse. ClickBench, by contrast, is at parity: 1.00x on execution
  geomean and 0.83x over 37 of its 43 queries. **This is join-shaped work. No ClickBench query joins at all, so none
  of it is measurable there.**
- **The anatomy of the worst query**, from `explain --analyze` at SF 100 on 14 threads. It groups on five keys above a
  semi join whose build side is an aggregate with a `HAVING`:

  ```text
  PartGroupAggregate  keys=[c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice]
    HashJoin SEMI     o_orderkey = l_orderkey   ->          44,786 rows   (the answer's input)
      HashJoin INNER  o_orderkey = l_orderkey   ->     600,037,902 rows   gather=26.692s
        Scan lineitem                                 600,037,902 rows
    HashBuild SEMI    (the filter side)        ->           6,398 rows
  ```

  The inner join materialises **600,037,902 rows and gathers five columns for every one of them**, and the semi join
  above it then keeps 44,786. A second inner join below gathers 150,000,000 rows (13.916 s). Summed over parts,
  `gather` is 40.6 s of the query's work; peak memory is **15.36 GB**. The engine is not slow at gathering — it is
  gathering rows it is about to discard, four orders of magnitude more of them than the query needs.
- **DuckDB answers the same query in 2.17 s, and its plan says how.** It computes the 6,398-row filter side first,
  then pushes that information *sideways* into both Parquet scans as **optional dynamic filters**: the orders scan
  carries `o_orderkey >= <min> AND o_orderkey < <max> AND o_orderkey IN BF(#0)` and the lineitem scan carries
  `l_orderkey IN BF(o_orderkey)` — a Bloom filter over the keys that survived, plus a min/max range. Its big join
  therefore never sees those rows. The word in its own plan output is `optional`, which is the property that makes
  this safe: a dynamic filter that is absent, late or imprecise costs time and never changes an answer.
- **Why [ADR 0026](0026-join-performance.md)'s C5 does not reach this, measured rather than assumed.** C5 shipped on
  2026-10-16 — DPccp dynamic programming over the connected subsets, with C1's filter-aware estimates — and the
  measurement above was taken on 2026-10-18 with it in place. Two independent reasons it cannot help here:
  - **A semi join is not in the search.** [ADR 0022](0022-joins-and-query-blocks.md) decides that "an outer, semi,
    anti or one-row join is a fixed unit for ordering", and the binder's `JoinBlock` holds only "the relations the
    commas, CROSS JOINs and INNER JOINs of one FROM list connect" (`src/plan/binder.cc:3075`). `join_order.cc`
    contains no join-kind handling at all: it orders inner edges over relation masks. So the semi join sits above the
    block by construction, and no cost model inside the block can move it.
  - **The selectivity is not in any footer.** What matters is that `GROUP BY` + `HAVING` leaves 6,398 of 150,000,000
    order keys — a factor of 23,000. That is the output cardinality of an aggregate with a predicate on the
    aggregate. C5's estimates are, in its own words, "integer arithmetic over metadata"; no Parquet footer records
    this, and no static rule can derive it. **The information exists only at runtime, which is why the filter must be
    dynamic.**
- **The seam already exists, and it does not cross a module edge.** `plan::ScanFilter`
  (`src/plan/include/antb1/plan/table.h:48`) is an abstract class "implemented by exec, applied by tables that
  support it (`Table::supports_scan_filter`)", already documented as "thread-safe: the parts of a query apply one
  filter concurrently". `exec` depends on `common;plan` and "never io: exec scans through plan::Table"
  (`cmake/Antb1Modules.cmake:21`). A dynamic join filter is therefore **a `ScanFilter` whose contents arrive at
  runtime** — the same interface ADR 0020's pushdown already uses, with no new dependency and no new module edge. The
  one thing the current interface does not express is *becoming ready*: `Apply` is `const`, and a filter is handed to
  a scan when the scan starts, not when a build finishes.

## Decision

Draft. What follows is the shape to build and measure, not a decision taken.

- **A completed hash build publishes a key filter; the probe side's scan reads it.** When a `HashBuild` finishes, it
  knows its key column's exact contents. It publishes, per key column: the **min and max**, the **null count**, and —
  when the build is small enough to be worth it — a **blocked Bloom filter** over the key hashes it has already
  computed. The probe side's scan applies them as an extra `ScanFilter`.
- **Two uses, in increasing cost.** Min/max prunes **whole row groups** from the Parquet footer, which is the
  mechanism ADR 0020's pushdown already has and which decodes nothing at all for a pruned group. The Bloom filter
  prunes **rows** within a surviving group, before the other columns of those rows are decoded — the order ADR 0020's
  filtered scan already uses, predicate column first and the rest for survivors only.
- **Optional, in DuckDB's sense, and that is a correctness property.** A dynamic filter is a pure filter over rows
  the join would have dropped anyway. So:
  - a filter that is not ready when a batch is scanned is skipped, and that batch is filtered by the join as today;
  - a false positive costs one row's work and nothing else; a false negative must be impossible, which is what
    bounds the design to min/max and Bloom and excludes anything approximate in the other direction;
  - the answer is byte-identical for any thread count, any arrival order and any cache state. **Determinism is not
    traded here**, and no `.slt` expectation, golden or ratchet changes. A test that asserts a *row count read* would
    be non-deterministic and must not be written; a test that asserts the *answer* is exactly the right test.
- **Readiness is the one interface change.** `ScanFilter::Apply` stays `const`, and a dynamic filter holds its
  payload behind an atomic "published" flag: written once by the build thread, read by every scanning part. Where the
  probe's scan begins before the build completes, the early batches simply go unfiltered. Nothing waits on anything,
  so no new scheduling order and no new deadlock surface — which is the property that makes this cheaper than it
  looks.
- **Bounded memory, decided from the build's own row count.** A Bloom filter is sized from the completed build's rows
  at a fixed bits-per-key, capped, and allocated on the Arrow pool like every other exec allocation so that it is
  inside `MemoryBudget` rather than beside it. Above the cap, min/max alone is published. Memory is what broke this
  workload before ([ADR 0026](0026-join-performance.md)), and a filter that saves 15 GB of materialisation must not
  cost an unbounded fraction of it back.

## Consequences

- A new runtime information path, build -> scan, that the module graph already permits: it is `exec` implementing a
  `plan` interface that `io` applies. No ADR-worthy edge, which is why this ADR is about measurement rather than
  architecture.
- `EXPLAIN ANALYZE` should say what a filter did — rows or row groups pruned, and whether it was ready — or the whole
  mechanism is unobservable and untunable. This is also where it becomes reviewable: a filter that never fires and a
  filter that is never ready look identical in a timing.
- The engine gains a cost that does not appear in any plan: hashing probe rows against a filter that may prune
  nothing. A selective-looking join that is not selective pays for the filter and gains nothing, so the published
  filter needs a cheap abandonment rule (publish min/max always, Bloom only when the build is small relative to the
  probe's estimated rows — an estimate C1 already computes).
- Nothing here helps ClickBench, and that should be said plainly in any PR that claims a win: the acceptance
  measurement is the 22 at SF 100, with ClickBench as a no-change control.

## Plan

One PR per id. **D0 is first and may make D1 and D2 much smaller**, so it is measured before either is built — the
lesson of [ADR 0027](0027-string-views.md)'s S1, which was planned as a win whose consumer did not exist.

- **D0, feat(plan): apply a semi join as early as the join graph allows.** Not a dynamic filter at all, and not this
  ADR's to own — it is a change to ADR 0022's "fixed unit for ordering" rule, and belongs there or in ADR 0026. It is
  listed here because it addresses the same measurement and is far cheaper: a semi join **cannot increase the row
  count**, and when its keys come entirely from one relation of the block (as `o_orderkey` does), applying it to that
  relation before the inner joins is semantically a filter — each joined row matches exactly when its row from that
  relation matches. In the query above that is 600,037,902 rows gathered becoming of the order of 44,786, with no
  runtime machinery whatsoever. Measure it first; if it lands, D1 and D2's remaining prize is the `lineitem` scan
  alone, which is the half DuckDB uses a Bloom filter for.
- **D1, feat(exec): publish a completed build's key min/max, and prune row groups with it.** The cheap half of the
  mechanism, reusing ADR 0020's footer pruning. Measured on the 22 at SF 100 and 14 threads, with ClickBench as the
  no-change control. The gate is the queries whose profile shows a large `gather` under a selective join; if none of
  them moves by 10%, D2 is not worth building.
- **D2, feat(exec): a blocked Bloom filter over the build's keys, applied per row.** Only with D1's measurement in
  hand, and only where D1's min/max proved too coarse — a key range that spans the whole column prunes no row group
  at all, which is exactly the case a Bloom filter answers.
- **D3, feat(exec): report what each filter pruned in `EXPLAIN ANALYZE`**, with D1, not after it.
- **D4, docs(adr): record what D0-D2 measured**, and either accept this ADR or defer it with the numbers.

Deliberately out of scope: filters derived from a Parquet file's own Bloom filters, which are static and answer a
different question; carrying a filter *between* queries; and late materialization, which is
[ADR 0016](0016-late-materialization.md)'s different answer to the same measurement — do not gather the columns
rather than do not gather the rows.

## Alternatives considered

- **D0 alone, and no dynamic filters.** The honest possibility, and the reason D0 is measured first. If letting a
  semi join sit where the graph allows removes the 600 M-row materialisation, the remaining win is the one scan
  DuckDB's second Bloom filter covers, and this ADR may reduce to a note. Rejected as *the plan* only because it
  cannot cover the general case: an inner join between two large relations where one side is filtered carries no semi
  join to push, and that is the shape of several of the seven worst queries.
- **Better static estimates.** Tried and shipped: ADR 0026's C1 (filter-aware estimates) and C5 (DPccp over them).
  The measurement in the Context is taken *with both* and shows the defect intact. The missing number is the output
  cardinality of an aggregate with a `HAVING`, which metadata does not hold. Rejected on evidence.
- **Sampling the build side to estimate selectivity, then reordering.** Replaces an exact runtime fact with an
  estimate of it, keeps the planner in charge, and costs a pass over data. Rejected: the exact keys are already in
  the build's hash table, so there is nothing to estimate.
- **Materialising the join output lazily (late materialization).** ADR 0016. It reduces the *columns* gathered per
  row rather than the rows, so on the query above it would still carry 600,037,902 row references. Complementary at
  best, and the measurement points at the row count.
- **A blocking dependency: have the probe's scan wait for the build.** Would make every filter ready and so maximise
  pruning. Rejected: it serialises two pipelines that run concurrently today, introduces a scheduling order where
  there is none, and trades a deterministic speed-up for a deadlock surface. The optional form gets most of the win
  because a build that is small is also fast.

This workload is derived from the TPC-H Benchmark and is not comparable to published TPC-H Benchmark results, as this
implementation does not comply with all requirements of the TPC-H Benchmark.
