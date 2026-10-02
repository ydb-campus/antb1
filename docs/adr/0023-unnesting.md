# 23. Correlated subqueries as dependent joins, unnested by the optimizer

Date: 2026-10-02

## Status

Proposed

## Context

- **The target:** six of the 22 queries derived from TPC-H have correlated subqueries: Q2, Q4, Q17, Q20, Q21 and
  Q22. Query numbers are theirs, as in [ADR 0022](0022-joins-and-query-blocks.md), which designs the joins, query
  blocks and uncorrelated subqueries that all 22 need. On 2026-10-02 the maintainer approved the roadmap to all 22 and
  its decision C5 on correlated subqueries, which this ADR records. Their shapes (no query text is quoted):
  - **Correlated EXISTS and NOT EXISTS** (Q4, Q21, Q22). Each correlates through one equality between an inner and an
    outer column. Q21 holds an EXISTS and a NOT EXISTS whose blocks read the outer block's table again, and adds a
    non-equality between two other columns: only that non-equality keeps an outer row from matching its own row.
    Q22's NOT EXISTS sits inside a derived table. Several inner blocks also have conjuncts on inner columns only.
  - **Correlated scalar aggregates under a NULL-rejecting comparison** (Q2, Q17, Q20): an ungrouped MIN, AVG or SUM
    (two of them scaled by a literal), compared with an outer expression by `=`, `<` or `>`. They correlate through
    equalities between outer and inner columns: on one column in Q2 and Q17, on two in Q20. Q2's subquery joins four
    tables that its outer block also reads, whose names shadow the outer ones. Q20's subquery correlates into a middle
    block: it sits inside an uncorrelated IN subquery and reads that block's columns, not the outermost block's.
- **What none of them has** (the research read all 22 texts): a correlated COUNT; a CASE or COALESCE over an
  aggregate; a correlated subquery under OR, in CASE, in a select list or in HAVING; a correlated IN or NOT IN; an
  aggregate, GROUP BY, HAVING, LIMIT or OFFSET inside a correlated EXISTS; a non-equality correlation inside an
  aggregate subquery; an outer reference outside the WHERE clause of its subquery, or one that skips a block.
- **Traps** that the research checked in DuckDB: an EXISTS planned as an inner join counts an outer row once per match
  (Q4). At SF 0.1, Q20 gives a different answer when an outer row without inner rows is treated as if its aggregate
  were 0 (the COUNT bug), and also when its uncorrelated IN is planned as an inner join.
- **Neumann's approach to unnesting:**
  - *Unnesting Arbitrary Queries* (T. Neumann and A. Kemper, BTW 2015) plans a correlated subquery as a dependent
    join R ⧑ S: S is evaluated for each row of R, with its free variables (the columns that S reads but does not
    define) bound to that row's values. The algorithm computes the domain D, the distinct values of the free variables
    in R, rewrites R ⧑ S as R ⋈ (D ⧑ S), and pushes D ⧑ down through S, with a rule for each operator, until nothing
    below it depends on D. It unnests every query.
  - *Improving Unnesting of Complex Queries* (T. Neumann, BTW 2025) makes this one top-down pass, so that subqueries
    nested in subqueries are no longer unnested repeatedly.
  - *A Formalization of Top-Down Unnesting* (T. Neumann, arXiv 2412.04294, 2024) proves both correct. A dependent
    join whose right side has no free variables is a plain join (its Lemma 3.1). A correlated selection or map moves
    above the dependent join (Lemmas 4.8 and 4.9), and a GROUP BY below it groups by D's columns too (Lemma 4.14). D
    may be any duplicate-free superset of the bindings (Theorem 4.1), so where every column of D equals a column of S,
    a map from those columns can stand in for D (Lemma 4.2). Its pseudo code renames columns, because Umbra needs
    unique column names.
  - **Two cases need no domain.** The algorithm first moves the correlated selections and maps above the dependent
    join, which is enough when nothing correlated is left below it; and it replaces D with a map when it has found
    equivalences for all of D's columns. The research rewrote the six queries along these two cases alone, and the
    rewrites gave the same answers as the original texts in DuckDB 1.5.5 at SF 0.01 and 0.1.
  - **The general case needs the domain:** D is computed from R and then read twice, inside S and by the final join,
    whose keys must treat NULL as equal to NULL (IS NOT DISTINCT FROM). DuckDB plans all six queries with delim joins,
    which buffer R for this.
- **What ADR 0022 provides:**
  - inner, semi and anti joins, with keys of one common type and residuals evaluated over candidate pairs;
  - sub-plans with their own scopes, linked to the scope of the enclosing block and resolved innermost first (its rule
    9), so an inner table shadows an outer one with the same name;
  - uncorrelated subqueries planned directly as joins; until this ADR's PRs, a name found only in an enclosing block
    exits 4 rather than failing with the bind error "does not exist";
  - stable column ids (roadmap PRs P1 and P2): every column has a plan-unique id that keeps its identity when a rewrite
    moves it, and `plan::ResolvePositions` turns ids into positions at the end of `plan::Optimize`.
- **What is missing:**
  - `plan::Optimize` returns a `LogicalPlan` and cannot fail (`src/plan/include/antb1/plan/optimizer.h`), so only the
    parser and the binder can reject a query today, while unnesting has to see a bound subquery to decide.
  - [ADR 0013](0013-parallel-execution.md)'s "Towards joins" still says that the binder decorrelates correlated
    subqueries.
- **The executor's rules** (ADR 0013, amended by ADR 0022): a table read by two pipelines is scanned twice, never
  buffered; a build feeds exactly one probe pipeline; plans depend on metadata only, so EXPLAIN and every answer are
  the same for any thread count.
- **SQL semantics that decide which rewrites are exact** (DuckDB 1.5.5, probed in the research):
  - EXISTS and NOT EXISTS are true or false, never NULL.
  - Over zero rows, SUM, AVG, MIN and MAX are NULL, while COUNT is 0.
  - A comparison with a NULL operand is NULL, and a WHERE conjunct that is NULL rejects its row.
  - `x IN (query)` is NULL when it finds no match and the set holds a NULL, and `x NOT IN (query)` is true for an
    empty set: for a correlated IN, the set, its NULLs and its emptiness change with every binding.

## Decision

Decision C5: Neumann's top-down approach, staged. The binder plans a correlated subquery as a dependent join, and the
first pass of `plan::Optimize` replaces each dependent join with ordinary joins through one of the two cases that need
no domain, or rejects the query with exit code 4. The general algorithm can be added to that pass later without a
change to the binder.

### Binding (U1)

- **Outer references** resolve through the scope chain, innermost first (ADR 0022's rule 9). A name that no FROM item
  of the subquery's block has, but an enclosing block has, is an outer reference to that block's column, by its id.
- **Free variables over column ids.** The free variables of a plan are the ids that its expressions read and none of
  its nodes outputs (the formalization's F(R)). With ids they need no references by depth and position, and they stay
  valid when a rewrite moves a column. A subquery is correlated when its plan, nested subqueries included, has free
  variables.
- **A dependent join only for a correlated subquery.** An uncorrelated subquery keeps ADR 0022's direct join, so an
  IN subquery with GROUP BY or HAVING stays allowed: it is its own sub-plan, and no predicate is pulled out of it. A
  correlated subquery becomes a `DependentJoin` node:
  - its left input is the block's plan at the place where ADR 0022 puts a subquery's join: above the block's inner
    joins and the subquery joins of the conjuncts written before it, a fixed unit for join ordering;
  - its right input is the subquery's plan, whose free variables are columns of the left input;
  - its kind is semi (`EXISTS`), anti (`NOT EXISTS`) or one-row (a scalar subquery, whose single value it appends to
    each left row; the comparison stays above it, as it does above ADR 0022's one-row join);
  - its meaning: for each left row, the right input evaluated with its free variables bound to that row's values.

  Every visitor handles the node, and EXPLAIN renders it for plan tests; no session prints one, because Optimize
  removes every dependent join or fails.
- **Correlated conjuncts at the top of their block.** A WHERE conjunct of the subquery's block that reads an outer
  column becomes neither a key nor an edge of the block's join graph, so a block whose relations connect only through
  an outer column is disconnected and exits 4 (ADR 0022). The binder keeps all such conjuncts in one filter at the top
  of the block, above all of its joins, subquery joins included, where the unnesting pass finds them. Conjuncts on
  inner columns only are classified as usual, so pushdown and part pruning apply to the subquery's scans.
- **Placement.** A correlated subquery is accepted as a top-level WHERE conjunct: `EXISTS (query)`,
  `NOT EXISTS (query)`, or `x <op> (query)` in either order. A conjunct of an inner join's ON counts as WHERE
  (ADR 0022). In HAVING a correlated subquery exits 4: its outer references would read the outer block's groups, not
  its rows, and no query of the workload needs that. Every other placement already exits 4 (ADR 0022).
- **Outer references only in WHERE.** An outer reference may appear only in the WHERE conjuncts of its subquery's own
  block. Anywhere else (its select list, an aggregate's argument included, GROUP BY, ORDER BY, a LEFT JOIN's ON, a
  derived table) it exits 4. An ORDER BY without LIMIT changes no result of a correlated subquery and is dropped.
- **EXISTS skips only the expansion and type checks of `*` (U2).** Inside `EXISTS (query)` the binder does not expand
  `*`, so a table with a column of a type that antb1 cannot read still works there. Every other select item is bound
  as usual: a bind error stays a bind error, as in DuckDB, and an aggregate makes the block an aggregate query.
- **A fallible Optimize.** `plan::Optimize` returns `arrow::Result<LogicalPlan>`, and the session propagates its error,
  so a query that the pass rejects exits 4 in `query`, `explain` and `explain --analyze` alike. The unnesting pass runs
  first; the existing passes and `plan::ResolvePositions` follow and never meet a dependent join.

### The unnesting pass

- **Top-down.** The pass visits the dependent joins outermost first, in sub-plans and build inputs too, as the
  top-down algorithm does, and replaces each one through Path 1 or Path 2. A dependent join that neither path fits
  fails the query with `kUnsupported` (exit code 4), at the subquery's span and with a message that names the shape.
- **The test of a rewrite is the formalization's Lemma 3.1:** a path applies only when, after its rewrite, nothing
  below the new join reads a column of the left input. A dependent join whose right input reads a column that its
  left input does not output (an outer reference that skips a block) never passes the test, and exits 4.
- **In U1 the pass rejects every dependent join.** U2 adds Path 1 and U3 adds Path 2.

### Path 1: correlated selections pulled up (EXISTS and NOT EXISTS, U2)

- **A plain block:** FROM, WHERE and a select list. A correlated EXISTS or NOT EXISTS whose block has an aggregate,
  GROUP BY, HAVING, LIMIT or OFFSET exits 4. An aggregate without GROUP BY returns a row even over no input, so such
  an EXISTS is always true and a semi join would drop rows; GROUP BY, HAVING, LIMIT and OFFSET would have to act per
  binding, which neither path does.
- **The pull-up** (Lemma 4.8, then Lemma 3.1): the conjuncts of the block's top filter move into the join, which
  becomes a semi join (EXISTS) or a plain anti join (NOT EXISTS, which has no NULL case, unlike NOT IN). Its build
  side is the subquery (ADR 0022). Each conjunct goes by the columns it reads:
  - **Keys:** an equality between a side that reads only inner columns and a side that reads only outer columns,
    whose common type is not DOUBLE, is a key, both sides cast to that type as in ADR 0022 (`t.k = o.k`).
  - **Residuals:** every other conjunct that reads inner and outer columns (a non-equality such as `t.x < o.x`, an OR,
    an equality whose common type is DOUBLE, a side that reads both) is a residual, evaluated over the candidate pairs
    (roadmap PR E2). The build then keeps every row, duplicate keys included.
  - **Outer-only conjuncts** stay residuals of an anti join. Where such a conjunct is false or NULL, the subquery has
    no row and NOT EXISTS keeps the outer row; a filter on the outer rows would drop it. In an EXISTS the same
    reasoning makes them a filter on the outer rows, above the semi join.
  - **Inner-only conjuncts** stay where the binder put them, below the build.
- **At least one key.** A correlated EXISTS or NOT EXISTS without a key exits 4, as a LEFT JOIN without one does in
  ADR 0022: the join hash table and the candidate pairs need a key.
- **NULLs:** a NULL key never matches, and a residual that is NULL is no match. Both mirror SQL, where a conjunct that
  is NULL selects no inner row: EXISTS is then false and the semi join drops the outer row, NOT EXISTS is true and the
  anti join keeps it.

For example, this conjunct of a block that reads a table `o` (the plans are schematic, not EXPLAIN output):

```sql
NOT EXISTS (SELECT * FROM t WHERE t.k = o.k AND t.x < o.x AND o.y > 0 AND t.z = 1)
```

```text
DependentJoin anti                               Join anti  keys o.k = t.k  residual t.x < o.x AND o.y > 0
  Scan o                                           Scan o
  Filter t.k = o.k AND t.x < o.x AND o.y > 0       Filter t.z = 1
    Filter t.z = 1                                   Scan t
      Scan t
```

### Path 2: equivalence substitution (scalar aggregates, U3)

- **Equivalence classes:** a union-find over column ids, built from the equalities between two columns among the WHERE
  conjuncts of the subquery's block (an inner join's ON conjuncts included). These are NULL-rejecting: in every row
  that the WHERE keeps, the columns of a class are equal and not NULL. A LEFT JOIN's ON never feeds a class, nor does
  an equality under OR, NOT or CASE, nor one whose common type is DOUBLE.
- **Accepted only when all of these hold:**
  - the subquery is an operand of a comparison (`=`, `<>`, `<`, `<=`, `>`, `>=`) that is a top-level WHERE conjunct,
    which rejects the row when either operand is NULL;
  - its block is an ungrouped aggregate (without GROUP BY, HAVING, LIMIT or OFFSET, as ADR 0022 requires of every
    scalar subquery) whose select item combines SUM, AVG, MIN and MAX calls with literals and strict operators: the
    arithmetic operators, unary minus included, each of which is NULL when an operand is NULL;
  - every outer column it reads (all of them in its WHERE conjuncts) has an inner equivalent: a column of the
    subquery's own FROM items in the same class.
- **The rewrite** (Lemmas 4.14 and 4.2): each outer column is replaced by its inner equivalent, so the correlated
  conjuncts read inner columns only and stay below the aggregation; an equality that became `c = c` is dropped, since
  it only removes rows whose `c` is NULL, a group that no outer row can match. The aggregation groups by the inner
  equivalents. The one-row dependent join becomes an inner join with one key per outer column, the outer column equal
  to its inner equivalent, and its build side follows ADR 0022's rule for inner joins. The comparison above it reads
  both inputs, so by ADR 0022's classification it becomes a residual, or another key when it is an equality whose
  common type is not DOUBLE.
- **Why the rewrite is exact.** Take an outer row whose correlated columns are not NULL. A row of the subquery passes
  its WHERE for that outer row exactly when it passes the rewritten WHERE and its inner equivalents equal the outer
  values, because the classes come from those very equalities. So the group with the outer values as its keys holds
  exactly the rows that the subquery aggregates for the outer row, and the outer row meets at most that one group:
  no outer row is duplicated, and a matched one gets the subquery's value. An outer row without a group (the subquery
  reads no row for it, or a correlated column is NULL) gets NULL in SQL from SUM, AVG, MIN and MAX; a strict
  combination keeps it NULL, and the comparison rejects the row, which the inner join drops too.
- **Why nothing else: the COUNT bug.** COUNT over no rows is 0, and CASE, COALESCE, AND and OR can turn NULL into a
  value, so the comparison could accept an outer row that has no group, which the inner join drops (R. A. Ganski and
  H. K. T. Wong, SIGMOD 1987). Those need a left join and a value for every outer row, as in the general algorithm,
  and exit 4.
- **Middle blocks are no special case.** In Q20 the dependent join lies inside the uncorrelated IN subquery's
  sub-plan, its left input is that block's FROM, and the pass reaches it like any other.
- **Divergence: groups that the outer query never uses are still computed.** DuckDB aggregates only the bindings that
  its outer side produces, while the aggregation here computes every group. An overflow that only such a group meets
  (in an aggregate's argument, the aggregate or the arithmetic that combines it) is an execution error in antb1 but
  not in DuckDB. U3 registers it in docs/sql-subset.md. Reducing the aggregation to the outer keys (a semi join below
  it) would remove the divergence, and waits for profiles of correlated aggregates or a user who meets it.

For example, this conjunct of a block that reads a table `o`:

```sql
o.v <= (SELECT max(t.x) - 1 FROM t WHERE t.k = o.k AND t.z = 1)
```

```text
Filter o.v <= s                                  Filter o.v <= s
  DependentJoin one-row                            Join inner  keys o.k = t.k
    Scan o                                           Scan o
    Compute s := m - 1                               Compute s := m - 1
      Aggregate m := max(t.x)                          GroupAggregate keys t.k  m := max(t.x)
        Filter t.k = o.k                                 Filter t.z = 1
          Filter t.z = 1                                   Scan t
            Scan t
```

### Exit code 4 for everything else

The binder or the pass rejects every other correlated shape that DuckDB answers with `kUnsupported` (exit code 4),
never with a bind error (exit code 1) or `NotImplemented` (exit code 70); DuckDB's bind errors stay bind errors, as
in ADR 0022:

- a correlated COUNT or count(DISTINCT);
- CASE, COALESCE, AND or OR over aggregates in a correlated scalar subquery;
- a correlated subquery under OR, in CASE or in a select list (these need mark or single joins), or in HAVING;
- an outer reference outside the WHERE conjuncts of its subquery's block, and one that skips a block;
- correlated IN and NOT IN, whose NULL semantics need the set's NULL and empty flags for each binding;
- an aggregate, GROUP BY, HAVING, LIMIT or OFFSET inside a correlated EXISTS or NOT EXISTS;
- a correlated EXISTS or NOT EXISTS without a key, and a correlated scalar subquery with an outer column that has no
  inner equivalent.

The rest of ADR 0022's list stands, the uncorrelated EXISTS included. The physical planner rejects a leftover
`DependentJoin` with `kUnsupported` too, never with `NotImplemented`: after Optimize none can arrive, but its visitor
must handle the node, and a missing feature must never look like an internal error
([ADR 0005](0005-error-boundary.md)).

### Execution

- **No new operator:** an unnested plan uses ADR 0022's semi, anti and inner joins and the existing GROUP BY, so
  EXPLAIN shows the unnested plan, and `explain --analyze` profiles it like any other.
- **No domain and no shared build,** so ADR 0013's re-scan rule holds: the outer side is computed once, a table that
  the outer block and its subqueries read is scanned once for each of them (Q2's four shared tables twice, the outer
  table of Q17 twice and that of Q21 three times), and each build feeds one probe pipeline. Every key comes from an
  equality that the query wrote, which a NULL never satisfies, so no key needs IS NOT DISTINCT FROM.
- **Deterministic** like every join and aggregation of ADR 0022: EXPLAIN and every answer are the same for any thread
  count.

## Consequences

- **What passes when:** with ADRs 0021 and 0022, U2 answers Q4, Q21 and Q22, and U3 Q2, Q17 and Q20, which completes
  22 of 22. Q17, Q20 and Q21 are meaningful tests only at SF 0.1, which the pass definition includes.
- **Exit codes:** a correlated subquery outside the two paths exits 4, each shape with a test, and every correlated
  subquery exits 4 until U2 or U3 adds its path. Optimize can fail now, so `explain` rejects what `query` rejects.
- **Correct plans that are not the fastest,** each deferred until its trigger:
  - an aggregation computes the group of every inner key, not only the groups that the outer side needs (in Q17 the
    scan of the inner table dominates anyway), until profiles of correlated aggregates or the divergence call for the
    semi-join reduction;
  - a table that the outer block and its subqueries share is scanned once for each of them, as ADR 0013 decided;
  - a semi or anti join with a residual keeps every build row (Q21);
  - an anti join builds on the subquery even where the outer side is smaller (Q22), until build-side output gets its
    ADR (ADR 0022).
- **Shapes deferred until a query needs them** (TPC-DS or user queries of the shape, each with an update of this ADR):
  the general domain join; mark and single joins (subqueries under OR, in CASE or in a select list; scalar subqueries
  that are not aggregates); correlated IN and NOT IN; correlated COUNT, and CASE or COALESCE over aggregates; IS NOT
  DISTINCT FROM keys; per-binding top-N; the uncorrelated EXISTS.
- **Tests:**
  - Our own star-schema fixtures (roadmap PR H6) bring the NULL keys, empty inner sides and duplicate keys that the
    TPC-H-derived data lacks, and a corpus of pending `onlyif duckdb` records whose expectations DuckDB writes:
    EXISTS and NOT EXISTS with non-equality residuals and outer-only conjuncts, and correlated aggregates over empty
    groups, a CASE over an aggregate included. U2 and U3 un-guard what they implement.
  - Named tests on those fixtures, never on the queries: an EXISTS that an inner join would count twice, the COUNT
    bug (exit 4), a NOT EXISTS with an outer-only conjunct, NULL keys on either side, and every exit-4 shape.
  - The random generator learns key-correlated EXISTS and NOT EXISTS, with an optional non-equality residual, and
    correlated SUM, AVG, MIN and MAX under NULL-rejecting comparisons (T3), before U2 and U3, so that the differential
    tests compare them from those PRs on. Two metamorphic relations are enforced from then: a correlated query
    equals its hand-written GROUP BY plus join rewrite, and the rows of an EXISTS plus those of the NOT EXISTS equal
    all outer rows.
  - Every test runs at 1 and 4 threads. Nothing derived from TPC-H is committed: no query text, data, answers or
    EXPLAIN output of the 22 queries.
- **Interfaces and modules:** U1 changes two public headers, the logical plan's and the optimizer's. Module edges stay
  as they are: the work lives in plan, with the error propagation in engine and the rejection in exec's physical
  planner.
- **Docs move with the code:** each PR updates the docs/sql-subset.md sections it changes, and U3 registers the
  divergence. ADR 0013's "Towards joins" is amended here: the binder plans dependent joins, and the first optimizer
  pass removes them.

## Plan

**The order of the subquery work** (amending the order proposed on 2026-09-29, which put this ADR after the dependent
join and kept per-binding top-N): the joins and unnesting ADRs first, in docs-only PRs; then parameterized types,
column ids, inner joins, derived tables and CTEs (bound per reference), and uncorrelated subqueries as joins (J5);
then the dependent join with a fallible Optimize (U1); then the two unnesting paths (U2 and U3). Per-binding top-N is
dropped from the order and deferred until a query needs it: no subquery of the workload has ORDER BY or LIMIT.

The PRs of this ADR, one each, by their roadmap ids; the roadmap's dependencies decide the order. Each PR updates the
docs/sql-subset.md sections it changes.

- **U1, refactor(plan): dependent joins for correlated subqueries.** After J5. Outer references through the scope
  chain, free variables over ids, the `DependentJoin` node in every visitor and in EXPLAIN, a fallible
  `plan::Optimize` whose unnesting pass rejects every dependent join, and the physical planner's rejection. No query
  passes yet; correlated shapes exit 4, not 1 or 70.
- **T3, test(diff): generate correlated subqueries.** The generator and the two metamorphic relations, pending until
  U2 and U3.
- **U2, feat(plan): unnest correlated exists and not exists.** Path 1, the `*` rule and the EXISTS shapes that exit 4.
  Q4, Q21 and Q22 pass.
- **U3, feat(plan): unnest correlated scalar aggregates.** Path 2, the scalar shapes that exit 4 and the divergence.
  Q2, Q17 and Q20 pass.

U2 and U3 can run side by side after U1 and T3. Roadmap PR A1 moves this ADR to Accepted, together with ADRs 0013,
0021 and 0022, once all 22 queries pass (decision C14).

## Alternatives considered

- **Kim- and Dayal-style rewrites in the binder** (W. Kim, ACM TODS 1982; U. Dayal, VLDB 1987): one rewrite per
  subquery shape, applied while binding. Fewer lines for the six queries, but each new shape adds binder code that a
  general approach would throw away, the binder is the file that most roadmap PRs edit already, and each rewrite has
  to avoid the COUNT bug by hand. A dependent join keeps the binder general: the pass can add cases, or the general
  algorithm, without touching it. Rejected (decision C5).
- **The general algorithm with a duplicate-free domain now:** it covers every shape, COUNT and OR included. But the
  domain is the outer side's result, read twice. Under ADR 0013 that needs one build probed by several pipelines
  (DuckDB's delim join; deferred by ADR 0022) or an outer side computed twice, keys that treat NULL as equal, and
  optimizer passes that understand shared nodes. No query of the workload needs it, because the two cases cover all
  six. Deferred; the dependent join and the top-down pass are its starting point.
- **Mark and single joins:** a mark join appends a three-valued "has a match" column, so that a subquery can sit
  under OR, in CASE or in a select list, and correlated IN and NOT IN get their NULL semantics; a single join pads
  like a left join and fails on a second match, for scalar subqueries that are not aggregates. They need new join
  kinds in exec and a run-time check, for shapes that no query of the workload has (decision C16). Deferred.
- **Per-binding top-N:** a correlated subquery with ORDER BY and LIMIT, such as the first row for each outer row. The
  formalization has no push-down rule for sort or limit, so it needs a top-N per binding, like a window function. No
  query of the workload has one, so it is dropped from the order and deferred.
- **Running the subquery once per outer row:** exact for every shape, but it costs the outer rows times the subquery,
  and ADR 0013 rules it out: the executor never runs a subquery per row. Rejected.

This workload is derived from the TPC-H Benchmark and is not comparable to published TPC-H Benchmark results, as
this implementation does not comply with all requirements of the TPC-H Benchmark.
