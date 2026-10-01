# 19. An aggregation hash table of our own, scheduled for later

Date: 2026-10-01

## Status

Proposed (deferred: the work is scheduled for a later optimization round)

## Context

- **Where the gap is:** after PRs #57-#62 the 43 ClickBench queries take 20.9 s at 128 threads, DuckDB 12.1 s. About
  3.9 s of the remaining gap is GROUP BY with many groups (Q16, Q18, Q27, Q32-Q34).
- **It is per-row cost:** on one thread antb1 needs about twice DuckDB's CPU for Q16, Q18 and Q33. A profile of Q16
  puts about 46% of the CPU in Arrow's generic grouper (`arrow::compute::Grouper`):

  | Part of the grouper | Share of Q16's CPU |
  | --- | ---: |
  | inserting new keys | 13% |
  | table doubling | 5% |
  | string hashing | 4% |
  | encoding and appending rows | 5% |
  | key comparisons | 4% |
  | probing | 4% |

  Much of a further 15% in memory copies comes from the same encoding.
- **Why it costs that much:** the grouper is generic.
  - It encodes every key into a row format of its own and hashes and compares in separate passes.
  - It decodes the keys back into arrays (`GetUniques`) when parts merge into partitions, where they are encoded again.
  - It cannot be pre-sized.
  - The aggregate states live apart from the keys, in `GroupedAggregateState` arrays indexed by group.
- **Smaller changes were measured and do not pay:**
  - Pre-sizing the merge-side tables would save at most about 1.8% of Q16's CPU, and Arrow has no API for it.
  - Capping the parts' tables made most GROUP BYs slower.
- **A first prototype of our own table** (2026-10-01; not committed):
  - **Design:** fixed-width keys packed into two words, one string key in an arena, linear probing with prefetch, and
    memory from the Arrow pool.
  - **Correct:** the same groups and keys as Arrow's grouper; all tests pass with it.
  - **Micro benchmark, one thread:** it was 1.17-1.30× faster with 1-4 M groups, but 0.22-0.70× with 1000 groups
    (cached). There the probe loop's mispredicted branches cost more than Arrow's SIMD probing, which checks 8 slot
    tags at once.
  - **End to end, 21 GROUP BY queries:** 2.3% faster in total. Q13 and Q18 were 8-11% faster, Q8, Q9 and Q15 7-10%
    slower.

## Decision

- **We will build an aggregation hash table of our own**, replacing `arrow::compute::Grouper` in `exec::GroupTable`.
  This is the main lever left for GROUP BY.
- **Scheduled for a later optimization round:** it is a multi-PR project, and the current engine is correct and
  measured.
- **The design direction, from the prototype and DuckDB:**
  - **SIMD tag probing:** a byte of the hash per slot, compared 8 or 16 at a time. It keeps cached tables (few
    groups) as fast as Arrow's.
  - **Keys and aggregate states in one row layout:** a probe finds the group's states in the same cache lines, and
    grouping and aggregating happen in one pass instead of an id array, then per-state scatter.
  - **Packed fixed-width keys:** compared as words. Strings in an arena, with a hash or prefix kept next to the slot.
  - **Merging partition tables without decoding keys:** a part's partition is hashed once and moved, not round-tripped
    through `GetUniques`.
  - **Sizing:** the part tables sized from `part_rows`, the partition tables from the groups merged so far.
  - **Memory:** from the query's pool and charged to the budget, with `OutOfMemory` (not exceptions) at the limit.
- **Gates before any PR replaces the grouper:**
  - **Micro benchmark, one thread:** at least 1.3× faster than Arrow's grouper on every shape (`int64`,
    `int64`+`int32`, string, `int64`+string) at 1000, 1 M and 4 M groups.
  - **End to end:** at least 10% faster on the GROUP BY queries, with no query more than 3% slower.
  - **Results:** identical, including first-seen keys, DOUBLE normalization, NULL keys and every test.

## Consequences

- **For now:** GROUP BY keeps `arrow::compute::Grouper`. The engine's other levers (partitioned merge, routing,
  partition top-N, dependent keys) stay as they are and carry over to the new table.
- **When the work starts:** it begins with that micro benchmark and the prototype's lessons:
  - avoid per-row branches in the probe loop;
  - keep the table's memory on the Arrow pool;
  - specialize per key shape.
- **Other gaps:** the string scan gap (Q20-Q22: the Parquet decoder copies every string; decoding into `BINARY_VIEW`
  saved only 10-16%) is a separate, later decision.
