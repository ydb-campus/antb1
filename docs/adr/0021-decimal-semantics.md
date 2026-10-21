# 21. DECIMAL semantics as in DuckDB

Date: 2026-10-02

## Status

Accepted (with the maintainer-approved plan for DECIMAL)

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
- **DuckDB's rules are its own.** The oracle is the locked libduckdb, DuckDB 1.5.6. Its DECIMAL typing is neither the
  SQL standard's nor Arrow's: widths are capped at 18 digits while every input has at most 18 digits, division gives
  DOUBLE, and CASE and comparisons take different common types. Every rule below was probed with DuckDB 1.5.5 on our
  own data (1.5.6 gives the same types, values and messages), and its formulas were read in DuckDB's source.
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

antb1 follows DuckDB 1.5.5 and 1.5.6 (they agree) for DECIMAL. In the rules, p is the width (precision) and s the
scale. The examples use a table of our own: `price` and `qty` DECIMAL(15,2), `rate` DECIMAL(5,3), `n` INTEGER,
`b` BIGINT and `d` DOUBLE.

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
     with its own formula, which antb1 reproduces in `src/common/int128.cc`.
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
    - CASE values fold pairwise, as in DuckDB: from the ELSE value's type (NULL without an ELSE) through the THEN
      values in written order, so the type can depend on the order of the values. Two DECIMALs take the same type
      while it fits in 38 digits; beyond 38 they keep their integer digits (p = 38 and s = 38 - max(p1 - s1,
      p2 - s2), values rounded half away from zero). A DECIMAL with an integer keeps its scale and widens only for
      an integer of more than p - s digits (p = max(p, min(38, the integer's digits + s)); a value that does not fit
      fails with DuckDB's conversion error on the rows that take it). An integer literal (also under unary minus)
      takes an integer type it fits, else the two types' common one (SMALLINT with USMALLINT is INTEGER), and counts
      as its own type next to a DECIMAL. Next to NULL (the first THEN value without an ELSE) or another literal, an
      integer literal becomes its own type and a string literal VARCHAR; otherwise (an ELSE literal too) a string
      literal takes the type folded next. Each value is cast once, to the final type, on the rows that take it;
    - `CASE WHEN n > 0 THEN price ELSE 7 END` is DECIMAL(15,2); with `rate` instead of `price` it is DECIMAL(13,3),
      because 7 counts as INTEGER; `ELSE b` gives DECIMAL(21,2), `ELSE 2.5` DECIMAL(15,2) and `ELSE d` DOUBLE. With
      a SMALLINT `s16`, `CASE WHEN n > 0 THEN 7 WHEN n > 1 THEN rate ELSE s16 END` is DECIMAL(8,3), as 7 becomes
      SMALLINT before `rate` comes, while `THEN rate WHEN n > 1 THEN 7` gives DECIMAL(13,3).
11. **Comparisons:**
    - **With an integer or decimal literal:** folded exactly into the operand's (p,s) at bind time, as integer
      columns are folded today (ADR 0004). `price < 12.345` becomes `price <= 12.34`, `price >= 12.345` becomes
      `price >= 12.35`, and `price = 12.345` is never true. A literal beyond the type's range makes the comparison
      constant, NULL still rejected, and a never-true one that is a whole `WHERE` or `HAVING` conjunct does not compute
      the operand (divergence D14). Folding is never an error, also for an integer literal of more than 38 - s digits,
      which DuckDB fails to cast to its capped DECIMAL(38,s) (divergence D13). An IN list of such literals is folded
      value by value.
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
    - **BETWEEN** takes DuckDB's one common type of its three values. Where that is DOUBLE and antb1 would compare
      a pair exactly (a DECIMAL operand with a bound that is not DOUBLE, or a DECIMAL bound), the BETWEEN is
      unsupported (exit code 4); otherwise its two comparisons give DuckDB's rows.
    - **Other operands:** a string literal is a bind error (divergence D3); a DATE, TIMESTAMP or VARCHAR operand is a
      bind error, as for any number. DuckDB rejects it for `<` and the like, but casts a VARCHAR operand for `=`, `<>`
      and IN, and fails on a DATE or TIMESTAMP only when a row is compared (divergence D4).
12. **SUM** of DECIMAL(p,s) is DECIMAL(38,s), summed exactly in Int128 (the HUGEINT sum states), so its value
    depends on neither the thread count, nor the parts, nor the order of the rows. Over no rows it is NULL.
    `SUM(price)` and `SUM(price * n)` are DECIMAL(38,2), `SUM(rate)` is DECIMAL(38,3). A sum beyond 10^38 - 1 in
    magnitude is an execution error (exit code 1), as for HUGEINT (divergence D9); DuckDB returns up to 39 digits
    until its 128-bit sum overflows, a new divergence. In both engines a partial sum beyond the 128-bit range fails,
    as for HUGEINT, so with values near 10^38 whether a sum fails can depend on the order of the rows and the parts.
13. **AVG** of DECIMAL(p,s) is DOUBLE. `AVG(price)` is computed as DuckDB computes it, with the same C types:
    - the exact sum, converted to `long double` with DuckDB's 128-bit formula (lower + upper × 2^64 from its two
      64-bit halves, with upper = -1 handled separately, as `src/common/int128.cc` does), divided
      by the count times 10^s (also in `long double`, with 10^s first rounded to a double), then rounded to double;
      for p ≤ 4 DuckDB computes the same in double;
    - `long double` differs by platform exactly as it does in DuckDB's own build (x87 80-bit on x86-64 Linux, IEEE
      quad on arm64 Linux, 64-bit double on arm64 macOS), so the two engines agree to the bit on each platform, and no
      AVG divergence is registered;
    - the result can miss the correctly rounded mean, by platform: the AVG of the single DECIMAL(38,0) value
      27670116110564329473 (2^64 + 2^63 + 2049) is 27670116110564327424.0 on x86-64 Linux (the halves' sum rounds
      to 64 bits, then to double) and on arm64 macOS (the lower half rounds, then the sum), but
      27670116110564331520.0, the correctly rounded value, on arm64 Linux, where the sum is exact; the unit tests pin
      each platform's answers (the macos-release leg, the nightly arm64 job);
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
    (`price = rate` as a key compares in DECIMAL(16,3)); where that type would need more than 38 digits there is
    no key at all and the equality stays a residual, compared as rule 11 compares it (divergence D13). A DECIMAL
    against a DOUBLE compares in DOUBLE, so it is a key only where DOUBLE keys are.
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
  - Update (2026-10-20): **the trigger fired** -- performance work resumed and three changes to the decimal
    aggregate and expression paths landed -- and the measurement under the Plan's decimal64 item puts the
    arithmetic, not io's cast, at the centre of it. The item is staged there as N1 to N4.

## Consequences

- **Divergences,** registered in [sql-subset.md](../sql-subset.md#divergences-from-duckdb) by the PR that implements
  each rule:
  - divergence D12, narrowed: a decimal literal compared with a DOUBLE operand is converted as DuckDB converts it
    (rule 11), so divergence D12 keeps only HUGEINT literals;
  - divergence D13, extended: antb1 compares exactly where DuckDB casts to a DECIMAL capped at 38 digits and fails
    with a conversion error, in comparisons and join keys alike, as for
    `price < 1.0000000000000000000000000000000000001` (DuckDB casts `price` to DECIMAL(38,37)) or a DECIMAL(38,2)
    against a DECIMAL(38,12) past 26 integer digits;
  - divergence D14, extended to DECIMAL operands: a `WHERE` or `HAVING` conjunct that rule 11 folds to never true
    computes nothing. Over a row whose product needs more than 18 digits, `price * qty > 100000000000000000000`
    returns no rows, where DuckDB computes the product and fails (`Overflow in multiplication of DECIMAL(18)`);
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

One PR each. PR H5 and PR D1 land in either order (PR D1 changes no behavior), both before PR D2; then PR D2, PR D3 and
PR D4 (D4a, D4b, then D4c), in this order. Each engine PR documents its rules in docs/sql-subset.md and rejects the rest
with exit code 4. The PR ids are the roadmap's, not divergence ids.

- **PR H5,** test(harness): compare decimal results exactly. Rule 15's text and exact type names, before engine code.
- **PR D1,** refactor(plan): logical types with width and scale. The type carries (p,s); no behavior change.
- **PR D2,** feat(plan,io,exec,engine): decimal columns. Rules 1, 2, 14-16 (join keys come with the joins) and io's
  cast to decimal128; rule 11 for integer and decimal literals and same-type operands.
- **PR D3,** feat(plan,exec): decimal arithmetic, sum and avg. Rules 4-7, 12, 13 and 18.
- **PR D4,** split in three at review size:
  - **PR D4a,** feat(plan,exec): decimal literals and division. Rules 3, 8 and 9 (`%` up to 38 digits), and rule
    11's decimal literal against a DOUBLE operand.
  - **PR D4b,** feat(plan,exec): decimal mixed comparisons. Rule 10 for comparisons and IN lists, and the rest of
    rule 11.
  - **PR D4c,** feat(plan,exec): decimal case values and modulo beyond 38 digits. Rule 10 for CASE values, and `%`
    beyond 38 digits.
- **decimal64:** decimal64 for p <= 18, with DECIMAL in the filtered scan and in part statistics.
  - **The trigger fired (2026-10-20), and the measurement says this item was costed the wrong way round.**
    The entry above and the consequence note both frame decimal64 as a *storage* narrowing in `io`. Storage is the
    smaller half. The arithmetic is the larger one, and the three figures that decide it, taken on this host
    against the environment's own Arrow on synthetic data rather than on any gate set:
    - **int64 multiply with an overflow check costs 0.3 ns a row; the same in Int128 costs 1.2 ns** -- 4x, and it
      is hardware rather than code. A 64-bit multiply is one instruction and vectorises; a 128-bit one compiles to
      `mul` plus `umulh`, and neither NEON nor AVX2 has a 64x64->128 widening multiply to vectorise it into.
    - **8 bytes a value instead of 16**, which halves the traffic of every decimal column read and every result
      written. That matters more than it sounds: one step read and written the way this engine does it costs about
      3.0 ns a row, of which the `Decimal128Builder` write is about 1.6 and the arithmetic with its range check
      about 1.25. **The write is dearer than the multiply.**
    - **Rule 6's cap is what makes this apply at all.** Because a product of two operands of at most 18 digits is
      capped at 18, the whole decimal pipeline of the gate set's heaviest decimal query is DECIMAL(18,s) or
      narrower: its discounted price is DECIMAL(18,4) and that price with tax is DECIMAL(18,6), both inside int64.
      So this is not a narrow case that happens to fit -- the capping rule this ADR adopted from DuckDB is what
      puts the arithmetic within reach of int64 in the first place.
  - **The stated blocker does not apply to the arithmetic.** "Arrow 25 has no decimal32 or decimal64 arithmetic
    kernels" is true and beside the point, because this engine does not use Arrow's decimal kernels for arithmetic:
    rule 6 caps where Arrow widens, which Alternatives records as the reason. The arithmetic is this engine's own
    program, so an int64 path in it is this engine's to write and waits on nothing. Arrow's decimal128 kernels were
    measured for completeness -- `multiply` over two decimal128(15,2) gives decimal128(31,4), Arrow's widening
    rule, at 1.3 ns a row against our own 1.2 -- so there was never any speed to borrow from them either.
  - **And a negative result, so that it is not rediscovered.** The other candidate for the same cost was the
    program's loop order: it runs a row at a time, and [ADR 0026](0026-join-performance.md)'s C10 chose that
    deliberately. Inverting it -- step outer, rows inner, over a tile small enough to stay in L1 -- was built and
    measured at **0.899x** of the operator's own time, with **the tile size making no difference at 128, 512 or
    2048**. That last part is what settles it: were the gain cache locality the tile size would dominate, so what
    an inversion buys is only the hoisted dispatch, and C10's own sentence about the dispatch predicting perfectly
    was closer to right than the analysis that produced the branch. Kept, unproposed, on `perf/tiled-decimal-program`.
  - **Where the cost actually sits**, so the next item is aimed rather than guessed: that operator costs 16.3 ns a
    row on one thread and 23.8 summed over parts on fourteen, while the four steps it runs, read and written this
    engine's way, cost 6.2 ns a row in isolation. So the expression evaluation proper is under 40% of it, memory
    contention is most of the difference between one thread and fourteen, and a part of the remainder is
    unexplained -- not the per-step dispatch (the branch above hoisted it), not per-batch allocation (one builder
    pair per 60,500-row batch measures the same as one pair for everything), and not the validity path (it returns
    no bitmap when no column is nullable). A sampling profiler, not another hypothesis, is what closes that.
  - **Staged, smallest first:**
    - **N1, perf(exec): compute a step in int64 where its types permit.** Inside the existing program only: where
      a step's result and both its operands have p <= 18, narrow to int64, test the overflow on int64, and fall
      back to the Int128 path for a value that does not fit -- which a column can hold, since a file may carry a
      value beyond its declared width and the scan does not narrow it. Storage, types and answers all unchanged;
      no module edge and no public header. **Its real purpose is to test the premise of N3 cheaply**: if the 4x
      above shows up in situ, the representation change is grounded; if it does not, the micro-benchmark does not
      transfer and N3 should be re-argued before anyone builds it.
    - **N2, dropped on its own premise (2026-10-20).** It claimed to halve the traffic between steps. There is no
      such traffic: the program's `slots` are one `Int128` per *step*, reused for every row
      (`src/exec/compute.cc:585`), so they live in registers and on the stack rather than in a per-row array. The
      entry was written while thinking of a tiled evaluation, where slots genuinely are per-tile arrays, and it
      did not survive reading the loop it describes. What is actually left is a redundant int64 test on an operand
      that an earlier narrow step produced and a round trip through a 128-bit slot -- about two instructions a
      step, below what this host can measure. Folded into N3, which changes the representation anyway.
    - **N3, feat(plan,io,exec): a decimal64 representation for p <= 18.** The item as originally written, plus the
      arithmetic: `io` reads into decimal64 rather than widening Parquet's INT32 or INT64 to decimal128, and
      aggregates, sorting, top-N, grouping, MIN and MAX, comparisons and join keys all take it. Several modules,
      so it gets its plan here before any code.
      - **Arrow 25 supports what this needs, probed on 2026-10-20 against the environment's own Arrow rather than
        assumed.** `decimal64` is a type with `byte_width` 8, `Decimal64Builder` builds it, and `equal`, `less`,
        `Take`, `Grouper::Make`, `Grouper::Consume` and casts to DOUBLE, to decimal128 and to another decimal64
        all work on it. Two kernels have none -- `array_sort_indices` and `sum` -- and neither matters: this
        engine sorts with its own key layout and never calls Arrow's `sum`, which
        `src/exec/include/antb1/exec/aggregate_state.h:31` records as wrapping at 64 bits. So the Context's note
        about missing decimal64 *arithmetic* kernels remains the only gap, and this engine's own arithmetic is
        what fills it.
      - **The surface, surveyed:** about 270 sites, most of them mechanical. The representation is fixed in
        `plan` in exactly two places, `plan::ToArrow` (`src/plan/types.cc:56`) and the scalar helper behind
        `plan::ToArrowScalar` (`src/plan/logical_plan.cc:48`); `FromArrow` (`src/plan/types.cc:95`) already
        accepts decimal64. Everything else in `plan` works in logical `(p,s)` and `Int128` and needs no change.
      - **The hazard that decides how this is sequenced.** Seven non-test sites hand `plan::ToArrow`'s result
        straight to an `arrow::Decimal128Builder` -- `src/exec/aggregate_state.cc:72`,
        `src/exec/compute.cc:159`, `src/exec/grouped_aggregate_state.cc:107`, `src/exec/decimal.cc:132` among
        them -- and one to `GroupedMinMax<arrow::Decimal128Type>` (`src/exec/grouped_aggregate_state.cc:748`).
        A builder constructed with a decimal64 type there goes through `checked_pointer_cast`, which is a
        `static_pointer_cast` under NDEBUG: **in a release build it would not fail, it would write 16 bytes into
        an 8-byte slot.** So those sites are found and made to fail loudly *before* `ToArrow` changes, not after.
      - **Three decisions it has to take, none of them mechanical:**
        - **A value a file carries beyond the column's declared width.** N1 above depends on such a value
          being representable, and so does `src/exec/decimal.h:53`.
          - **Settled (2026-10-20), and the premise above was wrong.** Every Parquet storage of a decimal is
            sized to the column's precision, so at no more than 18 digits none of them can carry a value an
            int64 does not hold: INT32 is four bytes, INT64 is eight, and a FIXED_LEN_BYTE_ARRAY's own length
            is derived from the precision -- measured against this environment's writer as 2 bytes at p=4, 4 at
            p=9, 7 at p=15 and 8 at p=18, reaching 9 only at p=19, which does not narrow. Narrowing is
            therefore lossless by the storage's own width rather than by trusting the declared precision, and
            even a value beyond that precision survives, because it had to fit the storage to exist. So the
            choice between narrowing only INT32 and INT64 columns and narrowing every column of at most 18
            digits has no behavioural difference for a conforming file. The second is taken, and the first
            could not have been expressed anyway: `plan::Table::schema()` is `ToArrow(logical)` by
            construction (`src/plan/include/antb1/plan/table.h:69`), and a per-storage decision is not a
            property of a logical type. What is left is a BYTE_ARRAY decimal carrying more bytes than its
            precision implies -- a malformed file -- where io names the column instead of truncating.
        - **p <= 9.** Arrow's own two mechanisms -- `arrow::smallest_decimal` and the Parquet reader's smallest-
          type option -- give decimal32 there, so the reader option is not a shortcut to a single narrow
          representation and `plan::ToArrow` must spell its own rule either way.
        - **One physical type per `(p,s)`, or two with conversions at the boundaries.** Every type-equality check
          in exec assumes one -- `src/exec/join_table.cc:298`, `src/exec/sort.cc:204`,
          `src/exec/aggregate_state.cc:33`, `src/exec/filter.cc:96` -- and `plan::Table::schema()`
          (`src/plan/include/antb1/plan/table.h:69`) is where that invariant is written down. Strict makes the
          change all-or-nothing across io, exec and every test table; tolerating both needs a conversion at each
          operator boundary and keeps the steps small.
      - **Split, because this is too wide for one PR:** the reader-and-storage question in `io` (where
        `parquet::ArrowReaderProperties::set_smallest_decimal_enabled` has to be set at all three type-inference
        and read sites or the recorded schema stops matching the decoded arrays); exec's read and write sites;
        and DECIMAL in part statistics, which is independent of the representation -- statistics are `Int128`
        either way -- and need not travel with it.
      - **And one thing to check rather than assume**: a group or join key hashed over 8 bytes instead of 16
        (`src/exec/group_table.cc:92`) may change which partition a group lands in. No golden carries group order
        that the survey could find, but the grouped-aggregate and join suites are what settle it.
      - **Every engine path reaches decimal64 as of 2026-10-20, and the switch-on run is what said so.** With
        `SetNarrowDecimals` defaulted on, the whole of slt, oracle, metamorphic, diff and the TPC-H-derived
        ratchet pass. Five places had to be fixed to get there, and the shape of the list is the finding: a type
        test spelled `== DECIMAL128` where the question was "is this a decimal" is the entire failure mode, at
        four of the five.
        - `src/exec/compute.cc`'s DECIMAL-to-DOUBLE conversion. **The one that answered wrongly rather than
          refusing**: a narrow column fell to Arrow's cast, which multiplies by 10^-s where ADR 0021 rule 8
          divides by 10^s, so the two land on different doubles (5.0005 is one). The conversion already had a
          test and could not catch it -- the test calls `DecimalToDouble`, and what broke was the route to it.
        - `src/exec/filter.cc`'s three: a DOUBLE constant on a decimal column, a DOUBLE IN list, and the
          decimal side of a comparison with a DOUBLE column. The first two refused the query as a type error;
          the third took Arrow's cast, which is the same wrong answer as above.
        - `plan::ToArrowScalar`, which built a `Decimal128Scalar` carrying a decimal64 type. Arrow's kernels
          reach a scalar's value through a `checked_cast` on its type, a `static_cast` in the Release build the
          prebuilt Arrow is, so this did not fail -- it read a `Decimal128`'s first eight bytes as a
          `Decimal64`, which is the right answer for every value at this width on a little-endian target. An
          accident of layout, now a class chosen from the type.
        - `src/engine/format.cc`, where a decimal64 column reached the default case and Arrow's own `ToString`
          wrote `0.125` for the `.125` rule 15 asks for.
        - `Scatter` in `src/exec/compute.cc`, the CASE merge: Arrow has `replace_with_mask` for decimal128 and
          **not** for decimal64, the same shape of gap as `min_max`'s. A scatter moves whole values, so the
          narrow arm goes through the kernel as the `fixed_size_binary(8)` it already is -- `Array::View` is
          zero-copy both ways, and the two routes were measured to agree on a NULL in either input, a NULL and
          an all-NULL mask, and sliced inputs. Two TPC-H-derived queries and three slt files turned on this one
          alone.
      - **The fixtures followed, and one of them was not a fixture.** 29 unit cases failed with the switch
        defaulted on; 28 were artifacts of the default, and the 29th was a soundness bug in `exec`.
        - **`ReadDecimalOperand` marked every non-in-place operand `checked`** (`src/exec/compute.cc`), under
          a comment whose own words were "only a decimal128 column can hold what its declared width does not
          admit". True while decimal64 did not exist; false the moment it did. The arm exists for an INTEGER
          operand, whose values are inside its physical type by the type rules -- a decimal64 column of
          DECIMAL(2,1) holds 1500 as easily as a decimal128 one. So `DecimalArith` and the program's step
          check both skipped the operand range test for a narrow column, and `SELECT (-d) - (-d)` over such a
          column answered 0 where DuckDB raises a conversion error. **This is the hole #191 closed on the
          reuse path, reopened one physical width over**, and it was `#191`'s own regression case that caught
          it. The fix is one line: only an integer skips the test. The case now runs at both widths, so the
          check is enforced with the switch off, which is the configuration CI runs.
        - The other 28: cases asserting the off state, fixtures hand-building a decimal128 array where the
          plan says decimal64, text helpers reading 16 bytes from an 8-byte stride, schemas naming a width
          their own column did not have, and two fixtures whose premise needed restating.
        - **One mechanism caused most of it**: `arrow::Decimal128Builder builder(plan::ToArrow(type))`, a
          16-byte writer handed whichever type the switch chose, in nine places. `Decimal128Builder` checks
          that type with `checked_pointer_cast`, a `static_pointer_cast` under NDEBUG, so what it does is
          neither fail nor reliably crash: `RandomColumn` in `grouped_aggregate_test.cc` made the exec binary
          pass alone and die in a suite run. They now all go through one width-aware fixture helper that
          fails loudly on a value the narrow width cannot hold, since a silent truncation is the one way a
          fixture could go quietly weaker.
        - **Two premises had to be restated rather than repaired.** A fixture carrying 10^19 in a
          DECIMAL(18,0) column was expressible only while a p <= 18 column was 128 bits wide; 10^18 is one
          past the same declared width and inside an int64, so it is a real input at both widths and probes
          the narrow range test as well. And "a column past its declared width AND past int64" has no narrow
          form at all -- 8 bytes cannot hold it -- so that row's operand type moved to 38 digits while its
          step stayed at 18, which keeps the operand test covered after the switch is gone instead of
          silently ceasing to run.
      - **DECIMAL in part statistics is not part of this after all.** `IntegerOf`
        (`src/io/parquet_table.cc:547`) admits only integer scalars, so decimal statistics were never used at
        either width. Adding them is a separate feature and does not gate the default.
    - **The flip was run on 2026-10-20, and it is deferred.** The 8-bytes-a-value claim finally got a number,
      and the number says the item pays in memory and not yet in time.
      - **Flipping the default with nothing else changed is a regression**: at SF 10, one thread, 22 queries,
        three repeats with the arms interleaved, geomean 1.027x, carried by Q06 at 1.691x and Q01 at 1.138x.
        Both were attributed rather than inferred -- Q06's whole cost sat in its scan (66 ms to 112 ms at SF
        1, same rows and predicates), Q01's entirely in Compute's own time.
      - **Arrow 25 has no specialised comparison kernel for decimal64.** Measured on this environment's
        Arrow: `greater_equal` of a column against a scalar costs 1.23 ns a row at decimal128, **3.44 at
        decimal64** (2.8x), and 0.13 at int64. The same shape of gap as the missing `min_max` and
        `replace_with_mask` kernels, and the cause of Q06: a pushed predicate goes through
        `PredicateEvaluator`, which calls that kernel.
      - **So the comparison stopped going through it.** A decimal's bytes ARE its unscaled integer, and
        `PredicateEvaluator::Make` already refuses a constant whose type differs from the column's, so where
        it does not refuse both sides carry one scale and the comparison IS an integer comparison.
        `Array::View` hands the same buffers to int64 at no copy, and the two routes were measured to agree
        including on NULLs. That is `Mode::kNarrowAsInteger`, and it makes Q06 **0.817x** -- faster than
        decimal128, not merely level with it, because the int64 kernel beats the decimal128 one 9x.
      - **And Q01 was a shape of our own making.** `ReadDecimalOperand` read a decimal128 column in place and
        sent a decimal64 one to the shape that allocates: a `std::vector<Int128>` filled by widening every
        value. Compute's own time, 926 ms against 1442 ms at SF 10. Halving the bytes a value moves cannot
        pay for widening every one of them back. A fourth operand shape reads the narrow column in place at
        its own stride, and Q01 becomes **0.958x**. One trap came with it: `RescaleOperand` cleared `bytes`
        and not the new pointer, so a rescaled operand read the value the rescale had replaced -- the modulo
        in `decimal_literals.slt` caught it, and `DecimalOperand::Detach` now clears every in-place shape.
      - **Where that leaves it.** SF 10 one thread geomean **0.984x**, four threads **0.991x**; SF 100 four
        threads over 22 queries **0.995x**. Peak memory falls everywhere it moves at all: 0.868x on SF 10's
        heaviest query (3765 MiB to 3267), and at SF 100 Q02 0.81x, Q09 down 636 MiB, Q05 down 338 MiB.
      - **The join-heavy regression was the unfiltered scan's narrowing pass, found with a sampling
        profiler after two hypotheses had been refuted by measurement.** It cost 2 to 8% at SF 100 on five
        queries (Q05 1.078x at five repeats with non-overlapping spreads). The gather was not it -- a
        micro-benchmark of exactly what `gather.cc` does is 0.90x at the narrow width -- and neither were
        the memory budget's heuristics reacting to the lower usage, since with an explicit identical limit on
        both arms Q05 still read 5.73 s against 6.12 s. `sample` over both arms settled it in one run:
        `io::Convert` took 4.9% of samples in the narrow arm and does not appear in the wide one at all.
        - **The asymmetry is which scan path a query takes.** A scan that pushes a predicate decodes
          Parquet's INT64 straight into int64 slots and narrows for free; a scan that pushes none goes
          through Arrow's Parquet reader, which produces decimal128, and every value is then narrowed in a
          pass of its own. Q06 and Q01 push predicates on lineitem and so never paid it; Q05 pushes none
          and paid it over 600 million rows of two columns.
        - **Making that pass cheap recovered most of it**: Q05 1.078x to 1.007x, Q09 1.032x to 1.008x, Q21
          1.022x to 1.000x. The loop zeroed its whole output buffer before writing it, and tested the high
          bytes of each value one byte at a time; it now writes each row once and compares whole 64-bit
          words, with the validity test hoisted out of the loop for a column that has no NULLs.
      - **Accepted, and the default is on.** SF 100, four threads, 22 queries, three repeats interleaved:
        geomean **0.985x**, total 0.992x. SF 10, one thread: geomean **0.984x**, total 0.989x. Peak memory
        falls wherever it moves: **0.765x** on Q01 at SF 100 (362 MiB to 277), 0.79x on Q02, Q09 down 577
        MiB, and 0.866x on SF 10's heaviest query. The largest single gains are Q06 at 0.804x and Q01 at
        0.948x, both from paths this item had to own because Arrow's decimal64 support is younger than its
        decimal128 support.
      - **What is still slower**, and small enough to accept with the memory saving: Q05 1.018x, Q07 1.018x,
        Q21 1.013x, Q18 1.009x, Q09 1.008x at SF 100. Q05's remainder is the same conversion pass, now
        cheaper rather than gone -- removing it altogether means decoding Parquet without Arrow's
        Parquet-to-Arrow transfer on the unfiltered path too, which is a larger change than this item.
      - **Done.** The switch is deleted: `plan::ToArrow` narrows unconditionally at 18 digits or fewer, and
        `SetNarrowDecimals`, `NarrowDecimals` and the four copies of the `NarrowDecimalsFor` guard are gone.
        What the deletion asked of the cases that compared the two widths is the part worth recording, because
        "collapse the arms" and "drop the assertion" look alike in a diff:
        - **Where the two widths were selected by a SWITCH, they are now selected by the PRECISION**, which is
          both expressible and stronger: 15 or 18 digits is decimal64 and 19 is decimal128, so the comparisons
          in `ADecimalToDoubleReadsEitherWidth`, `ADecimalConvertsToDoubleByDuckDbsRule...`,
          `MinMaxOfANarrowDecimalAnswersAtItsOwnWidth` and `ANarrowStepAgreesWithTheInt128Path...` survive
          with both arms real. Each asserts the physical type of each arm, so neither can quietly become the
          other.
        - **Where one arm WAS the expectation, the expectation is now written out** and derived from the
          arithmetic rather than read off the engine: the program's `94502250 20000000 0 null null
          99999999999900`, the fallbacks' two rows, the sort orders, the five group sums, the MIN and MAX
          texts. Every one of them was right first time, which is the only evidence that they state the rule
          rather than the behaviour.
        - **Where the arm was the subject itself, the case found a different differential**: the grouped
          aggregates now compare one pass against a merge of three parts, which is a question the one width
          still has.
        - Three mutations confirm the collapsed cases still bite: a narrow append truncated to 32 bits, a
          MIN/MAX answer left at the kernel's width, and a narrow sort prefix carrying no information.
    - **N4, docs(adr): record what N1 to N3 measured**, and accept or defer the rest on those numbers. Done,
      above: N1 and N3 are accepted on the SF 100 and SF 10 runs, N2 was dropped on its own premise, and the
      item is closed. What it bought, against decimal128 and measured with the arms interleaved, is a geomean
      of 0.985x at SF 100 and 0.984x at SF 10 with peak memory as low as 0.765x on one query; the single
      largest gain is Q06 at 0.804x, which is also the one TPC-H-derived query this engine now answers faster
      than DuckDB. What it cost is three paths this engine had to own because Arrow's decimal64 support is
      younger than its decimal128 support: the comparison, the operand read, and the scatter.

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
