# SQL subset

antb1 accepts a single `SELECT` statement over Parquet tables. The goal of the first slice is a small, exactly
specified subset that behaves like DuckDB, which serves as the test oracle. Everything outside the implemented subset
is rejected cleanly with an error and a source position (normally exit code 4), never answered wrongly.

This page is the contract: a PR that changes SQL behavior updates it in the same PR.

## What works today

Every query of the [grammar](#grammar) below runs: global and grouped (`GROUP BY`) aggregates, `COUNT(DISTINCT ...)`
included, projections (`*`, columns or constants), `WHERE` conditions (`column <op> literal` comparisons,
`column [NOT] LIKE 'pattern'`, `column [NOT] IN (literal, ...)` and `column [NOT] BETWEEN low AND high`, combined
with `AND`, `OR` and `NOT`, with constant integer arithmetic folded on a side), `GROUP BY`
and `ORDER BY` (also by position), `HAVING` (the same conditions on aggregates and keys), arithmetic
(`+ - * / // %` and unary `-`), the string functions `strlen` and `regexp_replace`, the timestamp functions
`toDateTime`, `EXTRACT` and `date_trunc`, and `CASE` in every clause, dates written as casts
(`CAST('2013-07-01' AS DATE)`, `'2013-07-01'::DATE`), `LIMIT` and `OFFSET`, over one table of Parquet files. This
covers all 43 ClickBench queries (see [ClickBench status](#clickbench-status)). None of the 22 queries derived from
TPC-H passes yet (see [Queries derived from TPC-H](#queries-derived-from-tpc-h)).

```sql
SELECT COUNT(*), SUM(ResolutionWidth) AS width, AVG(UserID), MAX(EventDate) FROM hits WHERE IsMobile = 1
SELECT WatchID, URL FROM hits WHERE RegionID < 300 AND SearchPhrase <> '' LIMIT 10
SELECT COUNT(*) FROM events WHERE url LIKE '%shop%' AND title NOT LIKE 'Promo_%'
SELECT RegionID, COUNT(*) AS n, AVG(ResolutionWidth) FROM hits WHERE IsMobile = 1 GROUP BY RegionID
SELECT OS, COUNT(*) AS n FROM hits GROUP BY OS ORDER BY n DESC, MAX(EventDate) NULLS FIRST LIMIT 10 OFFSET 5
SELECT CounterID, COUNT(*) AS n FROM hits GROUP BY CounterID HAVING n > 100 AND MIN(URL) LIKE 'http%'
SELECT * FROM '/data/hits_*.parquet' LIMIT 5
```

- The table is registered with `--table NAME=PATH[,PATH|GLOB]` (an identifier or a `"quoted identifier"`, matched
  ASCII case-insensitively) or given as a string literal with a path or glob, e.g. `FROM '/data/hits_*.parquet'`.
- Only the columns a query references are decoded. `COUNT(*)` without `WHERE` is answered from the Parquet footers
  (no data page is read); under `WHERE` it counts the rows the filter selects without copying them.
- Result names and types follow DuckDB ([Binding](#binding)); values follow the [Semantics](#semantics) below.
- `--` line comments, `/* block */` comments and one trailing `;` are allowed.
- SQL outside the grammar (`JOIN`, a window function such as `row_number() OVER ()`, `IS NULL`, other functions, ...)
  fails with exit code 4 and points at the first unsupported token. Malformed SQL (a syntax error) and SQL that is
  wrong for the table (a bind error) fail with exit code 1.

```bash
pixi run antb1 query -f query.sql --table hits=/data/clickbench/hits_0.parquet --clickbench
pixi run antb1 explain -f query.sql --table hits=/data/clickbench/hits_0.parquet --clickbench
pixi run antb1 explain --analyze -f query.sql --table hits=/data/clickbench/hits_0.parquet --clickbench
```

Globs are allowed in the file-name part of a path only (`/data/hits_*.parquet`, not `/data/*/hits.parquet`) and
expand to a sorted file list. All files of a table must have the same schema (divergence D1 below).

## Grammar

The parser accepts this grammar; the binder answers the part of it described below and checks it against the tables,
and the executor runs it. Keywords are case-insensitive.

```ebnf
statement   = query , [ ";" ] ;
query       = "SELECT" , select_list , "FROM" , table_ref , [ "WHERE" , expr ] ,
              [ "GROUP" , "BY" , expr , { "," , expr } ] , [ "HAVING" , expr ] ,
              [ "ORDER" , "BY" , order_item , { "," , order_item } ] ,
              [ limit_offset ] ;
limit_offset = "LIMIT" , integer , [ "OFFSET" , integer ] | "OFFSET" , integer , [ "LIMIT" , integer ] ;
select_list = "*" | select_item , { "," , select_item } ;
select_item = expr , [ [ "AS" ] , identifier ] ;
order_item  = expr , [ "ASC" | "DESC" ] , [ "NULLS" , ( "FIRST" | "LAST" ) ] ;
table_ref   = identifier | string_literal ;
expr        = expr , "OR" , expr | expr , "AND" , expr | "NOT" , expr | condition | sum ;
condition   = sum , cmp_op , sum
            | sum , [ "NOT" ] , "LIKE" , sum
            | sum , [ "NOT" ] , "IN" , "(" , expr , { "," , expr } , ")"
            | sum , [ "NOT" ] , "BETWEEN" , sum , "AND" , sum ;
sum         = sum , ( "+" | "-" ) , product | product ;
product     = product , ( "*" | "/" | "//" | "%" ) , unary | unary ;
unary       = "-" , unary | postfix ;
postfix     = primary , { "::" , type } ;
primary     = identifier | literal | "(" , expr , ")" | agg_call
            | identifier , "(" , [ expr , { "," , expr } ] , ")"
            | "CASE" , [ expr ] , "WHEN" , expr , "THEN" , expr , { "WHEN" , expr , "THEN" , expr } ,
              [ "ELSE" , expr ] , "END"
            | "EXTRACT" , "(" , identifier , "FROM" , expr , ")"
            | ( "CAST" | "TRY_CAST" ) , "(" , expr , "AS" , type , ")" ;
type        = identifier , [ "(" , integer , { "," , integer } , ")" ] ;
agg_call    = "COUNT" , "(" , "*" , ")"
            | "COUNT" , "(" , "DISTINCT" , expr , ")"
            | ( "COUNT" | "SUM" | "AVG" | "MIN" | "MAX" ) , "(" , expr , ")" ;
cmp_op      = "=" | "<>" | "!=" | "<" | "<=" | ">" | ">=" ;
literal     = [ "-" ] , integer | [ "-" ] , decimal | string_literal | "DATE" , string_literal
            | "TIMESTAMP" , string_literal ;
```

Operators bind from loosest to tightest: `OR`, `AND`, `NOT`, the comparisons with `LIKE`, `IN` and `BETWEEN` (which do
not chain: `a = b = c` and `a BETWEEN 1 AND 2 = b` are unsupported; the bounds of `BETWEEN` bind like the operand of
`+`, so its `AND` is its own and `a BETWEEN 1 AND 2 AND b = 3` is two conjuncts), `+` and `-`, `*`, `/`, `//` and `%`,
unary `-`, `::`; binary operators are left-associative, and parentheses group. An expression may be at most 256 levels
deep, counted along its deepest path through operators and parentheses (the top-level `AND` chain of `WHERE` and
`HAVING` does not count, unless an `OR` makes it one tree), else it is unsupported. In a chain such as `a + b + c` each
operator pushes everything before it one level down, so `f(f(...)) + 1 + 1` counts the calls and the operators together.
A `::` and a unary `-` count one level more, as they do in the canonical form `CAST(x AS T)` and `-(x)` (a `-` before a
parenthesized operand does not), and so does a `NOT` that is the right operand of a comparison or of arithmetic, a
`LIKE` pattern or a `BETWEEN` bound (`a = NOT b` is `a = (NOT b)`). The canonical form writes a `WHERE` or `HAVING`
predicate with a top-level `OR` without parentheses, so it reads back as it was parsed. Aggregates are allowed in the
select list, `HAVING` and `ORDER BY`, and cannot be nested.

Lexical rules: an `identifier` is a letter or `_` followed by letters, digits or `_`, or any text in double quotes
(`""` escapes a quote); a `string_literal` is text in single quotes (`''` escapes a quote); an `integer` is a
sequence of digits; a `decimal` is a number with a decimal point, an exponent or both (`1.5`, `.5`, `5.`, `1e3`).
Keywords are not reserved by the lexer. A `-` directly before a number makes a negative literal, except when `::`
follows the number: `-1::INTEGER` is `-(CAST(1 AS INTEGER))`, as in DuckDB. The `identifier` of a `type` is unquoted
and case-insensitive (`date` is `DATE`); its parameters are integers (`DECIMAL(15, 2)`). `CAST(x AS T)` and `x::T`
are the same expression.

**What the binder answers today.** Of the expressions above, antb1 answers:

- date casts: `CAST('YYYY-MM-DD' AS DATE)` and `'YYYY-MM-DD'::DATE` are the literal `DATE 'YYYY-MM-DD'`
  wherever it may stand, as in DuckDB (which names all three `CAST('YYYY-MM-DD' AS "DATE")`);
- value expressions: columns, literals, aggregates, arithmetic (`+ - * / // %`, unary `-`), the functions
  `strlen(varchar)`, `regexp_replace(varchar, 'pattern', 'replacement')`, `toDateTime(integer)`,
  `EXTRACT(field FROM timestamp)` and `date_trunc('unit', timestamp)`, and `CASE` (both forms, with conditions
  as below) of them, in the select list, aggregate arguments (not constant ones), `GROUP BY` and `ORDER BY` (literals
  there are positions or constants, see [Binding](#binding));
- conditions (`WHERE`, `HAVING` and `CASE WHEN`): `operand <op> literal` in either order, `operand <op> operand`,
  `operand [NOT] LIKE 'pattern'`, `operand [NOT] IN (literal, ...)` and `operand [NOT] BETWEEN low AND high`
  (`low <= operand AND operand <= high`, each bound a literal or an operand), where an operand is a value
  expression that reads a column (in `HAVING`, and in a `CASE` of an aggregate query, also one over aggregates),
  combined with `AND`, `OR` and `NOT`. Instead of a literal, a comparison or a bound may be a constant integer
  expression: integer literals under unary `-`, `+`, `-` and `*` (`a > 2 * 1000`), folded to its value in DuckDB's
  types (each literal `INTEGER` when its magnitude fits, else `BIGINT` or `HUGEINT` by its value, so `-2147483648`
  is a `BIGINT`; each operation in the wider type of its operands); one that overflows `INTEGER` or `BIGINT` is a
  bind error (exit code 1, divergence D17), and a `HUGEINT` value of more than 38 digits is unsupported. Other
  constant expressions (`/`, `//`, `%`, decimals) are unsupported. DuckDB compares the three values of a `BETWEEN` in
  one common type and antb1 each comparison in its own, so when a bound is `DOUBLE` (a `DOUBLE` column, a literal
  with an exponent or of more than 38 digits) and the operand is not, a `BETWEEN` that also has a `BIGINT` or
  `HUGEINT` value (a column, a literal or a folded constant of that type), a `FLOAT` column, or a decimal literal and
  an integer value is unsupported; one that mixes a `VARCHAR` value with a `DATE` or `TIMESTAMP` value is a bind
  error, as in DuckDB. As in DuckDB, every unary minus directly over an integer literal (through
  any nesting) belongs to the literal, so `-(-9223372036854775808)` is a `HUGEINT`, not an overflow, and
  `-(-(-9223372036854775808))` a `BIGINT`. A plain `BETWEEN` in the `AND` chain of `WHERE` or
  `HAVING` is its two comparisons, each folded and, over a table column, pushed into the scan like any comparison;
  `NOT BETWEEN`, `NOT (a BETWEEN ...)` and `BETWEEN` under `OR` or in `CASE` are compound conditions. DuckDB's names
  apply: `(a BETWEEN 1 AND 2)`, and both negations `(NOT (a BETWEEN 1 AND 2))`;
- any of these in parentheses (`(a)`, `SUM((a))`, `WHERE (a = 1 AND b = 2)`), which group without changing
  anything.

Every other expression (function calls other than the five aggregates and the functions above, `EXTRACT` of other
fields, a condition used as a value, as in `SELECT a = 1`, a comparison of two constants, a bare column as a
condition, `TRY_CAST` and every other cast, such as `CAST(a AS BIGINT)` or `CAST(d AS DATE)`) parses, and is then
rejected by the binder with exit code 4 at its first unsupported token, before any name is resolved. `GROUP BY ALL`
and `ORDER BY ALL` are rejected by the parser, and so are `SUM`, `AVG`, `MIN` and `MAX` with `DISTINCT`.

Outside the grammar, the parser recognizes common SQL and rejects it with exit code 4 and a source span, among others:
`SELECT DISTINCT`, joins, subqueries, `ILIKE`, `LIKE ... ESCAPE`, `NULL` literals, `IS [NOT] NULL`, `||`,
window functions, unary `+`, and in casts quoted or qualified type names, type names of several words
(`DOUBLE PRECISION`, `TIMESTAMP WITH TIME ZONE`), array types, `INTERVAL` and `UNION` types and type parameters other
than integers. Malformed SQL, such as `SELECT COUNT(*) FORM t`, is a syntax error with exit code 1.

## Binding

The binder (`plan::Bind`) resolves the statement against the table and builds the logical plan. A bind error has exit
code 1 and points at the offending name, call or literal; the first error in query order wins (the table, then the
select list, `WHERE`, the `GROUP BY` names, the grouping rule below, `LIMIT`, `OFFSET`, `HAVING` and the `ORDER BY`
items).

- Names: table and column names match ASCII case-insensitively, quoted identifiers included (as in DuckDB). An
  unknown table or column is a bind error, and so is a name that matches two columns differing only in case. A
  column of an unsupported type fails with exit code 4 wherever it is referenced (by `SELECT *` too).
- Select list: `*` alone, or plain columns, aggregates and constants. Without `GROUP BY`, aggregates (in the select
  list, in `HAVING` or in `ORDER BY`) and `HAVING` itself cannot be mixed with plain columns: a bind error at the
  first plain column (or at `*`). Constants mix with anything; with an aggregate (also one only in `HAVING` or
  `ORDER BY`) or `HAVING` the query has one row.
- Constants (as DuckDB types and names them): an integer is INTEGER when its magnitude fits (so `-2147483648` is
  BIGINT), else BIGINT or HUGEINT, and is named by its value (`007` is `7`); a string is VARCHAR named with its
  quotes (`'it''s'`); `DATE '2020-01-02'` is DATE named `CAST('2020-01-02' AS "DATE")` (so are
  `CAST('2020-01-02' AS DATE)` and `'2020-01-02'::DATE`), and
  `TIMESTAMP '2020-01-02 10:00:00'` TIMESTAMP named `CAST('2020-01-02 10:00:00' AS TIMESTAMP)`. A decimal (DuckDB's
  DECIMAL) and an integer beyond HUGEINT's 38 digits are unsupported (exit code 4).
- Positions: in `GROUP BY` and `ORDER BY` an integer literal names the select item at that position (1-based; `*`
  counts every column); one out of range, a negative one too, is a bind error. `GROUP BY` of an aggregate item is a
  bind error. Any other literal is a constant: in `GROUP BY` it is no key but still makes the query grouped (one group
  when there are rows, none otherwise), in `ORDER BY` a number or string is a bind error (it would order nothing,
  as in DuckDB) and a DATE literal orders nothing. A position naming a constant item groups or orders nothing.
- `GROUP BY`: each name is a table column, or else the alias of a plain column of the select list (as in DuckDB; an
  alias of an aggregate is a bind error, an alias of a constant is no key); a key repeated, also in another spelling,
  is one key, and a key need not be selected. Every plain column of the select list, and every column of `SELECT *`,
  must be a key: otherwise a bind error at that column
  (`column 'b' must appear in the GROUP BY clause or be inside an aggregate function`).
- Result types: `COUNT(*)`, `COUNT(col)` and `COUNT(DISTINCT col)` are BIGINT; `SUM` of an integer column is
  HUGEINT and of a DOUBLE column DOUBLE; `AVG` of a number is DOUBLE and of a DATE or TIMESTAMP TIMESTAMP, as in
  DuckDB; `MIN` and `MAX` have the type of their column. `SUM` of a VARCHAR, DATE or TIMESTAMP and `AVG` of a VARCHAR
  are bind errors.
- Result names follow DuckDB: a plain column is named as declared in the table (`SELECT regionid` gives
  `RegionID`); an aggregate is named `count_star()`, `count(x)`, `count(DISTINCT x)`, `sum(x)`, `avg(x)`, `min(x)`
  or `max(x)`, with the argument as written in the query, double-quoted when it is not a plain identifier or is a
  reserved word (`sum("from")`). Constants: see above. An alias replaces the name.
- `ORDER BY`: a name is the alias of a select item first (the last item with that alias, as in DuckDB; also when a
  table column has the same name), else a table column; an aggregate call is computed like a select-list aggregate.
  A projection may order by any column of the table, selected or not. A grouped query orders by its keys and
  aggregates (an aggregate the select list lacks is computed and not returned); any other column is a bind error
  (`column 'b' must appear in the GROUP BY clause or be inside an aggregate function`). An aggregate query without
  `GROUP BY` has one row, so it may order by aggregates only. A key that repeats an earlier one changes nothing.
- `HAVING`: an aggregate call is computed like a select-list aggregate (one the select list lacks is computed and
  not returned). A name is a `GROUP BY` key when the table column of that name is one, else the alias of a select
  item (the last one with it, as in DuckDB): a key or an aggregate; an alias of a constant is unsupported (exit code
  4), and any other column is a bind error (`column 'b' must appear in the GROUP BY clause or be inside an aggregate
  function`). Without `GROUP BY` there are no keys. Literals are typed like those of `WHERE` (below) against the
  operand's type: `COUNT` is BIGINT, an integer `SUM` HUGEINT, `AVG` and a DOUBLE `SUM` DOUBLE, `MIN` and `MAX` their
  column's type (and FLOAT for a FLOAT column, as in DuckDB, while its `SUM` and `AVG` are DOUBLE).
- Arithmetic (as DuckDB types it): an integer literal operand that fits the other operand's integer type takes that
  type (`smallint_col + 1` is SMALLINT, `smallint_col + 40000` INTEGER); two integer types give the wider one, where
  USMALLINT with SMALLINT gives BIGINT; an integer literal alone is INTEGER, BIGINT or HUGEINT by its value; DOUBLE
  with anything is DOUBLE, and so is a number DuckDB types as DOUBLE (`1e3`) or a decimal with a DOUBLE; `/` is always
  DOUBLE; unary `-` keeps the type. A decimal literal with an integer (DuckDB's DECIMAL), DATE arithmetic, negating a
  USMALLINT (DuckDB wraps it), `//` and `%` of HUGEINT values and arithmetic on FLOAT columns (divergence D11) are
  unsupported; arithmetic on VARCHAR is a bind error. The result name is DuckDB's: `(a + 1)`, `-(a)`,
  `sum((a + 1))`, with columns as written.
- Functions (names ASCII case-insensitive): `strlen(x)` takes a VARCHAR and is BIGINT; `regexp_replace(x, 'pattern',
  'replacement')` takes a VARCHAR and two string literals and is VARCHAR. A wrong number of arguments, another type or
  a non-literal pattern or replacement is a bind error (`strlen() needs a VARCHAR, but 'i16' is SMALLINT`); DuckDB's
  optional fourth argument (options), `\Q` in a pattern, `\8` and `\9` in a replacement and any other function are
  unsupported (exit code 4). The result name is
  DuckDB's: `strlen(URL)`, `regexp_replace(URL, '^(.)', '\1')`.
- Timestamps: `toDateTime(t)` is ClickBench's DuckDB macro `epoch_ms(t * 1000)` (its `duckdb-parquet/create.sql` at
  the pinned commit; the test oracle defines the same macro): `t * 1000` is typed and checked as arithmetic (so a
  SMALLINT `t` above 32 overflows, as in DuckDB), and the result is TIMESTAMP. It takes an integer other than
  HUGEINT; any other type is a bind error. `EXTRACT(field FROM x)` takes a TIMESTAMP or DATE and is BIGINT (`epoch`:
  DOUBLE); `date_trunc('unit', x)` takes a TIMESTAMP or DATE and a string literal unit and is TIMESTAMP. Fields and
  units are DuckDB's, in its spellings (case-insensitive): `year` (`years`, `y`, `yr`, `yrs`), `quarter`(`s`),
  `month` (`months`, `mon`, `mons`), `week` (`weeks`, `w`, `weekofyear`), `day` (`days`, `d`, `dayofmonth`),
  `hour` (`hours`, `h`, `hr`, `hrs`), `minute` (`minutes`, `m`, `min`, `mins`), `second` (`seconds`, `s`, `sec`,
  `secs`), `millisecond` (`milliseconds`, `ms`, `msec`, `msecs`), `microsecond` (`microseconds`, `us`, `usec`,
  `usecs`), `decade` (`decades`, `decs`; `dec` for `date_trunc` only), `century` (`centuries`, `c`, `cent`),
  `millennium` (`millennia`, `mil`, `mils`), `isoyear`, `epoch`, and for `EXTRACT` also `dow`
  (`dayofweek`, `weekday`), `isodow` and `doy` (`dayofyear`), which `date_trunc` takes as `day` (`epoch` as
  `second`). `EXTRACT` names its field as DuckDB does: a spelling that is a DuckDB keyword (the singular and plural
  of `year` to `microsecond`, `quarter`, `week`, `decade`, `century` and `millennium`) by its field in lower
  case, any other as written. A TIMESTAMP compares with a TIMESTAMP literal, a string
  literal (the timestamp it spells) or a DATE literal (its midnight), exactly, as DuckDB casts them; in `CASE` a
  string literal next to TIMESTAMP values is a TIMESTAMP. Comparing a TIMESTAMP with a number is a bind error. Other
  fields and units, comparing a TIMESTAMP with a DATE operand, a DATE with a TIMESTAMP literal, and TIMESTAMP
  arithmetic are unsupported (exit code 4). The result names are DuckDB's: `todatetime(EventTime)`,
  `main.date_part('minute', todatetime(EventTime))`, `date_trunc('minute', todatetime(EventTime))`.
- `CASE` (as DuckDB types it): the values (`THEN` and `ELSE`) take their common type, where an integer literal takes
  the other values' integer type when it fits (`CASE WHEN .. THEN smallint_col ELSE 0 END` is SMALLINT), two integer
  types give the wider one (USMALLINT with SMALLINT: INTEGER, unlike arithmetic), DOUBLE with any number DOUBLE, and
  a string literal takes VARCHAR or DATE (`ELSE '2013-07-15'` next to a DATE); without other values literals give
  their own types, and no value at all but string literals VARCHAR. A VARCHAR or DATE value with a number is a bind
  error (`cannot mix values of type VARCHAR and INTEGER in CASE`); a string literal next to numbers is unsupported
  (DuckDB casts it to the number). A decimal literal (DuckDB's DECIMAL) without a DOUBLE
  value and a FLOAT column as a value (divergence D11) are unsupported. `CASE x WHEN v THEN ..` is
  `CASE WHEN x = v THEN ..`. The result name is DuckDB's: `CASE  WHEN ((a = 1)) THEN (b) ELSE NULL END`.
- Conditions with `OR` and `NOT` (and `AND` below them): each comparison, `LIKE` and `IN` is bound and folded exactly
  as a `WHERE` comparison, and a `WHERE` or `HAVING` conjunct with `OR` or `NOT` is computed as one condition and
  filters on it.
- Expressions and keys: a select, `HAVING` or `ORDER BY` expression equal to a `GROUP BY` expression is that key (so
  `SELECT a - 1 ... GROUP BY a - 1` works), and in a grouped query every column must be inside an aggregate or part of
  such a key. Inside an `ORDER BY` or `HAVING` expression a name is a table column first, else a select alias (as in
  DuckDB; a bare name is an alias first, see above). A `GROUP BY` or `ORDER BY` expression without a column (`1 + 1`)
  is unsupported.
- Comparisons of two operands (`a < b`, `a + 1 = b * 2`) compare numbers in their common type, VARCHAR with VARCHAR
  and DATE with DATE; any other pair is a bind error. A comparison of an operand with a literal folds the literal into
  the operand's type, as for a column.
- `LIMIT n` and `OFFSET m` take integers from 0 to 9223372036854775807, in either order.

Literals in `WHERE` and `HAVING` must fit the column's (or the aggregate's) type; any other combination is a bind
error that points at the literal:

| Column type | Literals | Compared as |
| --- | --- | --- |
| SMALLINT, INTEGER, BIGINT, USMALLINT, HUGEINT | integer, decimal | exactly, after folding (below) |
| DECIMAL(p,s) | integer, decimal | exactly, after folding into the column's scale (below); a number DuckDB types as DOUBLE is unsupported (exit code 4) |
| DOUBLE | integer, decimal | the nearest double, as in DuckDB for a DOUBLE column; beyond the double range `inf` or `-inf`, below the smallest subnormal `0`. A column stored as FLOAT is compared as DuckDB compares it: an integer or DECIMAL literal becomes the FLOAT that DuckDB casts it to, with DuckDB's rounding (`0.1` is `0.1F`; `16777217.5` and some long spellings of `0.1`, such as 16 or 24 decimals, are not the nearest FLOAT, and HUGEINT literals are rounded through a double), beyond the FLOAT range `inf` or `-inf`; a number DuckDB types as DOUBLE compares with the nearest double |
| VARCHAR | string | bytes; `LIKE` and `NOT LIKE` take a string pattern (only a VARCHAR column: LIKE on another type is a bind error, as in DuckDB) |
| DATE | string, `DATE` string, date cast | a date written exactly `YYYY-MM-DD` (years 0000 to 9999) that exists in the calendar |
| TIMESTAMP (an expression) | string, `TIMESTAMP` string, `DATE` string, date cast | a timestamp written `YYYY-MM-DD`, optionally followed by a space (or `T`) and `HH:MM` or `HH:MM:SS` (hours 00 to 23) and an optional fraction of up to 9 digits (past the sixth truncated, as in DuckDB), or a DATE's midnight |

A comparison of an integer column with a number is folded exactly at bind time, never through a lossy cast:

- A decimal that is not an integer becomes the nearest integer on the side the comparison keeps: `c > 1.5` and
  `c >= 1.5` become `c >= 2`, `c < -1.5` and `c <= -1.5` become `c <= -2`. `c = 1.5` is never true and `c <> 1.5` is
  true for every value. Decimals with an integer value compare as that integer (`c = 2.0`, `c > 1e3`).
- A number that DuckDB types as DOUBLE, one with an exponent (`1e3`) or a decimal of more than 38 digits (leading
  zeros count), is first rounded to the nearest double, as in DuckDB, and that double is folded exactly:
  `c = 1.0000000000000000000001e0` is `c = 1` (divergence D7 for BIGINT values beyond 2^53).
- A number outside the column type's range (SMALLINT -32768 to 32767, USMALLINT 0 to 65535, INTEGER and BIGINT
  their 32-bit and 64-bit ranges, HUGEINT -(10^38 - 1) to 10^38 - 1) makes the comparison true for every value or
  for none, depending on the operator: `smallint_col < 40000` is always true, `usmallint_col = -1` never.
- A comparison that is true for every value still rejects NULL: EXPLAIN shows it as `c IS NOT NULL`. One that is
  never true shows as `FALSE` and reads no column.
- A DECIMAL(p,s) column folds the same way in units of its scale: the number is multiplied by 10^s exactly, a
  remainder moves it to the side the comparison keeps, and the range is -(10^p - 1) to 10^p - 1 units. On
  DECIMAL(15,2), `c <= 12.345` becomes `c <= 12.34`, `c >= 12.345` becomes `c >= 12.35`, `c = 12.345` is never true,
  and `c < 10000000000000` is true for every value. DuckDB compares in a DECIMAL capped at 38 digits instead
  (divergence D13). A number DuckDB types as DOUBLE (an exponent, or more than 38 digits) is unsupported for a
  DECIMAL column (exit code 4), as is a comparison with an expression of another type.

## Logical plans and EXPLAIN

A bound query is a tree of logical nodes: `Scan` (reads fields of the table), `Filter` (the `WHERE` conjunction, or
above the aggregation the `HAVING` one), `Compute` (appends one computed column per expression: over the table for
`WHERE` operands, aggregate arguments and `GROUP BY` expressions, and over the aggregation for select, `HAVING` and
`ORDER BY` expressions),
`Project` (`*` or the plain columns), `Aggregate` (the aggregates, one output row) or `GroupAggregate` (`GROUP BY`:
the keys, then the aggregates, one row per group; a `Project` above it restores the select order), `Sort`
(`ORDER BY`, below the `Project`, so it can use columns and aggregates the query does not return) and `Limit` (with
its offset). A `Join` has two inputs ([ADR 0022](adr/0022-joins-and-query-blocks.md)): its kind (inner, left, semi,
anti, null-aware anti or one-row), key pairs of one type each, residual conditions over both inputs and the input it
builds on. No query produces a `Join` yet, and the executor rejects a plan with one with exit code 4 until it
implements its kind. A rule optimizer then rewrites the plan, through both inputs of every `Join`:

- `Limit` moves below `Project`: a `Project` keeps every row, so the `Limit` copies only the rows it keeps and ends
  up right above a `Sort`, which the executor runs as a top-N (it keeps only `limit + offset` rows while it reads);
- a `GROUP BY` key computed only from other keys, none of them DOUBLE (`GROUP BY x, x + 1`), is dropped from the
  `GroupAggregate` and computed once per group in a `Compute` above it, with a `Project` restoring the output: the
  same groups with fewer keys to hash ([ADR 0018](adr/0018-dependent-group-keys.md)). Not under a `LIMIT` without
  `ORDER BY`, which reads only some groups: every group's keys are computed, so an overflow in any of them still
  fails the query;
- projection pruning: `Scan` reads only the columns that the nodes above it use (none for a bare `COUNT(*)`); each
  input of a `Join` keeps the columns used above it and the join's key and residual columns;
- `COUNT(*)` alone without `WHERE`, over a table whose row count is known without scanning (every Parquet table),
  becomes `RowCount`, answered from the footers.
- (execution, not a plan rule) an `ORDER BY ... LIMIT` over a table reads first only the columns its `WHERE` and its
  keys use, and the other columns only for the rows of the result, when `LIMIT + OFFSET` is at most half of the row
  groups read ([ADR 0016](adr/0016-late-materialization.md)); `antb1 explain --analyze` shows `late=N columns`.

`antb1 explain` prints the optimized plan: an `Output:` line with the result names and types, then one line per
node from the root down, each input indented by two more spaces. Names that are not plain identifiers are
double-quoted, and bytes outside printable ASCII are written as `\xHH`. A folded constant prints in its operand's
type: against a DECIMAL column with the column's scale (`p <= 12.34`, `d IN (1.25, 5.00)`):

```text
Output: count_star():BIGINT width:HUGEINT
Aggregate COUNT(*), SUM(ResolutionWidth)
  Filter IsMobile = 1 AND UserAgent >= 2 AND RegionID IS NOT NULL
    Scan table=t source=parquet(files=1, rows=10000) columns=[RegionID, UserAgent, ResolutionWidth, IsMobile]
```

```text
Output: RegionID:INTEGER c:BIGINT
Project RegionID, c
  Limit 10 OFFSET 5
    Sort c DESC NULLS LAST, "max(EventTime)" ASC NULLS FIRST
      GroupAggregate keys=[RegionID] COUNT(*), MAX(EventTime)
        Scan table=t source=parquet(files=1, rows=10000) columns=[EventTime, RegionID]
```

A `Join` line shows its kind (`INNER`, `LEFT`, `SEMI`, `ANTI`, `NULL-AWARE ANTI` or `ONE-ROW`), the input it builds
on, its key pairs and, when it has any, its residual conditions; its left input follows, then its right input, both
indented. A column whose binding the plan records is shown qualified, as `a.k`:

```text
Join INNER build=right keys=[a.k = b.k] residual=[(a.x < b.y)]
  Scan table=a source=parquet(files=1, rows=1000) columns=[k, x]
  Scan table=b source=parquet(files=1, rows=10) columns=[k, y]
```

### `explain --analyze`

`antb1 explain --analyze` runs the query, drops its rows, and prints the physical plan with what each operator did
([ADR 0015](adr/0015-query-profiles.md)). It takes `--threads`, `--memory-limit` and `--format text|json` (`--format`
only with `--analyze`); its errors and exit codes are those of `antb1 query`.

- The text starts with the `Output:` line and a `Total:` line (time, result rows, peak memory, threads).
- Then comes one line per physical operator, root first, indented as EXPLAIN: its name, its logical node's EXPLAIN
  text (without a first word that repeats the name), and in brackets:
  - `rows` and `batches` it returned;
  - its `time`, or for an operator of a part pipeline, `parts=N` and its time summed over the parts;
  - `self`, its time without the inputs that ran inside it;
  - its metrics.
- A `Scan` that applies predicates of the `Filter` above it while it reads (filter pushdown,
  [ADR 0020](adr/0020-filter-pushdown.md)) says so after its EXPLAIN text, `, N pushed predicates`; its `rows` are
  then those that passed them.
- Metrics:
  - `parts`, `skipped` (by statistics);
  - `raw_parts`, `raw_rows` (a GROUP BY's parts that sent rows straight to the partitions, because their first
    rows hardly reduced);
  - `part_time` (the part tasks' time, summed);
  - `wait` (for parts);
  - `merge`, `lanes_tail` (merging after the last part), `build` (output rows), `outer`;
  - `sort`, `groups`, and `sample_parts`, `heavy_keys`, `heavy_groups` of a two-level aggregation.
- The counts do not depend on the number of threads, except under a `LIMIT` (it stops parts that already started,
  and how many depends on the threads) and after a part ran out of memory next to others (it runs again alone). The
  times do depend on the threads.
- With `--format json`, errors are JSON objects, as with `antb1 query --format json`.
- `--format json` prints the same as one object, times in nanoseconds and memory in bytes.

```text
Output: RegionID:INTEGER c:BIGINT
Total: time=39.106ms rows=3 peak_memory=4.59 MB threads=1
Project RegionID, c  [rows=3 batches=1 time=38.946ms self=0.007ms]
  TopN Sort c DESC NULLS LAST, RegionID ASC NULLS LAST Limit 3  [rows=3 batches=1 time=38.939ms self=11.831ms sort=0.011ms]
    PartGroupAggregate GroupAggregate keys=[RegionID] COUNT(*)  [rows=4357 batches=698 time=27.108ms parts=11 skipped=0 part_time=6.161ms wait=6.185ms lanes_tail=0.001ms build=2.297ms groups=4357]
      Filter IsMobile = 1  [rows=5064 batches=11 parts=11 time=2.075ms (summed over parts) self=0.288ms]
        Scan table=t source=parquet(files=4, rows=10000) columns=[RegionID, IsMobile]  [rows=10000 batches=11 parts=11 time=1.787ms (summed over parts)]
```

## Types

| Parquet column | Arrow type as read | Engine type | Notes |
| --- | --- | --- | --- |
| INT32 annotated INT(16, signed) | int16 | SMALLINT | |
| INT32 | int32 | INTEGER | |
| INT64 | int64 | BIGINT | |
| INT32 annotated INT(16, unsigned) | uint16 | USMALLINT | read as DATE with `--clickbench` (EventDate) or `--column-type COL=DATE` |
| INT32 annotated DATE | date32 | DATE | |
| FLOAT | float | DOUBLE | widened exactly on read; `WHERE` compares like DuckDB (see Binding), results are DOUBLE (divergence D11) |
| DOUBLE | double | DOUBLE | |
| BYTE_ARRAY, unannotated | binary | VARCHAR | compared byte-wise |
| BYTE_ARRAY annotated STRING (UTF8) | utf8 | VARCHAR | same engine representation as unannotated |
| DECIMAL(p, s), p ≤ 38 (stored as INT32, INT64, FIXED_LEN_BYTE_ARRAY or BYTE_ARRAY) | decimal128(p, s); decimal32, decimal64 and decimal256 (an ARROW:schema may give them) are widened exactly on read | DECIMAL(p,s) | the exact unscaled value with scale s (Semantics); a DECIMAL(38, 0) column is DECIMAL(38,0), no longer HUGEINT ([ADR 0021](adr/0021-decimal-semantics.md) rule 2) |
| DECIMAL(p, s), p > 38 | – | unsupported | |
| – | decimal128(38, 0) | HUGEINT | only computed: the result type of integer SUM, and of integer literals beyond BIGINT |
| – | timestamp[us] | TIMESTAMP | only computed (`toDateTime`, `date_trunc`) or a literal; a Parquet timestamp column is unsupported |
| anything else | – | unsupported | `antb1 schema` shows `unsupported(<type>)`; a query that uses the column is rejected |

`--column-type COL=DATE` reinterprets a USMALLINT or INTEGER column as days since 1970-01-01; `--clickbench` is a
shortcut for `EventDate`. Both apply to every table (registered or opened with `FROM 'path'`) that has a column with
that name, matched ASCII case-insensitively like every column name (all columns it matches must be readable as DATE),
and tables without the column are left unchanged. The test oracle handles `FROM 'path'` differently (divergence D2
below).

## Semantics

The semantics follow DuckDB ([ADR 0004](adr/0004-types-null-overflow-semantics.md)).

- Identifiers: table and column names match ASCII case-insensitively, quoted identifiers included (as in DuckDB).
  Registering two tables whose names differ only in case is a usage error (exit code 2). A name that matches two
  columns differing only in case is a bind error.
- Literals: comparisons of a column with a literal are folded exactly at bind time, never through a lossy cast
  ([Binding](#binding)). A literal outside the column type's range turns the comparison into constant true or false
  for non-NULL values; NULL values still compare as NULL. A decimal literal compared with an integer column becomes
  an equivalent integer comparison: `c > 1.5` becomes `c >= 2`, and `c = 1.5` is never true.
- WHERE: the comparisons are evaluated with Arrow's comparison kernels and combined with Kleene AND; a row passes
  only when every comparison is true, so a comparison that is NULL rejects it. `OR` and `NOT` follow SQL's
  three-valued logic too (`NULL OR TRUE` is true, `NOT NULL` is NULL); a comparison folded to always or never true
  is still NULL for a NULL operand there, so `NOT (smallint_col = 1.5)` rejects NULL and keeps every other row.
- BETWEEN: `a BETWEEN lo AND hi` is `lo <= a AND a <= hi` in three-valued logic, both bounds inclusive: a range with
  `lo > hi` matches nothing, and a NULL operand (or a NULL bound column) makes it NULL, which rejects the row;
  `a NOT BETWEEN lo AND hi` is its negation, so it rejects that row too.
  An argument of `AND` or `OR` is computed only for the rows the earlier ones leave undecided (`x > 100 OR x * x > 0`
  never computes `x * x` where `x > 100`), as DuckDB does for the same order (divergence D16).
  VARCHAR compares byte-wise and DATE chronologically. A predicate folded to never-true reads no data at all.
- IN: `c IN (v1, v2, ...)` is `c = v1 OR c = v2 OR ...` with Kleene logic, and `c NOT IN (...)` its negation, so a
  NULL value rejects the row for both. Each value is typed and folded exactly like `c = v` ([Binding](#binding)); a
  value that no column value can equal (out of the column type's range, or not an integer for an integer column) is
  dropped, and without values `IN` is `FALSE` and `NOT IN` is `IS NOT NULL`. DuckDB gives the list one type: when a
  number in it types as DOUBLE (an exponent, or more than 38 digits), every number is read as a double, so an integer
  column compares with the nearest doubles (divergence D7) and a FLOAT column in DOUBLE (`f IN (0.1, 1e0)` does not
  match the FLOAT `0.1`, while `f IN (0.1, 2)` does). NaN is not equal to NaN, as for `=`
  (divergence D10).
- LIKE: `%` matches any sequence of characters (none included), `_` exactly one character, and every other byte
  itself; the match is case-sensitive and there is no escape character (`\` is a literal byte), as in DuckDB. A
  character is a UTF-8 sequence; in bytes that are not UTF-8 (which DuckDB refuses to read as VARCHAR) each byte is
  one character. `NOT LIKE` is the negation; a NULL value rejects the row for both. A pattern of only `%` folds:
  `LIKE` to `IS NOT NULL`, `NOT LIKE` to `FALSE`. Patterns without `_` are matched by their literal segments (a
  prefix, a suffix and substrings in order), the others by backtracking.
- COUNT: `COUNT(*)` counts rows; `COUNT(col)` counts non-NULL values; `COUNT(DISTINCT col)` counts distinct non-NULL
  values, with DOUBLE `-0.0` equal to `0.0` and every NaN one value (as DuckDB groups them) and VARCHAR by bytes. All
  return BIGINT, 0 over no values.
- SUM: over integer columns it accumulates in 128 bits and returns HUGEINT (decimal128(38, 0)), exactly like
  DuckDB, so it never overflows or wraps; Arrow's 64-bit `sum` kernel is never used. A result outside HUGEINT's
  range is an execution error (divergence D9); no column is HUGEINT, so only arithmetic on a `SUM` can reach it.
  Over DOUBLE it returns DOUBLE: it adds each row group's values (of each group), then the row group sums in order
  (the same for any number of threads; see Execution below). DOUBLE `AVG` sums the same way.
- AVG: over integer columns the sum accumulates exactly in 128 bits and is divided by the count once at the end, so
  the DOUBLE result is accurate to about one ulp (the oracle tests use a tight relative tolerance). Over DOUBLE it
  returns DOUBLE. Over DATE and TIMESTAMP it averages the microseconds (a DATE's midnight; its infinities as the
  TIMESTAMP ones, summed as their int64 values as DuckDB sums them, so that with finite values they give a large
  finite TIMESTAMP; a DATE beyond the TIMESTAMP range an error) in 128 bits and returns a TIMESTAMP, rounded as DuckDB
  rounds: the quotient truncated, one more when twice the remainder exceeds the count (a positive average to the
  nearest microsecond with halves down, a negative one toward zero).
- MIN and MAX: return the input type; VARCHAR compares byte-wise and DATE chronologically. They use Arrow's
  `min_max` kernel, over only the selected rows under `WHERE` (divergence D10 for NaN).
- NULL: aggregates skip NULLs. Over zero input rows `COUNT` returns 0 and `SUM`, `AVG`, `MIN` and `MAX` return NULL.
  A predicate that evaluates to NULL rejects the row.
- GROUP BY: one row per distinct combination of the keys; the aggregates of a group are exactly those of the same
  rows without `GROUP BY`. NULL is a key value (all NULLs form one group). A DOUBLE key groups `-0.0` with `0.0` and
  every NaN together, as DuckDB does, and the group shows the key as first seen. Over zero input rows there is no
  group, so no row. Groups come in no particular order (deterministic in antb1, not the order of first appearance),
  and the tests compare grouped results without regard to order; with `LIMIT` any groups are a right answer.
- Arithmetic: integer `+`, `-`, `*` and unary `-` compute in the result type, and an overflow fails the query with an
  execution error (exit code 1), as in DuckDB; HUGEINT (a `SUM` result) stays within -(10^38 - 1) to 10^38 - 1
  (divergence D9). `/` divides in DOUBLE: `x / 0` is `inf` or `-inf` and `0 / 0` NaN. `//` truncates toward zero
  and `%` takes the sign of the dividend (`-7 // 2` is -3, `-7 % 2` is -1); both are NULL for a zero divisor, and
  the type's minimum divided by -1 overflows. On DOUBLE, `//` divides (NULL for a zero divisor) and `%` is `fmod`
  (NaN for a zero divisor). NULL operands give NULL. An expression is computed only for the rows that `WHERE` keeps.
  Two rewrites of DuckDB's optimizer are reproduced, so that an overflow fails the same queries: without `GROUP BY`,
  `SUM(x + c)` (a signed integer `x`, an integer constant `c`) is `SUM(x) + c * COUNT(x)` in HUGEINT; and in every
  comparison (`WHERE`, `HAVING`, `CASE WHEN`; `x` a column, a key, an aggregate or an alias), `x + c <op> k`,
  `x - c <op> k`, `c - x <op> k` and `x * c <op> k` (a signed integer `x`, integer constants, `c` dividing `k` for
  `*`) compare `x` with a moved constant, repeatedly, while `k` and the new constant fit the type.
  The arithmetic is then never computed. Divergence D14 lists what still differs.
- Timestamps: microseconds since 1970-01-01 00:00:00, without a time zone, printed as DuckDB prints them
  (`2013-07-15 14:00:00`, a fraction without trailing zeros, `0001-12-31 (BC) 23:59:59` before year 1).
  `toDateTime` fails the query when `t * 1000` overflows its type or the time is outside DuckDB's TIMESTAMP range,
  `290309-12-22 (BC) 00:00:00` to `294247-01-10 04:00:54.775807`, as in DuckDB. `EXTRACT` gives the civil field
  (the year astronomically: 1 BC is 0; `week` and `isoyear` are ISO 8601's; `dow` counts from Sunday = 0,
  `isodow` from Monday = 1; `millisecond` and `microsecond` include the seconds; `epoch` is the seconds since 1970;
  `decade` is the year / 10, `century` and `millennium` count from year 1 = 1 and go down from -1 before it) and
  `date_trunc` rounds down (`decade` to `millennium` truncate the year toward zero, as DuckDB does: 2013 is 2000
  for both century and millennium; `isoyear` gives the Monday of ISO week 1; before 1970 too:
  `date_trunc('minute', toDateTime(-61))` is `1969-12-31 23:58:00`), in 64-bit calendar arithmetic over the whole
  range; weeks start on Monday. A DATE is the timestamp of its midnight; `date_trunc` of a DATE whose result is
  outside the TIMESTAMP range fails the query, and of an infinite DATE is infinite, where `EXTRACT` is NULL.
  TIMESTAMP values sort, group and aggregate (`MIN`, `MAX`, `COUNT`, `COUNT(DISTINCT)`) like integers.
- Strings: `strlen` counts bytes. `regexp_replace` replaces the first match only, with RE2 in UTF-8 mode as DuckDB
  runs it (`.` matches one character but no newline); in the replacement `\0` is the match, `\1` to `\9` its groups
  and `\\` a backslash. A replacement RE2 rejects (a group the pattern does not have, a lone or unknown backslash)
  leaves the text unchanged, as in DuckDB; an invalid pattern fails the query (exit code 1) when it runs. NULL gives
  NULL. Bytes that are not UTF-8 are divergence D15. It evaluates the pattern once per distinct
  value of a batch (values repeat within a batch), which only changes the speed.
- CASE: the first branch whose condition is true (a NULL condition is not) gives the value, else `ELSE`, else NULL.
  As in DuckDB, a condition is computed only for the rows no earlier branch took and a value only for the rows its
  branch answers, so `CASE WHEN x < 100 THEN x * 1000 END` never overflows on the other rows.
- HAVING: its conditions filter the rows of the aggregation (the groups, or the one row of an aggregate query
  without `GROUP BY`, which it may filter out) before `ORDER BY`, `LIMIT` and `OFFSET`, with the NULL, folding and
  operator rules of `WHERE`: a NULL aggregate (`SUM` of only NULLs) or NULL key rejects the row.
- ORDER BY: rows in the order of the first key, ties broken by the next ones. `ASC` is the default; NULLs come last
  unless `NULLS FIRST` is written, for `ASC` and `DESC` alike (DuckDB's default). VARCHAR orders by bytes, DATE
  chronologically, and DOUBLE with `-0.0` equal to `0.0` and NaN above every number (equal to NaN), as in DuckDB.
  Rows with equal keys come in no particular order (antb1 keeps their input order); the tests accept any order of
  tied rows, and any of the tied rows at the edges of a `LIMIT`/`OFFSET` window.
- LIMIT and OFFSET: `OFFSET m` skips the first `m` rows, then `LIMIT n` keeps at most `n`. With `ORDER BY` that is a
  window of the order. Without it, the first rows in file and row group order: the scan stops as soon as the window
  is out. `LIMIT 0` returns no row, also for an aggregate. Without `ORDER BY` a projection returns its rows in file
  order too, but SQL does not promise an order, and the tests compare such results without regard to order (with
  `LIMIT` or `OFFSET`, any rows of the full answer are right).
- VARCHAR: values are raw bytes. Unannotated BYTE_ARRAY columns (as in ClickBench) are VARCHAR and are never
  validated as UTF-8.
- DECIMAL: a DECIMAL(p,s) value is an exact integer of at most p digits, the unscaled value, times 10^-s
  ([ADR 0021](adr/0021-decimal-semantics.md)). Comparisons with numbers (`=`, `<>`, `<`, `IN`, `BETWEEN`), `ORDER BY`,
  `GROUP BY`, `COUNT(DISTINCT)`, `MIN` and `MAX` use the unscaled value; `MIN` and `MAX` keep DECIMAL(p,s), `COUNT`
  is BIGINT. Two DECIMAL columns compare only when both are the same DECIMAL(p,s). Not supported yet (exit code 4):
  `SUM`, `AVG`, arithmetic and unary `-` on a DECIMAL, a DECIMAL as a `CASE` value, and comparing a DECIMAL with
  another type (an integer or DOUBLE expression, a DECIMAL of another precision or scale, a number DuckDB types as
  DOUBLE). A `WHERE` condition on a DECIMAL column is evaluated by the `Filter`; it is not pushed into the scan and
  does not skip row groups.
- Execution: the row groups of a query run on `--threads` threads (default: the hardware threads), 64Ki-row
  batches; their results are combined in file and row group order, so a result is the same for any number of
  threads, and deterministic. A DOUBLE `SUM` or `AVG` adds up every row group (per group with `GROUP BY`),
  then the row group sums in order: it can differ from one running sum (and from DuckDB) by rounding. The order of
  the groups of a `GROUP BY` follows a hash of their keys and the row groups, the same for any number of threads.
- Memory: a query may hold at most `--memory-limit` bytes (default: 80% of physical memory, as DuckDB's
  `memory_limit`; sizes such as `4GB`, `512MiB` or `50%`): the data it reads and computes and its results. A query
  that needs more fails with a `memory` error (exit code 1), never with a crash; under pressure it reads fewer row
  groups at a time, down to one, and a row group that runs out of memory next to others is read again alone.

## Output formats

`antb1 query --format table|csv|json` (default `table`). Every format renders values with the same canonical
formatter:

- integers (HUGEINT included) exactly; DECIMAL(p,s) as DuckDB prints it, with exactly s digits after the point and a
  leading `0` only when p > s (`17.00`, `-0.25`; DECIMAL(3,3) `.500`); DOUBLE as the shortest round-trip form
  (`nan`, `inf`, `-inf` for non-finite values); DATE as `YYYY-MM-DD`, and like DuckDB outside years 1 to 9999:
  `YYYY-MM-DD (BC)` before year 1, more year digits after 9999, `infinity` and `-infinity` for DuckDB's sentinel
  day numbers; VARCHAR as its raw bytes;
- NULL as `NULL` in `table`, an empty field in `csv` and `null` in `json`;
- `csv` has a header row and RFC 4180 quoting, with LF line endings; `json` is an array of objects, where numbers are
  JSON numbers except HUGEINT, DECIMAL and non-finite doubles, which are strings (exactness), and every byte of ill-formed
  UTF-8 in strings (RFC 3629, so also overlong forms, surrogates and code points above U+10FFFF) is written as the
  text `\xHH` with lowercase hex digits, so the output is always valid UTF-8; `table` and `csv` write the raw bytes.

`--timing` prints the elapsed seconds in fixed-point notation (for example `0.003620`) as the last line on stderr.

## Exit codes

| Code | Meaning | Examples |
| --- | --- | --- |
| 0 | success | |
| 1 | query error: syntax, bind, execution or memory error | `SELECT COUNT(*) FORM t`; an unknown table or column; `SUM` of a VARCHAR column; arithmetic on a `SUM` outside HUGEINT's range; an invalid `regexp_replace` pattern; a query that needs more memory than `--memory-limit` |
| 2 | usage error | unknown option; neither or both of `-c` and `-f`; a malformed `--table`, `--column-type` or `--memory-limit`; a column that `--column-type` cannot read as DATE; a table name registered twice |
| 3 | I/O error | a missing or unreadable file; not a Parquet file; schemas that differ; a glob that matches nothing |
| 4 | unsupported: valid-looking SQL outside the supported subset | `row_number() OVER ()`; `IS NULL`; an unknown function; `SELECT 2.5`; `SUM(DISTINCT ...)`; `CAST(a AS BIGINT)`; `SUM` of a DECIMAL column; a column of an unsupported type |
| 70 | internal error: anything else, which is a bug | an uncaught exception; an Arrow `NotImplemented` or type error without SQL context |

Exit code 4 is used only for errors that the parser, the binder or the physical planner marks as unsupported
(`SqlErrorDetail` kind `kUnsupported`). Any other `NotImplemented` or type error from Arrow maps to 70, so a
missing feature can never hide an internal bug ([ADR 0005](adr/0005-error-boundary.md)).

Errors go to stderr as `antb1: <kind> error: <message>`, followed by the line, column and a caret marker when the
error has a source position. With `--format json` the error is one JSON object on stderr instead (command-line syntax
errors such as an unknown option are always reported as plain text by CLI11):

```json
{"error":{"kind":"bind","message":"table 'nope' does not exist","offset":21,"length":4,"line":1,"column":22}}
```

The kinds are `parse`, `unsupported`, `bind`, `io`, `execution`, `memory`, `internal` and, for command-line errors (exit
code 2), `usage`. Strings in the error object (and in the `antb1 bench` report) are escaped like `--format json`
values, so it is valid UTF-8 whatever bytes the SQL holds.

## Divergences from DuckDB

Every intentional difference from DuckDB is registered here, with the way the tests handle it. The oracle tests
compare against DuckDB, so an unregistered difference is a bug.

| ID | Area | antb1 | DuckDB | How tests handle it |
| --- | --- | --- | --- | --- |
| D1 | Files with different schemas | a table (or a `FROM '<glob>'`) whose files have different Parquet schemas is an I/O error, exit code 3: `schema of '<file>' differs from '<first file>'` | `read_parquet` over the same files reads them and answers | an `onlyif antb1` record in `tests/slt/cases/basic/errors.slt`; `integration.ParquetErrors.*` (mismatched and mixed schemas); the CLI golden `io_schema_mismatch`; every table in `tests/slt/tables.txt` has a single schema |
| D2 | Column-type overrides | `--clickbench` and `--column-type COL=DATE` apply to every table with that column, including tables opened with `FROM '<path>'` | the oracle applies `make_date(EventDate)` only to the named tables with the `clickbench` option in `tests/slt/tables.txt`; `FROM '<path>'` reads the raw integers | the runner rejects a table list where some tables with an `EventDate` column have the option and others do not; the random generator never reads an overridden column through `FROM '<path>'`; `.slt` records read `EventDate` only through table names |
| D3 | Literal types | a string literal compared with a numeric column is a bind error | casts the string to the column's type | `onlyif antb1` records in `tests/slt/cases/basic/bind_errors.slt`; the query generator writes numbers for numeric columns |
| D4 | Literal types | a number or a `DATE` literal (also written as a cast) compared with a VARCHAR column is a bind error | casts the column's values at run time (a conversion error unless every value converts) | as D3, and in `tests/slt/cases/where/cast_date.slt`; the generator writes strings for VARCHAR columns |
| D5 | Date and timestamp literals | a date must be written exactly `YYYY-MM-DD` (also in a cast to DATE), a timestamp exactly as in the literal table (two-digit fields, one space or `T`, hours 00 to 23) | also accepts `2013-7-1`, surrounding spaces and a time of day for a DATE, and single-digit fields, spaces and `24:00:00` for a TIMESTAMP | as D4; the generator writes `YYYY-MM-DD` and `YYYY-MM-DD HH:MM:SS` |
| D7 | DOUBLE literals and BIGINT | a number that DuckDB types as DOUBLE (an exponent, or more than 38 digits) is rounded to the nearest double like in DuckDB, then compared exactly with the integer column; in an `IN` list with such a number every value is rounded so | converts BIGINT (and HUGEINT) values to DOUBLE for the comparison, so values beyond 2^53 compare rounded: `i64 >= 9223372036854775808e0` holds for `9223372036854775807` | the `.slt` records with such literals avoid BIGINT values beyond 2^53 (`tests/slt/cases/where/folding.slt`); `plan.ApproximateNumbers/FoldThroughBinderTest.*` pins antb1's folding; the generator writes no exponents |
| D8 | Result names | an aggregate's argument is quoted when it is not a plain identifier or is a reserved word | also quotes non-reserved keywords (`sum("year")`) | the tests compare values and types, not names |
| D9 | HUGEINT range | HUGEINT is decimal128(38, 0): a `SUM`, or arithmetic on a `SUM`, outside -(10^38 - 1) to 10^38 - 1 is an execution error (exit code 1). An integer SUM over BIGINT or smaller types cannot reach it | HUGEINT holds -(2^127 - 1) to 2^127 - 1 | no column is HUGEINT (a DECIMAL(38, 0) column is DECIMAL); `exec.AggregateStateTest.HugeIntSumIsCheckedAgainstTheRange` checks the error |
| D10 | NaN | MIN and MAX ignore NaN like Arrow's `min_max`, whatever the batch and file boundaries: they return NaN only when every selected non-NULL value is NaN (so only MAX over NaN and other values differs from DuckDB). Arrow's comparison kernels follow IEEE 754: NaN compares unequal to everything, so `d > 1` and `d >= 1` are false for NaN, and `d IN (...)` never matches it | orders NaN above every other value and equal to itself: MIN and MAX return NaN when it is the extreme, `d > 1` is true for NaN | the fixtures contain no NaN (fixturegen builds doubles from integer ratios); `exec.AggregateStateTest.MinMaxOfDoublesIgnoreNaNInEveryBatchSplit` and `engine.SessionTest.MinMaxIgnoreNaNAcrossBatchesAndFiles` pin antb1's MIN and MAX |
| D11 | FLOAT columns | read as DOUBLE (widened exactly): results of FLOAT columns are DOUBLE and print with double precision; `WHERE` compares like DuckDB (see Binding); arithmetic on them, and comparing them with other expressions, is unsupported (exit code 4) | keeps FLOAT (`MIN`, `MAX` and projections return FLOAT) | the random generator never references a FLOAT column (`ColumnOf` in `tests/slt/runner/query_gen.cc`, `harness.LoadGenTables.SkipsFloatColumns`); `tests/slt/cases/where/float.slt` selects only other columns, and `engine.SessionTest.FloatColumnsCompareLikeDuckDb` pins that results stay DOUBLE |
| D12 | Long numbers against DOUBLE | a number compared with a DOUBLE column is the correctly rounded nearest double | converts a DECIMAL literal (at most 38 digits) or a HUGEINT literal to DOUBLE in two steps when its digits exceed 2^53, which can be one ulp off (`9007199254740993.5`) | the generator only writes decimals of at most 2^53 in their digits with at most 22 decimals, where both round the same (`ExactDecimalDouble` in `tests/slt/runner/query_gen.cc`) |
| D13 | Decimals with many digits against integer and DECIMAL columns | compared exactly | compares in a DECIMAL whose width is capped at 38 digits: when the column type's digits plus the literal's decimals exceed 38, a column value with too many integer digits fails the query with a conversion error (`i16 = 1.0000000000000000000000000000000000001` over the value -32768); likewise an integer `SUM` (HUGEINT, 38 digits) in `HAVING` against any decimal fails once the sum has more digits than 38 minus the literal's decimals, and a DECIMAL(p,s) column against a literal with more than s decimals or more than p - s integer digits once their total exceeds 38 (`d38_10 > 0.00000000001` over a value of 28 integer digits) | the `.slt` records and the generator keep literals short enough (`DecimalText` in `tests/slt/runner/query_gen.cc`), and `tests/slt/cases/types/decimal.slt` pins both answers; `plan.Binder/FoldThroughBinderTest.*` and `plan.Binder/FoldDecimalTest.*` cover the exact folding |
| D14 | Overflows DuckDB's optimizer does not avoid | a comparison that folds to always-true or never-true at bind time (a literal outside the operand's type, as in `smallint_col + 1 > 40000`) computes nothing, so it cannot overflow | computes the operand and fails on an overflow ("Overflow in addition of INT16") | the random generator never writes arithmetic that can overflow; `plan.BinderTest.WhereMovesConstantsLikeDuckDb` pins which comparisons move their constants |
| D15 | VARCHAR bytes that are not UTF-8 | answers: `strlen` counts every byte, and `regexp_replace` runs RE2 over the bytes as UTF-8, where an invalid byte never matches (not even `.` or `[^a]`) and stays in the result | cannot read such a value as VARCHAR: reading an unannotated BYTE_ARRAY column (`binary_as_string`) with it fails the query ("Invalid string encoding") | every fixture string is valid UTF-8, so the oracle tests never meet it; `exec.ComputeTest.StringFunctions` pins antb1's behavior |
| D16 | Evaluation order in conditions | computes the arguments of `AND` and `OR` in the order written, each only for the rows still undecided, and a WHERE conjunct's operands for every row; so an overflow inside a condition fails exactly when a row reaches it in that order | may reorder conjunctions by its cost model, and computes the argument of a `NOT` for every row, so an overflow can fail in one engine and not the other (`NOT (x < 100 AND x * x > 0)` fails in DuckDB) | the random generator never writes arithmetic that can overflow; `exec.ComputeTest.ConditionsAreThreeValued` pins antb1's order |
| D17 | Overflows in constant expressions | a constant integer expression on a side of a comparison or a `BETWEEN` bound that overflows its type (`a > 2147483647 + 1`) is a bind error, whatever the table holds | raises the overflow only when a row computes it: an empty table, or one whose rows another conjunct rejects first, answers | `plan.Expressions/BindErrorTest` pins the errors; `tests/slt/cases/where/between.slt` has the error on a table both engines fail on and, `onlyif antb1`, on the empty table |

## ClickBench status

The target of the first slice was ClickBench Q0, Q1, Q2, Q3 and Q6, run with `--clickbench`; `GROUP BY`, `ORDER BY`
and `OFFSET` added the queries listed below them. Query numbers follow
ClickBench's DuckDB/Parquet query file at ClickBench commit `5a56398c975bfd9f328f544894bcb92533ed134c` (0-based); the
query text itself is never committed. The data test `data.clickbench.status` (`pixi run test-data`, CI job
`clickbench-hits0`) runs every query on antb1 over the first partition of the `hits` dataset, compares the answers
with DuckDB and fails when the passing queries differ from the ratchet `tests/data/clickbench_status.json`. `pass`
below means exactly the ratchet (`pixi run lint` compares them); the PR that changes the pass set updates both
([testing.md](testing.md#the-clickbench-ratchet)). Every other query must fail cleanly (exit code 4, or a parse or
bind error); today there is none: every query passes.

| Query | Status | Notes |
| --- | --- | --- |
| Q0 | pass | `COUNT(*)`: answered from the Parquet footer row counts |
| Q1 | pass | `COUNT(*)` under a `WHERE` comparison: the true count of the filter's selection |
| Q2 | pass | `SUM` (HUGEINT), `COUNT(*)` and `AVG` in one scan of the referenced columns |
| Q3 | pass | `AVG` of a BIGINT column: exact 128-bit sum, one division |
| Q4 | pass | `COUNT(DISTINCT)` of a BIGINT column over the whole table |
| Q5 | pass | `COUNT(DISTINCT)` of a VARCHAR column over the whole table |
| Q6 | pass | `MIN` and `MAX` of `EventDate` read as DATE (`--clickbench`) |
| Q7 | pass | `GROUP BY` one column under `WHERE`, ordered by the count descending, no `LIMIT` |
| Q8 | pass | `GROUP BY` one column with `COUNT(DISTINCT)`, ordered by it descending, top-N |
| Q9 | pass | `GROUP BY` one column with `SUM`, `COUNT(*)`, `AVG` and `COUNT(DISTINCT)`, ordered by the count, top-N |
| Q10 | pass | `GROUP BY` one column under `WHERE` with `COUNT(DISTINCT)`, ordered by it descending, top-N |
| Q11 | pass | `GROUP BY` two columns under `WHERE` with `COUNT(DISTINCT)`, ordered by it descending, top-N |
| Q12 | pass | `GROUP BY` one column under `WHERE`, ordered by the count descending, top-N |
| Q13 | pass | `GROUP BY` one column under `WHERE` with `COUNT(DISTINCT)` of another column, ordered by it descending, top-N |
| Q14 | pass | `GROUP BY` two columns under `WHERE`, ordered by the count descending, top-N |
| Q15 | pass | `GROUP BY` one column, ordered by the count descending, top-N |
| Q16 | pass | `GROUP BY` two columns, ordered by the count descending, top-N |
| Q17 | pass | `GROUP BY` two columns with `COUNT(*)` and `LIMIT` without `ORDER BY`: any groups are a right answer, compared as a subset of DuckDB's unlimited answer |
| Q18 | pass | `EXTRACT(minute FROM toDateTime(...))` grouped by its alias with two columns, ordered by the count, top-N |
| Q19 | pass | not a target: a projection under a `WHERE` comparison; fits the grammar and passes incidentally |
| Q20 | pass | `COUNT(*)` under `LIKE '%...%'` |
| Q21 | pass | `LIKE '%...%'` and a comparison, `GROUP BY` one column with `MIN` and `COUNT(*)`, ordered by the count, top-N |
| Q22 | pass | `LIKE` and `NOT LIKE` and a comparison, `GROUP BY` one column with `MIN`, `COUNT(*)` and `COUNT(DISTINCT)`, ordered by the count, top-N |
| Q23 | pass | `SELECT *` under `LIKE '%...%'`, ordered by a column, top-N |
| Q24 | pass | a projection under `WHERE`, ordered by a column it does not select, top-N |
| Q25 | pass | a projection under `WHERE`, ordered by the column it selects, top-N |
| Q26 | pass | a projection under `WHERE`, ordered by a column it does not select and then by the one it selects, top-N |
| Q27 | pass | `AVG(strlen(...))` grouped by a column under `WHERE`, with `HAVING` on the count, ordered by the average, top-N |
| Q28 | pass | `regexp_replace` with a group grouped by its alias, `AVG(strlen(...))`, `MIN`, `HAVING` on the count, ordered, top-N |
| Q29 | pass | 90 `SUM`s of a column plus a constant: DuckDB's sum rewriter, `SUM(x) + c * COUNT(x)` |
| Q30 | pass | `GROUP BY` two columns under `WHERE` with `COUNT(*)`, `SUM` and `AVG`, ordered by the count, top-N |
| Q31 | pass | `GROUP BY` two columns under `WHERE` with `COUNT(*)`, `SUM` and `AVG`, ordered by the count, top-N |
| Q32 | pass | `GROUP BY` two columns with `COUNT(*)`, `SUM` and `AVG`, no `WHERE`, ordered by the count, top-N: many groups tie at the cut |
| Q33 | pass | `GROUP BY` one column, ordered by the count descending, top-N |
| Q34 | pass | a constant, a column and `COUNT(*)`, `GROUP BY` the constant's position and the column, ordered by the count, top-N |
| Q35 | pass | a column and three expressions `column - constant`, all of them `GROUP BY` keys, and `COUNT(*)`, ordered by the count, top-N |
| Q36 | pass | `GROUP BY` one column under a `WHERE` conjunction, ordered by the alias of the count, top-N |
| Q37 | pass | `GROUP BY` one column under a `WHERE` conjunction, ordered by the alias of the count, top-N |
| Q38 | pass | `GROUP BY` one column under a `WHERE` conjunction, ordered by the alias of the count, a window with `OFFSET` |
| Q39 | pass | `CASE` over an `AND` of two comparisons, grouped by its alias with other columns, under a `WHERE` conjunction, ordered by the count, a window with `OFFSET` |
| Q40 | pass | a `WHERE` conjunction with `IN` over two values, `GROUP BY` two columns, ordered by the count, a window with `OFFSET` |
| Q41 | pass | `GROUP BY` two columns under a `WHERE` conjunction, ordered by the alias of the count, a window with `OFFSET` |
| Q42 | pass | `date_trunc('minute', toDateTime(...))` selected, grouped and ordered by (the same expression, a TIMESTAMP key) under a `WHERE` conjunction, a window with `OFFSET` |

## Queries derived from TPC-H

The second workload is the 22 queries derived from TPC-H, in DuckDB's dialect, as the pinned conda-forge package
`duckdb-extension-tpch` provides them. Their text, the data and the answers are generated at test time and never
committed ([ADR 0006](adr/0006-test-strategy-and-data-policy.md)); queries are referred to by their number (1-based).
The ratchet `tests/data/tpch_status.json` lists the queries verified to pass, and `pass` below means exactly the
ratchet (`pixi run lint` compares them); the PR that makes a query pass updates both. A query that does not pass must
fail cleanly with exit code 4 (unsupported). Today none passes; the plan is recorded in ADRs 0021 (DECIMAL), 0022
(joins, query blocks and uncorrelated subqueries) and 0023 (correlated subqueries). `tests/tpch/` generates the data
at test time ([testing.md](testing.md#data-derived-from-tpc-h)), and `tpch.status.sf0_01`, `tpch.status.sf0_1` and
`parallel.tpch.status.sf0_1` enforce the ratchet
([testing.md](testing.md#the-ratchet-of-the-queries-derived-from-tpc-h)). The last column names the work whose PR
completes the query; most queries also need the DECIMAL work of ADR 0021 before it.

| Query | Status | Completed by |
| --- | --- | --- |
| Q1 | unsupported (exit code 4) | DECIMAL arithmetic, SUM and AVG (ADR 0021) |
| Q2 | unsupported (exit code 4) | correlated scalar aggregates (ADR 0023) |
| Q3 | unsupported (exit code 4) | inner joins (ADR 0022) |
| Q4 | unsupported (exit code 4) | correlated EXISTS and NOT EXISTS (ADR 0023) |
| Q5 | unsupported (exit code 4) | inner joins (ADR 0022) |
| Q6 | unsupported (exit code 4) | DECIMAL arithmetic, SUM and AVG (ADR 0021) |
| Q7 | unsupported (exit code 4) | derived tables (ADR 0022) |
| Q8 | unsupported (exit code 4) | derived tables (ADR 0022) |
| Q9 | unsupported (exit code 4) | derived tables (ADR 0022) |
| Q10 | unsupported (exit code 4) | inner joins (ADR 0022) |
| Q11 | unsupported (exit code 4) | uncorrelated subqueries as joins (ADR 0022) |
| Q12 | unsupported (exit code 4) | inner joins (ADR 0022) |
| Q13 | unsupported (exit code 4) | LEFT JOIN (ADR 0022) |
| Q14 | unsupported (exit code 4) | inner joins (ADR 0022) |
| Q15 | unsupported (exit code 4) | uncorrelated subqueries as joins (ADR 0022) |
| Q16 | unsupported (exit code 4) | uncorrelated subqueries as joins (ADR 0022) |
| Q17 | unsupported (exit code 4) | correlated scalar aggregates (ADR 0023) |
| Q18 | unsupported (exit code 4) | uncorrelated subqueries as joins (ADR 0022) |
| Q19 | unsupported (exit code 4) | OR factoring for joins (ADR 0022) |
| Q20 | unsupported (exit code 4) | correlated scalar aggregates (ADR 0023) |
| Q21 | unsupported (exit code 4) | correlated EXISTS and NOT EXISTS (ADR 0023) |
| Q22 | unsupported (exit code 4) | correlated EXISTS and NOT EXISTS (ADR 0023) |

This workload is derived from the TPC-H Benchmark and is not comparable to published TPC-H Benchmark results, as this
implementation does not comply with all requirements of the TPC-H Benchmark.
