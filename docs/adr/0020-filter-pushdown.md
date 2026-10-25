# 20. Filter pushdown into the scan

Date: 2026-10-01

## Status

Proposed

## Context

- **The string scans are bound by memory traffic.** Q20-Q22 (LIKE over long strings) were measured with hardware
  counters (2026-10-01):
  - instructions are constant from 1 to 128 threads, while IPC falls and last-level cache misses triple;
  - NUMA placement makes no difference;
  - antb1 is 1.25-1.7× slower than DuckDB on 1 thread and 1.85-2.95× on 128.
- **The traffic is a copy.** Arrow's high-level Parquet reader (`parquet::arrow::FileReader` → `RecordBatchReader`)
  copies every string into a `BinaryArray`, and the filter then reads it again.
- **The low-level reader avoids it.** `parquet::ByteArrayReader::ReadBatch` returns `parquet::ByteArray` views into
  the decompressed page. Decoding a column that way and matching a pattern in place, against the high-level path, on
  the full ClickBench data (a local experiment):

  | Column | Faster on 1 thread | Faster on 128 threads |
  | --- | ---: | ---: |
  | URL | 1.35× | 1.81× |
  | Title | 1.31× | 2.18× |
  | SearchPhrase | 1.88× | 1.59× |

  Decompression is the same either way. Decoding into Arrow's `BINARY_VIEW` instead saved only 10-16%, because its
  builder copies strings longer than 12 bytes.
- **This is filter pushdown into the scan.** antb1 already pushes the WHERE predicates on scan columns down to row
  groups: `exec::KeptParts` (#50) skips a row group whose footer min/max rules out every row. The same predicates can
  act at three granularities:
  1. **Row groups,** by footer statistics: exists.
  2. **Pages,** by page statistics or the page index: skip decompressing a page that cannot match. Later.
  3. **Rows,** by evaluating the predicates on the decoded values before they are copied, then copying only the rows
     that pass, for every scanned column. This ADR.
- **The module boundary.** The predicates' semantics (LIKE, DuckDB's comparisons) live in `exec`, which must not use
  `io`, and the decoding lives in `io`, which must not use `exec`.
- **Views are short-lived.** A view is valid only until its column reader reads its next page, and the pages of
  different columns do not line up.

## Decision

- **An interface in `plan`** (`src/plan/include/antb1/plan/table.h`), which both modules use, inverts the
  dependency: `exec` implements the predicates, `io` applies them to what it decodes. No module edge is added, and
  no code moves.
  - **`plan::ScanValues`:** one column's decoded values. A fixed-width column is an Arrow array of its engine type; a
    VARCHAR column is a `std::string_view` per row plus validity, valid only during the call.
  - **`plan::ScanFilter`:** the columns it reads (positions in the scan's fields), and
    `Apply(column, values, offset, selected)`, which clears the selection bits of the rows that fail that column's
    predicates.
    - **One column per call:** every pushed predicate reads one column, and the WHERE conjunction is their AND, so a
      table can apply each column's predicates on its own.
    - **A piece at a time:** a column can be applied in pieces while its views are valid.
  - **`plan::Table::supports_scan_filter(fields)`** (false by default), and a `ScanPart` that takes a filter and
    returns only the rows that pass, in part order. A batch that loses every row is not returned.
- **`io::ParquetTable`** implements it for flat INT16, INT32, USMALLINT, DATE, BIGINT, DOUBLE (also stored as
  FLOAT) and VARCHAR columns (`src/io/filtered_scan.{h,cc}`), through `ParquetFileReader` → `RowGroupReader` →
  `TypedColumnReader`. For every batch:
  - **The filter's columns first:** fixed-width columns are decoded whole and the filter applied. String columns are
    decoded page piece by page piece; the filter is applied to the piece's views, and the values of the rows still
    selected are copied.
  - **Then every other column:** only the rows that pass are copied (strings from views). A batch with none is
    skipped: nothing is copied, though a page that is skipped only in part is still decompressed and decoded.
  - **Errors:** as the high-level path reports them (an IOError naming the file, OutOfMemory, the footer checks). A
    filter's own errors pass through.
  - **Fallback:** other column types (nested, BOOLEAN, TIMESTAMP, HUGEINT) are not supported.
    `supports_scan_filter` needs every scanned column to be supported, so a scan with one such column is not
    filtered at all, and the high-level path reads it as before.
    Update (2026-10-02): DECIMAL columns ([ADR 0021](0021-decimal-semantics.md)) are not supported either. A scan
    that reads one gets no pushdown, and a predicate on one skips no row group (part statistics cover only
    integer-valued columns), until the decimal64 item of ADR 0021.
    Update (2026-10-21): both are closed. The decimal64 item gave a DECIMAL column pushdown, and its
    part-statistics item gave `Table::part_stats` a DECIMAL's unscaled min and max, so a predicate on one skips
    row groups -- from an INT32- or INT64-backed column only, because a FIXED_LEN_BYTE_ARRAY decimal's
    statistics are the ones some writers ordered as unsigned bytes.
- **The executor** pushes the predicates of a `Filter` directly over a part pipeline's scan into the scan, in a
  separate change (`exec::MakeScanFilter`, `src/exec/include/antb1/exec/scan_filter.h`): those that read one
  column against literals (`<op>`, `[NOT] IN`, `[NOT] LIKE`, `IS NOT NULL`).
  - Fixed-width columns are evaluated with the Filter operator's own evaluator, VARCHAR views with LIKE's matcher
    and bytewise comparisons.
  - The logical plan and EXPLAIN are unchanged; the Filter operator keeps only what was not pushed, and
    `explain --analyze` shows the count on the `Scan`.
  - **The order the scan applies the columns in is chosen, not inherited from the query** (`Order`, below).
- **Late materialization's narrow scans** (ADR 0016) are filtered too, on their early columns. Their row ids name a
  row's position in the part, which a filtered scan no longer gives by counting, so a filtered `ScanPart` can report
  them: with `positions`, every batch ends with a `position` column (INT64), each row's position in the part.
- **Not pushed:** predicates over two columns, or over computed values, stay in the Filter operator.

## Consequences

- **Fewer bytes per row:**
  - the string columns a filter reads are scanned in place;
  - the values of rows that fail are never copied, for every column, whatever its type;
  - batches with no passing row skip their other columns.
- **Two read paths in `io`:** the high-level one for unfiltered scans and unsupported types, and the filtered one.
  The io tests check that a filtered scan returns exactly the unfiltered scan's rows that pass, for:
  - every supported type, required and nullable columns, and the DATE overrides;
  - dictionary, plain and fallback encodings, pages smaller than a batch, and batch sizes from 1 row;
  - parts of several files.

  They also check that a corrupt page is an IOError naming the file.
- **Deferred** until a dataset needs it (the ClickBench files have no page index and large pages):
  - **skipping pages by statistics** (granularity 2), for files written with page indexes and smaller pages.
- **Evaluating a string predicate once per dictionary entry: built 2026-10-21**, and the shape it had to take is
  worth recording because the obvious form of it is not available.
  - **Arrow 25 will expose a column chunk's dictionary only for a chunk it can prove is fully dictionary
    encoded, and it proves that from the optional `encoding_stats` metadata alone.** Measured: the
    TPC-H-derived files on hand carry no `encoding_stats`, so both doors --
    `RowGroupReader::ColumnWithExposeEncoding` and `RecordReaderWithExposeEncoding` -- decline for them, even
    though their `encodings` list is `PLAIN_DICTIONARY` with no `PLAIN` at all, which for a v1 writer proves
    every data page is dictionary encoded. Files Arrow itself wrote, including this repository's fixtures,
    do carry it and are granted. A gate of our own is worse than useless there: Arrow's is the one that
    decides whether `ReadBatchWithDictionary` throws, and it throws rather than degrading on the first
    plain-encoded page, so the only sound sequence is to ask and read back the answer.
  - So **two paths, and the measurement is what justifies keeping both.** Where Arrow grants the dictionary,
    `plan::ScanValues` carries it with one index per row and `exec`'s scan filter asks each predicate once per
    distinct value -- every kind of predicate, LIKE included, since a dictionary says nothing about what is
    being asked. Where it does not, the per-row path got faster on its own account: a short `IN` list is now
    walked with a length test first rather than binary-searched, because the search calls `std::less<>` on
    string views, which is an out-of-line `memcmp`. Over a seven-value CHAR column, 20M rows: **12.5 ns a row
    for the search, 5.6 for the walk, 0.19 through the dictionary.**
  - **Measured end to end** at SF 100, four threads, three repeats interleaved: the bare predicate over 600
    million rows 3.37 s to 2.21 s (**0.655x**), Q12 **0.775x** and Q19 0.922x, geomean 0.989x over the 22
    queries. Q12 was the worst query against DuckDB at 3.35x of its time and is now 2.60x. Q3, Q7, Q8 and Q14
    looked up at three repeats and come out at 1.004x over five -- noise.
  - **The dictionary pointer the reader hands back is not stable across the pieces of one column chunk**,
    although the format gives a chunk one dictionary page. That was found by asserting the opposite: the views
    were first cached and rebuilt when the pointer changed, a reviewer pointed out that an equal pointer does
    not mean equal values once a dictionary has been freed and its successor allocated over it, and an
    ANTB1_CHECK written to pin "one dictionary per reader" fired on the first run. So the comparison had never
    skipped a rebuild, and the views are now rebuilt every piece -- the same work, without the reasoning.
  - **Only the per-row path reached the data that motivated the work**, which is the honest reading: the
    dictionary path is exercised by the fixtures and by any file from a writer that emits `encoding_stats`.
    Reaching it for the rest means decoding the pages without Arrow's reader, which is a larger decision than
    this item and the same one ADR 0021's residual narrowing pass wants.
    - **Correction (2026-10-25): it does not, and the reason given above for not deciding this ourselves was
      wrong on a point of fact.** This entry said "a second gate of our own could only disagree with Arrow's,
      and Arrow's is the one that governs whether `ReadBatchWithDictionary` throws". Arrow's gate and the throw
      are not the same question. `ColumnWithExposeEncoding` reads `encoding_stats` and, when it is satisfied,
      sets one field: `exposed_encoding_`, which Arrow 25's reader **sets, returns and acts on nowhere else**
      (`column_reader.cc:941,977,986` are its only three occurrences). What governs the throw is
      `current_encoding_ != RLE_DICTIONARY`, tested per data page. So the advisory flag can be left to Arrow
      while the real question -- has this chunk a page that would throw -- is answered from `encodings`, which
      Parquet **requires** of every column chunk where `encoding_stats` is optional. See the item below.
- **Ordering the columns a scan filters: built 2026-10-21.** The scan applies the filter's columns one at a
  time, each narrowing the selection the next one sees, so a column placed late does its per-row predicate work
  only over the rows the earlier ones left. Until this item the order was the order the predicates appeared in
  the `WHERE` clause, which made **the spelling of a query decide its cost**: one filter over 600 million rows at
  four threads, two bounds on a DATE column and a two-value `IN` on a VARCHAR one, measured **1.102 s written
  dates-first and 2.507 s written the other way round, a 2.28x penalty for a reordering that changes no answer.**
  Both spellings now read 1.103 s.
  - **The rule is Hellerstein and Stonebraker's rank ordering for a conjunction**: ascending `cost / (1 -
    selectivity)`, which is optimal for independent predicates applied in sequence, and the choke point Dreseler
    et al. call predicate positioning (PVLDB 13(8)). It is compared as a cross product, so no division and no
    floating point enters a planning decision, and the sort is **stable**, so columns of equal rank keep the
    order the query wrote them in -- `std::sort` would leave their order to the standard library and so to the
    platform, and an EXPLAIN must not depend on that. That last property is the one thing here no test can
    observe, since a single platform cannot disagree with itself.
  - **Only the work a late position avoids belongs in the weight**, which is narrower than it first looks and was
    got wrong before a measurement corrected it. `io` reads every row of every one of the filter's columns
    whatever the selection says, and a fixed-width column's predicate is an Arrow kernel over the whole array, so
    neither its decode nor its predicate moves with its position. What does move is a VARCHAR column's per-row
    predicate work, which skips the rows already rejected. **So a fixed-width column belongs first even when it
    removes far less than a VARCHAR one**: measured at SF 100 over 600 million rows, a fixed-width column
    removing a seventh of the rows against a VARCHAR column with two predicates removing almost all of them runs
    **2.13 s with the fixed-width column first and 2.30 s with it last**. A model that charged each column its
    read ordered that pair the other way round and was 8% slower on it.
  - **The weights are measured, not guessed**, one predicate on one column of 600 million rows at four threads: a
    DATE column with one range bound 0.389 s, **with two bounds also 0.389**, a VARCHAR column `=` one value
    1.560 s, `IN` two values 2.156 s, `LIKE` a suffix 2.742 s. Only differences between those are avoidable, so
    one value compared is 2.156 - 1.560 = 0.596 s and the rest of the 1.560 is decode. In units of a hundredth of
    a second: **60 for a value compared, 180 for a pattern matched, and 1 for all of a fixed-width column's
    predicates** -- 1 rather than 0 because two bounds cost what one bound does to within the ~0.005 s those
    repeats agree to, which bounds the avoidable part below one unit rather than at zero. Zero would also make
    the comparator intransitive, so a `static_assert` holds the weights positive.
  - **The selectivity defaults are coarse on purpose** and deliberately separate from `plan::PredicateRows`
    (`src/plan/join_order.cc`): that one estimates a relation's rows for the join order and divides by a column's
    domain where the footers give one, while this one ranks columns within a scan with no statistics at hand.
    Neither term of the rank dominates the other -- what a column removes spans 10 to 100 of these defaults, and
    nothing at all for the kinds assumed to keep every row, as wide a span as the weights. **The next real
    improvement here is this column's own part statistics**, which the scan could reach through
    `plan::Table::part_stats` (ADR 0021's part-statistics item made them comparable for DECIMAL).
  - **A dictionary-encoded VARCHAR column is far cheaper than these weights** (0.19 ns a row against 5.6,
    above), but whether Arrow will grant the dictionary is known only when the chunk is read, which is after this
    order is fixed. So the weights are the undiscounted ones and such a column is ordered more pessimistically
    than it deserves -- a known, bounded inaccuracy, not a correctness question: **no order changes an answer**,
    because every predicate only clears bits of one shared selection, which is why this item is testable by the
    order alone and why the existing equivalence test still covers the answers under any permutation.
  - **Measured end to end** at SF 100, four threads, three interleaved repeats: Q12 **0.688x**, Q16 0.948x,
    geomean **0.976x** over the 22 queries, peak memory unchanged. Two queries looked like regressions and were
    not, which is the same trap ADR 0021's acceptance recorded: Q8 read 1.030x at three repeats and 0.974x at
    four rotated ones, and Q3 read 1.024x with its B minimum above its A maximum, yet 1.005x rotated -- and
    could not have moved either way, since each of its scans has a single filter column for this item to order.
  - **What the tests pin, and what they do not.** Both terms of the comparator, the ordering of the predicate
    kinds by weight, that a fixed-width column's predicates are free of position, and that a second predicate on
    a VARCHAR column adds its own weight: each fails a case if changed, 12 of 13 mutations caught, the
    thirteenth being `stable_sort` to `sort`, which no single-platform test can see. The weights' exact values
    are pinned only as the bands in which they keep those orders -- a cost model is an approximation, and
    pinning each constant to the unit would turn a re-measurement into a test rewrite. Which mutations are
    observable at all was found by **enumerating the model's reachable column shapes and every ordered pair of
    them** (1198 shapes, 1.4 million pairs), the same enumeration that showed the comparator is exactly
    `cost / removed` compared over the rationals extended by the infinity at `removed == 0`, and so a strict
    weak ordering as `stable_sort` requires.

- **Refused (2026-10-24): dropping a scan column whose only reader is a pushed predicate.** The waste is real
  and visible in the code. A filter column is decoded for the predicate and then **also materialised for the
  batch**: for a VARCHAR, `FilteredScanReader` pushes a row id and a `BinaryBuilder` append for every row still
  selected *at that column's turn*, and `Finish` walks them a second time when a later column -- a key filter's
  included -- rejected any of them (it returns the buffer directly otherwise, and this ADR's own ordering item
  puts a VARCHAR last, so the second walk is the exception); for a fixed-width column `Compact` copies the
  survivors. Where nothing above the scan reads that column, all of it is thrown away.
  - **ADR 0016's late scan is the dual notion, not the same one.** A predicate-only column is *early* in
    `LateSplit`'s terms, so it is already in what `ReadFields(scan, late)` returns -- the droppable set is a
    subset of that, not its complement. And 0016's `NullArray` placeholder rides on a **narrowed `io` request**,
    where this needs `io` to read a column and not return it: a contract that does not exist.
  - **Measured, and it does not pay.** A prototype that skips materialising *every* filter column with no
    gating leaves **7 of the 22** with correct answers -- Q1, Q5, Q9, Q11, Q13, Q14 and Q15, the queries no
    filter column of which is read above the scan. At SF 100 on 4 threads, 5 repeats interleaved and rotated,
    those 7 go **37.214 s to 36.883 s**: a saving of **0.331 s**, which against the suite's ~99 s is about
    **0.997x over the 22**. Q1, the query this was proposed for, reads **0.997x** on its own; the largest clean
    reading is Q14's 0.989x. Q15 reads 0.979x, the best of the seven, and **cannot have moved at all**: it has
    no pushed predicate, its filter being on a derived table. One of the 7 is therefore noise and the honest
    saving comes from 6.
  - **That bound is per query the prototype answers correctly, not over the 22.** A query disqualified because
    *one* of its filter columns is read above may hold others that are not; the labelled version a correct
    implementation needs would drop those and gain there, while this prototype answers it wrongly and it enters
    the figure above at 1.000x. So the ceiling on the other 15 is **unknown**, not 1.000x.
  - **The expensive path is unmeasured on this workload rather than unreachable**, which is the distinction to
    keep. The per-row `BinaryBuilder` append is the costly half, and the two queries with a droppable VARCHAR --
    Q19's `l_shipinstruct` and `l_shipmode`, Q10's `l_returnflag` -- are both answered wrongly by the ungated
    prototype, so its cost was never measured. Q19's pushed conjuncts are **re-read by the residual** that OR
    factoring leaves above the scan (ADR 0022's J3 update records that shape), so its columns are not droppable
    at all. Q10's `l_returnflag` **is** droppable: what disqualifies it is sharing a scan pipeline with a
    key-filter column that is a grouping key, and since `MakeKeyScanFilter` refuses VARCHAR a key filter's
    column is always fixed-width -- so a labelled version would drop the VARCHAR and keep the key column, and
    would reach the path this measurement could not.
  - **And `io` cannot decide this alone at all.** It is told which columns to read and which to filter on, never
    which are read above the scan -- the prototype's 15 wrong answers are proof of that for pushed predicates by
    themselves (`SELECT x FROM t WHERE x > 5` filters and projects the same column). ADR 0029's conjunction of a
    pushed predicate set with a key filter adds a **second, independent** reason: a scan then sees one
    `ScanFilter` and cannot tell a predicate's column from the join key's, and that column is read above by
    construction -- as the join's key for D1, as the aggregate's grouping key for D5. So the planner would have
    to label the droppable positions whatever happened to the conjunction.

- **Taken after all (2026-10-25): a VARCHAR column read only by a pushed predicate is read as
  dictionary indices and never materialised.** The labelled version the refusal above named, which
  that entry could not measure, and it is worth far more than the ceiling the unlabelled prototype
  put on it: **0.971x of the 22** at four threads against 0.997x, because the two halves pay only
  together.

  | | 4 threads | 14 threads |
  | --- | ---: | ---: |
  | Q19 | **0.688x** | **0.771x** |
  | Q10 | **0.789x** | **0.824x** |
  | Q13 | 0.924x | 0.930x |
  | Q21, Q06 | 0.960x, 0.968x | 0.961x, 0.990x |
  | the 22, geomean | **0.963x** | **0.972x** |

  5 repeats interleaved and rotated, both arms from one tree built by `pixi run static` (ADR 0033),
  all 22 answers byte-identical at both thread counts. Peak memory does not move (14,161 to 14,201
  MiB at four threads, 15,154 to 15,188 at 14): Q18 sets it and has no droppable VARCHAR. Nothing
  regresses beyond the host's +-1.4% noise -- the widest are Q01 1.019x and Q04 1.008x, both with no
  VARCHAR predicate at all, which is what calibrates that floor.

  **A review round moved these figures, which is worth recording because the direction is unusual.**
  The first implementation measured 0.971x and 0.759x on Q19; the version here is 0.963x and 0.688x,
  because a finding pointed out that the branch which skips a column's values still filled a list of
  its surviving rows -- write-only, since no array is finished for such a column, and so exactly the
  per-row work the item exists to remove.

  **Against DuckDB this moves the suite's two worst queries off the top of the list.** Composing
  these ratios with a same-session DuckDB comparison (rather than dividing arms across sessions,
  which flatters the branch): Q10 1.698x to about **1.340x**, Q19 1.632x to about **1.123x**,
  Q13 0.999x to **0.923x**, and the 22 from 1.249x to about **1.203x** of DuckDB's geomean. Q10 was
  the worst query in the suite and is no longer. These are compositions of two measured ratios and
  not a measurement; and the DuckDB arm was a dynamic `ci-release` build where these figures are
  static ones -- [ADR 0033](0033-standalone-static-binary.md) measures static at 0.973x of the
  `bench` preset and `ci-release` within 1% of `bench` -- so the static standing against DuckDB is
  about three points better than the number above.

  - **Why the refusal's 0.997x was right about what it measured and wrong about the item.** The
    refusal skipped the *copy into the batch* and kept everything else. That copy is the smaller
    half: on a 4-value dictionary column over 600 million rows, one equality predicate measures
    2.49 ns a row through views against 0.99 ns through the dictionary, while **materialising the
    column densely is 10.83** -- so skipping the copy alone leaves the per-row view fill and the
    compare in place. Doing both, which is possible only where a dictionary answers the predicate,
    measures **0.646x** on that scan. Each half alone is noise; the item is their conjunction, and
    the refusal had no way to see that because it never reached a dictionary on this data.
  - **Three things had to be true at once**, which is why it took a correction to this ADR to find:
    1. **`io` must reach the dictionary at all.** It could not on any file this project benchmarks,
       for the reason the correction above records: Arrow's door reads the optional `encoding_stats`
       and DuckDB, which wrote this data, emits none. `io` now decides from the **required**
       `encodings` where Arrow declines (`EveryDataPageIsDictionary`). Only a **v1** dictionary page
       proves it -- v1 marks the dictionary page and its data pages alike, so a chunk listing
       `PLAIN_DICTIONARY` and nothing else holds no plain page, whereas v2 marks the dictionary page
       `PLAIN` and leaves `{PLAIN, RLE_DICTIONARY}` ambiguous. v2 files keep Arrow's door, which
       grants them, so the two paths do not overlap and neither is dead.
    2. **The planner must say which columns nothing above reads.** `FilterNode::needed_above`, the
       demand recorded in the moment before the Filter's own predicates join it -- the same field and
       the same capture point as ADR 0026's `JoinNode::needed_above`, with both of that item's
       review findings built in from the start (intersected with the node's own outputs, and
       replaced rather than appended). It is a **std::optional**, where the join's is a plain vector,
       because an empty set is the *valuable* case and not the missing one: a `COUNT(*)` with a
       `WHERE` reads nothing above its Filter, so every column its scan reads is read to be compared
       and for nothing else. The join's field gives that case up.
    3. **The position must survive.** Every operator above reads a column by the position
       `ResolvePositions` gave it, so a column cannot simply be removed from the middle of a scan's
       output. `plan::Table::ScanPart` takes a `returned` count and the table puts the returned
       fields first; the operator then puts them back where they belong, with an all-NULL array of
       the field's **declared** type in each gap. Not the `null` type ADR 0016's late split uses: a
       late column is re-read by a node that knows it is a placeholder, where this one can flow into
       a join build and be gathered past the join, and an `ANTB1_CHECK` in the gather said so on the
       first run.
  - **Where an empty `strings` is a trap, and why the condition is the consumer's.** With a
    dictionary the filter reads `dictionary` and `indices` and never `strings`, so the views need not
    be written at all -- but `KeepByDictionary` **declines** when the dictionary holds more distinct
    values than the piece has rows, falling back to one compare a row. Skipped views under any
    weaker condition would leave that fallback reading values nobody wrote. So `io` skips them per
    piece under exactly that condition, `ScanFilter::Apply` accepts the second shape only when it
    holds, and refuses a dictionary larger than the piece with no views -- a shape no table may
    produce, asserted rather than commented.
  - **What the tests reach.** The proof itself is unit-tested over the four shapes Arrow's writer
    can produce (v1 dictionary, v2 dictionary, a real fallback, no dictionary), each asserted against
    **both** doors, with the invariant that between them they reach every fully dictionary-encoded
    chunk. The chain from SQL to the metric is pinned by an integration case over a file the **test
    oracle** writes, because Arrow's writer cannot produce one without `encoding_stats` and that is
    the only situation in which the new proof decides anything: it asserts that Arrow's door declines
    for that file and that `cols_filter_only` and `dict_pieces` are what they should be. Removing
    either half of the item fails that one case. Both metrics exist for the reason ADR 0026's C8a
    learned the hard way -- a scan that quietly stops doing this answers exactly the same -- and the
    `explain --analyze` goldens now assert them rather than a reader checking once.

This workload is derived from the TPC-H Benchmark and is not comparable to published TPC-H Benchmark results, as
this implementation does not comply with all requirements of the TPC-H Benchmark.
