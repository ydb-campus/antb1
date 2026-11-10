# SQL subset

antb1 accepts a single `SELECT` statement over Parquet tables. The goal of the first slice is a small, exactly
specified subset that behaves like DuckDB, which serves as the test oracle. Everything outside the implemented subset
is rejected cleanly with an error and a source position (normally exit code 4), never answered wrongly.

This page is the contract: a PR that changes SQL behavior updates it in the same PR.

## What works today

All of this runs (the [grammar](#grammar) below accepts more than the binder answers): global and grouped
(`GROUP BY`) aggregates, `COUNT(DISTINCT ...)` included, projections (columns, constants, and `*` and `t.*`
anywhere among the select items), `WHERE`
conditions (comparisons between columns, expressions and literals, `[NOT] LIKE 'pattern'`,
`[NOT] IN (literal, ...)` and `[NOT] BETWEEN low AND high`, combined with `AND`, `OR` and `NOT`, with constant
integer arithmetic folded on a side and in an `IN` list), `GROUP BY` and `ORDER BY` (also by position), `HAVING`
(the same conditions on aggregates and keys), arithmetic (`+ - * / // %` and unary `-`) over integers, DOUBLE and
DECIMAL, the string
functions `strlen`, `regexp_replace` and `substring`, the timestamp functions `toDateTime`, `EXTRACT` and
`date_trunc`, and
`CASE` and `COALESCE` in every clause, dates written as casts or literals (`CAST('2013-07-01' AS DATE)`,
`'2013-07-01'::DATE`,
`DATE '2013-07-01'`), `LIMIT` and `OFFSET`, over one table of Parquet files or several joined by inner or left
outer joins, with uncorrelated `IN` subqueries, scalar and `EXISTS` subqueries correlated or not,
and `UNION ALL`, `UNION`, `INTERSECT` and `EXCEPT`
of two or more blocks. This covers all 43 ClickBench queries (see
[ClickBench status](#clickbench-status)). All 22 queries derived from TPC-H pass
(see [Queries derived from TPC-H](#queries-derived-from-tpc-h)).

```sql
SELECT COUNT(*), COUNT(DISTINCT OS), AVG(ResolutionWidth) AS width FROM hits WHERE OS BETWEEN 2 AND 44
SELECT WatchID, URL FROM hits WHERE URL LIKE 'https://%' AND Title NOT LIKE 'Promo_%' LIMIT 10
SELECT RegionID, COUNT(*) AS n, MAX(strlen(URL)) FROM hits GROUP BY 1 ORDER BY n DESC LIMIT 10 OFFSET 5
SELECT CounterID, COUNT(*) AS n FROM hits GROUP BY CounterID HAVING n > 260 AND MAX(URL) LIKE 'https%'
SELECT * FROM trips WHERE tr_promo IN (1, 2) AND NOT (tr_day > '2026-03-20'::DATE OR tr_rate < 1.25) LIMIT 5
SELECT z.label, SUM(t.tr_fare) FROM trips AS t JOIN zones AS z ON t.tr_pickup = z.zn_id GROUP BY z.label
SELECT AVG(t.tr_fare * f.tf_rate) FROM trips AS t, tariffs AS f WHERE t.tr_tariff = f.tf_code
SELECT CASE WHEN EXTRACT(DAY FROM tr_day) > 15 THEN 'late' ELSE 'early' END, COUNT(*) FROM trips GROUP BY 1
SELECT date_trunc('month', tr_day) AS m, MIN(tr_fare) FROM trips GROUP BY 1 ORDER BY m NULLS FIRST
```

- Each table is registered with `--table NAME=PATH[,PATH|GLOB]` (an identifier or a `"quoted identifier"`, matched
  ASCII case-insensitively), once per table, or given as a string literal with a path or glob, e.g.
  `FROM '/data/hits_*.parquet'`. The examples above read a hits-shaped table and the join fixtures `trips`, `zones`
  and `tariffs` ([Fixtures](testing.md#fixtures)).
- Only the columns a query references are decoded. `COUNT(*)` without `WHERE` is answered from the Parquet footers
  (no data page is read); under `WHERE` it counts the rows the filter selects without copying them.
- Result names and types follow DuckDB ([Binding](#binding)); values follow the [Semantics](#semantics) below.
- `--` line comments, `/* block */` comments and one trailing `;` are allowed.
- SQL outside the grammar (a window function such as `row_number() OVER ()`, `IS TRUE`, other functions, ...) fails
  with exit code 4 and points at the first unsupported token. A `FROM` list of tables and paths, joined by commas,
  `CROSS JOIN`, `[INNER] JOIN ... ON`, `LEFT [OUTER] JOIN ... ON` or `FULL [OUTER] JOIN ... ON`, with aliases and
  qualified names, is answered. Relations that no join key connects are a cross product
  ([ADR 0040](adr/0040-nested-loop-joins.md)), answered as a nested loop whose conditions between them (an inequality,
  `<>`, OR, or none) decide its pairs; a derived table or a CTE reference of exactly one row (an aggregate without
  `GROUP BY`, `HAVING`, `LIMIT` or `OFFSET`) that no condition reads, but one of the block's slot (a subquery
  conjunct, or one that holds a subquery's value), joins the others as one row. A `LEFT JOIN` or a `FULL JOIN` whose
  `ON` has no equality key between its own two inputs is a nested loop too,
  padding the rows no pair passes (ADR 0040's OJ4c).
  A derived table and a `WITH` list are answered too, each bound as a sub-plan of its own. Of the subquery forms in
  an expression, `x [NOT] IN (query)` and `NOT (x IN (query))` are answered as a top-level `AND` conjunct of `WHERE`,
  of an inner join's `ON` or of `HAVING`, where `x` is an operand (a column, or an expression over one; in `HAVING`,
  an aggregate too) and the query has exactly one column; an `IN` key needs a type that holds both sides (so no
  `DOUBLE`). A scalar subquery that reads no enclosing block is a value anywhere inside a condition of `WHERE` or of
  an inner join's `ON` (one inside a `FULL JOIN`'s left input too): under `AND`, `OR`, `NOT` and `CASE`, in
  arithmetic, functions, `COALESCE` and casts, `IS [NOT] NULL`, `IS [NOT] DISTINCT FROM`, an `IN` list, `LIKE`'s
  operand and `BETWEEN`; in `HAVING`, as a direct operand of a top-level comparison or a `BETWEEN`'s bound. Its query
  may have any shape: its value is NULL over no row, the row's over one, and over two or more the query fails with
  DuckDB's error (exit code 1). A correlated one must be a direct operand of a top-level comparison of `WHERE` or of
  an inner `ON`, and an aggregate over every row — no `GROUP BY`, `HAVING`, `LIMIT` or `OFFSET`, each of which could
  leave no row. Every other placement exits 4, `NOT (x NOT IN (query))`, `NOT NOT (x IN (query))`, an `IN` subquery
  under `OR`, `NOT` or `CASE`, and a value in an `IN` subquery's probe (`x + (query) IN (query)`, ADR 0041's SQ7)
  among them; a subquery column count other than one is a bind error, as in DuckDB. `EXISTS (query)` that reads no
  enclosing block is a condition anywhere
  inside a condition of `WHERE` or of an inner `ON` (under `AND`, `OR`, `NOT` and `CASE WHEN`) and a top-level
  conjunct of `HAVING`; a correlated one is answered as a top-level conjunct of `WHERE` or of an inner `ON`, and
  nowhere else; `TRY_CAST`, a
  lateral reference, more than 256
  relations counting every sub-plan and every subquery: exit code 4 as well. A query in parentheses that starts with neither
  `SELECT` nor `WITH` is refused at its first token: in an expression `(FROM t)` and `(TABLE t)` are, since those
  words are reserved, while `(VALUES (1))` and `(PIVOT ...)` are not — `VALUES` and the `PIVOT`, `SHOW`,
  `SUMMARIZE` and `DESCRIBE` family are names there (divergence D21), so `VALUES(1)` is a call and
  `-UNPIVOT(a)` keeps its canonical form. In `FROM`, where none of them is a name, every one of them is refused
  ([ADR 0025](adr/0025-values-table-constructor.md) postpones the constructor itself). A `(` opens a nested
  query only when a reserved query word follows it, so `((a + 1))` is an expression in two pairs of parentheses and the
  inner pair of `((SELECT ...))` is the subquery; `[NOT] IN ((query))` is therefore an `IN` list holding one scalar
  subquery, where DuckDB reads an `IN` subquery and answers it as the single-paren form (divergence D22). antb1 keeps
  its own reading and exits 4 there, at the inner `(`. Malformed SQL (a syntax error) and SQL that is wrong for the
  tables (a bind error) fail with exit code 1.

```bash
pixi run antb1 query -f query.sql --table hits=/data/clickbench/hits_0.parquet --clickbench
pixi run antb1 explain -f query.sql --table hits=/data/clickbench/hits_0.parquet --clickbench
pixi run antb1 explain --analyze -f query.sql --table hits=/data/clickbench/hits_0.parquet --clickbench
pixi run antb1 query -f query.sql --table trips=/data/trips.parquet --table zones=/data/zones.parquet
```

Globs are allowed in the file-name part of a path only (`/data/hits_*.parquet`, not `/data/*/hits.parquet`) and
expand to a sorted file list. All files of a table must have the same schema (divergence D1 below).

## Grammar

The parser accepts this grammar; the binder answers the part of it described below and checks it against the tables,
and the executor runs it. Keywords are case-insensitive.

```ebnf
statement   = query , [ ";" ] ;
query       = [ "WITH" , cte , { "," , cte } ] , primary ,
              { ( "UNION" | "INTERSECT" | "EXCEPT" ) , [ "ALL" | "DISTINCT" ] , primary } ,
              [ "ORDER" , "BY" , order_item , { "," , order_item } ] ,
              [ limit_offset ] ;
primary     = block | "(" , query , ")" ;
cte         = identifier , [ columns ] , "AS" , "(" , query , ")" ;
block       = "SELECT" , [ "DISTINCT" ] , select_list , "FROM" , from_list , [ "WHERE" , expr ] ,
              [ "GROUP" , "BY" , group_item , { "," , group_item } ] , [ "HAVING" , expr ] ;
group_item  = expr | grouping ;   (* grouping elements parse, and are refused: see below *)
grouping    = "(" , ")" | "(" , expr , "," , expr , { "," , expr } , ")"
            | ( "ROLLUP" | "CUBE" ) , "(" , rollup_item , { "," , rollup_item } , ")"
            | "GROUPING" , "SETS" , "(" , group_item , { "," , group_item } , ")" ;
rollup_item = expr | "(" , expr , "," , expr , { "," , expr } , ")" ;
limit_offset = "LIMIT" , integer , [ "OFFSET" , integer ] | "OFFSET" , integer , [ "LIMIT" , integer ] ;
select_list = select_item , { "," , select_item } ;
select_item = "*" | identifier , "." , "*" | expr , [ [ "AS" ] , identifier ] | "NULL" , [ "AS" , identifier ] ;
order_item  = expr , [ "ASC" | "DESC" ] , [ "NULLS" , ( "FIRST" | "LAST" ) ] ;
from_list   = from_item , { "," , from_item
                          | "CROSS" , "JOIN" , from_item
                          | [ "INNER" ] , "JOIN" , from_item , "ON" , expr
                          | "LEFT" , [ "OUTER" ] , "JOIN" , from_item , "ON" , expr
                          | "FULL" , [ "OUTER" ] , "JOIN" , from_item , "ON" , expr } ;
from_item   = ( identifier | string_literal ) , [ [ "AS" ] , identifier | "AS" , string_literal ]
            | "(" , query , ")" , [ ( [ "AS" ] , identifier | "AS" , string_literal ) , [ columns ] ] ;
columns     = "(" , identifier , { "," , identifier } , ")" ;
column_ref  = [ identifier , "." ] , identifier ;
expr        = expr , "OR" , expr | expr , "AND" , expr | "NOT" , expr | condition | sum ;
condition   = sum , cmp_op , sum
            | sum , [ "NOT" ] , "LIKE" , sum
            | sum , [ "NOT" ] , "IN" , "(" , expr , { "," , expr } , ")"
            | sum , [ "NOT" ] , "IN" , "(" , query , ")"
            | "EXISTS" , "(" , query , ")"
            | sum , [ "NOT" ] , "BETWEEN" , sum , "AND" , sum
            | sum , "IS" , [ "NOT" ] , "NULL"
            | ( sum | "NULL" ) , "IS" , [ "NOT" ] , "DISTINCT" , "FROM" , ( sum | "NULL" ) ;
sum         = sum , ( "+" | "-" ) , product | product ;
product     = product , ( "*" | "/" | "//" | "%" ) , unary | unary ;
unary       = "-" , unary | postfix ;
postfix     = primary , { "::" , type } ;
primary     = column_ref | literal | "(" , expr , ")" | "(" , query , ")" | agg_call
            | identifier , "(" , [ expr , { "," , expr } ] , ")"
            | "CASE" , [ expr ] , "WHEN" , expr , "THEN" , ( "NULL" | expr ) ,
              { "WHEN" , expr , "THEN" , ( "NULL" | expr ) } , [ "ELSE" , ( "NULL" | expr ) ] , "END"
            | "EXTRACT" , "(" , identifier , "FROM" , expr , ")"
            | ( "CAST" | "TRY_CAST" ) , "(" , expr , "AS" , type , ")" ;
type        = identifier , [ "(" , integer , { "," , integer } , ")" ] ;
agg_call    = "COUNT" , "(" , "*" , ")"
            | "COUNT" , "(" , "DISTINCT" , expr , ")"
            | ( "COUNT" | "SUM" | "AVG" | "MIN" | "MAX" | "STDDEV_SAMP" | "STDDEV" | "STDDEV_POP"
              | "VAR_SAMP" | "VARIANCE" | "VAR_POP" ) , "(" , expr , ")" ;
cmp_op      = "=" | "<>" | "!=" | "<" | "<=" | ">" | ">=" ;
literal     = [ "-" ] , integer | [ "-" ] , decimal | string_literal | "DATE" , string_literal
            | "TIMESTAMP" , string_literal ;
```

Operators bind from loosest to tightest: `OR`, `AND`, `NOT`, the comparisons with `LIKE`, `IN`, `BETWEEN`, `IS
[NOT] NULL` and `IS [NOT] DISTINCT FROM` (which do not chain: `a = b = c`, `a = b IS NULL`, `a = b IS DISTINCT FROM c`
and `a BETWEEN 1 AND 2 = b` are unsupported; the bounds
of `BETWEEN` bind like the operand of
`+`, so its `AND` is its own and `a BETWEEN 1 AND 2 AND b = 3` is two conjuncts), `+` and `-`, `*`, `/`, `//` and `%`,
unary `-`, `::`; binary operators are left-associative, and parentheses group. An expression may be at most 256 levels
deep, counted along its deepest path through operators and parentheses (the top-level `AND` chain of `ON`, `WHERE` and
`HAVING` does not count, unless an `OR` makes it one tree), else it is unsupported. In a chain such as `a + b + c` each
operator pushes everything before it one level down, so `f(f(...)) + 1 + 1` counts the calls and the operators together.
A `::` and a unary `-` count one level more, as they do in the canonical form `CAST(x AS T)` and `-(x)` (a `-` before a
parenthesized operand does not), and so does a `NOT` that is the right operand of a comparison or of arithmetic, a
`LIKE` pattern or a `BETWEEN` bound (`a = NOT b` is `a = (NOT b)`). A `-` that is part of a number (see the lexical
rules) counts one level, as a parenthesis does, unless it stands directly before the digits: `-1` is as deep as `1`,
`- -1` as `(1)` and `-(1)` as `((1))`. The canonical form writes an `ON`, `WHERE` or `HAVING` predicate with a
top-level `OR` without parentheses, so it reads back as it was parsed. Aggregates are allowed in the select list,
`HAVING` and `ORDER BY`, and cannot be nested; one in `ON`, `WHERE` or `GROUP BY` is a syntax error.
A nested call is unsupported rather than a syntax error when `OVER` follows the outer call (`SUM(SUM(a)) OVER ()`,
a window function over an aggregate, as DuckDB takes it).

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
reuse a name. The binder answers both ([ADR 0022](adr/0022-joins-and-query-blocks.md)): each is a sub-plan with its
own scope, and one binding of the block that reads it.

Compounds: `UNION`, `INTERSECT` and `EXCEPT`, each with `ALL` or `DISTINCT` (the default), join two or more primaries
into one query, which stands wherever a query does: as the whole statement, a derived table's query, a CTE's query or
the query of a subquery ([ADR 0036](adr/0036-union-all.md), [ADR 0039](adr/0039-set-operations.md)). A primary is a
block or a query in parentheses, which may have a `WITH` list, a compound and an `ORDER BY`, `LIMIT` and `OFFSET` of its
own; a block in parentheses is the block, and a query that is one parenthesized query is that query (`((q))` is `q`). A
`WITH` list before the first primary belongs to the compound, and so do an `ORDER BY`, `LIMIT` and `OFFSET` after the
last one: they apply to all of its rows, never to the last primary alone. The parser keeps the branches in one flat
list, each with the operator written before it; `INTERSECT` binds tighter than `UNION` and `EXCEPT`, which associate to
the left, as in DuckDB (ADR 0039's tree). Each branch is one level below the compound for the depth limit: a later
branch is parsed a level down, so its depth error is where it passes the limit, and the first one moves down at the
first operator, which is where it is reported when that makes it too deep; each pair of parentheses around a query
counts a level too. The flat list is no flat tree: an operator or quantifier other than the one before it can start a
new run over everything before it, so each such change lifts what was parsed a level (`a UNION b UNION ALL c UNION d`
alternates twice), and a compound that alternates too often passes the depth limit at the operator that does; a run of
`INTERSECT` after a `UNION` or `EXCEPT` is that run's right operand, so its branches after the first are parsed a
further level down. A compound of any number of branches of one operator parses. Unsupported (exit code 4): `UNION [ALL
| DISTINCT] BY NAME` (at `BY`); a primary that is not a `SELECT` block or a parenthesized query, at its first token: a
`VALUES` list, a FROM-first query or `TABLE t`; a set operation after a parenthesized subquery that is the first value
of an `IN` list or of a parenthesized expression, or such a subquery before an `ORDER BY`, `LIMIT` or `OFFSET` (`x IN
((SELECT ...) UNION ALL SELECT ...)`, `x = ((SELECT ...) LIMIT 1)`, at its `(`); an `ORDER BY`, `LIMIT` or `OFFSET`
after a parenthesized query that has another of them of its own (`(q LIMIT 5) ORDER BY 1`), which DuckDB answers; and,
as in any block, `DISTINCT ON` and a `SELECT` without `FROM`. After a later value of an `IN` list or inside a larger
operand (`(SELECT ...) + 1 UNION ALL ...`), a set operation stays a syntax error, as in DuckDB. Syntax errors (exit code
1), as in DuckDB: an operator after an `ORDER BY`, `LIMIT` or `OFFSET` outside parentheses; an operator that no primary
follows (at the end, or before `)`, a second `ALL` or `DISTINCT`, or `WITH`: a query with its own `WITH` list goes in
parentheses); `BY NAME` after `INTERSECT` or `EXCEPT`; a parenthesized query with its own `WITH` list after a `WITH`
list; and an `ORDER BY`, `LIMIT` or `OFFSET` after a parenthesized query that has the same clause. The binder answers a
compound of `UNION`, `INTERSECT` and `EXCEPT`, `ALL` or not, in any nesting and with parenthesized branches, as the
whole statement, a derived table's query and a CTE's query ([Binding](#binding)). A compound's own `ORDER BY`, `LIMIT`
and `OFFSET` -- the outermost compound's, or a parenthesized one's -- are unsupported (exit code 4) at the first of
them, after any construct in its branches that is unsupported anyway, until ADR 0036's X5; a parenthesized block's own
are answered. A compound as the query of a scalar or `EXISTS` subquery is answered where no arm reads an enclosing
block (X6, [ADR 0041](adr/0041-subqueries-and-decorrelation.md)'s SQ4a); one with such an arm is unsupported at the
subquery's `EXISTS` or `(`, until SQ15, and so is a compound as the query of an `IN` subquery, at its `IN` once the
other side of its conjunct is checked, until SQ7. A compound in the FROM list of a subquery's query is answered. A
compound in a CTE that nothing references is never checked or bound, tail included, as no such CTE is.

Lexical rules: an `identifier` is a letter or `_` followed by letters, digits or `_`, or any text in double quotes
(`""` escapes a quote); a `string_literal` is text in single quotes (`''` escapes a quote); an `integer` is a
sequence of digits; a `decimal` is a number with a decimal point, an exponent or both (`1.5`, `.5`, `5.`, `1e3`).
Keywords are not reserved by the lexer. A `-` before a number is part of the literal, through parentheses and other
minuses, as DuckDB's parser folds it: `-1` and `-(1)` are the literal -1, `- -1` and `-(-(1))` the literal 1 and
`-(1.5)` the DECIMAL -1.5, each named and typed as the literal. A cast binds tighter, so a `-` before a number that `::`
follows negates the cast: `-1::INTEGER` and `-(1)::INTEGER` are `-(CAST(1 AS INTEGER))`, as in DuckDB. A `-` over
anything else (`-(1 + 2)`, `-a`) is an operator. The `identifier` of a `type` and the field
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
  `FALSE`, `NULL`, `SOME` and `TRUE` (`null.a`: NULL literals are not supported).
  `t.*` is a select item of its own (below), with its qualifier written as a column's; inside an expression
  (`1 + t.*`, `(t.*)`, `COUNT(t.*)`) it is unsupported, and so are `a.b.c`, `t.f()` and a qualified name before a
  string (`main.integer '5'`, which DuckDB reads as a typed literal of a qualified type).
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
  (`FROM t AS "a"`), `JOIN` as `INNER JOIN`, `LEFT OUTER JOIN` as `LEFT JOIN`, `FULL OUTER JOIN` as `FULL JOIN`, a
  qualifier as written, a nested query in parentheses, CTE names and column aliases quoted (`WITH "c"("x") AS (...)`,
  `FROM (...) AS "s"("x")`), a star where it stands among the select items (`*`, or `t.*` with its qualifier as
  written), and a compound's blocks joined by `UNION ALL` in upper case.

**What the binder answers today.** Of the grammar above, antb1 answers:

- FROM: tables and paths, with an alias or without, joined by commas, `CROSS JOIN`, `[INNER] JOIN ... ON` or `LEFT
  [OUTER] JOIN ... ON`, with qualified column names (`t.x`) anywhere in the query, and derived tables and `WITH` lists,
  each a sub-plan of its own. A `LEFT JOIN` is one fixed unit: it is never reordered, its `ON` reads its own two inputs
  only, an `ON` conjunct over the right input alone filters that input, and every other one is a residual of the join —
  including one that reads only the preserved side, since a left row that fails it is padded and not dropped. A `WHERE`
  conjunct over the null-supplying side stays above the join. An equality is a key only when each of its sides reads one
  relation of one input, since a side that is an expression is computed on that relation's own branch; a side over two
  of them is a residual, as in an inner `ON`; `IS NOT DISTINCT FROM` is a key on the same terms, a NULL-equal one, on
  which a NULL matches a NULL ([ADR 0037](adr/0037-tpcds-roadmap.md)'s OJ5b). A `LEFT JOIN` with no equality key
  between its two inputs is a nested loop
  (ADR 0040's OJ4c): every `ON` conjunct over both inputs a residual, and rule 11 as with a key. A `FULL JOIN` (ADR
  0037's OJ3) is such a unit too, which pads both inputs: an `ON` conjunct over either input alone is a residual, since
  a row of either that fails it is padded and not dropped; a `WHERE` conjunct over either input stays above the join;
  the `ON` of an inner join before it in FROM order still binds inside its left input, which that join is part of, while
  a `WHERE` equality there is no key of that input (`FROM a CROSS JOIN b FULL JOIN c ON ... WHERE a.k = b.k` crosses `a`
  and `b` below the `FULL JOIN`, the equality a filter above it), and a conjunct of that inner `ON` that would bind
  above every join of the block (a correlated subquery conjunct, a column of an enclosing block, a comma sibling
  before the join group) is exit code 4, since it would drop the rows the `FULL JOIN` pads on the left, while an
  uncorrelated subquery's conjunct there filters that input in a slot of its own, below the `FULL JOIN` (ADR 0041's
  SQ1); and the same `ON` rules hold as for a `LEFT JOIN`, without a key too. More than 256 relations, counting a
  derived table, a CTE reference and a subquery in an expression as one plus the relations of their body, is exit code
  4 too, before the per-item checks;
- `UNION ALL` of two or more blocks as the whole statement, a derived table's query and a CTE's query, each block
  answered as any block is, with the first block's names and DuckDB's types where every block widens exactly to them
  ([Binding](#binding)); a branch in parentheses, a block with its own `ORDER BY`, `LIMIT` and `OFFSET` or a query
  with its own `WITH` list (ADR 0039's X4a);
- `UNION` [`DISTINCT`] wherever `UNION ALL` stands, alone or next to it in any nesting: each distinct row of its
  operands, with types folded along DuckDB's tree (ADR 0039's X4b, [Binding](#binding));
- `INTERSECT` [`DISTINCT`] and `INTERSECT ALL`, binding tighter than `UNION`, in the same places (ADR 0039's X4c);
- `EXCEPT` [`DISTINCT`] and `EXCEPT ALL`, at `UNION`'s level and associating to the left (ADR 0039's X4d);
- date casts: `CAST('YYYY-MM-DD' AS DATE)` and `'YYYY-MM-DD'::DATE` are the literal `DATE 'YYYY-MM-DD'`
  wherever it may stand, as in DuckDB (which names all three `CAST('YYYY-MM-DD' AS "DATE")`);
- other casts ([ADR 0037](adr/0037-tpcds-roadmap.md)'s CT1), `CAST(x AS T)` and `x::T` to SMALLINT (`INT2`,
  `SHORT`), INTEGER (`INT`, `INT4`, `SIGNED`), BIGINT (`INT8`, `LONG`), HUGEINT (`INT128`), USMALLINT (`UINT16`),
  `DECIMAL(p,s)` or `NUMERIC(p,s)` (`DECIMAL(p)` is scale 0, a bare `DECIMAL` DECIMAL(18,3); a width outside 1 to 38
  or a scale above it is DuckDB's bind error), DOUBLE (`FLOAT8`), VARCHAR (`TEXT`, `STRING`), DATE, TIMESTAMP
  (`DATETIME`) and BOOLEAN (`BOOL`), where the cast changes no value: a value of type T is itself (a FLOAT column as
  DOUBLE is the DOUBLE antb1 reads); an exact widening (an integer type into a wider one or into a DECIMAL with
  enough integer digits, a DECIMAL into one with at least its integer digits and its scale) converts; and a constant
  that T holds exactly folds into a constant of T: an integer constant in T's range, in a DECIMAL's digits or, up to
  2^53, in a DOUBLE; a decimal literal in a DECIMAL's digits, or in an integer type when it has no fraction; a
  string literal as VARCHAR or TIMESTAMP. An integer or a DECIMAL cast to any DECIMAL (CT2) is checked as DuckDB
  checks it: rescaled, rounded half away from zero where the scale shrinks (`CAST(-1.25 AS DECIMAL(5,1))` is -1.3),
  and failing the query with DuckDB's conversion error on a value the DECIMAL cannot hold (`Could not cast value
  12345 to DECIMAL(3,1) when casting from source column b`, an execution error). An integer, a DECIMAL or a DOUBLE
  cast to any integer type (CT3) is checked the same way: a DECIMAL rounded half away from zero (`CAST(2.5 AS
  INTEGER)` is 3), a DOUBLE half to even (2.5 is 2; not to HUGEINT, whose 38 digits hold less than DuckDB's), and a
  value the type cannot hold, a DOUBLE NaN or infinity or one that rounds past the maximum (divergence D28)
  included, failing with DuckDB's error, which names the integer types by their physical names (`Type INT64 with
  value 70000 can't be cast because the value is out of range for the destination type INT16`, `Failed to cast
  decimal value 40001 to type INT16`). Every other cast is unsupported (exit code 4: `CAST of DOUBLE values to
  DECIMAL(12,2) is not supported`), a conversion between kinds that DuckDB answers or fails at run time (VARCHAR to a
  number, a number to DATE), as is a constant DuckDB fails to convert and a cast to a type antb1 has none for
  (`FLOAT`, `INTERVAL`, `VARCHAR(10)`). The
  name is DuckDB's, the type as DuckDB names it whatever alias was written: `CAST(s AS INTEGER)`, `CAST(f AS
  "DOUBLE")`, `CAST(n AS DECIMAL(12, 2))`;
- value expressions: columns, literals, aggregates, arithmetic (`+ - * / // %`, unary `-`), the functions
  `strlen(varchar)`, `regexp_replace(varchar, 'pattern', 'replacement')`,
  `substring(varchar, start[, length])`, `toDateTime(integer)`,
  `EXTRACT(field FROM timestamp)` and `date_trunc('unit', timestamp)`, `CASE` (both forms, with conditions
  as below) and `COALESCE(value, ...)` of them, in the select list, aggregate arguments (not constant ones),
  `GROUP BY` and `ORDER BY` (literals
  there are positions or constants, see [Binding](#binding));
- conditions (`WHERE`, `HAVING` and `CASE WHEN`): `operand <op> literal` in either order, `operand <op> operand`,
  `operand [NOT] LIKE 'pattern'`, `operand [NOT] IN (literal, ...)`, `operand [NOT] BETWEEN low AND high`
  (`low <= operand AND operand <= high`, each bound a literal or an operand) and `operand IS [NOT] NULL`, where an
  operand is a value
  expression that reads a column (in `HAVING`, and in a `CASE` of an aggregate query, also one over aggregates),
  combined with `AND`, `OR` and `NOT`. Instead of a literal, a comparison, a bound or an `IN` list's value may be
  a constant integer expression: integer literals under unary `-`, `+`, `-` and `*` (`a > 2 * 1000`), folded to its
  value in DuckDB's
  types (each literal `INTEGER` when its magnitude fits, else `BIGINT` or `HUGEINT` by its value, so `-2147483648`
  is a `BIGINT`; each operation in the wider type of its operands); one that overflows `INTEGER` or `BIGINT` is a
  bind error (exit code 1, divergence D17), and a `HUGEINT` value of more than 38 digits is unsupported. A quotient
  of two numbers ([ADR 0037](adr/0037-tpcds-roadmap.md)'s CT4) -- integer or decimal literals, negated or not, or
  constant integer expressions -- is the DOUBLE DuckDB computes for it, each side converted to a double as DuckDB
  converts it (an integer by the type DuckDB gives it, a HUGEINT by its own formula) and then divided (`x BETWEEN
  2.0 / 3.0 AND 3.0 / 2.0` compares with 0.6666666666666666 and 1.5); a
  zero divisor stays unsupported (DuckDB divides it into an infinity). Other constant expressions (`//`, `%`, a
  quotient inside other arithmetic, decimals under `+`, `-` and `*`) are unsupported. DuckDB compares the three
  values of a `BETWEEN` in
  one common type and antb1 each comparison in its own, so when a bound is `DOUBLE` (a `DOUBLE` column, a literal
  with an exponent or of more than 38 digits) and the operand is not, a `BETWEEN` that also has a `BIGINT` or
  `HUGEINT` value (a column, a literal or a folded constant of that type), a `FLOAT` column, a decimal literal and
  an integer value, or a DECIMAL value that antb1 would compare exactly (a DECIMAL operand with a bound that is not
  `DOUBLE`, or a DECIMAL column or expression as a bound: `price BETWEEN 1e0 AND 5`) is unsupported; one that mixes
  a `VARCHAR` value with a `DATE` or `TIMESTAMP` value is a bind error, as in DuckDB. Every unary minus over an
  integer literal is part of it (the lexical rules above), so `-(-9223372036854775808)` is a `HUGEINT`, not an
  overflow, and `-(-(-9223372036854775808))` a `BIGINT`. A plain `BETWEEN` in the `AND` chain of
  `WHERE` or `HAVING` is its two comparisons, each folded and, over a table column, pushed into the scan like any
  comparison; `NOT BETWEEN`, `NOT (a BETWEEN ...)` and `BETWEEN` under `OR` or in `CASE` are compound conditions.
  DuckDB's names apply: `(a BETWEEN 1 AND 2)`, and both negations `(NOT (a BETWEEN 1 AND 2))`;
- uncorrelated subqueries: an `IN` as a top-level `AND` conjunct of `WHERE`, of an inner join's `ON` (which counts as a
  `WHERE` conjunct, and inside a `FULL JOIN`'s left input filters that input) or of `HAVING`, and a scalar subquery's
  and an `EXISTS`'s value inside a condition (above). `IN` in its three spellings `x
  IN (query)`, `x NOT IN (query)` and `NOT (x IN (query))`: `x` is an operand as above, the query is a block of its own
  with one column, and the two share a join key type (an exact widening from each, so never a `DOUBLE`). `IN` is a semi
  join, which keeps a row once when it has a match, so it is exact with NULLs too; `NOT IN` is a null-aware anti join,
  which keeps every row of an empty set, a NULL `x` included, and otherwise only a row whose `x` is not NULL, has no
  match, and faces a set with no NULL in it. A scalar subquery, in a comparison `x <op> (query)` in either order or
  anywhere else in a condition, may have any shape: it becomes a keyless one-row join that appends the query's value,
  which the condition then reads as an operand, following every rule an operand follows — the `DECIMAL` and `DATE`
  pairs a comparison admits, its folding of a literal side and the constants it moves, and its refusal of a `FLOAT`
  column (divergence D11), which the column carries across the block boundary. A query that is an aggregate over
  every row, under projections and computed columns alone (a CTE's too), returns exactly one row and is its own
  value. Any other query is wrapped in a single value (`Aggregate
  SINGLE_VALUE` in `EXPLAIN`; [ADR 0041](adr/0041-subqueries-and-decorrelation.md)'s decision 4). It is NULL over no row
  and the row's value over one, a NULL value counting as a row. Over two or more the query fails with DuckDB's error,
  `More than one row returned by a subquery used as an expression - scalar subqueries can only return a single row`
  (exit code 1), once every row of the subquery is counted. Over no row `SUM`, `AVG`, `MIN` and `MAX` are NULL, which
  the comparison rejects, and `COUNT` is 0, which it compares as usual; both as in DuckDB. A plain `BETWEEN` that
  holds such a subquery is its two comparisons, each a conjunct of its own: one that holds a value filters in the
  block's slot (below), and the other is an ordinary conjunct (pushed into the scan in `WHERE`). All three values are
  typed by the rules of a `BETWEEN` above, a subquery as its column, and both comparisons compare in that type; a `NOT
  BETWEEN` and a `BETWEEN` under `OR` or in a `CASE` are conditions with values like any other. An `IN` list may hold
  values among its literals, each reading no column (`x IN (1, (query))`): it is the `OR` of its equalities, `NOT IN`
  that `OR`'s `NOT`, which are `IN` exactly in three-valued logic; DuckDB compares all of the list's values in one
  type, so a list whose type would be `DOUBLE` next to a `BIGINT`, `HUGEINT`, `FLOAT` or `DECIMAL` value is exit code
  4, as a `BETWEEN` of those is. A value may be `LIKE`'s operand, not its pattern. `EXISTS` and `NOT
  EXISTS` over any block are `COUNT(*)` over `LIMIT 1` of the block under a one-row join, compared with 1 or with 0
  above it, as DuckDB plans them. The block's select list is dropped, so a select item that is no aggregate and that
  nothing else reads is never computed, under a `LIMIT` too; its aggregates are computed, grouped or not (divergence
  D32), and a `DISTINCT`'s list, being its rows, stays. Its `ORDER BY` keys are computed, as DuckDB computes them,
  but in a block that aggregates without `GROUP BY`, whose one row antb1 never sorts (D32). A star in it is checked
  by name and against the block's `GROUP BY` and aggregates, a bind error as in DuckDB, and expanded only under a
  `DISTINCT`, whose rows its columns are; a `GROUP BY` or `ORDER BY` position that falls in it exits 4. A compound of
  `UNION`, `INTERSECT` and `EXCEPT` is answered as the query of either form where no arm reads an enclosing block;
  each arm expands its stars, so a column of a type antb1 cannot read exits 4 there. Each block has slots for these
  joins ([ADR 0041](adr/0041-subqueries-and-decorrelation.md)'s decision 9): one above its inner joins (inside a
  `FULL JOIN`'s left input, one above that input's joins, below the `FULL JOIN`), and one for `HAVING` above the
  aggregation and the filter of `HAVING`'s other conjuncts, below the select list's computed columns. A subquery's
  value is bound once per slot (decision 10): a subquery written alike twice in `WHERE` and the inner `ON`s is one
  value, one join, planted where it is first bound. A conjunct that holds a value is a filter of the slot after the
  joins of its values: what it computes, then its predicate, never on one relation's branch. So a value is computed
  before any row is read, whatever an `AND`, `OR` or `CASE` around it decides, and its own error (two rows, an
  overflow in its query) fails the query even where no row needs it, as in DuckDB; an expression over the value is
  computed for the rows that reach it (divergence D16). The joins and filters stand in the order they are bound --
  except that a `WHERE` semi join whose key reads one relation of the block runs on that relation's own branch, below
  those joins, which answers the same and carries fewer rows through them (ADR 0029's D0). Where a subquery's join
  turns out empty, the order of these joins decides which builds run and which errors are reported (divergence D35).
  An `IN` query may have its
  own `GROUP BY` and `HAVING`, and nothing is pulled out of it; an aggregate inside any subquery does not make the outer
  block an aggregate query. A column count other than one is a bind error. A **correlated** subquery -- one that reads a
  column of the block enclosing it -- is answered where it is a top-level `WHERE` conjunct: the binder plans it as a
  dependent join, which the optimizer's unnesting pass turns into ordinary joins ([ADR 0023](adr/0023-unnesting.md)).
  `EXISTS` and `NOT EXISTS` become a semi or anti join, keyed on the equality they correlate through (or on an `IS NOT
  DISTINCT FROM`, a NULL-equal key: an outer NULL has the inner NULLs, OJ5b), over a block of
  `FROM` and `WHERE` alone; a scalar `SUM`, `AVG`, `MIN` or `MAX` under a comparison becomes a `GROUP BY` of the inner
  equivalent and an inner join (an outer column correlated only through `IS NOT DISTINCT FROM` has that inner column as
  its equivalent, and the join keys on it NULL-equal, so an outer NULL reads the NULL group), where the select item
  combines the aggregate with literals and arithmetic alone --
  `COUNT` is exit code 4 there, since it is 0 over no rows and the rewrite gives an outer row with no group no row at
  all (divergences D23 and D24), and so is a select item that reads no aggregate, which is not NULL over no rows either
  (`SELECT 1` of a block that aggregates only in its `ORDER BY`). An equality of outer columns stays in the subquery
  when an `IS NOT DISTINCT FROM` correlates one of them (for NULLs it is NULL, not true), and a correlated `SUM(x + c)`
  keeps its form, as in DuckDB (no `COUNT`, below). Every other correlated shape is exit code 4: a block with
  `GROUP BY`, `DISTINCT`, `HAVING`, `LIMIT`, `OFFSET` or an aggregate inside `EXISTS`, a correlated `EXISTS` without a
  key, a correlated scalar subquery that is no aggregate over every row or whose outer column has no inner equivalent,
  a compound with an arm that reads an enclosing block, and at the `IN` a correlated `IN` or `NOT IN`; at the subquery
  for one in `HAVING`, whose outer references would read the outer block's groups and not its rows; and at the `IN`
  for an `IN` conjunct whose probe reads an enclosing block's column, which neither unnesting path handles. A conjunct
  of a correlated block that reads the enclosing block and holds an uncorrelated value is answered: the value's join
  stands in the block's slot, below the filter of its correlated conjuncts, and the conjunct is a residual of the
  unnesting (ADR 0041's decision 8). A correlated scalar subquery compared with an uncorrelated one, directly or in
  an expression, is answered too, the uncorrelated one a value whose join stands below the dependent join. A
  correlated value anywhere but a direct operand of a top-level comparison, two different correlated subqueries in
  one comparison, a correlated subquery in a conjunct that reads the enclosing block itself, and a correlated `EXISTS`
  anywhere but a top-level conjunct exit 4 once their blocks bind (SQ5 and SQ8 answer them). A top-level correlated
  `EXISTS` whose block has a shape the unnesting does not take is refused at the `EXISTS` once its block's FROM list
  is bound and before any other clause of the block binds, so the bind errors of those clauses never show (a
  compound's arms all bind before a correlated arm is refused). What the enclosing block binds before the subquery
  comes first: its FROM list and every `ON`, the `WHERE` conjuncts written before it, its select list, and for one in
  `HAVING` its grouping; a conjunct written after it is never reached. A scalar subquery's shape is checked once its
  block is bound, so the block's own bind errors come first. DuckDB, which answers these subqueries, binds a block's
  `WHERE` before its select list and reports the first bind error it meets, so where a refused subquery hides one, it
  exits 1 where antb1 exits 4.
  [ADR 0041](adr/0041-subqueries-and-decorrelation.md) assigns these shapes to items of ADR 0037's phases 8 and 10.
  A subquery in an inner join's `ON` sees its block's FROM items as that `ON` does (rule 10 of
  [ADR 0022](adr/0022-joins-and-query-blocks.md)): an outer name there never resolves to a later item, as in DuckDB. An
  outer reference is read only in a `WHERE` conjunct of the subquery's own block, an inner join's `ON` included, and
  only one block out; in a select list, `GROUP BY`, `ORDER BY`, `HAVING` or a `LEFT JOIN`'s or a `FULL JOIN`'s `ON`,
  further out, or in a derived table or a CTE (a lateral reference) it is exit code 4 at the reference, each with its
  own message. A star in an `EXISTS` block is checked by name but not expanded (but under an uncorrelated block's
  `DISTINCT`): a `GROUP BY` or `ORDER BY` position there counts its columns, and one that names a column of it is exit
  code 4 (one out of range is a bind error, as in DuckDB);
- any of these in parentheses (`(a)`, `SUM((a))`, `WHERE (a = 1 AND b = 2)`), which group without changing
  anything.

Every other expression (function calls other than the five aggregates and the functions above, `EXTRACT` of other
fields, a condition used as a value, as in `SELECT a = 1`, a comparison of two constants, a bare column as a
condition, `TRY_CAST` and a cast to a type antb1 has no type for, such as `CAST(a AS FLOAT)`) parses, and is then
rejected by the binder with exit code 4 at its first unsupported token, before any name is resolved. `GROUP BY ALL`
and `ORDER BY ALL` are rejected by the parser, and so are `SUM`, `AVG`, `MIN` and
`MAX` with `DISTINCT`, and a string or quoted field name in `EXTRACT` (`EXTRACT('year' FROM d)`), which DuckDB accepts.
Grouping sets ([ADR 0038](adr/0038-grouping-sets.md)) parse in every form DuckDB parses: `ROLLUP (...)` and `CUBE (...)`
over expressions and composites `(a, b)`, `GROUPING SETS (...)` over any of these, nested ones included, the empty set
`()`, and a composite as a `GROUP BY` item; `rollup` and `cube` are names unless `(` follows. All of them are answered
(below). The forms DuckDB's grammar does not have are syntax errors (exit code 1), as there: `ROLLUP ()`, `()` inside
`ROLLUP` or `CUBE`, `GROUPING SETS ()`, and a list after `GROUP BY ALL`. The parser refuses with exit code 4 a trailing
comma inside `ROLLUP`, `CUBE` or `GROUPING SETS`, which DuckDB accepts, and an operator or a cast after a composite
(`(a, b) + 1`), a row value.

`GROUP BY ROLLUP (...)` (RU3), `GROUPING SETS (...)`, a composite `(a, b)`, the empty set `()` (RU5) and `CUBE (...)`
(RU6) are answered in every form, as DuckDB expands them: of keys and composites, nested, after or between other keys,
and several of them. `GROUPING SETS (s1, ..., sn)` groups by each of its elements' sets, one after another (a nested
`ROLLUP`'s in place); `CUBE (e1, ..., en)` groups by every subset of its elements, depth first as DuckDB lists them
(`CUBE (a, b, c)`: `()`, `(a)`, `(a, b)`, `(a, b, c)`, `(a, c)`, `(b)`, `(b, c)`, `(c)`), so 16 elements are past the
limit below; a composite is one set of its keys and `()` the empty set, so `GROUP BY ()` is the ungrouped aggregate,
with its one row even over no input rows. `ROLLUP (e1, ..., en)` groups by each prefix of its elements, from `()` to all
of them; the items of the `GROUP BY` combine as a cross product, so `x, ROLLUP (a, (b, c))` groups by `(x)`, `(x, a)`
and `(x, a, b, c)`. A key is an expression, a select alias or a position, as in a plain `GROUP BY`. Each set's rows are
its own `GROUP BY`, one after another: a key the set lacks is NULL in its rows (as a NULL of the data is: `GROUPING()`
tells them apart), and the empty set gives one row even over no input rows. A set listed twice (`ROLLUP (a, a)`) gives
its rows twice. More than 65535 sets is DuckDB's bind error, "Maximum grouping set count of 65535 exceeded". A constant
in a `ROLLUP`, a `CUBE`, a `GROUPING SETS` or a composite (a constant select item named by alias or position) is refused
with exit code 4: DuckDB groups by it, NULL in the sets without it, which the select list's constant would not show. So
is a constant `GROUP BY` item next to grouping sets, which DuckDB groups by in every set: no set is then `()`, and no
input rows give no row. One divergence: antb1 groups by one key however a column is written, while DuckDB keeps two
spellings of it in one list (`t.a` and `a`, or a column and its position or alias) as two grouping expressions, each
NULL in its own sets, so that its `HAVING` can read a different one than its select list (`GROUP BY 1, ROLLUP (a)` with
`a` the first item and `HAVING a = 1`). A single set of every key (`GROUP BY (a, b)`, or `a, ()`) is a plain `GROUP BY`,
as DuckDB plans it; otherwise the plan is a `GroupingSets` node ([ADR 0038](adr/0038-grouping-sets.md)), run in two
levels where every call merges exactly. `GROUPING(k1, ..., kn)` and its alias `GROUPING_ID(...)` (RU4) are a BIGINT per
row, bit n-1-i set when `ki` is not in the row's set (so `GROUPING(a)` is 1 in a row where `a` is rolled up, and 0 over
a plain `GROUP BY`), in the select list, `HAVING`, `ORDER BY` and expressions there. Each argument must be a `GROUP BY`
key written as an expression (not an alias or a position), and DuckDB's errors are antb1's: `GROUPING` without a `GROUP
BY`, in `WHERE` or inside an aggregate, and with 64 or more arguments ("cannot have more than 64 groups"). A `GROUPING`
of a constant `GROUP BY` item, which DuckDB answers with 0, is refused with exit code 4.

Outside the grammar, the parser recognizes common SQL and rejects it with exit code 4 and a source span, among others: a
star's modifiers (`* EXCLUDE (...)`, `REPLACE`, `RENAME`, and `LIKE`, `ILIKE`, `GLOB` or `SIMILAR TO`, the first two and
`SIMILAR TO` also after `NOT`, before a string, also one in parentheses, after any `*` or `t.*`), an alias after a star
(`* AS x`, `t.* x`, `* AS ''`, which DuckDB gives every column of the star; only before `,` or `FROM`, else a syntax
error as in DuckDB), `t.*` inside an expression, `DISTINCT ON`, `ILIKE`, `GLOB`, `LIKE ... ESCAPE`, `NULL` literals (but
a whole `CASE` value: `THEN NULL`, `ELSE NULL END`, a whole select item of a `UNION ALL` block, and a side of `IS [NOT]
DISTINCT FROM`), `IS` but `IS [NOT] NULL` and `IS [NOT] DISTINCT FROM` (`IS TRUE`), `ISNULL`, `NOTNULL`, a condition as
a select item (`SELECT a IS NULL`), `AT TIME
ZONE`, `||`, window functions, unary `+`, and in casts quoted or qualified type names, type names of several words
(`DOUBLE PRECISION`, `TIMESTAMP WITH TIME ZONE`), array types, `INTERVAL` and `UNION` types and type parameters other
than integers. After `LIMIT` and `OFFSET`: expressions such as `LIMIT 1 + 1`, `LIMIT '5'` and `LIMIT (5)`, calls and
typed literals, also of quoted and qualified names of at most three parts (`LIMIT abs(5)`, `LIMIT main.abs(5)`, `LIMIT
integer '5'`, `LIMIT E'5'`), a percentage, `LIMIT ALL` and `ROW` or `ROWS` after the value of `OFFSET` (`OFFSET 5 ROWS`,
also next to a `LIMIT`, which DuckDB answers; `LIMIT 5 ROWS` is a syntax error, as in DuckDB); a column there (`LIMIT
a`, `LIMIT t.a`) and a call or a typed literal of a longer name (`LIMIT a.b.c.d(1)`) are syntax errors, and DuckDB
refuses them too, and so are an operator after a star (`* + 1`, `t.* = 1`, `* ISNULL`) and a star with no FROM (`SELECT
1, *`). Known gaps, among others: a star in parentheses (`(*)`), `ORDER BY *` and `GROUP BY *` are syntax errors, which
DuckDB answers; `LIMIT` and `OFFSET` expressions that start with `CASE`, `NOT` or a minus (`LIMIT -(-5)`) and
conditions (`LIMIT 5 = 5`, `LIMIT 5 AND 3`) are syntax errors (exit code 1), although DuckDB answers them, and so are a
trailing comma in an `IN` list (`a IN (1,)`), `IN` before a list or a list column (`a IN [1, 2]`, `a IN b`), `BETWEEN
ASYMMETRIC` (`asymmetric` is a name, divergence D21), `MAP {...}`, a prefix alias (`SELECT x: 1`, `FROM x: t`), an
exponent without digits (`1e`, which DuckDB reads as `1 AS e`), a number that a name follows directly (`1x`, which
DuckDB reads as `1 AS x`), named arguments (`round(x := 2.5)`), a slice without its lower bound (`b[:2]`; `b[1:2]` is
unsupported), a `$` inside a name (`a$b`, which antb1 reads as a parameter), a one-parameter lambda (`lambda x: x + 1`)
and a string that continues after a line break (`'a'` and `'b'` on the next line, which DuckDB reads as `'ab'`), also in
a nested query; inside the arguments of a call some of them are unsupported (exit code 4) instead. In FROM: `JOIN ...
USING`, `NATURAL` and `RIGHT` joins, `SEMI`, `ANTI`, `ASOF` and `POSITIONAL` joins, nested joins (a `JOIN` before the
`ON` of an earlier one) and joins in parentheses, `LATERAL` before a subquery or a table function (also one with a
qualified name: `LATERAL main.range(3)`), `schema.table`, `ONLY`, table functions (also those named by the 13 reserved
words that DuckDB takes as function names, `CROSS`, `FULL`, `ILIKE`, `INNER`, `IS`, `JOIN`, `LEFT`, `LIKE`, `NATURAL`,
`OUTER`, `OVER`, `RIGHT` and `SIMILAR`: `FROM t, left(1)`), the reserved words `between`, `exists`, `interval` and
`over` as qualifiers of a table or a table function, which DuckDB accepts (`FROM t, over.x`, `FROM over.f(1)`, `LATERAL
over.f(1)`), column alias lists (`t AS a(x, y)`), `PIVOT`, `UNPIVOT`, `AT (...)` (time travel), `TABLESAMPLE`, a
trailing comma, and an empty, escape or dollar-quoted string as a table alias (`AS ''`, `AS E'x'`, `AS $$x$$`, which
DuckDB accepts). Malformed SQL, such as `SELECT COUNT(*) FORM t`, is a syntax error with exit code 1, and so is a join
keyword where DuckDB has none (`SELECT 1 JOIN u`, a `JOIN` after `WHERE`) or without the rest of its join (`LEFT u`,
`FULL OUTER u`, `NATURAL LEFT u`, also before the `ON` of a `JOIN`), `LATERAL` before a table or a path (`FROM t,
LATERAL u`), a trailing comma of the FROM list before `FROM` or `INTO` (`FROM t, FROM u`), and a table or a path in
parentheses that `)`, `,`, `;` or the end follows. An unsupported construct is reported at its first token, and the
parser looks at most three tokens ahead, so a malformed FROM form that starts like an unsupported one is unsupported too
(exit code 4, where DuckDB gives a syntax error), such as a table in parentheses that anything else follows (`FROM (t
a)`, `FROM (f(1))`, and a reserved qualifier: `FROM (over.x)`), `LATERAL` in parentheses before `(`, a name or a
reserved word that DuckDB takes as a function name or a qualifier (`FROM (LATERAL t)`, `FROM (LATERAL between)`),
`LATERAL` before a qualified name without a call (`LATERAL s.t`, `LATERAL over.x`) or `NATURAL LEFT OUTER u`.

In `WITH` lists and nested queries, these are unsupported (exit code 4): `WITH RECURSIVE`, `MATERIALIZED`,
`NOT MATERIALIZED` and `USING KEY`; a string literal as a CTE name or a column alias (`WITH 'c' AS`, `s('x')`) and a
trailing comma in a column alias list (`s(x,)`), which DuckDB accepts; a query in parentheses (in FROM or as a CTE's
query) that starts with no `SELECT` or `WITH` (`(VALUES (1))`, `(FROM t)`, `(TABLE t)`, `(PIVOT ...)` and
`(UNPIVOT ...)`, also written `PIVOT_WIDER` and `PIVOT_LONGER`, in FROM also `(DESCRIBE t)`, `(SHOW t)` and
`(SUMMARIZE t)`, or `((SELECT ...))`, at its first token), also after a `WITH` list (`WITH c AS (...) FROM c`); a
statement other than a query after a `WITH` list, which DuckDB runs (`WITH c AS (...) INSERT ...`, also `UPDATE`,
`DELETE` and `MERGE`); and in
a nested query `SELECT` without `FROM` (`(SELECT 1)`, which DuckDB answers), a trailing comma in its select list, FROM
list or `GROUP BY`, and the clauses that are unsupported after a statement (`FETCH`, `QUALIFY`, ...). As elsewhere, a
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
code 1 and points at the offending name, call or literal; the first error in query order wins (the tables, then every
star's name, as DuckDB expands its stars first, then the select list, the `ON` conditions and `WHERE`, the `GROUP BY`
names, the grouping rule below, `LIMIT`, `OFFSET`, `HAVING` and the `ORDER BY` items). A column of an unsupported
type exits 4 where it is referenced, so it can come before a later bind error, which DuckDB, which reads every type,
reports instead: `SELECT *, nope FROM t` over such a column exits 4 at `*`.

- Names: table and column names match ASCII case-insensitively, quoted identifiers included (as in DuckDB). An
  unknown table or column is a bind error, and so is a name that matches two columns differing only in case. A
  column of an unsupported type fails with exit code 4 wherever it is referenced (by a `*` or `t.*` that expands
  to it too).
  Each FROM item is one binding, named by its alias, else a table's name as written, else the file name of its path
  up to the first dot (a path with a glob character is named by its whole text). An alias hides that name, and the
  error for a qualifier nothing is named by says so. Two bindings may share a name: a qualified name then resolves
  to the one that has the column, and is a bind error when both do. A qualifier nothing is named by, and a binding
  without the column, are bind errors. An `ON` resolves a name in its own join group first (the items from the last
  comma up to its `JOIN`; a `CROSS JOIN` continues a group), then in the earlier comma siblings, and never in a
  later item, whose name its error points out; `WHERE` and every other clause see every binding in one level. A
  qualified name never names a select item's alias, as in DuckDB, so `ORDER BY t.x` over `SELECT i16 AS x` is a bind
  error.
- Select list: plain columns, aggregates, constants and other expressions, and stars anywhere among them, any number
  of times. A star expands where it stands: `*` to every binding's columns, in FROM order then column order (a bind
  error when two bindings of one name share a column name), and `t.*` to the columns of the one binding of its own
  block that `t` names, matched as a qualifier is. A `t.*` is a bind error when no binding of its block is named `t`
  (also when only an enclosing block's is: a star never correlates, as in DuckDB), or when two are, whatever their
  columns; a `t` that names no binding but a STRUCT column, whose fields DuckDB expands, is unsupported. A star takes
  no alias. Without `GROUP BY`, aggregates (in the select list, in `HAVING` or in `ORDER BY`) and `HAVING` itself
  cannot be mixed with plain columns: a bind error at the first plain column in select order, a star's columns
  included (at the star, naming its first column). Constants mix with anything; with an aggregate (also one only in
  `HAVING` or `ORDER BY`) or `HAVING` the query has one row.
- Constants (as DuckDB types and names them): an integer is INTEGER when its magnitude fits (so `-2147483648` is
  BIGINT), else BIGINT or HUGEINT, and is named by its value (`007` is `7`); a string is VARCHAR named with its
  quotes (`'it''s'`); `DATE '2020-01-02'` is DATE named `CAST('2020-01-02' AS "DATE")` (so are
  `CAST('2020-01-02' AS DATE)` and `'2020-01-02'::DATE`), and
  `TIMESTAMP '2020-01-02 10:00:00'` TIMESTAMP named `CAST('2020-01-02 10:00:00' AS TIMESTAMP)`; a decimal is
  DECIMAL(digits, digits after the point), leading zeros included and the sign not, named by its value (`2.5` is
  DECIMAL(2,1), `.125` DECIMAL(3,3), `007.50` DECIMAL(5,2) named `7.50`; [ADR 0021](adr/0021-decimal-semantics.md)
  rule 3). A number DuckDB types as DOUBLE (an exponent, or more than 38 digits) and an integer beyond HUGEINT's 38
  digits are unsupported (exit code 4).
- Positions: in `GROUP BY` and `ORDER BY` an integer literal names the select item at that position (1-based; a star
  counts every column it expands to, wherever it stands); one out of range, a negative one too, is a bind error.
  `GROUP BY` of an aggregate item is a bind error. Any other literal is a constant: in `GROUP BY` it is no key but
  still makes the query grouped (one group when there are rows, none otherwise), in `ORDER BY` a number or string is
  a bind error (it would order nothing, as in DuckDB) and a DATE literal orders nothing. A position naming a constant
  item groups or orders nothing.
- `GROUP BY`: each name is a table column, or else the alias of a plain column of the select list (as in DuckDB; an
  alias of an aggregate is a bind error, an alias of a constant is no key); a key repeated, also in another spelling,
  is one key, and a key need not be selected. Every plain column of the select list, and every column a star expands
  to, must be a key: otherwise a bind error at that column (at its star)
  (`column 'b' must appear in the GROUP BY clause or be inside an aggregate function`).
- Result types: `COUNT(*)`, `COUNT(col)` and `COUNT(DISTINCT col)` are BIGINT; `SUM` of an integer column is
  HUGEINT and of a DOUBLE column DOUBLE; `AVG` of a number is DOUBLE and of a DATE or TIMESTAMP TIMESTAMP, as in
  DuckDB; `MIN` and `MAX` have the type of their column; the variance family is DOUBLE. `SUM` of a VARCHAR, DATE or
  TIMESTAMP and `AVG` of a VARCHAR are bind errors.
- Result names follow DuckDB: a plain column is named as declared in the table (`SELECT regionid` gives
  `RegionID`); an aggregate is named `count_star()`, `count(x)`, `count(DISTINCT x)`, `sum(x)`, `avg(x)`, `min(x)`
  or `max(x)`, with the argument as written in the query, double-quoted when it is not a plain identifier or is a
  reserved word (`sum("from")`). A qualifier inside an expression's name stays as written, each part quoted by that
  same rule (`sum(t.i16)`, `sum(t."Mixed Case")`), while a plain column select item keeps its declared name alone
  (`SELECT t.i16` is `i16`); a message that echoes a reference back prints it as written and unquoted, so a grouping
  error over `t.label` names `t.label`. A star's columns keep their declared names, so a name can repeat at the top
  level (`SELECT *, *`); a derived table's or a CTE's names are made unique (`a`, `a_1`) after its column alias
  list, as in DuckDB. Constants: see above. An alias replaces the name.
- `ORDER BY`: a name is the alias of a select item first, in any case, the last item with it winning, as in DuckDB
  (also when a table column has the same name). Like DuckDB, antb1 counts a plain column written otherwise than its
  column is declared as an alias of the declared name: over `SELECT x AS k, K`, `ORDER BY k` orders by the column
  `k`. A star's columns and the columns written as declared have no alias. Else the name is a table column; an
  aggregate call is computed like a select-list aggregate. DuckDB has a step between the two: a name that exactly
  one un-aliased column item carries as declared, a star's column included, orders by that item. Where the table
  columns alone make the name ambiguous (`SELECT t.*, u.y FROM t JOIN u ON t.k = u.k ORDER BY k`), antb1 refuses it
  (exit code 4) until a later PR answers it; a name both find ambiguous stays a bind error.
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
  `/` is always DOUBLE; unary `-` keeps the type. A DATE plus or minus a number of days, or days plus a DATE
  ([ADR 0037](adr/0037-tpcds-roadmap.md)'s CT5), is a DATE, the days a SMALLINT, USMALLINT or INTEGER (`d + 7`);
  another integer type, a DECIMAL or a DOUBLE as the days, and days minus a DATE, are DuckDB's bind error (`No
  function matches the given name and argument types '+(DATE, BIGINT)'`); an infinite DATE stays itself, and a
  result that leaves INTEGER or lands on one of DuckDB's infinities fails the query (`Date out of range`). Other
  DATE arithmetic (a DATE minus a DATE) and TIMESTAMP arithmetic, negating a
  USMALLINT (DuckDB wraps it), `//` and `%` of HUGEINT values and arithmetic on FLOAT columns (divergence D11) are
  unsupported; arithmetic on VARCHAR is a bind error. The result name is DuckDB's: `(a + 1)`, `-(a)`, `sum((a + 1))`,
  with columns as written and decimal
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
  'replacement')` takes a VARCHAR and two string literals and is VARCHAR; `substring(x, start[, length])` and its
  alias `substr` take a VARCHAR and one or two integers and are VARCHAR, with the SQL-standard spelling
  `substring(x FROM start FOR length)` accepted for `substring` alone (the two clauses in either order, at most once
  each) and read as the comma form, which is also its canonical text and its result name. `SUBSTRING(x FOR n)`
  without `FROM` is unsupported (exit code 4): DuckDB reads it as `substring(x, 1, CAST(n AS INTEGER))`, and that
  cast both narrows the length to `INTEGER` -- a length the comma form answers is a conversion error there -- and
  shows in the result name, while antb1's subset casts nothing to `INTEGER`.
  A wrong number of arguments, another type or a non-literal pattern or replacement is a bind error
  (`strlen() needs a VARCHAR, but 'i16' is SMALLINT`); DuckDB's
  optional fourth argument (options), `\Q` in a pattern, `\8` and `\9` in a replacement and any other function are
  unsupported (exit code 4). The result name is
  DuckDB's: `strlen(URL)`, `regexp_replace(URL, '^(.)', '\1')`, and -- because DuckDB reserves the word --
  `main."substring"(URL, 1, 2)` where `substr(URL, 1, 2)` is plain. Unlike DuckDB, antb1 casts no argument
  implicitly, so a VARCHAR position that parses as a number (`substring(s, '2', 2)`, which DuckDB answers) is a bind
  error; for every other value and position type DuckDB is a binder error as well.
- `round(x)` and `round(x, n)` of a DOUBLE ([ADR 0037](adr/0037-tpcds-roadmap.md)'s FN2), n a constant integer
  expression of at least 0: a DOUBLE, as DuckDB rounds a DOUBLE -- half away from zero, and at n digits
  `round(x * 10^n) / 10^n` computed in double (so `round(2.675, 2)` of the DOUBLE nearest 2.675 is 2.68, its product
  being 267.5, while that of 1.005 is 1.0); the value itself where the result is not finite. Named as DuckDB names it
  (`round(f, 2)`). DuckDB's overloads for a DECIMAL (which give a DECIMAL) and for an integer type, a FLOAT column
  (D11) and a negative or computed precision are unsupported (exit code 4).
- `abs(x)` ([ADR 0037](adr/0037-tpcds-roadmap.md)'s FN3): the magnitude of a number, in its own type, as DuckDB's
  abs keeps it -- an integer type (`abs(-7)` is INTEGER), a DECIMAL, a DOUBLE (`fabs`: -0.0 is 0.0, NaN stays NaN);
  an integer type's minimum fails the query with DuckDB's `Overflow on abs(-32768)` (an execution error). A FLOAT
  column is unsupported (D11), a VARCHAR a bind error. Named `abs(x)`.
- `upper(s)` and `lower(s)` of a VARCHAR, and DuckDB's aliases `ucase(s)` and `lcase(s)`
  ([ADR 0037](adr/0037-tpcds-roadmap.md)'s FN4): a VARCHAR in which each codepoint is mapped as DuckDB maps it, by
  Unicode's simple case mapping in the version of DuckDB's bundled utf8proc, one codepoint to one codepoint (`ß`
  upper-cases to `ẞ`, never to `SS`; the Kelvin sign lower-cases to `k`). antb1 carries DuckDB's table
  (`src/exec/unicode_case.inc`, written by `tools/unicode/case_tables.py`), since the utf8proc Arrow links maps over
  a hundred codepoints differently; a test compares every codepoint with DuckDB. A value that is not UTF-8 stays as
  it is (D15). Any other argument type or count is a bind error. Named as written (`ucase(u)`).
- `concat(a, b, ...)` of one or more VARCHAR values ([ADR 0037](adr/0037-tpcds-roadmap.md)'s FN5): a VARCHAR, the
  values joined in order as DuckDB joins them, a NULL value skipped, so the result is never NULL (`concat(s)` of a
  NULL `s` is `''`). Named as written (`concat('<', s, '>')`). DuckDB casts a value of any other type to VARCHAR; in
  antb1 such a value is unsupported (exit code 4), the first one in the call reported, as is the `||` operator.
  `concat()` is a bind error, as in DuckDB.
- `COALESCE(a, b, ..., z)` ([ADR 0037](adr/0037-tpcds-roadmap.md)'s NL5): the first argument that is not NULL, NULL
  when every one is; it is `CASE WHEN a IS NOT NULL THEN a WHEN b IS NOT NULL THEN b ... ELSE z END`, so each
  argument is computed only for the rows no earlier one answered. Its arguments are typed as `CASE`'s values, folded
  from the first through the rest in written order (`COALESCE(s16, 7)` is SMALLINT; with a DECIMAL the order can
  change the type, `COALESCE(u16, s16, rate)` DECIMAL(13,3) where `COALESCE(rate, u16, s16)` is DECIMAL(8,3)), with
  `CASE`'s refusals naming COALESCE (`cannot mix values of type VARCHAR and SMALLINT in COALESCE`, a string literal
  next to a number unsupported) and its divergence D20 for some orders of literals without a DECIMAL. DuckDB's name:
  `COALESCE(x, 0)`, in upper case however it is written. A NULL argument is an unsupported NULL literal, and the
  quoted `"coalesce"(...)` an unsupported function (DuckDB has no function of that name: COALESCE is an operator).
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
  `CASE WHEN x = v THEN ..`. `ELSE NULL` is a `CASE` without `ELSE`, in DuckDB too: the same NULLs, type and name.
  `THEN NULL` (a whole value, before `WHEN`, `ELSE` or `END`; `THEN (NULL)` and `THEN NULL + 1` are unsupported NULL
  literals) is NULL for the rows its branch takes; it leaves the type of the other values as it is, and a literal its
  own type, as the NULL of a missing `ELSE` does (`CASE WHEN .. THEN NULL WHEN .. THEN s16 WHEN .. THEN rate ELSE 7
  END` is DECIMAL(13,3)). A `CASE` whose values are all NULL has DuckDB's NULL type, which antb1 does not support
  (exit code 4).
  The result name is DuckDB's: `CASE  WHEN ((a = 1)) THEN (b) ELSE NULL END`.
- `UNION ALL` ([ADR 0036](adr/0036-union-all.md)): each block binds as a query block of its own, in written order,
  against what the `UNION ALL` sees -- its `WITH` list, which is every block's, and for a derived table the FROM items
  before it, where a name only they have is a lateral reference (exit code 4) -- and never sees another block's
  names. As in DuckDB, every block binds before the column counts are compared, and a count that differs is a bind
  error at the `UNION ALL` before the first block that differs (`Set operations can only apply to expressions with
  the same number of result columns`); the types come after the counts. The result names are the first block's, made
  unique for a derived table's or a CTE's binding as any select list's are (rule 7) and kept as they are at the top
  level. Each column's type is a left fold over the blocks' types in written order, as DuckDB folds it: a literal is
  typed by itself (`1` next to a SMALLINT column is INTEGER, unlike in `CASE`), equal types stay, two integer types
  give `CASE`'s type (USMALLINT with SMALLINT: INTEGER), and a DECIMAL with a DECIMAL or an integer gives `CASE`'s
  DECIMAL: two DECIMALs DECIMAL(e + s, s), with e the most integer digits and s the larger scale, or DECIMAL(38,
  38 - e) beyond 38 digits; an integer keeps the DECIMAL's scale and widens it to its own digits (SMALLINT and
  USMALLINT 5, INTEGER 10, BIGINT 19, HUGEINT 38), DECIMAL(min(38, digits + s), s). So the order of the blocks can
  change the type: USMALLINT, SMALLINT and DECIMAL(9,2) make DECIMAL(12,2), but SMALLINT, DECIMAL(9,2) and USMALLINT
  DECIMAL(9,2); and BIGINT with DECIMAL(38,30) is DECIMAL(38,30), to which the BIGINT does not widen exactly. A
  parenthesized branch (ADR 0039's X4a): a run of `UNION ALL` in parentheses is spliced into the run around it, its
  blocks folded in the same written-order list (DECIMAL(9,2), then SMALLINT and USMALLINT in parentheses, is
  DECIMAL(9,2)), while one with a `WITH` list of its own, and a parenthesized block with its own `ORDER BY`, `LIMIT`
  or `OFFSET`, is typed on its own first and folds as one branch (the same blocks with a `WITH` list in the
  parentheses: INTEGER, then DECIMAL(12,2)); a count that differs is reported at the operator before the branch, inside
  the parentheses too, and the names are the first block's, through them. A nested compound's column of NULL items is
  still unsupported (exit code 4).
  FLOAT with FLOAT is FLOAT and FLOAT with DOUBLE is DOUBLE: a column of FLOAT values alone keeps comparing with
  literals at FLOAT precision, one with a DOUBLE block does not (divergence D11). Every block's values must widen
  exactly to the column's type, which a block of another type is cast to once, below the union; values that do not,
  such as a DECIMAL past 38 digits (which DuckDB rounds) or a HUGEINT next to a DECIMAL with a scale, are unsupported
  (exit code 4), and so is every other mix -- VARCHAR, DATE or TIMESTAMP with another type, a DOUBLE with an integer
  or a DECIMAL, a FLOAT with an integer or a DECIMAL (D11) -- which DuckDB answers by casting, failing per row where a
  cast fails. Such a refusal points at the first select item in the query whose type does not fold or widen, or at
  the `*` or `t.*` it expands from (`UNION ALL of INTEGER and VARCHAR values is not supported`); a star's columns
  count in the column counts. A block's select item may be `NULL` or `NULL AS name` ([ADR
  0037](adr/0037-tpcds-roadmap.md)'s NL3): the fold skips it, in any block, so a VARCHAR, DATE or any other type with
  NULLs keeps its type, and the block computes a NULL of the column's type below the union; the name is the first
  block's (`NULL` without an alias). A column that is NULL in every block has DuckDB's NULL type, which antb1 does not
  support (exit code 4), and a NULL item outside a `UNION ALL` block, an implicit alias (`NULL x`) and NULL in an
  expression, its alias inside `HAVING` included, stay unsupported NULL literals. A `WHERE` above the union rules out
  a block whose column there is NULL, as for the other constants of a block.
- `UNION` [`DISTINCT`] ([ADR 0039](adr/0039-set-operations.md)'s X4b): its leaves bind as a `UNION ALL`'s blocks do,
  and it gives each distinct row of its operands once, rows compared as `GROUP BY` keys (NULL equals NULL, `-0.0`
  equals `0.0`, every NaN equals every other), in no particular order. A compound of `UNION` and `UNION ALL` binds
  along DuckDB's tree (see Grammar): a run of `UNION`, or of `UNION ALL`, is one list, spliced through parentheses,
  and a run next to the other kind is a binary node. Each column's type folds along that tree, a run's operands in
  written order and every edge widening exactly, so parentheses can change the type: USMALLINT `UNION ALL`
  (SMALLINT `UNION` DECIMAL(9,2)) is DECIMAL(9,2), where USMALLINT `UNION ALL` SMALLINT `UNION` DECIMAL(9,2) is
  DECIMAL(12,2). A NULL item is the fold's identity, as in a `UNION ALL`. The column counts are compared as DuckDB
  compares them: a run of `UNION` after all its leaves bind, a binary node right after its two operands. A type
  refusal names the run's operator (`UNION of VARCHAR and SMALLINT values is not supported`); a compound's own
  `ORDER BY`, `LIMIT` and `OFFSET`, and a subquery over a compound, name its root operator (`ORDER BY on a UNION is not
  supported`). Each leaf is cast once, straight to the root's type, below the compound, and each maximal region of
  `UNION` is one `SetOperation` node (Logical plans), a run of `UNION ALL` a `UnionAll`. A `WHERE` condition above a
  `UNION` stays above it (divergence D27); the value a `UNION` keeps of `-0.0` and `0.0` is the first seen in written
  order (D29), and of several failing blocks the first in written order fails the query (D30).
- `INTERSECT` [`DISTINCT`] and `INTERSECT ALL` ([ADR 0039](adr/0039-set-operations.md)'s X4c) bind as `UNION`
  does, and tighter: `a UNION b INTERSECT c` is `a UNION (b INTERSECT c)`, in one `SetOperation` node whose formula
  keeps the precedence. A run of `INTERSECT` is DuckDB's binary nodes, `((a INTERSECT b) INTERSECT c)`: its column
  counts are compared right after each operand, its types fold pair by pair, and each step's two sides must widen
  exactly to that step's type (`INTERSECT of DECIMAL(38,0) and DECIMAL(15,2) values is not supported`). So SMALLINT
  `UNION ALL` USMALLINT `INTERSECT` DECIMAL(9,2) is DECIMAL(9,2): the `INTERSECT` folds first. An `INTERSECT ALL`
  root prints each row's normalized value; below a DISTINCT root it keeps the value first seen in its left operand
  (D29). Both operands are read in full, the left one first (D30, D31).
- `EXCEPT` [`DISTINCT`] and `EXCEPT ALL` ([ADR 0039](adr/0039-set-operations.md)'s X4d) share `UNION`'s level and
  associate to the left: `a EXCEPT b UNION c` is `(a EXCEPT b) UNION c`, and `a EXCEPT b INTERSECT c` is
  `a EXCEPT (b INTERSECT c)`. A run of `EXCEPT` is binary, as a run of `INTERSECT` is, for its column counts and its
  types. Only the left operand's rows can reach the result: a block in a later operand is read in full but only looked
  up. An `EXCEPT ALL` root prints each row's normalized value; below a DISTINCT root a row keeps the value first seen
  among the blocks that can bring rows in, even one whose own row an `EXCEPT` removed (D29). Every operand is read, the
  left one first, even when the left one is empty (D30).
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
directly above the aggregation the `HAVING` one), `Compute` (appends one computed column per expression: over the
table for `WHERE` operands, aggregate arguments and `GROUP BY` expressions, and over the aggregation for `HAVING`
operands, below its `Filter`, then for select and `ORDER BY` expressions, above it), `Project` (the select list's
columns, every star expanded where it stands), `Aggregate` (the aggregates, one output row; `SINGLE_VALUE`, which no
query writes, is a scalar subquery's single value) or
`GroupAggregate` (`GROUP BY`: the keys, then the aggregates, one row per group; a
`Project` above it restores the select order), `Sort` (`ORDER BY`, below the `Project`, so it can use columns and
aggregates the query does not return) and `Limit` (with its offset). A `Join` has two inputs ([ADR
0022](adr/0022-joins-and-query-blocks.md)): its kind (inner, left, semi, anti, null-aware anti, one-row or full), key
pairs of one type each, residual conditions over both inputs and the input it builds on; what each kind does with its
rows is one table, `plan::JoinKindTraits`. A full join (ADR 0037's OJ2) gives every match, each left row without one
padded with NULLs and each right row without one padded with NULLs; `FULL OUTER JOIN` binds to it (OJ3). An inner, left
or full join may have no key pairs: a nested loop ([ADR 0040](adr/0040-nested-loop-joins.md)), every right row a
candidate of every left row and its residuals deciding, which no SQL binds to before ADR 0037's OJ4b. A `UnionAll` has
two or more, or one once the optimizer has ruled the others out ([ADR 0036](adr/0036-union-all.md)): every row of its
first input, then of the next, and so on; its columns are new, of the types the binder folded, and each input gives them
its own columns, a block of another type cast below the union by a `Compute`. A `SetOperation` ([ADR
0039](adr/0039-set-operations.md)) answers one tree of UNION, INTERSECT and EXCEPT, ALL or not, over one or more inputs,
every column a key, the tree a formula over its inputs; the binder makes one per maximal region of `UNION`, `INTERSECT`
[`ALL`] and `EXCEPT` [`ALL`] (X4b to X4d), its inputs the region's blocks and `UnionAll` subtrees in written order, and
EXPLAIN prints its formula with the tree's precedence (`SetOperation inputs=3 formula=(0 UNION (1 INTERSECT 2))
columns=[a]`). A query produces inner `Join`s: the binder factors each element of `WHERE` and of an inner `ON` (below,
below) and sends every conjunct that leaves it to the relations it reads, so one relation's conjuncts filter that
relation's own branch, a cross-relation equality whose two sides share one key type is a key of their join (each side
cast to that type on its own branch, an exact widening, and an equality spelled twice is one key), and anything else
over two or more relations is the residual of the first join that has all of them. A join graph no key connects is a
cross product ([ADR 0040](adr/0040-nested-loop-joins.md)). A relation of exactly one row, a derived table or a CTE
reference whose query is an aggregate without `GROUP BY`, `HAVING`, `LIMIT` or `OFFSET`, that no key, no residual and no
conjunct of its own branch reads (one could leave it with no row), joins above the others, in FROM order, as a
`ONE-ROW` join that appends its row to every row. The others form parts that keys connect; each part joins on its own,
as below, keyed joins only, and the parts are then crossed left-deep: the part with the most rows first (an unknown
count as the most), the probe that streams, then the others from the fewest rows up, each the build of its cross step,
an inner `Join` without keys (a nested loop: every build row is a candidate of every probe row, and the residuals
decide). A residual between parts joins at the first cross step that has all its relations. A cross never comes inside
a part, where the estimates cannot tell it from a keyed join. A subquery conjunct, or a conjunct that holds a
subquery's value, that reads one stands in the block's slot above that join, as above every join of its block: a
value is a `Join ONE-ROW` there, and the conjunct that holds it a `Compute` of what it computes and a `Filter` above.
The relations then join in the order and tree shape a cost search chooses from footer metadata only (row counts, and
each key's domain from the parts' integer ranges capped at
their non-NULL rows, else their distinct-count hints; "Join order and build sides" under Semantics): every edge between
the two inputs of a join is a key of it, so the edge that closes a cycle becomes a second key, and a join builds on its
right input unless that input is estimated at more than `--build-side-ratio` times its left. A block of more than 12
relations, or any block under `--join-order greedy`, joins left-deep instead: the relation with the most rows (an
unknown count counts as the most) is probed first, then the connected relation with the smallest estimate joins next.
Because the order reads metadata alone, a plan does not depend on the thread count. The executor runs every kind as a
hash join: it builds a hash table of the input it builds on before it reads the other input, which then streams through
it in its rows' order.

- An inner join keeps each row's matches in the build input's order; its residual conditions are evaluated in order,
  each on the rows the ones before it kept.
- A semi join keeps each row with a match, once; an anti join each row without one (a NULL key never matches, but on a
  NULL-equal key, `IS NOT DISTINCT FROM`, where it matches a NULL); a
  null-aware anti join (SQL's `NOT IN`) each row without one whose key is not NULL, every row when its build input is
  empty, and none when its build input has a NULL key, though it still reads the other input to its end.
- A left join keeps each row with each of its matches, in the build input's order, and pads a row without one once
  with NULLs of the right input's types (a NULL key included).
- A full join gives a left join's rows, then each build row that no row matched, the NULL-key ones included, padded
  once with NULLs of the left input's types, after the whole probe.
- The residual conditions of a semi, anti or left join decide which candidates (the build rows with a row's key) are
  matches. They are evaluated in the order written, each on the candidates the ones before it passed (NULL counts as
  false), on every candidate of every row, without stopping at a row's first match, so whether an overflow fails the
  query does not depend on the batch size (unless a `LIMIT` above stops the join before it reads every row). The one
  exception gives the same answers and errors: a semi or anti join whose residuals read no column of the build but
  its keys meets only a row's first candidate, since every candidate of the row would give them the same values
  ([ADR 0022](adr/0022-joins-and-query-blocks.md)'s E2c). A row without candidates never meets them: an anti join
  keeps it, a left join pads it. A left join pads a row none of whose candidates passes right after its last
  candidate.
- A one-row join appends the single row of its right input, an aggregate without groups, to every row; any other row
  count is a planner bug, an internal error (exit code 70; [ADR 0041](adr/0041-subqueries-and-decorrelation.md)'s
  decision 23).
- When an inner or a semi join's build holds no row, the other input is never read.

OR factoring, before that classification: in each element of `WHERE` and of an inner `ON`, a conjunct whose root is
an `OR` gives up the conjuncts every one of its branches shares, because `(A AND X) OR (A AND Y)` is `A AND (X OR Y)`
in three-valued logic too. The shared conjuncts are then classified like any other, so a shared equality becomes a
join key — a condition that connects a join graph only through an `OR` is the reason for the rewrite — and a shared
single-relation conjunct reaches its relation's branch, while what is left of the branches stays one conjunct, a
residual when it reads more than one relation. When a branch keeps nothing, the shared conjuncts imply the whole
`OR`, which is then dropped (it is still bound, so a bind error inside it is still an error; a subquery in it is
bound for its errors but planted nowhere, so its own row error never shows: divergence D35). Its limits are
deliberate: one level only, so a branch's own nested `OR` is a conjunct of that branch and is not descended into, a
`NOT` is one opaque conjunct and a plain `BETWEEN` is one conjunct; matching is structural, so `a.k = b.k` in one
branch and `b.k = a.k` in another are not the same conjunct and nothing is shared; every branch must share it, two
of three are not enough; and `HAVING` is not factored. Hoisting a conjunct also changes when it is computed, which
divergence D16 covers.

The same rewrite derives the `IN` lists an `OR` implies ([ADR 0026](adr/0026-join-performance.md)): where every
branch bounds one column to literals — by `=` or by `IN`, with a literal on either side — the union of those literals
holds for every row the `OR` keeps, so `(a = 'X' AND b = 'Y') OR (a = 'Y' AND b = 'X')` also contributes
`a IN ('X', 'Y')` and `b IN ('Y', 'X')`. The derived conjunct is redundant, the `OR` staying where it is, so it
cannot change an answer; what it changes is the plan, since a single-column `IN` reaches that column's scan and its
row-count estimate where an `OR` over two relations is a join residual neither can see. Nothing is derived unless
*every* branch bounds the column: a branch that leaves it free, or bounds it by a range, a `<>`, a `NOT IN` or
another column, derives nothing for it, and literals of different kinds across branches derive nothing (the `IN`
would be a bind error on a query that binds). A single value for every branch is the shared-conjunct case above.
Because a derived conjunct can reach a scan, it takes rows away from the conditions above it, which **narrows**
divergence D16: `i16 * 2 > 1 AND (id = 6 OR id = 11)` used to overflow and now answers, agreeing with DuckDB.

A rule optimizer then rewrites the plan, through both inputs of every `Join` and every input of a `UnionAll` (a
`WHERE` condition is the one thing a rule moves through a `UnionAll`: a limit or a join's key filter above it stays
above it):

- a `WHERE` condition on a `UNION ALL` (a derived table's or a CTE's) goes into each of its blocks, read in the block's
  own columns ([ADR 0036](adr/0036-union-all.md)'s X7a); a condition the binder computes, a `NOT` or an `OR` say, stays
  above the union, though the `IN` list an `OR` implies (above) goes into the blocks. Where the block's select item is a
  constant -- a literal, through the exact widening that casts it to the union's type -- the constant decides the
  condition for the whole block: a block it makes never true is ruled out and never runs, and a condition it makes
  always true is dropped from that block. It decides a comparison or an `[NOT] IN` list whose constants have the item's
  type (numbers by value at any scale, DATE, TIMESTAMP, and VARCHAR by unsigned bytes) and a comparison of two constant
  items; never a DOUBLE or a `LIKE`. Any other condition filters the block's rows on top of the block, above its select
  list (divergence D27). A union left with one block stays a `UnionAll` of one input, and one left with none keeps its
  first block under a `FALSE` filter, which reads nothing. Each reference of a CTE keeps the blocks its own conditions
  leave;
- `Limit` moves below `Project`: a `Project` keeps every row, so the `Limit` copies only the rows it keeps and ends
  up right above a `Sort`, which the executor runs as a top-N (it keeps only `limit + offset` rows while it reads);
- a `GROUP BY` key computed only from other keys, none of them DOUBLE (`GROUP BY x, x + 1`), is dropped from the
  `GroupAggregate` and computed once per group in a `Compute` above it, with a `Project` restoring the output: the
  same groups with fewer keys to hash ([ADR 0018](adr/0018-dependent-group-keys.md)). Not under a `LIMIT` without
  `ORDER BY`, which reads only some groups: every group's keys are computed, so an overflow in any of them still
  fails the query;
- projection pruning: `Scan` reads only the columns that the nodes above it use (none for a bare `COUNT(*)`); each
  input of a `Join` keeps the columns used above it and the join's key and residual columns; a `UnionAll` keeps every
  column, read above it or not, and each of its inputs the columns it gives the union;
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

A `Join` line shows its kind (`INNER`, `LEFT`, `SEMI`, `ANTI`, `NULL-AWARE ANTI`, `ONE-ROW` or `FULL`), the input it builds
on, its key pairs (`a.k = b.k`, or `a.k IS NOT DISTINCT FROM b.k` for a NULL-equal one, OJ5b; `keys=[]` for a
nested loop, ADR 0040, which then has its residuals alone) and, when it has any,
its residual conditions; its left input follows, then its right input, both
indented. A column whose binding the plan records is shown qualified, as `a.k`. It ends with `carries N of M unread
above` where the join's output holds columns no node above it reads -- its own keys and residual columns, usually,
which its inputs must carry to it and no further (ADR 0026's C8a, and the `cols_dropped` metric below is what a build
did about it). The clause is absent where there are none, and on a plan rendered before `Optimize` has pruned it,
which records no such set:

```text
Join INNER build=right keys=[a.k = b.k] residual=[(a.x < b.y)] carries 2 of 4 unread above
  Scan table=a source=parquet(files=1, rows=1000) columns=[k, x]
  Scan table=b source=parquet(files=1, rows=10) columns=[k, y]
```

A `UnionAll` line shows the number of its inputs and its columns, named as the first block names them -- the first
block left, once a condition has ruled blocks out; its inputs follow in written order, indented. A CTE whose query is
a `UNION ALL`, referenced twice with the same conditions, prints `[shared #1 of 2, ...]` on the highest node the two
references repeat, since [ADR 0030](adr/0030-shared-sub-plans.md) finds the repeat: their `UnionAll` lines when every
condition went into the blocks and a block is left, else the `Filter` above the union (the `FALSE` when no block is
left, or a condition the binder computes). The executor never holds a `UNION ALL`'s rows all the same: each reference
computes its own (`--physical` shows no shared source). A `SetOperation` line shows the number of its inputs, its
formula over them in written order (`0 UNION 1 UNION 2`, parenthesized where a term nests) and its columns, named as
the first block names them; nothing below it is shared, and no result topped by one is.

```text
Output: x:INTEGER n:BIGINT
Project x, n
  GroupAggregate keys=[x] COUNT(*)
    UnionAll inputs=2 columns=[x]
      Compute CAST(x AS INTEGER)
        Project i16
          Filter id < 10
            Scan table=e source=parquet(files=1, rows=12) columns=[id, i16]
      Project i32
        Scan table=e source=parquet(files=1, rows=12) columns=[i32]
```

A condition on the union goes into its blocks (above). Over three blocks tagged `'small'`, `'large'` and `'wide'`,
`WHERE tag <> 'small' AND x > 5` rules the first block out, drops `tag <> 'small'` from the other two and puts
`x > 5` on top of each, above the cast:

```text
Output: x:BIGINT tag:VARCHAR
Project x, tag
  UnionAll inputs=2 columns=[i32, "'large'"]
    Filter i32 > 5
      Compute CAST(i32 AS BIGINT)
        Project i32, 'large'
          Scan table=e source=parquet(files=1, rows=12) columns=[i32]
    Filter i64 > 5
      Project i64, 'wide'
        Scan table=e source=parquet(files=1, rows=12) columns=[i64]
```

### `explain --physical`

`antb1 explain --physical` prints the physical plan the query would run, without running it: the operator tree
`--analyze` profiles, with none of what a run measures. It takes `--threads`, `--memory-limit`,
`--build-side-ratio` and `--join-order`, and cannot be given together with `--analyze`; its errors and exit codes
are those of `antb1 query`.

It shows what no logical plan holds, because the physical planner is what decides it:

- part pipelines and the operators that consume them (`PartUnion`, `PartAggregate`, `PartGroupAggregate`,
  `PartGroupingSets`, `PartTopN`), and each hash join split into its `HashJoin` probe and the `HashBuild` under the
  operator that prepares it, laid out exactly as `--analyze` lays them out; no part pipeline goes through a `UnionAll`,
  so each of its inputs runs its own parts and what reads the union runs on one thread (ADR 0036);
- a `Scan` that applies predicates of the `Filter` above it while it reads: `, N pushed predicates`
  ([ADR 0020](adr/0020-filter-pushdown.md));
- a `Scan` a dynamic join filter could be applied to ([ADR 0029](adr/0029-dynamic-join-filters.md)):
  `, key filter candidate on <column>` where that filter could reject rows as the scan decodes them, and
  `, part skip candidate on <column>` where it could only rule out a part whole, because the table has no
  filtered decoder for the fields this scan reads. Pushed predicates no longer stand in the way: a scan applies
  the conjunction of them and the key filter. Neither annotation appears for a key no build can publish a filter
  for, which is any key but a single integer or DATE one. **Several columns, as `<column> or <column>`,** where
  more than one join of the pipeline probes this scan: every join above it has a filter to offer, a run takes the
  innermost that published one worth reading, and these are listed in that order.

```text
Output: count_star():BIGINT
PartAggregate Aggregate COUNT(*)
  HashJoin Join INNER build=right keys=[trips.tr_pickup = zones.zn_id] carries 4 of 5 unread above
    Filter trips.tr_row >= 100
      Scan table=trips source=parquet(files=1, rows=3000) columns=[tr_row, tr_pickup], 1 pushed predicate, key filter candidate on tr_pickup
  HashBuild Join INNER build=right keys=[trips.tr_pickup = zones.zn_id] carries 4 of 5 unread above
    Scan table=zones source=parquet(files=1, rows=40) columns=[zn_id]
```

**A candidate is not a fact, and the word is load-bearing.** Four of the conditions behind a key filter are not
properties of a plan at all: whether the build really published a filter (it makes one as it finishes, and its keys
may not fit the budget), whether it stayed small enough against the probe for the filter to be worth its cost,
**which layout the build chose** -- a build that came out in the direct layout takes no per-row filter at the
innermost join, because its probe is two array loads and the filter costs more than they do, and takes one from a
join further up only where it covers a small enough share of its own key range to reject nearly every probe row --
and whether a part's statistics exclude every published key. It is also why a scan under several joins names
several columns: which of them a run reads follows from those conditions, so a plan naming one alone would be
read as a claim it cannot make. Everything a plan *can* settle — the join's kind, one key
of a type a build can publish, the chain of nodes down to the scan, and which of the two mechanisms the table
supports — is settled before the line is printed, so a candidate is never offered where no run could attach one.
What a run really did is in `--analyze`'s `key_filter`, `key_dropped` and `key_pruned` metrics below; a candidate
with none of them beside it is a filter the run declined.

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
  then those that passed them. A column such a predicate reads and nothing above the scan does is read and
  compared and never copied into a batch, which the `cols_filter_only` metric of `--analyze` counts.
- A hash join (any `Join` the executor runs) shows two lines, each with the join's EXPLAIN text:
  - `HashJoin`, its probe, where the input it does not build on is read: in that input's part pipeline (per part),
    or over it when it is no part pipeline;
  - `HashBuild`, its build, under the operator that prepares it: the operator that runs the probe's part pipeline
    (after the pipeline's lines), or a `HashJoin` over its input (after that input's line). Its input follows it,
    per part when it is a part pipeline. Its `rows` and `batches` are the rows its table holds and the table's
    chunks, and its time includes that of the builds its input probes, which come first. The rows it holds are those
    without a NULL in a key that compares `=` (a NULL-equal key's NULLs are held, as values), except for a join that
    emits the build rows which did not match (below), whose build keeps the rows with one too.
  - In the JSON, a `HashBuild` is one more entry of the `inputs` of the operator that prepares it.
- Metrics:
  - `parts`, `skipped` (by statistics);
  - of a dynamic join filter on a `Scan`, where this scan is the probe side of a hash join whose build published
    one ([ADR 0029](adr/0029-dynamic-join-filters.md)): `key_filter` (1 where the scan applies the filter row by
    row), `key_dropped` (the rows it rejected, which the scan's own `rows` no longer counts) and `key_pruned` (the
    parts ruled out whole, because the build published no key their key column spans — those parts are not read at
    all, so they contribute no time and no rows). These appear beside `N pushed predicates` where a scan has both:
    it applies their conjunction, so `key_dropped` counts the rows the keys cleared and no others — the bit is tested
    before it is touched, whichever of the two reaches a row first;
  - of a scan with pushed predicates ([ADR 0020](adr/0020-filter-pushdown.md)): `cols_filter_only`, the columns
    read so that a predicate can be applied to them and that no batch carries, because nothing above the scan
    reads them; and `dict_pieces`, how many (column, piece) pairs had a VARCHAR predicate asked once per distinct
    value of the column chunk's dictionary rather than once per row. `dict_pieces` appears only where a predicate
    is on a VARCHAR column, and a 0 there means the chunk's encoding did not allow it. Neither is visible in a
    result, which is why both are reported: a scan that stops doing either answers exactly the same;
  - `raw_parts`, `raw_rows` (a GROUP BY's parts that sent rows straight to the partitions, because their first
    rows hardly reduced);
  - `part_time` (the part tasks' time, summed);
  - `wait` (for parts);
  - `merge`, `lanes_tail` (merging after the last part), `build` (output rows), `outer`;
  - `sort`, `groups`, and `sample_parts`, `heavy_keys`, `heavy_groups` of a two-level aggregation;
  - a `PartGroupingSets`' `derive` (the second level), `direct_sets` (the sets merged straight from their root, not
    reduced per partition first) and `set_batches` (the second level's batches: more than one only under a memory
    limit, [ADR 0038](adr/0038-grouping-sets.md));
  - a `HashJoin`'s `find` (looking up the keys; none where a join keeps every row or none without looking: a one-row
    join, an anti join over a build without rows, a null-aware anti join over an empty build input or one with a NULL
    key, and an anti join on its left input over a build with no key at all, whose probe is never opened), `gather` (the
    rows' columns: the build columns an inner or a left join gathers, and the probe rows it takes on its other path than
    1:1; the columns of the candidates that residuals read; a one-row join's values, which its build makes once),
    `residual`, `pairs` (a join with residuals: the candidate pairs they met, on every path of an inner join too, one
    per probe row with candidates for a semi or anti join whose residuals read no build column but its keys, [ADR
    0022](adr/0022-joins-and-query-blocks.md)'s E2c) and, when the build's keys are unique, an inner join's
    `window_rows`, and a left join's without residuals (the rows of the probe's batches it gathered the build's columns
    for, next to the `rows` it returned), beside an inner join's `window_matched` (how many of those rows found a match:
    a window all of whose rows did keeps the probe's columns where they are and gathers the build for the window, and
    any other window gathers only the rows that matched, which is why a selective join's `gather` is near its output
    rather than near its probe — [ADR 0026](adr/0026-join-performance.md));
  - a `HashBuild`'s `finish` (the table built from its parts, the arrays of its columns that its gathers read, and a
    one-row join's values), `null_keys` (rows with a NULL in a key that compares `=`, which no key's range holds and no
    lookup returns; a join that emits the build rows which did not match keeps their rows apart from the table it
    indexes, since such a row is unmatched by definition), `unique` (1: no key repeats), `direct` (1: the table
    indexes its one integer key by value, ADR 0022) and `cols_dropped` (columns of its input it did not have to hold,
    because nothing above the join reads them: ADR 0026's C8a, and 0 for a build that holds them all);
  - a `RightJoin`'s `emitted` (the build rows it returned after the probe: those the probes marked for a semi join,
    those they did not plus the retained NULL-key ones for an anti or a full join), and the same `emitted` on the
    `HashJoin` of a full join whose probe is no part pipeline, which emits those rows itself; their gather counts in
    its `gather`.
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
| DECIMAL(p, s), p ≤ 38 (stored as INT32, INT64, FIXED_LEN_BYTE_ARRAY or BYTE_ARRAY) | decimal64(p, s) at p ≤ 18 and decimal128(p, s) above it ([ADR 0021](adr/0021-decimal-semantics.md)'s decimal64 item); every storage width an ARROW:schema may give is converted exactly on read | DECIMAL(p,s) | the exact unscaled value with scale s (Semantics); a DECIMAL(38, 0) column is DECIMAL(38,0), no longer HUGEINT ([ADR 0021](adr/0021-decimal-semantics.md) rule 2) |
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
  `x IS NULL` and `x IS NOT NULL` ([ADR 0037](adr/0037-tpcds-roadmap.md)'s NL4) are never NULL: true or false for
  any value `x`, an expression too (`x + 1 IS NULL`), so `NOT (x IS NOT NULL)` keeps the rows where `x` is NULL. They
  are named as DuckDB names them (`(x IS NULL)`); EXPLAIN shows `x IS NULL` and, for a `WHERE` conjunct, `x IS NOT
  NULL`. Above a `LEFT JOIN`, `IS NULL` on a column of the null-supplying side keeps the rows the join pads, and
  above a `FULL JOIN` on a column of either side. A
  condition as the operand (`(a = 1) IS NULL`, a chained comparison) is unsupported.
  `x IS DISTINCT FROM y` and `x IS NOT DISTINCT FROM y` ([ADR 0037](adr/0037-tpcds-roadmap.md)'s OJ5b) are never NULL
  either: NULL is not distinct from NULL and is distinct from every value, and two values compare as `=` compares them
  (a DECIMAL with a DECIMAL of another scale or an integer by value, with a DOUBLE in DOUBLE), except that a DOUBLE NaN
  is not distinct from NaN and `-0.0` not from `0.0`, as in DuckDB (divergence D10 is `=`'s). Either side may be an
  expression or the `NULL` literal, but not both constants. Against `NULL` it is `IS [NOT] NULL`; against a value `c`,
  `x IS NOT DISTINCT FROM c` is `x = c AND x IS NOT NULL` and `x IS DISTINCT FROM c` is `x <> c OR x IS NULL`, so a
  `WHERE` conjunct `x IS NOT DISTINCT FROM c` filters, and is pushed to the scan, as `x = c` is. They are named as
  DuckDB names them, NOT folded in (`NOT (a IS DISTINCT FROM b)` is `(a IS NOT DISTINCT FROM b)`); EXPLAIN shows two
  columns' `a IS DISTINCT FROM b`. The type pairs `=` refuses are refused alike (DATE with TIMESTAMP, a FLOAT column).
- BETWEEN: `a BETWEEN lo AND hi` is `lo <= a AND a <= hi` in three-valued logic, both bounds inclusive: a range with
  `lo > hi` matches nothing, and a NULL operand (or a NULL bound column) makes it NULL, which rejects the row;
  `a NOT BETWEEN lo AND hi` is its negation, so it rejects that row too.
  An argument of `AND` or `OR` is computed only for the rows the earlier ones leave undecided (`x > 100 OR x * x > 0`
  never computes `x * x` where `x > 100`), as DuckDB does for the same order (divergence D16).
  VARCHAR compares byte-wise and DATE chronologically. A predicate folded to never-true reads no data at all.
- IN: `c IN (v1, v2, ...)` is `c = v1 OR c = v2 OR ...` with Kleene logic, and `c NOT IN (...)` its negation, so a NULL
  value rejects the row for both. Each value is typed and folded exactly like `c = v` ([Binding](#binding)), a
  constant integer expression (`c IN (10, 10 + 1)`) as the literal of its value; a value
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
  with an integer literal beyond BIGINT) or arithmetic on a `SUM` can reach it. Every group's result is checked
  before the first row comes out, so a `LIMIT` that reads only some groups still fails the query, on any number of
  threads (likewise for DECIMAL, D18).
  Over DOUBLE it returns DOUBLE: it adds each row group's values (of each group), then the row group sums in order
  (the same for any number of threads; see Execution below). DOUBLE `AVG` sums the same way.
- AVG: over integer columns the sum accumulates exactly in 128 bits and is divided by the count once at the end, as
  DuckDB divides it ([ADR 0037](adr/0037-tpcds-roadmap.md)'s AG1): a SMALLINT's sum in double, any other integer's as
  a long double divided by the count in long double, then rounded to DOUBLE, so the result is DuckDB's to the bit on
  each platform (past 2^53 it can differ from the correctly rounded mean, and between platforms). Over DOUBLE it
  returns DOUBLE. Over DATE and TIMESTAMP it averages the microseconds (a DATE's midnight; its infinities as the
  TIMESTAMP ones, summed as their int64 values as DuckDB sums them, so that with finite values they give a large
  finite TIMESTAMP; a DATE beyond the TIMESTAMP range an error) in 128 bits and returns a TIMESTAMP, rounded as DuckDB
  rounds: the quotient truncated, one more when twice the remainder exceeds the count (a positive average to the
  nearest microsecond with halves down, a negative one toward zero).
- MIN and MAX: return the input type; VARCHAR compares byte-wise and DATE chronologically. They use Arrow's
  `min_max` kernel, over only the selected rows under `WHERE` (divergence D10 for NaN).
- The variance family ([ADR 0037](adr/0037-tpcds-roadmap.md)'s AG2): `STDDEV_SAMP`, `STDDEV_POP`, `VAR_SAMP` and
  `VAR_POP`, with DuckDB's aliases `STDDEV` (of `STDDEV_SAMP`) and `VARIANCE` (of `VAR_SAMP`), of any number column.
  They return DOUBLE, computed as DuckDB computes them: each value cast to DOUBLE as DuckDB casts it (a DECIMAL by
  [ADR 0021](adr/0021-decimal-semantics.md) rule 8), a running count, mean and sum of squared deviations updated per
  value, partial states combined by Chan's formula. The sample forms are NULL over at most one value, the population
  forms NULL over none and 0 over one. A result that is not finite (a NaN or infinite value, or an overflow) fails
  the query with DuckDB's message (`STDDEV_SAMP is out of range!`, `STDDEV_POP ...`, `VARSAMP ...`, `VARPOP ...`; an
  alias reports its target), before the first row. The result depends on the order of the additions, which differs
  between the engines in parallel runs, so the oracle compares it with the DOUBLE tolerance; antb1 merges in part
  order, so its result is the same for any number of threads. `DISTINCT` is unsupported (exit code 4), and a
  non-number is a bind error. Named as written (`stddev(x)`).
- NULL: aggregates skip NULLs. Over zero input rows `COUNT` returns 0 and every other aggregate returns NULL.
  A predicate that evaluates to NULL rejects the row.
- GROUP BY: one row per distinct combination of the keys; the aggregates of a group are exactly those of the same
  rows without `GROUP BY`. NULL is a key value (all NULLs form one group). A DOUBLE key groups `-0.0` with `0.0` and
  every NaN together, as DuckDB does, and the group shows the key as first seen. Over zero input rows there is no
  group, so no row. Groups come in no particular order (deterministic in antb1, not the order of first appearance),
  and the tests compare grouped results without regard to order; with `LIMIT` any groups are a right answer.
- SELECT DISTINCT ([ADR 0037](adr/0037-tpcds-roadmap.md)'s AG3): the distinct rows of the select list, in any block (a
  derived table, a CTE, a `UNION ALL` block, a subquery), compared as `GROUP BY` compares keys: NULL is a value,
  `-0.0` equals `0.0` and every NaN is one value. It applies after `WHERE`, `GROUP BY`, `HAVING` and the select list,
  and before `ORDER BY`, `LIMIT` and `OFFSET`; over a grouped block it removes repeated rows of the groups. Every item
  counts, a repeated one too (`SELECT DISTINCT a, a` gives two columns). `ORDER BY` takes a select item: a position,
  an alias, or an item written again; any other value is unsupported (exit code 4), since DuckDB orders such a query
  by a value of each distinct row that depends on the order of the rows. `DISTINCT ON (...)` is unsupported.
- Arithmetic: integer `+`, `-`, `*` and unary `-` compute in the result type, and an overflow fails the query with an
  execution error (exit code 1), as in DuckDB; HUGEINT (a `SUM` result) stays within -(10^38 - 1) to 10^38 - 1
  (divergence D9). `/` divides in DOUBLE: `x / 0` is `inf` or `-inf` and `0 / 0` NaN. `//` truncates toward zero
  and `%` takes the sign of the dividend (`-7 // 2` is -3, `-7 % 2` is -1); both are NULL for a zero divisor, and
  the type's minimum divided by -1 overflows. On DOUBLE, `//` divides (NULL for a zero divisor) and `%` is `fmod`
  (NaN for a zero divisor). NULL operands give NULL. An expression is computed only for the rows that `WHERE` keeps.
  Three rewrites of DuckDB's optimizer are reproduced, in its order, so that an overflow fails the same queries. First,
  every integer expression is computed as DuckDB's expression rewriter rewrites it: the constants of a chain of `+` (or
  of `*`) of one integer type -- operations of one operator and one type that are operands of each other; one of another
  type is an operand of the chain, as DuckDB reaches it through a cast, while a cast to a value's own type is none, as
  DuckDB's binder writes none -- move to the chain's front when there are at least two and one of them follows an
  operand that is no constant (`x + 1 + y + 2` is `((1 + 2) + x) + y`, `1 + (2 + x)` stays), where a constant is
  anything that reads no column; a chain whose rebuilt form would be deeper than the 256 levels of expression the parser
  allows stays as written. They fold in the chain's type where antb1 can evaluate them (literals and `+ - * // %`,
  negation, casts and `abs` over them): one that overflows stays unfolded and is computed, failing on the first row that
  computes it, and any other constant (a function or a `CASE` of constants) is computed, to the same value, but is none
  to the rules below that need its value (divergence D33). `x + 0`, `0 + x`, `x - 0`, `x * 1`, `1 * x` and `x // 1` are
  `x`; and `x * 0` is DuckDB's `constant_or_null(0, x)`, which still computes `x`, is NULL for a NULL `x` and is no
  member of a chain of `*`. DuckDB's `x // 0` is NULL without computing `x`, and an operation with a NULL constant
  operand (`2 // 0`) NULL without computing its other one; antb1 computes both, so `(x + 1) // 0` fails at the maximum,
  where DuckDB answers NULL. So `x + 1 + -1` computes nothing and `x * 2 * 0` no product, while `x + 2147483647 + 1` and
  `i16 + 30000 + 30000` fail on every row, as `(2147483647 + 1) + x` and, in SMALLINT, `(30000 + 30000) + i16`; an
  expression keeps the name and the type it is written with. Then, in a query that groups by no key (no `GROUP BY`, or
  one of empty grouping sets alone), `SUM(x + c)` (a sum of a signed integer type, `x` of any integer type, a constant
  `c`) of the expression so rewritten is `SUM(x) + c * COUNT(x)` in HUGEINT, except in a subquery correlated in its
  `WHERE` or an inner join's `ON`, whose aggregation DuckDB has grouped by the correlation when its rewriter runs;
  `SUM(x)` is rewritten again where it has the shape, so `SUM(1 + (2 + x))`, whose leading constants stay apart, also
  counts `2 + x`. And in every comparison (`WHERE`, `HAVING`, `CASE WHEN`, `BETWEEN`, `IS [NOT] DISTINCT FROM`; `x` a
  column, a key, an aggregate or an alias) with an integer literal `k` (or a decimal one of scale 0, such as `5.`, an
  integer compared in a DECIMAL), constants move in the order DuckDB's rewriter gives them: one pass of the first
  rewrite over the operand, then every move it allows, then another pass, until neither changes it. A move takes
  `x + c <op> k`, `c + x <op> k`, `x - c <op> k`, `c - x <op> k` and `x * c <op> k` (an integer `x` of any type, `c` a
  constant, dividing `k` for `*`) to `x` compared with the moved constant, and a cast of `x` -- written, or where an
  operation reads an operand of another type -- to `x` where DuckDB's comparison simplification drops it: between two
  integer types of one signedness, narrowing ones too, or between an integer type or a DECIMAL and a DECIMAL of the same
  scale (an integer type's being 0), where `k` converts exactly into both types (DuckDB compares a `k` the cast's own
  type does not hold in a wider type, behind a cast it cannot drop) (it drops none between a signed type and USMALLINT,
  so a USMALLINT operand of a wider operation stays computed behind its cast, nor one to a DOUBLE or a DECIMAL of
  another scale). An `=` whose moved constant leaves the type, or whose `c` does not divide `k`, is FALSE (NULL for a
  NULL `x`, which is computed inside an expression), and a `<>` whose `c` does not divide `k` TRUE, except for
  `IS [NOT] DISTINCT FROM`, which stops there, and whose null test reads the operand the moves leave; a `NOT` over an
  `=` or a `<>` is the comparison it flips, as DuckDB's parser writes it, so `NOT (x + c = k)` is `x + c <> k` and stops
  where the moved constant leaves the type (an ordered comparison keeps its `NOT`: its moves are the same, and NaN tells
  it from the flipped one under divergence D10). So `x + 1 + 2 > 5` is `x > 2`, `x + 2147483647 + 1 > 5` fails, and
  `(x + 2147483647) * 1 + 1 > 5` moves 1, the `* 1` the first pass removes, and 2147483647, before any pass can regroup
  the chain. The one other order DuckDB keeps: where the operand is a cast DuckDB drops -- a written one that changes
  the type, under any casts to the value's own type, or the one its binder writes to compare in a wider type, against a
  constant expression of one (`2147483648 - 1`, a BIGINT, against an INTEGER chain) or in a `BETWEEN`, which compares in
  the type of all three values, so that a literal or a column bound the operand's type does not hold widens it for both
  comparisons, and a literal operand takes a bound's type it fits (a DOUBLE bound, or a DECIMAL one of a nonzero scale,
  whose cast DuckDB keeps, moves nothing) -- it drops it before the operand's first pass, and constants move off the
  chain as written first, for as long as each is a literal of its operation's own type -- an INTEGER literal in an
  INTEGER chain, `-(1)` included; not a cast, a constant expression or a literal of another type -- so
  `x + 2147483647 + 1 > 2147483648 - 1` is `x > -1`. Divergences D14, D16, D32, D33 and D34 list what else differs.
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
- Strings: `strlen` counts bytes, while `substring` counts codepoints, as DuckDB counts them for each (so
  `substring('Привет', 1, 2)` is two letters of four bytes where `strlen` of them is 4).
  A byte that is no UTF-8
  continuation byte starts a codepoint, and a continuation byte belongs to the codepoint before it;
  the first byte of a value always starts one, even when it is a continuation byte, so no byte ever
  falls outside every codepoint and a window covering the value returns it whole (divergence D15,
  since an unannotated Parquet `BYTE_ARRAY` is a VARCHAR here and may hold any bytes). `substring`'s
  window is one rule: a negative start counts from the end (`-1` is the last codepoint), a negative length extends
  the window backwards from the start, and the window is clipped to the value, so a start past the end or a window
  that closes before it opens gives the empty string. NULL in any argument gives NULL. Written without a length the
  window does **not** reach the end of the value: DuckDB defaults the length to 4294967295, which shows only at the
  bottom of the start's range, where `substring(x, -4294967296)` loses the last codepoint and
  `substring(x, -4294967295)` keeps the value whole. A start or a length outside
  [-4294967296, 4294967295] fails the query when it runs (exit code 1), with DuckDB's own message, one per side of
  each bound.
  `regexp_replace` replaces the first match only, with RE2 in UTF-8 mode as DuckDB
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
  operator rules of `WHERE`: a NULL aggregate (`SUM` of only NULLs) or NULL key rejects the row. As in DuckDB, the
  conjuncts without a subquery filter the groups first, those with one next, and the select list and `ORDER BY` are
  computed last, only over the groups that pass (D16 names where DuckDB computes more or less).
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
- UNION ALL: every row of the first block, then of the next block, and so on, duplicates and NULLs kept, each
  block's rows in its own order; the same on any number of threads. A block runs only once the one before it has
  ended, and a `LIMIT` met without `ORDER BY` stops before the blocks it does not need, which never run -- so their
  errors are never reported (divergence D26). Nor are those of a block a `WHERE` condition on the union rules out by
  its constants, in both engines; a block it does not rule out runs, and fails where DuckDB's plan may not run it
  (divergence D27). A cast below the union is an exact widening and never fails.
- UNION: each distinct row of its operands once, in no particular order, compared as `GROUP BY` keys. It reads every
  block in full, in written order, before it returns a row, so a `LIMIT` above it saves no block, and the first
  failing block in written order fails the query (D30). Its memory holds the distinct rows, charged to the memory
  limit; nothing spills (ADR 0039).
- INTERSECT: each distinct row of its left operand that every other operand holds; INTERSECT ALL: min(m, n) copies
  of a row its operands hold m and n times, each copy with its normalized value (`-0.0` as `0.0`, every NaN as one).
  Both read every operand in full, in written order, even after one turns out empty (D31); only the left operand's
  distinct rows are held -- the others are looked up and add none.
- EXCEPT: each distinct row of its left operand that no other operand holds; EXCEPT ALL: max(m - n - ..., 0) copies of
  a row its operands hold m, n, ... times, each copy with its normalized value. Both read every operand in full, in
  written order, the left one first even when it is empty (D30); only the left operand's distinct rows are held.
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
- Join order and build sides: the relations of a `FROM` list join in the order and the tree shape that carry the
  fewest rows between the joins, chosen by dynamic programming over the subsets the join conditions connect
  ([ADR 0026](adr/0026-join-performance.md), C5). The tree need not be a chain: two groups of relations may be
  reduced separately and then joined, which is often much cheaper for a schema whose dimensions hang off one large
  table. The cost counts the rows each join produces and the rows it has to hold to produce them; it does not count
  the scans, which every order pays alike. A block of more than 12 relations keeps ADR 0022's older rule, the
  largest relation probing and the smallest join next, which `--join-order greedy` also selects for any block.
- The estimates behind that order account for the conjuncts routed to each relation, from the row counts, minimums,
  maximums and null counts in the Parquet footers, with a documented default factor for every condition the footers
  cannot measure. Each join condition narrows by the larger of its two columns' distinct-value estimates; two
  conditions between the same pair of relations narrow only once, being one composite key, while conditions to
  different relations narrow in turn. A join builds its hash table on its right input and probes with its left; it
  builds on the left instead when the right is estimated at more than `--build-side-ratio` times it (default 4; `1`
  always builds on the smaller side). The ratio changes which plan a query gets, never its answer, and an estimate
  nothing knows never moves a build -- a relation of unknown size is probed, never built. Estimates are integer
  arithmetic over footer metadata alone, so a plan is the same on every host and for any number of threads; `explain`
  prints `build=left|right` on every join.
- What the order does **not** account for: a build whose one integer key is dense enough is probed by an array
  indexed by the key rather than by a hash table, which is about twenty times faster per probe, and reducing that
  side's rows without reducing its key's range makes the key sparser. The order does not read the layout, so a plan
  that carries fewer rows can still be slower; what keeps the two from fighting is that the array is laid out far
  past the density it needs to beat a hash, up to 32 slots a row -- the memory of 4 bytes a slot, bounded at an
  eighth of `--memory-limit`, is all a sparser one costs ([ADR 0026](adr/0026-join-performance.md)). A query near its
  limit is hashed rather than failed, so the limit changes how a join is laid out, never its answer; `explain
  --analyze` prints `direct=1` on a build the array serves.
- A semi join of a correlated `EXISTS`, and an anti join of a correlated `NOT EXISTS`, build on the side they
  preserve -- the outer one -- when the subquery's side is estimated at more than the same ratio times it
  ([ADR 0026](adr/0026-join-performance.md)): such a subquery over a large table otherwise builds the whole of that
  table to probe it with far fewer rows. The estimate reads through a dependent join to its outer side, so the outer
  of two correlated subqueries at one level chooses a side as the inner one does. Such a join emits the rows of the
  side it builds on, so it ends its probe's pipeline rather than sitting inside one, and `explain` shows it as
  `RightJoin` with its probe and its build beneath; whatever is above it runs on one thread. The rows it returns are
  the rows the other direction returns, in the build's order rather than the probe's; neither order is specified
  without `ORDER BY`. An anti join returns the rows a semi join over the same condition drops, the rows whose key is
  NULL among them, so the two partition the side they preserve. A plan whose probe side is not a chain of streaming
  nodes over a scan -- a sorted or aggregated subquery, say -- keeps building on its right input, since there is no
  pipeline for the join to be the sink of; a probe scan whose row groups the statistics all prune does take this
  plan, and returns the whole preserved side for `NOT EXISTS` and no row for `EXISTS`. An `EXISTS` or `NOT EXISTS`
  that is not correlated, a `NOT IN`, a `LEFT JOIN` and a `FULL JOIN` always build on their right input.
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
- `csv` has a header row and RFC 4180 quoting, with LF line endings; `json` is an array of objects (a name that
  repeats, as two stars can make it, repeats as a key, as in DuckDB's CLI), where numbers are
  JSON numbers except HUGEINT, DECIMAL and non-finite doubles, which are strings (exactness), and every byte of ill-formed
  UTF-8 in strings (RFC 3629, so also overlong forms, surrogates and code points above U+10FFFF) is written as the
  text `\xHH` with lowercase hex digits, so the output is always valid UTF-8; `table` and `csv` write the raw bytes.

`--timing` prints the elapsed seconds in fixed-point notation (for example `0.003620`) as the last line on stderr.

## Exit codes

| Code | Meaning | Examples |
| --- | --- | --- |
| 0 | success | |
| 1 | query error: syntax, bind, execution or memory error | `SELECT COUNT(*) FORM t`; `FROM t semi` (a word that is never a table alias); an unknown table or column; `SUM` of a VARCHAR column; a `SUM`, or arithmetic on a `SUM`, outside HUGEINT's range; an invalid `regexp_replace` pattern; `UNION ALL` blocks of different column counts; a scalar subquery that returns more than one row; a query that needs more memory than `--memory-limit` |
| 2 | usage error | unknown option; neither or both of `-c` and `-f`; a malformed `--table`, `--column-type` or `--memory-limit`; a column that `--column-type` cannot read as DATE; a table name registered twice |
| 3 | I/O error | a missing or unreadable file; not a Parquet file; schemas that differ; a glob that matches nothing |
| 4 | unsupported: valid-looking SQL outside the supported subset | `row_number() OVER ()`; `IS TRUE`; an unknown function; `SELECT 1e3`; `SUM(DISTINCT ...)`; `CAST(a AS FLOAT)`; a string literal as a DECIMAL `CASE` value; a column of an unsupported type; `FROM a RIGHT JOIN b ON a.k = b.k`; `FROM t semi JOIN u ON ...`; `a IN (SELECT ...)` outside a top-level `AND` conjunct; `EXISTS (SELECT ...)` and `(SELECT ...)` outside a condition of `WHERE` or of an inner `ON` (in `HAVING`, anything but a top-level `EXISTS` or a direct operand of a top-level comparison or a `BETWEEN` bound); a correlated `(SELECT ...)` other than a direct operand of a top-level comparison, and a correlated `EXISTS` other than a top-level conjunct; more than 256 relations; `UNION` without `ALL`; `ORDER BY` on a `UNION ALL`; a `UNION ALL` of INTEGER and VARCHAR values ([ADR 0036](adr/0036-union-all.md)) |
| 70 | internal error: anything else, which is a bug | an uncaught exception; an Arrow `NotImplemented` or type error without SQL context; a one-row join whose right input is not one row |

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
| D10 | NaN | MIN and MAX ignore NaN like Arrow's `min_max`, whatever the batch and file boundaries: they return NaN only when every selected non-NULL value is NaN (so only MAX over NaN and other values differs from DuckDB). Arrow's comparison kernels follow IEEE 754: NaN compares unequal to everything, so `d > 1` and `d >= 1` are false for NaN (and `NOT (d > 1)` true: a `NOT` over an ordered comparison stays one), and `d IN (...)` never matches it; `IS [NOT] DISTINCT FROM` follows DuckDB instead (NaN is not distinct from NaN) | orders NaN above every other value and equal to itself: MIN and MAX return NaN when it is the extreme, `d > 1` is true for NaN | the fixtures contain no NaN (fixturegen builds doubles from integer ratios), and `tests/slt/cases/expressions/arithmetic.slt` computes one (`d % 0`) only where both engines agree; `exec.AggregateStateTest.MinMaxOfDoublesIgnoreNaNInEveryBatchSplit` and `engine.SessionTest.MinMaxIgnoreNaNAcrossBatchesAndFiles` pin antb1's MIN and MAX |
| D11 | FLOAT columns | read as DOUBLE (widened exactly): results of FLOAT columns are DOUBLE and print with double precision; `WHERE` compares like DuckDB (see Binding); arithmetic on them, and comparing them with other expressions, is unsupported (exit code 4); in a `UNION ALL`, FLOAT with FLOAT values stays FLOAT and FLOAT with DOUBLE values is DOUBLE, while FLOAT with integer or DECIMAL values is unsupported | keeps FLOAT (`MIN`, `MAX` and projections return FLOAT); a `UNION ALL` of FLOAT and integer or DECIMAL values is FLOAT | the random generator never references a FLOAT column (`ColumnOf` in `tests/slt/runner/query_gen.cc`, `harness.LoadGenTables.SkipsFloatColumns`); `tests/slt/cases/where/float.slt` selects only other columns, and `engine.SessionTest.FloatColumnsCompareLikeDuckDb` pins that results stay DOUBLE; `tests/slt/cases/having/having.slt` writes its FLOAT result twice, `onlyif duckdb` (FLOAT) and `onlyif antb1` (DOUBLE), since a record's type names must match, under a label shared with a record both engines answer as DOUBLE; `tests/slt/cases/setops/union_all.slt` counts the rows of FLOAT unions under `x = 0.1` |
| D12 | HUGEINT literals against DOUBLE | an integer literal beyond BIGINT compared with a DOUBLE column is the correctly rounded nearest double (a decimal literal converts as DuckDB converts a DECIMAL, [ADR 0021](adr/0021-decimal-semantics.md) rule 8: `9007199254740993.5` is 2^53 in both engines) | converts a HUGEINT literal through its 128-bit formula (lower + upper × 2^64), which can be one ulp off | the generator writes DOUBLE columns only their own sample values and short decimals (`DoubleText` and `ExactDecimalDouble` in `tests/slt/runner/query_gen.cc`); `plan.BinderTest` pins the decimal conversion and `tests/slt/cases/types/decimal_literals.slt` compares it with DuckDB |
| D13 | Decimals with many digits against integer and DECIMAL values | compared exactly, and a literal beyond the operand's type folds to a constant | compares in a DECIMAL whose width is capped at 38 digits: when the column type's digits plus the literal's decimals exceed 38, a column value with too many integer digits fails the query with a conversion error (`i16 = 1.0000000000000000000000000000000000001` over the value -32768); likewise an integer `SUM` (HUGEINT, 38 digits) in `HAVING` against any decimal fails once the sum has more digits than 38 minus the literal's decimals, and a DECIMAL(p,s) column against a literal with more than s decimals or more than p - s integer digits once their total exceeds 38 (`d38_10 > 0.00000000001` over a value of 28 integer digits). Two operands that are no literals fail the same way, in comparisons, `IN` lists and `BETWEEN`: a DECIMAL against a DECIMAL of another scale or an integer, once a value of the side with the smaller scale has more integer digits than 38 minus the larger scale (`d9_2 < d38_0` over d38_0's extremes, a DECIMAL(38,2) against a DECIMAL(38,12) past 26 integer digits). An integer literal of more than 38 - s digits against a DECIMAL(p,s) (`p < 1000000000000000000000000000000000000` on DECIMAL(15,2)), and one of 2^127 to 2^128 - 1 (UHUGEINT) against a SMALLINT, INTEGER or BIGINT operand, fails to cast on any input that reaches it | the `.slt` records and the generator keep literals short enough (`DecimalText` in `tests/slt/runner/query_gen.cc`) and compare two columns only while their common type stays within 38 digits (`ColumnComparison`, columns of two FROM items of a join included), and join two DECIMAL keys only then (`UsableEdges`); `tests/slt/cases/types/decimal.slt` and `decimal_comparisons.slt` pin both answers; `plan.Binder/FoldThroughBinderTest.*` and `plan.Binder/FoldDecimalTest.*` cover the exact folding, `exec.DecimalTest.CompareExactIsExactAcrossScalesAndIntegers` the exact comparison |
| D14 | Overflows DuckDB's optimizer does not avoid | a comparison that is a whole `WHERE` or `HAVING` conjunct and folds to never-true at bind time (a literal outside the operand's type, as in `smallint_col + 1 > 40000`, or a DECIMAL result's, as in `price * qty > 100000000000000000000` on DECIMAL(15,2) columns) computes nothing, so it cannot overflow (a subquery's value it reads still builds its join over a scan, whose own errors then show; over an aggregation or a set operation the join never builds, divergence D35); one that folds to always-true keeps `IS NOT NULL` over its operand, which is computed, and under `OR` or `NOT` or in a `CASE WHEN` condition a folded comparison keeps NULL for a NULL operand, so its operand is computed and overflows as in DuckDB (`p * q > 100000000000000000000 OR id = 1` fails in both engines) | computes the operand and fails on an overflow ("Overflow in addition of INT16") | the random generator never writes arithmetic that can overflow; `plan.BinderTest.WhereMovesConstantsLikeDuckDb` pins which comparisons move their constants |
| D15 | VARCHAR bytes that are not UTF-8 | answers: `strlen` counts every byte, `regexp_replace` runs RE2 over the bytes as UTF-8, where an invalid byte never matches (not even `.` or `[^a]`) and stays in the result, and `upper` and `lower` return such a value unchanged | cannot read such a value as VARCHAR: reading an unannotated BYTE_ARRAY column (`binary_as_string`) with it fails the query ("Invalid string encoding") | every fixture string is valid UTF-8, so the oracle tests never meet it; `exec.ComputeTest.StringFunctions` and `exec.ComputeTest.UpperAndLowerMapCodepointsAsDuckDb` pin antb1's behavior |
| D16 | Evaluation order in conditions | computes the arguments of `AND` and `OR` in the order written, each only for the rows still undecided (an `AND`'s later arguments for the rows its earlier ones leave true or NULL), and a WHERE conjunct's operands for every row; so an overflow, or a failed cast of a DECIMAL `CASE` value, inside a condition fails exactly when a row reaches it in that order. OR factoring makes a conjunct every branch of an `OR` shares a WHERE conjunct of its own, so its operands are then computed for every row: `id = 6 AND i16 * 2 > 1 OR id = 11 AND i16 * 2 > 1` overflows where the `OR`'s own argument order would have computed `i16 * 2` for one row only (the factoring's result written by hand, `i16 * 2 > 1 AND (id = 6 OR id = 11)`, has always failed the same way). In `HAVING` it computes the operands of every conjunct without a subquery for every group, a conjunct over `GROUP BY` keys alone included, and filters; then it joins the conjuncts that hold a subquery, each computing its operands over the groups that remain; then it computes the select list and `ORDER BY` over the groups that pass, so a derived table's or a CTE's select list is computed only over its groups that pass, also where an enclosing block filters on one of its columns. A subquery's value in a condition is computed before any row is read, whatever the `AND`, `OR` or `CASE` around it decides (ADR 0041's decision 9), as in DuckDB; a WHERE conjunct that holds one computes its operands only for the rows that the conjuncts without one keep -- each goes onto its relation, below the values, whatever the order written -- and that the conjuncts with one written before it keep; an `IN` list that holds a value is the `OR` of its equalities, so it computes a later value of the list only for the rows the earlier ones leave undecided | may reorder conjunctions by its cost model, computes the argument of a `NOT` for every row, and, in a condition it selects rows by (`WHERE`, `HAVING`, a `CASE WHEN`), an `AND`'s later arguments only for the rows its earlier ones make true; so an overflow or a failed cast can fail in one engine and not the other (`NOT (x < 100 AND x * x > 0)` fails in DuckDB; both factored forms above answer; `CASE WHEN i32 > 2147483647 AND i32 + 2147483647 + 1 > 0` answers in DuckDB and fails in antb1 at a NULL `i32`). In `HAVING` it pushes a conjunct over `GROUP BY` keys alone below its aggregation, so its other conjuncts run only over the groups that one keeps (`HAVING MAX(i32) * 2 > id AND id = 6` answers in DuckDB and overflows in antb1), and it filters before it computes the select list, a conjunct that holds a subquery included; but it pushes an enclosing block's filter on a column that a derived table computes over its aggregation into that table's `HAVING`, ahead of its conjuncts (into a CTE's too where every reference to it filters on that column, as one read once), so the column is computed for every group (`SELECT id FROM (SELECT id, MAX(i64) * 2 AS m FROM edge GROUP BY id HAVING MAX(i64) BETWEEN -50 AND 50) t WHERE m > 0` fails in DuckDB and answers in antb1). Where its plan makes a comparison with a subquery's value a join condition whose build is the subquery, it computes the subquery's side of it once, at that build, for no row too: `id < 0 AND i32 > (SELECT MAX(i64) FROM edge) + 1` fails in DuckDB and answers in antb1; and it drops an argument of `OR` its footer statistics prove true, so `i32 > (SELECT MAX(i64) FROM edge) + 1 OR id >= 0` answers in DuckDB and fails in antb1; and it computes every value of an `IN` list for every row that reaches the list, so `ct_id = 1 AND ct_id IN (1, (SELECT MAX(zn_id) FROM zones) * 1000000000)` fails in DuckDB and answers in antb1 | the random generator never writes arithmetic that can overflow nor a `CASE` value whose cast can fail; `exec.ComputeTest.ConditionsAreThreeValued` pins antb1's order, and `tests/slt/cases/joins/or_factoring.slt` both forms of the overflow against DuckDB's answer, and `tests/slt/cases/expressions/arithmetic.slt` the `CASE WHEN` one with both engines' outcomes; `tests/slt/cases/having/having.slt` pins the `HAVING` order, the conjunct over keys alone and the enclosing block's filter with both engines' outcomes, and `tests/slt/cases/subqueries/conditions.slt` the subquery values'. Narrowed by the implied-`IN` derivation (ADR 0026): a derived conjunct reaches the scan and takes rows away from the arithmetic above it, so all three of that file's overflow cases now answer, and agree with DuckDB |
| D17 | Overflows in constant expressions | a constant integer expression on a side of a comparison, a `BETWEEN` bound or a value of an `IN` list that overflows its type (`a > 2147483647 + 1`) is a bind error, whatever the table holds | raises the overflow only when a row computes it: an empty table, or one whose rows another conjunct rejects first, answers | `plan.Expressions/BindErrorTest` pins the errors; `tests/slt/cases/where/between.slt` has the error on a table both engines fail on and, `onlyif antb1`, on the empty table, and `tests/slt/cases/in/in.slt` the same for an `IN` list |
| D18 | DECIMAL SUM beyond 38 digits | a `SUM` of a DECIMAL(p,s) whose result has more than 38 digits is an execution error (`SUM overflow: the result is outside the range of DECIMAL(38,s) (38 decimal digits)`), as for HUGEINT (D9) | returns up to 39 digits until its 128-bit sum overflows | `tests/slt/cases/types/decimal_arithmetic.slt` pins both answers; the generator sums only DECIMAL columns whose sum over every row fits 38 digits |
| D19 | Which failing row an error names | the overflow and cast errors of DECIMAL arithmetic, and the cast errors of DECIMAL `CASE` values, print the values of the first failing row of a 64Ki-row batch (each operand, then the operation); a dependent `GROUP BY` key (ADR 0018) is computed per group, in group order; a failed cast names the column it casts (`when casting from source column b`) only for a column reference, an aggregate's output and a key included | evaluates 2048-row vectors, and every key per row: with several failing rows the message can show another row's values; it also names the column of an expression its optimizer reduces to one (`b + 0`); the exit code and whether a query fails are the same | the `.slt` records match the error text without the values; `exec.ComputeTest.DecimalOperandsInDuckDbOrder` pins the operand order, `exec.ComputeTest.DecimalCaseValuesCastToTheCaseType` the named columns |
| D20 | CASE types without a DECIMAL value | types the values that are no literals first, then lets each integer literal take their integer type when it fits and each string literal any type: `CASE WHEN c THEN 7 WHEN c2 THEN s16 END` and `CASE WHEN c THEN 8 WHEN c2 THEN s16 ELSE 7 END` are SMALLINT, and a string literal before a DATE value is that DATE (`THEN '2020-01-01' WHEN c2 THEN dt END`) and before a number unsupported | folds from the `ELSE` value (NULL without one) through the `THEN` values in order, where a literal next to NULL (the first `THEN` value without an `ELSE`, or a `THEN NULL`) or next to another literal becomes its own type (both CASEs are INTEGER), while an `ELSE` literal takes the next value's type, and a string literal next to NULL is VARCHAR (a bind error next to the DATE or the number) | `plan.BinderTest.CaseTypesWithoutADecimalAreAntb1s` pins antb1's types; the random generator writes the column first among its `CASE` values, where both agree; with a DECIMAL value antb1 folds as DuckDB does ([ADR 0021](adr/0021-decimal-semantics.md) rule 10) |
| D21 | Keywords as names | the 49 words that are never table aliases ([Grammar](#grammar)) are unquoted column, table and function names, qualifiers and select aliases, with or without `AS` (without `AS`, `glob`, `isnull` and `notnull` continue the expression: unsupported), so `SELECT default FROM t` answers with a column named `default`; `BETWEEN`, `EXISTS`, `INTERVAL` and `OVER` are reserved: no unquoted column or table names (an error) or qualifiers (unsupported), but table aliases, as in DuckDB; a name right after a star and before `,` or `FROM` is an alias there (unsupported), also a word DuckDB keeps as a keyword | refuses all 49 words as unquoted column names (`default` is its `DEFAULT` keyword, an error wherever a query uses it: `SELECT default FROM t` is a binder error), table names, qualifiers and select aliases without `AS` (after an expression `isnull` and `notnull` are its operators `IS NULL` and `IS NOT NULL`, not aliases); accepts them after `AS` and after a dot, and 14 of them as function names (`unpack(...)` is its `UNPACK` operator, the other 34 are syntax errors); accepts `BETWEEN`, `EXISTS`, `INTERVAL` and `OVER` as column and table names and as qualifiers; a keyword right after a star is a syntax error | the fixtures, the `.slt` records and the random generator use none of these words as an unquoted name; `sql.ParserTest.WordsThatCannotBeImplicitAliases`, `sql.ParserTest.EveryReservedWordAfterAsInFrom` and `sql.ParserTest.ReservedWordsThatQualifyNamesInFrom` and the reject case `KeywordAliasAfterAStar` pin antb1's rules, and `harness.TableAliasOracle.*` compares every DuckDB keyword as a table alias and as a qualifier in FROM with DuckDB itself |
| D22 | `IN` over a doubly parenthesized query | reads `x [NOT] IN ((query))` as an `IN` list holding one scalar subquery, because a `(` opens a nested query only before a reserved query word (divergence D21), and a list of one subquery is unsupported: exit code 4 at the inner `(` (a list of two or more values, or of values and literals, is the `OR` of its equalities, ADR 0041's SQ1) | reads an `IN` subquery and answers it exactly as the single-paren form | `plan.SubqueryPlacements/BindErrorTest` pins the refusal and its span, and `tests/slt/cases/subqueries/in.slt` compares the form with DuckDB's own answer under `onlyif duckdb` |
| D23 | A correlated `EXISTS`'s conjuncts over different rows | unnests it into a semi or anti join (ADR 0023's Path 1), so a conjunct that reads both blocks is a residual, computed only over the candidate pairs a key matches, and a conjunct that reads only the outer block is a residual too, computed over those same pairs rather than over every outer row | plans a delim join and computes the subquery's conjuncts for the bindings of the outer side, so an overflow in such a conjunct can fail in one engine and not the other | the random generator writes no arithmetic that can overflow (as for divergences D14 and D16), and `tests/slt/cases/subqueries/exists.slt` compares every shape Path 1 answers with DuckDB's own answer |
| D24 | A correlated scalar aggregate over keys no outer row binds | unnests it by equivalence substitution (ADR 0023's Path 2): the aggregation groups by the correlated key and computes the aggregate's argument for EVERY row of the subquery's own input, including rows whose key is NULL or bound by no outer row. Where the correlation is on more than one column, or on one column over a join or a union, the groups are restricted to the keys the outer side binds (the semi-join reduction), but above the argument, which is computed all the same; on one column over one table ADR 0029's D5 filter can skip, at run time, the rows whose key no outer row binds | 1.5.6 keeps its domain D, the outer rows' distinct correlated keys, where the subquery has a condition besides the correlation (`i.k = o.a AND i.k <> 5`, or one on another column) or the outer side has a filter of its own (`o.id < 6`), and then computes the argument only for the keys the outer rows bind; otherwise it removes D and computes the argument for every row whose correlated key is not NULL, keys no outer row binds included | the answers agree: an outer row with no group is dropped by the inner join and rejected by the comparison alike. Only an ERROR can differ, and in either direction: antb1 fails on a row whose key is NULL, or, where DuckDB keeps D, on a row whose key no outer row binds, where DuckDB answers; and it answers where D5 skips a key that DuckDB computes and fails on. So the random generator writes no arithmetic that can overflow (as for divergences D14, D16 and D23), and `tests/slt/cases/subqueries/scalar.slt` compares every shape Path 2 answers with DuckDB's own answer |
| D25 | A moved Compute over rows a join above discards | computes a grouped aggregate on one input of a join below it (ADR 0032), and where the aggregate's argument is an expression a `Compute` above the join defines, that `Compute` travels DOWN with the partial -- a partial cannot aggregate a column that does not exist below it. The expression is then computed for every row of the aggregated side, including the rows the joins above discard | computes it above the join, so only for the rows that survive | the answers agree: a discarded row contributes to no group in either plan. Only an ERROR can differ, so the random generator writes no arithmetic that can overflow (as for divergences D14, D16, D23 and D24), and `tests/slt/cases/aggregates/placement.slt` pins the overflow against DuckDB's own answer, with a control whose call refuses the placement and so answers in both engines |
| D26 | Errors of the blocks of a `UNION ALL` that a `LIMIT` does not need | a `LIMIT` met without `ORDER BY` stops before the blocks it does not need, which never run, so their errors -- an overflow, say -- are never reported ([ADR 0036](adr/0036-union-all.md)) | runs at least the first vector of every block, so an error in a later block fails the query | the answers agree where no block fails; the random generator writes no arithmetic that can overflow (as for divergences D14, D16, D23, D24 and D25), and `tests/slt/cases/setops/errors.slt` pins it: antb1's answer under `onlyif antb1`, DuckDB's error under `onlyif duckdb`, and with `ORDER BY` a control that fails in both |
| D27 | Errors of the blocks of a `UNION ALL` or a `UNION` that a condition on it does not rule out by their constants | a `WHERE` condition on a union goes on top of each block, above the block's select list, and rules a block out only where the block's constants decide it ([ADR 0036](adr/0036-union-all.md)'s X7a; never for a `LIKE` or a DOUBLE, and never for a condition the binder computes, such as a `NOT`, which stays above the union). Any other block runs, so an error of its select list -- an overflow, say -- fails the query, even on rows a condition on another column rejects. A condition above a `UNION` stays above it ([ADR 0039](adr/0039-set-operations.md)'s X4b, until its PF15), so every block runs in full | rules out a block whose constants make a `LIKE`, a DOUBLE comparison or a `NOT` never true too, and filters a column below the select list, so that error is never reported | the answers agree where no block fails; the random generator writes no arithmetic that can overflow and tags no block with a constant, and `tests/slt/cases/setops/errors.slt` and `union.slt` pin it: antb1's error under `onlyif antb1`, DuckDB's answer under `onlyif duckdb`, and the conditions both engines decide as records both pass |
| D28 | A DOUBLE cast to an integer type that rounds to one past the type's maximum (`CAST(32767.5 AS SMALLINT)`, half to even) | fails the query with DuckDB's out-of-range conversion error | answers what its platform's conversion of the rounded value gives (-32768 on arm64; an x86 build may give another value) | `exec.ComputeTest.CastsToIntegerTypesRoundAndCheckAsDuckDb` |
| D29 | Which of `-0.0` and `0.0`, or which NaN, a deduplication keeps | a `GROUP BY` key, a `SELECT DISTINCT` row and a `UNION`, `INTERSECT` or `EXCEPT` row show the value their group saw first, in input order and then part order (a set operation's blocks in written order, among those that can bring a row in: an `INTERSECT`'s or `EXCEPT`'s left operand, even a block whose own row an `EXCEPT` then removed), the same on any number of threads, and a condition above the deduplication tests that kept value; an `INTERSECT ALL` or `EXCEPT ALL` root shows the normalized value | shows a value that depends on its plan and threads, and can change between runs: `SELECT DISTINCT` over a `UNION ALL` of an empty block, a block holding `0.0` and one holding `-0.0` shows `-0.0` at one thread (D30's order), where antb1 shows `0.0`, and so does a `UNION` of the same three blocks; it passes an `INTERSECT ALL`'s rows on normalized, so `(N INTERSECT ALL N) UNION E`, with N holding `-0.0` and E empty, shows `0.0` where antb1 shows `-0.0`; `(B EXCEPT C) UNION A`, with B holding `-0.0` and C and A `0.0`, shows `0.0` where antb1 shows `-0.0`; it drops an `EXCEPT ALL` whose later operands a `LIMIT 0` or a constant condition empties, so `N EXCEPT ALL (P LIMIT 0)` shows `-0.0` where antb1 shows `0.0`; and it moves a condition on the value below the deduplication, so one that tells `-0.0` from `0.0` tests every row's own value: over `0.0`, `-0.0` and `3.0`, `SELECT DISTINCT` and then `WHERE 1 / d < 0` gives `-0.0` in DuckDB and no row in antb1 | the fixtures' DOUBLE columns hold no `-0.0` and no NaN, and the one FLOAT `-0.0` (`floats.parquet`) is compared only through counts (`tests/slt/cases/groupby/keys.slt`); the random generator groups by no DOUBLE key, writes no set operator but `UNION ALL` over a DOUBLE column and never reads a FLOAT column (D11); `tests/slt/cases/setops/union.slt`, `intersect.slt` and `except.slt` pin the set operations' cases with both engines' answers ([ADR 0039](adr/0039-set-operations.md)) |
| D30 | Which error a query reports when several blocks of a `UNION ALL` or a set operation fail | the first failing block in written order, since a union runs its blocks one after another ([ADR 0036](adr/0036-union-all.md)) | follows its pipeline order: at one thread, under an aggregate, a sort or a join's build side, it runs the first block and then the others from the last back, so `COUNT(*)` over three blocks whose second and third fail reports the third's error, as does a `UNION`, whose own deduplication is such a sink; it builds an `INTERSECT`'s or `EXCEPT`'s right operand before it reads the left one, so with both failing it reports the right one's error, and it skips the other operand where a `LIMIT 0` or a constant empties an `INTERSECT`'s operand or an `EXCEPT`'s first one, which antb1 still reads; streamed to the client it runs them in written order; above one thread its choice varies between runs | the answers agree where at most one block fails; the random generator writes no arithmetic that can overflow (as for divergences D14, D16 and D23 to D27), and each error record of `tests/slt/cases/setops/errors.slt` has one failing block; `union.slt`, `intersect.slt` and `except.slt` pin the set operations' cases with both engines' errors ([ADR 0039](adr/0039-set-operations.md)) |
| D31 | An `INTERSECT` whose right operand turns out empty | reads the left operand in full, so an error anywhere in it -- an overflow, say -- fails the query | stops each pipeline that streams into the join after the first vector of 2,048 rows that reaches it with a row, and returns no rows; below an aggregate, a `DISTINCT`, a sort, a join's build side or another set operation it reads the left operand in full too; an `INTERSECT ALL`, an `EXCEPT` and an `EXCEPT ALL` read it in full in both engines ([ADR 0039](adr/0039-set-operations.md)) | the answers agree where no left row fails; the random generator writes no arithmetic that can overflow, and `tests/slt/cases/setops/intersect.slt` pins it: antb1's error under `onlyif antb1`, DuckDB's empty answer under `onlyif duckdb` |
| D32 | Expressions nothing reads, and a join's probe keys over an empty build | computes every select item of a derived table, a CTE or a `UNION ALL` branch and every aggregate of a derived table or of an `EXISTS` block, whether or not a query above reads it (a `Project` and a `UnionAll` keep every output; an `EXISTS` block's select list is dropped, and with it that of a derived table or a CTE right under it, where nothing but a `LIMIT` or an `OFFSET` of the block stands between them, but not its aggregates, grouped or not), and the probe keys of a `LEFT JOIN`, an anti join or a `NOT IN` whose build side turns out empty, so an error there -- an overflow, say -- fails the query; the constants DuckDB's own order regroups (`x + 2147483647 + 1`) fail it on every row. The other way round, it computes no `ORDER BY` key of a query or a block that aggregates without `GROUP BY`, whose one row it never sorts, nor of a correlated `EXISTS` (ADR 0023's Path 1 takes its `FROM` and `WHERE` alone), so an error in them never shows | removes the columns and the aggregates nothing reads, and computes no probe key over an empty hash table, so it answers; it computes every `ORDER BY` key, also of one row or of an `EXISTS` block, correlated or not, so it fails | the answers agree where nothing fails; the random generator writes no arithmetic that can overflow, and `tests/slt/cases/expressions/arithmetic.slt` and `tests/slt/cases/subqueries/exists.slt` pin it: antb1's error under `onlyif antb1`, DuckDB's answer under `onlyif duckdb` (and the reverse for the `ORDER BY` keys), and a control that reads the column and fails in both |
| D33 | Constants antb1 does not evaluate | folds only integer literals and `+ - * // %`, negation, casts and `abs` over them; any other constant -- a function of constants such as `strlen('a')`, a `CASE` or a `COALESCE` of them -- is computed, to the same value, so the rewrites that need a constant's value do not apply to it: `x * 1`, `x + 0` and `x // 1` stay operations, a comparison moves no constant across it and `SUM(x + c)` is no sum rewrite, so `i32 + COALESCE(1, 2) > 5` computes the addition and fails at the type's maximum | folds every constant expression before these rewrites, so it answers | the answers agree where nothing overflows; the random generator writes no such constant, and `tests/slt/cases/expressions/arithmetic.slt` pins it: antb1's error under `onlyif antb1`, DuckDB's answer under `onlyif duckdb` |
| D34 | Comparisons of one value in a filter that contradict each other | computes the value they compare, so an overflow there fails the query: `WHERE i32 + 2147483647 + 1 BETWEEN 10 AND 5` computes `(2147483647 + 1) + i32` and fails on every row | its filter combiner finds that no row passes a `WHERE`, a `HAVING` or an inner join's `ON` whose comparisons of one value contradict each other (an empty `BETWEEN`, or `x > 10 AND x < 5`) and computes nothing, so it answers; in a `CASE WHEN` it computes the value as antb1 does | the answers agree where nothing overflows; the random generator writes no arithmetic that can overflow, and `tests/slt/cases/expressions/arithmetic.slt` pins it: antb1's error under `onlyif antb1`, DuckDB's answer under `onlyif duckdb` |
| D35 | Errors of subqueries next to a join that turns out empty or a condition that can never hold | prepares the builds of a pipeline's joins before its probe reads a row, outermost join first (a build whose input probes builds of its own prepares those first), and stops at the first build that empties its join, an inner or semi join whose build holds no row, such as an `IN` or a correlated `EXISTS` whose set turns out empty: the builds below it never run and its probe never opens its input, so their errors -- an overflow, say -- are never reported (`PrepareBuilds` in `src/exec/hash_join.cc`; [ADR 0022](adr/0022-joins-and-query-blocks.md)'s J1b update). A block's subquery joins stack above its own joins in the order their conjuncts are written, the last outermost (an `IN` that ADR 0029's D0 moves onto one relation's branch stands below them all). So over a `u` whose `k` values are 1 and 3 and whose INTEGER `x` values include 2, `WHERE t.b < (SELECT MAX(u.x * 2147483647) FROM u) AND t.a IN (SELECT u.k FROM u WHERE u.k = 2)` answers, and with the `IN` written first it fails. Across blocks too, a failing subquery never runs inside a derived table, a CTE body or a `UNION ALL` branch that probes a join whose build turns out empty, inside an `IN` subquery whose join stands below one that empties, or in a `WHERE` under a `HAVING` `IN` that turns out empty. A condition that can never hold empties nothing before the builds run: `t.a = 1 AND t.a = 2` next to the failing subquery fails the query (antb1 has no filter combiner, D34). A conjunct antb1 folds to false, though -- a comparison with a literal outside its operand's type (D14), which DuckDB computes -- ends the plan where its filter stands: the joins below it never build, unless they run in the filter's own pipeline over a scan (no aggregation or set operation between), whose builds run first. So a `WHERE` subquery's error under such a `HAVING` conjunct is never reported, nor the error of a subquery written before a `HAVING` conjunct that compares a subquery's value and folds so (that conjunct's filter stands above the joins written before it), nor a subquery's whose own comparison folds so over a set operation or an aggregation, while over a scan the query fails. A `HAVING` subquery's join stands above the filter of `HAVING`'s conjuncts without a subquery, so its build runs and the query fails even where DuckDB's filter combiner proves those conjuncts never true and answers (`ct_id = 40000 AND ct_id = 40001` next to it). The values of ADR 0041's SQ1 behave the same way wherever they stand in a condition -- under `OR`, in a `CASE` or `COALESCE`, as an `IN` list's value -- and two more cases are theirs: a subquery written twice in a slot is one value, planted where it is first bound (decision 10), so an `IN` that turns out empty written between the two hides its error (`ct_id > (q) AND ct_id IN (SELECT pm_id FROM promos) AND ct_id > (q)` answers); and a subquery that only an `OR` branch dropped by OR factoring holds is bound but never planted, so its error never shows, which differs from DuckDB only where the conjunct the branches share holds a value itself | prepares every build before any probe, whatever turns out empty, so the example fails in either order; an empty file, a filter outside a column's range and join keys outside the other side's range remove no build either. It answers only where its optimizer deletes the failing subquery before running: a condition its filter combiner proves never true (`a = 1 AND a = 2`, `a > 5 AND a < 3`, an empty `BETWEEN`) in the block the failing subquery is written in (its `WHERE`, an inner `ON` or its `HAVING`), or such a condition or a `LIMIT 0` in an `IN` subquery there or in a derived table among its FROM items, empties the block's plan, the subquery's join included; an `OR` that a plain conjunct of every branch implies, `(A AND X) OR A`, is that conjunct, the subquery in `X` going with the rest; and a derived-table or CTE column that nothing reads goes with its subquery (D32) | the answers agree where no subquery fails; the random generator writes no arithmetic that can overflow and no scalar subquery of more than one row (ADR 0041's decision 28), so no generated subquery fails. A scalar subquery's error over two or more rows (ADR 0041's SQ4a) is the family's most common member, and `tests/slt/cases/subqueries/values.slt` pins it: each engine's outcome under `onlyif`, for an `IN` written after the failing subquery, a condition that never holds, a conjunct folded to false above an aggregation, a `HAVING` subquery next to `HAVING` conjuncts DuckDB proves never true, a later folded subquery comparison, a `LIMIT 0` derived table and a `HAVING` `IN`, and controls that fail in both; `tests/slt/cases/subqueries/conditions.slt` pins the forms of values in conditions, the subquery written twice and the dropped branch |
| D36 | Which error a query reports when several of its subqueries fail | prepares the builds of a slot's value joins outermost first -- the value bound last -- and reports the first build that fails, so of two failing values the one bound last, the one written last unless OR factoring binds a shared conjunct's value first or a value written twice binds at its first occurrence (decision 10): `ct_pop > (SELECT MAX(i32) * 2 FROM edge) AND ct_id > (SELECT zn_city FROM zones)` reports the row error (live since ADR 0041's SQ4a) | reports an overflow before a row error (`More than one row returned by a subquery used as an expression`), whichever is written first, so that query reports its overflow; both exit with code 1 | the answers and the exit codes agree; the `.slt` records hold one failing subquery each, so the message each matches is both engines' |

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
fail cleanly with exit code 4 (unsupported). All 22 pass today; the plan is recorded in
ADRs 0021 (DECIMAL), 0022
(joins, query blocks and uncorrelated subqueries) and 0023 (correlated subqueries). `tests/tpch/` generates the data
at test time ([testing.md](testing.md#data-derived-from-tpc-h)), and `tpch.status.sf0_01`, `tpch.status.sf0_1` and
`parallel.tpch.status.sf0_1` enforce the ratchet
([testing.md](testing.md#the-ratchet-of-the-queries-derived-from-tpc-h)). The last column names the work whose PR
completes the query; most queries also need the DECIMAL work of ADR 0021 before it.

| Query | Status | Completed by |
| --- | --- | --- |
| Q1 | pass | DECIMAL arithmetic, SUM and AVG (ADR 0021) |
| Q2 | pass | a correlated scalar aggregate over four shadowing tables, unnested by equivalence substitution (ADR 0023's Path 2) |
| Q3 | pass | inner joins (ADR 0022) |
| Q4 | pass | a correlated EXISTS, unnested into a semi join (ADR 0023's Path 1) |
| Q5 | pass | inner joins (ADR 0022) |
| Q6 | pass | DECIMAL arithmetic, SUM and AVG (ADR 0021) |
| Q7 | pass | derived tables (ADR 0022) |
| Q8 | pass | derived tables (ADR 0022) |
| Q9 | pass | derived tables (ADR 0022) |
| Q10 | pass | inner joins (ADR 0022) |
| Q11 | pass | uncorrelated scalar subqueries (ADR 0022) |
| Q12 | pass | inner joins (ADR 0022) |
| Q13 | pass | left outer joins (ADR 0022) |
| Q14 | pass | inner joins (ADR 0022) |
| Q15 | pass | uncorrelated scalar subqueries (ADR 0022) |
| Q16 | pass | uncorrelated IN and NOT IN subqueries (ADR 0022) |
| Q17 | pass | a correlated scalar aggregate whose outer table the subquery reads again (ADR 0023's Path 2) |
| Q18 | pass | uncorrelated IN and NOT IN subqueries (ADR 0022) |
| Q19 | pass | OR factoring for joins (ADR 0022) |
| Q20 | pass | a correlated scalar aggregate inside an uncorrelated IN subquery, correlating into that middle block (ADR 0023's Path 2) |
| Q21 | pass | a correlated EXISTS and a NOT EXISTS over the outer block's own table, with a non-equality residual (ADR 0023's Path 1) |
| Q22 | pass | a correlated NOT EXISTS inside a derived table (ADR 0023's Path 1) |

This workload is derived from the TPC-H Benchmark and is not comparable to published TPC-H Benchmark results, as this
implementation does not comply with all requirements of the TPC-H Benchmark.

## Queries derived from TPC-DS

The third workload is the 99 queries derived from TPC-DS, in DuckDB's dialect, as the pinned conda-forge package
`duckdb-extension-tpcds` provides them. Their text and the data are generated at test time and never committed
([ADR 0006](adr/0006-test-strategy-and-data-policy.md)); queries are referred to by their number (1-based). The ratchet
`tests/data/tpcds_status.json` lists the queries verified to pass against DuckDB, and `pass` below means exactly the
ratchet (`pixi run lint` compares them); the PR that makes a query pass updates both. A query that does not pass must
fail cleanly with exit code 4 (unsupported). `tests/tpcds/` generates the data at scale factors 0.01, 0.1 and 1
([testing.md](testing.md#data-derived-from-tpc-ds)), and `tpcds.status.sf0_1`, the eight shards
`tpcds.status.sf1.1of8` to `8of8` and `parallel.tpcds.status.sf0_1` enforce the ratchet. For a query that does not
pass, the last column lists every construct it needs that antb1 does not answer yet, in the order of
[ADR 0037](adr/0037-tpcds-roadmap.md)'s Plan, from that ADR's survey of 2026-10-31. In parentheses is the id of the Plan
item that completes the query: the one that answers the last of them. A query that needs window functions names
phase 9 instead, whose PRs the window ADR will number. The subquery items (SQ) and the subquery constructs listed
below are those of [ADR 0041](adr/0041-subqueries-and-decorrelation.md) (2026-11-10), whose phase 8 order the Plan
follows.

| Query | Status | Completed by / needs |
| --- | --- | --- |
| Q1 | pass | answered when the ratchet was added |
| Q2 | pass | round of a DOUBLE (FN2) |
| Q3 | pass | answered when the ratchet was added |
| Q4 | pass | `UNION ALL` (ADR 0036) |
| Q5 | pass | ROLLUP (RU3) |
| Q6 | unsupported | needs scalar subqueries that are not aggregates and a correlated aggregate inside arithmetic (SQ5) |
| Q7 | pass | answered when the ratchet was added |
| Q8 | pass | `INTERSECT` (X4c) |
| Q9 | unsupported | needs a scalar subquery in the select list (SQ2) |
| Q10 | unsupported | needs correlated EXISTS under OR (SQ8) |
| Q11 | pass | `UNION ALL` (ADR 0036) |
| Q12 | unsupported | needs window aggregates over whole partitions and windows over aggregates (phase 9) |
| Q13 | pass | answered when the ratchet was added |
| Q14 | pass | scalar subqueries that select a CTE's column (SQ4a) |
| Q15 | pass | answered when the ratchet was added |
| Q16 | pass | answered when the ratchet was added |
| Q17 | pass | stddev_samp (AG2) |
| Q18 | pass | ROLLUP (RU3) |
| Q19 | pass | answered when the ratchet was added |
| Q20 | unsupported | needs window aggregates over whole partitions and windows over aggregates (phase 9) |
| Q21 | pass | a quotient of literals folded into DOUBLE (CT4) |
| Q22 | pass | ROLLUP (RU3) |
| Q23 | pass | `*` next to other select items |
| Q24 | pass | upper (FN4) |
| Q25 | pass | answered when the ratchet was added |
| Q26 | pass | answered when the ratchet was added |
| Q27 | pass | NULL items of `UNION ALL` branches (NL3) |
| Q28 | pass | cross products of one-row inputs |
| Q29 | pass | constant integer expressions in IN lists |
| Q30 | pass | answered when the ratchet was added |
| Q31 | pass | `ELSE NULL` as a `CASE` without `ELSE` |
| Q32 | pass | answered when the ratchet was added |
| Q33 | pass | `UNION ALL` (ADR 0036) |
| Q34 | pass | `ELSE NULL` as a `CASE` without `ELSE` |
| Q35 | unsupported | needs correlated EXISTS under OR (SQ8) |
| Q36 | unsupported | needs rank() and window partition expressions (phase 9) |
| Q37 | pass | answered when the ratchet was added |
| Q38 | pass | `INTERSECT` (X4c) |
| Q39 | pass | stddev_samp (AG2) |
| Q40 | pass | an identity CAST (CT1) |
| Q41 | unsupported | needs a correlated COUNT subquery (SQ6) |
| Q42 | pass | answered when the ratchet was added |
| Q43 | pass | `ELSE NULL` as a `CASE` without `ELSE` |
| Q44 | unsupported | needs scalar subqueries that are not aggregates, scalar subqueries inside a condition and rank() (phase 9) |
| Q45 | unsupported | needs an IN subquery under OR (SQ7) |
| Q46 | pass | constant integer expressions in IN lists |
| Q47 | unsupported | needs abs, window aggregates over whole partitions, rank() and windows over aggregates (phase 9) |
| Q48 | pass | answered when the ratchet was added |
| Q49 | unsupported | needs rank() (phase 9) |
| Q50 | pass | answered when the ratchet was added |
| Q51 | unsupported | needs running window aggregates and windows over aggregates (phase 9); its `FULL OUTER JOIN` is answered (OJ3) |
| Q52 | pass | answered when the ratchet was added |
| Q53 | unsupported | needs abs, window aggregates over whole partitions and windows over aggregates (phase 9) |
| Q54 | pass | scalar subqueries that are not aggregates, as the bounds of a BETWEEN (SQ4a) |
| Q55 | pass | answered when the ratchet was added |
| Q56 | pass | `UNION ALL` (ADR 0036) |
| Q57 | unsupported | needs abs, window aggregates over whole partitions, rank() and windows over aggregates (phase 9) |
| Q58 | pass | scalar subqueries that are not aggregates (SQ4a) |
| Q59 | pass | `ELSE NULL` as a `CASE` without `ELSE` |
| Q60 | pass | `UNION ALL` (ADR 0036) |
| Q61 | pass | CAST to DECIMAL(p,s) (CT2) |
| Q62 | pass | `*` next to other select items |
| Q63 | unsupported | needs abs, window aggregates over whole partitions and windows over aggregates (phase 9) |
| Q64 | pass | answered when the ratchet was added |
| Q65 | pass | answered when the ratchet was added |
| Q66 | pass | `UNION ALL` (ADR 0036) |
| Q67 | unsupported | needs rank() (phase 9) |
| Q68 | pass | constant integer expressions in IN lists |
| Q69 | pass | answered when the ratchet was added |
| Q70 | unsupported | needs rank(), windows over aggregates and window partition expressions (phase 9) |
| Q71 | pass | `UNION ALL` (ADR 0036) |
| Q72 | pass | DATE plus an integer (CT5) |
| Q73 | pass | `ELSE NULL` as a `CASE` without `ELSE` |
| Q74 | pass | `UNION ALL` (ADR 0036) |
| Q75 | pass | `UNION` (X4b) |
| Q76 | pass | IS NULL (NL4) |
| Q77 | pass | a cross product of two multi-row relations (OJ4b, [ADR 0040](adr/0040-nested-loop-joins.md)) |
| Q78 | pass | round of a DOUBLE (FN2) |
| Q79 | pass | constant integer expressions in IN lists |
| Q80 | pass | ROLLUP (RU3) |
| Q81 | pass | answered when the ratchet was added |
| Q82 | pass | answered when the ratchet was added |
| Q83 | pass | answered when the ratchet was added |
| Q84 | pass | concat (FN5) |
| Q85 | pass | answered when the ratchet was added |
| Q86 | unsupported | needs rank(), windows over aggregates and window partition expressions (phase 9) |
| Q87 | pass | `EXCEPT` (X4d) |
| Q88 | pass | cross products of one-row inputs |
| Q89 | unsupported | needs abs, window aggregates over whole partitions and windows over aggregates (phase 9) |
| Q90 | pass | NULL as a CASE value and CAST to DECIMAL(p,s) (CT2) |
| Q91 | pass | answered when the ratchet was added |
| Q92 | pass | answered when the ratchet was added |
| Q93 | pass | IS NOT NULL (NL4) |
| Q94 | pass | answered when the ratchet was added |
| Q95 | pass | answered when the ratchet was added |
| Q96 | pass | answered when the ratchet was added |
| Q97 | pass | `FULL OUTER JOIN` (OJ3) |
| Q98 | unsupported | needs window aggregates over whole partitions and windows over aggregates (phase 9) |
| Q99 | pass | lower (FN4) |

This workload is derived from the TPC-DS Benchmark and is not comparable to published TPC-DS Benchmark results, as
this implementation does not comply with all requirements of the TPC-DS Benchmark.
