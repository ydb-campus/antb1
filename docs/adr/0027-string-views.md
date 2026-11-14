# 27. Short strings inline: the view layout for VARCHAR

Date: 2026-10-15

## Status

Proposed (deferred: S3 measured the lever at 18% of one query, not at the gap; see S3)

The [2026-11-13 amendment](#amendment-2026-11-13-owned-views-and-staged-adoption) records new evidence and
the next proposed experiment. It revises the proposed ownership contract and implementation order below;
the original investigation remains as history. Production adoption of the view path remains deferred.

The [2026-11-14 amendment](#amendment-2026-11-14-prototype-results-and-dictionary-roadmap) records the
preserved experiment branch, the implemented payload path, its final measurements and the dictionary roadmap.

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

## Amendment (2026-11-13): owned views and staged adoption

### What changed since S3

[PR #364](https://github.com/ydb-campus/antb1/pull/364), merged as `8b0d4a4`, addressed
[issue #355](https://github.com/ydb-campus/antb1/issues/355) without a new hash table or a view array.
`exec::PackedStringKeyTable` packs one or two ordinary STRING/BINARY keys of at most seven bytes into
UInt64 words, uses the existing integer-key backend and decodes unique keys. Longer values promote to the
ordinary Grouper while preserving public group IDs. Packing buffers are cached only for batches of up to
256 rows, retaining at most 2 KiB per key column. This is a complete, independent optimization. It neither
implements S2's general string-key layout nor makes Arrow Grouper accept BinaryView.

A separate synthetic investigation tested three selection/gather stages followed by a consumer that reads
every output byte. The 36 shapes varied string length (1, 7, 12, 13, 32 and 128 bytes), selection density
(1%, 10% and 100%), fanout (1 and 4), batch size, slices, NULLs and empty values. The measurements used
static Release ThinLTO, Arrow 25, one pinned CPU and ten interleaved repetitions. Each arm's estimate is the
mean of its seven fastest wall-time repetitions; ratios below are geometric means across equally weighted
shapes, not a workload-weighted estimate or a statistical-significance claim.

| Timed work | View / ordinary Binary time | Shapes faster with views |
| --- | ---: | ---: |
| Binary-to-view conversion, transport and byte consumer | 2.315 | 11 / 36 |
| The same, plus final conversion back to Binary | 2.629 | 4 / 36 |
| Transport and byte consumer, with both input representations prepared outside timing | 0.784 | 36 / 36 |

The prepared-input ratio excluding the two tiny-batch shapes was 0.819. These are transport measurements,
not scan or SQL speedups. Creating descriptors for an entire source before keeping 1% of it visits rows
that ordinary selection never copies. Converting back to Binary can also consume the transport saving.
An eligible producer and a complete chain of consumers must therefore be measured together.

Retention is a separate constraint. In the prepared-input case with 262,144 rows, 128-byte values and 1%
selection, views retained 30,207,552 bytes of source capacity for 301,952 distinct live payload bytes.
Dense long-string fanout reduced both allocation and copying, but sparse views kept roughly 100 times
their useful source payload. Short inline values need 16-byte descriptors and can use more memory than
Binary even when processing is faster. These are Arrow buffer capacities and logical bytes, not RSS.

Arrow 25 capability probes still reject BinaryView/StringView in Take, Filter and Grouper. The saved
prototype supplies limited checked gathers only; it is not a replacement for those engine dependencies.
Reports and provenance remain local under `build/static/bench-results/binary-view-investigation-20261113/`
(`FINAL.txt`, `verification.json`, `summary.json`, `native-summary.json`). Source snapshots and the
benchmark-only patch are preserved separately from the merged packed-key implementation. These results
justify the experiment below, not enabling a general view representation.

### Proposed representation and ownership contract

This proposal replaces the original rule that every retained view must be materialized, and the claim
that Binary is the only possible physical representation at every operator boundary. It keeps the SQL
type and the default representation unchanged:

- **Logical VARCHAR remains arbitrary bytes.** Use `arrow::binary_view()` in an eligible physical path,
  not `arrow::utf8_view()`. Preserve byte-wise ordering, embedded zeros, invalid UTF-8, NULLs and empty
  values. `plan::ToArrow(kVarchar)` and public result columns continue to use `arrow::binary()`.
- **A view array owns its dependencies.** Its Arrow buffers must hold shared ownership of every referenced
  payload allocation. It may then outlive the input batch, join operator or producer. A bare
  `std::string_view`, a pointer into a decoder page, or a pointer whose owner has already been released
  does not meet this contract. Referenced bytes and addresses must remain immutable and stable while
  views exist; keeping a reusable decoder buffer alive does not prevent its next read from overwriting
  old results. Existing `plan::ScanValues` views remain borrowed for one call.
- **Physical schemas describe the arrays actually emitted.** An operator advertising BINARY cannot emit
  BINARY_VIEW under that schema. Eligible operators propagate an explicit physical representation while
  binding and SQL types stay unchanged. Projection, empty outputs, reopening an operator and final
  result assembly must honor the same schema. A materialization boundary advertises Binary itself.
- **Retention is budgeted for its full lifetime.** Descriptors, validity buffers, ownership metadata,
  copied payload and retained source allocations stay charged while reachable. Sharing a source does
  not charge its capacity again, and dropping the producer does not uncharge bytes still owned by a
  result. Retained capacity is measured by distinct backing allocations, not by the visible size of a
  slice; aliased slices must not make a large allocation appear small. The allocating MemoryPool and
  MemoryBudget must themselves outlive all buffers that call them on destruction; shared buffer
  ownership alone does not extend the pool's lifetime. Keep the necessary query/result owner alive.
- **Compaction controls sparse retention.** Drop unreferenced payload buffers and remap view buffer
  indices after selection. For referenced allocations with poor live-byte utilization, copy surviving
  values into compact owned buffers while keeping the BinaryView physical type. Count distinct live
  source ranges separately from duplicate output references so fanout cannot hide retention. The pilot
  must define and measure an explicit retention limit before enabling views, including the transient
  source-plus-destination peak during compaction. A failed allocation returns a checked error and
  releases temporary ownership; it must not bypass the query budget to finish the copy.
- **Eligibility covers the whole path.** Resolve consumer support before a producer emits views.
  Ordinary Binary remains the path for unsupported shapes. Do not discover missing kernels by failing
  halfway through a query, silently convert at every boundary, or change a stream's physical type in
  response to one batch's values. Preserve existing evaluation order and error behavior.

Materialization now has two distinct meanings: selecting rows from a batch, and copying view payload into
ordinary Binary. The implementation must distinguish them. `exec::Materialize` currently performs the
first through Arrow Filter; merely keeping its name or return type does not make it support views.

### Next implementation: payload transport through a complete narrow path

The first engine experiment is an integer-key INNER equijoin carrying selected build-side VARCHAR
payloads through numeric selection and numeric top-N, followed by one explicit Binary output boundary.
Strings are payload only: no string join key, grouping key, sort key or string expression is eligible.
Exclude residual join predicates, subsequent joins, shared subplans and aggregation/distinct consumers
from this first path. This gives a view producer a compatible consumer without waiting for a replacement
Grouper. The original S2-before-S1 dependency still applies to the measured string-grouping shape, but
not to this payload path.

Build the following pieces together before assessing the query speedup:

1. **Produce only gathered views from owned join chunks.** Extend the path around `exec::GatherRows`
   and hash-join output assembly. Write descriptors for emitted rows directly from the retained Binary
   arrays; do not convert a whole scan batch before its selection. Handle multiple chunks, sliced
   arrays, duplicate matches and valid NULL values. The shared gather helper must preserve no-row/NULL
   padding semantics before any outer-join caller uses it; outer-join eligibility is a later extension.
   Reading the inline bytes and prefix still costs work; this is not a zero-cost conversion. The scan
   API and pushed predicates remain unchanged in this first experiment.
2. **Select and carry the physical payload explicitly.** Provide checked exec-owned selection/gather
   helpers for the index types used by the participating paths. Honor nullable index and selection
   semantics where the caller permits them; keep Batch's non-null selection invariant. Projection,
   selection materialization and numeric top-N must preserve the view arrays and their owners.
   Unsupported Arrow calls are not a fallback mechanism. Keep the helpers local to exec instead of
   adding process-global kernels to Arrow's registry.
3. **Bound retention while rows are buffered.** The sort/top-N buffer must account for the source
   allocations its surviving rows pin, compact sparse payloads and release discarded owners. Sorting
   only numeric keys does not exempt the retained payload from this requirement. Exercise bounded-memory
   operation as well as an unconstrained run; the ordinary Binary path is the reference for retained
   and peak capacity.
4. **Materialize the final selected output once.** Preserve the public Binary schema, exact row order
   and existing overflow checks at the boundary before `Drain` assembles the result. Test a result
   retained after operators and input batches are destroyed. Keep the ordinary Binary execution path
   available for the controlled comparison and for ineligible plans.

The physical planner must prove this complete path, including any parallel merge or projection nodes it
inserts. Eligibility must be structural and deterministic, not based on observed values in the first
batch. Do not change join order, scan filtering, batch size or grouping strategy in the comparison.

### Later steps: consumers, dictionaries and scan production

These are separate decisions after the payload experiment, each requiring its own complete-path
measurement:

- **View-aware string grouping and joins.** Use the existing `KeyTable` interface to investigate a
  consumer that hashes and compares Binary and BinaryView values without first copying every row to
  Binary. It must own retained key bytes, preserve public IDs, non-inserting Lookup and merge semantics,
  and support longer values and high cardinality. The merged seven-byte packing specialization remains
  a separate choice for its eligible key shapes. A custom hash table is a candidate, not a prerequisite
  established by the transport microbenchmarks.
- **Dictionary encoding complements value layout.** A dictionary stores distinct values plus row
  indices; its values may themselves be Binary or BinaryView. Views reduce value movement, while a
  dictionary can avoid repeated hashing or predicate evaluation. Codes identify a value only within
  their dictionary domain. Cross-batch, cross-part and join comparisons need a checked translation to
  shared semantic IDs or a value-based fallback, never raw code equality across domains. Count the
  dictionary owners and translation tables in the memory budget. Existing dictionary predicate
  evaluation in the filtered scan remains useful independently of transport. Partition routing must
  hash equal values identically across Binary, view and dictionary representations, including composite
  keys and NULLs. Worker-local canonical IDs are not a common hash domain. Cached dictionary-entry
  hashes must preserve the ordinary keys' content-based routing semantics.
- **Dictionary grouping is still an experiment.** [ADR 0028](0028-dictionary-group-keys.md) remains
  rejected: the scan's per-batch dictionaries, routed rows and rebuild costs were measured limitations.
  Do not assume a stable dictionary per row group or raise the engine batch size to manufacture a win.
  Reconsider only with new evidence for stable domains or sufficiently cheap remapping, including high
  cardinality, changing dictionaries, NULLs and the complete scan/merge cost.
- **Native view production and wider expressions come later.** Reading owned views directly from
  Parquet, or producing owned views only for survivors of a pushed predicate, may avoid adaptation
  costs. Decoder-backed borrowed pointers cannot simply escape. Measure decode, filtering and final
  output together before changing the scan contract. Add each expression or key consumer deliberately;
  do not flip the global VARCHAR mapping in anticipation of future support.

### Acceptance and stopping conditions

Use independent generated fixtures for committed tests and benchmarks. The payload pilot needs tests for
lengths around the 12-byte inline boundary, invalid UTF-8 and embedded zeros, NULL and empty values,
offset slices, multiple and aliased source buffers, repeated indices, outer-join padding, empty inputs,
zero-column projections, deterministic top-N ties and limits, and unsupported-path eligibility. Verify
ownership after dropping sources and closing operators, compaction after sparse selection, memory-budget
failure and cleanup, cancellation, and repeated Open/Close. Cover the existing serial and parallel
harnesses without adding new test threads or network access. Expected SQL answers come from the existing
DuckDB oracle; do not hand-write new `.slt` expectations.

Performance acceptance requires complete static Release query runs including producer work, every
selection, compaction, final materialization and destruction. Compare the same physical plan with views
disabled/enabled on dense and sparse selections, short and long payloads, low and high fanout, and top-N
and output sizes that stress retention. Use ten interleaved measured repetitions after warm-up, balanced
first position, fastest-seven means and all-ten medians/ranges. Record OS CPU, wall time, peak RSS, peak
and retained query-pool capacity, allocation counts, and copied payload versus descriptor bytes. Include
normal 16-thread execution and a serial diagnostic. Microbenchmarks and operator profiles explain the
result; they do not replace it. Report losses as well as gains and recheck the full 22-query workload
derived from TPC-H before enabling a production path.

Keep the pilot disabled if final materialization consumes its benefit, retention cannot be bounded
within the normal query budget, or improvements depend on excluding producer/consumer work. Do not
compensate with a different join order or claim that a transport gain resolves the planner, hash-build
finalization or high-cardinality aggregation issues. `pixi run check` is required for a PR;
`pixi run check-full` is required for the ownership implementation. This amendment changes the proposed
roadmap, not the ADR status, Arrow dependency, SQL semantics or production execution path.

### Retention-policy experiment, 2026-11-13

The first owned-payload pilot bounded each gathered array's retained capacity by the larger of 64 KiB
and four times distinct live out-of-line bytes. In its independent synthetic long-string fanout
profile, sorting and merging referenced byte ranges consumed 40.35% of sampled serial CPU. Complete
16-thread query measurements across twelve shapes showed a 1.031 wall-time ratio and a 1.175 CPU
ratio against ordinary Binary, despite substantial copy and memory savings in some cases. The pilot
remains off by default. Evidence is local in
`build/static/bench-results/owned-view-pilot-20261113/REPORT.txt`.

The next opt-in experiment replaces that per-array sparse-retention rule with decisions at buffering
operators. It keeps the representation, immutable shared ownership, query-pool lifetime, complete-path
eligibility and public Binary output contracts above. Specifically:

- A streaming gather retains only the source buffers its descriptors reference and remaps buffer
  indices. It does not calculate an exact union of string intervals. Filters continue to use existing
  selections; selecting a new view array uses the same cheap sharing path.
- Top-N rejects rows using its numeric threshold first, then copies accepted long values into compact
  Arrow-owned payload buffers. The physical type remains BinaryView. Copying repeated values is
  allowed; this deliberately trades some fanout sharing for simpler lifetime and retention rules.
- Periodic top-N rebuilding gathers only survivors into fresh owned payload buffers and drops the
  old chunks. The existing row slack and an allocated-payload-byte threshold both trigger cleanup.
  The byte threshold is the larger of 8 MiB and twice the payload capacity just after the previous
  rebuild; cleanup requires discardable rows. This avoids repeatedly rebuilding large required
  survivors. It is an amortized cleanup policy, not a fixed upper bound on a query's string bytes.
- The hard query limit still charges each actual allocation once for its full lifetime, including
  descriptors, ownership metadata and old-plus-new payload during rebuilding. A row threshold alone
  does not bound variable-length payload. Allocation failure remains a checked error with cleanup.
- Final sorted output shares the owned payload buffers until the explicit Binary output boundary.
  Retained output remains valid after input batches or operators are destroyed.

The earlier exact-bound helper remains available for its general alias-aware behavior and tests;
the experimental pipeline selects the sharing/copying policy explicitly. No buffer is certified as
having passed the earlier exact live-byte bound merely because its ownership is safe.

This experiment may lose: the join can still own its build buffers while top-N copies strings, so
copying does not necessarily release any source memory and can increase peak allocation. Measure
ordinary Binary, the first pilot and this policy with unchanged inputs and plans, including the full
producer/consumer path, serial and 16-thread execution, bounded-memory runs and CPU profiles. Retain
all samples and balance interleaving positions as closely as possible across three arms. This changes
the experimental retention policy only; adoption and the ADR status remain deferred.

### Allocation-capacity sharing experiment

The eager-copy experiment removed interval sorting, but the twelve synthetic shapes still had a
1.042 complete-query wall-time ratio against ordinary Binary at sixteen threads. Its long-string
fanout case copied 168.93 MiB of payload versus 6.26 MiB with the exact-range pilot and 165.99 MiB
with ordinary Binary. Evidence is local in
`build/static/bench-results/view-retention-experiment-20261113/REPORT.txt`.

The next opt-in experiment keeps streaming sharing and changes the buffering decision:

- Within each output view column, compare each distinct backing allocation's full capacity with the
  total copy volume of its selected long values. Duplicate output references count in copy volume,
  as they would occupy separate
  storage under eager copying. Share the allocation when capacity is no larger; otherwise copy only
  its selected values into compact owned blocks. Make the choice separately for each allocation.
- Follow buffer parents when identifying an allocation, so slices and aliases cannot hide retained
  capacity or charge it repeatedly. Preserve descriptor offsets relative to their actual owners.
  Inline and null values retain no payload buffers. Do not sort or merge per-value byte intervals.
- Apply this policy after top-N pruning, during survivor rebuilding and when merging part buffers.
  Already compact part storage can remain shared when copying would duplicate more bytes. Periodic
  cleanup counts distinct backing allocations across buffered chunks and columns; the hard query
  memory budget still charges actual allocations and metadata for their full lifetime.
- This is a capacity-versus-copy-volume policy, not the original exact distinct-live-byte bound.
  Fanout can justify retaining a whole allocation even when its distinct selected bytes are sparse.
  Hybrid results must not be cached as having passed the earlier exact bound merely because they
  have safe ownership. The earlier bounded and eager-copy helpers keep their existing contracts.

Compare four interleaved arms: ordinary Binary, the exact-range pilot, eager copying and this policy.
Retain the same complete-output correctness checks, serial and sixteen-thread measurements, copy
counters, bounded-memory checks and profiles. The option remains off by default, and adoption and
the ADR status remain deferred.

## Amendment (2026-11-14): prototype results and dictionary roadmap

The ADR status remains **Proposed (deferred)**. This amendment records the implemented research path
and its stopping point; it does not accept a general view representation or enable one by default.
The earlier experiments remain historical evidence. The research branch
[`perf/binary-view-dictionaries`](https://github.com/ydb-campus/antb1/tree/perf/binary-view-dictionaries)
preserves the 28-file runtime/test prototype on base `c633669`, with views opt-in through
`--binary-view-payload` and disabled by default. It is not rebased onto the separately extracted
ordinary PR #370 and has no BinaryView adoption PR. The preserved runtime/test file hashes match
the prototype's completed `pixi run check-full` and static-build evidence; the ADR amendment records
that state without claiming new dictionary runtime support.

### What the prototype implements

- **Owned BinaryView transport.** Exec-local gather, Take and Filter helpers support the participating
  paths without registering global Arrow kernels. Descriptors own immutable payload dependencies;
  inline values need no payload owner. Slices, aliases, NULLs and repeated references preserve their
  byte semantics. Descriptors, ownership metadata and payload allocations remain budgeted, and their
  allocating pool must outlive the results. These helpers do not make Arrow 25 Grouper or arbitrary
  string expressions view-aware.
- **Retention policies with different contracts.** The exact-range and eager-copy implementations
  remain comparison paths. Streaming gathers share referenced owners. The hybrid retaining path
  compares full backing-allocation capacity with selected long-value copy volume, including duplicate
  references, separately within each output view column. It shares on equality or larger copy volume
  and otherwise copies selected values into compact owned blocks. Parent aliases share one decision;
  descriptor offsets remain relative to the actual owners. This is not certification of the earlier
  exact distinct-live-byte bound. TopN tracks distinct backing allocations across its retained
  columns/chunks and applies cleanup after pruning, rebuilding and merging, within the hard budget.
- **A complete, narrow physical path.** An integer-key inner join without residual predicates may
  produce selected build-side Binary payloads as views. Its children can contain ordinary joins or
  aggregation, but produce ordinary arrays. Above that producer, the planner permits only the checked
  projection, non-string selection/computation and TopN path. String sort keys remain Binary; using
  one projected alias as a sort key demotes that source column and all its aliases. Unsupported
  consumers, shared subplans and payloads dropped before output do not gain a view path. One explicit
  final boundary restores the public Binary schema. Logical VARCHAR and `plan::ToArrow` are unchanged.
- **Less repeated transport work.** A Binary-only sharing gather avoids generic source/root metadata
  processing while retaining checked source and index handling. View TopN combines selection with
  threshold pruning before one hybrid gather and avoids immediately retaining the same selection
  again. These are implemented mechanisms, not evidence of a complete-query win.

The later eligibility refinement therefore relaxes the original simple-input and numeric-sort-only
pilot, but does not allow a view value to become a string key or an arbitrary expression input. The
ownership, public-schema, deterministic eligibility and failure-cleanup requirements above still apply.

### What remains deferred

There is no new dictionary-encoded payload pipeline, dictionary-domain unification, dictionary grouping,
general view-aware string-key table, or native view scan in this prototype. Existing scan dictionary
predicate handling and the merged short-string packed-key specialization are independent mechanisms.
[ADR 0028](0028-dictionary-group-keys.md) retains its status and measured objections.

A future dictionary may store Binary or BinaryView values. Its indices identify values only within
that dictionary domain. A complete design must establish domain lifetime and identity, budget owned
values and translation tables, and use checked remapping or value comparison across batches, workers
and joins. Equal values must retain the ordinary content-based partition hash, including composite
keys and NULLs; worker-local IDs are not a shared hash domain. Changing dictionaries, high cardinality
and merge costs belong in the first test matrix, not in a fallback added after adoption.

### Final complete-query evidence and the separate ordinary optimization

The final controlled view comparison held ordinary selection-before-gather enabled in both arms and
changed only the view option. The observed view-eligible query was Q2 derived from TPC-H; the other queries
did not activate this path. The Q2 result did not justify adoption:

| Independent same-binary comparison | Q2 off wall | Q2 on wall | On/off wall | On/off CPU |
| --- | ---: | ---: | ---: | ---: |
| BinaryView, ordinary selection enabled in both arms | 0.909832 s | 0.912853 s | 1.003321 | 1.005333 |
| Extracted ordinary selection optimization, no BinaryView | 0.915622 s | 0.899885 s | 0.982813 | 0.997878 |

These are separate builds and campaigns: compare ratios within each row, not absolute times between
rows. In the view campaign, enabled was slower in nine of ten wall pairs and eight of ten CPU pairs.
A separate explicit-boolean launch control also found Q2 views slower, with wall/CPU ratios
1.014527/1.004979. Retention or transport savings alone did not establish an end-to-end advantage.

The independently extracted ordinary change is [PR #370](https://github.com/ydb-campus/antb1/pull/370),
with `--sort-selection-before-gather` off by default. It applies an existing selection mask and TopN
threshold before gathering ordinary columns. It contains no BinaryView implementation or dictionary
work. Its own full gates and static build passed; the original prototype's validation was not used as
proof of the extraction.

For that candidate, Q2 primary wall improved 1.72%, its all-ten wall median improved 1.28%, and eight
of ten paired wall runs favored enabled. The CPU point estimate improved only 0.21%, with six paired
wins and mixed order-stratum direction; no robust CPU reduction is claimed. The all-22 primary
geomeans were 0.998661 wall and 0.999361 CPU. Only Q2 activated partial-mask forwarding in the exact
candidate's separate activation check, so these small aggregate changes are not a broad optimization
claim. Q11 remains a caveat: primary wall was 0.83% worse and its all-ten median 1.61% worse, with
six paired losses. CPU signs split evenly and the all-ten mean wall favored enabled because disabled
had slow tails. That adverse wall observation is retained; it does not establish a repeatable
regression caused by the inactive forwarding path.

### Machine, method and limits of these measurements

The workload was derived from TPC-H at SF100 using the partitioned DECIMAL Parquet dataset. Both full
comparisons ran all 22 queries with the same static Release binary within each comparison, 16 engine
threads, CPU affinity 0–15 and a 700GB query-pool budget. The host had two Intel Xeon Gold 6338
2.00GHz sockets, 32 physical cores per socket, SMT2, about 1007.5 GiB RAM, Ubuntu 20.04.6 and
Linux 5.4.210-39.4 on x86-64. The selected CPUs are distinct physical cores on socket/NUMA node 0;
affinity does not imply a memory-binding policy, fixed clocks or an exclusive host.

Each query/arm had one warmup and ten adjacent measured pairs, with first position balanced five/five
and query order rotated. Primary wall is the mean of the fastest seven wall runs; CPU uses those
same seven runs, separately selected for each arm. All-ten medians/ranges and paired order strata are
retained. Native process wall and user-plus-system CPU include startup, execution, output and
shutdown; parent oracle checking is excluded. Runs used warm caches without cache flushing.
Separate diagnostics never enter native means. Both full suites passed their saved output oracles;
DECIMAL/integer/text comparisons are exact and only predefined DOUBLE result fields use the existing
1e-12 tolerance. Required order and multiplicity are checked without excusing LIMIT ties.

The final ordinary candidate used explicit `=0`/`=1` arguments in both arms and a frozen allowlisted
child environment: `PATH=/usr/bin:/bin`, `TZ=UTC`, `LC_ALL=C`, `LANG=C`,
`MIMALLOC_PURGE_DELAY=-1`, `PYTHONDONTWRITEBYTECODE=1`. The prototype full suites instead used
absent/present flags and wrappers with a temporary Git/Pixi PATH. The separate explicit-boolean
launch control used another PATH context and substantially different absolute times. It did not
isolate argument syntax as a unique cause across campaigns. No raw timings are pooled across these
builds or environments. The saved evidence includes binary/source/helper hashes and input
stat/footer identities; it is not a full dataset-content rehash on every phase.

The requested fastest-seven estimator drops three slow runs per arm and may change apparent effects.
Small changes on one host with ten pairs are not significance claims or general guarantees.
Public conclusions are limited to the measured local optimization and the absence of a demonstrated
view-path benefit. Private evidence, including all per-query timings and provenance, is retained under
`build/static/bench-results/tpch-sf100/strings-all22-t16-20261114/experiment/`
(`REPORT.txt`, `views/`, `candidate-suite/`, `candidate-independent-findings.txt`). Query text,
answers, plans, profiles and workload cardinalities remain outside this amendment.

### Next experiment and acceptance boundary

1. Keep the research branch separate from the ordinary PR. Preserve an ordinary baseline with
   selection-before-gather enabled before attributing any future saving to views or dictionaries.
2. Choose one complete consumer path that can avoid repeated string hashing/comparison or final
   conversion work. Establish the remaining cost before broadening eligibility; another descriptor
   transport microbenchmark alone cannot justify adoption.
3. For dictionaries, first implement a small domain/lifetime/remapping experiment using generated
   fixtures. Compare ordinary Binary, owned views and dictionary-backed values on equal semantics,
   including changing domains, sparse retention, duplicates, NULLs, high cardinality and merge.
   Keep the dictionary path opt-in; do not assume Parquet or worker dictionaries are interchangeable.
4. Before proposing a production path, require complete-query producer/consumer measurements,
   same-binary explicit-flag controls, matched plans, final Binary output, full-query CPU/RSS and
   budgeted retained capacity. Exercise aliasing, slices, reopening, deterministic ties, failed
   allocation cleanup and retained-result lifetime, then rerun the existing ownership gates and
   all-22 regression comparison. Stop if conversion, retention or remapping consumes the benefit.

This amendment changes the recorded implementation state and next experiments, not the ADR status,
SQL byte semantics, Arrow dependency, default representation or adoption decision.

This workload is derived from the TPC-H Benchmark and is not comparable to published TPC-H Benchmark
results, as this implementation does not comply with all requirements of the TPC-H Benchmark.
