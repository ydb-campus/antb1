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
- **C2b, feat(exec): build-side output for semi joins.** The atomic mark bitmap, the `rows()` position carried
  through the residual path, and the tail — whose location this ADR leaves open above, and which C2b must decide
  first. Semi alone, because it needs no change to the build. Unlocks Q21's semi join, which today builds the whole
  of the largest relation to probe it with 7.3M rows, and Q4's.
- **C2c, feat(exec): build-side output for anti and left outer joins.** Right anti and right outer, which
  additionally need the build to keep its NULL-key rows, since those are unmatched by definition and must be
  emitted. Unlocks Q21's anti join, Q22 and Q13.
- **C3, feat(plan): choose the build side for semi, anti and outer joins.** C1's estimates with C2b's and C2c's
  operators, one kind at a time as each lands. This is where Q21's 36 GB, its 10.8x and its 1.08x thread scaling go,
  and with them Q4, Q22 and Q13. The largest single win of this ADR.
- **C4, perf(exec): keep a large build parallel.** `OneAtATime` trades all of a build's parallelism for headroom at
  half the limit, with nothing in between. After C3 no query of the 22 reaches that threshold, so this is no longer
  urgent — but any query whose build is legitimately large still falls off the same cliff.
- **C5, feat(plan): cost-based join order with filter selectivity.** Rules 1 and 4 with the estimates of C1, for the
  queries that carry a join's output through later joins that would have cut it: Q7 first, then Q8 and Q5.
- **C6, feat(io): DECIMAL columns in the filtered scan.** ADR 0021's decimal64 item, or — cheaper and strictly
  better — `supports_scan_filter` per column, so a scan pushes what it can and decodes the rest for surviving rows
  only. Low measured value; last of the engine work, and the thing runtime join filters wait for.
- **C7, ci: the 22 queries on demand.** A job in `bench.yml`, on `workflow_dispatch` with a scale-factor input and
  never on `push` or `pull_request`, generating its data at test time as the SLT runner does. `.github/` is a
  protected path, so the diff needs the maintainer.
- **T4, test(diff): generate the shapes the build directions answer.** A build direction is not a new feature of the
  SQL subset, so `kGeneratorPending` has nothing to hold; what the generator owes is the shapes where the preserved
  side is the smaller one, so that `diff-random` covers the plans C3 newly chooses.

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
