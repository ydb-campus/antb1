# 30. Shared sub-plans: computing an identical sub-plan once

Date: 2026-10-18

## Status

Proposed (draft: the measurement below is taken, the design is not built)

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
- **Correction (2026-10-19, from E1 as built): there is a fourth, and this ADR dismissed it.** The Context below
  says Q21 "repeats a scan too, but ... of `lineitem` for genuinely different work". Half of that is wrong. Q21
  holds **three** references to `lineitem`, and two of them -- the outer one and the one under the anti join --
  carry the *same* filter over the *same* four fields, so they compute the same rows and E1 groups them. Only the
  third, the semi join's unfiltered probe, is different work. So the queries with a reducing sub-plan worth
  materialising are **Q15, Q11, Q2 and Q21**, and Q21 is the largest of the four at SF 100.
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

Draft. What follows is the shape to build and measure, not a decision taken.

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
- **E3, docs(adr): record what E1 and E2 measured**, and either accept this ADR or defer it with the numbers.

Deliberately out of scope: `WITH RECURSIVE` and `MATERIALIZED`, both of which the binder refuses today (ADR 0022);
sharing across statements; and making the optimizer's passes DAG-aware, which the first decision above exists to
avoid. Also out of scope, and each with its reason:

- **Sharing on filters as well as columns**, whether by subsumption (share the weaker predicate, each consumer
  applies its residual) or disjunction (share `A or B`). Nothing in the 22 asks for it, and both can materialise
  rows only one consumer wants -- disjoint predicates make the disjunctive form a pure loss. Column subsumption
  cannot: it widens the result, never lengthens it, so its cost is extra columns times shared rows, computable
  before it runs.
- **Scan sharing -- one decode feeding several consumers, with no materialisation.** A different mechanism, and the
  one Q17 needs: it reads `lineitem` twice, 600,037,902 rows with column sets of 3 and 2, and is **3.7x of
  DuckDB** at SF 100. Subsumption would match those two scans, and this ADR's cost gate then correctly refuses
  them -- holding 600 M rows of 3 columns to save a scan is the trade the gate exists to reject. So Q17's
  duplication is real, is not this ADR's to fix, and wants an item that tees a running scan rather than one that
  holds its output.
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
