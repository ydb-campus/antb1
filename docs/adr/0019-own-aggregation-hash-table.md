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
  - Update (2026-10-29, **step 0 chose the loop**; #253): before any engine change,
    [the step-0 loop](https://github.com/ydb-campus/antb1/blob/2a0b083/bench/group_loops.h)
    benchmarked loops of our own against the grouper under a `MemoryBudget` and picked the fixed-width design:
    - **A chunked pass, the fused pass the Consequences below ask for.** Each 256 rows get one room check and one
      hash pass, then the probes. Most probes prefetch the buckets of the row 8 places on; the last 8 of a chunk do
      not. Apart from the probe, the only per-row branch is that predictable prefetch check. It needed no SIMD
      instructions: the tag matching is SWAR, plain 64-bit arithmetic, so the pass keeps the one portable arithmetic
      definition the 2026-10-18 update below expected a vectorized pass to give up.
    - **A fixed-seed one-multiply hash** (a folded multiply, the scheme of foldhash's fast tier, with hex digits of pi
      as its constants), with positions from hash bits 32 and up and the tag from the top 7 bits.
    - **SIMD tag probing as SWAR:** eight 7-bit tags per 64-bit word, control bytes mirrored past the end, and a
      triangular probe. This is hashbrown's design, written from its description, with no code copied (ADR 0034).
    - **A 4-byte slot holding the group id alone,** with the keys in an id-ordered array. This replaces "packed
      fixed-width keys … compared as words" in the slot: the key beside the id, or a 32- or 64-bit hash beside it
      (8-24-byte slots), each lost.
    - **Sizing from the table's own data.** It replaces "part tables sized from `part_rows`": that bullet would charge
      slots for every unfiltered row of every part in flight. A table estimates its group count after its second
      batch from the share of that batch's rows that hit its first batch's groups (a capture-recapture estimate), and
      reserves 9/8 of it. It fails small: below 64 hits it reserves nothing, it never reserves more than 8× the
      groups seen, it never falls back to a row count, and a budget that refuses the reservation leaves the table
      growing as before. Partition tables size from the groups merged so far, as before.
    - **How the table grows is a template parameter** in step 0's code, with no default: walking the old slots in
      order and rehashing each key, or re-placing the groups in id order. Step 1 defaults it to slot order. The
      choice moves no id, so it cannot change a result; the measurements are in the Consequences.
    - **Memory: Arrow pool buffers,** as the bullet above says (decided 2026-10-29). Step 1 holds the control bytes,
      slots and keys in buffers from the query's pool (the `MemoryBudget`), behind a small typed wrapper:
      - every allocation is charged before it happens, with no separate bookkeeping to get wrong;
      - running out of memory is an `OutOfMemory` `Status`, never `std::bad_alloc`;
      - large arrays come from mimalloc instead of glibc's fresh pages;
      - a chunk's keys can leave the table as a slice, without a copy.

      Small per-chunk scratch stays in fixed-size members. Step 0's `std::vector`s with a `MemoryReservation`
      charged before each allocation (old and new buffers both, never per insert) were a benchmark convenience. The
      choice is made; step 1 only reports how the pool compares with vectors, and a large loss would come back here.
    - **Keys and aggregate states in one row layout** is not part of steps 1-4. It is reconsidered at step 5 (the
      states), with these numbers.
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
  - Update (2026-10-18, **the counts are wrong in both directions**): 4 M and 16 M, added the day before, measure
    tables the engine cannot build — the partition count and a part bounded by its row group put every one of them in
    10⁴-10⁶ groups (see the Consequences below). A table has to win **in that band**, so the counts to sweep are
    16 Ki, 256 Ki and 1 Mi, and the targets are the grouper's costs there rather than the 16 M column's. Keeping
    1000 is still right, and 4 M is worth reporting as the band the engine would reach if
    [ADR 0013](0013-parallel-execution.md)'s partition count ever changed — but it gates nothing today.
  - Update (2026-10-17, the end-to-end gate's baseline): "the GROUP BY queries" means Q16, Q17, Q18, Q32, Q33 and
    Q34, measured on the official single-file dataset at 14 threads, best of three, against DuckDB's unchanged binary
    in the same runs as the drift control. The 2026-10-01 baseline was a 100-partition dataset at 128 threads and is
    not comparable. Q4 and Q5 join the end-to-end gate as well, since a global COUNT(DISTINCT) runs through this
    table.
  - Update (2026-10-29, **gates per step**): the table replaces the grouper one key shape at a time — step 1 one or
    two fixed-width keys, step 3 one VARCHAR key, step 4 a fixed-width key with a VARCHAR — and the grouper stays as
    the fallback and the test oracle. Each step is gated on its own shapes; a single gate over every shape would
    block step 1 on string keys it does not touch. A key table alone has room for about 9% of Q32's CPU and 4.5% of
    Q18's, so those two are reported rather than gated.
    - **Micro, one thread:** at least 1.3× **the budgeted grouper of the same run and the same ids** at 1000,
      16 Ki, 256 Ki and 1 Mi groups, with 4 Mi reported. The inputs include random ids (groups repeat early, as in
      real data) beside the permutation. The absolute targets above are void: on the benchmark host (2 × Xeon Gold
      6338) the grouper itself takes about 4× the ns/row of the Context table above, which measured it on the earlier
      host (10.55 against 2.70 at 1000 groups for one `int64`). The static build and the conda build of Arrow agree
      (10.55 and 10.75), so the gap is the host, not the build.
    - **End to end:** step 1 on Q4 for one key, and on Q30 and Q31 for two (the condition of the two-key go in the
      Consequences), with Q15, Q35 and Q32 reported; step 3 on Q5, Q33 and Q34; step 4 on Q16 and Q17, with Q18
      reported.
    - **The end-to-end terms** (decided 2026-10-29):
      - every gated query at least 10% faster, not a total or a geomean;
      - no query of all 43 more than 3% slower;
      - the benchmark host at 8 and 32 threads on one socket, best of three, both arms from one static build, with
        DuckDB as the drift control.

      14 threads matched the earlier host. At 64 threads this host swung 3-5× between rounds of one binary on a quiet
      machine, so 64 gates nothing.
    - **Results** stay as above: identical, first-seen keys, DOUBLE normalization, NULL keys and every test.

## Consequences

- Update (2026-10-18, **the table was finished and measured in the pipeline; it reaches parity, not a win**): the
  second prototype was taken through the work the updates above called for, then put behind
  `arrow::compute::Grouper` in `exec::GroupTable` and measured on the queries themselves. What it cost and what it
  bought, in order:
  - **A batch-oriented loop** (pack every row of the batch one key column at a time, then hash the packed rows, then
    probe) and **its own probe hash**, which the update above establishes it is free to have. Together these took one
    `int64` key from 0.29× the grouper to 0.43× at a thousand groups, and from 0.80× to 1.09× at 4 M.
  - **Prefetching the bucket eight rows ahead**, the one element ADR 0019's *first* prototype had and the second
    dropped. At 16 M groups this alone took 44.16 ns a row to 20.14 — the table's whole advantage at high
    cardinality, from one hint. With it, and with rows padded to whole words, the micro benchmark reads: 0.43× and
    0.39× at a thousand groups, 0.73× and 0.86× at 1 M, **1.38× and 1.59× at 4 M, 1.44× and 1.56× at 16 M** for one
    and two fixed-width keys.
  - **What the gates could not see.** Those counts do not occur. `GroupTable::kPartitions` was 64 when this was
    measured and is 32 since #167 (`3132647`), which halves the partition count without moving the band this
    paragraph is about; the two-level operator keeps its own 64 (`src/exec/part_operators.cc`). A part's table
    is bounded by its rows, so the largest table this engine builds is a partition table of q32 at **1,562,460
    groups**, and a part table is at most a row group — 442,467 rows on the official dataset. Every table the engine
    constructs falls in the 10⁴-10⁶ band, which is where the micro benchmark has this design at 0.46-0.88×. **The
    4 M and 16 M columns added on 2026-10-17 measure tables the architecture prevents**, and they should be
    10⁴-10⁶ instead; that correction is mine to own, since I argued for those columns.
  - **End to end, with the table carrying every fixed-width key** (18 of the 43 queries; a string key still falls
    back, so q33, q34 and the rest of ADR 0027's share are untouched): **7.74 s to 35.65 s, +360%**. q32 went 1.58 s
    to 13.55 s.
  - **All of which was one flaw, not the table.** `Reserve(group + 1)` charges the `MemoryBudget` **once per
    inserted group**, so q32's hundred million groups are a hundred million contended updates across fourteen
    threads. The micro benchmark passed no budget and so never paid it — the whole distance between "1.4× faster"
    and "8.6× slower". With the budget bypassed the same binary gives **1.56 s against the grouper's 1.58 s on q32,
    0.43 against 0.43 on Q4, 0.41 against 0.43 on Q8**: parity. A table of this kind must charge its budget when its
    capacity changes, never per group.
  - **So the decision's premise does not hold.** "Replacing `arrow::compute::Grouper` ... the main lever left for
    GROUP BY" assumed a table of our own would be faster at the cardinalities the engine meets. Once its accounting
    is fixed it is level with the grouper there, and it still cannot hold a string or a BOOLEAN key. The levers that
    did pay over the same period were elsewhere: skipping top-N parts by their statistics (#159), and before it the
    join work of [ADR 0026](0026-join-performance.md).
  - **What would change the answer**, neither of which is a hash table: a **fused, vectorized** pass, which is how
    the grouper wins below the cache knee — its whole operation costs 2.77 ns a row at a thousand groups, less than
    this table's packing and hashing alone, before a single probe — and which means giving up the one portable
    arithmetic definition this design chose; or **fewer partitions**, so that large tables exist at all, which is
    [ADR 0013](0013-parallel-execution.md)'s decision rather than this one's.
  - Correctness, for the record: the table answers identically once wired in — 2733 tests with it in the pipeline —
    but it took a second round to get there. Nine differential cases against the grouper passed a version that
    failed **26** integration tests, because a reused pack buffer kept a row position's NULL bit from an earlier
    batch and collapsed distinct keys into the NULL group. A differential test over single batches cannot see that;
    the engine's own suite saw it immediately.
- Update (2026-10-29, **step 0: a go for one fixed-width key, a conditional go for two**; #253): the loop chosen in
  the Decision above, measured against the budgeted grouper of the same run and the same ids. Below is the minimum
  speed-up over 1000 to 1 Mi groups, with the size estimate, on random ids (where groups repeat early) and on the
  permutation (where no id repeats early, so the estimate cannot fire):

  | | one `int64`, random | one `int64`, permuted | `int64` + `int32`, random | `int64` + `int32`, permuted |
  | --- | --: | --: | --: | --: |
  | Xeon Gold 6338, growth in slot order | 1.60× | 1.32× | 1.50× | **1.25×** |
  | Xeon Gold 6338, growth in id order | 1.63× | 1.11× | 1.50× | 1.11× |
  | Apple M4 Pro, growth in slot order | 1.34× | 1.58× | 1.45× | 1.46× |
  | Apple M4 Pro, growth in id order | 1.35× | 1.63× | 1.47× | 1.48× |

  - **The verdict** (the maintainer's, 2026-10-29): one `int64` passes every cell with slot-order growth, so it goes.
    `int64` + `int32` passes on random ids on both hosts but reaches only 1.25× on the permutation at 1 Mi on the
    Xeon, below the 1.3× gate. It is a **conditional go**: step 1 ships both shapes, records that cell as a known
    limit, and gates the two-key shape end to end on Q30 and Q31 as well (see the gates above). Q32 stays
    reported, since a key table alone has room for about 9% of its CPU.

  - **What changed since 2026-10-18's parity:**
    - the chunked pass took the small tables from 0.43× to about 2×;
    - charging the budget on capacity changes only removed the +360%;
    - the estimate took 1 Mi groups from 1.13× to 1.78× (one `int64`, Xeon), because growth was half the time there:
      pre-sized, the same loop reaches 2.1-3.1× at 1 Mi.
  - **The growth choice splits by host:**
    - with ids that never repeat early (the permutation, standing in for sorted or clustered keys), the estimate
      cannot fire. Then, at 4 Mi groups, id-order growth falls to 0.79-0.87× on the Xeon, while slot-order growth
      holds 1.07-1.10×;
    - on the M4 Pro, id-order growth is 10-48% faster from 256 Ki groups up, and slot-order growth reaches 1.14-1.17×
      at 4 Mi.

    Slot order is the default because its worst case on either host is 1.07×. Step 1 decides it end to end.
  - **What lost:** one row at a time (DataFusion's shape), the key or a 32- or 64-bit hash beside the id (a 32-bit
    hash beside the id failed the gate on the M4 Pro), prefetching 16 or 32 rows ahead, a load factor of 1/2, and 4×
    growth.
  - **Known limit:** `int64` + `int32` with ids that never repeat early (sorted or clustered keys) reaches 1.25× at
    1 Mi on the Xeon with slot-order growth, and 1.11× with id-order growth.
  - **Arrow's SIMD:** the Xeon's grouper runs AVX2 kernels, which this ADR's original host did not. They make it
    1.6-1.8× *slower* at 1000-16 Ki groups and about 10% faster from 1 Mi up. But switching them off made eight
    GROUP BY queries 1-2% slower in total at 1-32 threads, so that is no lever, and the baseline stays the grouper as
    the engine runs it.
  - **Not covered by step 0:** NULL keys, DOUBLE normalization, string keys, threads sharing a cache, and the engine
    end to end. Step 1 wires the table into `GroupTable` behind an exec-private interface, under plan mode, and
    replaces [the step-0 loop](https://github.com/ydb-campus/antb1/blob/2a0b083/bench/group_loops.h) with the engine's table.
- **For now:** GROUP BY keeps `arrow::compute::Grouper`. The engine's other levers (partitioned merge, routing,
  partition top-N, dependent keys) stay as they are and carry over to the new table.
- **When the work starts:** it begins with that micro benchmark and the prototype's lessons:
  - avoid per-row branches in the probe loop;
  - keep the table's memory on the Arrow pool;
  - specialize per key shape.
  - Update (2026-10-29): step 0 is done (#253) and went. The grouper now serves every key shape without a table of
    our own and is the oracle in tests. Step 1 comes next, under the per-step gates above, and the lessons still
    hold: no per-row branches in the probe loop; keep the table's memory on the Arrow pool; charge it on capacity
    changes, before allocating; specialize per key shape; and re-measure against the grouper in the same run.
- **Other gaps:** the string scan gap (Q20-Q22: the Parquet decoder copies every string; decoding into `BINARY_VIEW`
  saved only 10-16%) is a separate, later decision.
  - Update (2026-10-15): that decision is [ADR 0027](0027-string-views.md), which keeps the Parquet side deferred on
    this measurement and takes up the two sites where antb1 owns the copying instead. Its key layout — the size and a
    four-byte prefix in the first eight bytes, so that most comparisons never read the bytes — is the "specialize per
    key shape" of this ADR applied to a string key, and belongs to the table this ADR proposes rather than before it:
    `arrow::compute::Grouper` rejects view key types outright.

## Amendment (2026-11-19): generic grouping and partitioned local tables

This amendment supplies the implementation sequence requested after the local-grouping attribution experiment.
It takes precedence over the earlier implementation order and the claim that engine tables cannot exceed the
10⁴-10⁶ group band. The historical measurements and failure reports above remain evidence, not current limits.
The status remains Proposed. This documentation-only PR must merge before implementation starts; its merge
allows starting the steps below, not merging their code without validation and human review.

### What the experiment established

A narrow prototype supports one BIGINT key and one narrow DECIMAL SUM. It computes a content hash once per row,
updates a checked numeric SUM directly, keeps hashes for growth and merging, and hands local tables to the existing
32 merge lanes. It has not established a generic replacement for KeyTable or GroupTable.

The attribution test compared three modes in one static Release executable: the original Arrow path with its
adaptive local/raw routing; the numeric backend with one local table per part and a final stable partition of group
IDs; and the same numeric backend with 32 local tables populated directly. Both numeric modes aggregate every row
locally, share the global tables, hash algorithm, state arithmetic and merge order, and avoid Arrow key round trips.
The experiment used partitioned DECIMAL data, 16 physical cores pinned on one socket, one warm-up per mode and
three measured rounds with rotating mode order. All 24 native executions, including warm-ups, matched saved
DuckDB references without background-load flags. Operator profiling ran separately after timing.

TPC-H-derived Q18 native wall medians, in seconds:

| Scale factor | Original path | One numeric local table | 32 local tables | Reduction from one to 32 |
| --- | ---: | ---: | ---: | ---: |
| 100 | 5.855 | 5.163 | 5.010 | 2.96% |
| 1000 | 70.406 | 51.241 | 49.689 | 3.03% |

The incremental CPU reduction was 3.60% and 3.41%, respectively. At SF1000 the separate profile's accumulated
merge elapsed fell 16.3%, dispatch elapsed rose 17.9%, and one-table ID partitioning consumed 3.917 thread-seconds.
Those timers overlap; they are neither CPU samples nor additive wall times. Peak query-tracked capacity fell from
126.886 GB to 126.294 GB between numeric modes. This is a modest additional layout benefit, not evidence that
partitioning explains the full improvement over the original path. The larger original-to-numeric difference
combines a different key backend, fused SUM and always-local aggregation; those effects remain to be separated.
Three repetitions and disjoint measured ranges do not establish a general significance or workload-wide claim.

This workload is derived from the TPC-H Benchmark and is not comparable to published TPC-H Benchmark results, as
this implementation does not comply with all requirements of the TPC-H Benchmark.

The prototype and attribution control are preserved at `dbcf207` on `research/partitioned-local-grouping` and as
local experimental changes on `research/local-grouping-attribution`, respectively. The latter's ignored report is
`build/static/bench-results/grouping-attribution-20261119T190055Z/report.md`; it is local evidence, not a portable
repository dependency. Raw workload inputs, plans and answers stay outside version control. Neither prototype is
production-ready. The NDV/sparsity experiment remains on hold and is not part of this plan.

### Existing work to reuse

`Group-by-hash-table-handoff.md` (2026-10-24, local investigation document) records the earlier DataFusion-inspired
attempts. Its referenced DataFusion source checkout is no longer available locally, so this review verifies the
antb1 branches and this ADR, not a fresh upstream comparison. DataFusion's separation of key storage, group IDs and
accumulators is a design reference; its loop shape or arithmetic is not automatically suitable for this engine.

- `efa7ec4` on `perf/group-hash-table` is the early tag-probing prototype. `7862d18` contains the completed older
  adapter; `f9879e3` is on an older base. Their lessons include stale NULL bits in reused packing scratch,
  integration failures missed by small differential tests, and the severe cost of per-insert budget accounting.
- `3b1f96e` on `perf/fixed-key-table` is newer than that handoff and is the starting reuse candidate. It implements
  one/two fixed-width keys, 256-row chunks, eight-row-ahead prefetch, SWAR tags, four-byte group-ID slots,
  pool-backed storage, slot-order growth and bounded capture-recapture sizing. Reuse its code and tests after
  review against current main rather than restart the scalar probe design.
- The newer branch lacks today's non-inserting `KeyTable::Lookup` and predates the merged packed-string backend.
  It still returns row IDs for separate aggregate updates and builds Arrow uniques by copying keys. It does not
  supply fused aggregate updates or direct merging of stored keys/states. Its growth rehashes stored keys rather
  than retaining the prototype's full routing hash. These are integration and measurement tasks, not reasons for
  an unconditional cherry-pick. No historical branch was rebuilt or timed in this attribution campaign.

### Step 1 candidate measurements (2026-11-19)

The fixed-width implementation from `3b1f96e` has been adapted to current KeyTable, including non-inserting
Lookup, pool-allocation failure/retry tests, NULL-payload safety and the existing packed-string backend. The
microbenchmarks now use that implementation instead of a duplicate step-0 loop. These measurements used the
enabled research candidate against main `2a0b083`; the implementation PR keeps automatic selection on the
existing backends.

On the Xeon Gold 6338, pinned to one physical core, the final nine-repetition micro grid (0.8 seconds minimum per
sample, randomly interleaved) met the 1.3x gate at every required count and distribution. Speed-ups ranged from
1.369x to 2.112x over the budgeted grouper. At 4 Mi groups, reported rather than gated, the minimum was 1.323x.
The earlier three-repetition grid had one borderline failure, 1.287x for one BIGINT at 1 Mi groups; a longer isolated
repeat reached 1.313x. All three runs are retained; the final grid supplies a consistent measurement depth rather
than replacing a failed observation silently.

The complete-query gates did not pass. Official single-file ClickBench input, 8/32 physical cores on one socket,
default NUMA placement, no cache dropping, one warm-up and three rotating interleaved measurements per engine,
with an unchanged DuckDB control, produced these native execution medians:

| Query | Threads | Main seconds | Enabled candidate seconds | Wall reduction |
| --- | ---: | ---: | ---: | ---: |
| Q4 | 8 | 1.113426 | 0.676172 | 39.27% |
| Q4 | 32 | 0.516126 | 0.547976 | -6.17% |
| Q30 | 8 | 1.106680 | 0.997589 | 9.86% |
| Q30 | 32 | 0.897832 | 0.908398 | -1.18% |
| Q31 | 8 | 1.378881 | 1.208633 | 12.35% |
| Q31 | 32 | 1.234620 | 1.209247 | 2.06% |

All 72 executions completed without background-load flags. Q4's measured 32-thread ranges overlap: main
0.493393-0.590320 seconds, candidate 0.459509-0.557583 seconds. Its process CPU median fell from 11.639 to 9.653
seconds there, despite the higher wall median. Three samples do not establish an intrinsic regression; they also
do not justify accepting the candidate. The required 32-thread gains are absent for both key shapes, under either
these medians or the best-of-three view.

Separate cycle profiles of Q4 put about 18% of the candidate's cycles in rehashing and 36-38% in Consume, at both
thread counts. They do not show a prominent budget-accounting hotspot or isolate the cause of the scaling loss.
Stored-key merging, fused state updates and a different growth policy have not been demonstrated as fixes by
these profiles. They remain independently measured work under the sequence below.

Consequently, step 1 adds a tested candidate and explicit private test/benchmark selection, while `kAuto` keeps
the Grouper for fixed-width keys and the existing packed-string path for eligible strings. It introduces no
production override or default activation. Activation needs a subsequent PR meeting the existing gates; neither
the gates nor this ADR's status are weakened. The full all-query timing campaign is not an activation proof for
this PR: the targeted gates already stopped that decision. The enabled source and private evidence remain on
`research/fixed-key-table-autoselection` and under `build/static/bench-results/fixed-key-integration` locally.

### Target architecture and invariants

Keep these components private to `exec`, behind the current KeyTable and GroupTable contracts. These names
describe responsibilities; they do not require a public API or one universal representation.

- **Key layout and storage.** Select a schema-specific layout once: typed fixed-width words, owned string bytes,
  or a column-oriented composite layout. The design must represent any currently supported key type and number
  of columns, including mixed fixed/string keys; unsupported optimized shapes retain the Arrow fallback. Start
  with the one/two-key fixed layouts already implemented. Own keys only when a distinct group is inserted,
  preserve dense stable IDs through growth, and retain any first-seen representation needed for output separately
  from normalized identity. NULL is a grouping value, including per-column NULLs in composite keys. DOUBLE
  normalization, signed zero, NaNs and first-seen spelling remain as documented today.
- **Group index.** Batch hash, probe and compare without per-row virtual dispatch, allocation or string copies.
  Equal keys must remain equal across array offsets, batches and physical representations. Keep the non-inserting
  Lookup contract, group-ID bounds and explicit error propagation. Consider fused probe/update where useful,
  while retaining an ID-producing adapter for sets, windows and existing callers.
- **Aggregate program.** Resolve types and operations once per query; update typed state columns in bounded
  batches from group references. A reference identifies a local partition and group; it does not carry a copied
  key. Dispatch per aggregate/type outside the row loop. Preserve current SUM/AVG Int128 accumulation, DECIMAL
  width/scale and overflow rules, NULL/count behavior, MIN/MAX comparisons, empty-input results and finalization
  errors. State layout is measured separately: do not assume that interleaving all keys and states in one row
  beats separate typed arrays for every aggregate family.
- **Partition policy.** Ordinary grouping retains its fixed 32 lanes and part-order merge. Partition count is not
  derived from worker count and does not become a tuning knob. Preserve the separate two-level distinct policy,
  prefix hashes and heavy-prefix routing; do not replace those with ordinary all-key routing. Do not add a full
  row shuffle before local aggregation as the default: partitioned local tables dispatch rows inside each worker.
- **Immutable handoff.** After a part finishes, freeze its owned keys, cached hashes and aggregate states. Each
  lane reads only its partition and merges stored keys/states directly, without GetUniques/decode/re-encode.
  Materialize any fallback Arrow scratch once before sharing; Arrow Grouper's mutable GetUniques scratch must
  never be called concurrently. Release partition-local storage when its last reader finishes. Shared backing
  buffers remain owned and charged until the last dependent reader releases them.

A worker processes a bounded chunk of selected source rows without first gathering all key or payload columns.
It hashes the normalized key values, computes each lane, probes that lane's local index, appends owned keys only
for new groups, and records `(lane, group_id)` in reusable budgeted scratch when the aggregate program needs it.
Typed aggregate loops then read the original input buffers through the selection and those references. An eligible
fused specialization can combine probe and update without the reference array. Use either direct row dispatch or
stable per-lane row-index buckets according to measurement; neither requires moving the complete payload. Hash
caches and scratch are bounded by chunk or group capacity, not total input rows. At part completion, lanes merge
immutable stored keys and states; construct Arrow output only at a consumer/output boundary that requires it.
Result buffers must retain the necessary query/pool owner after operators close, not merely borrowed pointers.

Probe hashes and routing hashes are different contracts. The probe hash is private; routing must match ordinary
KeyHashes semantics wherever old/new tables or routed raw rows meet. Reuse a content/routing hash only after
proving the bit domains: `hash & 31` chooses a lane, so a table confined to that lane must not use those constant
low bits for its own buckets. The prototype shifts by five bits; a private independent probe hash is another valid
choice. Cache any prefix/full hashes needed by two-level distinct separately. Whether caching full hashes beats
rehashing during growth must be measured, including retained memory, rather than copied from either prototype.

All persistent arrays, retained bytes, hash caches, scratch, ID arrays and ownership metadata use the query's pool
or existing budgeted containers. Charge at capacity changes before allocation, including overlap during growth;
never reserve once per inserted group. Optional sizing estimates fail small if refused. Required allocation
failures return Status and clean up normally; a late fallback must not bypass the budget or repeat partially applied
updates. Plan eligibility and backend choice are deterministic, not environment- or thread-dependent. The research
selector does not ship: production controls, if needed, must meet [ADR 0031](0031-runtime-knobs.md).

### Implementation sequence: separate, reviewable PRs

Each numbered step is a logical PR. If its review becomes too large, split at a tested interface boundary and
keep the same acceptance conditions. Do not combine backend, aggregation policy, planner estimates and view
transport into one performance claim. Every step compares against its immediate predecessor as well as the
original baseline, preserving an oracle/fallback and recording which path actually ran.

1. **Integrate the fixed-width key backend.** Adapt `3b1f96e` to current main, add non-inserting Lookup, preserve
   packed-string selection, and replace duplicate benchmark machinery with the engine backend where practical.
   Establish owned stored-key access and dense-ID invariants without changing local/raw policy or aggregate
   update order. Ship only the one/two fixed-width shapes justified by the per-shape gates; keep other types on
   the existing path. Include DOUBLE, NULL, growth, offsets, repeated batches and allocation-failure tests.
2. **Generalize layouts and composite keys.** Extend the same interface to additional supported fixed-width types
   and arbitrary key counts, with a generic column-oriented fallback inside the design. Avoid one template for
   every possible tuple of types. Keep an explicit eligibility matrix and exact output/Lookup tests across mixed
   layouts. Existing Arrow fallback remains available until each shape is both correct and measured; no SQL type
   becomes newly accepted merely because a storage layout can hold it.
3. **Add batch aggregate programs and typed merge.** Support multiple aggregate calls and their current result
   types through typed update/merge/finalize operations. First measure the new key backend with existing state
   updates, then fused updates with the same local/raw policy. Remove avoidable row-ID materialization only for
   callers that do not need it. Implement direct stored-key/state merging with an immutable ownership boundary;
   preserve part-order arithmetic, dependent keys, grouping-set projection, grouping without keys and whole-input
   finalization checks even under LIMIT/top-N. Existing DISTINCT/two-level operators remain on their tested path
   until their prefix and merge contracts are implemented and validated explicitly.
4. **Integrate partitioned local tables and direct dispatch.** Use fixed routing inside each worker to populate
   per-lane tables, cache/reuse hashes where measured, and hand completed partitions to their lanes. Cover
   multiple keys, aggregate programs, selections and NULLs without copying payload rows. Measure three policies
   separately: the existing adaptive local/raw path, one local table, and partitioned local tables. A comparison
   of the last two must use the same all-local/adaptive policy, hash, state arithmetic and global merge path.
   Decide whether to retain or replace adaptive bypass only from complete-query CPU, wall and memory results;
   the prototype's always-local choice is not an accepted heuristic. Keep skew, part-order merging, cancellation,
   backpressure and per-partition cleanup correct at every worker count. Preserve partition top-N and the separate
   two-level distinct path, or leave them on an explicit fallback until supported.
5. **Add owned string and mixed-key layouts.** Hash/compare values in place and copy bytes only for new groups.
   Support arbitrary lengths and multiple/mixed columns; preserve the current short-string packing optimization
   for its eligible shapes. Inline prefixes may reject unequal candidates but do not replace full equality for
   equal long values. Follow [ADR 0027](0027-string-views.md) for owned BinaryView inputs, retention/compaction and
   public Binary output. Admit a view-producing path only with complete consumer eligibility and lifetime proof;
   ordinary Binary remains supported independently. Dictionary-entry hashes can be cached, but raw codes are
   comparable only within a proven common domain. Translation, owners and fallback are budgeted. Dictionary
   grouping remains a separately measured experiment; this plan does not reverse rejected
   [ADR 0028](0028-dictionary-group-keys.md) or require global view transport before ordinary grouping can improve.
6. **Integrate whole-partition window grouping.** The serial window operator already uses GroupTable, so an
   eligible new key backend can help without changing its scheduling. Explicitly preserve each input row's group
   mapping and gather finalized results back to input order; never confuse partition-local IDs with final IDs.
   Parallelize independent semantic window partitions only with an explicit row-to-partition/group mapping,
   stable output placement, bounded input retention and unchanged arithmetic/error ordering. Hash lanes contain
   many semantic partitions and are only scheduling buckets. Keep serial accumulation for cases where splitting
   and merging partial states would change the window's defined evaluation behavior. This step implements a
   measured slice of [ADR 0043](0043-window-functions.md)'s WP1, not all window execution.
7. **Evaluate ordered windows separately.** RANK, peers, ROWS prefixes, navigation and ordered frames keep their
   sort/order requirements. Reuse key storage or partition discovery only when the measured path benefits; direct
   hash dispatch does not replace sorting, peer detection or prefix evaluation. Broader parallel window scheduling,
   bounded buffering or spilling belongs to ADR 0043's follow-ups and needs its own evidence and design. A
   non-beneficial experiment is recorded and stopped, not enabled to complete this checklist.

Steps 1-4 form the fixed-width grouping path. Step 5 extends it to general strings; step 6 can reuse proven key
backends independently of step 5 for eligible numeric partitions. Step 7 is a follow-up investigation, not a blocker
for useful grouped aggregation. Each PR records what remains on fallback; optimization coverage must not silently
change which SQL answers. No dependency, module-edge, thread-count or planner-cardinality change is part of this
sequence without a separate justified design.

### Verification and acceptance

The earlier October 29 per-shape and end-to-end gates remain for replacing the grouper; this amendment does not
waive a failed gate or claim that the narrow Q18 experiment passes them. Evaluate complete-query improvements
separately from microbenchmarks and report CPU and memory beside wall time. Reuse existing synthetic generators
and approved benchmark inputs; commit only synthetic tests and permitted performance summaries.

- **Correctness:** differential key/ID/Lookup and aggregate tests against the existing backend, DuckDB-completed
  SQL tests when behavior paths change, and existing serial/parallel/oracle suites. Cover composite NULLs, empty
  input, slices/selections, scalar and array inputs where supported, multiple batches with reused scratch, growing
  tables, collisions, duplicate keys and representation changes. Check exact DECIMAL/integer arithmetic, all
  existing DOUBLE guarantees, first-seen keys, group-ID limits and finalization errors hidden by LIMIT/top-N.
  Include sets, grouping sets, dependent keys and two-level distinct whenever their shared interfaces change.
- **Ownership:** test immutable source sharing after the producing batch/operator is destroyed, independent lane
  cleanup, retained result lifetime, denied reservations, growth overlap, cancellation and repeated Open/Close.
  Use existing parallel harnesses; adding threads to tests still needs the repository's separate approval.
- **Microbenchmarks:** retain 1000, 16 Ki, 256 Ki and 1 Mi groups and report 4 Mi. Add a large-global regime around
  47 Mi groups per lane: SF1000 qualification grouping can reach that average with 32 lanes. Measure local and
  merge paths, one/two/many keys, random versus clustered keys, duplicate rates, skew, NULLs, string lengths,
  multiple aggregates, growth, hash reuse and budgeted allocations. Large regimes supplement the prior gates;
  earlier claims that partitioning makes them impossible no longer apply.
- **End to end:** retain the earlier ClickBench-derived per-shape gates and all-query regression check. Also
  compare all 22 TPC-H-derived queries at SF100, targeted SF1000 grouping including Q18, and affected TPC-DS-derived
  queries. Use 16 and 32 threads for the added scale comparisons; include the earlier required 8/32-thread legs
  when evaluating its ClickBench gates. Fix CPU placement, document memory placement, watch external load,
  warm each mode and interleave at least three measured repetitions with rotating order. Keep the same physical
  plan when attributing runtime effects. Run DuckDB unchanged as a drift control where required by the earlier
  gate. Publish repetitions/ranges and CPU rather than silently dropping slow observations. No new 64-thread gate
  is inferred from these tests; high-thread/NUMA behavior is reported when relevant.
- **Attribution and stopping:** record backend selection, input/local groups, local/raw rows, hash/dispatch time,
  ID partitioning, merge time, waits, peak tracked capacity and process CPU. Explain overlapping profile timers.
  Retain controls until a step's benefit is understood; do not silently retune another optimization to hide its
  regression. A failed per-shape gate leaves that shape on fallback or prompts a separately reviewed revision.
  Production switches need ADR 0031's answer/error neutrality proof and observable path witness.
- **Gates:** run `pixi run check` for every PR. Key storage, state ownership, immutable handoff and parallel
  execution changes also require `pixi run check-full`; affected workload ratchets and SF1 sanitizer validation
  remain subject to their existing policies. No skips, timeout increases, coverage changes or status-ratchet
  edits are authorized by this plan.

### Consequences of this amendment

The implementation becomes several independently measurable PRs. It reuses substantial existing key-table work,
keeps generic key identity separate from aggregate specialization, and treats partitioned local layout as an
incremental improvement. Larger global tables, exact arithmetic and the current window/set consumers prevent a
blind import of an older prototype or an upstream loop. Maintaining fallback paths costs code and tests until
coverage and performance justify consolidation. This amendment changes neither SQL semantics, ADR statuses,
partition counts, dependencies nor runtime defaults; implementation starts after this documentation PR merges.
