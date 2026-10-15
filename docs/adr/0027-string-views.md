# 27. Short strings inline: the view layout for VARCHAR

Date: 2026-10-15

## Status

Proposed

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

One PR per id. The first stands alone and decides whether the rest is worth doing.

- **S1, perf(exec): gather VARCHAR by reference in the hash join.** The one site antb1 owns, with the lifetime rule
  enforced where the join retains rows. Measured before and after on the three-table join above and on all 22 at
  SF 100. **If it does not move `gather` and peak memory materially, this ADR stops here** and the string cost is
  ADR 0019's to carry.
- **S2, perf(exec): the inline-string key layout, inside ADR 0019's own hash table.** Not before it: the key layout
  is a property of that table, and building it against `arrow::compute::Grouper` is not possible — the grouper
  rejects view keys outright.
- **S3, docs(adr): record what S1 and S2 measured**, and either accept this ADR or move it to
  `Proposed (deferred)` with the numbers that say why.

Deliberately out of scope: decoding Parquet into `BINARY_VIEW` (ADR 0019 measured 10-16% and the decode is not
zero-copy); changing the engine's VARCHAR type; and late materialization, which is ADR 0016's and is a different
answer to the same question — carry a row reference rather than a cheaper value.

## Alternatives considered

- **Make `arrow::utf8_view()` the engine's VARCHAR type.** The clean version, and what a greenfield engine would do.
  Rejected for Arrow 25: `take`, `filter`, `sort_indices`, `Grouper` and every string function reject view types, so
  this means writing our own kernels for all of them before a single query gets faster, and `DispatchBest` will not
  quietly cast back. Worth revisiting when a later Arrow fills the compute layer in — that is this ADR's trigger to
  reopen.
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
