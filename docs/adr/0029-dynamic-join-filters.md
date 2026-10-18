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
- **Which pruning mechanism can reach it, measured on the same query (2026-10-18).** The filter side's 6,398 keys
  span **99.99% of the key domain**, so a single min/max interval from the build prunes **no row group at all**: it
  is one interval covering everything. Testing each row group's *own* range against the key set is a different
  question and prunes **1,337 of lineitem's 4,896 row groups (27%)**, over an average span of about 122,500 keys a
  group. That 27% is structural rather than a property of this data: with `k` keys scattered over `g` row groups the
  share that contains none is `e^(-k/g)`, which for 6,398 over 4,896 predicts 1,325 against the 1,337 measured. So
  the reach of row-group pruning is **known in advance from two counts** -- 90% at 500 keys, 82% at 1,000, 27% at
  6,398, nothing by 50,000 -- and it pays only where the build is small relative to the probe's row-group count. Only
  a per-row test reaches the rest, which is why DuckDB's plan carries a Bloom filter here and not merely a range.
  **A scattered key set is the case against min/max and the case for Bloom, not evidence against both.**
- **Why the min/max interval fails here, which is not the reason it looks like.** The probe side is *ideally* ordered
  on the join key: those 4,896 row groups each span **0.0204%** of the key domain and their spans sum to **100.0%**,
  so they tile it contiguously and a range test is positioned to prune almost everything. What defeats the interval
  is the **build** side, not the probe: 6,398 keys scattered over a domain the probe tiles in 4,896 pieces put at
  least one key in 73% of the pieces, so the envelope `[min, max]` covers every piece while the key *set* misses
  1,337 of them. The fix for that is a finer question, not a wider interval -- which is the row-group test above.
- **And in this workload the interval has no home at all, measured rather than assumed.** It is tempting to expect a
  date-filtered dimension to be min/max's case. It is not: at SF 100 the orders rows of a three-month window are
  5,735,195 of 150,000,000 (3.8% selectivity) and their `o_orderkey` range still spans **100.00%** of the domain,
  as does a single-priority filter's 29,995,209 rows. `dbgen` draws `o_orderdate` independently of `o_orderkey`, so
  no filter on a non-key column narrows the key envelope. A min/max runtime filter would therefore prune nothing
  anywhere in the 22, and ClickBench has no join to put one on. Its real home is a build side **contiguous** on the
  join key -- a date-partitioned or key-sorted table -- which neither benchmark has, so it would ship unmeasurable.
  DuckDB publishes one regardless (its plan carries `o_orderkey >= <min> AND o_orderkey < <max>` beside the Bloom
  filter), and by this measurement it earns nothing there either: it is carried because it is nearly free and
  because a range is the only filter a Parquet statistics path can take.
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
  knows its key column's exact contents. It publishes, per key column, what the mechanism below it is going to read
  and nothing more: the **null count** and, when the build is small enough to be worth it, a **blocked Bloom filter**
  over the key hashes it has already computed, which the probe side's scan applies as an extra `ScanFilter`; the
  **sorted key set** and its **min and max** arrive with the row-group test that reads them, since each is published
  by the item that consumes it.
- **Three uses, not two, and they do not substitute for each other.** The measurement above separates them:
  - a **min/max interval** prunes row groups from the footer, and reaches nothing once the keys are scattered. It
    shares the row-group test's entry point and its measurement, so it is published and applied **with that item**
    and not before it: two integers that nothing reads are two integers nobody can measure. A **per-row** range test
    is deliberately not built at all -- it is the weakest of the three in both directions, since where the envelope
    is narrow the row-group test has already pruned before anything was decoded, and where it is wide the per-row
    test rejects nothing that the Bloom filter does not reject more precisely.
  - **each row group's range against the key set** prunes whole row groups, decoding nothing for a pruned one. Its
    reach is `e^(-k/g)` and therefore decidable before it runs. A Bloom filter cannot do this -- it tests points, not
    ranges -- so this is its own mechanism and needs its own entry point: `ScanFilter::Apply` sees values a table has
    already decoded, where this must be asked *before* the decode, against footer statistics. That is an addition to
    `plan::Table`, which makes it the one item here with an interface change in it.
  - a **per-row Bloom filter** prunes rows inside a surviving group, before that row's other columns are decoded --
    the order ADR 0020's filtered scan already uses, predicate column first and the rest for survivors only. It is
    the only mechanism that reaches a scattered key set, so on the query above it is the one that pays.
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
  inside `MemoryBudget` rather than beside it. Above the cap **nothing is published and the probe runs as it does
  today**, which is the one behaviour a filter that cannot pay for itself should have; the fallback is not a cheaper
  filter, since the cheaper filter prunes nothing on the data this ADR measures. Memory is what broke this workload
  before ([ADR 0026](0026-join-performance.md)), and a filter that saves 15 GB of materialisation must not cost an
  unbounded fraction of it back.

## Consequences

- A new runtime information path, build -> scan, that the module graph already permits: it is `exec` implementing a
  `plan` interface that `io` applies. No ADR-worthy edge, which is why this ADR is about measurement rather than
  architecture.
- `EXPLAIN ANALYZE` should say what a filter did — rows or row groups pruned, and whether it was ready — or the whole
  mechanism is unobservable and untunable. This is also where it becomes reviewable: a filter that never fires and a
  filter that is never ready look identical in a timing.
- The engine gains a cost that does not appear in any plan: hashing probe rows against a filter that may prune
  nothing. A selective-looking join that is not selective pays for the filter and gains nothing, so each published
  filter needs a cheap abandonment rule, and each has one that is arithmetic rather than a guess: the Bloom filter
  only where the build is small relative to the probe's estimated rows, which C1 already computes, and the row-group
  test only where `e^(-k/g)` is large, which is two counts the build has in hand. Where neither holds, nothing is
  published.
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
  - Update (2026-10-18, D0): **shipped, and worth 1.82x on the query this ADR is built around.** A WHERE semi join
    whose key reads one relation of the statement's block now runs on that relation's own branch
    (`Binder::PushableSemiJoin`), guarded on the relation's estimate not exceeding the estimate of the tree the join
    would otherwise stand on: a semi join probes the rows of whatever it stands on, so moving it below a very
    selective join would cost more than it saves. [ADR 0022](0022-joins-and-query-blocks.md)'s "fixed unit for
    ordering" now carries the exception. Measured on the 22 at SF 100, 14 threads, best of three, on one binary with
    the pass behind a temporary override removed before the commit:

    | | before | after | |
    | --- | ---: | ---: | --- |
    | the query, end to end | 8.598 s | **4.729 s** | **0.550x**, 3.9x to **2.1x** of DuckDB |
    | rows its largest join outputs | 600,037,902 | **44,786** | the rows the answer needs |
    | that join's `gather`, summed over parts | 26.692 s | 7.451 s | |
    | peak memory | 15.36 GB | 13.65 GB | |
    | the 22, geomean | 2.402 s | 2.341 s | 0.975x, 2.15x to **2.09x** of DuckDB |

    **Only that one query moved.** The other 21 are inside +-2.8%, and DuckDB's own binary read 1.117 s and 1.118 s
    across the pair of runs, which is the drift control. It is the only one of the 22 whose `IN (subquery)` stands
    over a FROM of more than one relation. ClickBench cannot be affected at all: none of its 43 queries holds a join
    or a subquery.
  - **What D0 leaves for D1 and D2, from the same profile.** That join now outputs 44,786 rows but still *probes*
    291,130,673 of them (`find` 3.184 s, `self` 10.878 s), because the 600,037,902-row scan of its probe side is
    untouched -- nothing yet tells it which keys can match. That is exactly the Bloom filter's half, and it is now
    the largest single cost of the query, which is the order this plan predicted. Peak memory fell by only 1.71 GB,
    so the 600 M-row join output was never the peak; the other build is, and that is a separate question from this
    ADR's.
- **D1, perf(exec): a blocked Bloom filter over a completed build's keys, applied per row.** The mechanism the
  measurement says pays, so it is first of the dynamic filters rather than last: it is the only one that reaches a
  key set spanning its whole domain. The min/max interval is **not** published here, nor is the sorted key set --
  their only reader is D2, and ADR 0027's S1 is this ADR's standing lesson about shipping a producer before its
  consumer. All the other plumbing lives here: publishing from a finished build, the atomic readiness, the memory
  bound, and the probe side finding its filter -- which is why the cheaper-looking item is not the smaller one.
  Measured on the 22 at SF 100 and 14 threads **after D0**, so that D0's win is not counted twice, with ClickBench
  as the no-change control (no ClickBench query joins).
  - Update (2026-10-18, D1): **shipped, 0.958x of the 22, and three of this ADR's assumptions were wrong.**
    - **It needed a prerequisite this ADR did not know it had.** `supports_scan_filter` is gated on *every* field a
      scan reads, and the filtered path had no DECIMAL case, so a scan reading one could take no filter at all --
      including both probe scans of the query this ADR is built around. [ADR 0026](0026-join-performance.md)'s C6
      shipped first and is what let D1 reach them.
    - **The filter is always ready, so none of the readiness machinery above was built.** `PrepareBuilds` runs
      before the sink that schedules the probe pipeline's parts, and the serial path before `input_->Open`, so a
      build completes at one global moment on the consumer thread and every probe part is constructed after it. No
      atomic flag, no unfiltered early batches, no new ordering: the bullet above is simply not needed.
    - **The per-row hashes do not survive the build**, against "over the key hashes it has already computed":
      `JoinTableBuilder::Finish` frees every `JoinBuildPart` and the direct layout keeps no hash at all. So
      `JoinTableBuilder::MakeKeyFilter` fills the filter from the parts' pieces *before* Finish. Setting a bit twice
      sets it once, so the filter does not depend on the order the parts arrived in or the merges ran in.
    - **The null count is not published either.** Nothing reads it: a NULL key matches nothing on either side, so a
      probe row with one is dropped by the filter not holding it, which is the answer the join gives anyway.
    - **The probe hashes with `exec::KeyHashes`, the function the build hashed with.** That is the whole of the
      correctness argument for the hash, and a hash of its own would have been the one mistake that loses answers
      silently rather than loudly.
  - **Three gates decide when a filter is used, and each came out of a regression rather than from taste.**
    - The join must not emit an unmatched probe row: INNER and SEMI always, ANTI and LEFT only when they build on
      their left (`EmitsBuildRows`). An ANTI building on its right emits precisely the rows a filter would drop.
    - The build must hold at most a sixteenth of the probe's rows. Without this, a query whose build held every key
      of its probe's domain ran **1.138x**: the filter rejected nothing and the probe paid the hash for it.
    - The filter must fit 16 MB. It is probed once a row with no locality at all, so it pays only while it stays in
      cache: filters of 6 to 14 MB are worth 0.72x to 0.96x, and a 45 MB one cost **17%** on a query whose build
      held a sixth of its probe's keys.
  - **The measurement**, arms alternating per query on one binary, best of three: the 22 go 62.96 s to 60.30 s,
    **0.958x**. Seven queries gain -- 0.724x, 0.851x, 0.890x, 0.927x, 0.931x, 0.947x and 0.966x -- and the largest
    of them is the query D0 left with a 291,130,673-row probe, which is the prize this plan predicted.
  - **Two sub-second queries lose 43 and 47 ms (1.132x and 1.099x), and the cause is not the filter.** Both probe the
    same table, and their scans do drop 96% of the rows they read: the filtered decoder is simply slower per row for
    that table's column mix than the unfiltered one. No gate separates them honestly -- the rejection rate is ~96%
    for both the query that gains 28% and the ones that lose, so a probe-row floor would be curve-fitting to five
    points. Deliberately left open rather than papered over, and the maintainer has parked it: the next step is to
    find out why that decoder is slower per row for those columns, which is a question about `filtered_scan.cc` and
    not about dynamic filters.
  - **D3 is only half done here.** The scan reports *that* a filter is attached (a `key_filter` metric, per part);
    the rows it dropped would need a counter inside the filtered scan, since the scan operator above it sees only
    the survivors. That half stays with D3.
- **D2, feat(plan+io): ask a table whether a row group can contain any published key.** The 27% the measurement
  found, from about `g` binary searches over the sorted key set. It needs the new entry point named in the Decision,
  since `ScanFilter::Apply` is handed values that have already been decoded, and a pruned row group is one that is
  never decoded at all. Worth it only where `e^(-k/g)` is large, which the engine can evaluate before publishing
  anything: both counts are known once the build finishes. **The min/max interval belongs to this item**, published
  and applied through the same entry point, which is the only place anything reads it. It will prune nothing in the
  22 and nothing in ClickBench, for the reason the Context measures -- so it is carried on the argument that it is
  two integers behind a gate that already has to exist, and this item's acceptance rests on the key-set test alone.
  If that reads as too thin a justification, the honest alternative is to leave the interval out until a workload
  with a contiguous build side is in the gate set.
- **D3, feat(exec): report what each filter pruned in `EXPLAIN ANALYZE`**, each half with the item that produces it
  -- rows with D1, row groups with D2 -- and not after either. Rows and row groups skipped, and whether the filter
  was ready: without that last one, a filter that never fires and a filter that is never ready are the same timing.
- **D4, docs(adr): record what D0-D2 measured**, and either accept this ADR or defer it with the numbers.

**The gate, stated so it cannot be read backwards.** An earlier draft of D1 gated the Bloom filter on min/max having
moved a query by 10%, which is exactly wrong: min/max pruning nothing *is* the signal that the keys are scattered,
and a scattered key set is what a Bloom filter is for. The gate is D1's own measurement on the queries whose profile
shows a large `gather` or probe under a selective join, taken after D0; and D2's gate is `e^(-k/g)` on those same
queries, which is arithmetic over two counts and needs no run at all.

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
- **A min/max runtime filter as a mechanism of its own, ahead of the Bloom filter.** The cheapest thing to build
  and the first thing to reach for: two integers from a finished build, applied through the footer statistics the
  scan already reads. Rejected as an item, and reduced to a passenger of D2, on measurement rather than on taste --
  0 of 4,896 row groups on the query this ADR is built around, and a key envelope spanning 100.00% of the domain
  even behind a 3.8%-selective date filter, because `dbgen` draws the filtered column independently of the key.
  The trigger to promote it: a gate workload whose **build** side is contiguous on the join key, which is the shape
  where it prunes whole row groups for two integers and where the Bloom filter is the redundant one. A key-sorted or
  date-partitioned table would create one; TPC-H and ClickBench do not.
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
