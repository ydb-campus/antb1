# 22. Joins, query blocks and uncorrelated subqueries

Date: 2026-10-02

## Status

Proposed

## Context

- **The target:** the 22 queries derived from TPC-H. Query numbers in this ADR are theirs, not ClickBench's. Each of
  them exits 4 today, at its first unsupported construct. On 2026-10-02 the maintainer approved a roadmap to all 22,
  with the decisions cited here by their ids (C6, C8, C9, C10, C16) and the PRs named in the Plan below. Its DECIMAL
  work is ADR 0021, its correlated subqueries are ADR 0023, and this ADR designs the rest of the relational work.
  What the queries need from it:
  - **Inner equi-joins of several FROM items:** 17 queries (Q2, Q3, Q5, Q7-Q12, Q14-Q21). Q5's join graph has a
    cycle, Q9 joins on a two-column key, and in Q2, Q8 and Q9 the FROM order starts with two tables that share no
    predicate, so joining in FROM order would build a cross product.
  - **Aliases and qualified names:** Q7, Q8 and Q21. Q7 and Q8 join a table with itself.
  - **Derived tables:** Q7, Q8, Q9, Q13 and Q22, each the only FROM item of its block.
  - **One common table expression** (Q15), referenced twice.
  - **One LEFT JOIN** (Q13), whose ON holds a conjunct on its right table only.
  - **Uncorrelated subqueries:** scalar ones in Q11 and Q15, IN in Q18 and NOT IN in Q16. Q20 and Q22 hold
    uncorrelated subqueries next to correlated ones.
  - **Correlated subqueries:** Q2, Q4, Q17, Q20, Q21 and Q22. They are ADR 0023's subject.
  - **An OR across tables:** Q19's only equality between its two tables sits inside every branch of an OR.
- **What they never need** (the research read all 22 texts):
  - a subquery under OR, in CASE or in a select list: every subquery is a top-level conjunct of WHERE or HAVING;
  - a scalar subquery that is not an ungrouped aggregate;
  - USING, NATURAL, RIGHT or FULL joins, LATERAL, nested joins or set operations;
  - a DOUBLE join key.
- **Their data:** every join key pair has one type on both sides (BIGINT with BIGINT, INTEGER with INTEGER), and no
  key is NULL. NULL keys, dangling keys and duplicate dimension keys therefore need fixtures of our own.
- **The front end today** parses one query block with one FROM item (`sql::SelectStatement`). The parser rejects
  comma lists, every join keyword, aliases, qualified names, derived tables, WITH and subqueries with exit code 4.
  Its reserved words (`kReservedWords`: the join keywords, `QUALIFY`, `WINDOW` and others) cannot be unquoted names,
  but words such as `SEMI` and `ANTI` are not reserved. The depth limit of 256 levels
  ([ADR 0008](0008-parser-and-unparser.md)) counts expressions only.
- **The binder today** binds one table. `plan::BoundColumn` is `{index, name, type}`, where `index` is the position
  in the input node's output; computed columns get temporary index bands (`kPreBase`, `kPostBase`) that the binder
  relocates when it assembles the plan, and a WHERE predicate goes to the scan's filter or to a computed filter by
  comparing its index with the table's width.
- **The logical plan today** has nine node kinds, each a leaf or with one input (`plan::InputOf` returns one). The
  four optimizer passes, EXPLAIN and the physical planner each walk one chain, and `plan::Optimize` cannot fail.
- **The executor today** ([ADR 0013](0013-parallel-execution.md)):
  - Part pipelines (`Filter`, `Compute` and `Project` over a `Scan`) are built once per part by a factory and run on
    the pool. Sinks merge the parts' partial results in part order, through a window of twice the threads.
  - The GROUP BY sink splits each part's groups into 64 partitions by key hash (`exec::KeyHashes`, deterministic),
    and `exec::PartitionLanes` merges each partition's parts in part order, one lane per partition.
  - `exec::MemoryBudget` counts every buffer and, through reservations, the operators' own containers. The window
    narrows above half the limit, and a part that runs out of memory next to others runs again alone.
  - Every wait happens on the consumer thread: a worker that blocks on pool tasks can deadlock the pool
    ([ADR 0014](0014-two-level-aggregation.md)).
- **Arrow's `Grouper` cannot be probed by several threads.** Its `Lookup` is not const and writes the grouper's
  scratch state, so concurrent probes of one grouper race. It also keeps only distinct keys and cannot be pre-sized.
- **Arrow's Acero is not installed, and it emits rows in schedule order.** The environment has `libarrow` and
  `libarrow_compute` but not `libarrow_acero`, and `exec` may link only the first two (`cmake/Antb1Modules.cmake`).
  Acero's hash join hands each output batch to a callback on whichever thread produced it.
- **Statistics:** the footers give row counts, part rows and, for integer-valued columns, exact min, max and NULL
  counts per part (`plan::Table::part_stats`). DuckDB's Parquet writer also stores distinct-count hints, but only for
  dictionary-encoded column chunks, and `plan::Table` does not expose them yet. No statistics persist between
  queries; the plan of 2026-09-29 had sketches come with joins.
- **Answers must not depend on the thread count:** an answer with 4 threads is byte-identical to one with 1 thread
  (ADR 0013), and EXPLAIN does not depend on it either.

## Decision

### Front end

- **A flat FROM list, not a join tree.** A FROM clause is a list of items, and each item after the first carries its
  connector to the items before it: a comma, `CROSS JOIN`, `[INNER] JOIN ... ON` or `LEFT [OUTER] JOIN ... ON`. As
  in DuckDB, a JOIN binds tighter than a comma and associates to the left, so the items from the last comma up to a
  LEFT JOIN form its left input. Without nested joins (parenthesized ones, or a JOIN whose ON comes after a later
  JOIN) this represents every FROM clause, and no walk of the AST recurses along the list: a long chain of joins
  cannot overflow the stack when it is copied, compared or printed. The plan that the binder builds from it is a tree
  of joins whose walks recurse per node; the relation limit below is what keeps every such walk bounded. The grammar
  grows by these rules (in the notation of docs/sql-subset.md):

  ```ebnf
  statement  = query , [ ";" ] ;
  query      = [ "WITH" , cte , { "," , cte } ] , block ;
  cte        = identifier , [ columns ] , "AS" , "(" , query , ")" ;
  block      = "SELECT" , select_list , "FROM" , from_list (* then WHERE to LIMIT, as today *) ;
  from_list  = from_item , { "," , from_item
                           | "CROSS" , "JOIN" , from_item
                           | [ "INNER" ] , "JOIN" , from_item , "ON" , expr
                           | "LEFT" , [ "OUTER" ] , "JOIN" , from_item , "ON" , expr } ;
  from_item  = ( identifier | string_literal | "(" , query , ")" ) , [ [ "AS" ] , identifier , [ columns ] ] ;
  columns    = "(" , identifier , { "," , identifier } , ")" ;
  column_ref = [ identifier , "." ] , identifier ;
  primary    = (* as today, with column_ref for a column *) | "(" , block , ")" ;
  condition  = (* as today *) | sum , [ "NOT" ] , "IN" , "(" , block , ")" | "EXISTS" , "(" , block , ")" ;
  ```

- **ON conditions** are split into conjuncts like WHERE, and their names resolve in the FROM items up to their own
  JOIN, never in a later one (rule 10 below). The ON conjuncts of an inner join then behave as WHERE conjuncts; a
  LEFT JOIN keeps its ON with the join (see the logical plan). ON gets an aggregate-placement context of its own, in
  which an aggregate is an error.
- **Nested query blocks:** derived tables, WITH lists, scalar subqueries, `[NOT] IN (query)` and `EXISTS (query)`
  parse as blocks of their own (`NOT EXISTS` through the prefix `NOT`).
  - Each block starts fresh aggregate-placement contexts: an aggregate in the select list of a subquery inside WHERE
    is legal.
  - Every nested block counts against the depth limit of 256 levels, together with the expressions around it, so
    neither the parser nor any recursive walk of the AST can overflow the stack.
  - **At most 256 relations per query.** The depth limit bounds the AST, not the plan that the binder builds from it:
    a FROM list becomes a left-deep tree of joins and each CTE reference a sub-plan of its own, and every walk of the
    plan recurses per node (the optimizer passes, EXPLAIN, the physical planner, the operators' `Next` calls and the
    plan's destruction). Before anything is bound, `CheckSupported` counts the FROM items of every block as
    relations, those of a CTE's body once per reference to the CTE (an unreferenced CTE is never bound, as in
    DuckDB). Each CTE's count is computed once, so counting stays linear, and a query of more than 256 relations
    exits 4, like the depth limit. Every block has a FROM item, so the count bounds the subqueries too, and with them
    the size and depth of every plan. The largest of the 22 queries joins 8 relations in one block, far below the
    limit.
  - The doubly parenthesized `x IN ((query))` exits 4: DuckDB reads it as an IN subquery, while `x IN ((query), 3)`
    is a list that holds a scalar subquery. `(WITH ...)` inside an expression exits 4 too.
  - Update (2026-10-09, roadmap PR S4a): the parser accepts derived tables and WITH lists, and the binder exits 4 for
    them until J4; S4b adds the subqueries in expressions.
    - A CTE's name and the names of a column alias list follow S3's rules for a table alias without AS. DuckDB also
      takes a string there and a trailing comma in a column alias list, which exit 4. A CTE's column alias list is
      kept at any length (DuckDB ignores the extra names).
    - A CTE name that repeats an earlier one of its list, ASCII case-insensitively, is a syntax error at that name,
      as in DuckDB, before the repeated CTE's query is parsed (an escape or dollar-quoted string name exits 4 first).
    - A CTE named `BETWEEN`, `EXISTS`, `INTERVAL` or `OVER` is read quoted only, since these words are no unquoted
      table names (divergence D21).
    - `WITH RECURSIVE`, `MATERIALIZED`, `NOT MATERIALIZED` and `USING KEY` exit 4; `RECURSIVE` before AS, `(` or
      USING names the first CTE, as in DuckDB.
    - Depth: a derived table's or a CTE's query is one level below the clauses around it, and `sql::Depth` counts the
      same levels as the parser: 256 derived tables in each other parse, the 257th exits 4.
    - For J4: DuckDB reads a string FROM item as a CTE's name before it reads it as a path
      (`WITH c AS (...) SELECT * FROM 'c'` reads `c`), so a string FROM item names a CTE first.
- **Names follow DuckDB 1.5.5,** as the research checked them against it. The binder implements them in steps (see
  the Plan): rules 1-6 in J2b (rule 1's CTEs and derived tables in J4); 7 and 8 in J4 (8 inside subqueries in J5);
  9, 12 and 13 in J5; 11 in J6; and the three parts of rule 10 (the scope of an ON, lateral references and a LEFT
  JOIN ON that reads outside the join) in J2b, J4 and J6.
  1. Each FROM item is one binding, and names match ASCII case-insensitively. A binding is named by its alias, else as
     DuckDB names it:
     - a table or a CTE by its name;
     - a path by its file name up to the first dot, leading dots skipped: `FROM 'dir/a.b.parquet'` binds as `a`;
     - a path with a glob character (`*`, `?` or `[`) by its whole text: `FROM 'dir/t*.parquet'` binds as
       `"dir/t*.parquet"`;
     - the derived tables of a block that have no alias, in FROM order, as `unnamed_subquery`, `unnamed_subquery2`,
       `unnamed_subquery3` and so on.
  2. An alias hides the table's name: with `FROM t AS a`, the name `t.x` is a bind error.
  3. Two bindings may have the same name (`FROM t, t`). A qualified reference `t.x` then resolves to the one of them
     that has a column `x`, and is ambiguous, a bind error, when both have one.
  4. An unqualified column found in two bindings is ambiguous, a bind error that names the qualified candidates. Two
     columns whose names differ only in case stay ambiguous, as today.
  5. `SELECT *` returns every binding's columns in FROM order, duplicates included. When two bindings of the same name
     share a column name, `*` is a bind error (`SELECT * FROM t, t`), like the qualified reference to that column
     (rule 3).
  6. A plain column item `a.x` is named `x`; inside an expression the qualifier stays as written: `sum(a.x)`,
     `(a.x + 1)`.
  7. A derived table's output names are de-duplicated (`x`, `x_1`). A column alias list may be shorter than the
     select list; a longer one is a bind error. A derived table needs no alias (rule 1 names it). A column alias list
     after a table name, a CTE name or a path exits 4 (DuckDB renames a table's leading columns).
  8. A CTE hides a catalog table of the same name and is visible inside subqueries.
  9. A name resolves in the innermost block that has it; a name found only in an enclosing block is a correlation
     (ADR 0023; until then it exits 4, never the bind error "does not exist").
  10. Within its block, an ON resolves names only in the FROM items up to and including its own JOIN, earlier comma
      siblings included (DuckDB answers `a, b JOIN c ON a.x = c.x`). A name that only a later item has is a bind
      error there, as in DuckDB (`a JOIN b ON b.x = c.x JOIN c ON a.y = c.y`), or a correlation when an enclosing
      block has it (rule 9); a later item's column never makes a name in an ON ambiguous. Once bound, inner ON
      conjuncts are treated as WHERE conjuncts, which gives DuckDB's answers. DuckDB also answers an implicitly lateral
      derived table; antb1 exits 4 for a lateral reference and for a LEFT JOIN ON that reads outside the join.
  11. A LEFT JOIN ON conjunct that reads only the right input filters that input, not the join's result.
  12. Scalar subqueries: their rules for columns and rows are under the uncorrelated subqueries below.
  13. A scalar subquery's result is named by its text in parentheses; the tests compare values and types, not names
      (divergence D8).
- **Words that cannot be implicit aliases** are every word that DuckDB 1.5.5 refuses as an implicit table alias and
  antb1 does not reserve, among them `SEMI`, `ANTI`, `ASOF`, `POSITIONAL`, `PIVOT`, `PIVOT_WIDER`,
  `PIVOT_LONGER`, `UNPIVOT`, `TABLESAMPLE`, `AT`, `ONLY` and `RETURNING`. DuckDB also refuses `QUALIFY`, `WINDOW`,
  `LATERAL`, `NATURAL`, `SIMILAR` and `COLLATE`, which antb1 already reserves. DuckDB reads
  `t semi JOIN u ON t.x = u.x` as a semi join and the same with `anti` as an anti join; read as an alias, the word
  would run an inner join instead, a silent wrong answer.
  - After a FROM item such a word is never an alias, and it gets an error of its own: `kUnsupported` where DuckDB
    gives the word a meaning there (`SEMI JOIN is not supported`), a syntax error where DuckDB refuses it there too.
    S3's parser tests enumerate every word.
  - They are not added to the parser's reserved words, which decide result-name quoting (D8) and would break
    columns of those names.
  - After AS, antb1 accepts what DuckDB accepts; a quoted alias is always allowed.
- **Canonical forms** (ADR 0008's round trip and idempotence hold for every new form):
  - every alias prints as a quoted `AS "a"`, written with AS or not, so the oracle, which reads `sql::ToSql` output,
    never reads an alias as a join keyword;
  - `JOIN` prints as `INNER JOIN`, and `LEFT OUTER JOIN` as `LEFT JOIN`;
  - a qualified name prints its qualifier, quoted where the source quoted it;
  - a nested block prints in parentheses, and a column alias list as `("c1", "c2")`.

  Every canonical form is SQL that DuckDB parses with the same meaning.
- **Exit code 4 for the rest:** `USING`, `NATURAL`, `RIGHT` and `FULL` joins, `LATERAL`, nested joins (parenthesized
  ones, and a JOIN followed by another JOIN before its ON), `schema.table` in FROM, `t.*`, `a.b.c`, a column alias
  list after a table, CTE or path (rule 7), `WITH RECURSIVE` and `MATERIALIZED`. The binder's `CheckSupported` walks
  the FROM items, ON conditions, CTEs and subqueries before any table resolves, so such a join exits 4 even over a
  table that is not registered. Each grammar PR lands ahead of the engine, as the expressions did (ADR 0008's
  update): the binder rejects every form it does not answer yet with exit code 4.

### Logical plan

- **Stable column ids come first** (P1 and P2, before any join code; decision C6).
  - Every column a node outputs gets a plan-unique `ColumnId`, which `BoundColumn` and `ColumnExpr` carry, and every
    node exposes the ids it outputs. The binder and every optimizer pass refer to columns by id.
  - `plan::ResolvePositions`, the last step of `plan::Optimize`, fills each `index` with the column's position in its
    input's output. The executor and its hand-built test plans stay positional and unchanged.
  - Join ordering, pushdown into either input, pruning across two inputs, sub-plans and the free variables of
    ADR 0023 all need a column to keep its identity when its position changes. With positions, the multi-table
    binder and the pruning would be written for positions first and rewritten for ids later.
- **Scopes of bindings (J2a).** A binding is one FROM item: its name, its source (a table or a bound sub-plan) and
  its columns (id, name, type, and whether it is stored as FLOAT). A block's scope holds its bindings and links to the
  scope of the enclosing block. Scopes replace the single-table column lookup, the table width and the positional
  FLOAT check.
  - Update (2026-10-07): P2 had already removed the table width and the positional FLOAT check. `plan::Scope`
    (`src/plan/scope.h`) replaces the single-table column lookup and the binder's table and schema members. A column
    without an engine type keeps its id and slot, and is `kUnsupported` only where it is referenced.
- **Join nodes (J1a).** `JoinNode` has two inputs, a kind, key pairs of one common type each, residual conjuncts
  over both inputs and the chosen build side. A match is a pair of rows with equal keys whose residual is true; a
  NULL key never matches.

  | Kind | Output columns | Rows | Planned from |
  | --- | --- | --- | --- |
  | inner | left, then right | every match | commas, `CROSS JOIN`, `INNER JOIN` |
  | left | left, then right | every match, and each left row without one, padded with NULLs | `LEFT JOIN` (J6) |
  | semi | left | each left row with a match, once | `x IN (query)` (J5); `EXISTS` (ADR 0023) |
  | anti | left | each left row without a match | `NOT EXISTS` (ADR 0023) |
  | null-aware anti | left | as SQL's `NOT IN` (see below) | `x NOT IN (query)`, `NOT (x IN (query))` (J5) |
  | one-row | left, then right | each left row with the right input's single row | a scalar subquery (J5) |

  Every consumer handles both inputs: a list of inputs and a rebuild over new inputs replace the single-input
  helpers; every optimizer pass handles the node on ids; projection pruning splits what is needed between the inputs
  and adds the columns of the keys and residuals; EXPLAIN renders both inputs, with qualified column names. The
  physical planner rejects a kind with `kUnsupported` until J1b (inner) or E2 (the others) implements it.
- **Inner blocks.** The FROM items that commas, `CROSS JOIN` and `INNER JOIN` connect form an inner block, and their
  ON conjuncts, bound in the scope of their ON (rule 10), join the block's WHERE conjuncts. A LEFT JOIN with its two
  inputs is one fixed unit: a single relation of the enclosing block, while each of its inputs is planned as an inner
  block of its own.
- **WHERE classification (J2b).** Each conjunct goes by the relations of its block that it reads, which ids give
  directly, also through the `Compute` that an OR or NOT conjunct is lowered into; a binding inside a fixed unit
  counts as the unit:
  - **One relation:** directly above it. Over a scan, part pruning and filter pushdown
    ([ADR 0020](0020-filter-pushdown.md)) apply as today. Over a LEFT JOIN unit, a conjunct that reads only the unit's
    left input moves on into that input, and one that reads its right input stays above the unit.
  - **An equality between two relations,** each side reading one of them, whose common type is not DOUBLE: a key of
    the join that brings the second of them in. Both sides are cast to that common type (DECIMAL included,
    ADR 0021): the key hash reads raw bytes, so an INTEGER key would never meet the equal BIGINT. A side that is an
    expression is computed below the join.
  - **Anything else** (a non-equality across relations, an OR or NOT across relations, a side that reads two
    relations, an equality whose common type is DOUBLE): a residual, directly above the lowest join that brings all
    its relations in.
  - **No DOUBLE keys yet:** an equality whose common type is DOUBLE never becomes a key, whichever side makes it
    DOUBLE: a DOUBLE or FLOAT column, or a DOUBLE expression such as `/` or an AVG of numbers. On raw bytes `-0.0`
    would never meet `0`, which DuckDB matches; a DOUBLE key needs GROUP BY's normalization of `-0.0` and NaN, and
    waits until a query joins on DOUBLE columns. In WHERE and in an inner ON such an equality stays a residual, so a
    join graph connected only through it exits 4 (see the connectivity rule). An IN or NOT IN subquery with that
    common type, and a LEFT JOIN whose ON has no other key, exit 4.
- **OR factoring (J3).** Before classification, `(A AND X) OR (A AND Y)` becomes `A AND (X OR Y)` for every conjunct
  `A` that all branches share, compared by structure (`sql::EqualIgnoringSpans`). AND distributes over OR in
  three-valued logic too, so the rewrite is exact. When a branch has no conjunct left, the OR is true and is
  dropped: `A OR (A AND Y)` is `A`. A shared equality then becomes a key, a shared single-binding conjunct reaches
  its scan, and what is left of the OR is classified like any other conjunct: a residual when it reads several
  relations. Filters implied by an OR (each binding's conjuncts from every branch, OR-ed) are deferred.
- **Connected join graphs only.** The relations of an inner block are the nodes of its join graph and its keys are
  the edges; residuals connect nothing.
  - The binder rejects a disconnected graph with exit code 4, whether the cross product is written with a comma or
    with `CROSS JOIN`. Cross products have no design yet, and one planned by accident grows with the product of its
    inputs.
  - The keyless one-row joins of scalar subqueries are exempt: their right input has exactly one row.
- **Join order (J2b, decision C9): greedy and left-deep, from footer statistics only.**
  1. The probe is the relation with the largest footer row count (for a sub-plan or a fixed unit, its estimate).
  2. Then, of the relations connected to those already joined, the one that gives the smallest estimated result
     joins next. Every equality between it and the joined relations becomes a key of that join, so the equality
     that closes a cycle becomes a second key.
  3. An equi-join of L and R is estimated as |L| × |R| / max(dom(L key), dom(R key)). The domain dom is the key
     column's exact integer range (max - min + 1 over the parts), else its footer distinct-count hint, else its
     relation's row count. A column's distinct-count hint is the largest over its parts, used only when every part
     has one: the writer stores a hint per part, and the largest is a lower bound of the column's count. With several
     key pairs, the pair with the largest domain decides: a lower bound of the whole key's domain, so the estimate
     errs high. A LEFT JOIN unit is estimated like an inner join of its inputs, but never below its left input.
  4. A single-table filter scales its relation by a selectivity: from the min/max range for a range comparison on an
     integer-valued column, from the distinct-count hint for an equality or an IN list, and a fixed default
     otherwise. A sub-plan is estimated from its input, an ungrouped aggregate as one row.
  5. Ties go to the relation that comes first in FROM.

  The domains keep a many-to-many edge (a key with few values on both sides) out of the order while a key edge is
  available; ordering by row counts alone can take such an edge first and multiply the intermediate result. The
  plan depends on metadata only, so EXPLAIN and every answer are the same for any thread count.
- **Build sides.**
  - An inner join builds on the input with the smaller footer row count, where a join below stands in with its
    estimate, and on the relation being added on a tie. With the largest relation as the probe, the probe side
    normally streams through all of a block's joins in one pipeline.
  - An outer, semi, anti or one-row join is a fixed unit for ordering, and builds on the side whose rows it does not
    preserve: a LEFT JOIN on its right input, a semi or anti join on its subquery, a one-row join on its single row.
    Building on the preserved side needs build-side output, which waits for its own ADR.
- **LEFT JOIN (J6).**
  - The ON condition stays with the join. ON conjuncts that read only the right input are pushed into it (rule 11),
    equalities between the two inputs are keys as in WHERE, and every other ON conjunct is a residual of the join,
    even one that reads only the left input: a left row that fails it is padded, not dropped. An ON conjunct that
    reads a binding outside the join's two inputs (a comma sibling before them; later items are out of its scope,
    rule 10) exits 4.
  - A LEFT JOIN without a key between its two inputs (an ON with no equality between them, or only ones whose common
    type is DOUBLE) exits 4, as a disconnected inner graph does: E1's table and E2's candidate pairs need a key.
  - A WHERE conjunct that reads only the preserved side goes below the join as usual; one that reads the
    null-supplying side stays above it. There is no LEFT-to-INNER conversion under a NULL-rejecting WHERE.
  - No join is reordered across a LEFT JOIN, and its ON equalities never feed equivalence classes (ADR 0023 builds
    those from WHERE equalities only).
- **Sub-plans (J4, decision C8).**
  - Each derived table and each CTE reference is bound as its own sub-plan with its own scope, and is one binding of
    its block. A CTE referenced twice is bound twice and scanned twice, as ADR 0013 scans a table that two pipelines
    read; the per-part determinism makes both copies identical.
  - Binding every reference anew multiplies the work along a chain of CTEs: when each CTE joins the previous one with
    itself, 30 of them bind 2^30 copies of the first. The relation limit (at most 256 per query) counts a CTE's body
    at each reference, so such a chain exits 4 before any sub-plan is bound.
  - Names follow rules 7 and 8, and a lateral reference to a sibling FROM item exits 4. An aggregate over an
    aggregate uses the existing serial operators.
  - A sub-plan without a sink (a select-project-join block) streams into its consumer's pipeline. One that ends in a
    sink (an aggregation, a top-N) is drained by its consumer: a build, or a probe pipeline over a serial input.
- **Uncorrelated subqueries (J5, decision C16).**
  - They are allowed only as a top-level conjunct of WHERE or HAVING: `x IN (query)`, `x NOT IN (query)`,
    `NOT (x IN (query))` and `x <op> (query)` in either order. A subquery conjunct of an inner join's ON counts as a
    WHERE conjunct; one in a LEFT JOIN's ON exits 4. Any other placement exits 4, and so does an uncorrelated
    `EXISTS`, which no query of the workload has.
  - Each subquery is a sub-plan with its own scope, resolved innermost first (rule 9); a name found only in an
    enclosing block exits 4 until ADR 0023's PRs. An IN subquery may have GROUP BY and HAVING: nothing is pulled out
    of it. Aggregate detection never descends into a subquery, so an aggregate inside one does not make the outer
    block an aggregate query.
  - `x IN (query)` is a semi join on `x` and the subquery's column, cast to their common type; a DOUBLE common type
    exits 4 (no DOUBLE keys yet). As a WHERE or HAVING conjunct, IN keeps a row only when it has a match, so the semi
    join is exact with NULLs too.
  - `x NOT IN (query)` and `NOT (x IN (query))` both become the null-aware anti join, keyed as IN is. An empty set
    keeps every row, one with a NULL `x` included. Otherwise a row is kept only when `x` is not NULL, has no match,
    and the set holds no NULL. A plain anti join would keep rows that SQL drops.
  - A scalar subquery must have one column: two are a bind error, as in DuckDB. In DuckDB zero rows give NULL and
    several rows are an execution error; antb1 meets neither, because it accepts only an ungrouped aggregate without
    HAVING, LIMIT or OFFSET (each of which can leave no row), which returns exactly one row, so no row count is
    checked at run time. Any other scalar subquery exits 4. It becomes a one-row join that appends its row's
    columns, and a comparison above it. Over an empty input SUM, AVG, MIN and MAX are NULL, which the comparison
    rejects, and COUNT is 0, which it compares as usual; both as in DuckDB.
  - A subquery's join sits above the inner joins of its block (WHERE) or above the aggregation (HAVING), in the
    order the conjuncts are written.
- **Statistics come from the footers only (decision C10).** Planning reads row counts, part rows, integer min/max
  and NULL counts and, new with J2b, the distinct-count hints that a writer stored, through Arrow's Parquet
  statistics. It reads no data and keeps nothing between queries. Persistent sketches (HyperLogLog, Count-Min) come
  with cost-based join ordering if the footers prove insufficient, with their own storage decision.
- **Where it runs.** OR factoring, classification, the connectivity check and the join order run when a block is
  bound, on ids, so plain EXPLAIN shows the order and the build sides. `plan::Optimize` then prunes columns through
  both inputs of every join and ends with `plan::ResolvePositions`; ADR 0023 adds its unnesting pass in front.

### Execution

- **A DAG of pipelines** (ADR 0013, "Towards joins"). A join's build input ends in a build sink, and its probe is a
  streaming operator, like `Filter`, of the pipeline that reads its probe input. A probe pipeline over a table's parts
  runs on the pool, and the sink above it merges in part order, as today.
- **Builds come first, on the consumer thread.** The operator that runs a probe pipeline (its sink, or the probe
  itself over a serial input) prepares the pipeline's builds in its `Open`, before it creates its part scheduler or
  pulls any input, and a build whose input probes builds of its own prepares those first: post-order. One part
  scheduler runs at a time, as the two-level aggregation's two runs of parts already do, and no part task ever waits
  for a build, since a worker that blocks on pool tasks can deadlock the pool.
  - Update (2026-10-09, J1b): the operator that runs a probe pipeline prepares the pipeline's builds when it is first
    pulled, not in its `Open`, and opens its sink only then: an unpulled query (`LIMIT 0`) runs no build, as in
    DuckDB. Within a pipeline the outermost join's build comes first, and a build whose input probes builds of its
    own prepares those first (post-order). Errors follow that order: the first failing part of the first failing
    build decides (in a chain the outer build's), before any probe part starts. A probe whose build holds no row
    never opens its input, and the builds below it are not prepared. Builds are released once the probe pipeline's
    parts are done (the part sinks' parts-done callback) or at its `Close`. An inner join's residuals are evaluated by
    its probe, in order, each only on the rows the ones before it kept (NULL counts as false), not by a `Filter`
    above it. A build's profile line sits under the operator that prepares it.
- **Each build is created once,** by the physical planner, outside the factory that makes a fresh operator chain for
  every part. The factory captures it, and every part's probe reads the same table.
- **Build inputs:** a part pipeline, whose parts run on the pool through the part scheduler, with its window, its
  memory rules and its retry; or any other operator (a sub-plan's aggregation, a HAVING above a GROUP BY), drained on
  the consumer thread and then partitioned in parallel.
- **Partitioned in part order (E1).** For each part, the build materializes the selected key and payload columns,
  hashes the keys with `exec::KeyHashes` and splits the rows into 64 partitions by the GROUP BY rule (the hash
  modulo 64, with one Take per column). The parts go to `exec::PartitionLanes` in part order, so every partition holds
  its rows in (part, row) order, and the build depends only on its input's parts; a drained input's batches play the
  parts. The partitions' tables are then built in parallel; they are also the partitions a spilling grace hash join
  would write.
- **The join hash table (E1)** is our own, and read-only once built, so the probe threads share it without locks.
  - **Direct index** for one dense integer key, whose value range is at most about 8 times the build's rows: the key
    minus the minimum indexes an offsets array into the rows. It is one array for the whole range, which the
    partitions fill in parallel (each key lies in one partition).
  - **CSR** otherwise: per partition, the rows' 64-bit hashes and a bucket directory over the rows in insertion
    order, with no next pointers.
  - In both layouts a key's rows come in (part, row) order. A **uniqueness flag** says that no key repeats.
  - Keys are one or more typed columns of any type but DOUBLE (integers, DATE, TIMESTAMP, VARCHAR, decimal128), which
    the planner has cast to a common type on both sides; equal values of one such type have equal bytes.
  - Update (2026-10-05): a partition's CSR directory lists its distinct keys bucket by bucket, first seen first, each
    with its 64-bit hash and the range of its rows, which keep their (part, row) order; rows are referenced as
    (chunk, row) in the parts' batches, which the table keeps without copying; a build of more than 2^32 - 1 rows is
    an out-of-memory error.
- **The probe (J1b)** is a streaming operator of the probe side's part pipeline, or one over a serial input.
  - It hashes the keys of the selected rows, looks them up and emits at most `batch_size` rows per `Next`, resuming
    inside a probe batch: a 1:N join can fan out beyond one batch, and a part union holds a part's whole output.
  - **The 1:1 path:** with a unique build, an inner join keeps the probe batch and its columns, clears the selection
    bits of the rows without a match and appends the build's payload columns, taken by match; a semi join only
    clears bits. No probe column is copied.
    - Update (2026-10-09, J1b): with residuals, a window where some rows did not match is copied to its matched
      rows before the residuals are evaluated (the update under "Builds come first, on the consumer thread"), and
      a residual that drops some rows leaves a copy of the rest; only a window where every row matched and no
      residual drops a row, or a window without residuals, copies no probe column.
  - Rows keep the probe side's part and row order, and a probe row's matches come in the build's (part, row) order.
    Every sink above merges as it does today, and answers are byte-identical for any thread count.
- **NULL keys never match,** on either side: the build does not insert them, and a probe row with a NULL key has
  no match. That row then fares as any row without a match: an inner or semi join drops it, a left join pads it, an
  anti join keeps it, and the null-aware anti join keeps it only when the set is empty. GROUP BY's hash treats NULL
  as a key value; joins do not. The build keeps two flags for the null-aware anti join: its input had a NULL key,
  and its input was empty. A left join pads with NULLs of every type, DECIMAL included.
- **Residuals (E2).** An inner join's residual is a `Filter` above the probe, in the same pipeline. A semi, anti or
  left join evaluates its residual inside the join, over candidate (probe, build) pairs gathered in chunks, with the
  filter's `exec::PredicateEvaluator`. A semi or anti join ORs the results for each probe row; a left join emits
  every pair that passes and pads a probe row only when none passes. A semi or anti build keeps only distinct keys
  when its join has no residual, and every row when it has one.
  - Update (2026-10-09, J1b): an inner join's residuals are evaluated by its probe, not by a `Filter` above it: in
    order, each only on the rows the ones before it kept (the update under "Builds come first, on the consumer
    thread"). The other kinds' residuals stay E2's, as above.
- **One build feeds one probe pipeline,** until a later ADR. Nothing else is buffered for reuse: a table or sub-plan
  read twice is computed twice (ADR 0013).
- **Memory.** A build's Arrow buffers come from the budget's pool, and its own containers are charged through
  `exec::MemoryReservation`. A build part that runs out of memory next to others runs again alone, as any part does;
  a build that does not fit is an out-of-memory error (exit code 1). Finished builds stay pinned until their probe
  pipeline finishes, and count toward the pressure that narrows the window. The research estimates every build of
  the workload far below half of the default limit up to SF 100 on the development host.
- **Errors** follow the serial order of the plan: builds in post-order, then the probe. The first failing part of
  the first failing build decides the error, before any probe part starts; within a pipeline, the first failing part
  in part order decides, as today.
  - Update (2026-10-09, J1b): within a pipeline the outermost join's build is prepared first, and a build whose input
    probes builds of its own prepares those first (post-order), so in a chain of joins over one pipeline the outer
    build's error wins (the update under "Builds come first, on the consumer thread").
- **The hidden physical rules follow the probe input or decline (J1b):** Filter-on-Scan pushdown, part pruning by
  footers (`exec::KeptParts`), late materialization, the `COUNT(DISTINCT)` rewrite, the partition top-N and the
  Limit over a pipeline. Each looks only along the probe input, or does not apply. Late materialization declines over
  joins ([ADR 0016](0016-late-materialization.md)'s update).
- **Profiles (J1b).** `explain --analyze` shows a line for every build and every probe, with their metrics;
  [ADR 0015](0015-query-profiles.md) gets an update line that lists them.
- **Pull pipelines stay.** This answers [ADR 0003](0003-engine-architecture.md)'s pull-versus-push question for joins:
  builds are sinks prepared on the consumer thread, probes are streaming operators of pull pipelines, and there are
  no exchange operators.
- **No Acero** (see the alternatives) **and no spilling yet.** Spilling gets its own ADR when its trigger is met: a
  target of SF 300 or above, or a runner with less memory than the builds need.

## Consequences

- **What passes when,** with ADR 0021's DECIMAL work: J2b answers Q3, Q5, Q10, Q12 and Q14 (7 of 22, with Q1 and
  Q6), J3 Q19 and J4 Q7, Q8 and Q9 (11 of 22), J5 Q11, Q15, Q16 and Q18, and J6 Q13 (16 of 22). The six queries
  with correlated subqueries need ADR 0023.
- **Plans and answers are deterministic.** Plans come from footer metadata only, builds are partitioned in part
  order and probes keep part order, so EXPLAIN and every answer are the same on 1 thread and on 64.
- **Exit codes.** A join or subquery outside this design exits 4 until a query needs it: USING, NATURAL, RIGHT and
  FULL joins, DuckDB's SEMI, ANTI, ASOF and POSITIONAL joins, LATERAL, nested joins (parenthesized ones, and a JOIN
  followed by another JOIN before its ON), `schema.table` in FROM, `t.*`, `a.b.c`, a column alias list after a table,
  CTE or path, WITH RECURSIVE, MATERIALIZED, more than 256 relations in a query (a CTE's body counted at each
  reference), a disconnected join graph (a cross product), a LEFT JOIN without a key, an IN or NOT IN subquery whose
  common type with its left operand is DOUBLE, a subquery that is not a top-level conjunct (under OR, in CASE, in a
  select list, in a LEFT JOIN's ON), an uncorrelated EXISTS, a scalar subquery other than an ungrouped aggregate
  without HAVING, LIMIT or OFFSET (others can return no row, or several, which needs DuckDB's run-time error), a
  lateral reference, a LEFT JOIN ON that reads outside the join and, until ADR 0023, an outer reference. DuckDB's
  bind errors stay bind errors (exit code 1): an ambiguous name, `*` over two bindings of the same name that share a
  column name, a table name that its alias hides, a name in an ON that only a later FROM item has, a column alias
  list longer than the select list, a scalar subquery with two columns.
- **Correct plans that are not the fastest,** each deferred until its trigger:
  - DOUBLE join keys, until a user or generated query joins on DOUBLE columns;
  - cost-based join order (dynamic programming, selectivity estimates, equivalence edges, key ranges carried across
    joins), until a query of the SF 1 nightly run is far slower than DuckDB, or builds a very large table, because of
    its join order;
  - building on the preserved side (right outer, right semi and right anti joins) and one build probed by several
    pipelines, until profiles of Q13 or Q20-Q22 at SF 10 or above, or the general unnesting domain, need them;
  - runtime join filters and part pruning by build keys, until performance work resumes, after DECIMAL reaches the
    filtered scan;
  - batch-range parts (ADR 0013) and co-clustered range joins with in-order aggregation, until benchmarks sit idle
    at 64 or more threads, or SF 100 is targeted;
  - pressure measured without the pinned builds, until a memory-limit test or a run at SF 10 or above shows probes
    running one part at a time;
  - materialized CTEs, until a profile shows that the recomputation dominates;
  - filters implied by ORs and late materialization across joins, until profiles ask for them;
  - LEFT-to-INNER conversion, with IS NULL support and a profile that needs it;
  - spilling, until a target of SF 300 or above, or a runner with less memory than the builds need; until then a
    query whose builds do not fit fails with exit code 1.
- **Tests:**
  - Our own star-schema fixtures (roadmap PR H6) have NULL keys, dangling keys, duplicate dimension keys, an empty
    dimension and several row groups, and come with a corpus of join and NULL semantics whose expectations DuckDB
    writes.
  - The `.slt` cases run at 1 and 4 threads. Plan-shape EXPLAIN goldens over those fixtures check that there is no
    cross product, the build sides, and that no many-to-many edge is taken while a key edge is available.
  - Exec tests use in-memory tables of several parts, since exec cannot use io; the Parquet-based join tests are
    integration and `.slt` tests.
  - The random generator learns joins (T1), then derived tables, CTEs, LEFT JOINs and uncorrelated subqueries (T2),
    before the PR that declares each feature supported, so the differential tests compare them from that PR on.
  - Nothing derived from TPC-H is committed: no query text, data, answers or EXPLAIN output of the 22 queries.
- **Module edges stay as they are:** joins live in plan and exec, statistics reach the planner through `plan::Table`,
  and no library is added.
- **Docs move with the code:** each PR updates the docs/sql-subset.md sections it changes, J2b rewrites the binding
  step of docs/architecture.md, and J1b adds the build and probe metrics to ADR 0015.

## Plan

**The order of the tracks** (amending the order approved on 2026-09-29: DECIMAL and INTERVAL, hash joins,
decorrelation): first CAST of string literals to DATE, BETWEEN and DECIMAL; then hash joins, derived tables and
CTEs; then uncorrelated subqueries as joins, and correlated ones by unnesting; spilling only on its trigger. The type
lane and the relational lane run side by side after their shared refactors, and INTERVAL waits for a query that
needs it (none of the 22 does).

The PRs of this ADR, one each, by their roadmap ids; the roadmap's dependencies decide the order. Each PR updates the
docs/sql-subset.md sections it changes.

- **P1, refactor(plan): stable column ids.** Ids on `BoundColumn` and `ColumnExpr`, the output ids of every node,
  and `plan::ResolvePositions` at the end of `plan::Optimize`, with a debug check against the binder's positions.
- **P2, refactor(plan): optimizer rules on column ids.** Dependent GROUP BY keys, the Limit pushdown and the
  projection pruning on ids; the index bands and the remapping code are deleted.
- **J1a, feat(plan): join nodes with two inputs.** `JoinNode` with every kind, traversal of several inputs, pruning
  through both inputs and EXPLAIN; the physical planner rejects a join with exit code 4.
- **S3, feat(sql,plan): from lists, joins, aliases and qualified names in the grammar.** The flat FROM list, aliases,
  qualified names, the words that cannot be implicit aliases and the canonical forms; the binder exits 4 for more
  than one FROM item, joins, aliases and qualified names until J2b.
- **S4, feat(sql,plan): derived tables, with and subqueries in the grammar.** Nested blocks, fresh aggregate
  contexts and the depth limit; the binder exits 4 for every new form until J4 and J5. Update (2026-10-09): split
  into S4a (derived tables and WITH lists) and S4b (subqueries in expressions).
- **E1, feat(exec): a hash table for join builds.** The 64 partitions in part order, both layouts, the uniqueness
  flag, typed keys of several columns, the NULL flags and the reservations.
- **J1b, feat(exec): inner hash join.** Build sinks prepared in post-order on the consumer thread, the streaming probe
  with the 1:1 path, the hidden rules along the probe input, and the profile lines.
- **J2a, refactor(plan): name scopes in the binder.** Scopes of bindings, with single-table behavior unchanged.
- **T1, test(diff): generate joins along foreign keys.** Joins of two and three tables in the random generator, as
  comma joins and as JOIN ... ON, with aliases and qualified names. Update (2026-10-07): also `CROSS JOIN`; every join
  stays within a true row bound from the refs' key statistics (non-NULL rows, distinct keys and the largest
  multiplicity), and two metamorphic relations of join order and connectors are pending until J2b.
- **J2b, feat(plan): inner joins of the tables in from.** Rules 1-6, the scope of an ON (rule 10), the relation
  limit, the WHERE classification, the connectivity check, and the join order and build sides from footer statistics
  with distinct-count hints. Its `.slt` cases cover the bind errors of these rules, among them `*` over two bindings
  of the same name that share a column name and an ON that reads a later FROM item. Q3, Q5, Q10, Q12 and Q14 pass.
- **E2, feat(exec): semi, anti, null-aware anti, left outer and one-row joins.** Residuals over candidate pairs,
  builds of distinct keys, NULL padding of every type and the one-row join.
- **T2, test(diff): generate derived tables, ctes, left joins and uncorrelated subqueries.** With their metamorphic
  relations, pending until the engine supports them.
- **J3, feat(plan): factor conjuncts shared by every branch of an or.** Q19 passes.
- **J4, feat(plan): derived tables and common table expressions.** Sub-plans with rules 7 and 8 and rule 1's names
  for CTEs and derived tables, and a CTE's body counted against the relation limit at each reference. Q7, Q8 and Q9
  pass.
- **J5, feat(plan): uncorrelated subqueries as joins.** IN, both spellings of NOT IN and scalar subqueries, with
  rules 9 and 12-13, and CTEs visible inside subqueries (rule 8); outer references exit 4. Q11, Q15, Q16 and Q18
  pass.
- **J6, feat(plan): left outer joins.** The ON placement and the fixed-unit rules. Q13 passes.

## Alternatives considered

- **Arrow's Acero hash join:** a complete join, but not installed, so linking it needs a new package, a new module
  edge and an ADR. It hands output batches to a callback in schedule order, which breaks byte-identical answers
  across thread counts. It takes residuals as Arrow expressions, which would duplicate our evaluators of DuckDB's
  semantics; it has no null-aware anti join; and it does not take part in the memory budget's retry. Rejected.
- **Arrow's `Grouper` as the table that probes read:** concurrent probes race on its scratch state. Probing it safely
  needs a radix exchange of the probe rows into one lane per partition, a pipeline breaker at every join, and it
  keeps only distinct keys and cannot be pre-sized. Rejected; GROUP BY keeps the grouper
  ([ADR 0019](0019-own-aggregation-hash-table.md) stays parked).
- **Dynamic-programming join order now** (DPccp with a cost on intermediate results, and a greedy fallback above
  about 12 relations): cheap at this size, since an 8-relation clique has 255 connected subsets. But it pays only
  with selectivity estimates and equivalence edges, and the greedy connected order is correct and deterministic up
  to SF 1. Deferred to its trigger.
- **Materialized CTEs and shared sub-plans:** a CTE computed once into a shared sink. One query of the workload has a
  CTE, a second scan costs one more scan and aggregation, and every optimizer pass would have to become aware of
  shared nodes (today a rebuild un-shares a DAG and prunes each path its own way). Deferred to its trigger
  (decision C8).
- **Build-side output now** (right outer, right semi and right anti joins, which build on the preserved side): they
  need matched flags and a source that runs after the probe pipeline. DuckDB builds on the preserved side when it is
  the smaller one, but building on the side that is not preserved is correct, so this is performance only. Deferred
  to its trigger.
- **Persistent sketches now** (HyperLogLog, Count-Min; the plan of 2026-09-29): they need a storage decision, and
  tests may write only under their temporary directory. The footers decide every build side the workload needs.
  Deferred to cost-based ordering (decision C10).
- **Kim-style rewrites of subqueries in the binder:** special cases for each subquery shape would be thrown away once
  a general unnesting approach lands. The binder plans uncorrelated subqueries directly as joins and leaves
  correlated ones to ADR 0023's dependent joins and its unnesting pass. Rejected.
- **Joins over positions, without column ids** (decision C6): the first join would arrive about one PR earlier, but
  the multi-table binder and the pruning would be rewritten for ids anyway, before ordering or unnesting. Rejected.
- **A recursive join tree in the AST:** every walk of the AST would recurse along the joins, so a long chain could
  overflow the stack before the binder's relation limit applies. The flat list covers every FROM clause without
  nested joins (parenthesized, or a JOIN whose ON comes after a later JOIN). Rejected.

This workload is derived from the TPC-H Benchmark and is not comparable to published TPC-H Benchmark results, as
this implementation does not comply with all requirements of the TPC-H Benchmark.
