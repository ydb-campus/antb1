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
| HC4 | SF 1 shards by measured cost: a committed map of queries to shards instead of every eighth query | | Planned |
| HC5 | A memory limit for the DuckDB oracle of the TPC ratchets, spilling to its temp directory | | Planned |
| HC6 | Ordered comparison with ORDER BY positions after a star (the star's width from the oracle's answer) | | Planned |
| HC7 | Optional: seeded differential queries over the tables derived from TPC-DS at SF 0.1, redacted like `diff.tpch` | | Planned |
| HC8 | `pending` guards take the Plan's ids: `IsRoadmapId` accepts one or two upper-case letters, digits and an optional lower-case letter | | Built |
| **Phase 1** | **Performance groundwork** | | |
| X7a | ADR 0036's X7, first part: conjuncts and dynamic key filters into UNION ALL branches, constant branches pruned | | Planned |
| PF1 | The join order of a many-to-many pair of facts closed by a cycle, pinned on our own fixtures | | Planned |
| **Phase 2** | **NULL as a value** | | |
| NL1 | One DuckDB type fold, with the NULL type as its identity, for CASE values and UNION ALL; no answer changes | | Planned |
| NL2 | NULL literals as CASE values (THEN and ELSE), typed by NL1's fold; a NULL anywhere else still exits 4 | | Planned |
| NL3 | NULL items of set-operation branches: the fold accepts a NULL column, cast below the union | 27 | Planned |
| NL4 | IS NULL and IS NOT NULL | 76, 93 | Planned |
| NL5 | COALESCE | | Planned |
| **Phase 3** | **Casts, constants and DATE arithmetic** | | |
| CT1 | A general cast expression: identity casts, exact widenings and constants that fit | 40 | Planned |
| CT2 | Checked casts to DECIMAL(p,s) | 61, 90 | Planned |
| CT3 | DOUBLE and DECIMAL to integer types, and DuckDB's names of the integer types | | Planned |
| CT4 | Quotients of numeric literals folded into DOUBLE constants | 21 | Planned |
| CT5 | DATE plus or minus an integer | 72 | Planned |
| **Phase 4** | **Scalar functions** | | |
| FN1 | Typed overloads for scalar functions | | Planned |
| FN2 | round | 2, 78 | Planned |
| FN3 | abs | | Planned |
| FN4 | upper and lower | 24, 99 | Planned |
| FN5 | concat | 84 | Planned |
| **Phase 5** | **Aggregation and grouping sets** | | |
| AG1 | Integer AVG finalized as DuckDB finalizes it | | Planned |
| AG2 | stddev_samp | 17, 39 | Planned |
| AG3 | SELECT DISTINCT | | Planned |
| RU1 | ROLLUP and GROUPING() in the grammar, refused by the binder, with an ADR for grouping sets | | Planned |
| RU2 | A grouping-sets plan node and its operator | | Planned |
| RU3 | GROUP BY ROLLUP | 5, 18, 22, 80 | Planned |
| RU4 | GROUPING() | | Planned |
| **Phase 6** | **Set operations (ADR 0036's X4 without `BY NAME` and non-SELECT branches)** | | |
| X4a | UNION, INTERSECT and EXCEPT in the grammar, with parenthesized branches | | Planned |
| X4b | UNION | 75 | Planned |
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
| **Guards** | **Pulled in front of the PR they protect when the budget gate fires** | | |
| PF2 | Filters into derived tables and CTE references through their GROUP BY keys | | Conditional |
| PF3 | LEFT JOIN as INNER or anti join where WHERE rejects the padded rows | | Conditional |
| PF4 | One aggregation for one-row subqueries over the same rows | | Conditional |
| PF5 | ADR 0036's X7, its sharing part: a shared result past `MaterializedTable`'s cap first falls back to recomputation, then CTE references topped by a union or set operation are shared | | Conditional |
| PF6 | A partial aggregate below the join under a ROLLUP | | Conditional |
| PF7 | One-row and single joins on the branch of the relation they read | | Conditional |
| PF8 | Hash-set membership for long IN lists | | Conditional |
| **Closing** | | | |
| CL1 | SF 1 shards rebalanced with all 99 passing | | Planned |
| CL2 | This ADR and the feature ADRs accepted | | Planned |

## Alternatives considered

- **The features that complete the most queries first.** Rejected by the maintainer: the aim is all 99, and a layer
  whose rules others reuse (NULL typing, casts) comes first however few queries it completes.
- **Window functions in parallel with phases 2 to 8**, since they are the longest chain. Rejected by the maintainer:
  windows are designed as a generic engine part once the layers they rest on exist, and their scope is decided then.
- **One ADR per phase.** Rejected: the ids need one home, while each feature's design belongs to the ADR that
  deferred it (0004, 0012, 0021, 0022, 0023, 0036) or to a new one for grouping sets and for windows.

This workload is derived from the TPC-DS Benchmark and is not comparable to published TPC-DS Benchmark results, as
this implementation does not comply with all requirements of the TPC-DS Benchmark.
