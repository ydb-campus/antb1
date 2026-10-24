# 29. Dynamic join filters: the build side's keys at the probe's scan

Date: 2026-10-18

## Status

Accepted (D0-D3 and D5 built and measured; D4's refreshed table carries the numbers it was accepted on. A
completed build publishes its keys sideways to the probe's scan, where they skip a part, a row, or a group below
the aggregate a join reads. D2 shipped and earns 0.2%, and two queries are left slower -- both recorded rather
than removed.)

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
    and not before it: two integers that nothing reads are two integers nobody can measure.
    - Correction (2026-10-19, D2 as built): it is not a separate mechanism to carry or drop either. A binary
      search over the published key set already finds nothing for a part lying outside `[min, max]`, so the
      interval is the **degenerate case of the key-set test**, not a second test beside it. It is published
      because it costs one pass over values already being walked, and because it is what answers when there are
      too many keys to publish as values; there was never a decision here to take.
    A **per-row** range test
    is deliberately not built at all -- it is the weakest of the three in both directions, since where the envelope
    is narrow the row-group test has already pruned before anything was decoded, and where it is wide the per-row
    test rejects nothing that the Bloom filter does not reject more precisely.
  - **each row group's range against the key set** prunes whole row groups, decoding nothing for a pruned one. Its
    reach is `e^(-k/g)` and therefore decidable before it runs. A Bloom filter cannot do this -- it tests points, not
    ranges -- so this is its own mechanism, asked before anything is decoded rather than through
    `ScanFilter::Apply`, which only ever sees values a table has already handed back.
    - Correction (2026-10-19, D2 as built): this said the test "needs its own entry point ... an addition to
      `plan::Table`, which makes it the one item here with an interface change in it". **It needs neither.** A
      Parquet part *is* one row group of one file (`plan/table.h`'s `num_parts`), exec drives parts, and
      `Table::part_stats(part, field)` already reports a part's exact integer statistics -- the top-N has pruned
      parts at runtime through it ([ADR 0011](0011-sorting-and-top-n.md), `exec/part_pruning.h`'s `PartMayBeat`). So the
      test belongs in exec beside that one, where the part is skipped before `ScanPart` is ever called, and this
      item carries **no interface change at all**.
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
      - Note (2026-10-19): **C6 shipped in D1's PR, so this ADR's window contains it, and its gain is not this
        ADR's.** C6 is worth 0.981x of the 22 on its own -- it unblocked *static* pushdown (ADR 0020) on every
        query reading a DECIMAL, which is most of them -- and that is recorded in ADR 0026, not here. Any
        before/after taken across D0-D3 therefore includes C6; D4's table says which share is whose, and the
        join-free queries that move in it (Q01, Q06) are the proof that it must.
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
    - **The build must not use the direct layout** (added 2026-10-24). A direct probe is two loads of `offsets_`
      and no hash at all, about half a nanosecond a row, where the filter costs about five -- so it cannot pay
      there **whatever it would reject**, and it is the one shape where it loses badly. The share bullet above
      does not cover it: that bounds the build's *rows* against the probe table's and says nothing about key
      coverage, so a build that is small in rows and covers every key its probe holds passes it.
      - **Measured by removal, which is the stronger direction, and it is worth more than it looks.** Refusing D1
        for a direct build takes away filters that reject **137,992,368** and **58,293,472** probe rows -- and
        the 22 get **faster**: geomean **0.979x on 4 threads and 0.988x on 14**, 7 repeats interleaved and
        rotated on the queries that move, spreads not overlapping.

        | | 4 threads | 14 threads |
        | --- | ---: | ---: |
        | Q11 | **0.838x** | 0.908x |
        | Q22 | **0.886x** | 0.928x |
        | Q2 | **0.923x** | 0.964x |
        | Q16 | 0.949x | 0.970x |
        | Q21 | 0.975x | 0.989x |
        | the 22, geomean | **0.979x** | **0.988x** |

        **Nothing regresses.** Q3 is the one to name: a 3-repeat control read it at 1.033x with separated
        spreads, and 7 rotated repeats put it at **1.006x** with overlapping ones -- the first reading was noise,
        and it is recorded because a 3% regression on the query this area was last tuned for is exactly what one
        would want to believe. Those rejected rows were real and bought nothing, which is the mechanism rather
        than an estimate of it.
      - **What it fixes outright.** A dense-key build of 5,000,000 keys under a 200,000,000-row probe whose keys
        it covers entirely -- the ordinary surrogate-key dimension under a fact table -- runs **0.705x** with
        this condition: the filter had been costing 42% for nothing, and that was true before the cap moved. The
        same shape at 10,000,000 keys, which the 32 MB cap newly admits, is **0.718x**. Synthetic, because no
        query of the 22 has the shape; `key_filter=1 key_dropped=0` was confirmed before either was timed.
      - **It gates D1 and must not gate D2.** A build publishes ONE `KeyFilter`, carrying both the per-row bits
        and the key interval the part test reads, so declining to publish would take part pruning with it. The
        condition lives in `CanFilterRows` (`physical_planner.cc`), which `KeyScanOf` consults separately from
        `PartHasNoKey`; a direct build still publishes, still prunes whole parts, and simply takes no per-row
        filter. Declining at publication instead fails four tests, one of them D2's own.
    - The filter must fit **32 MB** (16 MB until 2026-10-24). It is probed once a row with no locality at all, so
      it pays only while it stays in cache: filters of 6 to 14 MB are worth 0.72x to 0.96x, and a 45 MB one cost
      **17%** on a query whose build held a sixth of its probe's keys.
      - Update (2026-10-24): **raised to 32 MB, and the raise is the measurement this bullet had been missing.**
        16 MB was the conservative side of that gap and nothing had measured the middle, because until D1 could
        attach beside a pushed predicate set no query of the 22 had a refused candidate to spend the bytes on.
        That is an inference from the 1.001x null total ADR 0032 recorded, not a filter count on the pre-#227
        tree: had either of today's two gainers attached then, that run would have contained a 7 to 9% win and
        could not have read as a null. With the conjunction shipped, two queries do have a refused candidate.
        **Measured at SF 100, 7 repeats interleaved and rotated, spreads not overlapping, on both thread counts
        this project measures on** -- 14 threads being where this bullet's own curve came from and the least
        favourable to a bigger filter:

        | | 4 threads | 14 threads |
        | --- | ---: | ---: |
        | the two gainers | **0.795x** and **0.921x** | **0.843x** and **0.931x** |
        | the 22, geomean | 0.986x | 0.991x |
        | the 22's peak memory | 14,150 -> 14,092 MiB | 14,975 -> 14,916 MiB |

        No other query's plan moves, which is checked by **filter count** and not read off the timings: of the 22
        only these two change (1 to 2 and 3 to 4 filters), the three that read within 1% of noise gain none, and
        the rest publish no key filter in either arm.
      - **The band's worst case, measured rather than described.** The quantisation below excludes the one query
        the 17% was measured on and **only** that one, so the band needed its own experiment: a build of
        10,000,000 keys (the largest filter the cap admits, 32 MB) under a 200,000,000-row probe **every one of
        whose keys is in the build**, so the filter rejects nothing and is pure overhead. It passes the share
        gate above (10M <= 200M/16), which bounds the build's *rows* against the probe table's and not its key
        coverage. Synthetic, since no query of the 22 has the shape; 4 threads, repeats interleaved and rotated:

        | the same shape, zero rejection | probe cost | with a 32 MB filter |
        | --- | ---: | ---: |
        | **hashed build** (sparse keys, `direct=0`) | `find` 68 ns/row | **1.070x** |
        | **direct build** (dense keys, `direct=1`) | `find` 0.5 ns/row | **1.533x** |

        **1.070x is the raise's own exposure**, and it is in line with this bullet's existing curve. **1.533x is
        not: it is a pre-existing defect this cap neither creates nor fixes.** A direct build's probe is two
        offset loads, about 0.5 ns a row, so a filter costing ~5 ns a row cannot pay there **at any rejection
        rate** -- and `main` already attaches one, verified on a 5,000,000-key direct build over the same probe
        (`key_filter=1 key_dropped=0 direct=1`), where the filter is 16 MB and refused nothing. Raising the cap
        widens the key range over which that can happen; it is not what makes it happen.
      - **Both queries that gain here build `direct=0`** -- 14,567,861 and 11,997,461 rows, hashed -- so the
        measured win and the measured 1.533x are on opposite sides of a flag the build already computes. **The
        next item in this area is therefore to refuse D1 for a direct-layout build**, which would close the
        1.533x shape outright and keep both gains. It is not in this change because it also removes a filter one
        of the 22 currently carries on a small direct build, and whether that costs anything needs its own
        measurement.
      - **Quantised, so it cannot interpolate into the measured-bad size**: `bytes` rounds up to a power-of-two
        block count, so 32 MB admits builds of 8,388,609 to 16,777,216 keys and the next size is 64 MB -- the
        query the 17% was measured on builds 22,760,819 keys and stays refused. `key_filter_test.cc` pins that
        band from both sides.
      - **Two consequences worth naming.** A filter that is now made where none was holds up to 32 MB against a
        `MemoryBudget`, so under a tight `--memory-limit` a query that answered can fail on a later allocation,
        and a 32 MB charge reaches `under_pressure()` sooner; the ceiling is still `limit/8`, so the kind is
        unchanged and the magnitude doubles. And a build that now gets a filter also publishes its interval, so
        D2's part pruning becomes available to it where it was not.
  - **The measurement**, arms alternating per query on one binary, best of three: the 22 go 62.96 s to 60.30 s,
    **0.958x**. Seven queries gain -- 0.724x, 0.851x, 0.890x, 0.927x, 0.931x, 0.947x and 0.966x -- and the largest
    of them is the query D0 left with a 291,130,673-row probe, which is the prize this plan predicted.
  - **Two sub-second queries lose 43 and 47 ms, and the first diagnosis of that was wrong.** This entry first said
    the cost was "the filtered decoder being slower per row for that table's column mix". It is not: both of those
    queries read two BIGINT columns and no DECIMAL, and the arms that settle it hold the decoder constant. Putting an
    always-true predicate on the probe's scan makes the static pushdown take the single filter slot, so both arms run
    the *filtered* decoder and differ only in the key filter. On Q16 the scan then goes 746 ms to 1,443 ms summed
    over parts **while emitting 6.7x fewer rows**, and the query 477 ms unfiltered, 483 ms with the decoder switch
    alone and 521 ms with the filter. **The filter is 87% of the regression and the decoder switch 13%.**
  - **What decides whether a filter pays, measured on both sides.** The same arms over a query that gains 28% and one
    that loses 9%:

    | | the query that gains | the query that loses |
    | --- | ---: | ---: |
    | join time without the filter, summed | 13.819 s | 0.500 s |
    | join time with it | 1.634 s | 0.391 s |
    | **join time saved** | **12.2 s** | **0.11 s** |
    | extra scan time, the filter's cost | 5.6 s | 0.75 s |
    | the join's cost per probe row | **23 ns** | **6.3 ns** |

    So a filter pays exactly when the join costs more per probe row than the filter does, and the rejection rate --
    85% to 96% in every one of these -- does not discriminate at all. Neither does the build's size: the query that
    gains most has a 6,398-row build and the one that loses most a 40,045-row one. **There is no static quantity
    here that separates them**, which is why the gates above bound the filter's cost rather than predict its gain.
  - **The filter's own cost, and halving it.** 9.4 ns a row on that 80 M-row probe, split by short-circuiting the
    lookup while leaving the hash and the prefetch: **1.8 ns the hash and 7.0 ns the lookup**. The lookup is a read
    with no locality, so it is a prefetch's to fix, and prefetching the block 128 rows ahead brings the filter to
    **4.7 ns a row** -- Q04 0.724x to 0.622x, Q21 0.890x to 0.831x, and the 1.099x regression to **1.042x**, inside
    the band every other query of the 22 moves within. Rejected on the way: **8 bits a key instead of 16**, which
    halves the filter and should help its cache residency. It is worse on both queries (Q16 508.8 ms to 517.9,
    Q04 1.240 s to 1.266): the 467,000 extra false positives cost more than the smaller filter saves.
  - **What is left is one query at 1.147x, and it is partly ADR 0030's.** Its filter is about 128 KB, entirely in
    cache, so there is no miss for a prefetch to remove and its join's probes are cheap -- the shape where a filter
    cannot win. It is also one of the queries that **scans its probe side twice** for want of shared sub-plans
    ([ADR 0030](0030-shared-sub-plans.md)), so it pays the filter twice and saves little twice; eliminating the
    duplication halves the cost before anything else is tried.
  - **D3 is only half done here.** The scan reports *that* a filter is attached (a `key_filter` metric, per part);
    the rows it dropped would need a counter inside the filtered scan, since the scan operator above it sees only
    the survivors. That half stays with D3 -- which found it needed no counter in `io` at all, because the code
    that clears the bits is exec's own.
- **D2, perf(exec): skip a probe part no published key can be in.** The 27% the measurement found, from about `g`
  binary searches over the sorted key set. Worth it only where `e^(-k/g)` is large, which the engine can evaluate
  before publishing anything: both counts are known once the build finishes. **The min/max interval belongs to this
  item**, published with it and read by the same test. It will prune nothing in the 22 and nothing in ClickBench,
  for the reason the Context measures -- so it is carried on the argument that it is two integers collected in a
  pass that already has to happen, and this item's acceptance rests on the key-set test alone.
  - Update (2026-10-19, D2): **shipped, it prunes exactly what this ADR predicted, and it is worth almost
    nothing.** Both halves of that sentence matter, and the second is the one to read first.
    - **The prediction was right to within 1%.** On the query this ADR is built around the test prunes **1,337 of
      lineitem's 4,896 row groups** against the 1,325 `e^(-k/g)` predicts, and 6 of orders' 1,222 against 6.7. The
      profile reports both (D3's `key_pruned`), so this is the engine's own count and not an offline one.
    - **It is worth 0.998x of the 22**, 61.90 s to 61.78 s: the one query it reaches goes **0.978x** (median of
      seven) and every other query stays inside +-1.2%, which is this harness's noise. Across all 22 queries it
      prunes a row group on **one** of them -- no other build is small enough against its probe's row-group count.
    - **D1 had already taken this item's prize, and measuring row groups hid it.** The Context counted row groups
      and assumed the time followed. It does not: D1's filtered scan decodes the key column, rejects the rows and
      never touches the other columns, so a row group with no matching key was *already* cheap. Skipping the part
      saves that key column's decode, its hashes and its probes and nothing else -- the probe scan goes 5.034 s to
      3.692 s summed over 14 threads, which is about 96 ms of wall clock on a 4.5 s query. **A pruning mechanism's
      reach and its value are different quantities, and this ADR measured the first while arguing the second.**
    - **The item that would have paid is the one that shipped first.** Had D2 come before D1 it would have been
      worth most of what D1 is worth on that query, and D1 would then have looked marginal. The order was chosen
      (correctly) on D1 being the only mechanism that reaches a scattered key set -- but the corollary, that
      whichever of the two ships first collects the shared prize, was not stated and should have been.
    - **Three premises in the Decision were wrong, all in the cheaper direction**, and each is corrected inline
      above: no `plan::Table` entry point is needed (a part *is* a row group and `part_stats` already exists), the
      min/max interval is not a separate mechanism (the key-set search subsumes it), and D3's row counter does not
      belong in the filtered scan. What shipped is **exec-only**: no interface change, no `io` change.
    - **It reaches further than the per-row filter does, by construction.** A part that is never read needs no
      filtered decoder, takes none of the single `ScanFilter` slot, and costs one binary search whether it prunes
      or not -- so it applies to a probe scan that already pushes static predicates, which D1 must refuse, and it
      ignores the build-share gate that bounds D1's cost. No query of the 22 exercises that combination, so it is
      carried on its tests and not on a measurement.
    - **The values are published only up to `KeyFilter::kMaxPublishedKeys` (65,536).** Above it the interval alone
      is published, which costs one pass and no memory. That is a bound on the publication's cost, not a guess at
      its benefit: the test itself is free once published. It is why the two SEMI/ANTI joins of another query --
      7.3 million keys each -- get nothing, which is the right answer, since `e^(-k/g)` there is zero to many
      decimal places.
    - **Shipped rather than withdrawn, and the reasons are here for D4 to weigh** -- this entry records a
      measurement, not an acceptance; the Status line above is still `Proposed`, and only a maintainer moves it.
      For: it regresses nothing, its reach is arithmetic over two counts known before it runs (90% of row groups
      at a 500-key build, 82% at 1,000, 27% at 6,398, nothing by 50,000), and the mechanism is about 320 lines of
      exec confined to one path. Against: 0.2% of a gate set is below what new code usually has to earn, and ours
      holds one 6,398-key case and nothing smaller, so the reach that would pay is not represented in it.
- **D3, feat(exec): report what each filter pruned in `EXPLAIN ANALYZE`**, each half with the item that produces it
  -- rows with D1, row groups with D2 -- and not after either. Rows and row groups skipped, and whether the filter
  was ready: without that last one, a filter that never fires and a filter that is never ready are the same timing.
  - Update (2026-10-19, D3): **shipped, as two metrics on the scan and not three.**
    - `key_dropped`, the rows the per-row filter cleared, counted **inside the filter** rather than in the
      filtered scan as D1's entry supposed. The code that clears the bit is `KeyScanFilter::Apply`, which is exec's
      own, so it counts what it clears: one atomic add per call and not per row. A row another filter had already
      cleared is not counted, because the bit is tested before it is touched.
    - `key_pruned`, the parts the key set ruled out, written by the planner where it decides them. It is a count
      and not a flag, so the per-part profile sums it to the figure above: 1,337 on the query that has it.
    - **Together they account for the probe exactly, which is the first time either mechanism's share has been
      visible.** On that query's lineitem probe: **163,106,013 rows never read** (the 1,337 row groups D2 pruned),
      **436,744,092 read and dropped** by D1's filter, and 187,797 handed to the join -- the three summing to the
      table's 600,037,902. So **D1 does 73% of the rejecting and D2 27%**, which is the same split the timing
      shows and the quantitative form of "D1 had already taken this item's prize".
    - **The readiness metric is not implemented, and this is the record of why.** D1 established that a build
      completes before any probe part is constructed (`PrepareBuilds` runs before the sink that schedules them),
      so there is no not-ready state for a filter to be in and nothing for the metric to distinguish. Left
      unsaid, a reader would keep looking for it.
    - Nothing was needed to carry a metric from the scan: `Profiled` already calls `Operator::set_profile` on
      every operator, so `TableScanOperator::Close` reports through `profile()` like any other operator.
- **D5, perf(exec): apply a published filter below the grouped aggregate a join reads.** Not in this ADR's first
  plan, and found by asking what scan sharing would do for a query [ADR 0030](0030-shared-sub-plans.md) had
  wrongly named: the answer was that its real cost is a grouped aggregate averaging **20,000,000** parts when the
  join above reads the **20,031** that pass a filter. [ADR 0023](0023-unnesting.md)'s Path 2 replaces the domain D
  with a map, which keeps the answer and drops the restriction D carried -- so the aggregate has no way to know.
  This item puts that restriction back from the one place that has it: the join's build, which already publishes
  the key set D1 attaches to probe scans.
  - Update (2026-10-19, D5): **shipped, 0.935x of the 22 -- the largest single item of this ADR.**

    | | before | after |
    | --- | ---: | ---: |
    | the query | 5.833 s | **1.797 s (0.308x)** |
    | its groups | 20,000,000 | **20,031** |
    | its local group insertions | 319,678,579 | **599,206** |
    | its peak memory | 1.87 GB | **138 MB** |
    | the 22 | 61.19 s | **57.20 s (0.935x)** |
    | the other 21 | | 1.0009, this harness's noise |

    It closes that query from **4.6x of DuckDB to 1.35x**, and the answer is DuckDB's to the last digit.
  - Update (2026-10-22, D5): **a Filter between the join and the aggregate dropped the offer**, so D5 reached
    only an aggregate whose input had no `WHERE` of its own. The field carrying the offer names the nodes that
    pass it on -- "a Filter as it is, a Project and a Compute with the key remapped, and every other node drops
    it" -- and `Compute` and `Project` each do, each with the remap its node needs. `Filter`, named first and
    needing no remap at all, never read it: its fallback passed six arguments where those two pass seven, so the
    offer became a null pointer. The fix is that seventh argument.
    - **The symptom is sharper than the cause**: a predicate that removes nothing at all still cost the filter.
      Over one tenth-scale fact table, the aggregate built 147,241 groups with no predicate below it and
      **1,500,000** with a predicate keeping every row. Selectivity had nothing to do with it, and neither did
      the scan's one filter slot -- a predicate the scan cannot apply leaves that slot free and the offer was
      still dropped.
    - **Measured on the shape this item was built for**, plus a predicate below the aggregate that the scan
      cannot apply, at SF 100 and four threads:

      | | before | after |
      | --- | ---: | ---: |
      | the query | 8.373 s | **2.791 s (0.333x)** |
      | rows out of its scan | 600,037,902 | **24,175,686** |
      | its groups | 20,000,000 | **805,847** |
      | its peak memory | 2,121 MB | **391 MB** |
      | the 22 | | **1.002x -- no query of the 22 has this shape** |

      That last line is the honest reading: this is worth 3x where it applies and nothing on this benchmark,
      because every `WHERE` the 22 put below such an aggregate is one the scan can apply itself.
    - **Why the suite could not see it.** D5's only positive case has no `WHERE` inside its inline view, which
      is the one arrangement that worked, and its three refusals assert that nothing is dropped -- so they went
      on passing for the wrong reason. Three cases join it: a predicate the scan cannot apply, red before the
      fix; the refusal on a key the aggregate does not group by **with a Filter in the way**, a combination
      nothing could reach before and which must still refuse, since a Filter does nothing to make the
      aggregate's own value safe to filter on; and the limitation the fix stops at, pinned so that lifting it is
      a visible change.
    - **Carried 2026-10-23: a Filter whose predicates go into the scan** (ADR 0020). They used to hold the one
      filter slot a scan takes, leaving the published keys nowhere to attach.
      - **The policy decision this was left for does not have to be made.** The item asked which of two filters
        to choose when only one slot exists, "and which prunes more is not knowable at planning time". The answer
        is to choose neither: `exec::MakeBothScanFilters` applies their conjunction, which the interface already
        allowed -- a `ScanFilter` only ever *clears* bits of the mask it is handed, so running both in either
        order leaves the rows that pass both, and neither has to know what the other did.
      - **And the cost this item feared is already paid.** The 9% regression measured above came from a scan that
        the *always-true predicate* had switched to the filtered decoder (13% of it) plus the filter itself
        (87%). A scan that reaches this case has real pushed predicates, so it was decoding with a filter
        already: the key filter adds its own per-row cost against join work it can only reduce.
      - **Measured** at SF 100, 4 threads, 3 repeats, arms interleaved: the 22 go **0.992x** in geomean and
        0.991x in total, with **no query slower** (the worst is 1.003x, inside its own spread, and the query that
        lost 9% above is unchanged at 1.000x). The two that gain are 0.958x and 0.976x. On the first, the probe
        falls from 148,067,261 rows to 11,488,814 -- `key_dropped=136,578,447` -- and its join's `find` from
        2.936 s to 728 ms, while its scan goes 15.926 s to 17.181 s summed: **a 92% row prune is worth 4% and not
        more**, because reading the rows is the cost a row filter cannot avoid and that scan is the query.
      - **Two economies that look obvious over a shared scan, measured.** The filter now often runs after a
        predicate has cleared most bits, so both of these were put to the 22:
        - **Testing the bit before issuing the prefetch is 1.010x, and 1.028x on the query it was for** (7
          repeats, interleaved and rotated). A prefetch is a non-blocking hint whose line is evicted harmlessly;
          the bitmap read that avoids issuing one is not. A single run had said 0.976x, which is what three
          repeats are worth on a sub-5% effect. Not done, and recorded at the loop so it is not retried blind.
        - **Hashing only the selected rows is worth about 3%** on that query -- `KeyHashes` runs over the whole
          array, 600,037,902 rows of which 148,067,261 are still selected, about 0.81 s summed over parts. It
          needs the selected values compacted before hashing and a threshold for when that beats hashing
          everything. **Deferred as its own item**, with that sizing as its trigger.
      - **Not carried with it:** the filter a join above a grouped aggregate offers (D5) still yields to pushed
        predicates, because it is offered in the scan's own branch of the planner and a scan with predicates is
        built in the `FilterNode` branch. The same conjunction would serve it; it is not measured here, so it is
        not done here. `src/exec/tests/aggregate_filter_test.cc`'s `YieldsToPredicatesPushedIntoTheScan` still
        stands on that behaviour.
  - **The correctness condition is one sentence**: the filter is applied to a column the aggregate **groups by**,
    so it removes whole groups and never changes a surviving group's value. On any other column it would corrupt
    every aggregate. Mutation-checked -- dropping that requirement fails a test.
  - **An approximate filter is sound here, and that is why this is cheaper than the delim join.** A Bloom filter
    has no false negatives, so it can never remove a group the join needs; false positives leave extra groups,
    which the join never reads. **The exact domain is not required, only a reduction** -- so this needs none of
    D's duplicate elimination.
  - **The key travels as a ColumnId, remapped at each node rather than carried.** Positions move; ids survive one.
    A Project mints a new id even for a column it passes through, so without the remap the match at the aggregate
    fails silently -- and the shape this was built for has a Project and a Compute between the join and the
    aggregate.
  - **The ordering is self-enforcing, against what the plan for this item assumed.** It is offered on the probe
    side because that is where the build is prepared first -- but a build that is not prepared publishes no
    filter, so reading one too early attaches nothing. It cannot produce a wrong answer, only a missed filter,
    which is also why no test can catch the difference.
  - **What it does not do**: ADR 0023's general duplicate-free domain, which stays deferred there and is justified
    by the shapes it would *answer* (COUNT, OR) rather than by this query's speed. The two do not conflict; if the
    general algorithm later restricts such an aggregate exactly, this filter finds nothing left to drop and the
    build-share gate refuses it.
- **D4, docs(adr): record what D0-D2 measured**, and either accept this ADR or defer it with the numbers.
  - Update (2026-10-19, D4): **the cumulative measurement, taken fresh end to end rather than chained.** Each item
    above was measured against its own contemporaneous baseline, and those baselines drift between sessions, so
    the per-item ratios do **not** multiply out to the figure below -- which is the reason to take one matched
    pair rather than trust arithmetic over four runs.
  - **Protocol**, stated because the first attempt at it was wrong: both arms were built and measured in a single
    run, each preceded by a **discarded warm-up pass over all 22**, best of three, antb1 and DuckDB interleaved
    per query. The first attempt measured the two arms in separate sessions and attributed a 7-10% warm-up and
    page-cache asymmetry to the code; the warm-up pass and DuckDB-as-control are what remove that. SF 100,
    14 threads, `before` = the last commit with none of D0-D3 in it.

    | | before | after | |
    | --- | ---: | ---: | --- |
    | the 22, total | 69.46 s | 60.35 s | 0.869x |
    | the 22, geomean | 2.384 s | 2.147 s | 0.901x |
    | DuckDB, total (the drift control) | 29.78 s | 29.51 s | 0.991x -- so the move above is antb1's |
    | against DuckDB, total | 2.33x | 2.05x | |
    | against DuckDB, geomean | 2.18x | 1.98x | |

  - **Where the 9.10 s went, which is not where this ADR's plan put it.** Q18 0.504x, Q04 0.609x and Q21 0.828x --
    the three queries D0 and D1 were built for -- are **6.25 s of it, 69%**. Most of the rest is C6's static
    pushdown (the note under D1), spread a few percent over queries no item here was aiming at. Two queries
    regress, both already recorded under D1: Q11 1.129x and Q16 1.052x.
  - **Re-measured (2026-10-19), because the table above was taken before D5 and before
    [ADR 0030](0030-shared-sub-plans.md)'s E2 and is no longer the cumulative figure.** The numbers above stand as
    what was true when they were taken; these supersede them. Same protocol -- both arms built and measured in one
    run, a discarded warm-up pass each, best of three, antb1 and DuckDB interleaved per query -- and the same
    `before`, the last commit with none of this work in it:

    | | before | after | |
    | --- | ---: | ---: | --- |
    | **the 22, total** | 71.09 s | **55.97 s** | **0.787x**, 15.12 s saved |
    | the 22, geomean | 2.438 s | **2.001 s** | 0.821x |
    | DuckDB, total (the drift control) | 30.73 s | 30.23 s | 0.984x -- so the move above is antb1's |
    | **against DuckDB, total** | 2.31x | **1.85x** | |
    | **against DuckDB, geomean** | 2.16x | **1.80x** | |

  - **Attribution, since this window holds three ADRs and not one.** The five named items' queries are **12.04 s
    of the 15.12 s, 80%**: Q17 **0.284x** (D5), Q18 0.503x (D0 with D1 and D2), Q15 0.482x (ADR 0030's E2), Q21
    0.831x and Q04 0.627x (D1). The remaining 3.1 s is spread thinly -- Q19 0.824x, Q08 0.805x, Q14 0.843x, Q01
    0.890x, Q06 0.884x and six more inside 10% -- and belongs to ADR 0026's C6, which is also where it is
    recorded. **Q01 and Q06 hold no join at all**, so they cannot be any filter of this ADR's: that is the check
    that keeps the attribution honest. The same two queries regress as before, both recorded under D1: Q11 1.160x
    and Q16 1.053x.
  - **D5 is the largest item of the ADR**, which no version of this plan predicted: it was found by asking what
    scan sharing would do for a query ADR 0030 had misnamed, and it restores a restriction ADR 0023's unnesting
    discards rather than adding a filter of its own.
  - **Per item, as measured by each**: D0 0.550x on its one query and 0.975x of the 22; D1 0.958x of the 22; D2
    0.998x; D3 no timing of its own; D5 0.935x of the 22. So **D0, D1 and D5 are this ADR, and D2 is a mechanism
    that works and does not pay**. Note that these five do not multiply to 0.787x, and the pair above is the
    figure: each was measured in its own session against its own control, and D5's control arm alone read 61.19 s
    where the previous pair's "after" read 60.35 s -- a session-to-session drift larger than several of the items.
    **A cumulative claim needs a cumulative pair, not a product of ratios.**
  - **Correction (2026-10-19): this pair reads 0.828x, not 0.787x, when the arms run in the other order.**
    [ADR 0026](0026-join-performance.md)'s C12 measured four arms in one session, two of them these same two
    trees. The "after" arm reproduces to 1.0% (55.97 s against 56.51 s); the "before" arm differs by 4.0%
    (71.09 s against 68.28 s), and the direction follows arm order -- it ran first and cold here, third and warm
    there. DuckDB registered 1.7% of that, so the drift control under-reported the effect it exists to catch.
    **Read this entry as 0.79x to 0.83x, and the gap as 1.85x to 1.89x**; C12's figures are the pessimistic ones
    and are preferred where the two disagree. The decision below does not turn on the difference, and the
    attribution above is unaffected: it divides one run's arms, not two runs' totals.
  - **Twice the value landed somewhere the plan did not predict**, which is the transferable lesson: D2 arrived to
    find D1 had already taken the prize it was costed on, and C6 was planned as plumbing for D1 while being worth
    more than its own entry expected. Both are failures of *ordering* analysis rather than of measurement -- the
    plan reasoned about each item alone, and two mechanisms over the same rows share one prize, which goes to
    whichever ships first.
  - **Accepted on these figures (2026-10-19), by the maintainer.** The case put both ways before the call, and
    recorded here because the negatives are part of what was accepted. For: the work closes **0.787x** of the 22
    and the gap to DuckDB from 2.31x to **1.85x**, with four of its five items paying and every mechanism
    measured, bounded and instrumented. Against: D2 earns 0.2% and the reach that would pay for it is not
    represented in this gate set, and two queries are left slower than they were -- Q11 1.160x and Q16 1.053x,
    both the cost of a filter that cannot pay on them, bounded by `kFilterBuildShare` and recorded under D1.
    **Accepting this ADR accepts those two numbers**, which is why they are in the Status line as well. D2 stays
    in the tree rather than being reverted: it is correct, instrumented by D3, and costs a part's statistics
    lookup where it does not pay -- its case is a key set small relative to the probe's row-group count, which
    neither gate set has.

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
