# 18. GROUP BY keys that are functions of other keys

Date: 2026-09-30

## Status

Proposed

## Context

- **The gap:** a GROUP BY whose keys are one column and arithmetic expressions of it (Q35 on the full ClickBench
  data: four keys, about 9.8 M groups) took 0.85 s, against 0.23 s in DuckDB and 0.12 s in ClickHouse.
- **Where the time goes:** `explain --analyze` shows about 48 s of its 59.5 s of part CPU in hashing and aggregating
  the four keys. Grouping by the one column alone takes 0.37 s.
- **Why the extra keys add nothing:** a key computed only from other keys has one value per combination of theirs.
  It can never split a group or merge two, so grouping by the other keys gives the same groups. ClickHouse drops
  such keys (injective functions of other keys in GROUP BY).

## Decision

- **An optimizer rule** (`plan::Optimize`, after COUNT(*) → RowCount, before Limit below Project and pruning). A key
  of a `GroupAggregate` is *dependent* when all of these hold:
  - it is an expression of the `Compute` directly below the `GroupAggregate` (where the binder puts key
    expressions) and reads at least one column;
  - every column it reads is a key passed through that `Compute`;
  - none of those keys is DOUBLE. A DOUBLE key groups -0.0 with 0.0 and every NaN together, and a function of it
    (`1 / d`) could tell those apart.
- **The rewrite:**
  - the `GroupAggregate` keeps the other keys;
  - a `Compute` above it evaluates the dependent keys from the kept keys, once per group;
  - a `Project` restores the original output order (the keys, then the calls).
  - The nodes above keep their column indices, and pruning then drops the dependent expressions from the `Compute`
    below, so they are no longer computed per row.
- **Exactness:**
  - *Groups:* the same, as above.
  - *NULL:* a dependent key over a NULL key is computed from that NULL.
  - *Values:* computed from the group's first-seen values of the kept keys. These are not DOUBLE, so every row of
    the group has those values.
  - *Errors:* the expression sees the same set of key values either way. Nothing sits between that `Compute` and the
    `GroupAggregate`, so an overflow happens for the same input.
- **Not under a `LIMIT` without a `Sort` in between.** The nodes above would stop reading after the rows they need,
  so the dependent keys would be computed for some groups only, and an overflow in another group would no longer
  fail the query. The rule walks down from the root and skips a `GroupAggregate` when a `Limit` is above it with no
  `Sort` in between. A `Sort` reads every row, so `ORDER BY ... LIMIT` keeps the rewrite.
- **Not done: computing the dependent keys above the Sort and Limit.** It would evaluate them for the rows kept only,
  but an overflow in a group outside the result would then no longer fail the query, while DuckDB fails it.

## Consequences

- **Q35:** 0.85 s → 0.68 s. The 9.8 M groups are hashed by one key instead of four.
- **The recomputation is still serial:** about 0.3 s of Q35 is the `Compute` over the groups, one thread over the
  aggregate's output. A parallel `Compute` over materialized input, evaluating its batches on the executor in order,
  would remove most of it. That is a separate change.
- **EXPLAIN** shows the rewrite: `GroupAggregate keys=[k]` under a `Compute` of the dependent keys and a `Project`.
- **Tests:**
  - optimizer tests: the rewrite and the cases it leaves alone;
  - `tests/slt/cases/groupby/dependent_keys.slt` against DuckDB: NULL keys, VARCHAR, HAVING and ORDER BY on a
    dependent key, COUNT(DISTINCT), parallel parts, a DOUBLE key, an overflow (also under a LIMIT, with and without
    ORDER BY);
  - an EXPLAIN golden.
