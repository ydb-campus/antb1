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
  - **Then every other column:** only the rows that pass are copied (strings from views); a batch with none is
    skipped without decoding.
  - **Errors:** as the high-level path reports them (an IOError naming the file, OutOfMemory, the footer checks). A
    filter's own errors pass through.
  - **Fallback:** other column types (nested, BOOLEAN, TIMESTAMP, HUGEINT) are not supported, and the high-level path
    reads them as before.
- **The executor** pushes the predicates of a part pipeline's scan into the scan, in a separate change. The logical
  plan and EXPLAIN are unchanged; the Filter operator keeps only what was not pushed.
- **Not pushed (first version):** late materialization's narrow scans (ADR 0016), whose row ids count the part's
  rows. Predicates over two columns, or over computed values, stay in the Filter operator.

## Consequences

- **Fewer bytes per row:**
  - the string columns a filter reads are scanned in place;
  - the values of rows that fail are never copied, for every column, whatever its type;
  - batches with no passing row skip their other columns.
- **Two read paths in `io`:** the high-level one for unfiltered scans and unsupported types, and the filtered one.
  The io tests check that a filtered scan returns exactly the unfiltered scan's rows that pass, for every supported
  type, NULLs, dictionary, plain and fallback encodings, pages smaller than a batch, and batch sizes from 1 row.
- **Follow-ups:**
  - evaluating string predicates once per dictionary entry for dictionary-encoded chunks;
  - skipping pages by statistics (granularity 2);
  - pushdown into late materialization's scans, with the scan reporting the positions of the rows that pass.
