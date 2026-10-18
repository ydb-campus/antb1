# 27. Short strings inline: the view layout for VARCHAR

Date: 2026-10-15

## Status

Proposed (deferred: S3 measured the lever at 18% of one query, not at the gap; see S3)

## Context

- **The target.** A VARCHAR value is `arrow::binary()` everywhere above the scan — one decision, in
  `plan::ToArrow` (`src/plan/types.cc`), with the rule written down in
  `src/plan/include/antb1/plan/types.h`. That is a 32-bit-offset array: a contiguous bytes buffer plus one
  offset per row. Copying a *selection* of such an array means copying its bytes, because a subset of a contiguous
  buffer is not a contiguous buffer. Every operator boundary therefore copies every string it passes on.
- **What that costs, measured.** Over a TPC-H-derived three-table join at SF 100 on 4 threads, varying only the
  GROUP BY width and nothing else:

  | GROUP BY | antb1 | DuckDB |
  | --- | ---: | ---: |
  | one BIGINT key | 8.92 s | 2.69 s |
  | and four VARCHAR keys | 17.44 s | 3.25 s |

  The four VARCHAR keys cost antb1 **+8.52 s** and DuckDB **+0.56 s** — about 15x on the marginal cost, on top of a
  general per-operator gap of about 3.4x that the one-key row already shows. The profile puts it in one place: that
  join's `gather` goes from 2.573 s to 16.338 s (summed over parts), and the query's peak memory from **910 MB to
  4.93 GB**. The group count is identical in both rows, 3,884,218 — so this is not more groups or more rows, it is
  the bytes of about 130 characters per row over 11.46 M rows, copied and then hashed.
- **Why the bytes are copied so often** (`src/exec/gather.cc` is one of the sites, dispatching VARCHAR to
  `GatherTyped<arrow::BinaryArray, arrow::BinaryBuilder>`): the join build takes every column of every batch, the
  join gather appends one value per row per VARCHAR column into a fresh `BinaryBuilder`, a non-unique or padded probe
  takes the probe side as well, `Materialize` filters the whole batch before each GROUP BY `Consume`, and the group
  table's row routing takes every column again. Group identity is `arrow::compute::Grouper`
  (`src/exec/group_table.cc`), which encodes each key into a row format of its own.
- **The engine already knows how to pass a string by reference, for exactly one call.** `plan::ScanValues` carries a
  VARCHAR column as `std::span<const std::string_view>` into the table's own decoded buffers
  (`src/plan/include/antb1/plan/table.h`), which is what lets a pushed-down predicate compare, `LIKE` and `IN`
  without copying ([ADR 0020](0020-filter-pushdown.md)). Those views are valid only during that call: the filtered
  scan copies the survivors into a `BinaryBuilder` in the same loop, and no view reaches an operator.
- **The 32-bit offsets are a correctness limit too, not only a cost.** `io::LargeToBinary` fails with a
  `CapacityError` when one batch of a VARCHAR column would exceed 2 GiB, and `GroupTable::kMaxJoinedKeyBytes`
  (`src/exec/group_table.h`) caps a joined key batch at 16 MiB expressly to stay far from that 2 GiB.
- **Two of our own ADRs have been here before, and one of them already measured the obvious version of this idea.**
  - [ADR 0019](0019-own-aggregation-hash-table.md) profiles a ClickBench GROUP BY query with about **46% of its CPU
    inside `arrow::compute::Grouper`** — 4% string hashing, 5% encoding and appending rows, 4% key comparisons, with
    much of a further 15% in copies from the same encoding — and decides to replace the grouper with a table of our
    own, "specialize[d] per key shape", scheduled for a later round.
  - That ADR's Consequences also record: "the string scan gap (Q20-Q22: the Parquet decoder copies every string;
    decoding into `BINARY_VIEW` **saved only 10-16%**) is a separate, later decision." So reading Parquet straight
    into the view layout has already been tried and is already known to be a small win.
  - [ADR 0016](0016-late-materialization.md) proposes late materialization of top-N columns: the other way to stop
    copying a string is to not carry it at all.
- **What Arrow 25 gives us, probed against the installed build rather than assumed.** The layout is there and it is
  the Umbra/DuckDB one: a 16-byte view, 12 bytes inline, otherwise a 4-byte prefix plus a buffer index and offset,
  with variadic data buffers, `BinaryViewArray`/`StringViewArray`, their builders, and a `binary_view_util.h` whose
  comparison fast path tests size and prefix in the first eight bytes. `arrow::utf8_view()` and
  `arrow::binary_view()` are declared in `arrow/type_fwd.h`, and
  `parquet::ArrowReaderProperties::set_binary_type(Type::BINARY_VIEW)` really does make the Parquet reader return
  view columns.

  The compute layer is almost empty. Of Arrow 25's 306 registered compute functions, **18 kernels across 9 functions
  accept a view type** — the six comparisons, `unique`, `value_counts`, `dictionary_encode`. Everything this engine
  leans on refuses:

  | What antb1 needs | Arrow 25 with a view type |
  | --- | --- |
  | `take`, `filter` | `NotImplemented` — no view kernel at all |
  | `sort_indices` | `NotImplemented`; the table path says "Sorting not supported for type binary_view" |
  | `arrow::compute::Grouper::Make` | `NotImplemented: Keys of type string_view` |
  | `lower`/`upper`, `match_substring`, `match_like`, `substring`, `binary_length`, `is_in`, `if_else` | none accept views |
  | `cast(utf8 -> utf8_view)` | **zero-copy**: aliases the source buffer, allocates only 16 B/row |
  | the six comparison kernels | supported natively |

  There is no implicit fallback: `DispatchBest` refuses rather than casting a view back to `utf8`, so adoption fails
  loudly at each call site instead of hiding a copy. And the Parquet view decode is **not** zero-copy — it copies
  every out-of-line byte into 32 KiB heap blocks — which is consistent with ADR 0019 having measured only 10-16%
  there.
- **That table reads worse than the situation is, because the two sites that matter are not Arrow kernels at all.**
  `src/exec/gather.cc` contains no `arrow::compute` reference: the join gather is antb1's own `GatherTyped` loop over
  a builder, and the sort's comparison is antb1's own `OrderFor`. So the sites the measurement points at need plain
  C++ and no kernel. Nor is the grouper a dead end: `arrow::compute::Grouper` is an abstract class whose `Consume`,
  `Lookup`, `Populate`, `GetUniques`, `num_groups` and `Reset` are public and pure virtual, and `GroupTable` holds it
  as a `std::unique_ptr`, so a grouper of our own accepting view keys is a drop-in — which is exactly what ADR 0019
  proposes to build anyway. And if a kernel is genuinely wanted, Arrow's registry is open: `GetFunction` returns a
  **non-const** `shared_ptr<Function>` and `VectorFunction::AddKernel` is public, so a view kernel can be added to
  Arrow's own `take` at startup beside `arrow::compute::Initialize()`. What remains genuinely Arrow's is only the
  string functions and the `Filter`/`Take` calls in `src/exec/aggregate_state.cc` and `src/exec/sort.cc`.

## Decision

- **The lever is not the type, it is the copying.** Switching `plan::ToArrow(kVarchar)` to `arrow::utf8_view()` would
  break `take`, `filter`, `sort_indices`, `Grouper` and every string function at once, and would buy nothing by
  itself: Arrow 25 hands us the layout, the builder, the comparisons and a free `utf8 -> utf8_view` cast, and no
  kernel we actually use. **We will not change the engine's VARCHAR type.** `arrow::binary()` stays the
  representation that crosses operators, and `docs/sql-subset.md`'s VARCHAR semantics — bytes, compared byte-wise,
  never validated as UTF-8 — are unaffected either way.
- **We will adopt the view layout where we own the loop, and only there.** Two sites are antb1's own code rather
  than an Arrow kernel, and both are measured costs:
  - **The hash join's gather** (`src/exec/gather.cc`): today one `BinaryBuilder::UnsafeAppend` per row per VARCHAR
    column. A gather that writes 16-byte views referencing the build's own chunks copies no bytes. A probe
    established that constructing such an array through the public API is about fifteen lines and passes
    `ValidateFull()`, and that the native `equal` kernel accepts the result.
  - **The group table's keys**, as part of ADR 0019's own hash table and not before it: a key layout with the size
    and a four-byte prefix in the first eight bytes resolves most comparisons without touching the bytes, which is
    the same trick `binary_view_util.h` uses and the reason DuckDB pays 0.56 s where we pay 8.52 s.
- **The boundary between a view and a value is an operator boundary, and it is explicit.** A view is only ever valid
  while the batch it references is alive. An operator that *retains* rows beyond its input's lifetime — the join
  build, the group table, the sort buffer, a result the caller holds — materialises them. This is the rule
  `plan::ScanValues` already states for one call; this ADR extends it to a batch's lifetime and writes it down,
  because the alternative is a dangling view and a wrong answer rather than a slow one.
- **The cost of this work is retention, not kernels.** A view array keeps its *whole* source data buffers alive, so a
  gather that keeps one row in a hundred pins a hundred rows' bytes. That converts a copy cost into a
  memory-retention cost, and memory is what already broke Q21 at SF 100 ([ADR 0026](0026-join-performance.md)). So
  every site that adopts a view must say which buffers it retains and for how long, and the gather of S1 must be
  measured on peak memory as well as on time — a gather that halves the time and triples the retention is a
  regression here, not a win. Where retention would exceed the bytes saved, the answer is to materialise, which is
  the same rule as above read from the other side.
- **What this does not change.** Determinism is untouched: a view carries the same bytes in the same order, and an
  answer stays byte-identical for any thread count. `common` and `sql` still never include Arrow, since none of this
  is visible to them. No new dependency: the layout, the builders and the comparison kernels are in the pinned
  Arrow 25.

### What we expect, and what would refute it

The gather measurement above is the prediction: 16.338 s of `gather` and 4.0 GB of peak memory on one join, for four
string columns whose bytes are already resident in the build's chunks. A view gather should remove most of the copy
and most of that memory, and leave the rest of the query alone. It would be refuted if the saving were small because
the cost is really in the hashing rather than the copying — which is precisely what ADR 0019's 10-16% result at the
scan should teach us to expect as a possibility, and why this ADR's first item is a measurement of the one site, not
a representation change across the engine.

## Consequences

- VARCHAR keeps one representation across operators, so every existing type switch over `arrow::Type::BINARY` keeps
  working and no golden, `.slt` expectation or output format changes.
- The view layout becomes a *local* optimisation with an explicit lifetime rule, not a type change. That keeps its
  blast radius to the site that adopts it, and means a site can adopt it without the others.
- The 2 GiB-per-batch VARCHAR limit and `kMaxJoinedKeyBytes` stay as they are. Removing them needs the view layout to
  cross operators, which this ADR declines; if a user hits that CapacityError, that is the trigger to reopen it.
- ADR 0019's own hash table absorbs the group-key half of this, and should cite this ADR for the key layout. The two
  were always one piece of work: a table "specialize[d] per key shape" is where an inline-string key shape belongs.
- A new hazard to watch for in review: a view that outlives the batch it points into. The rule above makes that a
  reviewable property of each site rather than a property of the type.

## Plan

One PR per id. See S1's dated update: the order below is **S2 then S1**, not as first written.

- **S1, perf(exec): gather VARCHAR by reference in the hash join.** The one site antb1 owns, with the lifetime rule
  enforced where the join retains rows. Measured before and after on the three-table join above and on all 22 at
  SF 100.
  - Update (2026-10-15): **S1 cannot stand alone, and the order below is wrong as first written.** In the query this
    ADR measures, the join's output goes to a `Compute` and then to a `PartGroupAggregate`, whose
    `arrow::compute::Grouper` rejects view keys — so a view gather would be materialised at the grouper immediately,
    which *moves* the copy rather than removing it. The four VARCHAR columns are group keys, so every one of the
    11.46M rows reaches that grouper; there is no selection in between to shrink what is materialised. **S1 has no
    consumer until S2 gives it one.** S2 is therefore first, and S1's acceptance measurement has to be taken with S2
    in place. The claim that S1 "stands alone and decides whether the rest is worth doing" was wrong: what decides it
    is S2 plus S1 together, and the way to find out cheaply is to measure S2's key layout against the same query
    before building a view gather at all.
- **S2, perf(exec): the inline-string key layout, inside ADR 0019's own hash table.** Not before it: the key layout
  is a property of that table, and building it against `arrow::compute::Grouper` is not possible — the grouper
  rejects view keys outright.
- **S3, docs(adr): record what S1 and S2 measured**, and either accept this ADR or move it to
  `Proposed (deferred)` with the numbers that say why.
  - Update (2026-10-18, S3): **deferred, and neither S1 nor S2 was built.** S2 lost its host and S1 still has no
    consumer, so what follows is measured around them rather than by them. Four results, in the order they land on
    this ADR:
    1. **S2 has no host.** [ADR 0019](0019-own-aggregation-hash-table.md)'s own table was finished and measured in
       the pipeline on 2026-10-18 and **reaches parity, not a win**: every table this engine builds falls in the
       10^4-10^6 band, where that design is 0.46-0.88x of `arrow::compute::Grouper`, and its 1.4-1.6x wins are at
       4-16 M groups, which the partitioning prevents. S2 is "the inline-string key layout, inside ADR 0019's own
       hash table"; there is no table to put it in. The key layout itself was tried on that prototype and removed
       only *false* candidates — which the stored hash already removed — so it bought nothing measurable.
    2. **The ClickBench gap is not string-shaped, which is the result this ADR would most have liked to be wrong.**
       Over the official single file at 14 threads, best of three, execution time: the six grouped queries that carry
       the whole remaining gap are 7.205 s against DuckDB's 4.611 s (1.56x, +2.59 s), while **the other 37 are
       0.83x** — faster than DuckDB. Inside those six, q32 keys on a BIGINT and an INTEGER with no string anywhere
       and runs **1.61x**, against 1.62x and 1.57x for the two keyed on a VARCHAR URL and 1.47x and 1.49x for the two
       whose keys include a VARCHAR phrase. The deficit is the same with and without strings. Two other queries
       *grouped on a VARCHAR* run 1.05x and 0.80x, and a pure string filter runs 0.80x. So the gap tracks group
       cardinality and not key type, and an inline-string key layout cannot close it. (String *handling* on the scan
       side is already by reference: [ADR 0020](0020-filter-pushdown.md)'s pushdown compares, `LIKE`s and `IN`s
       through `plan::ScanValues`'s views without copying.)
    3. **The string cost this ADR predicted is real, and it is 18% of the worst query.** The prediction was isolated
       on the 22 at SF 100, 14 threads, best of three, by removing the one VARCHAR column from the query with the
       largest absolute deficit — its remaining key functionally determines that column, so the group count and every
       row count are unchanged:

       | the query with the largest deficit | time | peak memory |
       | --- | ---: | ---: |
       | as written, one VARCHAR among five keys | 8.407 s | 15.36 GB |
       | without the VARCHAR column | 6.860 s | 12.84 GB |
       | **the marginal cost of one VARCHAR key** | **+1.55 s (18%)** | **+2.5 GB** |

       That reproduces this ADR's +8.52 s for four keys at 4 threads in kind and in order of magnitude, so the
       Context's measurement stands. What it does not support is the conclusion: **the same query without any VARCHAR
       at all is still 3.1x DuckDB** (6.860 s against 2.188 s). A view gather would remove most of the copy half of
       1.55 s on an 8.4 s query that is 3.9x off the oracle.
    4. **What the gap actually is, in the same profile.** That query materialises **600,037,902 rows to keep
       44,786**: an inner join whose `gather` costs 26.692 s summed over parts, under a semi join that then discards
       all but 44,786 rows, with a build side of 6,398. The cost is the *number of rows gathered*, not the bytes of
       the strings among them. DuckDB answers it in 2.17 s by computing that 6,398-row filter first and pushing it
       into both Parquet scans. That is [ADR 0029](0029-dynamic-join-filters.md), and it is a different lever
       entirely.
  - **The trigger to reopen.** This ADR stays deferred rather than rejected, because nothing in it was refuted
    except its ranking. Reopen when any one of: ADR 0029 or a semi-join ordering rule removes the 600 M-row
    materialisation, so that what remains at that site is the string bytes and an 18% lever is the top of the list;
    ADR 0019's table finds a shape that beats the grouper, giving S2 a host again; or a later Arrow accepts view
    types in `take`, `filter`, `sort_indices` and `Grouper`, which turns the whole-engine alternative from a sweep
    into a type change. The probe of Arrow 25 in the Context is the thing to re-run first, since it is what makes
    the difference.

Deliberately out of scope: decoding Parquet into `BINARY_VIEW` (ADR 0019 measured 10-16% and the decode is not
zero-copy); changing the engine's VARCHAR type; and late materialization, which is ADR 0016's and is a different
answer to the same question — carry a row reference rather than a cheaper value.

## Alternatives considered

- **Make `arrow::utf8_view()` the engine's VARCHAR type.** The clean version, and what a greenfield engine would do.
  Rejected for Arrow 25, but not because it is impossible: the work is a whole-engine change before a single query
  gets faster. Every type switch over `arrow::Type::BINARY` needs a second arm (`KeyWidth`, `JoinTable::ViewOf`,
  `GatherRows`, `OrderFor`, `FilteredColumnOf`, the scan filter's string branch), every remaining `arrow::compute`
  call needs a view path or a materialisation point, literals need a `BinaryViewScalar` built from a `Buffer` because
  `MakeScalar` refuses view types, and the whole engine carries the retention risk at once rather than one site at a
  time. Deferred rather than refused: the trigger to reopen is a later Arrow whose compute layer accepts views, or
  S1 and S2 showing the win is large enough to be worth the sweep.
- **Add view kernels to Arrow's registry up front.** Possible — `GetFunction` hands back a non-const
  `shared_ptr<Function>` and `VectorFunction::AddKernel` is public, so `take` and `filter` could learn view types at
  startup. Rejected as a starting point, not on feasibility: it is a process-global mutation that must happen before
  any query and that the registry is not safe to take concurrently, Arrow's selection internals are not in the
  installed headers so each kernel body is ours to write regardless, a later Arrow adding its own view kernel would
  collide and needs a version guard, and none of it is needed for the two sites the measurement actually points at.
  If S1 and S2 land and a third site turns out to want `arrow::compute::Take` on views, this is how.
- **`arrow::large_binary()` instead.** Removes the 2 GiB limit and nothing else: it is still contiguous bytes plus
  offsets, so every selection still copies, and it costs 8 bytes per offset rather than 4. Rejected: it addresses
  the limit we are not hitting and not the cost we measured.
- **Dictionary-encode VARCHAR columns.** A gather of dictionary indices copies 4 bytes per row and no string bytes,
  which would also fix the measured site. Rejected as the general answer: it pays off only where the column has few
  distinct values, the engine would have to decide per column, and a join or a group by across two differently
  encoded columns needs unification — more machinery than the view layout, for a narrower case.
- **Leave it to ADR 0019's hash table alone.** Tempting, since that ADR already owns the grouper's 46%. Rejected
  because the measurement above puts 16.338 s in `gather`, which is a join cost and not a GROUP BY cost: the group
  count did not change between the two rows at all. The two halves are separable, and S1 is the separable one.

This workload is derived from the TPC-H Benchmark and is not comparable to published TPC-H Benchmark results, as
this implementation does not comply with all requirements of the TPC-H Benchmark.
