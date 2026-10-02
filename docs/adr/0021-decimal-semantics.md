# 21. DECIMAL semantics as in DuckDB

Date: 2026-10-02

## Status

Proposed

## Context

- **17 of the 22 queries derived from TPC-H need DECIMAL:** Q1-Q3, Q5-Q11, Q14, Q15, Q17-Q20 and Q22 (query numbers
  in this ADR are theirs, not ClickBench's). Every DECIMAL column of the tables derived from TPC-H (prices,
  quantities, discounts, taxes, balances and costs) is DECIMAL(15,2), and DuckDB writes them as INT64. The queries
  compute with these columns, sum and average them, compare them with literals, with each other and with averages,
  group and sort by them and print them.
- **Some answers hinge on the last digit or bit:**
  - Q2 and Q15 test DECIMAL values for equality with independently computed aggregates;
  - Q11 and Q20 compare DECIMALs of different scales, or a DECIMAL with an integer column;
  - Q17 and Q22 compare a DECIMAL column with a DOUBLE computed from an average, so the last bit of the average
    decides which rows pass.
- **antb1 has no DECIMAL type.**
  - A Parquet DECIMAL column has no engine type (`src/plan/types.cc`) and exits 4 at its first reference, except
    DECIMAL(38,0), which reads as HUGEINT: HUGEINT is decimal128(38,0) and stands for DECIMAL(38,0), limited to 38
    digits (divergence D9).
  - A decimal literal is accepted only where no DECIMAL value reaches the result: compared with an integer operand
    (folded exactly, [ADR 0004](0004-types-null-overflow-semantics.md)), a DOUBLE operand or a FLOAT column, in `/`
    and `//` (both give DOUBLE), and in arithmetic or CASE next to a DOUBLE. As a select constant, with an integer
    operand in `+`, `-`, `*` or `%`, or as another CASE value it exits 4.
- **The harness hides wrong cents.** The test runner's DuckDB adapter reads a DECIMAL with a scale as a DOUBLE of
  class R, compared within a relative 1e-9, and names its type a bare `DECIMAL`
  (`tests/slt/runner/duckdb_engine.cc`). On a value with ten integer digits that tolerance is at least one whole
  unit, a hundred cents, and a wrong width or scale passes as well.
- **DuckDB's rules are its own.** The oracle is DuckDB 1.5.5, the locked libduckdb. Its DECIMAL typing is neither the
  SQL standard's nor Arrow's: widths are capped at 18 digits while every input has at most 18 digits, division gives
  DOUBLE, and CASE and comparisons take different common types. Every rule below was probed with DuckDB 1.5.5 on our
  own data, and its formulas were read in DuckDB's source.
- **Storage:**
  - Arrow reads a Parquet DECIMAL with p ≤ 38 (INT32, INT64, FIXED_LEN_BYTE_ARRAY or BYTE_ARRAY) as decimal128(p,s)
    by default, and DuckDB reads all four as DECIMAL(p,s). Reading the smallest type (decimal32 for p ≤ 9, decimal64
    for p ≤ 18) is a reader option. A file's ARROW:schema overrides both: Arrow 25 restores the decimal type it
    records, decimal32, decimal64 and decimal256 included (probed with DECIMAL(5,3), DECIMAL(15,2) and DECIMAL(38,0)
    columns), while DuckDB ignores the ARROW:schema.
  - antb1's HUGEINT already runs on decimal128: Int128 arithmetic with range checks, exact sums, sorting, top-N,
    grouping and MIN/MAX.
  - Arrow 25 has no decimal32 or decimal64 arithmetic kernels.
- **INTERVAL waits.** None of the 22 queries in DuckDB's dialect uses INTERVAL (they cast date strings instead), so
  the maintainer deferred it on 2026-10-02. This ADR covers DECIMAL only.

## Decision

antb1 follows DuckDB 1.5.5 for DECIMAL. In the rules, p is the width (precision) and s the scale. The examples use a
table of our own: `price` and `qty` DECIMAL(15,2), `rate` DECIMAL(5,3), `n` INTEGER, `b` BIGINT and `d` DOUBLE.

This amends ADR 0012's list of unsupported constructs and three points of ADR 0004; both ADRs keep their status:

- ADR 0004's list of types;
- its nearest double for a decimal literal against a DOUBLE operand (rule 11);
- its rule that no comparison goes through a lossy cast and that a number DuckDB types as DOUBLE is folded exactly:
  a DECIMAL operand compared with a DOUBLE operand or with such a number is converted to DOUBLE as DuckDB converts it
  (rule 8) and compared in DOUBLE (rule 11). So `price = 1e-1` holds for a price of 0.10, as in DuckDB, where the
  double nearest to 0.1, folded exactly into DECIMAL(15,2), would match no row.

1. **Type.** DECIMAL(p,s), with 1 ≤ p ≤ 38 and 0 ≤ s ≤ p, holds an unscaled integer v with |v| ≤ 10^p - 1 and
   stands for v / 10^s. It is named `DECIMAL(p,s)` without spaces, as DuckDB prints it, in `antb1 schema`, EXPLAIN
   and the test adapters. A Parquet DECIMAL column with p ≤ 38 reads as DECIMAL(p,s), whatever its physical type; a
   larger p stays unsupported (exit code 4).
2. **DECIMAL(38,0) and HUGEINT.** A Parquet DECIMAL(38,0) column reads as DECIMAL(38,0), as in DuckDB, and no longer
   as HUGEINT. HUGEINT stays the type of integer SUM and of integer literals beyond BIGINT, with divergence D9's
   range. The type names, result types and arithmetic of such columns change (`c + 1` on such a column `c` is
   DECIMAL(38,0), no longer HUGEINT): a breaking change, labelled in the PR that makes it.
3. **Literals.** A number with a point, no exponent and at most 38 digits is DECIMAL(digits, digits after the
   point); leading zeros count and the sign does not.
   - `2.5` is DECIMAL(2,1), `.125` DECIMAL(3,3), `007.50` DECIMAL(5,2) and `5.` DECIMAL(1,0).
   - An exponent or more than 38 digits makes the number DOUBLE (`2.5e0`). Integer literals stay INTEGER, BIGINT or
     HUGEINT by value.
   - A decimal constant is named by its value as rule 15 prints it: `007.50` is named `7.50`.
4. **Integers next to a DECIMAL** count as DECIMAL(5,0) (SMALLINT, USMALLINT), (10,0) (INTEGER), (19,0) (BIGINT)
   or (38,0) (HUGEINT). An integer literal counts by its type and does not shrink to fit: `7` is (10,0).
5. **`+` and `-`:**
   - s = max(s1, s2) and p = max(p1 - s1, p2 - s2) + s + 1;
   - the 18-digit cap: if p > 18 while p1 and p2 are both at most 18, p is 18 (DuckDB keeps such results in 64
     bits); the 38-digit cap: if p > 38, p is 38;
   - an operand with a smaller scale is first rescaled to DECIMAL(p,s);
   - `price + qty`, `price + n` and `price + 2.5` are DECIMAL(16,2), `price + rate` is DECIMAL(17,3), `price + b` is
     DECIMAL(22,2) (BIGINT has 19 digits, so no 18-digit cap), and DECIMAL(18,2) plus DECIMAL(18,2) is
     DECIMAL(18,2).
6. **`*`:**
   - s = s1 + s2; a scale above 38 is a bind error with DuckDB's message (`Needed scale 40 to accurately represent
     the multiplication result, …`);
   - p = p1 + p2; if p > 18 while p1 and p2 are at most 18 and s < 18, p is 18; if p > 38, p is 38;
   - `price * qty` is DECIMAL(18,4) (30 digits capped), `price * n` DECIMAL(18,2), `price * 1.5` DECIMAL(17,3),
     `rate * rate` DECIMAL(10,6) and `price * b` DECIMAL(34,2);
   - unary `-` keeps the type.
7. **Overflow is an error.** A result of `+`, `-` or `*` must satisfy |v| ≤ 10^p - 1 for its p, which only a capped
   p can break. Otherwise the query fails with an execution error (exit code 1) and DuckDB's message, which names
   the capped width and the unscaled operands:
   - `Overflow in addition of DECIMAL(18) (…)`, with `subtract` (DuckDB's word) or `multiplication` for the other
     operators, and `DECIMAL(38)` at the other cap;
   - a rescaled operand that does not fit a capped type fails first, with DuckDB's conversion error
     (`Casting value "…" to type DECIMAL(18,2) failed: value is out of range!`).

   Nothing falls back to DOUBLE: `price * qty` fails as soon as an unscaled product needs more than 18 digits, in
   both engines, unless a comparison around it folds to a constant (rule 11, divergence D14).
8. **`/` and `//`** give DOUBLE.
   - A DECIMAL operand becomes a double as DuckDB converts it: v / 10^s when |v| ≤ 2^53 or s = 0, otherwise
     (v div 10^s) + (v mod 10^s) / 10^s, where div and mod truncate toward zero and each part is converted on its
     own. This two-step conversion is not always correctly rounded. For p > 18 DuckDB converts a 128-bit integer
     with its own formula, which antb1 already reproduces for FLOAT comparisons (`src/plan/literal.cc`).
   - `/` divides as for DOUBLE: a zero divisor gives `inf`, `-inf` or NaN.
   - `//` on a DECIMAL is the same division, not truncated, and NULL for a zero divisor: `price // 4` is 3.0625
     when `price` is 12.25.
9. **`%`** computes in rule 10's common type (s = max(s1, s2) and p = max(p1 - s1, p2 - s2) + s), takes the sign
   of the dividend and is NULL for a zero divisor: `price % 4` is DECIMAL(15,2), and `-7.25 % 2` is -1.25 as
   DECIMAL(12,2). Where that p would exceed 38, DuckDB types `%` as DOUBLE at bind time (C's `fmod` of rule 8's
   doubles), still NULL for a zero divisor.
10. **Common types:**
    - comparisons and IN lists take s = max(s1, s2, …) and p = min(38, max(p1 - s1, p2 - s2, …) + s) over all
      their operands; DOUBLE with any number gives DOUBLE;
    - CASE values take the same type while it fits in 38 digits. Beyond 38, two DECIMALs keep their integer digits
      (p = 38 and s = 38 - max(p1 - s1, p2 - s2), values rounded half away from zero), and a DECIMAL with an integer
      keeps its scale (p = 38; a value that does not fit fails with DuckDB's conversion error);
    - `CASE WHEN n > 0 THEN price ELSE 7 END` is DECIMAL(15,2); with `rate` instead of `price` it is DECIMAL(13,3),
      because 7 counts as INTEGER; `ELSE b` gives DECIMAL(21,2), `ELSE 2.5` DECIMAL(15,2) and `ELSE d` DOUBLE.
11. **Comparisons:**
    - **With an integer or decimal literal:** folded exactly into the operand's (p,s) at bind time, as integer
      columns are folded today (ADR 0004). `price < 12.345` becomes `price <= 12.34`, `price >= 12.345` becomes
      `price >= 12.35`, and `price = 12.345` is never true. A literal beyond the type's range makes the comparison
      constant, NULL still rejected, and the operand is then not computed (divergence D14). Folding is never an
      error. An IN list of such literals is folded value by value.
    - **Two operands that are not literals,** DECIMAL with DECIMAL or with an integer (`price < rate`, `price = b`):
      compared exactly by value.
    - **Both give DuckDB's rows wherever DuckDB answers.** DuckDB compares in rule 10's type, which is exact below
      38 digits; at the 38-digit cap it fails with a conversion error where antb1 answers (divergence D13,
      extended).
    - **With a DOUBLE operand, or a literal that DuckDB types as DOUBLE:** compared in DOUBLE after rule 8's
      conversion, not folded (`price < 2.5e1`; `price = 1e-1` holds for 0.10, as in DuckDB). An IN list with such a
      literal compares every value in DOUBLE.
    - **A decimal literal against a DOUBLE operand** is a DECIMAL too (rule 3), so it is converted as in rule 8, as
      DuckDB does, and no longer to the nearest double as ADR 0004 has it: `d = 9007199254740993.5` compares with
      9007199254740992.0, not 9007199254740994.0. PR D4 then narrows divergence D12 to HUGEINT literals.
    - **With a FLOAT column:** a decimal literal keeps ADR 0004's FLOAT rule (plan::DuckDbFloatOf); only a DECIMAL
      that is not a literal next to a FLOAT column stays unsupported (divergence D11).
    - **Other operands:** a string literal is a bind error (divergence D3); a DATE or VARCHAR operand is a bind
      error, as in DuckDB.
12. **SUM** of DECIMAL(p,s) is DECIMAL(38,s), summed exactly in Int128 (the HUGEINT sum states), so its value
    depends on neither the thread count, nor the parts, nor the order of the rows. Over no rows it is NULL.
    `SUM(price)` and `SUM(price * n)` are DECIMAL(38,2), `SUM(rate)` is DECIMAL(38,3). A sum beyond 10^38 - 1 in
    magnitude is an execution error (exit code 1), as for HUGEINT (divergence D9); DuckDB returns up to 39 digits
    until its 128-bit sum overflows, a new divergence. In both engines a partial sum beyond the 128-bit range fails,
    as for HUGEINT, so with values near 10^38 whether a sum fails can depend on the order of the rows and the parts.
13. **AVG** of DECIMAL(p,s) is DOUBLE. `AVG(price)` is computed as DuckDB computes it, with the same C types:
    - the exact sum, converted to `long double` with DuckDB's 128-bit formula (lower + upper × 2^64 from its two
      64-bit halves, with upper = -1 handled separately, as `src/plan/literal.cc` already does in double), divided
      by the count times 10^s (also in `long double`, with 10^s first rounded to a double), then rounded to double;
      for p ≤ 4 DuckDB computes the same in double;
    - `long double` differs by platform (x87 80-bit on x86-64 Linux, 64-bit on arm64 macOS) exactly as it does in
      DuckDB's own build, so the two engines agree to the bit on each platform, and no AVG divergence is registered;
    - the formula rounds once on x86-64 but can round twice on arm64 macOS, where a direct conversion of the Int128
      would be one ulp off: the AVG of the single DECIMAL(38,0) value 27670116110564329473 (2^64 + 2^63 + 2049) is
      27670116110564327424.0 on both platforms, while a direct conversion gives 27670116110564331520.0 on arm64, a
      test case for the macos-release leg;
    - on x86-64, an emulation of this formula matched DuckDB on 1,800 random sets (scales 2 to 6, up to 36 digits),
      where the correctly rounded mean missed one.
14. **MIN and MAX** keep the type (`MIN(rate)` is DECIMAL(5,3)); COUNT and COUNT(DISTINCT) are BIGINT. Over no rows
    MIN and MAX are NULL and COUNT is 0.
15. **Output:** exactly s digits after the point, a `-` for a negative value, and a leading `0` only when p > s.
    DECIMAL(15,2) prints `17.00`, `-0.25` and `0.00`; DECIMAL(3,3) prints `.500`, `-.500` and `.000`; DECIMAL(p,0)
    prints no point. `--format json` writes a DECIMAL as a string, like HUGEINT. EXPLAIN constants and the test
    harness's canonical text use the same formatter.
16. **Keys:** GROUP BY, ORDER BY, top-N and COUNT(DISTINCT) compare DECIMAL values by value within their one type,
    which is the order of the unscaled integers; NULL behaves as for other types. A join key that pairs a DECIMAL
    with another DECIMAL or an integer type compares by value, as in rule 11: both sides are cast to rule 10's type
    (`price = rate` as a key compares in DECIMAL(16,3)), and at the 38-digit cap a value that does not fit matches
    no row, where DuckDB fails with a conversion error (divergence D13). A DECIMAL against a DOUBLE compares in
    DOUBLE, so it is a key only where DOUBLE keys are.
17. **CAST,** when general CAST arrives (it is deferred):
    - DECIMAL to a smaller scale, and DECIMAL to an integer, round half away from zero: 1.005 and -1.005 become 1.01
      and -1.01 as DECIMAL(15,2), and -2.5 becomes -3 as INTEGER;
    - DOUBLE to DECIMAL(p,s) rounds x × 10^s, computed in double, half away from zero: the double 1.125 becomes 1.13
      as DECIMAL(15,2), but the double 1.005 becomes 1.00, because it lies slightly below 1.005;
    - VARCHAR to DECIMAL parses the text exactly and rounds half away from zero;
    - a value out of range is a conversion error, and DECIMAL without (p,s) is DECIMAL(18,3).
18. **No integer-only rewrites.** ADR 0012 and the binder reproduce two rewrites of DuckDB's optimizer: `SUM(x + c)`
    as `SUM(x) + c * COUNT(x)`, and constant moving in comparisons (`x + c <op> k` compares `x` with `k - c`). Both
    stay for signed integers, because DuckDB applies neither to DECIMAL: `SUM(price * qty + 7)` adds 7 on every row,
    and `price * qty + 7 > 12` computes the addition, both in DECIMAL(18,4), so an overflow of that addition fails in
    both engines (unless rule 11 folds the comparison to a constant, divergence D14).

**Storage:**

- **decimal128 now.** Every DECIMAL is a decimal128(p,s) array, Arrow's default read type, and reuses the HUGEINT
  code paths listed in Context. A file's ARROW:schema can restore decimal32, decimal64 or decimal256 instead
  (Context), so io casts such a field with p ≤ 38 to decimal128(p,s), and rule 1 holds whatever an ARROW:schema
  records. The semantics never depend on the storage: ranges are checked against 10^p - 1, not against the physical
  type.
- **decimal64 later.** decimal64 for p ≤ 18 comes together with DECIMAL in the filtered scan
  ([ADR 0020](0020-filter-pushdown.md)) and in part statistics, when performance work resumes or the scale factor 1
  nightly misses its time budget on scan-bound queries. io's cast then targets decimal64 for p ≤ 18, narrowing a
  decimal128 or decimal256 that an ARROW:schema records, and it changes no answer.

## Consequences

- **Divergences,** registered in [sql-subset.md](../sql-subset.md#divergences-from-duckdb) by the PR that implements
  each rule:
  - divergence D12, narrowed: a decimal literal compared with a DOUBLE operand is converted as DuckDB converts it
    (rule 11), so divergence D12 keeps only HUGEINT literals;
  - divergence D13, extended: antb1 compares exactly where DuckDB casts to a DECIMAL capped at 38 digits and fails
    with a conversion error, in comparisons and join keys alike, as for
    `price < 1.0000000000000000000000000000000000001` (DuckDB casts `price` to DECIMAL(38,37)) or a DECIMAL(38,2)
    against a DECIMAL(38,12) past 26 integer digits;
  - divergence D14, extended to DECIMAL operands: a comparison that rule 11 folds to a constant computes nothing.
    Over a row whose product needs more than 18 digits, `price * qty > 100000000000000000000` returns no rows, where
    DuckDB computes the product and fails (`Overflow in multiplication of DECIMAL(18)`);
  - a new divergence: a DECIMAL SUM beyond 38 digits is an error.

  The unregistered difference of reading a Parquet DECIMAL(38,0) column as HUGEINT goes away, and AVG needs no
  divergence.
- **Exact comparison comes first.** The harness compares DECIMAL text and `DECIMAL(p,s)` type names exactly (PR H5)
  before any DECIMAL engine code lands, since the DOUBLE tolerance would let a wrong cent pass.
- **Filter pushdown is lost for scans that read a DECIMAL column,** until the decimal64 item. ADR 0020 needs every
  read column to be filterable, so such a scan is not filtered at all, and a predicate on a DECIMAL column skips no
  row group, because part statistics cover integer-valued columns only. Most scans of the queries derived from
  TPC-H read DECIMAL columns, so they run unfiltered until then. Answers are unaffected.
- **Cost:** a DECIMAL of up to 18 digits takes 16 bytes and Int128 arithmetic instead of 8 bytes and int64. Accepted
  until decimal64.
- **Every slice fails cleanly.** Each code PR rejects the DECIMAL contexts it does not implement yet with
  kUnsupported (exit code 4), each with a test: the ratchet of the queries derived from TPC-H counts only exit code
  4 as a clean failure.
- **Tests:**
  - plan: tables of result types that cover every rule, both caps, the scale-38 bind error and an integer literal
    as a CASE value; literal widths; folding up and down in scale, out of range and with extra digits; a decimal
    literal against a DOUBLE column beyond 2^53;
  - exec: kernels at 10^18 - 1 and 10^38 - 1, `%` signs and zero divisors, the two-step conversion, and SUM, AVG,
    MIN and MAX in global, grouped and merged forms at 1 and 4 threads, with rule 13's AVG case;
  - io: INT32, INT64, FIXED_LEN_BYTE_ARRAY and BYTE_ARRAY columns, with and without an ARROW:schema; an ARROW:schema
    that records decimal32, decimal64 or decimal256, read as decimal128(p,s); and DECIMAL(38,0), also under an
    ARROW:schema that records decimal256(38,0);
  - `.slt` records whose expectations come from DuckDB, over a fixture of our own with NULLs, negative and
    near-maximum values and several row groups;
  - the random generator: DECIMAL columns, literals and arithmetic that cannot overflow, with exact type names.
- **Docs:** ADR 0004 and ADR 0012 point here, and ADR 0020 notes the lost pushdown. docs/sql-subset.md changes with
  each code PR, never ahead of the code.

## Plan

One PR each. PR H5 and PR D1 land in either order (PR D1 changes no behavior), both before PR D2; then PR D2, PR D3
and PR D4, in this order. Each engine PR documents its rules in docs/sql-subset.md and rejects the rest with exit
code 4. The PR ids are the roadmap's, not divergence ids.

- **PR H5,** test(harness): compare decimal results exactly. Rule 15's text and exact type names, before engine code.
- **PR D1,** refactor(plan): logical types with width and scale. The type carries (p,s); no behavior change.
- **PR D2,** feat(plan,io,exec,engine): decimal columns. Rules 1, 2, 14-16 (join keys come with the joins) and io's
  cast to decimal128; rule 11 for integer and decimal literals and same-type operands.
- **PR D3,** feat(plan,exec): decimal arithmetic, sum and avg. Rules 4-7, 12, 13 and 18.
- **PR D4,** feat(plan,exec): decimal literals, division and mixed comparisons. Rules 3 and 8-11.
- **decimal64 (deferred):** decimal64 for p ≤ 18, with DECIMAL in the filtered scan and in part statistics.

## Alternatives considered

- **decimal64 from the start:** 8 bytes per value for p ≤ 18. It needs io's cast to narrow a decimal128 or
  decimal256 that an ARROW:schema records, and two kernel paths at once, for speed only. It comes later, with the
  filtered scan.
- **Emulating DECIMAL with DOUBLE:** wrong result types (DOUBLE instead of DECIMAL(38,s)), wrong digits (a sum prints
  binary rounding noise instead of its cents), and exact equalities between sums break. Rejected.
- **Arrow's decimal kernels for arithmetic:** they follow other width rules: they widen instead of capping (a product
  gets p1 + p2 + 1 digits, and past 38 digits a kernel fails before it runs), so DuckDB's capped types and their
  10^p checks would need code of our own anyway. Arrow 25 also has no decimal32 or decimal64 arithmetic. Rejected;
  Arrow still decodes, compares, filters and takes DECIMAL arrays.
- **Comparing in DuckDB's capped common type:** it would reproduce DuckDB's conversion errors too, but every
  comparison would need a run-time cast that can fail, and literal folding would differ from the exact folding of
  integer columns (divergence D13). Rejected.
- **A correctly rounded AVG:** one exact division, but it differs from DuckDB in the last bit for a few sets, which
  can change the rows that Q17 and Q22 keep. A registered divergence would leave those answers to chance. Rejected.
- **SUM up to 39 digits, as in DuckDB:** such a value is outside DECIMAL(38,s), the result's own type, and outside
  the precision of its decimal128(38,s) array. An error, as for HUGEINT (divergence D9), is simpler, and DuckDB
  reaches 39 digits only by leaving its own type. Rejected.
- **DECIMAL(38,0) columns as HUGEINT, as today:** no breaking change, but DuckDB reads such a column as
  DECIMAL(38,0), so type names, result types and arithmetic would differ. Rejected.

This workload is derived from the TPC-H Benchmark and is not comparable to published TPC-H Benchmark results, as
this implementation does not comply with all requirements of the TPC-H Benchmark.
