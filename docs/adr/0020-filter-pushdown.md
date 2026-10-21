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
  - **Only the per-row path reached the data that motivated the work**, which is the honest reading: the
    dictionary path is exercised by the fixtures and by any file from a writer that emits `encoding_stats`.
    Reaching it for the rest means decoding the pages without Arrow's reader, which is a larger decision than
    this item and the same one ADR 0021's residual narrowing pass wants.
