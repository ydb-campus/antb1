# 38. Grouping sets: ROLLUP, CUBE and GROUPING SETS in two levels

Date: 2026-11-05

## Status

Accepted (2026-11-05, with ADR 0037's RU6). Proposed with RU1, which built the grammar; the plan node and its
operator are RU2, the forms RU3 (ROLLUP), RU5 (GROUPING SETS and `()`) and RU6 (CUBE), GROUPING() RU4.

## Context

- **ADR 0037's phase 5 answers every form of grouping sets** (its update of 2026-11-05, the maintainer's decision):
  ROLLUP, CUBE and GROUPING SETS as DuckDB accepts them, with GROUPING() and GROUPING_ID(). Nine of the 99 queries
  derived from TPC-DS use ROLLUP, of bare columns, with 3 to 9 sets; two of them call GROUPING(); none uses CUBE,
  GROUPING SETS, a DISTINCT aggregate or HAVING under a ROLLUP. Their aggregates are DECIMAL and integer SUM and AVG.
- **DuckDB 1.5.6** expands every form to a list of sets over the distinct grouping expressions, and answers it with one
  hash aggregate over the input that keeps a table per set.
- **The maintainer's direction:** two levels, the deepest aggregation first and every other set derived from it, with
  measurements against the alternatives.

## Decision

### The forms and their meaning (DuckDB's)

1. **Grammar.** A GROUP BY item is an expression, a composite `(a, b)`, the empty set `()`, `ROLLUP (...)` or
   `CUBE (...)` over expressions and composites, or `GROUPING SETS (...)` over any item, nested ones included.
   `rollup` and `cube` are names unless `(` follows. `ROLLUP ()`, `()` inside ROLLUP or CUBE, `GROUPING SETS ()` and a
   list after `GROUP BY ALL` are syntax errors, as in DuckDB. The parser keeps a list with any such item whole
   (`SelectStatement::grouping`); a plain list stays `group_by`.
2. **Expansion.** Each item gives a list of sets over the distinct grouping expressions, as DuckDB's
   `json_serialize_sql` shows them:
   - an expression one set of itself, a composite one set of its expressions, `()` the empty set;
   - `ROLLUP (e1, ..., en)` the prefixes, `()` through all n;
   - `CUBE (e1, ..., en)` every subset (2^n sets);
   - `GROUPING SETS (...)` the union of its items' lists;
   - the items of one GROUP BY combine as a cross product, so `x, ROLLUP (a, b)` is `(x), (x, a), (x, a, b)`.

   Duplicate sets stay: `ROLLUP (a, a)` emits the groups of `(a)` twice. More than 65535 sets is DuckDB's error.
3. **Rows.** A set's groups are a GROUP BY of its keys; a key the set lacks is NULL in its rows, padded at output, so
   a rolled-up NULL and a NULL of the data look alike (GROUPING() tells them apart). A `()` set gives one row even over
   no input, as a global aggregate does; a set of keys gives none.
4. **GROUPING(a, ...)** and its alias GROUPING_ID(): a BIGINT per row, bit n-1-i set when argument i is not in the
   row's set (the first argument the most significant bit). Allowed in the select list, HAVING, ORDER BY and CASE;
   an argument that is not a grouping expression, GROUPING in WHERE or inside an aggregate, and 64 or more arguments
   are errors, as in DuckDB.

### The plan node

1. **`GroupingSetsNode`**: the input, the distinct grouping keys (computed by the Compute below, as GROUP BY keys
   are), the sets as lists of key indices in DuckDB's order with duplicates, the aggregate calls, and one column per
   GROUPING call. Its output is the keys, then the calls, then the GROUPING columns. One set lowers to a
   GroupAggregateNode, `GROUP BY ()` to an AggregateNode, as DuckDB plans them. A new node rather than a field of
   GroupAggregateNode: rules that match a GroupAggregate by shape (ADR 0018's dependent keys, ADR 0032's placement,
   the partition top-N) must not apply to it unseen.

### Execution in two levels (the maintainer's direction)

1. **The first level.** Each set that no other set contains is aggregated once from the input, as a GROUP BY is today
   (ADR 0013, 0014): each part's rows into a partial table, then the parts merged partition by partition. For ROLLUP
   and CUBE that is the finest set alone, so each input row is hashed once. A GROUPING SETS list builds no table for a
   union of keys it does not list.
2. **The second level.** Every other set is merged from the merged groups of a set that contains it, with
   `GroupedAggregateState::MergeGroups` many-to-one (as `OuterGroups` re-aggregates by a prefix today), all of them in
   one pass over those groups with one barrier, not as a chain of sets each waiting for the one before. The merge
   reads partial states, so no finalization stands between the levels.
3. **Exactness.** Sets merge only where every call merges exactly: COUNT(*) and COUNT(x), integer and DECIMAL SUM and
   AVG (a 128-bit sum and a count), MIN and MAX of types other than DOUBLE, with no DOUBLE key. Otherwise each set is
   aggregated from the input rows into a table of its own, as DuckDB does: COUNT(DISTINCT) does not merge from finer
   groups, and a DOUBLE sum, the sign of a zero and the first spelling of a DOUBLE key depend on the merge order.
   Every set's rows then equal antb1's plain GROUP BY of that set.
4. **Errors** of every set (an overflow, a range error) are raised before the first row, as `CheckFinalize` does today.
5. **Guards.** PF11 partitions the first level by a key prefix that most sets share (a ROLLUP's leading keys with
    enough distinct values), so those sets merge inside each partition, in a chain with nothing between its steps,
    and only the rest (at least `()`) wait for every partition. RU6 merges CUBE's 2^n - 1 sets in batches that fit the
    memory budget. PF6 places a partial aggregate below a join under the node.

## Evidence

Synthetic data, 20 million rows, ROLLUP of 4 and 8 keys over five key profiles, from a finest set of 10 thousand
groups to one nearly as large as the input; each figure is the time against the finest set alone.

- **The engine's GroupAggregateOperator, one thread** (`bench/grouping_sets_bench.cc`, `antb1-bench-gsets`; parts of
  128Ki rows as the engine's row groups):

  | Every other set ... | Time |
  | --- | --- |
  | merged from the finest set's groups, each from the next finer set | 0.99 to 1.77 times |
  | merged from the finest set's groups, each from the finest set | 1.00 to 2.61 times |
  | aggregated from the input rows, one table per set (DuckDB's design) | 1.25 to 4.01 times |
  | merged from each part's partial groups | 1.08 to 4.14 times |
  | from the input rows copied once per set with a hidden set id | 1.81 to 6.47 times |

- **DuckDB 1.5.6 in SQL**, each set re-aggregated from a materialized set by a parallel aggregation of its own,
  against its one-pass operator: at one thread the chain took 1.00 to 2.13 times, each set from the finest set 1.02 to
  3.31, the operator 1.35 to 3.18. At 14 threads, where the finest set reduces the input, each set from the finest set
  took 1.07 and 1.11 times against the operator's 2.32 and 1.45; where it barely does, 1.35, 2.24 and 4.36 against
  1.34, 2.03 and 4.34 -- merging from about as many groups as input rows is as much work as aggregating the rows, and
  only a smaller source saves. The chain, each set from the next finer one, took 1.65, 2.63 and 3.62 times there: it
  won only where its later sets are much smaller (8 keys), and each of its steps waits for the one before, which PF11
  avoids by chaining inside a partition.
- RU2 measures its operator on the engine's parallel path against these and records the numbers as an update here
  (the RU2 update below).

## Consequences

- Each input row is hashed once for ROLLUP and CUBE where the calls merge exactly, which covers the nine queries.
- The node is new to every exhaustive visit of the plan (EXPLAIN, the optimizer's pruner, shared sub-plans, the
  physical planner), each answered in RU2.
- Tests per item: `.slt` records from DuckDB (with GROUPING() or `rowsort` breaking the tie between a data NULL and a
  rolled-up one), the generator's Features (`kRollup`, `kCube`, `kGroupingSets`, `kGroupingFunction`, pending until
  their items), and metamorphic relations: each set equals the plain GROUP BY over it (within the DOUBLE tolerance),
  ROLLUP equals the UNION ALL of its sets, the `()` row equals the ungrouped aggregate, and CUBE and ROLLUP equal their
  GROUPING SETS. DuckDB answers a lone `COUNT(*)` over `GROUPING SETS ((), ())` from statistics with one row; the
  generator keeps that shape out.

## Update (2026-11-05, RU2): the operator, measured

RU2 builds `GroupingSetsNode`, `GroupingSetsOperator` (serial) and `PartGroupingSetsOperator` (over a part pipeline).

- **What changed from the decision.**
  - **The exactness rule** also leaves out a HUGEINT sum and a DECIMAL sum wider than 18 digits: their 128-bit
    additions check for overflow one at a time, so the grouping of the additions decides whether the query fails.
    It admits AVG of a DATE or a TIMESTAMP, which merges exactly.
  - **The second level takes one of two ways per set**, decided from the root's largest partition, which makes the
    choice a pure function of the data:
    - **Reduce first:** where that partition's groups reduce at least 4 to 1 by the set's keys, and always for `()`,
      each partition first merges its groups into a table of the set's own, and the set's final partitions gather
      those.
    - **Direct:** otherwise, each partition only splits its groups by the hash of the set's keys, and the final
      partitions merge them straight from the root's tables, once each.

    The first build merged every set twice, through a table per partition. On the input-sized profiles it took
    about twice the time of one table per set (Wide8 on one thread: 269 ms against 134 ms over 300 thousand rows).
- **Measured** with `antb1-bench-gsets` (`BM_Operator`). The input is the Evidence's synthetic profiles, 20 million
  rows, as in-memory parts of 128Ki rows through the operators' own part pipelines, on Apple silicon (14 cores).
  Each figure is the time against the finest set alone (`PartGroupAggregateOperator`); every arm runs on an executor
  of that many threads:

  | Profile (groups of the finest set) | Two levels, 1 thread | One table per set, 1 thread | Two levels, 14 threads | One table per set, 14 threads |
  | --- | --- | --- | --- | --- |
  | hierarchy, 10 thousand | 1.03 | 2.63 | 1.20 | 2.37 |
  | hierarchy, 1 million | 1.06 | 1.56 | 1.08 | 1.42 |
  | hierarchy, 12.6 million | 1.28 | 1.30 | 1.26 | 1.20 |
  | 4 independent keys, 18.1 million | 1.72 | 1.89 | 1.82 | 1.84 |
  | 8 keys, 19.3 million | 3.05 | 3.75 | 3.26 | 3.66 |

  - **Where the finest set reduces the input,** two levels cost a few percent more than the finest set alone, and one
    table per set up to 2.6 times.
  - **Where it barely does,** the two ways are within 5 % of each other at 14 threads (two levels 5 % slower on the
    12.6-million hierarchy), as the Evidence predicted: merging from about as many groups as rows is as much work
    as aggregating the rows.
  - **Memory:** on the 8-key profile two levels peaked at 6.2 GiB against 4.6 to 5.7 GiB for one table per set. The
    root's tables stay until the sets merged from them are built. RU6 bounds the peak with CUBE's batches, and
    PF11 would merge the sets inside each partition.

## Update (2026-11-05, RU6): CUBE, and the second level in batches

- **CUBE** expands as DuckDB lists it: every subset of its elements, depth first (`CUBE (a, b, c)`: `()`, `(a)`,
  `(a, b)`, `(a, b, c)`, `(a, c)`, `(b)`, `(b, c)`, `(c)`), a composite one element, duplicates kept. Sixteen elements
  are DuckDB's "Maximum grouping set count of 65535 exceeded".
- **Batches.** `PartGroupingSetsOperator` merges the sets other than the roots in batches, in the order of their first
  entries, each batch emitted before the next is built:
  - The probes (RU2) also estimate each set's groups: the probe table's groups over the root partition's, times the
    root's groups, and at most those.
  - A batch takes sets while their estimated bytes fit half of the budget's free bytes, at the bytes of a root group
    (the budget's charge after the first level over the roots' groups), and at least one set. Without a limit it is
    every set at once, the RU2 operator. The estimate counts groups, not a table's fixed cost (64 tables a set at most,
    each with its key table's first buffers), which matters only for sets of few groups: a limit just above the roots
    can still run out in a batch of many small sets.
  - A root stays until the last set merged from it is built: its `NextChunk` moves out only its first-seen keys, so
    its key table and states still serve later batches.
  - A set's rows do not depend on its batch, so the result is the same under any limit and any number of threads.
- **Errors.** The roots' and the first batch's finalize errors come before the first row; a later batch's when it is
  built, after the earlier batches' rows. With the calls that merge exactly, a later set can fail only where its
  DECIMAL sum passes 38 digits, which needs about 10^20 input rows.
- **The serial `GroupingSetsOperator`** still builds every set before its first row: it runs only over inputs that are
  no part pipeline.

## Alternatives considered

- **A UNION ALL of one aggregation per set, by the binder.** Rejected: the plan is a tree, so each branch evaluates
  the input again (a 4-table join nine times for an 8-key ROLLUP), and antb1 shares no sub-plan under a UNION ALL
  today (PF5).
- **The input copied once per set with a hidden set id** (ADR 0037's survey). Rejected: the slowest measured, and
  ADR 0014 already rejected copying rows per aggregate.
- **One table per set fed from the input (DuckDB's design)** as the main path. Kept only as the fallback for calls
  that do not merge exactly: it hashes each row once per set.
- **Deriving the sets from each part's partial groups,** before the first level's merge. Rejected: a part barely
  reduces fine keys, so each set merges about as many partial groups as there are input rows.
- **A chain of sets across the whole input,** each waiting for the one before. Rejected as the general shape: in
  parallel each step adds a barrier and an exchange; it survives inside a partition (PF11).
