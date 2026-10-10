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
(`CAST('2013-07-01' AS DATE)`, `'2013-07-01'::DATE`), `LIMIT` and `OFFSET`, over one table of Parquet files or
several joined by inner joins. This covers all 43 ClickBench queries (see
[ClickBench status](#clickbench-status)). Of the 22 queries derived from TPC-H, Q1, Q3, Q5, Q6, Q10, Q12 and Q14
pass (see [Queries derived from TPC-H](#queries-derived-from-tpc-h)).

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
- SQL outside the grammar (a window function such as `row_number() OVER ()`, `IS NULL`, other functions, ...) fails
  with exit code 4 and points at the first unsupported token. A `FROM` list of tables and paths, joined by commas,
  `CROSS JOIN` or `[INNER] JOIN ... ON`, with aliases and qualified names, is answered; a `LEFT JOIN`, a derived
  table, a `WITH` list, more than 256 relations, and a join graph that no join key connects (a cross product) parse
  ([Grammar](#grammar)) but are not answered: exit code 4 as well. Malformed SQL (a syntax error) and SQL that is
  wrong for the tables (a bind error) fail with exit code 1.

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
query       = [ "WITH" , cte , { "," , cte } ] , block ;
cte         = identifier , [ columns ] , "AS" , "(" , query , ")" ;
block       = "SELECT" , select_list , "FROM" , from_list , [ "WHERE" , expr ] ,
              [ "GROUP" , "BY" , expr , { "," , expr } ] , [ "HAVING" , expr ] ,
              [ "ORDER" , "BY" , order_item , { "," , order_item } ] ,
              [ limit_offset ] ;
limit_offset = "LIMIT" , integer , [ "OFFSET" , integer ] | "OFFSET" , integer , [ "LIMIT" , integer ] ;
select_list = "*" | select_item , { "," , select_item } ;
select_item = expr , [ [ "AS" ] , identifier ] ;
order_item  = expr , [ "ASC" | "DESC" ] , [ "NULLS" , ( "FIRST" | "LAST" ) ] ;
from_list   = from_item , { "," , from_item
                          | "CROSS" , "JOIN" , from_item
                          | [ "INNER" ] , "JOIN" , from_item , "ON" , expr
                          | "LEFT" , [ "OUTER" ] , "JOIN" , from_item , "ON" , expr } ;
from_item   = ( identifier | string_literal ) , [ [ "AS" ] , identifier | "AS" , string_literal ]
            | "(" , query , ")" , [ ( [ "AS" ] , identifier | "AS" , string_literal ) , [ columns ] ] ;
columns     = "(" , identifier , { "," , identifier } , ")" ;
column_ref  = [ identifier , "." ] , identifier ;
expr        = expr , "OR" , expr | expr , "AND" , expr | "NOT" , expr | condition | sum ;
condition   = sum , cmp_op , sum
            | sum , [ "NOT" ] , "LIKE" , sum
            | sum , [ "NOT" ] , "IN" , "(" , expr , { "," , expr } , ")"
            | sum , [ "NOT" ] , "BETWEEN" , sum , "AND" , sum ;
sum         = sum , ( "+" | "-" ) , product | product ;
product     = product , ( "*" | "/" | "//" | "%" ) , unary | unary ;
unary       = "-" , unary | postfix ;
postfix     = primary , { "::" , type } ;
primary     = column_ref | literal | "(" , expr , ")" | agg_call
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
deep, counted along its deepest path through operators and parentheses (the top-level `AND` chain of `ON`, `WHERE` and
`HAVING` does not count, unless an `OR` makes it one tree), else it is unsupported. In a chain such as `a + b + c` each
operator pushes everything before it one level down, so `f(f(...)) + 1 + 1` counts the calls and the operators together.
A `::` and a unary `-` count one level more, as they do in the canonical form `CAST(x AS T)` and `-(x)` (a `-` before a
parenthesized operand does not), and so does a `NOT` that is the right operand of a comparison or of arithmetic, a
`LIKE` pattern or a `BETWEEN` bound (`a = NOT b` is `a = (NOT b)`). The canonical form writes an `ON`, `WHERE` or
`HAVING` predicate with a top-level `OR` without parentheses, so it reads back as it was parsed. Aggregates are allowed
in the select list, `HAVING` and `ORDER BY`, and cannot be nested; one in `ON`, `WHERE` or `GROUP BY` is a syntax error.

The FROM list is flat ([ADR 0022](adr/0022-joins-and-query-blocks.md)): each item after the first records how it joins
the items before it. A `JOIN` binds tighter than a comma and associates to the left, as in DuckDB, so `a, b JOIN c ON
...` joins `b` and `c` first and is not `a CROSS JOIN b JOIN c ON ...`. `JOIN` alone is an inner join; `CROSS JOIN` and
a comma take no `ON`, and the other joins need one (a syntax error otherwise, as in DuckDB). The `ON` condition is split
at its top-level `AND` chain like `WHERE`.

Nested queries: a derived table (a query in parentheses in FROM) and the query of a common table expression (a CTE of a
`WITH` list) are queries of their own. Each may start with a `WITH` list, which stands nowhere else, and its block
places aggregates by the rules above afresh; it has the grammar's known gaps too (see below). A nested query is one
level below the clauses around it, so it counts against the depth limit together with the expressions inside it: 256
derived tables, each in the FROM list of the next, parse, and so do 255 CTEs around `SELECT a FROM t`, whose select item
is the 256th level. A column alias list follows a derived table's alias (`FROM (...) AS s(x, y)`) or a CTE's name
(`WITH c(x, y) AS (...)`), and the parser takes it at any length. Within one `WITH` list the CTE names differ ASCII
case-insensitively, as in DuckDB: a repeated one (`c` and `"C"`, also `'c'`, but not `E'c'` or `$$c$$`, which are
unsupported first) is a syntax error at the repeated name, before its query is parsed, while a nested `WITH` list may
reuse a name. Until the binder answers them (roadmap PR J4, [ADR 0022](adr/0022-joins-and-query-blocks.md)), it rejects
a `WITH` list at its `WITH` and a derived table at its `(` with exit code 4, in query order (the `WITH` list first) and
before any table resolves.

Lexical rules: an `identifier` is a letter or `_` followed by letters, digits or `_`, or any text in double quotes
(`""` escapes a quote); a `string_literal` is text in single quotes (`''` escapes a quote); an `integer` is a
sequence of digits; a `decimal` is a number with a decimal point, an exponent or both (`1.5`, `.5`, `5.`, `1e3`).
Keywords are not reserved by the lexer. A `-` directly before a number makes a negative literal, except when `::`
follows the number: `-1::INTEGER` is `-(CAST(1 AS INTEGER))`, as in DuckDB. The `identifier` of a `type` and the field
of `EXTRACT` are unquoted and case-insensitive (`date` is `DATE`); a type's parameters are integers (`DECIMAL(15, 2)`).
`CAST(x AS T)` and `x::T` are the same expression.

- Reserved words: these 60 words are no unquoted column, table or alias names (an error, with the exceptions below);
  quoted they are names like any other (`"from"`): `ALL`, `AND`, `ANY`, `ARRAY`, `AS`, `ASC`, `BETWEEN`, `BY`,
  `CASE`, `CAST`, `COLLATE`, `CROSS`, `DESC`, `DISTINCT`, `ELSE`, `END`, `EXCEPT`, `EXISTS`, `FALSE`, `FETCH`, `FOR`,
  `FROM`, `FULL`, `GROUP`, `HAVING`, `ILIKE`, `IN`, `INNER`, `INTERSECT`, `INTERVAL`, `INTO`, `IS`, `JOIN`, `LATERAL`,
  `LEFT`, `LIKE`, `LIMIT`, `NATURAL`, `NOT`, `NULL`, `OFFSET`, `ON`, `OR`, `ORDER`, `OUTER`, `OVER`, `QUALIFY`,
  `RIGHT`, `SELECT`, `SIMILAR`, `SOME`, `TABLE`, `THEN`, `TRUE`, `UNION`, `USING`, `WHEN`, `WHERE`, `WINDOW` and
  `WITH`. A select alias that is a reserved word is unsupported (DuckDB accepts any word after `AS` there).
- Qualified names: a `column_ref` has at most one qualifier (`t.x`), and comments and spaces around the dot are
  allowed. Either part may be quoted (`"T"."x y"`), and a reserved word on either side must be: `t.from` and the
  qualifiers `between`, `exists`, `interval` and `over`, which DuckDB accepts, are unsupported, and any other reserved
  word before the dot is an error, as in DuckDB, unless the parser takes the word there for the start of a construct
  that it does not support and reports that (exit code 4, where DuckDB gives a syntax error): `ALL`, `ANY`, `ARRAY`,
  `DISTINCT`, `FALSE`, `NULL`, `SOME` and `TRUE` (`null.a`: NULL literals are not supported; `SELECT DISTINCT.a`).
  `t.*`, `a.b.c`, `t.f()` and a qualified name before a string (`main.integer '5'`, which DuckDB reads as a typed
  literal of a qualified type) are unsupported.
- Typed literals: as in DuckDB, a string after a type name makes a typed literal, whether the string is plain, an escape
  or a dollar-quoted one, and the name plain, quoted or qualified (`integer '5'`, `integer E'5'`, `DATE $$2020-01-01$$`,
  `"integer" '5'`, `main.integer '5'`). Only `DATE` and `TIMESTAMP` before a plain string are in the grammar: every
  other typed literal is unsupported, in every clause, `LIMIT` and `OFFSET` included, and so is an escape or a
  dollar-quoted string on its own (`E'\n'`, `$$x$$`). Every unreserved word before a string is taken for a type name,
  also one that DuckDB does not take for one (`coalesce '5'`: exit code 4, where DuckDB gives a syntax error). `E` and a
  string with a space between them are no escape string (`integer E '5'` is a syntax error, as in DuckDB). After the dot
  of a two-part column name and in `LIMIT` and `OFFSET`, `B'1'`, `E'x'` and `X'1F'` are one string constant each, as
  DuckDB lexes them, and no name before a string: `t.E'x'` and `LIMIT main.E'5'` are syntax errors, as in DuckDB.
  Elsewhere an earlier rule can report such a form as unsupported (exit code 4, where DuckDB gives a syntax error), for
  example in a call's arguments (`abs(t.E'x')`), after a name of more than two parts (`a.b.E'x'`), in a cast
  (`a::main.E'x'`) and in FROM (`main.E'x'`).
- Table aliases: after `AS` a name, a quoted identifier or a non-empty string literal (`AS 'a'`, as DuckDB); without
  `AS` a name or a quoted identifier. As in DuckDB, `BETWEEN`, `EXISTS`, `INTERVAL` and `OVER` are table aliases with
  or without `AS`, although they are reserved elsewhere, and these 49 words, which antb1 does not reserve, never are:
  `ANALYSE`, `ANALYZE`, `ANTI`, `ASOF`, `ASYMMETRIC`, `AT`, `AUTHORIZATION`, `BINARY`, `BOTH`, `CHECK`, `COLLATION`,
  `COLUMN`, `CONCURRENTLY`, `CONSTRAINT`, `CREATE`, `DEFAULT`, `DEFERRABLE`, `DESCRIBE`, `DO`, `FOREIGN`, `FREEZE`,
  `GLOB`, `INITIALLY`, `ISNULL`, `LAMBDA`, `LEADING`, `NOTNULL`, `ONLY`, `OVERLAPS`, `PIVOT`, `PIVOT_LONGER`,
  `PIVOT_WIDER`, `PLACING`, `POSITIONAL`, `PRIMARY`, `REFERENCES`, `RETURNING`, `SEMI`, `SHOW`, `SUMMARIZE`,
  `SYMMETRIC`, `TABLESAMPLE`, `TO`, `TRAILING`, `UNIQUE`, `UNPACK`, `UNPIVOT`, `VARIADIC` and `VERBOSE`. After a FROM
  item (a table, a path, a derived table, an alias or a column alias list) such a word is unsupported where DuckDB
  gives it a meaning: `SEMI`, `ANTI` or `POSITIONAL` before `JOIN`, `ASOF` before a join, `AT (` (time travel, after a
  table or a path only), `PIVOT (`, `UNPIVOT` before `(`, `INCLUDE` or `EXCLUDE`, and `TABLESAMPLE` before a number,
  `(` or a name and `(`; after an `ON` condition the joins, `PIVOT` and `UNPIVOT`, and the operators `GLOB`,
  `AT TIME ZONE`, `ISNULL` and `NOTNULL`. Anywhere else after a FROM item it is a syntax error, as in DuckDB
  (`FROM t semi`: a table alias cannot be the keyword SEMI). Read as an alias, `SEMI` and `ANTI` would turn DuckDB's
  semi and anti joins into inner joins. Quoted (`FROM t "semi"`) every word is an alias, and elsewhere the 49 words are
  names (divergence D21).
- CTE names and column aliases follow the rules of a table alias without `AS`: a name, a quoted identifier, `BETWEEN`,
  `EXISTS`, `INTERVAL` or `OVER`, never one of the 49 words (a syntax error, as in DuckDB). DuckDB also takes a string
  literal there, which is unsupported. `RECURSIVE` right after `WITH` is unsupported (`WITH RECURSIVE`), unless `AS`,
  `(` or `USING` follows it: then it names the first CTE, as in DuckDB. A CTE named `BETWEEN`, `EXISTS`, `INTERVAL` or
  `OVER` is read quoted only (`FROM "over"`): unquoted, these words are no table names (divergence D21), so
  `WITH over AS (...) SELECT a FROM over` is a syntax error, which DuckDB answers.
- The canonical form (`sql::ToSql`, [ADR 0008](adr/0008-parser-and-unparser.md)) writes every alias quoted after `AS`
  (`FROM t AS "a"`), `JOIN` as `INNER JOIN`, `LEFT OUTER JOIN` as `LEFT JOIN`, a qualifier as written, a nested query
  in parentheses, and CTE names and column aliases quoted (`WITH "c"("x") AS (...)`, `FROM (...) AS "s"("x")`).

**What the binder answers today.** Of the grammar above, antb1 answers:

- FROM: tables and paths, with an alias or without, joined by commas, `CROSS JOIN` or `[INNER] JOIN ... ON`, with
  qualified column names (`t.x`) anywhere in the query. A `LEFT JOIN`, a derived table and a `WITH` list parse and
  are rejected with exit code 4, in query order and before any table resolves, so also over tables that do not
  exist (ADR 0022 plans their answers), except inside the arguments of a call with the wrong number of arguments,
  which stays a bind error (`strlen(t.s, 1)`: strlen() takes 1 argument, not 2; over a table that does not exist,
  the missing table is the error). More than 256 relations is exit code 4 too, before the per-item checks;
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
  `HUGEINT` value (a column, a literal or a folded constant of that type), a `FLOAT` column, a decimal literal and
  an integer value, or a DECIMAL value that antb1 would compare exactly (a DECIMAL operand with a bound that is not
  `DOUBLE`, or a DECIMAL column or expression as a bound: `price BETWEEN 1e0 AND 5`) is unsupported; one that mixes
  a `VARCHAR` value with a `DATE` or `TIMESTAMP` value is a bind error, as in DuckDB. As in DuckDB, every unary minus
  directly over an integer literal (through any nesting) belongs to the literal, so `-(-9223372036854775808)` is a
  `HUGEINT`, not an overflow, and `-(-(-9223372036854775808))` a `BIGINT`. A plain `BETWEEN` in the `AND` chain of
  `WHERE` or `HAVING` is its two comparisons, each folded and, over a table column, pushed into the scan like any
  comparison; `NOT BETWEEN`, `NOT (a BETWEEN ...)` and `BETWEEN` under `OR` or in `CASE` are compound conditions.
  DuckDB's names apply: `(a BETWEEN 1 AND 2)`, and both negations `(NOT (a BETWEEN 1 AND 2))`;
- any of these in parentheses (`(a)`, `SUM((a))`, `WHERE (a = 1 AND b = 2)`), which group without changing
  anything.

Every other expression (function calls other than the five aggregates and the functions above, `EXTRACT` of other
fields, a condition used as a value, as in `SELECT a = 1`, a comparison of two constants, a bare column as a
condition, `TRY_CAST` and every other cast, such as `CAST(a AS BIGINT)` or `CAST(d AS DATE)`) parses, and is then
rejected by the binder with exit code 4 at its first unsupported token, before any name is resolved. `GROUP BY ALL`,
the empty grouping set `GROUP BY ()` and `ORDER BY ALL` are rejected by the parser, and so are `SUM`, `AVG`, `MIN` and
`MAX` with `DISTINCT`, and a string or quoted field name in `EXTRACT` (`EXTRACT('year' FROM d)`), which DuckDB accepts.

Outside the grammar, the parser recognizes common SQL and rejects it with exit code 4 and a source span, among others:
`SELECT DISTINCT`, subqueries in expressions, `ILIKE`, `GLOB`, `LIKE ... ESCAPE`, `NULL` literals, `IS [NOT] NULL`,
`AT TIME ZONE`, `||`, window functions, unary `+`, and in casts quoted or qualified type names, type names of several
words (`DOUBLE PRECISION`, `TIMESTAMP WITH TIME ZONE`), array types, `INTERVAL` and `UNION` types and type parameters
other than integers. After `LIMIT` and `OFFSET`: expressions such as `LIMIT 1 + 1`, `LIMIT '5'` and `LIMIT (5)`, calls and
typed literals, also of quoted and qualified names of at most three parts (`LIMIT abs(5)`, `LIMIT main.abs(5)`,
`LIMIT integer '5'`, `LIMIT E'5'`), a percentage, `LIMIT ALL` and `ROW` or `ROWS` after the value of `OFFSET`
(`OFFSET 5 ROWS`, also next to a `LIMIT`, which DuckDB answers; `LIMIT 5 ROWS` is a syntax error, as in DuckDB); a
column there (`LIMIT a`, `LIMIT t.a`) and a call or a typed literal of a longer name (`LIMIT a.b.c.d(1)`) are syntax
errors, and DuckDB refuses them too. Known gaps, among others:
`LIMIT` and `OFFSET` expressions that start with `CASE`, `NOT` or a unary minus (`LIMIT -(-5)`) and conditions
(`LIMIT 5 = 5`, `LIMIT 5 AND 3`) are syntax errors (exit code 1), although DuckDB answers them, and so are a trailing
comma in an `IN` list (`a IN (1,)`), `IN` before a list or a list column (`a IN [1, 2]`, `a IN b`), `BETWEEN ASYMMETRIC`
(`asymmetric` is a name, divergence D21), `MAP {...}`, a prefix alias (`SELECT x: 1`, `FROM x: t`), an exponent without
digits (`1e`, which DuckDB reads as `1 AS e`), a number that a name follows directly (`1x`, which DuckDB reads as
`1 AS x`), named arguments (`round(x := 2.5)`), a slice without its lower bound (`b[:2]`; `b[1:2]` is unsupported), a
`$` inside a name (`a$b`, which antb1 reads as a parameter), a one-parameter lambda (`lambda x: x + 1`) and a string
that continues after a line break (`'a'` and `'b'` on the next line, which DuckDB reads as `'ab'`), also in a nested
query; inside the arguments of a call some of them are unsupported (exit code 4) instead. In FROM:
`JOIN ... USING`, `NATURAL`, `RIGHT` and `FULL` joins, `SEMI`, `ANTI`, `ASOF` and `POSITIONAL`
joins, nested joins (a `JOIN` before the `ON` of an earlier one) and joins in parentheses, `LATERAL` before a subquery
or a table function (also one with a qualified name: `LATERAL main.range(3)`), `schema.table`, `ONLY`, table functions
(also those named by the 13 reserved words that DuckDB takes as function names, `CROSS`, `FULL`, `ILIKE`, `INNER`,
`IS`, `JOIN`, `LEFT`, `LIKE`, `NATURAL`, `OUTER`, `OVER`, `RIGHT` and `SIMILAR`: `FROM t, left(1)`), the reserved
words `between`, `exists`, `interval` and `over` as qualifiers of a table or a table function, which DuckDB accepts
(`FROM t, over.x`, `FROM over.f(1)`, `LATERAL over.f(1)`), column alias lists (`t AS a(x, y)`), `PIVOT`, `UNPIVOT`,
`AT (...)` (time travel), `TABLESAMPLE`, a trailing comma, and an empty, escape or dollar-quoted string as a table
alias (`AS ''`, `AS E'x'`, `AS $$x$$`, which DuckDB accepts). Malformed SQL, such
as `SELECT COUNT(*) FORM t`, is a syntax error with exit code 1, and so is a join keyword where DuckDB has none
(`SELECT 1 JOIN u`, a `JOIN` after `WHERE`) or without the rest of its join (`LEFT u`, `NATURAL LEFT u`, also before
the `ON` of a `JOIN`), `LATERAL` before a table or a path (`FROM t, LATERAL u`), a trailing comma of the FROM list
before `FROM` or `INTO` (`FROM t, FROM u`), and a table or a path in parentheses that `)`, `,`, `;` or the end
follows. An unsupported construct is reported at its first token, and the parser looks at most three tokens ahead, so
a malformed FROM form that starts like an unsupported one is unsupported too (exit code 4, where DuckDB gives a syntax
error), such as a table in parentheses that anything else follows (`FROM (t a)`, `FROM (f(1))`, and a reserved
qualifier: `FROM (over.x)`), `LATERAL` in parentheses before `(`, a name or a reserved word that DuckDB takes as a
function name or a qualifier (`FROM (LATERAL t)`, `FROM (LATERAL between)`), `LATERAL` before a qualified name
without a call (`LATERAL s.t`, `LATERAL over.x`) or `NATURAL LEFT OUTER u`.

In `WITH` lists and nested queries, these are unsupported (exit code 4): `WITH RECURSIVE`, `MATERIALIZED`,
`NOT MATERIALIZED` and `USING KEY`; a string literal as a CTE name or a column alias (`WITH 'c' AS`, `s('x')`) and a
trailing comma in a column alias list (`s(x,)`), which DuckDB accepts; a query in parentheses (in FROM or as a CTE's
query) that starts with no `SELECT` or `WITH` (`(VALUES (1))`, `(FROM t)`, `(TABLE t)`, `(PIVOT ...)` and
`(UNPIVOT ...)`, also written `PIVOT_WIDER` and `PIVOT_LONGER`, in FROM also `(DESCRIBE t)`, `(SHOW t)` and
`(SUMMARIZE t)`, or `((SELECT ...))`, at its first token), also after a `WITH` list (`WITH c AS (...) FROM c`); a
statement other than a query after a `WITH` list, which DuckDB runs (`WITH c AS (...) INSERT ...`, also `UPDATE`,
`DELETE` and `MERGE`); and in
a nested query `SELECT` without `FROM` (`(SELECT 1)`, which DuckDB answers), a trailing comma in its select list, FROM
list or `GROUP BY`, and the clauses that are unsupported after a statement (`UNION`, `FETCH`, ...). As elsewhere, a
construct is reported at its first token, so some of these are unsupported where DuckDB gives a syntax error, among
others: a trailing comma in a nested `ORDER BY` (`(SELECT ... ORDER BY a,)`); a nested query that `;` or the end of the
input cuts short (`(SELECT 1;`, `FROM (SELECT 1`, `FROM (SELECT a FROM t,`); `MATERIALIZED` without `(`; `RECURSIVE`
that no CTE follows (`WITH RECURSIVE;`, `WITH recursive, c AS (...)`); a word that starts a query, alone in parentheses
or before something else there (`FROM (pivot)`, `FROM (pivot_wider JOIN u ON ...)`, `WITH c AS (values) ...`);
`(DESCRIBE t)`, `(SHOW t)` and `(SUMMARIZE t)` as a CTE's query, which DuckDB takes in FROM only; another statement as
a CTE's query (`WITH c AS (DROP TABLE t) ...`) or one that DuckDB refuses after a `WITH` list
(`WITH c AS (...) DESCRIBE c`); what DuckDB refuses after `OFFSET n ROWS` (`OFFSET 1 ROWS ONLY`); a string CTE name
that a later CTE of its list repeats; and an escape or dollar-quoted CTE name that repeats an earlier one
(`WITH c AS (...), E'c' AS (...)`). Syntax errors (exit code 1), as in DuckDB: a
`WITH` list without its query (`WITH c AS (...)`, also before `;` or `)`), a CTE without `AS` or its parentheses, a
trailing comma in a `WITH` list, a second `WITH` list, an empty column alias list or one with a comma alone, a column
alias list after a derived table without an alias (`FROM (...) (x)`) or after a second one, a repeated CTE name,
`AT (...)` after a derived table, and `FROM (values)` (`values` is a name there: `FROM (values JOIN u ON ...)` is a join
in parentheses).

## Binding

The binder (`plan::Bind`) resolves the statement against the table and builds the logical plan. A bind error has exit
code 1 and points at the offending name, call or literal; the first error in query order wins (the tables, then the
select list, the `ON` conditions and `WHERE`, the `GROUP BY` names, the grouping rule below, `LIMIT`, `OFFSET`,
`HAVING` and the `ORDER BY` items).

- Names: table and column names match ASCII case-insensitively, quoted identifiers included (as in DuckDB). An
  unknown table or column is a bind error, and so is a name that matches two columns differing only in case. A
  column of an unsupported type fails with exit code 4 wherever it is referenced (by `SELECT *` too).
  Each FROM item is one binding, named by its alias, else a table's name as written, else the file name of its path
  up to the first dot (a path with a glob character is named by its whole text). An alias hides that name, and the
  error for a qualifier nothing is named by says so. Two bindings may share a name: a qualified name then resolves
  to the one that has the column, and is a bind error when both do. A qualifier nothing is named by, and a binding
  without the column, are bind errors. An `ON` resolves a name in its own join group first (the items from the last
  comma up to its `JOIN`; a `CROSS JOIN` continues a group), then in the earlier comma siblings, and never in a
  later item, whose name its error points out; `WHERE` and every other clause see every binding in one level. A
  qualified name never names a select item's alias, as in DuckDB, so `ORDER BY t.x` over `SELECT i16 AS x` is a bind
  error.
- Select list: `*` alone, or plain columns, aggregates and constants. `*` expands to every binding's columns, in
  FROM order then column order, and is a bind error when two bindings of one name share a column name. Without
  `GROUP BY`, aggregates (in the select list, in `HAVING` or in `ORDER BY`) and `HAVING` itself cannot be mixed with
  plain columns: a bind error at the first plain column (or at `*`). Constants mix with anything; with an aggregate
  (also one only in `HAVING` or `ORDER BY`) or `HAVING` the query has one row.
- Constants (as DuckDB types and names them): an integer is INTEGER when its magnitude fits (so `-2147483648` is
  BIGINT), else BIGINT or HUGEINT, and is named by its value (`007` is `7`); a string is VARCHAR named with its
  quotes (`'it''s'`); `DATE '2020-01-02'` is DATE named `CAST('2020-01-02' AS "DATE")` (so are
  `CAST('2020-01-02' AS DATE)` and `'2020-01-02'::DATE`), and
  `TIMESTAMP '2020-01-02 10:00:00'` TIMESTAMP named `CAST('2020-01-02 10:00:00' AS TIMESTAMP)`; a decimal is
  DECIMAL(digits, digits after the point), leading zeros included and the sign not, named by its value (`2.5` is
  DECIMAL(2,1), `.125` DECIMAL(3,3), `007.50` DECIMAL(5,2) named `7.50`; [ADR 0021](adr/0021-decimal-semantics.md)
  rule 3). A number DuckDB types as DOUBLE (an exponent, or more than 38 digits) and an integer beyond HUGEINT's 38
  digits are unsupported (exit code 4).
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
  reserved word (`sum("from")`). A qualifier inside an expression's name stays as written, each part quoted by that
  same rule (`sum(t.i16)`, `sum(t."Mixed Case")`), while a plain column select item keeps its declared name alone
  (`SELECT t.i16` is `i16`); a message that echoes a reference back prints it as written and unquoted, so a grouping
  error over `t.label` names `t.label`. Constants: see above. An alias replaces the name.
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
  operand's type: `COUNT` is BIGINT, an integer `SUM` HUGEINT, a DECIMAL(p,s) `SUM` DECIMAL(38,s), `AVG` and a
  DOUBLE `SUM` DOUBLE, `MIN` and `MAX` their column's type (and FLOAT for a FLOAT column, as in DuckDB, while its
  `SUM` and `AVG` are DOUBLE).
- Arithmetic (as DuckDB types it): an integer literal operand that fits the other operand's integer type takes that
  type (`smallint_col + 1` is SMALLINT, `smallint_col + 40000` INTEGER); two integer types give the wider one, where
  USMALLINT with SMALLINT gives BIGINT; an integer literal alone is INTEGER, BIGINT or HUGEINT by its value; DOUBLE
  with anything is DOUBLE, and so is a number DuckDB types as DOUBLE (`1e3`); a decimal literal is a DECIMAL (below);
  `/` is always DOUBLE; unary `-` keeps the type. DATE arithmetic, negating a USMALLINT (DuckDB wraps it), `//` and
  `%` of HUGEINT values and arithmetic on FLOAT columns (divergence D11) are unsupported; arithmetic on VARCHAR is a
  bind error. The result name is DuckDB's: `(a + 1)`, `-(a)`, `sum((a + 1))`, with columns as written and decimal
  literals by their value (`(a + 7.50)`).
- DECIMAL arithmetic ([ADR 0021](adr/0021-decimal-semantics.md) rules 4 to 6, 8 and 9): `+`, `-`, `*` and `%` of a
  DECIMAL (a column, an expression or a decimal literal) with a DECIMAL or an integer, which counts as DECIMAL(5,0)
  (SMALLINT, USMALLINT), DECIMAL(10,0) (INTEGER, and an integer literal that fits it: `7` does not shrink),
  DECIMAL(19,0) (BIGINT) or DECIMAL(38,0) (HUGEINT). `+` and `-` keep the larger scale s and give
  max(p1 - s1, p2 - s2) + s + 1 digits; `*` adds the scales and the widths. A width beyond 18 from two operands of at
  most 18 digits is 18 (for `*`, unless the scale reaches 18), and beyond 38 it is 38: DECIMAL(15,2) `+`
  DECIMAL(15,2) is DECIMAL(16,2), `*` is DECIMAL(18,4), DECIMAL(15,2) `*` BIGINT is DECIMAL(34,2), DECIMAL(38,10)
  `*` DECIMAL(38,10) is DECIMAL(38,20), `int_col + 1.5` is DECIMAL(12,1) and `1.5 * 2.25` DECIMAL(5,3), and a scale
  beyond 38 is a bind error with DuckDB's message (`Needed scale 40 to accurately represent the multiplication
  result, ...`). `%` keeps the larger scale and gives max(p1 - s1, p2 - s2) + s digits, with no cap to 18
  (DECIMAL(15,2) `%` INTEGER is DECIMAL(15,2), `int_col % 2.5` DECIMAL(11,1)). `/` and `//` of a DECIMAL, and any
  DECIMAL with a DOUBLE, are DOUBLE. Unary `-` keeps the type. `%` beyond 38 digits is DOUBLE, as in DuckDB
  (DECIMAL(38,0) `%` `0.5`): `fmod` of the operands converted to DOUBLE (Semantics, DECIMAL), with the dividend's
  sign (`-0` for a negative multiple) and NULL for a zero divisor.
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
- `CASE` (as DuckDB types it): the values (`THEN` and `ELSE`) take their common type, where an integer literal takes the
  other values' integer type when it fits (`CASE WHEN .. THEN smallint_col ELSE 0 END` is SMALLINT), two integer types
  give the wider one (USMALLINT with SMALLINT: INTEGER, unlike arithmetic), DOUBLE with any number DOUBLE, and a string
  literal takes VARCHAR or DATE (`ELSE '2013-07-15'` next to a DATE); without other values literals give their own
  types, and no value at all but string literals VARCHAR. A VARCHAR or DATE value with a number is a bind error (`cannot
  mix values of type VARCHAR and INTEGER in CASE`); a string literal next to numbers is unsupported (DuckDB casts it to
  the number); for some orders of literals antb1's type differs from DuckDB's (divergence D20). With a DECIMAL value (a
  decimal literal too) the type is DuckDB's fold of [ADR 0021](adr/0021-decimal-semantics.md) rule 10, so the order of
  the values can change it: it starts from the `ELSE` value's type (NULL without an `ELSE`) and takes each `THEN` value
  in written order. Two DECIMALs give the larger scale s and the most integer digits e, DECIMAL(e + s, s), or
  DECIMAL(38, 38 - e) beyond 38 digits; a DECIMAL and an integer keep the DECIMAL's scale and widen to the integer's
  digits (SMALLINT and USMALLINT 5, INTEGER 10, BIGINT 19, HUGEINT 38), up to 38; an integer literal (negated too,
  `-(7)`, not `7 + 0`) takes an integer type it fits, becomes its own type (INTEGER) next to NULL (the first `THEN`
  value without an `ELSE`) or another literal, and counts as its own type next to a DECIMAL, so with a DECIMAL(5,3)
  column `rate` and a SMALLINT column `s16`, `CASE WHEN .. THEN 7 WHEN .. THEN rate ELSE s16 END` is DECIMAL(8,3) and
  `CASE WHEN .. THEN rate WHEN .. THEN 7 ELSE s16 END` DECIMAL(13,3); a DOUBLE value makes the CASE DOUBLE, the DECIMALs
  converted as DuckDB converts them (rule 8). Each value is cast to the result on the rows that take it, as DuckDB
  casts: rounded half away from zero where the scale shrinks, and an integer that does not fit fails the query with
  DuckDB's conversion error (`Could not cast value 123456789 to DECIMAL(38,30) when casting from source column b`, an
  execution error). A string literal is VARCHAR next to NULL or another literal, so a later DECIMAL is a bind error
  (`cannot mix values of type VARCHAR and DECIMAL(15,2) in CASE`), as is a DATE, TIMESTAMP or VARCHAR value with a
  DECIMAL; otherwise (an `ELSE` literal too) it takes the type folded next, and a string literal that a DECIMAL type
  takes is unsupported (DuckDB casts it), as is a FLOAT column as a value (divergence D11). `CASE x WHEN v THEN ..` is
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
  and DATE with DATE; any other pair is a bind error (a number or DECIMAL against a VARCHAR, DATE or TIMESTAMP
  operand: divergence D4). A DECIMAL compares with a DECIMAL of any precision and scale and with an integer exactly
  by value, and with a DOUBLE in DOUBLE, the DECIMAL converted as DuckDB converts it ([ADR
  0021](adr/0021-decimal-semantics.md) rule 11; DuckDB compares in a DECIMAL capped at 38 digits instead, divergence
  D13). A comparison of an operand with a literal folds the literal into the operand's type, as for a column.
- `LIMIT n` and `OFFSET m` take integers from 0 to 9223372036854775807, in either order.

Literals in `WHERE` and `HAVING` must fit the column's (or the aggregate's) type; any other combination is a bind
error that points at the literal:

| Column type | Literals | Compared as |
| --- | --- | --- |
| SMALLINT, INTEGER, BIGINT, USMALLINT, HUGEINT | integer, decimal | exactly, after folding (below) |
| DECIMAL(p,s) | integer, decimal | exactly, after folding into the column's scale (below); a number DuckDB types as DOUBLE (an exponent, a decimal of more than 38 digits, an integer outside -2^127 to 2^128 - 1), and in an `IN` list with one every number, in DOUBLE: the literal as DuckDB converts it to DOUBLE, the column as DuckDB converts a DECIMAL (Semantics, DECIMAL) |
| DOUBLE | integer, decimal | the nearest double, as in DuckDB for a DOUBLE column, except a decimal of at most 38 digits, which DuckDB types as DECIMAL and converts with its own two-step cast (Semantics, DECIMAL); beyond the double range `inf` or `-inf`, below the smallest subnormal `0`. A column stored as FLOAT is compared as DuckDB compares it: an integer or DECIMAL literal becomes the FLOAT that DuckDB casts it to, with DuckDB's rounding (`0.1` is `0.1F`; `16777217.5` and some long spellings of `0.1`, such as 16 or 24 decimals, are not the nearest FLOAT, and HUGEINT literals are rounded through a double), beyond the FLOAT range `inf` or `-inf`; a number DuckDB types as DOUBLE compares with the nearest double |
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
  and `c < 10000000000000` is true for every value, as is `c < 100000000000000000000000000000000000000`. DuckDB
  compares in a DECIMAL(38,s) instead, capped at 38 digits, where an integer of more than 38 - s digits, as this one,
  fails to cast (divergence D13).
- A number DuckDB types as DOUBLE (an exponent, a decimal of more than 38 digits, an integer outside -2^127 to
  2^128 - 1) is not folded into a DECIMAL column: DuckDB compares them in DOUBLE, so does antb1, with the column
  converted as DuckDB converts a DECIMAL (`c = 1e-1` holds for 0.10, and a DECIMAL(38,10) value of
  9007199254740993.5 equals `9007199254740992e0`). An `IN` list with such a number compares every value in DOUBLE.

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
builds on. A query produces inner `Join`s: the binder sends each conjunct of `WHERE` and of an `ON` to the relations
it reads, so one relation's conjuncts filter that relation's own branch, a cross-relation equality whose two sides
share one key type is a key of their join (each side cast to that type on its own branch, an exact widening, and an
equality spelled twice is one key), and anything else over two or more relations is the residual of the first join
that has all of them. A join graph no key connects is a cross product: exit code 4, naming the item nothing connects.
The relations then join left-deep, greedily, from footer row counts and distinct-count hints only: the relation with
the most rows (an unknown count counts as the most) is probed first, then the connected relation with the smallest
estimate joins next, with every edge to the relations already joined as a key, so the edge that closes a cycle
becomes a second key. Every join builds on the relation it adds. Because the order reads metadata alone, a plan does
not depend on the thread count. The executor runs every kind as a hash join: it builds a hash table of the
input it builds on before it reads the other input, which then streams through it in its rows' order.

- An inner join keeps each row's matches in the build input's order; its residual conditions are evaluated in order,
  each on the rows the ones before it kept.
- A semi join keeps each row with a match, once; an anti join each row without one (a NULL key never matches); a
  null-aware anti join (SQL's `NOT IN`) each row without one whose key is not NULL, every row when its build input is
  empty, and none when its build input has a NULL key, though it still reads the other input to its end.
- A left join keeps each row with each of its matches, in the build input's order, and pads a row without one once
  with NULLs of the right input's types (a NULL key included).
- The residual conditions of a semi, anti or left join decide which candidates (the build rows with a row's key) are
  matches. They are evaluated in the order written, each on the candidates the ones before it passed (NULL counts as
  false), on every candidate of every row, without stopping at a row's first match, so whether an overflow fails the
  query does not depend on the batch size (unless a `LIMIT` above stops the join before it reads every row). A row
  without candidates never meets them: an anti join keeps it, a left join pads it. A left join pads a row none of
  whose candidates passes right after its last candidate.
- A one-row join appends the single row of its right input, an aggregate without groups, to every row; any other row
  count is a planner bug, reported as an error (exit code 1).
- When an inner or a semi join's build holds no row, the other input is never read.

A rule optimizer then rewrites the plan, through both inputs of every `Join`:

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
type: against a DECIMAL column with the column's scale (`p <= 12.34`, `d IN (1.25, 5.00)`); a DECIMAL column compared
in DOUBLE shows its conversion (`CAST(p AS DOUBLE) > 10`):

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
- A hash join (any `Join` the executor runs) shows two lines, each with the join's EXPLAIN text:
  - `HashJoin`, its probe, where the input it does not build on is read: in that input's part pipeline (per part),
    or over it when it is no part pipeline;
  - `HashBuild`, its build, under the operator that prepares it: the operator that runs the probe's part pipeline
    (after the pipeline's lines), or a `HashJoin` over its input (after that input's line). Its input follows it,
    per part when it is a part pipeline. Its `rows` and `batches` are the rows its table holds (those without a NULL
    key) and the table's chunks, and its time includes that of the builds its input probes, which come first.
  - In the JSON, a `HashBuild` is one more entry of the `inputs` of the operator that prepares it.
- Metrics:
  - `parts`, `skipped` (by statistics);
  - `raw_parts`, `raw_rows` (a GROUP BY's parts that sent rows straight to the partitions, because their first
    rows hardly reduced);
  - `part_time` (the part tasks' time, summed);
  - `wait` (for parts);
  - `merge`, `lanes_tail` (merging after the last part), `build` (output rows), `outer`;
  - `sort`, `groups`, and `sample_parts`, `heavy_keys`, `heavy_groups` of a two-level aggregation;
  - a `HashJoin`'s `find` (looking up the keys; none where a join keeps every row or none without looking: a one-row
    join, an anti join over a build without rows, a null-aware anti join over an empty build input or one with a
    NULL key), `gather` (the rows' columns: the build columns an inner or a left join gathers, and the probe rows it
    takes on its other path than 1:1; the columns of the candidates that residuals read; a one-row join's values, which
    its build makes once), `residual` and, when the build's keys are unique, an inner join's `window_rows`, and a left
    join's without residuals (the rows of the probe's batches it gathered the build's columns for, next to the `rows` it
    returned);
  - a `HashBuild`'s `finish` (the table built from its parts, and a one-row join's values), `null_keys` (rows with a
    NULL key, never held), `unique` (1: no key repeats) and `direct` (1: the table indexes its one integer key by
    value, ADR 0022).
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
  ([Binding](#binding)), except that a DECIMAL compares with a number DuckDB types as DOUBLE in DOUBLE, as DuckDB
  does. A literal outside the column type's range turns the comparison into constant true or false for non-NULL
  values; NULL values still compare as NULL. A decimal literal compared with an integer column becomes an
  equivalent integer comparison: `c > 1.5` becomes `c >= 2`, and `c = 1.5` is never true.
- WHERE: the comparisons are evaluated with Arrow's comparison kernels (a DECIMAL against a DECIMAL of another scale
  or an integer exactly by its unscaled values, against a DOUBLE after DuckDB's conversion) and combined with Kleene
  AND; a row passes
  only when every comparison is true, so a comparison that is NULL rejects it. `OR` and `NOT` follow SQL's
  three-valued logic too (`NULL OR TRUE` is true, `NOT NULL` is NULL); a comparison folded to always or never true
  is still NULL for a NULL operand there, so `NOT (smallint_col = 1.5)` rejects NULL and keeps every other row.
- BETWEEN: `a BETWEEN lo AND hi` is `lo <= a AND a <= hi` in three-valued logic, both bounds inclusive: a range with
  `lo > hi` matches nothing, and a NULL operand (or a NULL bound column) makes it NULL, which rejects the row;
  `a NOT BETWEEN lo AND hi` is its negation, so it rejects that row too.
  An argument of `AND` or `OR` is computed only for the rows the earlier ones leave undecided (`x > 100 OR x * x > 0`
  never computes `x * x` where `x > 100`), as DuckDB does for the same order (divergence D16).
  VARCHAR compares byte-wise and DATE chronologically. A predicate folded to never-true reads no data at all.
- IN: `c IN (v1, v2, ...)` is `c = v1 OR c = v2 OR ...` with Kleene logic, and `c NOT IN (...)` its negation, so a NULL
  value rejects the row for both. Each value is typed and folded exactly like `c = v` ([Binding](#binding)); a value
  that no column value can equal (out of the column type's range, or not an integer for an integer column) is dropped,
  and without values `IN` is `FALSE` and `NOT IN` is `IS NOT NULL`. DuckDB gives the list one type: when a number in it
  types as DOUBLE (an exponent, or a decimal of more than 38 digits), every number is read as a double, so an integer
  column compares with the nearest doubles (divergence D7) and a FLOAT column in DOUBLE (`f IN (0.1, 1e0)` does not
  match the FLOAT `0.1`, while `f IN (0.1, 2)` does). A DECIMAL column then compares in DOUBLE with every value, each
  converted as DuckDB converts it (a decimal literal as its DECIMAL, a HUGEINT literal through DuckDB's 128-bit
  formula), and an integer outside -2^127 to 2^128 - 1 makes its list DOUBLE too. NaN is not equal to NaN, as for `=`
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
  range is an execution error (divergence D9); no column is HUGEINT, so only a `SUM` of a HUGEINT expression (one
  with an integer literal beyond BIGINT) or arithmetic on a `SUM` can reach it.
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
- DECIMAL: a DECIMAL(p,s) value is an exact integer of at most p digits, the unscaled value, times 10^-s ([ADR
  0021](adr/0021-decimal-semantics.md)). Comparisons with numbers (`=`, `<>`, `<`, `IN`, `BETWEEN`), `ORDER BY`, `GROUP
  BY`, `COUNT(DISTINCT)`, `MIN` and `MAX` use the unscaled value; `MIN` and `MAX` keep DECIMAL(p,s), `COUNT` is BIGINT.
  A DECIMAL compares with a DECIMAL of another precision or scale and with an integer exactly by value (the side with
  the smaller scale scaled up, never rounded, at any number of digits), and with a DOUBLE (a column, an expression, a
  number DuckDB types as DOUBLE) in DOUBLE after the conversion below. `+`, `-` and `*` compute exactly in their
  result type (Binding): an operand of `+` or `-` with a smaller scale is first rescaled to it, and a value that does
  not fit fails the query with DuckDB's conversion error (`Casting value "..." to type DECIMAL(38,10) failed: value is
  out of range! when casting from source column d`, or `Could not cast value ... to DECIMAL(...)` for an integer); a
  result beyond the width, which only a width capped to 18 or 38 can reach, fails with DuckDB's overflow error
  (`Overflow in addition of DECIMAL(18) (a + b). You might want to add an explicit cast to a bigger decimal.`, likewise
  `subtract` and `multiplication`). Both are execution errors (exit code 1). As in DuckDB, the left operand is computed
  and rescaled before the right one, then the operation; with several failing rows the error can name another row than
  DuckDB's (divergence D19). `SUM` of a DECIMAL(p,s) is an exact DECIMAL(38,s): the same for any number of threads, NULL
  over no values, and a sum beyond 38 digits is an execution error (divergence D18). `AVG` is DOUBLE, computed as DuckDB
  computes it: the exact sum, as a `long double` from its two 64-bit halves, divided by the count times 10^s (in
  `double` for a width up to 4), so it agrees with DuckDB to the bit on each platform, even where that is not the
  correctly rounded mean. `%` computes in its result type, both operands rescaled to its scale; it takes the dividend's
  sign and is NULL for a zero divisor. A DECIMAL becomes DOUBLE (in `/` and `//`, next to a DOUBLE, or as a decimal
  literal compared with a DOUBLE) as DuckDB converts it: the unscaled value divided by 10^s when the width is at most 4,
  the scale 0 or the value at most 2^53 in magnitude, else (value div 10^s) + (value mod 10^s) / 10^s, each part
  converted on its own (a value beyond 18 digits through the 128-bit formula); this is not always the nearest double
  (`9007199254740993.5` becomes 2^53). `/` then divides as for DOUBLE (`inf`, `-inf` or NaN for a zero divisor), and
  `//` is the same division, NULL for a zero divisor; `%` beyond 38 digits is `fmod` of the converted operands, NULL
  for a zero divisor (NaN only with a DOUBLE operand). A `CASE` casts each value to its DECIMAL type as DuckDB casts
  (Binding): rescaled, rounded half away from zero where the scale shrinks, an integer times 10^s, and a value that
  does not fit fails with DuckDB's conversion error on the rows that take it. Not supported (exit code 4): a `BETWEEN`
  that mixes a DOUBLE value with DECIMAL values that antb1 would compare exactly ([Grammar](#grammar)), and a string
  literal as a value of a DECIMAL `CASE`. A DECIMAL against a VARCHAR, DATE or TIMESTAMP operand that is no literal is
  a bind error, as for any number (divergence D4). A scan that reads a DECIMAL column applies no predicate itself, so
  every `WHERE` condition of that scan is evaluated by the `Filter` (`explain --analyze` shows no pushed predicate),
  and a condition on a DECIMAL column skips no row group ([ADR 0021](adr/0021-decimal-semantics.md)).
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
| 1 | query error: syntax, bind, execution or memory error | `SELECT COUNT(*) FORM t`; `FROM t semi` (a word that is never a table alias); an unknown table or column; `SUM` of a VARCHAR column; a `SUM`, or arithmetic on a `SUM`, outside HUGEINT's range; an invalid `regexp_replace` pattern; a query that needs more memory than `--memory-limit` |
| 2 | usage error | unknown option; neither or both of `-c` and `-f`; a malformed `--table`, `--column-type` or `--memory-limit`; a column that `--column-type` cannot read as DATE; a table name registered twice |
| 3 | I/O error | a missing or unreadable file; not a Parquet file; schemas that differ; a glob that matches nothing |
| 4 | unsupported: valid-looking SQL outside the supported subset | `row_number() OVER ()`; `IS NULL`; an unknown function; `SELECT 1e3`; `SUM(DISTINCT ...)`; `CAST(a AS BIGINT)`; a string literal as a DECIMAL `CASE` value; a column of an unsupported type; `FROM a LEFT JOIN b ON a.k = b.k`; `FROM t semi JOIN u ON ...`; `FROM a, b` with no join key between them (a cross product); more than 256 relations |
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
| D2 | Column-type overrides | `--clickbench` and `--column-type COL=DATE` apply to every table with that column, including tables opened with `FROM '<path>'` | the oracle applies `make_date(EventDate)` only to the named tables with the `clickbench` option in `tests/slt/tables.txt`; `FROM '<path>'` reads the raw integers | the runner rejects a table list where some tables with an `EventDate` column have the option and others do not; the random generator reads no overridden column in a query with a `FROM '<path>'` item, a join's included; `.slt` records read `EventDate` only through table names |
| D3 | Literal types | a string literal compared with a numeric or DECIMAL column is a bind error | casts the string to the column's type | `onlyif antb1` records in `tests/slt/cases/basic/bind_errors.slt`; the query generator writes numbers for numeric and DECIMAL columns |
| D4 | Literal types | a number or a `DATE` literal (also written as a cast) compared with a VARCHAR column is a bind error, and so is a number (a DECIMAL too) compared with a VARCHAR, DATE or TIMESTAMP operand that is no literal | casts the column's values at run time (a conversion error unless every value converts); with `=`, `<>` and `IN` it casts a VARCHAR operand to the number, and a number against a DATE or TIMESTAMP operand fails only when a row is compared | as D3, and in `tests/slt/cases/where/cast_date.slt`; `plan.Expressions/BindErrorTest` pins the operand errors (`h = s`, `h = dt`, `h = toDateTime(i64)`); the generator writes strings for VARCHAR columns and compares a DECIMAL column only with columns of numbers |
| D5 | Date and timestamp literals | a date must be written exactly `YYYY-MM-DD` (also in a cast to DATE), a timestamp exactly as in the literal table (two-digit fields, one space or `T`, hours 00 to 23) | also accepts `2013-7-1`, surrounding spaces and a time of day for a DATE, and single-digit fields, spaces and `24:00:00` for a TIMESTAMP | as D4; the generator writes `YYYY-MM-DD` and `YYYY-MM-DD HH:MM:SS` |
| D7 | DOUBLE literals and BIGINT | a number that DuckDB types as DOUBLE (an exponent, or a decimal of more than 38 digits) is rounded to the nearest double like in DuckDB, then compared exactly with the integer column; in an `IN` list with such a number every value is rounded to the nearest double, a decimal literal too, and an integer outside -2^127 to 2^128 - 1 does not make the list DOUBLE | converts BIGINT (and HUGEINT) values to DOUBLE for the comparison, so values beyond 2^53 compare rounded: `i64 >= 9223372036854775808e0` holds for `9223372036854775807`; in a DOUBLE `IN` list a decimal literal converts as its DECIMAL (`9007199254740993.5` becomes 2^53), and an integer outside -2^127 to 2^128 - 1 is DOUBLE as well | the `.slt` records with such literals avoid BIGINT values beyond 2^53 (`tests/slt/cases/where/folding.slt`); `plan.ApproximateNumbers/FoldThroughBinderTest.*` and `plan.BinderTest.In` pin antb1's folding; the generator writes no exponents for integer columns |
| D8 | Result names | an aggregate's argument is quoted when it is not a plain identifier or is a reserved word | also quotes non-reserved keywords (`sum("year")`) | the tests compare values and types, not names |
| D9 | HUGEINT range | HUGEINT is decimal128(38, 0): a `SUM`, or arithmetic on a `SUM`, outside -(10^38 - 1) to 10^38 - 1 is an execution error (exit code 1). An integer SUM over BIGINT or smaller types cannot reach it | HUGEINT holds -(2^127 - 1) to 2^127 - 1 | no column is HUGEINT (a DECIMAL(38, 0) column is DECIMAL); `exec.AggregateStateTest.HugeIntSumIsCheckedAgainstTheRange` checks the error |
| D10 | NaN | MIN and MAX ignore NaN like Arrow's `min_max`, whatever the batch and file boundaries: they return NaN only when every selected non-NULL value is NaN (so only MAX over NaN and other values differs from DuckDB). Arrow's comparison kernels follow IEEE 754: NaN compares unequal to everything, so `d > 1` and `d >= 1` are false for NaN, and `d IN (...)` never matches it | orders NaN above every other value and equal to itself: MIN and MAX return NaN when it is the extreme, `d > 1` is true for NaN | the fixtures contain no NaN (fixturegen builds doubles from integer ratios); `exec.AggregateStateTest.MinMaxOfDoublesIgnoreNaNInEveryBatchSplit` and `engine.SessionTest.MinMaxIgnoreNaNAcrossBatchesAndFiles` pin antb1's MIN and MAX |
| D11 | FLOAT columns | read as DOUBLE (widened exactly): results of FLOAT columns are DOUBLE and print with double precision; `WHERE` compares like DuckDB (see Binding); arithmetic on them, and comparing them with other expressions, is unsupported (exit code 4) | keeps FLOAT (`MIN`, `MAX` and projections return FLOAT) | the random generator never references a FLOAT column (`ColumnOf` in `tests/slt/runner/query_gen.cc`, `harness.LoadGenTables.SkipsFloatColumns`); `tests/slt/cases/where/float.slt` selects only other columns, and `engine.SessionTest.FloatColumnsCompareLikeDuckDb` pins that results stay DOUBLE |
| D12 | HUGEINT literals against DOUBLE | an integer literal beyond BIGINT compared with a DOUBLE column is the correctly rounded nearest double (a decimal literal converts as DuckDB converts a DECIMAL, [ADR 0021](adr/0021-decimal-semantics.md) rule 8: `9007199254740993.5` is 2^53 in both engines) | converts a HUGEINT literal through its 128-bit formula (lower + upper × 2^64), which can be one ulp off | the generator writes DOUBLE columns only their own sample values and short decimals (`DoubleText` and `ExactDecimalDouble` in `tests/slt/runner/query_gen.cc`); `plan.BinderTest` pins the decimal conversion and `tests/slt/cases/types/decimal_literals.slt` compares it with DuckDB |
| D13 | Decimals with many digits against integer and DECIMAL values | compared exactly, and a literal beyond the operand's type folds to a constant | compares in a DECIMAL whose width is capped at 38 digits: when the column type's digits plus the literal's decimals exceed 38, a column value with too many integer digits fails the query with a conversion error (`i16 = 1.0000000000000000000000000000000000001` over the value -32768); likewise an integer `SUM` (HUGEINT, 38 digits) in `HAVING` against any decimal fails once the sum has more digits than 38 minus the literal's decimals, and a DECIMAL(p,s) column against a literal with more than s decimals or more than p - s integer digits once their total exceeds 38 (`d38_10 > 0.00000000001` over a value of 28 integer digits). Two operands that are no literals fail the same way, in comparisons, `IN` lists and `BETWEEN`: a DECIMAL against a DECIMAL of another scale or an integer, once a value of the side with the smaller scale has more integer digits than 38 minus the larger scale (`d9_2 < d38_0` over d38_0's extremes, a DECIMAL(38,2) against a DECIMAL(38,12) past 26 integer digits). An integer literal of more than 38 - s digits against a DECIMAL(p,s) (`p < 1000000000000000000000000000000000000` on DECIMAL(15,2)), and one of 2^127 to 2^128 - 1 (UHUGEINT) against a SMALLINT, INTEGER or BIGINT operand, fails to cast on any input that reaches it | the `.slt` records and the generator keep literals short enough (`DecimalText` in `tests/slt/runner/query_gen.cc`) and compare two columns only while their common type stays within 38 digits (`ColumnComparison`, columns of two FROM items of a join included), and join two DECIMAL keys only then (`UsableEdges`); `tests/slt/cases/types/decimal.slt` and `decimal_comparisons.slt` pin both answers; `plan.Binder/FoldThroughBinderTest.*` and `plan.Binder/FoldDecimalTest.*` cover the exact folding, `exec.DecimalTest.CompareExactIsExactAcrossScalesAndIntegers` the exact comparison |
| D14 | Overflows DuckDB's optimizer does not avoid | a comparison that is a whole `WHERE` or `HAVING` conjunct and folds to never-true at bind time (a literal outside the operand's type, as in `smallint_col + 1 > 40000`, or a DECIMAL result's, as in `price * qty > 100000000000000000000` on DECIMAL(15,2) columns) computes nothing, so it cannot overflow; one that folds to always-true keeps `IS NOT NULL` over its operand, which is computed, and under `OR` or `NOT` or in a `CASE WHEN` condition a folded comparison keeps NULL for a NULL operand, so its operand is computed and overflows as in DuckDB (`p * q > 100000000000000000000 OR id = 1` fails in both engines) | computes the operand and fails on an overflow ("Overflow in addition of INT16") | the random generator never writes arithmetic that can overflow; `plan.BinderTest.WhereMovesConstantsLikeDuckDb` pins which comparisons move their constants |
| D15 | VARCHAR bytes that are not UTF-8 | answers: `strlen` counts every byte, and `regexp_replace` runs RE2 over the bytes as UTF-8, where an invalid byte never matches (not even `.` or `[^a]`) and stays in the result | cannot read such a value as VARCHAR: reading an unannotated BYTE_ARRAY column (`binary_as_string`) with it fails the query ("Invalid string encoding") | every fixture string is valid UTF-8, so the oracle tests never meet it; `exec.ComputeTest.StringFunctions` pins antb1's behavior |
| D16 | Evaluation order in conditions | computes the arguments of `AND` and `OR` in the order written, each only for the rows still undecided, and a WHERE conjunct's operands for every row; so an overflow, or a failed cast of a DECIMAL `CASE` value, inside a condition fails exactly when a row reaches it in that order | may reorder conjunctions by its cost model, and computes the argument of a `NOT` for every row, so an overflow or a failed cast can fail in one engine and not the other (`NOT (x < 100 AND x * x > 0)` fails in DuckDB) | the random generator never writes arithmetic that can overflow nor a `CASE` value whose cast can fail; `exec.ComputeTest.ConditionsAreThreeValued` pins antb1's order |
| D17 | Overflows in constant expressions | a constant integer expression on a side of a comparison or a `BETWEEN` bound that overflows its type (`a > 2147483647 + 1`) is a bind error, whatever the table holds | raises the overflow only when a row computes it: an empty table, or one whose rows another conjunct rejects first, answers | `plan.Expressions/BindErrorTest` pins the errors; `tests/slt/cases/where/between.slt` has the error on a table both engines fail on and, `onlyif antb1`, on the empty table |
| D18 | DECIMAL SUM beyond 38 digits | a `SUM` of a DECIMAL(p,s) whose result has more than 38 digits is an execution error (`SUM overflow: the result is outside the range of DECIMAL(38,s) (38 decimal digits)`), as for HUGEINT (D9) | returns up to 39 digits until its 128-bit sum overflows | `tests/slt/cases/types/decimal_arithmetic.slt` pins both answers; the generator sums only DECIMAL columns whose sum over every row fits 38 digits |
| D19 | Which failing row an error names | the overflow and cast errors of DECIMAL arithmetic, and the cast errors of DECIMAL `CASE` values, print the values of the first failing row of a 64Ki-row batch (each operand, then the operation); a dependent `GROUP BY` key (ADR 0018) is computed per group, in group order; a failed cast names the column it casts (`when casting from source column b`) only for a column reference, an aggregate's output and a key included | evaluates 2048-row vectors, and every key per row: with several failing rows the message can show another row's values; it also names the column of an expression its optimizer reduces to one (`b + 0`); the exit code and whether a query fails are the same | the `.slt` records match the error text without the values; `exec.ComputeTest.DecimalOperandsInDuckDbOrder` pins the operand order, `exec.ComputeTest.DecimalCaseValuesCastToTheCaseType` the named columns |
| D20 | CASE types without a DECIMAL value | types the values that are no literals first, then lets each integer literal take their integer type when it fits and each string literal any type: `CASE WHEN c THEN 7 WHEN c2 THEN s16 END` and `CASE WHEN c THEN 8 WHEN c2 THEN s16 ELSE 7 END` are SMALLINT, a negated literal in parentheses (`-(7)`) counts as an INTEGER expression, and a string literal before a DATE value is that DATE (`THEN '2020-01-01' WHEN c2 THEN dt END`) and before a number unsupported | folds from the `ELSE` value (NULL without one) through the `THEN` values in order, where a literal next to NULL (the first `THEN` value without an `ELSE`) or next to another literal becomes its own type (both CASEs are INTEGER), while an `ELSE` literal takes the next value's type; `-(7)` is a literal (SMALLINT next to `s16`), and a string literal next to NULL is VARCHAR (a bind error next to the DATE or the number) | `plan.BinderTest.CaseTypesWithoutADecimalAreAntb1s` pins antb1's types; the random generator writes the column first among its `CASE` values, where both agree; with a DECIMAL value antb1 folds as DuckDB does ([ADR 0021](adr/0021-decimal-semantics.md) rule 10) |
| D21 | Keywords as names | the 49 words that are never table aliases ([Grammar](#grammar)) are unquoted column, table and function names, qualifiers and select aliases, with or without `AS` (without `AS`, `glob`, `isnull` and `notnull` continue the expression: unsupported), so `SELECT default FROM t` answers with a column named `default`; `BETWEEN`, `EXISTS`, `INTERVAL` and `OVER` are reserved: no unquoted column or table names (an error) or qualifiers (unsupported), but table aliases, as in DuckDB | refuses all 49 words as unquoted column names (`default` is its `DEFAULT` keyword, an error wherever a query uses it: `SELECT default FROM t` is a binder error), table names, qualifiers and select aliases without `AS` (after an expression `isnull` and `notnull` are its operators `IS NULL` and `IS NOT NULL`, not aliases); accepts them after `AS` and after a dot, and 14 of them as function names (`unpack(...)` is its `UNPACK` operator, the other 34 are syntax errors); accepts `BETWEEN`, `EXISTS`, `INTERVAL` and `OVER` as column and table names and as qualifiers | the fixtures, the `.slt` records and the random generator use none of these words as an unquoted name; `sql.ParserTest.WordsThatCannotBeImplicitAliases`, `sql.ParserTest.EveryReservedWordAfterAsInFrom` and `sql.ParserTest.ReservedWordsThatQualifyNamesInFrom` pin antb1's rules, and `harness.TableAliasOracle.*` compares every DuckDB keyword as a table alias and as a qualifier in FROM with DuckDB itself |

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
fail cleanly with exit code 4 (unsupported). Today Q1, Q3, Q5, Q6, Q10, Q12 and Q14 pass; the plan is recorded in
ADRs 0021 (DECIMAL), 0022
(joins, query blocks and uncorrelated subqueries) and 0023 (correlated subqueries). `tests/tpch/` generates the data
at test time ([testing.md](testing.md#data-derived-from-tpc-h)), and `tpch.status.sf0_01`, `tpch.status.sf0_1` and
`parallel.tpch.status.sf0_1` enforce the ratchet
([testing.md](testing.md#the-ratchet-of-the-queries-derived-from-tpc-h)). The last column names the work whose PR
completes the query; most queries also need the DECIMAL work of ADR 0021 before it.

| Query | Status | Completed by |
| --- | --- | --- |
| Q1 | pass | DECIMAL arithmetic, SUM and AVG (ADR 0021) |
| Q2 | unsupported (exit code 4) | correlated scalar aggregates (ADR 0023) |
| Q3 | pass | inner joins (ADR 0022) |
| Q4 | unsupported (exit code 4) | correlated EXISTS and NOT EXISTS (ADR 0023) |
| Q5 | pass | inner joins (ADR 0022) |
| Q6 | pass | DECIMAL arithmetic, SUM and AVG (ADR 0021) |
| Q7 | unsupported (exit code 4) | derived tables (ADR 0022) |
| Q8 | unsupported (exit code 4) | derived tables (ADR 0022) |
| Q9 | unsupported (exit code 4) | derived tables (ADR 0022) |
| Q10 | pass | inner joins (ADR 0022) |
| Q11 | unsupported (exit code 4) | uncorrelated subqueries as joins (ADR 0022) |
| Q12 | pass | inner joins (ADR 0022) |
| Q13 | unsupported (exit code 4) | LEFT JOIN (ADR 0022) |
| Q14 | pass | inner joins (ADR 0022) |
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
