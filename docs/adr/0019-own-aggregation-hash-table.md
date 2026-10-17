# 19. An aggregation hash table of our own

Date: 2026-10-01

## Status

Proposed

- Update (2026-10-17): the status was `Proposed (deferred: the work is scheduled for a later optimization round)` and
  the title ended "scheduled for later". The maintainer has taken the work off that shelf, so both are stale and both
  are dropped; nothing else about the status changes. The scheduling note had also come to read as a gate on starting,
  which it never was — [ADR 0001](0001-record-architecture-decisions.md) gives the statuses no such meaning, and
  [ADR 0026](0026-join-performance.md) took ten merged perf PRs while `Proposed`.

## Context

- **Where the gap is:** after PRs #57-#62 the 43 ClickBench queries take 20.9 s at 128 threads, DuckDB 12.1 s. About
  3.9 s of the remaining gap is GROUP BY with many groups (Q16, Q18, Q27, Q32-Q34).
  - Update (2026-10-17, re-measured): on the official single-file dataset at 14 threads, best of three, antb1's
    geomean is 1.31× DuckDB's and 1.11× with each engine's process startup subtracted. The premise holds and the
    queries are the same ones, but the margin is smaller than the 2026-10-01 figures read, and those were taken on a
    100-partition dataset whose per-file footer cost inflated them. The grouped queries against DuckDB:
    Q16 1.78×, Q17 1.97×, Q32 1.72×, Q33 1.76×, Q34 1.71×, against a 1.31× geomean, so grouping is still where the
    gap concentrates. For scale, DataFusion — Arrow-based, and it replaced Arrow's grouper with its own — is 0.86× on
    the same run, so the gap is not a cost of building on Arrow.
  - Update (2026-10-17, the cost of an insert is not a constant): `bench/micro_bench.cc` now sweeps the group count
    (PR #160). Cost per row, one thread, by the table's group count:

    | groups | 1000 | 16 Ki | 256 Ki | 1 Mi | 4 Mi | 16 Mi |
    | --- | ---: | ---: | ---: | ---: | ---: | ---: |
    | `int64` | 2.70 ns | 3.17 | 4.79 | 7.75 | 23.44 | **29.76** |
    | `int64`+`int32` | 3.29 ns | 3.73 | 5.05 | 11.42 | 31.90 | **34.66** |
    | string | 6.88 ns | 7.70 | 9.83 | 17.52 | 41.80 | **54.12** |

    An insert costs 11× more at 16 M groups than at 1000, with the knee between 1 M and 4 M (L2 is 4 MiB per core on
    the host). Two consequences: the gates below top out a third of the way up the curve, below every grouped
    ClickBench query (18-100 M groups); and a string key costs 1.8× a fixed-width one at 16 M, which is why
    [ADR 0027](0027-string-views.md)'s S2 layout belongs to this table and is sized by this row.
  - Update (2026-10-17, the same cost reaches COUNT(DISTINCT)): a global `COUNT(DISTINCT)` of one column is planned as
    a GROUP BY of it ([ADR 0014](0014-two-level-aggregation.md) keeps that rewrite), so it pays this table's per-row
    cost too. ClickBench Q4 costs antb1 26 ns/row on one thread against DuckDB's 15, and a `sample` profile puts 57%
    of it inside the grouper. It is not materialization (building 17.6 M uniques is 10 ms of 349) and it is not
    parallel efficiency (54% at 14 threads, against DuckDB's 60%). Q4 and Q5 are 2.12× and 1.76× DuckDB, and both
    follow this table rather than a COUNT(DISTINCT) of their own.
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
- **A second prototype** (2026-10-16, on the branch `perf/group-hash-table`, not merged and with no PR):
  - **Design:** this ADR's SIMD tag probing — a byte of the hash per slot, eight tags to a 64-bit word matched
    arithmetically so one definition serves every architecture — with keys packed one row to a group, sizing from an
    expected group count, and memory charged to the budget.
  - **Correct:** nine differential cases against `arrow::compute::Grouper` — the same ids, group count and uniques
    across NULLs, several batches, six doublings, pre-sizing, normalized doubles, DATE and DECIMAL.
  - **Micro benchmark, one thread:** 0.29× at 1000 groups and 0.80× at 4 M for one `int64` key; 0.22× and 0.93× for
    two keys. The same shape as the first prototype's, *with* the tag probing this ADR names as the fix — so the
    probe was not what the first prototype got wrong. The ratios improve monotonically with the group count and were
    never measured above 4 M, which the curve above now shows is where the table's cost is still rising steeply.
  - **Why it was set aside, and the part of that reasoning which was wrong:** the branch records that the table has to
    hash with `KeyHashes`, since `Partition`, `RouteRows` and `PrefixHashes` agree through it, and that `KeyHashes`
    alone is 8.03 ms of the grouper's 11.1 ms — leaving 0.51 ms of the gate's budget for everything else.
    - Update (2026-10-17): the premise is false. `KeyHashes` is applied **per group**, not per row:
      `GroupTable::Partition` and `PrefixHashes` hash `GetUniques()`, and only `RouteRows` hashes rows, to send a
      part's unaggregated rows to the partition its group will land in. `Consume` never calls it — it delegates to
      the grouper, which hashes internally with a hash of its own. So partition agreement constrains only the hash
      over *uniques*, and a table's own probe hash is private to it and may be anything. That removes the arithmetic
      that closed the gate, but not the gap: the table was 38.3 ms against the grouper's 11.1 ms at 1000 groups, and
      subtracting all 8.03 ms of `KeyHashes` still leaves 30.3 ms. What is left is the row-at-a-time scalar loop —
      a buffer refill, a validity test and a `memcpy` per key column, a `memcmp` per probe, a builder append —
      against Arrow's vectorized encode-then-probe. **The next attempt's first job is a batch-oriented loop, not a
      cheaper hash.**

## Decision

- **We will build an aggregation hash table of our own**, replacing `arrow::compute::Grouper` in `exec::GroupTable`.
  This is the main lever left for GROUP BY.
- **Scheduled for a later optimization round:** it is a multi-PR project, and the current engine is correct and
  measured.
  - Update (2026-10-17): no longer scheduled for later; the maintainer has asked for the work. It is still a
    multi-PR project, and the staging is the one the Consequences below already set out — the micro benchmark first
    (now merged, PR #160), then the table, then [ADR 0027](0027-string-views.md)'s S2 key layout inside it.
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
  - Update (2026-10-17, the gates are restated; the Results gate is unchanged): 4 M was the largest count the
    benchmark could reach, not a chosen bound — it held `kGroupRows` rows and so could not exceed them. The counts
    above are kept, since 1000 is where both prototypes lost worst and a cached table is the common case, and
    **16 M is added**, which is where the engine's own grouped queries sit. The numbers to beat are the table in the
    Context above; 1.3× of the 16 M column is 22.9 ns/row for one `int64` key, 26.7 for two, and **41.6 for a
    string**. A table that cleared the old gates and not the 16 M column would not move Q32-Q34 at all, which is
    what the gates exist to prevent.
  - Update (2026-10-17, the end-to-end gate's baseline): "the GROUP BY queries" means Q16, Q17, Q18, Q32, Q33 and
    Q34, measured on the official single-file dataset at 14 threads, best of three, against DuckDB's unchanged binary
    in the same runs as the drift control. The 2026-10-01 baseline was a 100-partition dataset at 128 threads and is
    not comparable. Q4 and Q5 join the end-to-end gate as well, since a global COUNT(DISTINCT) runs through this
    table.

## Consequences

- **For now:** GROUP BY keeps `arrow::compute::Grouper`. The engine's other levers (partitioned merge, routing,
  partition top-N, dependent keys) stay as they are and carry over to the new table.
- **When the work starts:** it begins with that micro benchmark and the prototype's lessons:
  - avoid per-row branches in the probe loop;
  - keep the table's memory on the Arrow pool;
  - specialize per key shape.
- **Other gaps:** the string scan gap (Q20-Q22: the Parquet decoder copies every string; decoding into `BINARY_VIEW`
  saved only 10-16%) is a separate, later decision.
  - Update (2026-10-15): that decision is [ADR 0027](0027-string-views.md), which keeps the Parquet side deferred on
    this measurement and takes up the two sites where antb1 owns the copying instead. Its key layout — the size and a
    four-byte prefix in the first eight bytes, so that most comparisons never read the bytes — is the "specialize per
    key shape" of this ADR applied to a string key, and belongs to the table this ADR proposes rather than before it:
    `arrow::compute::Grouper` rejects view key types outright.
