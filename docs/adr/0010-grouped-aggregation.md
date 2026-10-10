# 10. Grouped aggregation

Date: 2026-09-27

## Status

Accepted (with the maintainer-approved plan for GROUP BY, ORDER BY and COUNT(DISTINCT))

## Context

- Most ClickBench queries beyond the first slice group rows and aggregate per group. The engine had only global
  aggregates: every `AggregateState` holds one value, and nothing maps rows to groups.
- Arrow C++ 25 has a grouper (`arrow::compute::Grouper` in `arrow/compute/row/grouper.h`) that maps rows of key
  columns of any engine type (integers, decimal128, float64, binary, date32) to dense `uint32` group ids, with NULL
  as a key value. It lives in `libarrow_compute`, which `exec` already links, and needs no Acero.
- Arrow's grouped aggregate kernels (`hash_sum` and others) wrap integer sums at 64 bits, while antb1 must return
  DuckDB's exact 128-bit `SUM` and `AVG` (ADR 0004).
- Execution is single-threaded (ADR 0003), but parallel execution over row groups is the expected next step; the
  grouped design should not have to be rewritten for it.

## Decision

- **A new logical node, `GroupAggregate`**, separate from `Aggregate` (so the `COUNT(*)` to `RowCount` rewrite can
  never apply to it). It outputs the keys, then the aggregates; the binder puts a `Project` above it that restores the
  select-list order. The planning shape is `Scan <- [Filter] <- GroupAggregate <- Project <- [Limit]`.
- **Group ids from `arrow::compute::Grouper`; per-group accumulators of our own.** `GroupedAggregateState`
  (`src/exec/include/antb1/exec/grouped_aggregate_state.h`) mirrors each scalar state per group: exact `Int128`
  sums, checked HUGEINT additions, DOUBLE sums in row order, MIN and MAX with the NaN rule of divergence D10. Arrow's
  grouped kernels are not used.
- **DuckDB's key semantics.** DOUBLE keys are normalized before grouping (`-0.0` to `0.0`, every NaN to one NaN) so
  that they group as in DuckDB; the operator keeps each group's key as first seen for the output. Groups come out in
  the grouper's id order: deterministic, but not promised to users (SQL has no row order without `ORDER BY`).
- **Mergeable states.** `GroupedAggregateState::Merge(other, group_map)` folds another state's groups into this one
  through a group map. Partial states over parts of the input merged in input order give the single-pass result
  (DOUBLE sums up to the rounding of adding partial sums, which is still the same for the same part boundaries).
  Parallel execution can therefore aggregate row groups independently and merge, without new states. A sparse
  `MergeGroups(other, from, to)` folds only the listed groups (group `from[i]` of the other state into group
  `to[i]`), so that a partitioned merge folds each part's state into several states, a subset into each, in time
  proportional to the subset; `Merge(other, map)` is built on it. (Done by
  `PartGroupAggregateOperator` since ADR 0013's GROUP BY step: the merged groups follow the parts' groups in part
  order.)
- **In memory, no spilling.** All groups and their accumulators stay in memory; a query with more groups than memory
  fails (with a `memory` error under `--memory-limit`). Spilling is left for later, when a workload needs it.

## Consequences

- `GROUP BY` on plain columns (table columns, or aliases of plain select columns) answers exactly like DuckDB,
  checked against DuckDB by the `.slt` cases, the random differential test and the ClickBench ratchet.
- The test harness compares grouped results without regard to order, and a `LIMIT` without `ORDER BY` as a subset
  of DuckDB's unlimited answer (any rows are right).
- High-cardinality groupings (for example by a user id over the full ClickBench dataset) use memory proportional to
  the number of groups; this is measured, not bounded.
- `HAVING`, expressions as keys, `GROUP BY` positions and `ALL`, and `GROUPING SETS` stay outside the subset.
  (Later PRs added `GROUP BY` positions and `HAVING`, a `Filter` over the aggregation below the `Sort`, and
  expressions as keys with the scalar expressions of [ADR 0012](0012-scalar-expressions.md); `GROUP BY ALL` and
  `GROUPING SETS` are still rejected by the parser.)
