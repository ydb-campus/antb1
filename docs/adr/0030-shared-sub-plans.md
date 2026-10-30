# 30. Shared sub-plans: computing an identical sub-plan once

Date: 2026-10-18

## Status

Accepted (E1 and E2 built and measured; E3's entry carries the numbers it was accepted on. A sub-plan the plan
computes more than once is computed once where it **reduces** and where it reads enough to be worth not reading
twice; the join-topped and filter-topped cases were measured and **refused**, not deferred.)

## Context

- **This is [ADR 0022](0022-joins-and-query-blocks.md)'s decision C8, and its trigger has fired.** That ADR deferred
  "**Materialized CTEs and shared sub-plans:** a CTE computed once into a shared sink", with the trigger stated as
  "materialized CTEs, until a profile shows that the recomputation dominates". It does.
- **Q15, at SF 100 on 14 threads: 2.101 s, of which the duplicated branch is 1.026 s — 49% of the query.** The
  query writes its `revenue` CTE once and references it twice, and the plan holds the identical
  `Scan lineitem -> Filter -> Compute -> GroupAggregate -> Project` **twice**, so the query reads 1.2 billion rows
  to answer a question over 600 million. DuckDB answers it in **0.829 s**, with one materialised CTE and two
  `CTE_SCAN`s.
- **Q11: 365 ms, of which the duplicated branch is 136 ms — 37% — and there is no CTE in it at all.** Its `HAVING`
  subquery repeats the `FROM` list, so the binder produces two joins of `partsupp`, `supplier` and `nation` and two
  scans of 80,000,000 rows. DuckDB reads `partsupp` once (79,997,396 rows), keeps the 3,203,600-row join result and
  scans it twice, in **0.255 s**.
- **Q2: the same shape a third time.** A correlated `MIN(ps_supplycost)` subquery unnests into a second join over
  the same `partsupp` columns, so that 80,000,000-row scan also happens twice, inside 750 ms.
- **Correction (2026-10-19): "identical" above was not checked, and for two of the three it is false.** Reading the
  three plans puts the duplicated subtrees side by side, and only one pair is equal:

  | query | the duplicated sub-plan | equal? | the difference |
  | --- | --- | --- | --- |
  | Q15 | lineitem scan, filter, compute, grouped SUM, project; result of the order of 1,000,000 rows | **yes** | none: both branches match node for node, columns included |
  | Q11 | `partsupp` joined to `supplier` and `nation`; result of the order of 3,200,000 rows | no | the `partsupp` scans read **4 columns against 3** -- the globally aggregating branch needs no `ps_partkey` |
  | Q2 | `supplier` joined to `nation` and `region` | no | `supplier` read with **7 columns against 2**, `nation` with **3 against 2** |

  In every case the smaller column set is a **strict subset** of the larger, so column subsumption is the whole of
  what separates them. **Exact structural equality catches one of this ADR's three motivating queries.** That is
  the question E1 was written to answer, answered by reading plans instead of building it.
- **Correction (2026-10-19, from E1 as built): there is a fourth repeated sub-plan, and this ADR dismissed it.**
  The Context below says Q21 "repeats a scan too, but ... of `lineitem` for genuinely different work". Half of
  that is wrong. Q21 holds **three** references to `lineitem`, and two of them -- the outer one and the one under
  the anti join -- carry the *same* filter over the *same* four fields, so they compute the same rows and E1
  groups them. Only the third, the semi join's unfiltered probe, is different work.
  - **Second correction (2026-10-19, from E2): Q21 is nonetheless not a candidate, and the reason is a
    cross-ADR interaction worth more than the finding above.** The entry first called it "the largest of the four
    at SF 100". It is the largest and it must be refused. Those two members compute **379,356,474** rows, measured
    directly, and the profile shows them *emitting* 24 M and 15 M only because
    [ADR 0029](0029-dynamic-join-filters.md)'s dynamic join filters prune each probe scan **differently**. One
    shared result cannot carry two consumers' different Bloom filters, so sharing would replace two cheaply
    filtered scans with a 12 GB materialisation and two full re-scans: **strictly worse**. A pruning mechanism
    downstream of a sharing mechanism can turn a real duplication into one it is a mistake to remove, and nothing
    in either ADR would have predicted that.
- **E1 also reports five queries that are a scan and nothing more** (Q7, Q8, Q17, Q18, Q22: repeated scans of
  `nation`, `lineitem` or `customer` with no filter between them). Those are true findings and not false ones --
  the scans do compute the same rows -- and every one is refused by the cost gate for the reason the Alternatives
  give: a sub-plan is worth holding when it *reduces*, and a scan does not. They are what tells E1's report apart
  from E2's decision, which is why the report is a separate item.
- **And Q15's match is incidental, which is the stronger argument.** Its second reference is a scalar subquery
  taking the maximum of the CTE's revenue column, so it reads that column and **not** the supplier column the
  CTE also outputs -- yet both branches carry a project of both, so the unused one survives into the branch that
  aggregates. The two
  subtrees are equal *because nothing prunes a column the SQL plainly does not reference*. A column-pruning pass
  that noticed it would turn Q15 into Q11's case and stop exact-equality sharing from firing at all. **A
  performance feature whose trigger depends on another pass not being improved is the wrong coupling**, and no
  amount of measuring exact equality would have revealed it.
- **A plain `WITH` therefore gives no guarantee under exact equality.** Q15 is the only one of the 22 that uses one,
  and two references to a CTE share only when they happen to need the same columns -- which is not a property a
  user can predict or a plan can explain. With subsumption the answer is unconditional, which is what makes the
  rule explainable.
- **So C8's "one query of the workload has a CTE" is the premise to correct.** Three of the 22 duplicate substantial
  work and **only one of them is written with a CTE**. The duplication is of *sub-plans*, whatever syntax produced
  them — a CTE reference, a `HAVING` subquery, an unnested correlated subquery — so a rule that materialises CTEs
  alone would miss two of the three. (Q7 and Q21 repeat a scan too, but of `nation`'s 25 rows and of `lineitem` for
  genuinely different work, so neither is this.)
- **Why it happens.** ADR 0022 binds "the relations of a CTE's body once per reference to the CTE", so every
  reference becomes a subtree of its own, and nothing after the binder notices that two subtrees are the same.
- **The hard part C8 named, and it is still true**: "every optimizer pass would have to become aware of shared nodes
  (today a rebuild un-shares a DAG and prunes each path its own way)". `plan::LogicalNodePtr` is a
  `shared_ptr<const LogicalNode>`, so a DAG is representable; what is not safe today is a pass walking one.
- **The machinery already exists, under another name.** A join build *is* a sub-plan computed once and read by
  something else: `JoinBuild` holds one `JoinTable`, `PipelineBuildsOf` hands it to the probe pipeline, and
  `PrepareBuilds` completes it before any probe part runs (`src/exec/hash_join.cc:374`). A shared sub-plan is that
  shape with no join on top, which is why this is less new than C8 assumed.

## Decision

Taken, and narrower than drafted: E3's entry in the Plan records which of these held, which the measurement
changed, and which were refused outright. The bullets below are the shape as designed; the corrections under them
are what was built.

- **Share after the passes, not before.** Identical sub-plans are found once the optimizer's passes are done, and
  the sharing is a physical-planning decision. That answers C8's objection without paying it: every pass keeps
  seeing a tree, no pass learns about DAGs, and a rebuild cannot un-share what is not yet shared.
- **Identity is structural equality of the logical sub-tree up to the columns it reads** -- two sub-plans match when
  they differ only in that one reads a subset of the other's columns, and the shared one then reads the **union**.
  Each consumer projects its own subset from it. Two references to a CTE and two spellings of the same subquery are
  caught by the same rule, which is what the measurement asks for. A hash of the sub-tree makes finding candidates
  linear; the equality check confirms each candidate.
  - **Exact equality, without subsumption, was the first draft of this rule and the Context refutes it**: it catches
    one of the three queries, and that one only by an accident of column pruning. Subsumption is therefore not an
    extension to be deferred to a later item -- it is what makes the rule hit its own targets at all.
  - **Column references are compared by name, not by position.** `BoundColumn::index` is a position in the input,
    so a branch whose scan reads one column fewer has different indices in *every* node above it -- the two Q11
    joins are unequal by index and equal by name (`partsupp.ps_suppkey = supplier.s_suppkey` on both sides). The
    canonical form of a sub-tree therefore expresses each column reference as its qualifier and name, and a scan as
    its table and the set of fields it reads. Positions are what the sharing has to *rewrite*; names are what it
    matches on.
    - Update (2026-10-30, a wrong answer): as built, a reference was its column's origin (`table#field` for a
      scanned one), which cannot tell the two sides of a self-join apart, nor the COUNT(*)s of two aggregations.
      Two sub-plans grouping one self-join by `a.k` and by `b.k` were one group, and E2 answered the second from
      the first's result (`src/exec/tests/shared_builds_test.cc` measured SUM 72 where the answer is 48). Each
      reference now also carries its **path** -- the inputs from the node that reads it down to the node that
      defines it -- in node keys, in derived origins and when members' outputs are matched. Equivalent sub-plans
      share their shape and so their paths, which keeps every sharing this ADR measured.
- **The consumers' column indices are rewritten, and that is the real cost of subsumption.** A consumer reading the
  shared result sees the union's width and ordering, not its own, so every column reference above the shared node
  shifts. This is the one rewrite this ADR needs, it is mechanical, and it is confined to the consumers of a shared
  node -- but it is why subsumption is more than a loosened comparison.
- **One materialisation, read by every consumer**, on the `JoinBuild` pattern: computed before its consumers run,
  held while any of them needs it, released with the query. Three details that pattern does not settle:
  - **The result is an in-memory `plan::Table`, not a `JoinTable`.** A `JoinTable` is partitioned and hashed for
    keyed lookup; a consumer here wants a sequential re-scan. Materialising into a table implemented in `exec`
    makes every consumer an ordinary part pipeline over it -- `num_parts()` is the chunk count -- so the existing
    part scheduling, profiling and even `part_stats` work untouched, and `exec` already scans through
    `plan::Table` by design (`cmake/Antb1Modules.cmake`). The exec tests' own in-memory table is the precedent.
  - **Prepared where the builds are**, before any consumer part is scheduled, as `PrepareBuilds` does. For Q15 this
    is forced rather than chosen: its second reference feeds a `WHERE` predicate, so the aggregate over the CTE
    must finish before the outer filter runs. The two consumers are strictly ordered, so no amount of teeing would
    avoid holding the result.
  - **Released when the last consumer is done, which is new.** `JoinBuild` is released through
    `PartSink::set_parts_done`, which assumes one consumer. Two need a reference count. It is small, and it is the
    one piece here with a correctness edge: release early and a consumer reads freed rows.
- **Only where it pays**, and both halves of that are measurable before anything runs: the sub-plan has to be dear
  enough to be worth materialising (its own estimate), and its *result* small enough to hold. A sub-plan whose
  result is the size of its input trades a scan for a materialisation and wins nothing.
- **Determinism is untouched**: both consumers read the same rows in the same order, and a shared result is computed
  by the same operators that would have computed it twice. An answer stays byte-identical for any thread count.
- **Memory is the cost C8 did not weigh.** Recomputing costs time; sharing costs retention. Q15's shared result is
  about a million rows against a 600-million-row input, which is the shape where this pays; the reverse shape is a
  regression and the gate above is what keeps it out. Memory is also what broke this workload before
  ([ADR 0026](0026-join-performance.md)), so a shared sub-plan must be inside `MemoryBudget`, not beside it.

## Consequences

- `EXPLAIN` has to show that a sub-plan is shared and which consumers read it, or the optimisation is invisible and
  a plan with one shared node reads like a plan with two independent ones.
- The plan stops being a tree in the physical planner, where today every structure is a tree. That boundary is the
  thing to keep narrow: one place that knows about sharing rather than a property of every node.
- A query whose sub-plan is shared holds its result for longer than either consumer alone would, which is visible in
  peak memory. The measurement has to report peak memory beside time, since trading 1.0 s for a gigabyte is not
  obviously a win at SF 100.
- Nothing here helps ClickBench: none of its 43 queries has a CTE, a subquery or a join.

## Plan

One PR per id.

- **E1, feat(plan): find sub-plans equal up to the columns they read, and say so in `EXPLAIN`**, without
  materialising or rewriting anything. The question it was first written to answer -- how often does this fire, and
  does it catch Q2 -- the Context now answers by reading plans, so E1's job is narrower and better defined: the
  canonical form, the candidate hash, the subsumption check, and reporting what matched with the column union it
  would read. It stays worth its own item because every later item depends on that comparison being right, and a
  pass that only reports cannot give a wrong answer.
- **E2, perf(exec): materialise a shared sub-plan once.** Measured on Q15, Q11 and Q2 at SF 100, with peak memory
  reported beside time, and the other 19 as the control.
  - Update (2026-10-19, E2): **shipped, 0.980x of the 22, and all of it is one query.** Q15 goes 2.250 s to
    **1.208 s (0.537x)** and the other 21 move 0.9967 together, which is this harness's noise. The gate ended up
    admitting that one query and nothing else, and every narrowing was forced by a measurement rather than chosen.
    - **The gate needs both halves this ADR specified, and the second is the one that bit.** "Small enough to
      hold" was easy; "dear enough to be worth materialising" is what separates the cases. Q2's shared join reads
      about a million rows and came out at **1.017x** -- slower than recomputing it, because materialising
      200,535 rows and scanning them twice costs more than redoing cheap work. So a sub-plan must also *read*
      enough to be worth not reading twice (`kMinSharedRows`).
    - **Join-topped sub-plans are refused outright, on their numbers.** Q11 shares 3,203,600 rows for
      **0.937x while holding about 269 MB** -- 30 ms bought with a quarter of a gigabyte -- and Q2 is the 1.017x
      above. Carrying a path with those numbers would be worse than recording them, so only an aggregate (which
      reduces by definition) qualifies. That also refuses Q21 by **shape** rather than by an estimate that might
      be wrong.
    - **Sharing needs no DAG, against this ADR's expectation.** `ScanNode::table` is already a
      `shared_ptr<Table>`, so each member becomes *its own* ScanNode over *one shared* Table object, carrying its
      own column ids. Nothing above a consumer is rewritten, no pass sees a DAG, and `ResolvePositions` is not
      re-run -- so the Decision's "the consumers' column indices are rewritten, and that is the real cost of
      subsumption" is **not** a cost this pays, and the Consequences' "the plan stops being a tree in the physical
      planner" does not happen either.
    - **No reference count either.** One owner above the whole plan fills every shared result before it opens the
      root and frees them with the query, so "released when the last consumer is done, which is new" was not
      needed. The cost is peak memory held to the end of the query, which is the honest trade for not tracking
      consumers.
    - **The shared result is one part, and that is forced.** The physical planner reads `num_parts()` while it
      *builds* operators, which is before anything has run -- so a chunk-per-part table would hand its consumers
      zero parts and they would read nothing. One part read at Open is the shape that works, and it costs the
      consumers part-parallelism over a result the gate has already established is small.
    - **A group whose members' outputs do not nest is not reported at all**, found while building E2: one member's
      result must contain every other's for one table to serve them. Q8's two `nation` scans read
      `[n_nationkey, n_regionkey]` and `[n_nationkey, n_name]` -- overlapping, neither inside the other -- so E1
      now reports eight queries rather than nine. Serving that shape needs a union schema and a rewrite of the
      members, which is the cost this design exists to avoid.
- **E3, docs(adr): record what E1 and E2 measured**, and either accept this ADR or defer it with the numbers.
  - Update (2026-10-19, E3): **accepted, on 0.980x of the 22 and one second bought with 56 MB.**
  - **The measurement this ADR made its own acceptance turn on.** The Consequences say "the measurement has to
    report peak memory beside time, since trading 1.0 s for a gigabyte is not obviously a win at SF 100". On the
    one query that shares, at SF 100 and 14 threads:

    | | time | peak memory |
    | --- | ---: | ---: |
    | recomputed | 2.125 s | 244 MB |
    | **shared** | **1.11 s** | **300 MB** |

    So **1.02 seconds for 56 MB** -- against the gigabyte this ADR named as the price at which the trade stops
    being obvious, about eighteen times the wrong side of it. That is the number the acceptance rests on, and it
    was nearly not taken: E2 shipped having measured the shared result's *rows* and not the query's peak.
  - **What the whole ADR is worth**: 0.980x of the 22, Q15 at 0.521x, and the other 21 together at 0.9967, which
    is this harness's noise. E1 contributes no time at all -- it reports -- so the figure is E2's and the ADR's
    alike.
  - **Accepted as built, which is narrower than it was drafted**, and every narrowing is a measurement:
    - **only an aggregate-topped sub-plan is shared**, because a join-topped one was 0.937x while holding 269 MB
      and another was 1.017x, slower than recomputing it;
    - **and only where it reads enough to be worth not reading twice**, because materialising something cheap and
      scanning it twice costs more than redoing it;
    - **Q21 is refused though it is the largest duplication in the workload**, because ADR 0029's dynamic filters
      prune its two consumers differently and one shared result cannot carry both.
  - **What is out is out on evidence, not pending.** Filter-topped sub-plans, scan sharing, filter subsumption and
    a union schema for members whose outputs do not nest are all recorded above with the reason. None is a
    half-finished item; each would need its own measurement to reopen.
  - **Three lessons worth more than the 2%**, and all three are about plans rather than about this feature:
    - **A cost gate needs to ask two questions, not one.** "Is the result small enough to hold" is the easy half;
      "is the work worth avoiding" is the half that decides, and this ADR specified both while the first
      implementation had only one.
    - **A mechanism's prize can belong to another mechanism.** Q21 duplicates real work that it is a *mistake* to
      remove, because a pruning mechanism downstream already handles it two different ways. Nothing in either ADR
      predicted an interaction of that shape, and the next plan that layers two mechanisms over the same rows
      should look for one.
    - **The expected cost did not materialise.** The DAG, the rewritten column indices and the reference-counted
      release were all argued for at length and none was needed: sharing a `shared_ptr<Table>` between two scans
      is the whole mechanism. [ADR 0022](0022-joins-and-query-blocks.md)'s C8 deferred this work on the DAG
      objection, and the objection was answerable by not building a DAG.

Deliberately out of scope: `WITH RECURSIVE` and `MATERIALIZED`, both of which the binder refuses today (ADR 0022);
sharing across statements; and making the optimizer's passes DAG-aware, which the first decision above exists to
avoid. Also out of scope, and each with its reason:

- **Sharing on filters as well as columns**, whether by subsumption (share the weaker predicate, each consumer
  applies its residual) or disjunction (share `A or B`). Nothing in the 22 asks for it, and both can materialise
  rows only one consumer wants -- disjoint predicates make the disjunctive form a pure loss. Column subsumption
  cannot: it widens the result, never lengthens it, so its cost is extra columns times shared rows, computable
  before it runs.
- **Scan sharing -- one decode feeding several consumers, with no materialisation.** A different mechanism, and
  this ADR has **no case for it in the workload**. Subsumption matches several repeated scans and the cost gate
  refuses them all, correctly; what was missing was the reason it would be wrong to tee them either.
  - **Correction (2026-10-19).** This bullet first named Q17 as "the one Q17 needs", on the grounds that it reads
    `lineitem` twice. It does, and sharing that decode would be **strictly worse**, for the reason recorded two
    bullets above for Q21 and not applied here: its two scans emit **600,037,902 and 660,572** rows, because the
    second is the probe of a join whose build holds 20,031 parts and
    [ADR 0029](0029-dynamic-join-filters.md)'s filter drops **599,377,330** of its rows -- 99.9%. One decode
    feeding both would hand the probe every row the filter exists to skip.
  - **The rule that generalises it, which is this ADR's most portable finding.** Three of the repeated sub-plans
    the workload holds -- Q21's, Q17's and Q8's -- looked shareable and are not, two of them because a consumer
    **already carries a dynamic filter** that prunes it. So: **before costing a shared sub-plan, check whether a
    consumer is already pruned by one.** A duplication that ADR 0029 has made cheap is a duplication it is a
    mistake to remove, and the cost gate cannot see that -- it reads the logical plan, where both consumers look
    alike.
  - **And Q17's real cost is not duplication at all**, which is why naming it here was a mistake twice over. Its
    grouped aggregate computes an average for **every one of 20,000,000 parts** when the answer uses the 20,031
    that pass a brand and container filter: 4.40 s of a 6.22 s query, 319,678,579 local group insertions, 1.87 GB
    peak. What that wants is the qualifying keys pushed *below the aggregate* -- a dynamic filter through a
    grouped aggregate, which is ADR 0029's family and not this one's.
- **A bare shared scan**, which subsumption matches and the cost gate refuses for the same reason: Q2 has two
  identical 80,000,000-row `partsupp` scans, and materialising one to avoid the other trades a scan for about two
  gigabytes. A sub-plan is worth holding when it *reduces*; a scan does not.

## Alternatives considered

- **Materialise CTEs only**, which is what C8 proposed. Rejected on the measurement: it would fix Q15 and miss Q11
  and Q2, which duplicate as much work with no CTE in them. It is also the narrower rule for the same machinery --
  once a shared sink exists, restricting it to one syntax buys nothing.
- **Exact structural equality, columns included** -- this ADR's own first draft of the identity rule. Rejected by
  the Context: it catches Q15 alone of the three, and only because a column the SQL does not reference survives
  into one branch, so a better pruning pass would retire the optimisation. Subsumption costs one mechanical
  rewrite of the consumers' column indices and makes the rule both complete over the measured cases and
  explainable to a user.
- **Make every optimizer pass DAG-aware.** The general answer, and C8's stated objection to the whole idea. Rejected
  as unnecessary: sharing after the passes gets the same result, and a pass that must handle a shared node is a
  permanent tax on every future pass.
- **Rewrite the SQL instead** (hoist the repeated sub-plan into a CTE by hand). Not available: the queries are the
  workload's, and two of the three already have the duplication without a CTE.
- **Leave it, and take the recomputation.** The status quo, and what C8 chose until the trigger. Refuted by 49% of
  one query and 37% of another.
- **Cache a sub-plan's result across queries.** A different feature with a different lifetime and its own
  invalidation problem; nothing in the workload asks for it.

This workload is derived from the TPC-H Benchmark and is not comparable to published TPC-H Benchmark results, as
this implementation does not comply with all requirements of the TPC-H Benchmark.
