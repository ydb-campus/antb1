# 4. Types, NULL and overflow semantics

Date: 2026-09-25

## Status

Accepted

## Context

- The tests use DuckDB as the oracle: SQL logic tests take their expected results from DuckDB, and differential and
  ClickBench data tests compare the two engines row by row. Every semantic difference either becomes a test failure
  or has to be registered and handled explicitly.
- ClickBench's Parquet files use unannotated BYTE_ARRAY columns for strings and store `EventDate` as an unsigned
  16-bit day number.
- Arrow's integer `sum` kernel returns int64 and silently wraps on overflow, and implicit casts from int64 to
  double lose precision above 2^53.
- DuckDB returns HUGEINT (128-bit) for SUM over integers, matches quoted identifiers case-insensitively, compares
  literals exactly and uses standard SQL NULL rules.

## Decision

antb1 follows DuckDB's semantics for everything in the supported subset:

- Identifiers: table and column names match ASCII case-insensitively, including quoted identifiers.
- Types: Parquet and Arrow types map to SMALLINT, INTEGER, BIGINT, USMALLINT, DOUBLE (FLOAT is widened), VARCHAR
  (raw bytes, byte-wise comparison, for both unannotated and UTF8 BYTE_ARRAY), DATE and HUGEINT, as listed in
  [sql-subset.md](../sql-subset.md#types). Other types are unsupported, and queries that use them are rejected.
  `EventDate` is read as DATE with `--clickbench`, or any USMALLINT or INTEGER column with `--column-type COL=DATE`.
- Literals are folded exactly at bind time. A literal outside the column type's range makes the comparison constant
  true or false for non-NULL values while NULL stays NULL; a decimal literal against an integer column becomes an
  equivalent integer comparison (`c > 1.5` becomes `c >= 2`; `c = 1.5` is never true). No comparison goes through a
  lossy cast.
- Integer SUM accumulates in 128 bits (`antb1::Int128`) and returns HUGEINT, represented as Arrow
  `decimal128(38, 0)`; it never overflows or wraps. Integer AVG accumulates the sum exactly in 128 bits and divides
  by the count once at the end, which keeps the DOUBLE result within about one ulp of the exact mean. MIN and MAX
  return the input type.
- NULL: aggregates skip NULLs; over zero rows COUNT returns 0 and SUM, AVG, MIN and MAX return NULL; a predicate
  that is NULL rejects the row.
- Every intentional difference from DuckDB is registered in the divergence table of
  [sql-subset.md](../sql-subset.md#divergences-from-duckdb) together with how the tests handle it.

## Consequences

- Results can be compared with DuckDB exactly (integers, dates, strings) or with a tight relative tolerance
  (doubles), which makes the oracle tests strong.
- Integer aggregates cost more than Arrow's native `sum` because of the 128-bit accumulator. That is the price of
  exact results.
- Because `__int128` is not an integral type for strict C++23 libstdc++, the code uses hand-written range checks,
  `__builtin_*_overflow` and `std::format` for it.
- HUGEINT values are written as JSON strings in `--format json` to keep them exact.
- Features beyond the subset must decide their semantics against DuckDB first and update this ADR or supersede it.
