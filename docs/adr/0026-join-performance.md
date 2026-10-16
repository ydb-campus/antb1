# 26. Join performance: build sides, right-sided joins and cost-based order

Date: 2026-10-14

## Status

Proposed

## Context

- **The triggers fired.** [ADR 0022](0022-joins-and-query-blocks.md) deferred its performance items each "until its
  trigger", and a run of all 22 queries derived from TPC-H at SF 1, SF 10 and SF 100 — against DuckDB and ClickHouse
  over the same Parquet, outside the repository as [ADR 0006](0006-test-strategy-and-data-policy.md) requires —
  fires three of them as written:
  - *"cost-based join order (dynamic programming, selectivity estimates, equivalence edges, key ranges carried across
    joins), until a query of the SF 1 nightly run is far slower than DuckDB, or builds a very large table, because of
    its join order"*: Q21 builds the whole of the largest relation and runs 19.6x DuckDB on 8 threads.
  - *"building on the preserved side (right outer, right semi and right anti joins) and one build probed by several
    pipelines, until profiles of Q13 or Q20-Q22 at SF 10 or above, or the general unnesting domain, need them"*: the
    profiles are of Q13 and Q20-Q22 at SF 100, and they ask for exactly this. The trigger named the queries correctly.
  - *"pressure measured without the pinned builds, until a memory-limit test or a run at SF 10 or above shows probes
    running one part at a time"*: at SF 100 a build, not a probe, is what serializes — see the chain below.
- **The trigger's own precondition does not exist.** No workflow runs any of the 22: `bench.yml` runs the micro
  benchmarks and ClickBench, on a push to `main` that touches `src/`, on `workflow_dispatch`, or on a pull request
  carrying the `performance` label. There is no "SF 1 nightly run", so the evidence here comes from a run outside the
  repository at SF 100 — a stronger condition than the trigger asks for — and this ADR adds the on-demand run that
  makes it repeatable.
- **Where the gap is.** antb1 is 3.2x DuckDB's geomean at SF 100 on 4 threads and 3.4x on 8. It is not a scaling
  defect: 10x the data costs a median 10.0x the time, with 21 of the 22 between 7.3x and 11.7x, and each doubling of
  threads holds 85-88% efficiency, within a point or two of DuckDB's. So the gap is per-operator cost, and it
  concentrates in joins. Six queries that each join four or more relations (Q5, Q7, Q8, Q9, Q10, Q21) sit at a 5.33x
  geomean ratio; five a scan and an aggregate dominate (Q1, Q6, Q13, Q14, Q22) sit at 1.70x. Scan and aggregate
  throughput are already close to DuckDB's. Bringing the six to the level of the five takes the overall ratio to
  2.34x and nearly halves the wall clock of the 22.
- **The cause: the estimates ignore filters, so the build side is often the big one.** `OrderJoins` picks the probe by
  rule 1, the relation with the most rows, and does not pick the build side at all: `src/plan/join_order.cc` sets
  `BuildSide::kRight` unconditionally, computing an `estimate` on the line above and never reading it — which is what
  the comment in `src/plan/join_order.h` says it does. Footer row counts know nothing of filters, nor of what a join
  below produced, so wherever a filter or an earlier join shrinks one side the choice is made on stale numbers. In 12
  of the 22 queries, 14 joins build on a side 2x to 185x larger than the stream that probes it: worst are Q2 at 185x,
  Q21 at 82x and 54x, Q22 at 79x, Q4 at 66x, Q20 at 63x, Q12 at 48x, Q17 at 33x, Q8 at 12x and Q13 at 10x. The other
  join shapes hard-code the side too (the binder's LEFT JOIN units and subquery joins, and the unnesting rewrites of
  ADR 0023).
- **It is a memory problem before it is a speed problem.** Peak memory is set by the wrong side: 5 of the 22 pass
  8 GB at SF 100, Q21 reaches 36 GB on a 48 GB host, and Q4 holds 15 GB for a query that finishes in under six
  seconds. ADR 0022 judged "the workload far below half of the default limit up to SF 100 on the development host";
  that no longer holds for Q21, and it is the build side, not the data, that broke it.
- **The memory then costs the parallelism,** in a chain whose top is the item above:
  1. the estimates ignore filters, so Q21's semi join builds the whole of the largest relation instead of the small
     stream that probes it;
  2. the build peaks at 36 GB;
  3. `OneAtATime` in `src/exec/join_table.cc` gives the build a null executor once the budget passes half the memory
     limit, and `ForEach` in `src/exec/partition_lanes.cc` then runs the 64 partition lanes on the calling thread, so
     the one phase of the build that could use the pool does not. The CLI's default limit is 80% of physical memory,
     which puts the threshold well below this build;
  4. so Q21 gains 1.08x from 4 to 8 threads where the other 21 gain 1.65x to 1.94x and DuckDB's own Q21 gains 1.91x,
     and it is the one query of the 22 that scales superlinearly with the data, at 39.7x for 10x. Measured directly,
     that build's `finish` phase is 11.0s on 4 threads and 10.4s on 8.

  Each step follows from the one above it, so the first fix is the only one that must land.
- **What the measurements do not support.** A scan gets no filter pushdown at all when any column it reads is
  DECIMAL: `ParquetTable::supports_scan_filter` requires every projected field to be one `FilteredColumnOf` handles,
  and DECIMAL is not. This is deliberate and documented — [ADR 0020](0020-filter-pushdown.md): "A scan that reads one
  gets no pushdown ... until the decimal64 item of ADR 0021" — and it is why Q12 is the only scan of the largest
  relation in the workload that filters while decoding, its projection being the only one without a DECIMAL column.
  It is also worth little here: a full scan of one integer column of that relation at SF 100 takes about half a
  second warm, and losing the pushdown above it costs about a tenth of that. Row-group pruning cannot help either,
  because the workload's order dates do not correlate with its order keys, so a row group's date range spans nearly
  the whole domain and `skipped` is 0 everywhere. ADR 0022 already orders runtime join filters after "DECIMAL reaches
  the filtered scan"; this ADR keeps that order and keeps the item last.

## Decision

### The build side comes from estimates that account for filters, behind one ratio (C1)

`JoinRelation::rows` becomes an estimate that applies the conjuncts the binder has already attributed to that single
relation, and `OrderJoins` decides `JoinStep::build` from the estimates instead of hard-coding `kRight`. Rule 1 reads
the same estimates, so the probe and the build side move together: a relation that a filter shrinks stops being
chosen as the probe for having the most rows in its footer.

Selectivity uses only what the footers already give, as ADR 0022 requires of a plan ("from metadata alone, and the
same for any thread count"): `Table::part_stats` has exact minimum and maximum for integer, DATE and USMALLINT
columns, so a range conjunct estimates from the overlap of its range with the column's, and `KeyDomain` already reads
those statistics the same way. A column type without statistics — DECIMAL, which `parquet_table.cc` excludes per
[ADR 0021](0021-decimal-semantics.md) — takes a documented default factor, never a silent one.

Two rules keep a selective-looking conjunct from estimating a relation into nothing, because a relation estimated at
no row is ordered first and then makes every later estimate in its block 0 as well, which would decide the whole
order and every build side after it:

- **An equality and its complement both read the range.** A constant the column's range excludes matches no row, so
  the equality keeps none and the complement keeps all. Without the second half, a column of one distinct value
  estimates `col <> other` at almost none of its rows, when in truth it keeps every one — a plan strictly worse than
  the unfiltered footer count gave, in the direction this ADR exists to fix.
- **A complement never estimates no row.** Where the matching part saturates, the rest is floored at one row rather
  than rounded to zero.

**One knob decides how much to trust it.** The side flips only when the estimated build side exceeds the estimated
probe side by more than a ratio, `--build-side-ratio`:

- at **4.0**, the default, the choice is conservative: wherever the estimates are close, today's plan stands, so an
  estimate that is wrong cannot move a join that is already right;
- at **1.0** it is ADR 0022's original rule, "the input with the smaller footer row count ... and on the relation
  being added on a tie", now with filters;
- unknown on either side keeps `kRight`. Unknown must never read as small.

The ratio exists so that the default is answerable by measurement rather than by argument: one sweep over the 22 at
SF 100 chooses it, and that sweep is recorded here when C1 lands. `BuildSide::kLeft` already works in the physical
planner and the hash join, and is reached today only from tests, so C1 is a planner change alone.

### Building on the preserved side (C2, C3)

A semi, anti or left outer join builds on the side it does not preserve, so for these the ratio has nothing to
choose: the shape fixes the side, and it is the wrong one wherever the preserved side is the smaller. Q21, Q4, Q22
and Q13 are that case, and together they hold the largest builds in the workload.

The mirror of each is the standard one: mark the build rows a probe matched, then emit the marked rows (right semi),
the unmarked rows (right anti), or the unmarked rows padded with NULLs (right outer). The direction is already a
field — `JoinNode::build` — so no new `JoinKind` is needed and the enum stays at six.

Update (2026-10-15, C2a): **C2 was one PR and is now three.** A read of the hash join against this design says the
marking is the easy half and the emitting is not, and that the split falls on a line of correctness rather than of
convenience.

What is already there: a dense build-row index, the position in `JoinTable::rows()`, which is exactly what
`JoinMatches{begin, end}` carries — so a mark bitmap of `num_rows()` bits needs no new addressing, and `num_rows()`
is final before any probe row is read.

What is not:

- **Where the tail goes is undecided, and it is C2b's first question.** A right-direction join must emit its build
  rows once, but the probe runs per part: a fresh `HashJoinOperator` is made, drained and destroyed per part, up to
  twice the thread count at a time, and `End()` already releases the table at the input's end. The only
  all-parts-done signal is `PartSink`'s single `parts_done` slot, which `BuildsFirstOperator` already owns in order to
  release the build. Three candidates, none free: emit inside the operator with a serial probe (**rejected** — in the
  mirrored direction the probe is the large side, so this trades the whole point away); emit at the sink (correct only
  where the join is the last streaming operator of its probe pipeline — true of Q4 and Q13, false of Q21, which has an
  anti join above it, and of Q22, which has a Compute and a Project; it buys memory rather than time, since Q4 and Q13
  hold 14.6 GB and 12.6 GB at ratios of only 2.73x and 1.85x); or make the join pipeline-breaking, consuming its
  probe pipeline and becoming a source for a new one, which is the only candidate that reaches Q21 and is ADR 0022's
  separately deferred "one build probed by several pipelines" — the two were deferred together because they are one
  piece of work.
  - Update (2026-10-15, C2b): **decided, and it is the third candidate: the join becomes the sink of its own probe
    pipeline.** The decision section below records the design and what reading the planner changed about it.
- **A NULL-key build row is discarded, not merely unindexed.** `JoinBuildPart::Append` keeps only selected rows
  whose keys are not NULL, so their payload never enters `chunks()`. Right **anti** and right **outer** must emit
  unmatched build rows, and a NULL key is unmatched by definition, so both need the build to keep them. Right
  **semi** does not: a NULL-key row never matches, so its absence is already right. That is why C2b is semi alone.
- **Marks are the first mutable state two probes would share.** `JoinTable` is handed out as
  `shared_ptr<const JoinTable>` with no mutable and no atomic member, documented lock-free for any number of probe
  threads, so a bitmap must be atomic even for set-only writes; and `PartScheduler` may rerun a part and discard the
  first run, so a mark must be idempotent — an OR is, a count is not.
- **A residual forces per-row marks.** With one, each (probe row, build row) pair is accepted or rejected
  individually, so a key's whole match range cannot be marked wholesale. Both of Q21's joins carry residuals.
- **The dense index is dropped one line after it is read**, when a `rows()` position becomes a `JoinRowRef` and no
  reverse map exists, so the position has to be carried alongside.

**The hazard C2a closes.** `src/exec/hash_join.cc` switched on `JoinKind` twice behind `default:` arms, so neither a
new kind nor a new direction failed to compile: a right-semi join would have fallen into the arm that emits slices of
the probe batch — the left-sided answer — with no error and no assert. Nothing produced that combination (the
`ResolvePositions` check rejects it and the operator's `Make` returns `Invalid`), so it was latent; it was also
exactly the trap C2b and C2c would have walked into, which is why it is closed before them rather than with them.

### Where the tail goes: the join becomes the sink of its probe pipeline (C2b)

A right-direction join **owns the `PartScheduler` of its probe pipeline**, consumes every part while emitting
nothing, and then emits the build rows from its own `Next()`. It is a sixth member of the `PartSink` family, not a
new concept in the execution model.

Reading `src/exec/physical_planner.cc` moved this design twice, and both corrections are the point of writing it
down.

**What is not available.** The first sketch had the join become a *source* for a new part pipeline above it, so that
the rest of the plan stayed parallel. The planner has no such shape: a part pipeline's bottom must be a `ScanNode`
with a table (`PipelineScan`), and above every one of the five existing `PartSink` subclasses sits a **serial** chain,
built with no part number. Nothing in the engine terminates one part pipeline and begins another. Representing a
materialised row set as a pipeline source is a much larger change than C2b, and this ADR does not take it.

**What is available, and is the precedent to copy.** `JoinBuild` over a pipeline input is already a pipeline breaker:
it owns its own `PartScheduler`, consumes a whole part pipeline, and its product — a `JoinTable` rather than batches —
is read by the probes of a different pipeline. At most one scheduler is ever live, by construction, because
`BuildsFirstOperator` prepares the builds before it opens the sink. A right-direction join is that same shape with
one more phase: build, then probe-and-mark over its own scheduler, then emit.

Owning the scheduler also disposes of two traps that the sink-level candidate would have had to solve. The join never
needs `PartSink`'s single `parts_done` slot — already taken by `BuildsFirstOperator` to release the build — because it
knows directly when its own scheduler is drained. And it is unaffected by `parts_done` firing *before* output in four
of the five sinks, which would have made a tail keyed on that signal emit too early.

**The cost, stated plainly.** Everything above the break runs serially. For Q21 that is an anti join and a grouped
aggregate over the roughly 7M rows the semi join emits — on the order of a second, against the 110s the query takes
and the 36 GB and serial `finish` phase that building the whole of the largest relation costs. The trade is strongly
favourable there, but it is a trade, and C2b measures it rather than assuming it: a query whose break sits under a
large parallel chain could lose.

**What the marking must satisfy**, each from the code rather than from first principles:

- **The marks live on `JoinBuild`, never on `JoinTable`.** The table is handed out as `shared_ptr<const JoinTable>`
  and documented immutable for any number of threads; `JoinBuild` is reachable non-const from a probe. Adding a
  mutable member to the table would change a contract that every probe depends on.
- **Set-only and idempotent, an OR and never a count.** `PartScheduler` stops the parts ahead of one that ran out of
  memory, discards their results and runs them again, and `RunPart` abandons a part mid-probe on `stop`. A part's
  marks can therefore be written twice, or partially and then again.
- **Atomic, because the writers are concurrent.** Up to twice the thread count of independent `HashJoinOperator`
  objects probe one shared table, and a per-part `ExecContext` has no executor, so a probe cannot schedule the work
  itself.
- **The `rows()` position must be carried through the residual path.** It is the only dense build-row index, and it
  is converted to a `JoinRowRef` and dropped one line after it is read. Without a residual the whole matched range
  can be marked at once; with one, only the pairs that survive the residual may be marked, which is per-row.
- **The bitmap is reserved on the consumer thread and charged before it is allocated**, as every other container of
  a build is. A `MemoryReservation` is non-movable and single-threaded, so it belongs to the build, never to the
  per-part operator that releases its own memory in `End()`. Charging it raises `bytes_allocated()`, which pushes a
  large build toward `OneAtATime`'s half-the-limit threshold — this ADR's C4 cliff — so C2b reports the peak it adds.
- **Emission in `rows()` order is deterministic across thread counts**, which is what ADR 0022 and ADR 0013 require
  and what `antb1-slt --same-as-threads` enforces. It is not stable across datasets: the order is ascending key in
  the direct layout and bucket order in the hashed one, and the layout is chosen by key density. That is sound, no
  query having an order without `ORDER BY`, and worth knowing before reading a diff of two plans' output.
- **Early stop must suppress the tail.** A `LimitOperator` above simply stops pulling, which is correct — the tail is
  the output being limited. The per-part row cap is the trap: parts stop early and `parts_done` still fires, so a
  tail keyed on that signal would emit build rows whose matches were never probed. A join that owns its scheduler
  passes no row cap, so the path does not arise, and C2b has a test that pins it.

**One guard must move in four places at once.** `PipelineInput` picks a join's probe input with a hard-coded test for
an inner join building on its left, and the same combination is rejected in `ShapeOf`, in `HashJoinOperator::Make`
and in `plan::ResolvePositions`. `PipelineScan`, `FiltersOnScan` and `PipelineBuildsOf` all walk pipelines with
`PipelineInput` and must keep agreeing with it. C2a made the two `JoinKind` switches exhaustive so that this is a
compile error rather than a silent probe-side answer; that was the point of shipping it first.

### Join order (C5)

Choosing the build side does not change which joins run first, and the row counts between joins say that order is
the other half. Four of Q7's five joins each output the same rows, and only the fifth cuts them, by 312x; Q8 and Q5
do the same over fewer joins. This is ADR 0022's cost-based ordering proper — rules 1 and 4 with selectivity — and
it comes after C1, which is its prerequisite, and after C3, which is worth more for less.

### What stays deferred

Runtime join filters and part pruning by build keys keep ADR 0022's order, after DECIMAL reaches the filtered scan;
`plan::ScanFilter` is built once at physical-plan time from static predicates, and nothing reaches back from a build
into a probe's scan. Spilling keeps its own trigger of SF 300. Q10's 5.5x to 6.5x is not a build-side case — its
worst build is 1.3x the stream probing it, not the 10x and above of the queries above — so it needs its own profile
before it gets a roadmap item.

## Consequences

- A plan still depends on metadata alone, so it is the same for any thread count and any batch size, and EXPLAIN
  stays a function of the catalog. Selectivity from exact footer statistics keeps that property; a sampled or
  historical estimate would not, and is not proposed.
- `build=left|right` is printed on every join already; C1 adds the estimate beside it, so a wrong side is visible
  from `explain` without `--analyze`. This is also what makes C5 reviewable.
- A query's peak memory becomes a function of the smaller side of each join rather than of its text. That is the
  change that matters most at SF 100 and above, and it is what ADR 0022's SF 300 spilling trigger was reserved for.
- C1 changes plans, not answers. `pixi run diff-random` and the metamorphic relations `star_join_commutes` and
  `star_join_order_three_tables` are what prove that, and they are the gate this work leans on.
- The on-demand run (C7) makes the triggers above measurable inside the repository for the first time. It stays off
  `push` and `pull_request` so that no PR pays for it.

## Plan

One PR per id. C1 is the prerequisite for C3 and C5: nothing chooses a side without estimates that account for
filters. Each PR reports the 22 queries at SF 100 before and after, and updates the docs/sql-subset.md sections it
changes.

- **C1, feat(plan): filter-aware relation estimates and the build-side ratio.** `JoinRelation::rows` applies the
  conjuncts of its own relation; `OrderJoins` takes the ratio and sets `JoinStep::build` from the estimates; rule 1
  reads them too. A `BindOptions` parameter on `plan::Bind`, the ratio on `engine::SessionOptions`, and
  `--build-side-ratio` on the CLI, defaulting to 4. Plans change, answers do not.
  - Update (2026-10-15, C1): shipped, and it moves **Q8 and Q12**, not the five this bullet first guessed. Q12's
    probe flips to the unfiltered relation and the filtered one builds; Q8 gets two `build=left` joins and, because
    rule 2 now reads the same estimates, its most selective join moves to the bottom of the tree -- a part of C5
    arriving early. Q2 and Q17 do not move: their oversized build side is a subquery or an aggregate, whose rows
    `EstimateRows` reports as unknown, and unknown keeps the build where it is. Q20's is a semi join, so it waits for
    C3. Estimating through a join and an aggregate is C5's work, and Q2 and Q17 are the measurements that ask for it.
  - Measured at SF 100 on 4 threads, best of 2, against the same run before the change: **Q8 falls 57%** and its
    ratio to DuckDB goes from 4.52x to **2.03x**; **Q12 falls 11%**. Nothing regresses: the worst query moves +0.2%.
    The two are the only queries outside the run-to-run band, which DuckDB's own unchanged binary set at 1.9% to 9.5%
    over the same pair of runs -- so the geomean, 3.20x to 3.10x, is partly a warmer cache and only Q8 and Q12 are
    this change. Both are exactly the queries whose `build=` and join order moved, which is the agreement that makes
    the attribution safe.
  - EXPLAIN printing the estimate next to `build=` is deliberately **not** part of C1: it would rewrite about fifteen
    golden files and thirteen in-code assertions with pure diagnostics and bury the rule change. `build=left|right`
    already shows which side was chosen, so the golden churn of C1 is exactly the plans that moved.
- **C2a, fix(exec): make the join-kind switches exhaustive.** Both switches in `hash_join.cc` list every enumerator,
  so a kind or a build direction that belongs elsewhere fails to compile instead of silently taking the probe-side
  arm, and a test pins that the binder never produces `BuildSide::kLeft` for a non-inner kind — which only an
  aborting internal check stood behind, and which C1 made worth pinning by making `kLeft` reachable from real SQL.
  No plan and no answer changes. **Shipped first, before C2b and C2c, because it is the trap they would walk into.**
- **C2b, feat(exec): build-side output for semi joins.** Shipped 2026-10-15, with C3's rule for a semi join: the
  mark bitmap (`JoinMarks`), the `rows()` position carried through the residual path, and the join as the sink of its
  own probe pipeline (`PartRightJoinOperator`). Semi alone, because it needs no change to the build.
  - **Measured on the sf100 harness at 4 threads, best of 2:** Q21 went from 110.60 s to 32.50 s, **3.4x faster**,
    and its ratio to DuckDB from 11.10x to 3.2x; its peak memory from about 36 GB to 18.81 GB (the profiled run,
    which is the one the peak comes from, takes 33.0 s). At SF 10 it went from 2.808 s to 2.625 s and 3.62 GB to
    1.89 GB. The semi
    join's build fell from every row of the largest relation to the outer side's: 82x fewer rows. The time won is far
    more than the memory saved would suggest on its own, which is the reading this ADR expected: at 36 GB against
    this host's 51.5 GB the query was evicting the page cache for a 33 GB dataset, so it also explains why Q21 was
    the one query that did not answer to thread count.
  - **What the design traded away, measured:** the serial chain above the break costs nothing here. Q21's aggregate
    now runs on one thread over 39,448 rows and 4,009 groups, and the anti join above the break is serial too; both
    are in the 33.1 s. The bitmap's own peak is one bit per build row — 8 bytes per 64 rows — and does not approach
    C4's `OneAtATime` threshold. **So C2c follows**, and the remaining 18.81 GB is mostly its anti join's build,
    which is still every filtered row of the largest relation.
  - **Two findings from the work**, neither of which this design had anticipated:
    - A residual's columns are the join's left input's and then its right input's, but the pair-batch split read
      them as the probe's and then the build's. Those agree only while the probe is the left input. The 53 existing
      hash-join cases all pass with the old split, so it was latent, and it is the second trap of the kind C2a
      closed — found by a test with relations of unlike values, since one with the same values on both sides cannot
      tell the two orderings apart.
    - The pipeline boundary is not expressible in `PipelineInput` alone. `PipelineScan` is asked two different
      questions — *where does the pipeline this node tops begin* and *may I run this node inside a pipeline of
      mine* — and a right-direction join answers them differently. It takes a parameter now, and only its own sink
      passes it. Where its probe has no part pipeline at all there is no sink to emit from, so the physical planner
      keeps building on the right instead: the side is a performance choice and never a semantic one, which is what
      lets it.
  - Sequenced after a smaller change that moved the probe-side guard in `PipelineInput`, `ShapeOf`,
    `HashJoinOperator::Make` and `plan::ResolvePositions` in lockstep. That went in as the first commit of the same
    PR rather than its own: with the operator still refusing the combination, nothing could exercise the moved
    guard, so splitting it would have shipped a diff no test could reach.
- **C2c, feat(exec): build-side output for anti joins.** Shipped 2026-10-16 with C3's anti-join rule, as C2b
  shipped with its semi one and for the same reason: neither moves a query alone. **Plain anti only**, where the
  ADR had said "anti and left outer" — the measurements do not ask for the other two, and a dated note below
  defers them.
  - **A prerequisite that had to ship first**, found while planning this and confirmed on the shipped binary
    before any of it was written: Q21's anti join **would not have flipped at all**. The unnesting pass rewrites
    top-down, so the outer of two nested dependent joins is rewritten while its left input still holds the inner
    one, and `EstimateRows` read a `DependentJoinNode` as unknown — which rule 5 must treat as "never read as
    small". Q21's `NOT EXISTS` is the outer one, so every bit of C2c's exec work would have been dead weight.
    C2b's semi only flipped because its `EXISTS` is the inner one, which also means **C2b shipped leaving this on
    the table**. An estimate now reads through a dependent join to its left input, an upper bound for every kind
    the pass admits, and `Path1` and `Path2` keep that bound on the join they build. That change moved no TPC-H
    plan on its own: Q21 is the only one of the 22 with two correlated subqueries at one level.
  - **Measured at SF 100, best of 2:** Q21's peak memory fell from 18.81 GB to **4.27 GB**, 4.4x, and its anti
    join's build from every filtered row of the largest relation to the outer side's — 54x fewer rows, the same
    ratio the semi join gave. Time fell 12.9% at 4 threads (32.87 s to 28.64 s) and 15.1% at 8 (19.11 s to
    16.22 s); the ratio to DuckDB went from 3.25x to 2.86x. At SF 10 the peak fell from 1.89 GB to 433.81 MB.
  - **The time win is far smaller than C2b's 3.4x, and the page-cache reading is why.** C2b's win was outsized
    because 36 GB against this host's 51.5 GB was evicting the cache for a 33 GB dataset; 18.81 GB was no longer
    doing that, so what is left here is the operator's own work. The same reading predicted this, which is some
    evidence it is right. The memory is the headline, and it is what makes Q21 safe at a larger scale factor.
    Thread scaling is now healthy too: 4 to 8 threads buys 1.77x, where before C2b the query ignored threads.
  - **Two findings from the work.** The retained rows had to go in a store of their own, not in `chunks_`:
    `JoinTableBuilder::Merge` numbers a `Segment::chunk` by counting pieces, and that numbering must equal
    `JoinTable::Build`'s push order or every `JoinRowRef` silently addresses the wrong batch, with nothing
    checking it. And a batch retained before the keyed rows of the same `Append` survived a later failure in that
    call, so the part was not unchanged and its Arrow buffers stayed charged — caught by a test written because
    `RunsOutOfMemoryCleanly` cannot see it, since a `MemoryReservation` returns everything in its destructor
    before that test's assertion runs.
  - Q21's anti build reaches `DrainInput` rather than a part pipeline, because its build input is the semi join's
    serial output: the 54x oversizing becomes a drained 707,593 rows, not zero.
- **C2c's remainder, deferred 2026-10-16: right null-aware anti and right outer.** Not on effort but on the
  measurements. Q22 (`NOT IN`, a null-aware anti join) is **1.05x** DuckDB and Q13 (`LEFT JOIN`) **1.79x**, so
  neither has a win to collect, and each costs more than plain anti did: a null-aware anti join must suppress every
  output row once any probe key is NULL, which needs a flag the probes write and the tail reads, and it must then
  withhold its own NULL-key rows rather than emit them; a left outer join emits matched pairs *during* the probe as
  well as padded rows after it, so the sink would carry two emission paths. Until a profile of Q13 or Q22 at SF 100
  or above asks for them, `HashJoinOperator::Make` refuses both and says so.
- **C3, feat(plan): choose the build side for semi, anti and outer joins.** C1's estimates with C2b's and C2c's
  operators, one kind at a time as each lands. This is where Q21's 36 GB, its 10.8x and its 1.08x thread scaling go,
  and with them Q4, Q22 and Q13. The largest single win of this ADR.
  - Update (2026-10-15): **the semi-join half shipped with C2b, in the same PR**, as the rule the unnesting pass
    applies to the join it creates (`ChooseBuildSide`, the same function and the same `--build-side-ratio` the join
    order uses). The two went together deliberately, against this ADR's own "one PR per id": C2b alone moves no plan,
    so on its own it is an operator nothing reaches and nothing can measure, and C3's rule alone has no operator to
    run. Q21's 36 GB and its 10.8x are therefore already collected under C2b above. **The anti and outer halves wait
    on C2c**, which is now the only thing between this ADR and Q22, Q13 and Q21's remaining 18.81 GB.
  - What did *not* move, and why it is worth knowing: Q4, whose correlated `EXISTS` has the same shape as Q21's.
    `EstimateRows` reads a `Filter` as an upper bound -- it does not apply the predicate -- so it sees the subquery
    side at 4.0x the outer side where the true ratio is far higher, and 4.0x is not *more* than the default ratio.
    Making `EstimateRows` filter-aware (it would reuse C1's `PredicateRows`) is the smallest remaining lever here and
    belongs with C5, which needs estimates through a join and an aggregate for the same reason.
- **C4, perf(exec): keep a large build parallel.** `OneAtATime` trades all of a build's parallelism for headroom at
  half the limit, with nothing in between. After C3 no query of the 22 reaches that threshold, so this is no longer
  urgent — but any query whose build is legitimately large still falls off the same cliff.
- **C3a, feat(plan): build sides for the joins the unnesting pass creates.** The part of C3 that needs no C2: ADR
  0023's Path 2 produces an *inner* join, so either side may build it, and it hardcoded the aggregate it puts on the
  right. It now chooses by the same rule the order uses (`ChooseBuildSide`, one function so the two cannot drift),
  which needs the join order's estimate kept on `JoinNode` and `EstimateRows` reading it — the domains rule 3 divides
  by are not reachable from a plan node, so nothing downstream could recompute one.
  - Update (2026-10-15, C3a): shipped, and **it buys nothing measurable.** It flips exactly the two joins it was
    meant to — Q2's, whose build side was 185x the stream probing it, and Q17's, 33x — and only those two plans move;
    every other plan at SF 100 is byte-identical. Q17 falls 9.3% and Q2 1.7%, both inside the ±15.8% band DuckDB's
    own unchanged binary set over the same pair of runs, the geomean does not move, and peak memory does not either
    (Q17 1.64 GB to 1.70 GB). The reason is that in both queries the oversized side is an **aggregate whose own group
    state dominates** — Q17's is 20M groups over the whole of the largest relation — so the join's hash table was
    never the bottleneck. ADR 0023 said as much about Q17 before this was tried: "the scan of the inner table
    dominates anyway". The lever for Q2 and Q17 is therefore ADR 0023's deferred **semi-join reduction**, computing
    only the groups the outer side needs, not the build side. Kept all the same: the estimate on the node, the shared
    rule and `EstimateRows` through a join are what C5 and C3 both need, and one pass no longer hardcodes a side the
    other chooses by rule.
- **C5, feat(plan): cost-based join order with filter selectivity.** Rules 1 and 4 with the estimates of C1, for the
  queries that carry a join's output through later joins that would have cut it: Q5, and Q7 only once its selectivity
  is visible (see below).
  - Update (2026-10-16, C5): **shipped.** The greedy rule gives way to dynamic programming over the subsets the join
    conditions connect (DPccp, Moerkotte and Neumann 2006), minimising the rows each join produces plus the rows it
    must hold to produce them. A block of more than 12 relations keeps the greedy rule, which `--join-order greedy`
    also selects for any block; it reproduces every one of the 22 plans byte for byte, so the rule it replaces stays
    reachable and comparable.
  - **Exec needed no change, against this ADR's expectation.** A bushy tree already binds, plans, profiles and
    answers: `PipelineInput` descends only probe sides, so each part pipeline keeps its single scan, and
    `PipelineBuildsOf`, `MakeJoinBuild` and `PartsOf` already recurse through a join on a build side. A LEFT JOIN unit
    and a derived table have been producing a join as another join's input all along. C5 is planner-only.
  - **Measured at SF 100, 4 threads, best of 2**, against the greedy rule on the same binary:

    | query | greedy | search | |
    | --- | ---: | ---: | --- |
    | Q10 | 22.69 s | **9.49 s** | **-58%** |
    | Q21 | 28.92 s | **22.29 s** | **-23%** |
    | Q05 | 16.26 s | **14.82 s** | **-8.9%** |
    | Q11 | 1.42 s | 1.90 s | **+33%** |

    The geomean over the 22 falls from 7.73 s to 7.37 s, 2.8x DuckDB to 2.7x, with everything else inside the +-8%
    band DuckDB's own unchanged binary showed over the same pair of runs. Peak memory falls where plans move: Q03
    4.79 GB to 1.33 GB, Q07 7.59 GB to 1.05 GB, Q10 5.40 GB to 3.67 GB. **Q10 was the worst query of the 22 at 6.1x
    and is now 2.8x** — which was not the target; Q05 was, and gained the least of the three.
  - **Two estimates, because the two consumers need opposite errors.** Everything that decides memory — a step's
    stored estimate and every `ChooseBuildSide` call — keeps C1's reading, the largest single key domain, which never
    over-states selectivity. Only the search's cost reads the narrower one, where each pair of relations divides once.
    A search only compares orders, so an optimistic estimate costs a worse plan; a build side reads it to decide what
    to materialise, where the same optimism costs the query. **The search may be optimistic; the build side may not.**
  - **The estimate rule took two tries, and only measurement caught the first.** Multiplying the domains of *every*
    key made Q09 fail outright at SF 100 (25.5 s and 11.65 GB became an out-of-memory error against a 41.23 GB
    limit): `lineitem` joins `partsupp` on `(l_partkey, l_suppkey) = (ps_partkey, ps_suppkey)`, a composite foreign
    key whose parts are perfectly correlated, so the product read 2,400 rows where there are about 600M, and three
    build sides flipped. Capping each pair's product at its larger row count left it 7.5x low, which still made the
    search choose that join first and cost 79%. The rule that works takes the **largest domain within a pair of
    relations** and multiplies **across** pairs: two conditions between the same two relations are the one composite
    key they describe, while conditions to different relations narrow in turn. Q09 is then neutral and Q05 keeps its
    gain. Nothing in a Parquet footer records a composite distinct count, so this is a choice of bound and not a
    measurement; the ADR's determinism property is unaffected, since it is still integer arithmetic over metadata.
  - **DPccp over relation masks, not DPhyp.** DPhyp generalises the same enumeration to hyperedges and to
    reorderability constraints, and antb1 has neither: a `JoinEdge` is a pair, every join the order builds is inner,
    and a LEFT JOIN unit is one opaque relation of its block. Over a simple graph DPhyp *is* DPccp, so its extra
    machinery would ship with no test able to reach it. Its producer would be opening those units up so that outer
    joins join the order — a separate semantic change — and adopting TPC-DS would not create one by itself. The
    search therefore reads edges as **pairs of relation masks** even though every edge has one relation on each side
    today, which makes its connectivity test already the one a hypergraph needs and leaves `Neighbours` and
    `Connected` as the only two places the graph model is read: the seam DPhyp would replace.
  - **A plain subset-and-split search stays in the tests as the reference.** DPccp's forbidden-set bookkeeping is
    intricate, and a connected pair it loses would only make the plan quietly worse — nothing else would notice. The
    reference agrees with it on cost and plan for every graph in the test set, which is what turned the Q09
    regression into a question about the estimate rather than about the enumeration.
  - **Two orientation rules that look cosmetic and are not.** A side whose rows nothing knows is made the left, so
    that the other is built: `ChooseBuildSide` keeps the right on an unknown estimate, and rule 1 used to keep such a
    relation as the probe. Otherwise the lower relation index is the left. Orienting by the estimates instead reads
    better and keeps a plan independent of the order a `FROM` list is written in, but it puts the larger side on the
    left always, which makes "unless the right is decisively larger" unreachable and silently stops
    `--build-side-ratio` doing anything. Which side is *built* does not depend on the orientation; only the labels do,
    so `plan.BinderTest` now pins the built relation rather than the branches.
- **C5a, perf(exec): lay out a sparse key directly too, done 2026-10-16.** The one regression C5 left: **Q11 was 33%
  slower** (1.42 s to 1.90 s), and the cause was exact. Its new plan reduces `supplier` to the 40,045 rows of one
  nation before the large join — the classic star-schema move, and it carries 3.2M rows where the greedy plan carries
  80M. But `s_suppkey` is dense over all of `supplier`, 1..1,000,000 over 1,000,000 rows, so a build on the whole
  table qualifies for the direct layout: an array indexed by the key, no hashing. The 40,045 surviving keys stay
  spread over the same range, so `max - min + 1` of about 1,000,000 against `8 x 40,045` failed `JoinTable`'s
  density test and the build fell back to hashing. Measured over the same 80M probes: `find` went from 52 ms to
  884 ms, about 17x per probe, and the two blocks' join time accounted for the whole regression to the millisecond.
  **Making a build 25x smaller made it 17x dearer to probe.**

  **The plan-side fix this ADR sketched cannot work, and the arithmetic says so without writing it.** A search that
  saw the layout would need the build's key range *after* its filters; a footer only has the whole column's. Both of
  Q11's plans contain a join whose build is a filtered relation, so the footer range misreads both and the penalty
  cancels: the greedy plan's second build is `nation` at **one row**, whose footer span of 25 against `8 x 1` reads
  as hashed where the actual span is 0 and `JoinTable` lays it out directly. Charging a hashed probe its rows then
  leaves the greedy plan at 164.2M against the new plan's 84.3M and **the search still picks the slower one**.
  Separating them needs to know whether filtering collapses a key's range — `nation`'s does, `supplier`'s does not —
  which no statistic in a Parquet footer records.

  So the fix belongs where both the actual range and the actual rows are already known: **the layout rule.** Not
  "make the search avoid the plan that loses the array" but "let the better plan keep it". `JoinTable::Build` allows
  32 slots a row rather than 8 — and the `8` rule is still unconditional, so this only ever *adds* the array. The
  density bound turns out to have been far too tight, and not for the reason it looks like: **the array wins even
  when it cannot be cache-resident.** Q05's build is 22.8M rows spanning 600M, a **2.4 GB** index read at random, and
  it is 17% faster than hashing the same keys — one dependent load beats a hash, a bucket load and a key compare
  whichever level of memory each lands in. Nothing above 32 slots a row moved any measurement, so the constant is a
  bound, not a tuning. Past 8 slots a row the index is memory the hashed layout would not spend, so the sparse half
  of the rule asks the budget first and takes at most an eighth of `--memory-limit`: a build that would fail its
  budget on an index is better hashed than failed. A query's limit therefore changes how a join is laid out and never
  its answer, as `--build-side-ratio` and `--join-order` do for its plan.

  Measured at SF 100 on 4 threads, best of 2, both columns from the same binary: **Q11 2.04 s to 1.56 s (-23%)**,
  **Q05 15.55 s to 12.90 s (-17%)**, **Q09 24.95 s to 22.34 s (-10%)**, Q21 23.23 s to 21.76 s (-6%), and the geomean
  over the 22 falls **7.58 s to 7.29 s (-3.8%)**. Nothing regressed: the worst query moved +0.2%, and Q06 — which has
  no join at all — moved -5.4%, which sets the noise band this was read against. The cost is peak memory where a
  sparse index is taken: Q05 4.55 to 6.27 GB, Q21 5.03 to 5.59 GB, Q09 12.10 to 12.38 GB, every other query flat.
  **Q11 is now faster than either plan C5 could choose**, so C5's regression is closed rather than traded away.

Re-profiling all 22 at SF 100 after C1 and C3a says the roadmap above no longer matches where the time is. Three of
the four worst queries need items this ADR does not own, and two of those are deferrals of ADR 0022's whose triggers
this profiling fires:

- **Q10, 6.6x, nothing in this ADR targets it, and it is not a plan problem.** Its cost is `gather`: 21.8s on one
  join materialising seven customer columns, four of them VARCHAR, for 11.46M rows. The first reading of this was
  that the `LIMIT 20` makes most of that materialisation pointless — it does not. The query groups by all seven
  columns, 3,884,218 groups out of 11,463,171 rows, so the values are needed to form the groups at all, and DuckDB's
  plan groups by all seven too (`Groups: #0` through `#6`): neither engine can decline to load them, and no
  functional-dependency reduction is available over Parquet, which declares no key.

  Measured instead, over the same join with only the GROUP BY width varied: with one narrow key antb1 takes 9.18s
  against DuckDB's 2.69s — the engine's general per-operator gap of about 3.4x. Adding the four VARCHAR group keys
  costs antb1 **+8.2s** and DuckDB **+0.56s**, a 14.6x difference in the marginal cost alone. Q10's excess over the
  baseline gap is therefore the price of carrying and hashing wide string keys — roughly 130 bytes of string per row
  over 11.46M rows, copied in `gather` and then hashed — not the plan. The lever is an execution one: gather a string
  column by reference into the build's own buffers rather than by copying its bytes, and compare group keys by hash
  before their bytes. That belongs to `exec`, not to this ADR, and it is a different thing from
  [ADR 0016](0016-late-materialization.md)'s late materialization of top-N columns.
- **Q7, 4.8x.** Its whole selectivity is locked in a join residual: both nation relations scan all 25 rows, there is
  no filter node at all, and four joins each carry 182,301,190 rows before the fifth cuts them to 583,512. No
  estimate can see that, so C5 cannot help until `n_name IN (...)` is *derived* from the disjunction — ADR 0022's
  deferred "filters implied by ORs". Self-contained, and `or_factoring.h` already exists for the related transform.
- **Q2 and Q17** want the semi-join reduction, as C3a's update above establishes by measurement.
- **Q5, 4.6x**, is the one that genuinely wants C5: its filters are visible and its order is still greedy left-deep
  from the largest relation.
- **C6, feat(io): DECIMAL columns in the filtered scan.** ADR 0021's decimal64 item, or — cheaper and strictly
  better — `supports_scan_filter` per column, so a scan pushes what it can and decodes the rest for surviving rows
  only. Low measured value; last of the engine work, and the thing runtime join filters wait for.
- **C7, ci: the 22 queries on demand.** A job in `bench.yml`, on `workflow_dispatch` with a scale-factor input and
  never on `push` or `pull_request`, generating its data at test time as the SLT runner does. `.github/` is a
  protected path, so the diff needs the maintainer.
- **T4, test(diff): generate the shapes the build directions answer.** A build direction is not a new feature of the
  SQL subset, so `kGeneratorPending` has nothing to hold; what the generator owes is the shapes where the preserved
  side is the smaller one, so that `diff-random` covers the plans C3 newly chooses.
- **C8, perf(exec): gather only the rows a 1:1 window matched, done 2026-10-16.** Re-profiling all 22 after C5 and
  C5a put the whole remaining join cost in one metric: **`gather` 235.3 s against `find`'s 78.9 s**, summed over
  parts, and `gather` was charged per *probe* row almost regardless of how many rows the join kept. A join keeping
  0.50% of its probe rows paid **27.8 ns** each where one keeping 100% paid **19.9 ns** — a 200x difference in match
  rate producing a 1.4x difference in cost, in the wrong direction.

  The cause was in `HashJoinOperator::NextWindow`, the inner join's 1:1 path. It left the probe where it was — a
  zero-copy slice of the batch, which is the design's point — and gathered the build's columns for **every row of the
  window**, NULL where a row had no match, then masked the misses off with a selection. So a window where one row in
  200 matched still materialised the build of all 200. Adding the matched count as a metric beside `window_rows`
  settled what the waste was: of the 235.3 s, **169.9 s went on probe rows with no match and 0.0 s on pairs a
  residual rejected** — so the fix was to compact, not to reorder the residual.

  The rule needs no threshold, and that was the measurement's doing rather than the design's. Sweeping the share at
  which compacting beats slicing — compact below 1 in 2, 1 in 4, 1 in 8 — found 1 in 4 and *any miss at all* within
  1% of each other, and the all-matched case untouched either way, because a window whose every row matched never
  compacts under either rule. So the condition is simply `matched < rows`, and with it the window path runs **only**
  when the window matched entirely: its selection, and the NULL-aware build gather it needed, are both gone. The
  faster path is also the one with less code in it.

  Measured at SF 100 on 4 threads, best of 2, both columns from the same binary: the geomean over the 22 falls
  **7.259 s to 5.695 s (-21.5%)**, and antb1 goes from 2.61x DuckDB to **2.04x**. Thirteen queries gain more than
  12%: Q02 -50.4%, Q07 -49.8%, Q05 -49.5%, Q11 -44.5%, Q09 -39.2%, Q14 -32.9%, Q03 -28.3%, Q16 -26.0%, Q19 -25.9%,
  Q21 -19.9%, Q10 -18.5%, Q08 -16.5%, Q17 -12.1%. Nothing regressed: the largest move the other way is Q06 at +2.8%,
  which has no join in it at all and so sets the noise band.

  **This is the item the roadmap above did not have, and three of its entries were waiting behind it.** C5's own
  re-profiling read Q10's cost as `gather` and handed it to `exec` as a string-width problem; a good part of it was
  this instead. The lesson for the entries that remain is that a metric covering two different things — here
  `Take`-on-probe and the build gather under one timer — hid a 2x for as long as it was not split.
- **C9, perf(exec): DECIMAL arithmetic without copying its operands, done 2026-10-16.** Once C8 had taken the join
  costs out, the slowest query of the 22 was **Q01 at 16.84 s and 3.25x, which contains no join at all**: 38.4 s of
  its 49.8 s (summed over parts) was `Compute`, evaluating two DECIMAL expressions over 591.6 M rows. Isolated
  against a floor of the same scan with no arithmetic, **one DECIMAL addition cost 2.68 ns a row — 2.4x what reading
  and summing the whole column costs** — and each constant in an expression added 0.50 ns a row on top.

  None of it was Arrow's: DECIMAL arithmetic takes antb1's own path (`DecimalArith`), and it was moving about
  **176 bytes a row for one addition of two columns** where 48 would do. `DecimalOperand` held a
  `std::vector<Int128>`, so every operand of every operation was value-initialised (a memset), filled from the Arrow
  buffer and read back — while a decimal128 array already *is* 16 contiguous bytes a row. On top of that
  `RescaleOperand` walked both operands even when the scales already agreed and its factor was therefore 1, and a
  constant was built into a full array of copies of itself (`MakeArrayFromScalar`) and then widened into a full
  vector of them.

  So: an operand is read in place from its own buffer, a constant is one value, and a rescale that would multiply by
  1 does not happen. **One addition now costs 1.27 ns a row**, and a constant operand went from costing 0.50 ns a
  row *more* than a column to 0.14 ns *less*, which is what it should always have been. Measured at SF 100 on
  4 threads, best of 3, over the same column so only the arithmetic varies: four additions 6.98 s to 3.64 s, the
  same four with four constants 8.16 s to 3.31 s, and Q01's own pair of expressions 10.76 s to 6.37 s. Over the 22:
  **Q01 16.84 s to 12.39 s (-26.4%)**, 3.25x to 2.41x, geomean 5.659 s to 5.589 s and **2.04x DuckDB to 2.02x**;
  everything else inside the noise band.

  Skipping the rescale is where the one defect of this change was, and it was caught in review: the type rules do
  make a result at least as wide as an operand of its own scale, so the walk could not fail for a value within its
  column's declared width -- but **nothing checks that width when a file is read**, which the code it replaced said
  in as many words. A column holding more than it declares, added to its own negation, would then have answered 0
  where DuckDB raises a conversion error: a wrong answer, not a lost error. The check is back, in the arithmetic's
  own loop where the value is already in a register rather than in a pass of its own, and only for an operand no
  rescale has already vouched for (a constant and a widened integer are within their types by construction). It
  costs about 1% of Q01, and `DecimalArithmeticErrors` now pins it with the sum-in-range case that distinguishes a
  wrong answer from a lost one.

  Two things are deliberately left, both measurable from Q01's remaining `Compute` (20.8 s summed, still 42% of the
  query): its two expressions **share a subexpression** that is evaluated twice, because the evaluator is a tree walk
  with no memoisation (worth about 1.7 s of the 6.25 s probe above); and each operation still **materialises its
  result**, where a fused walk would keep intermediates in registers. The second subsumes the first and is the larger
  change. At 48 bytes a row for one operation the current form is near what a materialising evaluator can do.

## Alternatives considered

- **Keep the footer-only estimates and swap the side by a rule of thumb** (build on a scan, probe a join; build on
  the second relation of the FROM list). Cheap, and it would fix Q12. Rejected: it is not a rule anyone can predict
  from the query, it moves with the text rather than the data, and it would not touch Q21, where the shape and not
  the footer picks the side.
- **A full cost model with sampling or histograms.** Rejected for now: it breaks the property that a plan depends on
  metadata alone, which is what makes EXPLAIN reproducible and the tests hermetic, and ten of the fourteen wrong
  sides are wrong by a factor of 10 or more — far more margin than an estimate from exact footer minimums and
  maximums needs in order to separate them.
- **Runtime join filters first.** They would cut the probe side of Q8 and Q19 without any estimate at all, and need
  no cardinality model. Rejected as the first step: ADR 0022 orders them after DECIMAL reaches the filtered scan,
  nothing in the engine passes a filter sideways today, and they do not address memory, which is what Q21 is about.
- **Spilling instead of choosing the side.** It would stop Q21 from failing on a smaller host, but it would not make
  it faster, and it would make the 36 GB build legitimate rather than unnecessary. Deferred to its own SF 300
  trigger, as ADR 0022 has it.

This workload is derived from the TPC-H Benchmark and is not comparable to published TPC-H Benchmark results, as
this implementation does not comply with all requirements of the TPC-H Benchmark.
