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
  - Update (2026-10-29, **step 0 chose the loop**; #253, `bench/group_loops.h`): before any engine change, a
    benchmark of loops of our own against the grouper under a `MemoryBudget` picked the design for fixed-width keys:
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
    replaces `bench/group_loops.h` with the engine's table.
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
