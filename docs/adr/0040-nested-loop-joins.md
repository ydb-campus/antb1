# 40. Nested-loop joins and cross products

Date: 2026-11-07

## Status

Proposed (2026-11-07). This is a design ADR the maintainer asked for ahead of the code (the ADR README's exception).
Its items are [ADR 0037](0037-tpcds-roadmap.md)'s OJ4, split three ways:

- OJ4a: the executor;
- OJ4b: inner cross products and joins without an equality key, in the binder and the join order;
- OJ4c: LEFT and FULL JOINs without an equality key.

It is accepted when OJ4c is built.

## Context

- **What antb1 answers today.** A join graph that no equality key connects is a cross product, and exits 4
  ([ADR 0022](0022-joins-and-query-blocks.md), "Connected join graphs only": "Cross products have no design yet").
  The one exception is a relation of exactly one row, which joins above the block as a `ONE-ROW` join, a build of
  that single row appended to every probe row.

  A condition between two relations that is not an equality of one key type, such as `a.x < b.y`, `a.x <> b.y`, an
  OR, or a DOUBLE equality, is a residual. Residuals connect nothing, so two relations that only such a condition
  relates are a cross product and exit 4 too. So does a LEFT or FULL JOIN whose ON has no equality key.
- **The queries derived from TPC-DS.** One of the 99, Q77, needs a cross product. It crosses two grouped relations
  that have no condition between them. Every other join of the 99 has an equality key. No query is quoted
  ([ADR 0006](0006-test-strategy-and-data-policy.md)).
- **The maintainer's direction (2026-11-07):**
  - Engine parts are generic, not shaped to the corpus.
  - Joins without an equality key are answered whatever their conditions: inequalities and any other residual.
  - Inner, LEFT and FULL joins are all covered.
  - The join-order cost search places cross products, rather than only putting them above the key-connected
    relations.
  - There is no size guard.
- **DuckDB 1.5.6**, checked on synthetic data:
  - It answers inner, LEFT and FULL joins on `<`, `<>`, OR and no condition at all.
  - It plans them as `CROSS_PRODUCT`, `NESTED_LOOP_JOIN`, `BLOCKWISE_NL_JOIN` (an OR, or `ON true`),
    `PIECEWISE_MERGE_JOIN` (inequalities) or `IE_JOIN` (two inequalities, at some sizes).
  - A condition that is NULL is false, so a LEFT JOIN pads the left rows no pair passes, and a FULL JOIN pads the
    right rows too.
  - It has no output-size guard.
- **The executor already has most of a nested loop.** A keyless `JoinTable` (`JoinBuildSpec::Keyless`) holds any
  number of rows as rows of one key. Each probe row's candidates are a range into the build's rows
  (`JoinMatches{begin, end}`).

  The inner, semi, anti, LEFT and FULL paths walk those ranges in output batches of at most `batch_size` pairs. They
  evaluate the residuals on the pairs, and pad and mark as each kind does. No path holds all m×n pairs.

  What stops a keyless multi-row join today:
  - The plan contract: the trait `keyless` means "exactly one row", and the checks enforce it.
  - The executor's keyless build: `JoinBuildSpec::Keyless` keeps every column of its input, where a keyed build
    keeps only the ones read above. It also never keeps NULL-key rows, which the operator requires of a FULL join
    built on its right input.
  - `Pull` looks every probe row up with `Find`, which is Invalid on a keyless build.

## Decision

### Semantics

1. **A join with no equality key is a nested loop.** Every build row is a candidate of every probe row. The join's
   conditions are its residuals, evaluated on the candidate pairs exactly as a hash join's residuals are: in the
   order written, each on the pairs the earlier ones passed, with NULL as false. With no condition, the join is a
   cross product: every pair passes.
2. **Inner, LEFT and FULL joins may be keyless.**
   - An inner join keeps the passing pairs.
   - A LEFT join also pads each left row that no pair passes, once.
   - A FULL join also pads each right row that no pair passes, once, after the probe, as OJ2's FULL join does.
   - An empty build empties an inner join; a LEFT or FULL join then pads every left row.
3. **Semi, anti and null-aware anti joins stay keyed.** Unnesting still needs a key, so a correlated subquery whose
   correlation has no equality exits 4 as today ([ADR 0023](0023-unnesting.md)).

### The plan

1. **The traits** ([ADR 0037](0037-tpcds-roadmap.md)'s OJ1 table, `join_kind.h`):
   - `keyless` is renamed `one_row`: the right input is exactly one row, with no keys and no residuals.
   - A new trait, `nested_loop`, means that the kind may run with no keys. It is true for inner, LEFT and FULL.
   - A `JoinNode` has at least one key unless its kind is `one_row` or `nested_loop`.
   - A keyless join is still built where its kind may be built: an inner one on either input, LEFT and FULL on the
     right.
2. **EXPLAIN** prints a keyless join as `Join INNER build=… keys=[]`, with its residuals. The profile keeps the
   operator names `HashBuild` and `HashJoin`, and adds `pairs` for the inner path.
3. **The binder no longer refuses a disconnected join graph** (OJ4b). Its relations go to the join order with their
   key edges as today. A residual still joins at the lowest join that has all of its relations, whether that join
   is keyed or a cross. One-row relations keep their `ONE-ROW` join above the block, unchanged.

   A LEFT or FULL JOIN without a key becomes a keyless join of its unit (OJ4c). Every ON conjunct is then a
   residual, except a LEFT JOIN's right-only conjunct, which rule 11 still pushes into the right input.

### The join order

1. **The cost search also considers cross products.** DPccp ([ADR 0026](0026-join-performance.md)'s C5) enumerates
   the connected complement pairs as today, whose two sides are each connected. It then also considers every other
   split of a subset into two sides that both have a plan. Such a split is either of two kinds:
   - **a cross split**, when no edge connects its two sides: a step with no edges, which the binder builds as a
     keyless inner join;
   - **a keyed split with a side that is not connected**, such as a cross of two small relations on one side and a
     large relation that an edge joins to both on the other. It is keyed on every edge between the sides, as `Emit`
     already collects them.

   Every split is costed alike:
   - cost(S) = cost(S1) + cost(S2) + rows(S) + min(rows(S1), rows(S2));
   - rows(S) of a cross split is the product, which is also the pairs its nested loop evaluates. Residuals have no
     selectivity in today's estimates.

   Within one subset, the DPccp pairs are applied first, and any other split replaces the best only when it is
   strictly cheaper. So a plan changes only where a split that reads a cross is estimated to cost less. The typical
   case is two small relations crossed below a large one that joins both on a two-column key.

   Every split of up to 12 relations is about 3^12 / 2 ≈ 2.7·10^5 pairs, which is cheap next to the query.
2. **The greedy order** is used beyond 12 relations, or with `--join-order greedy`. It keeps rules 1 to 5 over the
   connected relations. Only when no connected relation is left does it cross the remaining relation with the
   smallest estimate (an unknown one counts as the largest). It never crosses where a key could join.

### The execution

1. **The operator is the hash join's.** A keyless build takes `JoinBuildSpec::Keyless`, which OJ4a extends:
   - it keeps only the columns read above, as a keyed build does;
   - a FULL join's keyless build is accepted although it keeps no NULL-key rows, since it has no keys.

   `Pull` gains a keyless path: no hash and no `Find`, but each selected probe row's candidates are the whole
   build, and an unselected row's none. Over those ranges, the inner, LEFT and FULL paths run as they do for a hash
   join, with the residuals, the padding, the marks, the part pipelines and the shared build. The probe streams in
   batches, and the build is charged to the memory budget like any build.
2. **A keyless build skips the hash directory and the per-row hashes**, which it never reads, as long as its rows
    stay in (part, row) order (OJ4a). Otherwise this becomes a performance item of its own.

### No size guard

 1. **A cross product is not refused for its size.** The output streams in batches, and running out of memory is an
    error. What is held in memory:
    - **The build.** For an inner join it is the side the cost search or rule 5 chose. A LEFT or FULL unit always
      builds its right input, whatever the sizes, so a keyless LEFT JOIN with a huge right input holds all of it.
    - **A cross below a keyed join.** In the typical case of the cost search, the cross's whole m × n output is the
      build of the keyed join above it: materialised and hashed, not streamed.
    - **A part pipeline that ends in a union.** It buffers each part's whole output until that part is handed on, as
      it does for any fan-out join, and up to 2 × threads parts at once until budget pressure shrinks that window. A
      cross product's output is m × n rows per part, so a single part whose output exceeds the budget fails with
      out-of-memory. An aggregate, top-N or LIMIT above the join consumes its batches as they come.

## Consequences

- **Q77 passes once OJ4b is built.** No other query of the 99 changes its answer.

  Where a split that reads a cross is estimated cheaper, plans of queries that already pass may change. OJ4b compares
  the EXPLAIN of every passing query derived from TPC-H and TPC-DS before and after the change, locally, and times each
  changed plan. A cross that loses is pinned and the cost is fixed before OJ4b lands.
- **The random differential test** gets a cross feature (OJ4b) and keyless LEFT and FULL promotions (OJ4c), each on
  a random stream of its own.
- **The exit-4 cases left, for joins that subqueries make:**
  - a correlated subquery whose correlation has no equality, as an EXISTS or IN (a semi or anti join) and as a
    scalar value ([ADR 0023](0023-unnesting.md)'s path 2);
  - an uncorrelated IN or NOT IN whose two sides have no common key type, such as DOUBLE.
- **An inequality join evaluates every pair.** This is O(m × n), where a piecewise merge join or an IEJoin would
  sort. No query of the 99 needs those, and they stay possible later behind the same plan shape.

  On the inner path, every output column is gathered before the residuals run. OJ4a measures a selective
  inequality join. If the gathers dominate, a performance item follows that evaluates the residuals on their own
  columns first, as the semi, anti, LEFT and FULL paths already do.

## Alternatives considered

- **Cross the key-connected components only at the top of the block**, each ordered on its own, smallest first.
  This is simpler and changes no existing plan. The maintainer chose the cost search instead, which can also cross
  two small relations below a large one.
- **A dedicated nested-loop operator.** Rejected: the hash join's pair paths already do everything a nested loop
  does once every build row is a candidate, including the parallel probe, the residuals, padding, marks and the
  memory budget.
- **An output-size guard, refusing a cross product above an estimate.** Rejected by the maintainer, as in DuckDB and
  PostgreSQL: estimates are often unknown, and any threshold would be arbitrary.
- **Piecewise merge join and IEJoin for inequality joins.** Deferred: they are performance work over the same plan
  shape, and no query of the 99 needs them.

## Updates

- Update (2026-11-08, OJ4a): built. The trait `keyless` is now `one_row`, and `nested_loop` is true for inner, LEFT
  and FULL joins. A plan's join of those kinds may have no keys. `JoinBuildSpec::Keyless` takes the columns to
  retain. A FULL join accepts a keyless build. `JoinTable::FindEvery` gives each selected probe row the whole build
  as its candidates, so the operator's paths run unchanged over them. The inner path reports `pairs` when it has
  residuals.

  A keyless build hashes nothing and builds no directory (decision 10). Its rows go to one partition as they come,
  and a batch whose rows are all selected is kept without a copy. The table lists them in (part, row) order. This
  holds for a one-row join's build too.

  No SQL reaches a nested loop yet: OJ4b's binder does. The measurement of a selective inequality join moves to
  OJ4b, where SQL reaches the operator and `explain --analyze` splits the gather time from the residual time.
