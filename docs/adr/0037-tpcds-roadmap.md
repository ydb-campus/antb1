# 37. A roadmap to the 99 queries derived from TPC-DS

Date: 2026-10-31

## Status

Proposed (2026-10-31). A design ADR the maintainer asked for ahead of the code (the ADR README's exception). It records
the target, the order of the work and the ids of the Plan. Each feature's own design is decided in the PR that builds
it, in the ADR that deferred it or in a new one, and this ADR is accepted when the Plan is done (CL2).

## Context

- **The target.** [ADR 0006](0006-test-strategy-and-data-policy.md)'s amendment of 2026-10-30 made the 99 queries
  derived from TPC-DS the third workload, with a ratchet like TPC-H's (`tests/data/tpcds_status.json`). All 22 queries
  derived from TPC-H pass; 50 of the 99 do.
- **A survey of the other 49 (2026-10-31).** Their texts were read from the pinned package and are quoted nowhere
  (ADR 0006). It lists every construct each query uses that antb1 does not answer, not only the first one antb1
  refuses. It found the table of [sql-subset.md](../sql-subset.md#queries-derived-from-tpc-ds) wrong or incomplete
  in many rows, among them:
  - only Q70 and Q86 call GROUPING(), and seven queries the table credited with it need ROLLUP alone;
  - Q87's first refusal is its parenthesized set-operation branches, and only Q87 parenthesizes one, not the four
    queries [ADR 0036](0036-union-all.md)'s context names;
  - Q6, Q21, Q44, Q54 and Q77 need constructs the table did not list: a correlated aggregate inside arithmetic, the
    folding of a quotient of literals, scalar subqueries inside a condition and not over an aggregate, and a cross
    product of multi-row relations.
- **What the 49 need, by area** (queries per construct; a query may need several):
  - NULL as a value: NULL literals as CASE values (2) and as set-operation items (2), IS [NOT] NULL (7), COALESCE (8).
  - Casts and constants: identity casts (2), numbers to DECIMAL(p,s) (6), DOUBLE to an integer type (1), a folded
    quotient of literals (1), DATE plus an integer (1).
  - Scalar functions: round (3), abs (5), upper (1), lower (1), concat (3).
  - Aggregation: stddev_samp (2), SELECT DISTINCT (5), ROLLUP (9), GROUPING() (2).
  - Set operations: UNION (3), INTERSECT (3), EXCEPT (1), parenthesized branches (1).
  - Join kinds: FULL OUTER JOIN (2), a cross product of multi-row relations (1).
  - Subqueries beyond [ADR 0022](0022-joins-and-query-blocks.md)'s and [ADR 0023](0023-unnesting.md)'s top-level
    conjuncts: one-row subqueries as values in select lists and conditions (4), subqueries that are not aggregates
    (4), correlated aggregates inside arithmetic (1), a correlated COUNT (1), IN and EXISTS under OR (3).
  - Window functions (15): aggregates over whole partitions (8), rank() (8), one running frame (1), all but four over
    a grouped block's aggregates.
- **Scale factor 1 is the budget.** Each of its eight shards has a 900-second ctest timeout (870 seconds inside), and
  each query a 2 GiB memory limit. The shard that holds Q4 took 79% of its timeout under AddressSanitizer, which is
  why that run moved to a nightly job (ADR 0006's update, ADR 0036's consequences). Each newly answered heavy query adds
  to its shard: Q9, Q14, Q22, Q39, Q72, Q75 and Q78 are the likeliest to press on its time, and Q78, Q67, Q72, Q39,
  Q38, Q87 and Q14 on the 2 GiB limit.
- **The harness keeps window functions as its permanent pending canary** (`kWindowFunctions`,
  `tests/slt/supported_features.h`): a feature no PR may answer, so the pending checks always have one to check.

## Decision

1. **All 99, none skipped or special-cased.** The ratchet only grows. No query is answered by a rule written for it,
   none goes on a skip list, and no limit is raised or gate weakened to let one pass.
2. **Generic engine parts.** Each feature is built as the general SQL feature, with DuckDB's grammar and semantics
   for any query, so it serves queries beyond the 99. A PR may answer a slice of a feature and refuse the rest with
   exit code 4, but the slice is a category of SQL (casts to DECIMAL targets, round of a DOUBLE), never a shape chosen
   because a query uses it.
3. **Layers in a fixed order, one after another.** Phase 0 readies the harness, phase 1 lays performance groundwork,
   and phases 2 to 8 follow the layers of the engine: NULL as a value, casts and constants, scalar functions,
   aggregation, set operations, join kinds and subqueries. Each layer reuses the rules of the ones before it: NULL
   typing before COALESCE, ROLLUP's NULL keys and FULL JOIN padding; one cast framework before every feature that
   casts. Phases 0 to 8 are completed before window functions start; no phase runs in parallel with another.
4. **Window functions come last** (phase 9). Their scope and design are decided then, in an ADR of their own, which
   also replaces `kWindowFunctions` as the permanent canary. Until then they stay out of scope.
5. **Every PR of the Plan** builds one feature, with:
   - `.slt` records completed from DuckDB, with result type names where a type is decided (HC2). Update
     (2026-11-01, HC2): every query record carries the type names of its result, so any change of a result type
     shows in the diff of `slt-complete`;
   - a `Feature` the random generator writes from a random stream of its own, or a place in `kGeneratorPending` with
     the PR that will write it named;
   - metamorphic relations where a relation holds;
   - docs/sql-subset.md (grammar, semantics, exit-code examples) and the ADR the feature updates;
   - the ratchet and its table, where it completes queries, with each completed query's SF 1 seconds and peak memory
     (HC3) in the PR description. Update (2026-11-01, HC3): the numbers come from the "Costs of the queries derived
     from TPC-H and TPC-DS" job summary of the PR's `gcc-compat` run; the description names that leg and carries the
     disclaimer.
6. **An SF 1 budget gate.** A completed query must peak below about 1.5 GiB, and every shard must stay below about
   60% of its inner timeout in each leg that runs it. A PR that misses either waits for the performance guard (PF)
   that fixes its cause, pulled in front of it. Update (2026-11-01, HC3):
   - `tools/ci/tpc_costs.py` warns once for a shard that misses either, or that did not finish, in every leg that
     runs it, and never fails the job.
   - On every PR, `gcc-compat` and `clang-coverage-fuzz` run scale factor 1. The numbers under AddressSanitizer
     come from the nightly `asan-tpc-sf1`. Before merging a PR that completes a heavy query, run
     `pixi run asan-tpc-sf1` on a Linux host. Dispatching the nightly workflow on the branch also works, but it runs
     every nightly job.

   Update (2026-11-01, HC5): DuckDB, the oracle, may use 1 GiB in each run, beyond which it spills to the run's temp
   dir, so the shards that ctest runs at once cannot each take most of a runner's memory for it. The cap leaves DuckDB
   room on all 99 queries at SF 1, measured locally; a query that outgrows it fails its shard with an
   `Out of Memory Error`, which is when to revisit it.

   Update (2026-11-01, HC4): the shards run the queries of `tests/tpcds/sf1_shards.json`, split by measured cost.
   From one run of all 99 in the ASan build (`build/ci-asan`) on a developer's Apple silicon Mac (test-build
   diagnostics, not performance numbers), Q4 has a shard to itself, and the other seven are within 2% of each other. A
   query that does not pass yet counts as the median passing one, so the queries still to come spread over those seven.
   When a shard misses the budget, `tools/ci/tpc_costs.py --shard-map 8` proposes the map again.
7. **The Plan's ids** name the roadmap's PRs in `pending` guards and docs: HC, PF, NL, CT, FN, AG, RU, OJ, SQ and CL;
   X4a-X4d, ADR 0036's X4 but for `BY NAME` and non-SELECT branches, which no query of the 99 needs and which stay on
   X4 with their `pending X4` guards; X7a and PF5, ADR 0036's X7 in two parts; and W1 onward, which the window ADR
   numbers. None collides with an earlier ADR's ids. A `pending` line takes one upper-case letter today; HC8 makes it
   take two before any PR writes such a guard. Update (2026-11-01, HC8): it does, so `pending NL2` and the like parse.

## Consequences

- The ratchet climbs to 84 through phases 2 to 8 and to 99 with the windows of phase 9. The order takes shared
  semantics first rather than the most queries first, so phase 2 completes three queries and phase 8 nine.
- Each feature lands as a general capability, so a PR is larger than what its queries alone need, and other queries
  benefit.
- The 15 window queries wait for the last phase, Q12, Q20 and Q98 included, which need nothing else.
- docs/sql-subset.md's table now lists every construct each failing query needs and the Plan id that completes it.
- The designs the survey suggested are inputs for their PRs, not decisions of this ADR: ROLLUP as one grouping-sets
  operator with a hidden set id, set operations as UNION ALL plus a grouped marker (ADR 0036's X4), and mark joins for
  subqueries under OR.
- OJ5 is the maintainer's addition, not the survey's: NULL-equal join keys and IS [NOT] DISTINCT FROM as a general
  capability, the keys ADR 0023 deferred for its general domain join. It completes none of the 99 by itself. The survey
  rejected computing whole-partition window aggregates as an aggregation joined back to its input only because antb1's
  hash joins have no such keys; with OJ5 that design, which the maintainer prefers, is an option for phase 9's window
  ADR to weigh against a window operator.

## Plan

Queries are those a PR completes when the PRs merge in this order.

| Id | Item | Completes | Status |
| --- | --- | --- | --- |
| **Phase 0** | **The harness** | | |
| HC1 | This ADR; the TPC-DS table of docs/sql-subset.md rewritten from the survey; ADR 0036's context corrected | | Built |
| HC2 | Result type names in `.slt` records: a line `slt-complete` writes from DuckDB's types, matched exactly by both engines | | Built |
| HC3 | Each SF 1 query's seconds and peak memory in the shard logs of every leg, and a local report of each refusal | | Built |
| HC4 | SF 1 shards by measured cost: a committed map of queries to shards instead of every eighth query | | Built |
| HC5 | A memory limit for the DuckDB oracle of the TPC ratchets, spilling to its temp directory | | Built |
| HC6 | Ordered comparison with ORDER BY positions after a star (the star's width from the oracle's answer) | | Built |
| HC7 | Optional: seeded differential queries over the tables derived from TPC-DS at SF 0.1, redacted like `diff.tpch`; built at SF 0.01 (the HC7 update below) | | Built |
| HC8 | `pending` guards take the Plan's ids: `IsRoadmapId` accepts one or two upper-case letters, digits and an optional lower-case letter | | Built |
| **Phase 1** | **Performance groundwork** | | |
| X7a | ADR 0036's X7, first part: conjuncts and dynamic key filters into UNION ALL branches, constant branches pruned; the key filters stay on X7, unscheduled (the X7a updates below) | | Built |
| PF1 | The join order of a many-to-many pair of facts closed by a cycle, pinned on our own fixtures (the PF1 update below) | | Built |
| **Phase 2** | **NULL as a value** | | |
| NL1 | One DuckDB type fold, with the NULL type as its identity, for CASE values and UNION ALL; no answer changes (the NL1 update below) | | Built |
| NL2 | NULL literals as CASE values (THEN and ELSE), typed by NL1's fold; a NULL anywhere else still exits 4 (the NL2 update below) | | Built |
| NL3 | NULL items of set-operation branches: the fold accepts a NULL column, cast below the union (the NL3 update below) | 27 | Built |
| NL4 | IS NULL and IS NOT NULL (the NL4 update below) | 76, 93 | Built |
| NL5 | COALESCE (the NL5 update below) | | Built |
| **Phase 3** | **Casts, constants and DATE arithmetic** | | |
| CT1 | A general cast expression: identity casts, exact widenings and constants that fit (the CT1 update below) | 40 | Built |
| CT2 | Checked casts to DECIMAL(p,s) (the CT2 update below) | 61, 90 | Built |
| CT3 | DOUBLE and DECIMAL to integer types, and DuckDB's names of the integer types (the CT3 update below) | | Built |
| CT4 | Quotients of numeric literals folded into DOUBLE constants (the CT4 update below) | 21 | Built |
| CT5 | DATE plus or minus an integer (the CT5 update below) | 72 | Built |
| **Phase 4** | **Scalar functions** | | |
| FN1 | Typed overloads for scalar functions (ADR 0012's FN1 update) | | Built |
| FN2 | round (the FN2 update below) | 2, 78 | Built |
| FN3 | abs (the FN3 update below) | | Built |
| FN4 | upper and lower (the FN4 update below) | 24, 99 | Built |
| FN5 | concat (the FN5 update below) | 84 | Built |
| **Phase 5** | **Aggregation and grouping sets** | | |
| AG1 | Integer AVG finalized as DuckDB finalizes it (the AG1 update below) | | Built |
| AG2 | stddev_samp, with the rest of DuckDB's variance family (the AG2 update below) | 17, 39 | Built |
| AG3 | SELECT DISTINCT (the AG3 update below) | | Built |
| RU1 | Grouping sets in the grammar: ROLLUP, CUBE and GROUPING SETS in every form DuckDB accepts, GROUPING() and GROUPING_ID(); refused by the binder with exit code 4 naming the item that answers each form, and the forms DuckDB does not parse made syntax errors; with the ADR for grouping sets (the phase 5 and RU1 updates below; ADR 0038) | | Built |
| RU2 | A grouping-sets plan node and its operator, in two levels: each set that no other set contains aggregated once from the input, every other set merged from the groups of a set that contains it; one table per set fed from the input for calls that do not merge exactly (the phase 5 update below) | | Built |
| RU3 | GROUP BY ROLLUP, in every form, duplicate sets kept, with DuckDB's limit of 65535 sets | 5, 18, 22, 80 | Built |
| RU4 | GROUPING() and GROUPING_ID() | | Built |
| RU5 | GROUP BY GROUPING SETS and GROUP BY () | | Built |
| RU6 | GROUP BY CUBE, its sets merged in batches that fit the memory budget | | Planned |
| **Phase 6** | **Set operations (ADR 0036's X4 without `BY NAME` and non-SELECT branches)** | | |
| X4a | UNION, INTERSECT and EXCEPT in the grammar, with parenthesized branches | | Planned |
| X4b | UNION, after PF5's fallback (the X7a update below) | 75 | Planned |
| X4c | INTERSECT | 8, 38 | Planned |
| X4d | EXCEPT | 87 | Planned |
| **Phase 7** | **Join kinds** | | |
| OJ1 | One table of join-kind properties, read by every rule that tells kinds apart | | Planned |
| OJ2 | A FULL join kind and its operator | | Planned |
| OJ3 | FULL OUTER JOIN | 97 | Planned |
| OJ4 | Cross products of multi-row relations | 77 | Planned |
| OJ5 | NULL-equal join keys, and IS [NOT] DISTINCT FROM | | Planned |
| **Phase 8** | **Subqueries** | | |
| SQ1 | Uncorrelated one-row subqueries as values inside conditions | | Planned |
| SQ2 | The same in select lists and CASE | 9 | Planned |
| SQ3 | The one-row proof through CTE and derived-table references | 14 | Planned |
| SQ4 | Scalar subqueries that are not aggregates (a single join, with DuckDB's row check) | 54, 58 | Planned |
| SQ5 | Correlated aggregates inside arithmetic | 6 | Planned |
| SQ6 | Correlated COUNT subqueries | 41 | Planned |
| SQ7 | Mark joins, and IN subqueries under OR | 45 | Planned |
| SQ8 | Correlated EXISTS under OR | 10, 35 | Planned |
| **Phase 9** | **Window functions** | | |
| W1, W2, … | An ADR of their own decides their scope and design, numbers their PRs and names the permanent canary that replaces `kWindowFunctions` | 12, 20, 36, 44, 47, 49, 51, 53, 57, 63, 67, 70, 86, 89, 98 | Planned |
| **Guards** | **Pulled in front of the PR they protect when the budget gate or their own trigger (the updates below) fires** | | |
| PF2 | Filters into derived tables and CTE references: through their Projects, GROUP BY keys and inner joins to the scan of the relation that defines the column | | Conditional |
| PF3 | LEFT JOIN as INNER or anti join where WHERE rejects the padded rows | | Conditional |
| PF4 | One aggregation for one-row subqueries over the same rows | | Conditional |
| PF5 | ADR 0036's X7, its sharing part: a shared result past `MaterializedTable`'s cap first falls back to recomputation, then CTE references topped by a union or set operation are shared; the fallback lands before X4b at the latest (the X7a update below) | | Conditional |
| PF6 | A partial aggregate below the join under a grouping-sets node: grouped by its join keys and every column of its side that any set groups by (ADR 0032's condition 3 over the union of the sets), with AVG as a (SUM, COUNT) partial (the phase 5 update below) | | Conditional |
| PF7 | One-row and single joins on the branch of the relation they read | | Conditional |
| PF8 | Hash-set membership for long IN lists | | Conditional |
| PF9 | One candidate per probe row for semi and anti residuals that read no build column but its keys (ADR 0022's E2c) | | Built |
| PF10 | A derived table's or CTE reference's row estimate after its conditions, without the union branches they rule out, for the join order and the build sides | | Conditional |
| PF11 | Grouping sets merged inside the finest set's partitions: the finest set partitioned by a key prefix that most sets share, so that those sets need no second exchange (the phase 5 update below) | | Conditional |
| **Closing** | | | |
| CL1 | SF 1 shards rebalanced with all 99 passing | | Planned |
| CL2 | This ADR and the feature ADRs accepted | | Planned |

Update (2026-11-01, HC6): the ORDER BY positions of Q47, Q57 and Q89 name columns inside a lone star, not after one. So
the comparison reads a position's key from the answer's own column, wherever stars stand, and the answer's width is
the only width it needs.

Update (2026-11-02, PF9): pulled in front of HC7. HC7's differential test over the tables derived from TPC-DS
generated a correlated `EXISTS` whose residual compares the join's key columns again. The semi join met every row of a
probe row's key, one pair at a time, and the test under ASan would have outrun its timeout. PF9 lets such a join meet
one candidate per row, with E2b's answers and errors ([ADR 0022](0022-joins-and-query-blocks.md)'s E2c).

Update (2026-11-02, HC7): built as `diff.tpcds` and `parallel.diff.tpcds` at SF 0.01, not 0.1, with 200 queries
where `diff.tpch` runs 300. Both choices keep the runs within the default timeout under ASan on a CI runner:

- at SF 0.1, the same 200 queries took 5.8 times as long in a Debug build (test-build diagnostics, on a developer's
  Mac);
- 300 queries at SF 0.01 would bring the parallel run near its timeout on a slow runner.

The tables file declares the schema's 107 foreign keys (`ref=`, table and column names only), along which the
generator joins. The run must print that the generator can follow all of them, since it drops one it cannot follow
without an error. The seed's first 300 queries leave DuckDB clean under LeakSanitizer, against the leak of a string
MIN or MAX that `tests/tpch/CMakeLists.txt` describes.

Update (2026-11-02, X7a's design, agreed with the maintainer before its PR): four changes to the Plan.

- **X7a defers the dynamic key filters into branches.** No passing query would gain much from them. The CTE
  references that Q4, Q11 and Q74 join to each other meet on a string key, and a build publishes a key filter for one
  integer or DATE key only (`exec::CanPublishKeyFilter`, for [ADR 0029](0029-dynamic-join-filters.md)'s filters).
  Q71, the one passing query that joins a union on such a key, would gain little. The key filters stay on
  [ADR 0036](0036-union-all.md)'s X7, unscheduled, until a profile, or the SF 1 budget, shows a join above a union
  whose key filter would cut a measured share of a passing query's time (Q2, Q5 or Q54 may be one once they pass).
  Until then ADR 0036's planning rule stands, and no filter a join offers travels into a branch.

  X7a keeps the conjuncts into the branches and the pruning of the branches a constant rules out, as an Optimize pass
  that puts each conjunct on top of each branch. A union that pruning leaves with one branch stays a union of one
  input, and one it leaves with none keeps one branch under a filter that is never true. Either way nothing under it
  becomes shareable before PF5's fallback: ADR 0036 keeps branches out of sharing because an aggregate-topped branch
  held alone can pass the cap, and the two references of each channel in Q4, Q11 and Q74 would otherwise hold the
  same branch outside any union.
- **PF2 goes through Projects, GROUP BY keys and inner joins.** Today a condition on a derived table or a CTE
  reference stops above its Project, so its scan gets no pushed predicate and no part pruning
  ([ADR 0020](0020-filter-pushdown.md)), as a plain table's scan does. The rule that carries a condition to the scan
  of the relation that defines its column serves every derived table. In a block that reads more than one relation,
  the condition has to pass its inner joins as well: each branch of Q4, Q11 and Q74 joins three relations under its
  GROUP BY, and nothing routes a condition past a join today except the binder's classification of WHERE. The same
  rule carries X7a's conditions from a branch's top down to its scan. Its PR decides how a condition is folded into a
  narrower type below the union's widening cast, and records the changed error order below a Project's computations.
- **PF10 is new.** The binder chooses the join order and the build sides before Optimize runs. Its estimates count
  every branch of a union and leave out the conditions on a derived table or a CTE reference
  (`FilteredRelationRows`), so X7a's pruning never reaches those choices. An estimate that is too high costs a larger
  build than needed, never a build larger than the planner expected. The trigger is a profile, or the SF 1 budget,
  showing a join order or build side chosen badly from such an estimate. The fix shares X7a's pruning check with the
  estimate rather than moving the rewrite into the binder.
- **PF5's first half gets a second trigger.** Its fallback to recomputation past `MaterializedTable`'s cap lands
  before the first PR that makes a result topped by a union or a set operation shareable, not only when the SF 1
  budget fires. The cap can fail a query only above SF 1, since sharing needs 10 million scanned rows, and no CI leg
  runs there. X7a is not such a PR, since its unions of one input stay out of sharing. X4b is the latest: it compiles
  UNION to a GROUP BY over a UNION ALL, which ADR 0030's sharing collects, so a CTE whose body is a UNION, referenced
  more than once, would hold the union's distinct rows. None of the 99 has a CTE whose body is a UNION, INTERSECT or
  EXCEPT referenced more than once, so this trigger guards SQL beyond them.

Update (2026-11-03, X7a): built as the Optimize pass the note above describes (`src/plan/union_branches.h`).

- Queries 4, 11 and 74 run 6 of 18, 4 of 8 and 4 of 8 branches. At scale factor 1, in the release build on a
  developer's Apple silicon Mac, they take 0.35, 0.51 and 0.52 of their time at 14 threads and 0.33, 0.50 and 0.50 at
  one thread (ADR 0036's consequences give the seconds). Under AddressSanitizer there, query 4 took 24.0 seconds at
  one thread instead of 74.8 (test-build diagnostics), so the shard that holds it alone (HC4) has room to spare. The
  plans of the other 47 passing queries do not change.
- The pass decides only what exec's filters decide the same way: a comparison or an IN list whose constants have the
  column's own type (numbers at any scale, DATE, TIMESTAMP, VARCHAR by unsigned bytes), never a DOUBLE or a LIKE.
  DuckDB rules out more, so divergence D27 records the errors of the blocks antb1 still runs. PF2 narrows it.
- NL3 must teach the evaluator NULL constants before a NULL item reaches a union, since a NULL makes a comparison
  neither true nor false. A `static_assert` in `src/plan/branch_truth.cc` stops a new alternative of a constant's
  value until the evaluator decides it.

Update (2026-11-03, PF1): built as pins only, with no estimate changed. The default cost search ([ADR
0026](0026-join-performance.md)'s C5) already orders the pair of facts the way Q72 needs. Checked locally on Q72 at
scale factors 0.1 and 1, its unsupported parts replaced, the plan had no large intermediate and stayed far below the
memory limit.

- **The shape.** The sales-like fact is first reduced through its filtered dimensions and the year of its first
  calendar role. The second calendar role joins it through the week equality, the only key of that join. That side
  is built, and the snapshot streams as the probe of the facts' join, on two keys: the product, and the snapshot's day
  against the second role's.
- **The literal form loses.** That form takes the product and the week as the facts' two keys. The search does not
  choose it while the week columns' domains are known: it would carry the snapshot through one more join.
- **Where it is pinned.** On our own fixtures, a second pair of facts in the star schema: `bookings` and the weekly
  snapshot `fleet` on `models`, closed through two roles of the calendar `days` (`tools/fixturegen/star.h`). The CLI
  goldens `explain_join_fact_pair` (the search) and `explain_join_fact_pair_greedy` (the control) pin the order.
  `plan.BinderTest.AFactPairClosedByACycleMeetsOnTwoKeysAfterTheSalesSideShrinks` and its greedy control assert the
  same structure over FakeTables at a larger scale, and `tests/slt/cases/joins/fact_pair.slt` the answers. That file's
  last records wait on NL4 and CT5.
- **Hazards** that would lose the shape, each a trigger for a guard of its own:
  - **A sparse key.** A key's domain is its integer range capped at its rows, so a product key sparse within its
    range reads as unique, and the facts join on it alone. `harness.StarSchema` pins that the fixtures' model key
    stays dense.
  - **Unknown LEFT JOIN units.** A LEFT JOIN unit's estimate is always unknown, against ADR 0022's rule 3, and it
    already changes the plan of the comma spelling in `fact_pair.slt` (with the same answer). This needs a fix of its
    own. Update (2026-11-03): fixed. A unit's blocks of one relation now carry that relation's estimate
    (`Binder::BlockEstimate`), which every reader of a join's estimate now sees: the join order, a derived
    table's or CTE's `EstimateRows`, the build-side choice and ADR 0032's gate (its dated update). No plan of the
    passing queries derived from TPC-H or TPC-DS changes; `explain_join_left_unit_built` and
    `explain_join_left_unit_derived_built` pin units that are now built.
  - **The greedy rule.** It replaces the search beyond 12 relations and probes from the snapshot, building the whole
    sales fact.
  - **The calendar's width.** A calendar much wider than the facts' days under-estimates the reduced sales side; it is
    harmless here.
  - **A cycle equality that is no key** becomes a residual, and the facts meet on the product alone. That happens to
    a side that reads both roles, such as a difference of the two weeks compared with 0, or to sides with no exact
    common key type. A computed equality whose sides each read one role stays a key, but without a domain; with both
    sides computed, the search takes the literal form above.
  - **Filters the estimate cannot see** (an OR, a NOT) leave the sales side looking large.
- **Speed, not PF1.** A join on two keys publishes no dynamic key filter (ADR 0029), so the snapshot is read whole.

Update (2026-11-03, NL1): built as `plan::CommonValueType` (`src/plan/include/antb1/plan/type_fold.h`), the type DuckDB
gives two values together, with the NULL type (`std::nullopt`) as its identity. It is commutative but not associative,
as DuckDB's fold is not: a UNION ALL column folds left in written order. `UnionAllType` and `CaseCommon` now take
their type from it. No answer changes and no refusal message changes.

- **Where CASE and the union differ.** DuckDB's CASE and UNION ALL agree on every pair of numbers, on BOOLEAN with an
  integer (the integer) and on DATE with TIMESTAMP. The union types every other mix too:
  - VARCHAR with another type gives VARCHAR;
  - BOOLEAN with a DECIMAL or a DOUBLE gives the number;
  - a number or BOOLEAN with a DATE or TIMESTAMP gives the date, whose cast then fails as the query runs.

  DuckDB's CASE refuses those mixes, and `plan::DuckDbCaseMixes` says which. antb1 still accepts less than either:
  in a union, only integers and DECIMALs, which it widens exactly (ADR 0036); in CASE, numbers only.
- **The NULL type is not a `LogicalType` yet.** NL2 and NL3 type a NULL value through the fold. A CASE or a column
  whose values are all NULL keeps DuckDB's NULL type, which no result column holds yet; it stays exit 4 until a PR
  needs it.
- **Pinned** by `plan.TypeFoldTest` and by `TypeFoldOracle`, which checks the fold against the DuckDB library over
  every pair of the engine's types and NULL, for UNION ALL and for CASE.

Update (2026-11-03, NL2): `THEN NULL` is built. It covers a whole value before `WHEN`, `ELSE` or `END`, the only shape
the queries write besides `ELSE NULL END`, which was already a `CASE` without `ELSE`.

- **Parsing.** The parser makes it a `sql::Literal` of kind `kNull`, which NL3 and NL5 can reuse. Any other NULL stays
  the parser's unsupported literal, `THEN (NULL)` and `THEN NULL + 1` included.
- **Binding and execution.** The binder binds it as a null `thens` entry of the plan's `CaseExpr`, as a missing ELSE
  is a null `otherwise`. The executor leaves its rows NULL.
- **Typing.**
  - Without a DECIMAL value, it is the identity of the fold.
  - DuckDB also makes a literal next to it its own type (`THEN NULL THEN s16 ELSE 7` is INTEGER). That is divergence
    D20's existing rule for the missing ELSE, whose row now names `THEN NULL` too.
  - With a DECIMAL value, `DecimalCaseType` folds it as DuckDB does.
- **All NULL.** A `CASE` whose values are all NULL keeps exit 4.
- **What it unlocks.** Two of the 99 queries get past this refusal and now stop later: one at `stddev_samp`, one at a
  cast to DECIMAL. The ratchet does not change.

Update (2026-11-03, NL3): built for UNION ALL. NULL items of INTERSECT and EXCEPT wait for X4c and X4d, which reuse it.

- **Parsing.** `NULL` or `NULL AS name` as a whole select item parses to NL2's `kNull` literal. A query that is no
  compound refuses it once its block and tail are parsed, so a later parse error in that block now comes first. An
  implicit alias (`NULL x`) and NULL in an expression stay unsupported.
- **The fold.** The binder binds the item as a NULL of INTEGER flagged `OutputColumn::null_item`. `BindUnionAll`
  folds over the other blocks, with NULL as the identity, and computes a NULL of the column's type below the union in
  each block that writes one. A column that is NULL in every block keeps exit 4.
- **NULL constants.** `plan::Constant` holds a NULL as `std::monostate`, and `ToArrowScalar` makes Arrow's NULL of
  the type. `TruthOver` decides it first: no predicate that reads a NULL is true. So a WHERE above the union rules
  out a branch whose column is NULL, through `ConstantColumn`, which now reads a constant of a `ComputeNode`.
- **The ratchet.** Query 27 passes: 51 of the 99. The other queries with NULL items need INTERSECT, EXCEPT or UNION,
  or a later item.

Update (2026-11-04, NL4): built. Queries 76 and 93 pass: 53 of the 99.

- **Parsing.** `operand IS [NOT] NULL` is a new AST node, `sql::IsNullExpr`, at the comparisons' level as in DuckDB,
  and like them it does not chain (`a = b IS NULL` is unsupported). Every other `IS` stays unsupported, `IS DISTINCT
  FROM` until OJ5.
- **Binding.** `IS NULL` binds as a new two-valued predicate, `Predicate::Kind::kIsNull`, never NULL. `IS NOT NULL`
  as a `WHERE` or `HAVING` conjunct binds as the existing folded `kIsNotNull`, which a filter evaluates the same way,
  so the scan, part pruning and the estimates keep reading it. Inside an expression it is `NOT (IS NULL)`: false for
  NULL, where the folded kind would be NULL.
- **Execution.** `kIsNull` never goes into a scan's per-column filter, whose fast paths drop NULL rows: a filter
  above the scan applies it.
- **The other passes.** Branch truth decides it for a constant (always for NULL, never for any other value). The
  join order estimates it from the null count.
- **Outer joins.** Nothing assumes that a `WHERE` on the null-supplying side of a `LEFT JOIN` rejects the padded
  rows. Above the join, `IS NULL` keeps them, which is the anti-join shape; PF3 may plan it as an anti join.

Update (2026-11-04, NL5): built, and phase 2 is complete. No query passes from it alone: the eight that need
`COALESCE` now stop at a later construct.

- **Binding.** `COALESCE(a, ..., z)` binds as a plan `CaseExpr` whose branches have no condition: a null `when`
  takes the rows where the branch's value is not NULL, and `z` is the ELSE. Each argument is in the plan once and is
  computed once, only for the rows the earlier ones left NULL, as in DuckDB. A nested COALESCE therefore stays linear;
  binding `a` both as an `IS NOT NULL` condition and as a value doubled the tree at every level.
- **Typing.** It shares the CASE value typing, now one function, `ValuesType`, with DuckDB's fold from the first
  argument in written order (the first kept as it is, like an ELSE value). The refusals name COALESCE.
- **NULL arguments.** A NULL argument stays an unsupported NULL literal.

Update (2026-11-04, CT1): built. Query 40 passes: 54 of the 99.

- **Binding.** `CAST(x AS T)` binds for every T that antb1 has a type for, under DuckDB's aliases (`INT`, `INT4`,
  `NUMERIC`, `FLOAT8`, `TEXT`, ...). It answers three kinds of cast: an identity, which keeps the value; an exact
  widening, which is the executor's `CastExpr`; and a constant that T holds exactly, folded into a constant of T. The
  result is named as DuckDB names a cast.
- **Refusals.** A cast to a type antb1 has none for stays CheckSupported's refusal, before any name is resolved.
  Every other cast is unsupported at bind time, naming the two types. An identity is computed (a `CastExpr` of
  a type to itself), so a `GROUP BY` of it groups by the cast, as in DuckDB, and not by the bare column.
- **What answers more later.** CT2 answers checked casts to DECIMAL (two queries reach that refusal now), and CT3 the
  integer casts of DOUBLE and DECIMAL.

Update (2026-11-04, CT2): built. Queries 61 and 90 pass: 56 of the 99.

- **What it answers.** An integer or a DECIMAL cast to any DECIMAL is the executor's `CastExpr`, which
  `plan::IsExecutableCast` admits beside the exact widenings. It runs `CastToDecimal`, the conversion CASE values
  already used, so it rescales as DuckDB casts: rounded half away from zero where the scale shrinks, and failing with
  DuckDB's conversion error, naming the source column, on a value the DECIMAL cannot hold. A decimal literal folds
  into a constant with the same rounding.
- **What it does not.** A DOUBLE to DECIMAL stays unsupported: DuckDB's binary-to-decimal rounding is not
  reproduced. So does a constant that does not fit, which DuckDB fails to convert on any row (exit 4 here).
- **Join keys.** A key whose side is such a cast is no longer taken to have its column's domain, since rounding can
  merge values; only an exact widening is.
- **Shared sub-plans.** An expression's key now holds its full type, a DECIMAL's width and scale included, so two
  sub-plans that cast one column to different DECIMALs are no longer computed once. They were grouped since CT1,
  for exact widenings to different scales.

Update (2026-11-04, CT3): built, for integer sources too. No query passes from it alone: query 54, the one that
casts to an integer, also needs constructs of a later phase.

- **The casts.** `plan::IsExecutableCast` admits an integer type, a DECIMAL or a DOUBLE cast to any integer type.
  The new `exec::CastToInteger` runs it as DuckDB casts: an integer kept, a DECIMAL rounded half away from zero, a
  DOUBLE half to even. A decimal literal folds with the DECIMAL rounding.
- **Errors.** A value the type cannot hold fails with DuckDB's conversion error, which names the integer types by
  their physical names (INT16, INT32, INT64, INT128, UINT16) and a DOUBLE as DuckDB prints it
  (`exec::DuckDbDoubleText`).
- **Range of a DOUBLE.** A DOUBLE is checked against `[min, max + 1)` before rounding, as DuckDB checks it. A value
  that then rounds past the maximum fails, where DuckDB's answer depends on the platform (divergence D28).
- **Not covered.** DOUBLE to HUGEINT stays unsupported: DuckDB's HUGEINT reaches 2^127, antb1's 38 digits.

Update (2026-11-04, CT4): built. Query 21 passes: 57 of the 99.

- **Where it folds.** A quotient of two numbers, in a comparison, a `BETWEEN` bound or an `IN` list, folds where a
  constant integer expression already folded (`IsFoldableConstant`, `FoldConstant`). A number here is an integer or
  decimal literal, negated or not, or a constant integer expression.
- **The value.** It is the DOUBLE DuckDB computes: each side converted to a double by `plan::DuckDbDoubleOf`, then
  divided. It is written as a literal with an exponent, so that it reads back as that DOUBLE and is compared as one.
- **What stays refused.** A zero divisor, which DuckDB divides into an infinity. A quotient inside other arithmetic.

Update (2026-11-04, CT5): built, and phase 3 is complete. Query 72 passes: 58 of the 99.

- **Typing.** A DATE plus or minus days, or days plus a DATE, is a DATE (`ArithType`). The days are a SMALLINT,
  USMALLINT or INTEGER, as DuckDB's `+(DATE, INTEGER)` takes them; another number, or days minus a DATE, is DuckDB's
  bind error.
- **Execution.** The executor adds the days in 64 bits (`DateArith`). An infinite DATE stays itself, and a result
  that leaves INTEGER or lands on one of DuckDB's infinities fails with its `Date out of range`, as DuckDB adds them.
- **What stays unsupported.** A DATE minus a DATE, which DuckDB answers as a BIGINT of days, and TIMESTAMP arithmetic
  wait for a query that needs them. The `pending CT5` guard in `tests/slt/cases/joins/fact_pair.slt` is removed.

Update (2026-11-04, FN2): built as two rows of FN1's table, `round(DOUBLE)` and `round(DOUBLE, precision)`. Queries
2 and 78 pass: 60 of the 99. At scale factor 1, query 78 peaks at 665 MiB in the Debug build, under the 2 GiB limit.

- **The value.** It is computed as DuckDB's `RoundOperator` and `RoundOperatorPrecision` compute it: `std::round`,
  and at p digits `std::round(x * std::pow(10, p)) / std::pow(10, p)`. The value itself is kept where that result is
  not finite.
- **The precision.** It is a constant integer expression of at least 0.
- **What stays unsupported.** DuckDB's DECIMAL and integer overloads, a FLOAT column (D11) and a negative or
  computed precision. A new row field, `other_numbers_unsupported`, makes a number that no row of a function takes
  unsupported rather than a bind error.

Update (2026-11-04, FN3): built as one row of FN1's table that keeps its value's type. A new argument kind,
`kNumber`, takes any number. No query passes from it alone: the five that call abs need windows (phase 9).

- **The value.** It is computed as DuckDB's abs: `fabs` for a DOUBLE; the magnitude for an integer type, with
  DuckDB's `Overflow on abs(<min>)` at the type's minimum; the magnitude for a DECIMAL or HUGEINT, whose ranges are
  symmetric.
- **What stays unsupported.** A FLOAT column (D11).

Update (2026-11-04, FN4): built as four rows of FN1's table, `upper` and `lower` and DuckDB's aliases `ucase` and
`lcase`. Queries 24 and 99 pass.

- **The mapping.** DuckDB maps each codepoint to one codepoint by the simple case mapping of its bundled utf8proc.
  Arrow's `utf8_upper` and `utf8_lower` use the utf8proc that Arrow links, whose Unicode version differs: it maps 110
  codepoints differently, and the conda and static builds link different versions. So antb1 carries DuckDB's own
  table, `src/exec/unicode_case.inc`, written from the DuckDB CLI by `tools/unicode/case_tables.py`. A harness test
  compares every codepoint with the DuckDB library the tests link, so a DuckDB upgrade that changes a mapping fails
  it.
- **Bytes that are not UTF-8.** DuckDB cannot read them as VARCHAR. antb1 returns such a value unchanged, which
  extends divergence D15.

Update (2026-11-04, FN5): built as one row of FN1's table, which gains variadic rows: every argument of such a row is
a value of its first kind, from `min_args` on. Query 84 passes.

- **The value.** DuckDB's concat: the values in order, a NULL one skipped, so the result is never NULL.
- **What stays unsupported.** A value of a type other than VARCHAR, which DuckDB casts to VARCHAR (a new row field,
  `other_types_unsupported`, makes it unsupported rather than a bind error), and the `||` operator.

Update (2026-11-05, phase 5): the maintainer's decision. Phase 5 answers every form of grouping sets, not ROLLUP
alone: RU1, RU3 and RU4 widen, RU5 and RU6 are new, and RU2 takes the maintainer's design direction. RU1's ADR decides
the design in full and records the measurements below; RU2 measures its operator (the RU1 update below).

- **The forms.** ROLLUP, CUBE and GROUPING SETS as DuckDB accepts them: nested sets, composite elements such as
  `ROLLUP((a, b), c)`, plain keys and several clauses combined (the cross product of their set lists), the empty set
  `()` as a GROUP BY item or a GROUPING SETS element, and expressions, positions, aliases and constants as keys.
  - Duplicate sets stay, as in DuckDB: `ROLLUP(a, a)` emits the groups of `(a)` twice. RU3 meets them first, and with
    them DuckDB's limit of 65535 sets.
  - Where DuckDB raises an error the binder raises it too: more than 65535 sets, 64 or more GROUPING arguments, a
    GROUPING argument that is not a key, and GROUPING in WHERE or inside an aggregate.
  - The forms DuckDB does not parse, such as `ROLLUP ()`, `ROLLUP (a, ())` and `GROUP BY ALL, ROLLUP (a)`, become
    syntax errors (exit code 1) in RU1; today they exit 4.
- **Two levels (RU2).** Each set that no other set contains is aggregated once from the input, as a GROUP BY is today:
  each part's rows into a partial table, then the parts merged partition by partition. For ROLLUP and CUBE that is the
  finest set alone, the union of every set's keys. Every other set is merged from the merged groups of a set that
  contains it, all of them in one pass over those groups with one barrier, not as a chain of sets that each wait for
  the one before.
  - A GROUPING SETS list builds no table for a union of its keys that it does not list: `GROUPING SETS ((a), (b))`
    aggregates both of its sets from the input.
  - Sets merge where every call merges exactly: COUNT(*) and COUNT(x), integer and DECIMAL SUM and AVG (a 128-bit sum
    and a count), and MIN and MAX of types other than DOUBLE, with no DOUBLE key. The aggregates of the nine queries
    that use ROLLUP all merge exactly.
  - Otherwise each set is aggregated from the input rows into a table of its own, as DuckDB does: COUNT(DISTINCT)
    does not merge from finer groups, and a DOUBLE sum, the sign of a zero and the first spelling of a DOUBLE key
    depend on the order of the merge.
  - The parts' partial groups are no source for the other sets: a part of 128Ki rows barely reduces fine keys, so each
    set would then merge about as many partial groups as there are input rows.
- **Measured** on synthetic data: 20 million rows, ROLLUP of 4 and 8 keys over five key profiles, from a finest set of
  10 thousand groups to one nearly as large as the input. Each figure is the time against the finest set alone.
  - A local prototype on the engine's GroupAggregateOperator, one thread:

    | Every other set ... | Time |
    | --- | --- |
    | merged from the finest set's groups, each from the next finer set | 0.99 to 1.77 times |
    | merged from the finest set's groups, each from the finest set | 1.00 to 2.61 times |
    | aggregated from the input rows, one table per set | 1.25 to 4.01 times |
    | merged from each part's partial groups | 1.08 to 4.14 times |
    | from the input rows copied once per set with a set id (the survey's "hidden set id"; rejected) | 1.81 to 6.47 times |

  - DuckDB 1.5.6 in SQL, each set re-aggregated by a parallel aggregation of its own over a materialized set, against
    DuckDB's one-pass operator:
    - At one thread, each set from the next finer set took 1.00 to 2.13 times, each from the finest set 1.02 to 3.31,
      and the operator 1.35 to 3.18.
    - At 14 threads, where the finest set reduces the input, each set from the finest set took 1.07 and 1.11 times
      against the operator's 2.32 and 1.45.
    - Where the finest set barely reduces the input, each set from the finest set took 1.35, 2.24 and 4.36 times
      against the operator's 1.34, 2.03 and 4.34. Merging a set from about as many groups as there are input rows is
      about as much work as aggregating it from the rows; only a smaller source saves. The chain, each set from the
      next finer one, took 1.65, 2.63 and 3.62: it lost on two of the three and won on the eight-key profile.
- **PF11.** The finest set's merged groups are partitioned by the hash of all its keys, so one group of a coarser set
  lies in many partitions and its merge needs a second exchange. PF11 partitions the finest set by a key prefix that
  most sets share instead, such as a ROLLUP's leading keys with enough distinct values. The sets that contain the
  prefix then merge inside each partition, in a chain with nothing between its steps; the rest, at least `()`, merge
  each partition's few rows once every partition finishes. It is pulled in front of a grouping-sets PR when the budget
  gate fires on it or a profile shows the exchange.
- **CUBE (RU6).** One pass fills every merged set's table at once, and CUBE has 2^n - 1 of them. RU6 merges its sets
  from the finest set in batches that fit the memory budget, one pass and one barrier per batch, each batch emitted
  before the next. The fallback, one table per set fed from the input, holds them all, up to the memory limit.
- **PF6** now names any grouping-sets node, groups by ADR 0032's condition 3 over the union of the sets, and takes AVG
  as a (SUM, COUNT) partial. As written it could not help RU3, whose queries 18 and 22 average only.
- **docs/sql-subset.md** no longer says that queries 5 and 80 need concat, which FN5 built, and it lists how the
  parser refuses grouping sets today: `GROUP BY a, GROUPING SETS (...)` is a syntax error (exit code 1), which RU1
  makes exit code 4.

Update (2026-11-05, AG1): built. An integer AVG divides its exact 128-bit sum by the count as DuckDB does, so it is
DuckDB's to the bit on each platform. No query passes from it alone.

- **The division** (`DuckDbIntegerAverage`, DuckDB's DECIMAL average at scale 0): a SMALLINT's sum as an int64,
  divided in double; any other integer's sum as a long double from its two 64-bit halves, divided by the count in long
  double, then rounded to DOUBLE. DuckDB averages a USMALLINT as a BIGINT. Before, antb1 divided the exact quotient
  and remainder separately, within about one ulp of the mean, which differs past 2^53: three BIGINT values of 2^53 + 1
  now average to 2^53 + 2 in both engines on Apple silicon, where long double is double; an 80-bit long double gives
  2^53.
- **The generator** now compares an integer AVG in correlated blocks, where ADR 0023 allowed only a DECIMAL one. A
  DOUBLE AVG stays out: each engine adds its values in its own order.

Update (2026-11-05, AG2): built as DuckDB's variance family, not `stddev_samp` alone: `stddev_samp`, `stddev_pop`,
`var_samp` and `var_pop`, and the aliases `stddev` and `variance`. Queries 17 and 39 pass: 65 of the 99.

- **The value.** DuckDB's STDDevBaseOperation, operation for operation: each value cast to DOUBLE as DuckDB casts it,
  a running count, mean and sum of squared deviations (Welford's update), partial states combined by Chan's formula,
  and DuckDB's finalization: NULL over no values, and over one for the sample forms; 0 over one for the population
  forms; DuckDB's out-of-range error for a result that is not finite.
- **Order.** The result depends on the order of the additions, as a DOUBLE SUM's does. antb1 merges in part order
  and never routes a part's rows unaggregated, so its result is the same for any number of threads. The oracle
  compares it with the DOUBLE tolerance, since DuckDB's parallel merge order differs.
- **Names.** Each spelling is a kind of its own in the parser, so a result is named as written (`stddev(x)`), and an
  alias binds to its target.
- **What stays unsupported.** `DISTINCT`, as for every aggregate but COUNT. The generator writes the family as select
  items over integer and DECIMAL columns, from a stream of its own, and two metamorphic relations hold for it.

Update (2026-11-05, AG3): built. No query passes from it alone: the five that use it need later items too, and
docs/sql-subset.md's table no longer lists it for them (nor, for query 54, the DOUBLE-to-integer cast and round that
CT3 and FN2 built).

- **The plan.** A GroupAggregate of every select-list column, without calls, between the block's Project and its
  Sort, so it removes repeated rows of the select list after HAVING and before ORDER BY and LIMIT, in any block.
  Every item is a key of its own, so a repeated item keeps its column; over a global aggregate's one row it is left
  out. Being an ordinary GROUP BY, it compares as one and runs on the parallel operators already built.
- **ORDER BY.** It takes a select item: a position, an alias, or an item written again. DuckDB orders by any other
  value through `first()` of each distinct row, which depends on the order of the rows, so antb1 refuses it with exit
  code 4. `DISTINCT ON (...)` is unsupported too.
- **Tests.** The generator writes DISTINCT on the outer block from a stream of its own, ordering only by aliases and
  positions then, and three metamorphic relations hold for it.

Update (2026-11-05, RU1): built, with [ADR 0038](0038-grouping-sets.md), which decides the grouping sets' semantics,
plan node and two-level execution and records the measurements of the phase 5 update. The measurement on the engine's
parallel operator comes with RU2, as an update of ADR 0038, not in RU1, which builds no operator.

- **The grammar.** Every form DuckDB parses, kept whole in `SelectStatement::grouping`, refused by the binder at its
  keyword or parenthesis with exit code 4 naming RU3, RU5 or RU6; GROUPING() and GROUPING_ID() name RU4. DuckDB's
  syntax errors are antb1's: `ROLLUP ()`, `()` inside ROLLUP or CUBE, `GROUPING SETS ()`, a list after
  `GROUP BY ALL`. `rollup` and `cube` stay names before anything but `(`, which repairs the round trip of
  `GROUP BY (cube)`; a later `GROUPING SETS` item is no longer a syntax error.
- **Tests.** `.slt` records under `pending RU3` to `pending RU6`, and the generator's four new Features wait in
  `kGeneratorPending` for those items.

Update (2026-11-05, RU2): built. `GroupingSetsNode` is in every pass that walks a plan. `GroupingSetsOperator` and
`PartGroupingSetsOperator` run it in ADR 0038's two levels, with one table per set for calls that do not merge
exactly. No SQL binds to the node before RU3, so no query passes from it alone.

- **The second level** merges each set either from per-partition tables or straight from the root's, chosen per set
  by how much the root's largest partition reduces. ADR 0038's RU2 update has the measurements: at 20 million rows
  on 14 threads, two levels cost 1.08 to 1.20 times the finest set alone where it reduces the input, against 1.42 to
  2.37 for one table per set, and the two are within 5 % where it does not.
- **Tests.** Plan-level tests cover the node in every visitor. Exec tests check both operators against every set's
  GROUP BY (ROLLUP, CUBE, duplicates, two roots, the empty set over no input, the fallback), at 1 and 4 threads,
  with every set's errors raised before the first row.

Update (2026-11-05, RU3): built. `GROUP BY ROLLUP` binds to ADR 0038's `GroupingSetsNode` in every form: keys and
composites, several clauses (their cross product, the earlier clause varying slowest), duplicate sets kept, and DuckDB's
limit of 65535 sets. Queries 5, 18, 22 and 80 pass; 69 of the 99 do.

- **Refused:** a constant in a ROLLUP, which DuckDB groups by and shows NULL where rolled up (exit code 4).
- **A divergence, kept out of the generator:** DuckDB keeps two spellings of one column in a grouping list as two
  grouping expressions, each NULL in its own sets, so its HAVING can read a different one than its select list; antb1
  groups by one key, as its plain GROUP BY does (docs/sql-subset.md).
- **Tests.** `.slt` records over the star schema, the binder's sets and errors, the generator's ROLLUP stream (a salted
  stream of its own, so no other query of a seed changes), and three metamorphic relations: ROLLUP is the union of
  its sets' GROUP BYs, its empty set over no rows is the ungrouped aggregate, and its rows do not depend on the batch
  size or the split.
- **The harness** pairs the rows of a rowsort block with R columns within the tolerance when their text order separates
  them (a last-digit difference of a DOUBLE ahead of the other cells), and the generator keeps the variance family out
  of a SELECT DISTINCT's list (two values a last digit apart are two rows in one engine and one in the other): both
  failed rare seeds of the variance family before RU3, which made them more frequent.

Update (2026-11-05, RU4): built. `GROUPING()` and `GROUPING_ID()` are columns of ADR 0038's `GroupingSetsNode`
(a GROUPING call per distinct argument list; over a plain GROUP BY the node has one set of every key), in the select
list, HAVING, ORDER BY and expressions there, with DuckDB's errors (no GROUP BY, an argument that is no key as an
expression, WHERE, inside an aggregate, 64 or more arguments). No query passes from it alone: 70 and 86, which call it,
also need window functions. The generator selects GROUPING() of a grouped query's keys from the ROLLUP's stream, and two
metamorphic relations check it: HAVING on its value picks one set's own GROUP BY, and it is 0 over a plain GROUP BY.

Update (2026-11-05, RU5): built. GROUPING SETS (its elements' sets one after another, a nested ROLLUP's in place,
nested lists included), a composite `(a, b)` as a GROUP BY item and the empty set `()` bind as DuckDB expands them;
clauses still combine as a cross product, duplicates stay, and the 65535-set limit holds. A single set of every key is a
plain GroupAggregate, as DuckDB plans it; `GROUP BY ()` alone keeps the GroupingSets node, whose empty set has its row
even over no input. A constant anywhere in grouping-set syntax is refused (exit code 4). No query derived from TPC-DS
uses these forms. The generator writes GROUPING SETS from the grouping-set stream (the set of every key first, each key
spelled the same everywhere), and four metamorphic relations check them: written-out ROLLUP sets are the ROLLUP, a list
of sets is the union of their GROUP BYs, and `()` is the ungrouped aggregate over rows and over none.

## Alternatives considered

- **The features that complete the most queries first.** Rejected by the maintainer: the aim is all 99, and a layer
  whose rules others reuse (NULL typing, casts) comes first however few queries it completes.
- **Window functions in parallel with phases 2 to 8**, since they are the longest chain. Rejected by the maintainer:
  windows are designed as a generic engine part once the layers they rest on exist, and their scope is decided then.
- **One ADR per phase.** Rejected: the ids need one home, while each feature's design belongs to the ADR that
  deferred it (0004, 0012, 0021, 0022, 0023, 0036) or to a new one for grouping sets and for windows.

This workload is derived from the TPC-DS Benchmark and is not comparable to published TPC-DS Benchmark results, as
this implementation does not comply with all requirements of the TPC-DS Benchmark.
