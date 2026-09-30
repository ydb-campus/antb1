# 14. Two-level aggregation with skew-aware partitioning

Date: 2026-09-30

## Status

Proposed

## Context

- **Grouped `COUNT(DISTINCT x)`** (ClickBench Q8-Q11, Q13, Q22) runs in `PartGroupAggregateOperator` (ADR 0013).
  - The state `GroupedCountDistinct` holds an Arrow `Grouper` over the (group id, x) pairs.
  - The parallel merge splits the groups into 64 partitions by the hash of the keys K. All the pairs of one K land in
    one partition.
- **Skew serializes the merge.** Measured on the full data at 128 threads (Q8, before this change):
  - The merge took 1.6 s of 1.9 s.
  - The busiest partition merged about 6.7× the average work. One K with a large share of the pairs makes one
    partition work while the other 63 wait.
- **A per-part serial step:** a part's pairs are sorted by group once (`PairsByGroup`, about 0.4 s in total for Q8)
  under a `call_once` that the other 63 partition tasks wait on.
- **The global form was solved in #49** as a `GROUP BY x` with `COUNT` on top.
  - The same rewrite for the grouped form (`GROUP BY K, x`, then `GROUP BY K`) made Q11 and Q13 1.7-2.8× slower: the
    outer grouping was serial over up to every inner group.
- **Later work needs the same shape.** Derived tables, decorrelated subqueries (TPC-H) and several `COUNT(DISTINCT)`
  columns all come down to an outer GROUP BY K over an inner GROUP BY K ∪ X.

## Decision

- **One fused sink, `PartTwoLevelAggregateOperator`** (`src/exec/part_operators.h`), runs an aggregation by keys K (or
  none) whose calls include `COUNT(DISTINCT)`.
- **The inner level: one inner table per distinct column.** Each part builds its tables from the same batches.
  - One `GroupTable` over K ∪ {x_i} for each distinct column x_i, with no calls.
  - One plain `GroupTable` over K holding the other calls, when there are any.
  - Rows are never copied or padded. The Expand of other engines, which copies each row per distinct column with a
    tag, is not used.
  - The tables keep no first-seen keys: the outer level reads the groupers' normalized keys.
- **The heavy keys come from a sample.**
  - The sample is the first parts, in part order, until they hold `kTwoLevelSampleRows` (4M) rows by `part_rows`, or
    all parts. It is chosen from metadata, never by the thread count.
  - These parts run in parallel, followed by a barrier.
  - A Misra-Gries summary (`HeavyHitters`, 512 counters, the deterministic counterpart of SpaceSaving) counts the
    hash of K over the inner groups of the distinct tables. It is fed in part, table and group order; the hashes are
    computed on the workers.
  - A K is heavy when it may hold more than 1/128 of the sample's inner groups, which is more than half of a fair
    partition's share. The result is one sorted set of K hashes shared by every table.
- **The partition rule (`GroupTable::Partition(prefix, heavy)`) is a pure function of each group's keys.** So every
  part puts a group in the same partition.
  - A light K goes to partition `hash(K) % 64` in every table.
  - Table i spreads a heavy K by `hash(K, x_i) % 64`.
  - The plain table always uses `hash(K)`: after the per-part pre-aggregation it has one group per K per part.
  - A hash collision only makes an extra K heavy, which is still correct.
- **The inner merge is the existing one:** `MergePartition` of every table, parts in part order, a part's partitions
  in parallel.
  - The sample's tables are partitioned after the barrier, in parallel.
  - Later parts partition on their workers.
- **The outer level runs per partition, in parallel (`OuterGroups`).**
  - One grouper over K is fed from all the tables' groups, in table order.
  - `COUNT(DISTINCT x_i)` is the number of table i's groups whose x_i is not NULL.
  - The plain states fold in with `GroupedAggregateState::MergeGroups`.
  - The groups of a light K are complete, so their output rows are built in the same parallel step.
- **The heavy merge is serial**, in partition order, over at most about 64 × 128 groups. The counts add exactly,
  because each (K, x_i) is in exactly one partition of table i.
- **Output:** the light groups partition by partition, then the heavy groups. A global aggregation has one row even
  without input.
- **When it applies** (`TwoLevelAggregation`, decided by the physical planner; the logical plan and EXPLAIN are
  unchanged):
  - some `COUNT(DISTINCT)` of a column that is not a key;
  - no DOUBLE key: -0.0 and 0.0 group together, and the first-seen spelling would depend on the partitions;
  - every other call independent of its merge order: COUNT, integer SUM and AVG (Int128), DATE and TIMESTAMP AVG, and
    MIN and MAX of anything but DOUBLE.
  - A global aggregation whose calls are all `COUNT(DISTINCT)` of one column keeps the #49 rewrite. Other global ones
    run here with K empty: every inner group is heavy, and each table spreads by `hash(x_i)`.
- **Everything else keeps `PartGroupAggregateOperator` and `PartAggregateOperator`.**
- **The partitioned merge of every GROUP BY takes one Take per key column** and slices it per partition, instead of 64
  Takes. This matters for the many small parts.

## Consequences

- **Results are the same for any number of threads:** 64 partitions, a fixed hash, a sample chosen by metadata, and
  merges in part and partition order. They equal the serial operator's rows; the row order differs, as SQL allows.
- **Speed** (full data, 128 threads, paired A/B against main):
  - Q8 1.84×, Q9 1.79×, Q10 1.76×, Q11 1.66× and Q13 1.35× faster.
  - Q22 changes little: it is bound by the per-part scan and aggregation work, not the merge.
  - The single Take per key column also speeds up other parallel GROUP BYs, e.g. Q21 1.34×.
- **The sample is a barrier:** the parts after it wait for its slowest part.
  - If the heavy keys are not in the sample, balance suffers but correctness does not.
  - A key that is only heavy later still merges in one partition, as before.
- **Costs without skew:** the inner tables hash K ∪ {x_i} and the outer level hashes K again. For a high-cardinality
  VARCHAR K with no heavy key (Q13) this costs about as much as the old pair sort saved, so the gain there comes from
  the rest.
- **Memory:** the inner tables and outer groups are charged to the budget (their groupers through the pool, their
  vectors through reservations), and the part scheduler's OOM retry applies to both runs of parts.
- **Reuse:** derived tables and decorrelation can build an outer aggregation over an inner GROUP BY with the same
  operator. Persistent statistics (Count-Min sketches, HyperLogLog) come with joins and can replace the sample.

## Alternatives considered

- **Always hash (K, x), then exchange the partial outer groups by K:** no sample, but a second shuffle of every outer
  group. It costs the most exactly where there is no skew (high-cardinality K).
- **Expand:** one inner table over K, a tag and all the distinct columns, with each row copied per column. It
  multiplies the rows and pads with NULLs; separate tables hash only their own columns.
- **Blocking the workers until the heavy keys are known,** instead of a barrier: parts would not wait for the sample.
  Rejected: a worker blocked on the sample while the scheduler re-runs a sample part alone after an out-of-memory can
  deadlock the pool.
- **Exact counts of K over the sample:** memory grows with the sample's keys. The summary is bounded, deterministic and
  finds every K above the share.
- **Spreading the pairs inside `GroupedCountDistinct`:** it keeps the pair sort and cannot carry other calls. The two
  levels generalize to derived tables.
- **Persistent statistics:** none are kept yet; the sample needs none.
