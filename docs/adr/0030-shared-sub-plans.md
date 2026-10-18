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
  subquery repeats the `FROM` list verbatim, so the binder produces two identical joins of `partsupp`, `supplier`
  and `nation` and two scans of 80,000,000 rows. DuckDB reads `partsupp` once (79,997,396 rows), keeps the
  3,203,600-row join result and scans it twice, in **0.255 s**.
- **Q2: the same shape a third time.** A correlated `MIN(ps_supplycost)` subquery unnests into a second join over
  the same `partsupp` columns, so that 80,000,000-row scan also happens twice, inside 750 ms.
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
- **Identity is structural equality of the logical sub-tree**, including the columns it outputs. Two references to a
  CTE and two spellings of the same subquery are then caught by the same rule, which is what the measurement asks
  for. A hash of the sub-tree makes finding candidates linear; equality confirms each candidate.
- **One materialisation, read by every consumer**, on the `JoinBuild` pattern: computed before its consumers run,
  held while any of them needs it, released with the query.
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

- **E1, feat(plan): find identical sub-plans and say so in `EXPLAIN`**, without materialising anything. Cheap, and it
  answers the question the rest depends on: how often does this fire, and on what? It is also the honest way to
  learn whether structural equality catches Q2's unnested subquery, which is the case this ADR is least sure of.
- **E2, perf(exec): materialise a shared sub-plan once.** Measured on Q15, Q11 and Q2 at SF 100, with peak memory
  reported beside time, and the other 19 as the control.
- **E3, docs(adr): record what E1 and E2 measured**, and either accept this ADR or defer it with the numbers.

Deliberately out of scope: `WITH RECURSIVE` and `MATERIALIZED`, both of which the binder refuses today (ADR 0022);
sharing across statements; and making the optimizer's passes DAG-aware, which the first decision above exists to
avoid.

## Alternatives considered

- **Materialise CTEs only**, which is what C8 proposed. Rejected on the measurement: it would fix Q15 and miss Q11
  and Q2, which duplicate as much work with no CTE in them. It is also the narrower rule for the same machinery --
  once a shared sink exists, restricting it to one syntax buys nothing.
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
