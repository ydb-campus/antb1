# 41. Subqueries and full decorrelation

Date: 2026-11-10

## Status

Proposed (2026-11-10), as a design ADR the maintainer asked for ahead of the code (the ADR README's exception).

Its items are [ADR 0037](0037-tpcds-roadmap.md)'s phase 8 in a new order (SQ4a, SQ1, SQ2, SQ7, SQ0, SQ5 and SQ6, SQ8;
SQ3 is absorbed into SQ4a) and a new phase 10 after the window functions of phase 9 (SQ4b and SQ9 to SQ17), with PF4,
PF7 and a new PF16 as guards. It extends [ADR 0023](0023-unnesting.md): Path 1 and Path 2 become rules of one
unnesting pass, and phase 10 builds the general algorithm that ADR deferred.

## Context

### What antb1 answers today

- **Uncorrelated subqueries** ([ADR 0022](0022-joins-and-query-blocks.md)) stand only as a top-level conjunct of
  WHERE, of an inner ON or of HAVING: `x [NOT] IN (q)`, `NOT (x IN (q))`, and `x <op> (q)` with q an aggregate
  without GROUP BY, HAVING, LIMIT or OFFSET as a direct operand. IN is a semi join and NOT IN a null-aware anti join.
  The scalar is a one-row join (`JoinKind::kOneRow`), which appends the subquery's single row to every probe row, with
  the comparison in a Filter above it.
  - The one-row join is exact: its build must hold one row, or its `Prepare` returns `Invalid`
    (`src/exec/hash_join.cc`), a planner bug that exits 1 today like a query error. One-row derived tables in FROM
    share the kind.
  - An uncorrelated EXISTS, a subquery under OR or CASE or under a NOT other than `NOT (x IN (q))`, and a subquery
    anywhere else exit 4.
- **Correlated subqueries** (ADR 0023) become dependent joins: semi (EXISTS), anti (NOT EXISTS) or one-row (a
  scalar). The binder puts every correlated conjunct of the subquery's block into one filter above the block's joins,
  which is [NK15] §3.1's pull-up done at bind time: nothing below that filter reads an outer column. The first pass
  of `plan::Optimize` then replaces each dependent join through one of two paths, each an exact match on a stack of
  nodes (`BlockOf` and `ScalarBlockOf` in `src/plan/optimizer.cc`):
  - **Path 1** (EXISTS and NOT EXISTS over a plain block) makes each conjunct of the filter a key (an `=` of one key
    type, or IS NOT DISTINCT FROM, NULL-equal) or a residual over candidate pairs; the conjuncts the binder placed
    below the filter stay below the build. It needs a key.
  - **Path 2** (SUM, AVG, MIN and MAX, combined strictly, under a comparison) builds equivalence classes from the
    filter's equalities of one type, substitutes each outer column by an inner equivalent ([N24] Lemma 4.2), groups
    the aggregation by the equivalents and joins back INNER, with a semi-join reduction over a clone of the outer side
    where [ADR 0029](0029-dynamic-join-filters.md)'s D5 cannot restrict the groups.
  - The INNER join back is exact only because the binder puts a comparison, which rejects NULL, directly above the
    dependent join, and because the item is NULL over no rows (`NullOverNoRows`, ADR 0023's update of 2026-11-08).
    The optimizer never looks above the dependent join.
  - Everything else exits 4: COUNT, CASE or COALESCE over the aggregate; scalars that are not aggregates; correlated
    IN; subqueries under OR or CASE or under a NOT other than NOT EXISTS, in a select list or in HAVING; aggregates,
    GROUP BY or LIMIT inside EXISTS; correlations without a key or an inner equivalent; references further out than
    the enclosing block.

### DuckDB 1.5.6

- **An uncorrelated scalar** is FIRST and COUNT(*) under a CASE that raises "More than one row returned by a subquery
  used as an expression" for two rows or more, crossed with the outer side. Every build runs, whatever turns out
  empty, so the error fires with an empty outer side, next to an empty file, a filter outside a column's range or
  join keys outside the other side's range, under an OR and in a CASE branch that is never taken. The optimizer
  deletes the subquery, and its error with it, only where its filter combiner proves a condition never true in the
  block the subquery is written in (a contradiction pair such as `a = 1 AND a = 2`, an empty BETWEEN, a constant
  false; also inside an IN subquery there), where an IN subquery there has `LIMIT 0`, or where nothing reads the
  derived-table or CTE column that holds it (decision 24).
- **An uncorrelated EXISTS** is COUNT(*) over LIMIT 1, compared with 1 and crossed; never a mark.
- **IN and `= ANY`** are a three-valued MARK join. Where the mark is a conjunct it becomes a SEMI join, and a NOT over
  it an ANTI join only where its condition is NULL-safe, so NOT IN stays a mark under a NOT.
- **Correlated subqueries** are delim joins over the domain D, which the optimizer removes unless a selection under
  the delim join (a condition of the subquery besides the correlation, or a filter of the outer side) or a LEFT join
  of D with the aggregation (as for COUNT) keeps it (D24). A scalar is a SINGLE join that checks each probe row;
  EXISTS and IN are MARK joins; the join back never drops an outer row, and COUNT is read through
  `CASE WHEN count IS NULL THEN 0`.
- **Its decorrelation answers some shapes wrongly** (decision 29).

### The evidence

- **Probes in both engines.** A design discussion of 2026-11-09, which followed a review of 2026-11-08 (neither is
  committed), ran 815 probes on synthetic Parquet tables in antb1 and in DuckDB: 344 distinct subquery shapes of forms
  and placements, of correlations, and of NULL, row-count, error, type and thread semantics. It also read the papers
  below and DuckDB's source. On 2026-11-10 a fact-check re-ran about 140 of its behaviour claims in both engines and
  checked about 160 citations.
- **No subquery-specific wrong answer in antb1 today.** The only wrong rows are NaN through MIN or MAX (divergence
  D10). Six shapes exit with the wrong class (decision 23), and two fail where DuckDB answers, under the registered
  divergences D4 and D24. Every other refusal is a clean exit 4.
- **A few mechanisms cover the shapes:** a one-row value; its placement in a clause; marks; one unnesting pass whose
  terminal step is substitution, with a LEFT join back for an aggregate that returns a row over no input; and, for
  what substitution cannot express, the single join and the domain D.
- **The papers,** cited by their LNI pages ([N24], an arXiv paper, by its lemmas):
  - [NK15] T. Neumann, A. Kemper: *Unnesting Arbitrary Queries*, BTW 2015.
  - [N24] T. Neumann: *A Formalization of Top-Down Unnesting*, arXiv 2412.04294, 2024.
  - [N25] T. Neumann: *Improving Unnesting of Complex Queries*, BTW 2025.
  - [NLK17] T. Neumann, V. Leis, A. Kemper: *The Complete Story of Joins (in HyPer)*, BTW 2017.

### The nine queries of phase 8

From DuckDB's parse trees of the queries derived from TPC-DS, node kinds, operators and types only; no text is quoted
([ADR 0006](0006-test-strategy-and-data-policy.md)):

- **Q58, Q14 and Q54** need uncorrelated scalar subqueries that today's syntactic rule refuses: ones that are not
  aggregates as direct `=` operands (Q58), ones that select a CTE's column as direct `>` operands in HAVING (Q14), and
  two as the bounds of a BETWEEN (Q54). The IN conjuncts of Q58 and Q14 are answered today.
- **Q9** needs uncorrelated scalars in select items, CASE among them, all aliased.
- **Q45** needs an uncorrelated IN under OR, on VARCHAR keys.
- **Q6** needs an uncorrelated scalar that is not an aggregate, and a correlated aggregate inside arithmetic,
  correlated by one `=` of VARCHAR columns.
- **Q41** needs a correlated COUNT as a direct operand of `>`, correlated through an OR whose two branches share one
  `=` of VARCHAR columns.
- **Q10 and Q35** need correlated EXISTS under OR, each correlated by one `=` of BIGINT columns, with no residual.

None needs a single join, D, a keyless join, a subquery in an outer join's ON or a quantified comparison.

### The maintainer's direction (2026-11-10)

- The order of phase 8 below, with ADR 0037's Plan updated to match.
- Errors next to joins that turn out empty: register one divergence family, and reorder no join.
- Full decorrelation after Neumann must be built: a new phase 10 after the window functions, of which every phase 8
  item is a subset, so that nothing is reworked. Full DuckDB coverage may follow it.
- SQ0 is a refactor that changes no answer.
- The design discussion's smaller questions are answered as the decisions below record.

## Decision

### Scope and order

**1. Phase 8, in this order.** The uncorrelated items come first: they are independent, carry little
risk and complete five queries before the riskiest change. SQ7 comes before SQ0, since its mark needs neither
correlation nor the framework, and its audit of the join-kind traits then serves SQ8. SQ0 comes before any correlated
widening, so that the consumer proof (decision 20) exists before a correlated value reaches a new placement.

| Id | Item | Completes | Ratchet |
| --- | --- | --- | --- |
| SQ4a | Uncorrelated scalar subqueries of any shape as direct comparison operands, through the single-value aggregate (decision 4); uncorrelated EXISTS as a conjunct (5); the BETWEEN split (6); a set operation as a scalar's or an EXISTS's query (7); the column count checked first, and the one-row check at exit 70 (23). It absorbs SQ3 | 58, 14, 54 | 78 |
| SQ1 | Uncorrelated subqueries as values in conditions (8), with the binding machinery: the slots (9), each subquery bound once (10) | | 78 |
| SQ2 | Uncorrelated subqueries as values in the select list, ORDER BY, GROUP BY and aggregate arguments (8); their names (11) | 9 | 79 |
| SQ7 | Uncorrelated IN, NOT IN, `= ANY` and `<> ALL` in any condition, through `kNullAwareMark` under OR, NOT or CASE (12 to 14) | 45 | 80 |
| SQ0 | One rule table in place of Path 1 and Path 2, substitution as its terminal step, and the consumer proof; no answer and no plan of an answered shape changes (16 to 18, 20) | | 80 |
| SQ5, SQ6 | One PR or two. SQ6: the static-aggregate rule, a LEFT join back with empty values (19, 21). SQ5: correlated aggregates as values in the per-row slots, under the proof (21) | 6, 41 | 82 |
| SQ8 | Correlated EXISTS in any condition, through `kMark` under OR, NOT or CASE, and over any block; correlated IN and NOT IN as conjuncts (12, 13, 15) | 10, 35 | 84 |

If SQ5 and SQ6 split, SQ6 lands first: SQ5's scope, correlated aggregates of any kind with any select item, needs
SQ6's LEFT join back. Each query needs only its own half: SQ6 completes Q41, and SQ5 Q6. ADR 0037's Plan changes with
this ADR: SQ3 goes, SQ4 becomes SQ4a here and SQ4b in phase 10, and SQ0 is new.

**2. Full decorrelation, as phase 10.** Neumann's top-down decorrelation ([N25], with [NK15]'s rules and [N24]'s
proofs) is built in full, as a new phase 10 of ADR 0037 after phase 9's window functions, with ids of its own. Every
phase 8 item is designed as a subset of it, with the same rule table, join kinds, traits and empty values, so that
phase 10 adds rules, kinds and a second terminal step and reworks nothing. Phase 10 completes none of the 99 queries,
which ADR 0037 completes with phase 9; it answers general SQL that DuckDB answers.

| Id | Item | Needs |
| --- | --- | --- |
| SQ4b | The single join, `kSingle` (decision 22): correlated scalar subqueries that are not aggregates, grouped ones included, checked per probe row | SQ0, SQ8 |
| SQ9 | The domain D, the second terminal step (decision 18), for correlations that substitution cannot express. Relational division is the general-SQL shape that needs it: a double NOT EXISTS, or NOT EXISTS over an EXCEPT whose left arm is uncorrelated. Also non-equality and OR correlations below an aggregate, outer columns inside an aggregate's argument, references two blocks out that the middle block does not equate, and the input that reads no outer column of a join that must be unnested on both inputs (decision 17) | SQ0 |
| SQ10 | Correlation forms that need no D: keys over computed expressions; widened keys of two types; an outer-only conjunct of a scalar aggregate, lifted out under the consumer proof and otherwise kept as `CASE WHEN p(o) THEN value ELSE <empty value> END` above the join; substitution through a middle block's classes; a correlation inside a derived table or CTE body nested in the subquery; a join whose two inputs both read outer columns, unnested on both by substitution with their representatives equated NULL-equal (decision 17); maps in select items moved above the join ([N24] Lemma 4.9); and SQL's rule that an aggregate whose arguments are only outer columns is the outer block's, built before outer references are admitted in select lists, with DuckDB's bind error for one in a subquery's select list where the subquery stands in the enclosing WHERE, ON or GROUP BY (decision 23) | SQ0, SQ6 |
| SQ11 | Keyless semi, anti and mark joins, amending [ADR 0040](0040-nested-loop-joins.md)'s decision 3: correlated EXISTS, NOT EXISTS, IN and NOT IN whose correlation has no equality, which need no D ([N24] Lemma 4.8) | SQ8 |
| SQ12 | Correlated marks: a correlated IN, NOT IN, `= ANY` or `<> ALL` under OR, NOT or CASE, three-valued, with per-binding NULL and empty flags. With SQ8's `kMark` for EXISTS, every correlated IN and EXISTS with a key is then answered in any condition | SQ8 |
| SQ13 | Correlation above an aggregation: free variables remapped to the GROUP BY or grouping-sets output, NULL where rolled up; correlated subqueries in HAVING and in a grouped block's select list; an aggregate of the enclosing block inside a subquery that stands in the enclosing HAVING or select list, and DuckDB's bind error for one in a subquery's HAVING where the subquery stands in the enclosing WHERE, ON or GROUP BY (decision 23) | SQ0, SQ5 |
| SQ14 | Per-binding LIMIT and OFFSET: a ROW_NUMBER partitioned by the representatives ([N25] §4.4) | phase 9; SQ9 for an ORDER BY over an outer column without an equivalent |
| SQ15 | Set operations with correlated arms: an uncorrelated right arm of EXCEPT or INTERSECT without D (for the DISTINCT forms, a NULL-equal anti or semi join of the left arm, then the deduplication; for EXCEPT ALL and INTERSECT ALL, per-binding counts of each arm); a UNION arm through D or a decomposition (EXISTS over a UNION is an OR of EXISTS, and IN distributes over UNION) | SQ9 |
| SQ16 | Grouping sets that mix `()` with other sets inside a correlated block, which is static and multi-row at once, with GROUPING()'s empty value | SQ4b, SQ6 |
| SQ17 | Correlated values in an aggregate's FILTER clause and in window arguments, read with the empty values | phase 9; FILTER itself |

The ids follow the order in which the maintainer listed the items. They name items; they are not a build order (an
open decision).

**3. Full DuckDB coverage after phase 10.** The shapes DuckDB answers that no phase covers follow phase 10, listed
without ids until they are planned, each exit 4 until then:

- subqueries in the ON of a LEFT or FULL JOIN;
- the quantified comparisons other than `= ANY`, `= SOME` and `<> ALL`, such as `> ALL`, `< SOME`, `= ALL` and
  `<> ANY`;
- LATERAL;
- BOOLEAN values: a condition as a value, IN and EXISTS as select items included;
- a deparser in DuckDB's style, for the names of unaliased subquery items;
- DOUBLE join keys, with a rule for NaN;
- row values;
- LIST types;
- recursive CTEs inside subqueries.

### Uncorrelated values

**4. The single-value aggregate (SQ4a).**

- A new `AggKind`. Over no row it is NULL; over one row, that row's value; over two rows or more it fails with an
  `ExecutionError` (exit 1) whose message begins with DuckDB's first sentence, "More than one row returned by a
  subquery used as an expression". A NULL value counts as a row: it is the first aggregate of a column that does not
  skip NULL values, an exception the contract of `src/exec/include/antb1/exec/aggregate_state.h` gains.
- Its state is a row count and the first value, and merging adds the counts. It returns a value only for exactly one
  row, so its result depends neither on the order of the parts nor on the number of threads.
- It wraps an uncorrelated scalar subquery's plan under today's one-row join, which stays exact: one-row FROM items
  share that join, and for them zero rows is an empty product, not a NULL. `ExactlyOneRow` in `src/plan/binder.cc`
  skips the wrapper over a plan that is exactly one row, so every scalar subquery answered today keeps its plan and its
  EXPLAIN goldens.
- The aggregate is on the build side, and builds are prepared before the probe reads a row, so the error fires over an
  empty outer side, as in DuckDB (within decision 24's limits).
- No `LIMIT 2` goes below it: DuckDB reads every row, and a limit would change which error a failing subquery reports.
  DuckDB's setting `scalar_subquery_error_on_multiple_rows` is not offered; antb1 follows its default.
- Every switch over `AggKind` handles the kind: strictness, aggregate placement, the exec states, grouping sets and
  the empty values (decision 19).

**5. Uncorrelated EXISTS (SQ4a).** `EXISTS (q)` is COUNT(*) over `LIMIT 1` of q, a one-row value under the one-row
join compared with 1, and NOT EXISTS compares it with 0. It is DuckDB's plan, and never a mark, which would keep every
probe row and take no dynamic filter for a value that is the same for every row. Since it reads any plan, no block is
special: over an ungrouped aggregate it is always true, and over `LIMIT 0` always false. SQ4a answers it as a
top-level conjunct of WHERE, an inner ON and HAVING, and inside a scalar subquery; SQ1 under OR, NOT or CASE in a
condition, and SQ2 in a CASE of the select list. As a select item it is a BOOLEAN value (decision 3).

**6. The BETWEEN split (SQ4a).** A BETWEEN whose bounds are subqueries is split into its two comparisons
(`BetweenComparisons`) before the check of subquery conjuncts, so that each comparison is a conjunct with a subquery
as a direct operand. That answers Q54 without SQ1. A NOT BETWEEN, arithmetic on a bound, and a subquery as the
BETWEEN's operand, which the split would bind twice, wait for SQ1.

**7. A set operation as a subquery's query (SQ4a, SQ7).** A UNION, INTERSECT or EXCEPT, ALL or not, is answered as
the query of a scalar subquery or an EXISTS in SQ4a, since the wrapper and the LIMIT read any plan, and as the query
of IN in SQ7, as long as no arm is correlated. [ADR 0036](0036-union-all.md)'s X6 is then done for uncorrelated arms;
correlated arms are SQ15.

**8. Values in every clause DuckDB answers (SQ1, SQ2).** An uncorrelated subquery is a value wherever DuckDB answers
one, in two PRs, each a category of SQL:

- **SQ1, in conditions:** WHERE, an inner ON (also inside a FULL JOIN's left input, which ADR 0037's OJ3 update left
  to this phase) and HAVING, under AND, OR, NOT and CASE, in arithmetic, functions, COALESCE and casts,
  IS [NOT] NULL, IS [NOT] DISTINCT FROM, IN lists and BETWEEN. A conjunct inside a correlated EXISTS that holds one
  and also reads the enclosing block becomes a plain residual for Path 1.
- **SQ2, as values:** select items (CASE included), ORDER BY, GROUP BY keys, aggregate arguments, the select list of
  SELECT DISTINCT, and the select list of a correlated EXISTS, which Path 1 then drops. Q9 completes.
- The newly opened places raise DuckDB's own binder errors, at exit 1, where DuckDB raises one, such as an ORDER BY
  over an expression that a set operation does not output, a subquery in a set operation's LIMIT, and a HAVING that
  reads a select alias that is not grouped.
- A subquery inside a grouping-set element (ROLLUP, CUBE, GROUPING SETS) is refused with exit 4, as a constant there
  is (ADR 0037's RU3 and RU5 updates, and the grouping-sets rules of [docs/sql-subset.md](../sql-subset.md)).
- Still exit 4: a subquery in the ON of a LEFT or FULL JOIN (decision 3). LIMIT and OFFSET given by a subquery, and a
  subquery as the whole of an aggregate's argument, wait for general features that no item plans (an open decision).
- Correlated values stay where ADR 0023 answers them until SQ5 places them in the per-row slots under the consumer
  proof, and out of the slot after the aggregation until SQ13 remaps their free variables. Without the proof,
  `COALESCE((q), 0) < o.v` would lose rows silently.

**9. Slots, and eager evaluation (SQ1).**

- Each block gets two slots for the joins of its subquery values:
  - **per row**, between the block's joins and its input Compute, for the uses that read rows: WHERE, an inner ON,
    GROUP BY keys, aggregate arguments, and the select items and ORDER BY of a block that does not aggregate;
  - **after the aggregation**, directly above it and below the Computes over it, for the uses that read groups:
    HAVING, and select items and ORDER BY over aggregates.

  DuckDB places them the same way. A slot after the windows (QUALIFY, items over windows) belongs to phase 9, and
  the FILTER and window-argument slots of correlated values to SQ17.
- Within a slot the joins stack in written order, as today; none is moved for errors (decision 24).
- A conjunct that holds a subquery is never routed to one relation of the block: it is filtered above its slot. A
  subquery counts as an operand, so `(q) > 5` is not a comparison of two literals.
- A slot's join computes its value for every row before an AND, OR or CASE above it decides. So a subquery's row
  error in a CASE branch that is never taken, or under an OR whose other branch is true, fails as in DuckDB. D16's
  lazy AND and OR apply to the expression over the appended column, and SQ1 says so in D16's text.

**10. Each subquery bound once (SQ1).**

- `ExprName` names a subquery instead of aborting: today it reaches `RejectedSubquery`, an `ANTB1_CHECK`, which only
  `CheckSupported`'s earlier refusal keeps away.
- The binder binds one SQL expression more than once: `BindOutput`'s speculative `BindInput`, an alias bound again,
  BETWEEN's operand and bounds, and the deep copy of a folded comparison. Each binding would plant a join. A memo keyed
  by (block scope, binding mode, structure) binds each subquery once per scope and mode: GROUP BY matching needs both
  bindings of a select item to return one column, and WHERE and HAVING bind at different levels.
- Keyed by structure (`sql::EqualIgnoringSpans`), the memo also merges subqueries written alike in one scope and mode.
  That saves joins, not scans: [ADR 0030](0030-shared-sub-plans.md) already computes an identical sub-plan topped by
  an aggregate once where its cost gate admits it.
- OR factoring's bind-only path binds a branch and then drops it, so it runs as a dry run that registers no join.

**11. Names of unaliased subquery items (SQ2).** DuckDB names an unaliased subquery item by a deparse of the whole
subquery, such as `(SELECT max(k) FROM u)`. antb1 names it its own way, under divergence D8, which SQ2 extends: the
tests compare values and types, not names. A deparser in DuckDB's style is a coverage item (decision 3). All of Q9's
subquery items are aliased.

### Marks

**12. Two mark kinds.**

- **`kNullAwareMark` (SQ7)**, for uncorrelated IN, NOT IN, `= ANY` and `<> ALL` under OR, NOT or CASE; as a
  top-level conjunct they keep their semi and null-aware anti joins (decision 14), so SQ7 answers every condition.
  Per probe row, three-valued: TRUE on a match; else FALSE when the set is empty, even for a NULL probe; else NULL
  when the probe is NULL or the set holds a NULL; else FALSE. The build's `has_null` and `empty` flags decide it, as
  they decide the null-aware anti join today. It has one `=` key, no residual and no NULL-equal key. NOT IN is a NOT
  over the mark, which inverts it three-valued.
- **`kMark` (SQ8)**, for correlated EXISTS under OR, NOT or CASE, through the correlated filter's rule (decision 17),
  today Path 1's classification: `=` or NULL-equal keys, and residuals. Two-valued: TRUE when some pair passes the
  keys and the residuals, and a pair whose residual is NULL is no match. [NLK17] §5.7 (p. 45) turns a FALSE marker
  into NULL for a NULL condition; that rule belongs only to a quantified comparison's own predicate, not to an EXISTS
  residual, nor to the WHERE of a correlated IN's subquery, which only filters its set.
- The two kinds mirror the split between the anti and the null-aware anti join: one needs the build's flags and no
  residual, the other residuals and no flags.

**13. A mark in the plan (SQ7).**

- A new column of `JoinKindTraits`: each left row comes out exactly once, followed by one BOOLEAN column, its mark.
  Every reader of the traits is audited: `PaddedJoinRows` gives a mark the left input's rows; `EmptyBuildEmptiesJoin`
  and `ProbeDropsUnmatchedRows` are false, so that no dynamic filter drops a row; `OutputsMatchesOnly` is false, which
  keeps aggregate placement away.
- `JoinNode` gains the mark's column, as `DependentJoinNode::mark` already anticipates (ADR 0023's "one output-id
  contract for two shapes"). `OutputIds`, `ResolvePositions`, `PositionMismatch`, `CloneWithFreshIds`, the
  fingerprint of shared sub-plans, the pruner, EXPLAIN, the physical planner's `ShapeOf` and the operator's output
  path follow.
- A mark builds on its right input only: a left build would have to emit every build row with its mark after the
  probe.
- Marks are keyed through phase 8: ADR 0040's decision 3 stands until SQ11.

**14. Where marks come from (SQ7, SQ8).**

- The binder chooses by position: a top-level conjunct stays a semi, anti or null-aware anti join, as today, and only
  a subquery under OR, NOT or CASE becomes a mark. That is the conversion [NLK17] describes (§3.3, p. 36; §4.4,
  p. 39) and DuckDB's optimizer makes after binding, so no optimizer rule converts marks here.
- OR factoring runs before a subquery under OR is classified. Today `CheckConjuncts` refuses such a subquery before
  `FactorSharedConjuncts` runs; lifting that refusal alone would send an EXISTS, IN or scalar that every branch shares
  down the mark path. Q41 needs the same factoring for an equality inside a correlated subquery's WHERE, before the
  unnesting classifies its conjuncts; SQ6 verifies it.
- SQ7 also answers NOT over NOT IN and NOT NOT over IN, a constant probe (`2 IN (q)`), and `= ANY`, `= SOME` and
  `<> ALL` as the parser's aliases of IN and NOT IN. The other quantified comparisons stay exit 4 (decision 3).

**15. Correlated IN, NOT IN and EXISTS (SQ8).**

- `x IN (q)` with q correlated, as a top-level conjunct, is a semi join keyed on the correlation and on the IN pair, as
  DuckDB plans it. `x NOT IN (q)` is an anti join keyed on the correlation, with the residual
  `x = s OR x IS NULL OR s IS NULL` for q's item s. As a conjunct neither needs per-binding flags: a NULL and an empty
  set decide only the outer row at hand. That corrects ADR 0023, which tied correlated IN to such flags; they are
  needed under OR, NOT or CASE, which is SQ12.
- An IN item that reads the outer block (a name the subquery's tables lack, resolved outward) makes the IN pair a
  residual, not a key. Without another key the shape is keyless, and its refusal names the outer column.
- Correlated EXISTS is answered in any condition, as today's semi or anti join where it is a top-level conjunct and
  through `kMark` under OR, NOT or CASE, and over any block through the rule table (decision 17): GROUP BY ([N24]
  Lemma 4.14); DISTINCT and `LIMIT n` with n ≥ 1 dropped; `LIMIT 0` false; `OFFSET n` as COUNT(*) > n per binding;
  an ungrouped aggregate without HAVING always true; a HAVING through the static-aggregate rule. Outer references in
  an EXISTS's select list or ORDER BY are admitted, since both are dropped, except inside an aggregate whose
  arguments are only outer columns, which is the enclosing block's (decision 23).

### The unnesting framework

**16. One rule table in place of Path 1 and Path 2 (SQ0).**

- The binder already does [NK15] §3.1's pull-up, so the unnesting work lies in the nodes between a dependent join
  and its block's correlated filter, and in nested blocks. SQ0 replaces `BlockOf`, `ScalarBlockOf` and `UnnestNode`'s
  choice of a path with [N25]'s top-down algorithm: per dependent join, its outer references; per subtree, the
  equivalence classes and the representatives of the outer columns ([N25] Fig. 4, p. 33); and a rule per node type
  (decision 17).
- The rules are an exhaustive visitor over the plan's node types, so that a new node does not compile without its
  rule. They read joins through `JoinKindTraits`, not by name: semi, anti, null-aware anti and one-row joins already
  occur inside correlated blocks, and marks and the single join are join kinds, not nodes. The kinds of the dependent
  join are semi, anti, one-row (unnested INNER or LEFT), the mark from SQ8, and the single join and the correlated
  mark in phase 10.
- It keeps the equivalence classes, `AsKey`, `MapColumns`, the semi-join reduction, the choice of build side, and the
  check after each rewrite that the new join's right input has no free variables ([N24] Lemma 3.1).
- It changes no answer and no plan of a shape answered today: the EXPLAIN goldens and `optimizer_test` stay as they
  are, names included, and the EXPLAIN of every correlated `.slt` record is compared before and after. Every
  correlated query of both ratchets is planned again: the queries derived from TPC-H (Q2, Q4, Q17, Q20, Q21 and
  Q22) and the correlated ones among the passing queries derived from TPC-DS.
- It lands the consumer proof (decision 20), which every shape answered today passes.
- Why: each of today's paths matches an exact stack of nodes with an exactness argument of its own, and the wrong
  answer ADR 0023's update of 2026-11-08 fixed, a constant item over an ORDER BY aggregate, was such an argument
  failing. Every later item adds a rule, not a path.

**17. The rules per node.** The last column names the item from which a rule answers a shape; before it, the shape
exits 4.

| Node | Rule | From |
| --- | --- | --- |
| Filter: the correlated filter | Each `=` of one type feeds the classes, and IS NOT DISTINCT FROM gives NULL-equal links; each conjunct becomes a key or a residual over candidate pairs, as Path 1 classifies today | SQ0 |
| Filter: a HAVING above a static aggregate | Evaluated above the join on the empty values: `CASE WHEN having THEN item END` for a value, never a filter | SQ6 |
| Compute, Project | Outer columns rewritten to their representatives, whose columns are appended; a map that reads outer columns moves above the join ([N24] Lemma 4.9) | SQ0; the map SQ10 |
| Sort | Dropped where no Limit is above it | SQ0 |
| Limit | Under EXISTS: `LIMIT n` with n ≥ 1 dropped, `LIMIT 0` false, `OFFSET n` as COUNT(*) > n per binding; elsewhere per binding | SQ8; SQ14 |
| Aggregate (static) | A GroupAggregate keyed by the representatives, joined back LEFT with the empty values, INNER only under the consumer proof ([N25] p. 39) | SQ0 (INNER); SQ6 |
| GroupAggregate | The representatives join its keys ([N24] Lemma 4.14) | SQ8 (EXISTS); SQ4b |
| GroupingSets | As a GroupAggregate when no set is `()`; static and multi-row when `()` is among other sets ([N25] p. 39) | SQ8; SQ16 |
| Join | By its traits, below | SQ0; both inputs SQ10, with D for an input that reads no outer column SQ9 |
| UnionAll, SetOperation | By arm: correlated arms | SQ15 |
| DependentJoin | The outer one first, as today; its left input is unnested before D reads it | SQ0; SQ9 |
| Scan, RowCount | Terminal: every outer column needs a representative (substitution), or D | SQ0; SQ9 |

The join rule ([N25] p. 40; [N24] Lemmas 4.11, 4.13 and 4.15): where one input alone reads outer columns, the pass
recurses into it alone if the join outputs no unmatched row of the other input: into the left input when the join
outputs no unmatched right row (no `pads_left`), and into the right input only when the join outputs the right
input's columns and drops every unmatched left row, as an inner join does. Otherwise it unnests both inputs and adds
`IS NOT DISTINCT FROM` between their representatives to the join's condition, with a COALESCE of each pair above a
FULL join; only an input that reads no outer column needs D for that. So an inner join recurses into either input, or
into both with their representatives equated; a LEFT, semi, anti, one-row, mark or single join only into its left
input; and a FULL join always needs both inputs. Phase 8 refuses what needs both (exit 4). Phase 10 unnests both by
substitution, with no D, where both inputs read outer columns (SQ10), and with D for an input that reads none (SQ9).

**18. Substitution, and D as the second terminal step.**

- Phase 8's only terminal step is substitution ([N24] Lemma 4.2): at a leaf every outer column needs a representative
  among the classes, or the query exits 4. It is today's Path 2, and the plan DuckDB reaches where its optimizer
  removes D.
- SQ9 adds the domain D ([NK15] §3.2; [N24] Theorem 4.1) for the correlations decision 2 lists. Its design is fixed
  now, so that SQ0's table has its place:
  - D is a GroupAggregate without calls, keyed by the free variables, over a clone of the left input. The clone keeps
    the base relations that provide the free variables and every operator that can add a value to one: a join that
    pads the input it comes from, a grouping set that rolls it up, a Compute that defines it. It may leave out only
    operators that restrict (filters, inner and semi joins), so D stays a duplicate-free superset of the bindings,
    which [N24] Theorem 4.1 requires. Leaving those out keeps the clone small, where a clone of the whole left input
    grows as 2^k for k stacked dependent joins (ADR 0023 measured 24,573 plan lines at k = 12).
  - The left input is unnested first ([N25] p. 34), so the clone holds no dependent join.
  - D meets the leaves through keyed joins first.
  - The join back is NULL-equal (`JoinKey::null_equal`, ADR 0037's OJ5a), since D may hold NULL.
- Its cost is known. The outer side's relations are computed twice, since one build feeds one probe pipeline
  (ADR 0022). A keyless D × leaf has |D| × |leaf| pairs: ADR 0040 measured 9,000,000 pairs at 749 ms on one Debug
  thread. Relational division's product of D with the middle block is inherent to it. Whether a keyless D is refused
  above some size is an open decision.

### Static aggregates and the consumer proof

**19. Empty values.** A static aggregate (no GROUP BY, or a grouping set `()`) returns one row over no input, whose
calls hold their empty values:

| Kind | Empty value |
| --- | --- |
| COUNT(*), COUNT, COUNT(DISTINCT), `kSumCounts` | 0 |
| SUM, AVG, MIN, MAX, the variance family, the single-value aggregate | NULL |
| GROUPING() (SQ16) | every bit set: each argument rolled up |

The call kinds are an exhaustive switch over `AggKind`, which replaces `StrictAggregates`. GROUPING() is no `AggKind`
but a `GroupingCall` of `GroupingSetsNode`, so its empty value comes from the grouping-sets rule. A subquery's empty
value is what its block computes over the empty values: its select item, or NULL where a HAVING rejects them. A
strict combination of SUM, AVG, MIN or MAX is NULL there, COUNT(*) is 0, `COUNT(*) + 1` is 1, and a constant item
keeps its constant.

**20. The consumer proof (SQ0).** The optimizer proves from the plan above a dependent join whether its join back may
be INNER: only where every reader of the subquery's value rejects the subquery's empty value. It follows the value's
column up through Computes to the first Filter that reads it, which must be false or NULL for every row when the value
is the empty value. Anything else that reads the value, or a node on the way that is not a Compute, leaves the join
LEFT (decision 21). [NLK17] §4.4 (pp. 39-40) turns an outer join into an inner one under a predicate that rejects
NULL; the proof does the same with the empty value in place of NULL.

- `o.v > 2 * (q)` over SUM is NULL there: rejected, INNER.
- `(q) > 0` over COUNT(*) is `0 > 0`, false: rejected, INNER.
- `(q) > o.x` over COUNT(*), `COALESCE((q), 0) < o.v`, `(q) IS NULL`, an OR, a select item: not proven, LEFT.

Why in the optimizer: it verifies, where a flag the binder set on `DependentJoinNode` would be trusted, and it changes
no public header. It is PF3's analysis restricted to the joins the unnesting makes, so user-written LEFT JOINs keep
their plans. Every shape answered today passes it, so SQ0 lands it with no answer changed; from SQ5 and SQ6 on it
decides (from SQ6 if they split, since SQ6 lands first).

**21. The LEFT join back (SQ6), and correlated values (SQ5).**

- **SQ6:** where the proof fails, a static aggregate's join back is the outer side LEFT JOIN the GroupAggregate keyed
  by the representatives ([N25] p. 39, under substitution):
  - a COUNT column is read through `COALESCE(c, 0)`, a `CaseExpr` with a null `when`: a real group's COUNT is never
    NULL;
  - the select item moves above the join, as Computes that define the same ids, so a constant keeps its value and
    `COALESCE(SUM(x), 0)` its default;
  - a HAVING inside the subquery becomes `CASE WHEN having THEN item END` above the join;
  - an uncorrelated one-row join between the block's aggregate and its item, from the slot after the aggregation,
    moves up with the item, so that a binding without rows gets `0 + value`, not NULL;
  - a NULL-equal correlation reads the NULL group, as Path 2's does;
  - a subquery with GROUP BY gets no COALESCE: no row gives NULL, not 0, which is the single join, refused until SQ4b;
  - the semi-join reduction fires for a LEFT join back even over one key and a bare scan, the case it leaves to D5
    today: D5 never reaches a LEFT join, whose probe keeps its unmatched rows, so every inner key would otherwise be
    grouped and built.
- **SQ5:** correlated aggregates of any kind, with any select item, are values in the per-row slots (decision 9): in
  any expression of a condition, in select items, ORDER BY, GROUP BY and aggregate arguments, with the join back
  chosen by the proof. Above an aggregation they wait for SQ13.
- **An outer-only conjunct** in a correlated scalar aggregate's WHERE, such as `p(o)`, is lifted out of the subquery
  only under the proof, and otherwise becomes `CASE WHEN p(o) THEN value ELSE <empty value> END` above the join: lifted
  out, it would drop outer rows that the subquery gives its empty value. It is answered from SQ10.

### The single join

**22. `kSingle` (SQ4b).**

- **Its meaning, per probe row, after its keys and residuals:** no match gives NULL, and a NULL key under `=`
  matches nothing; one match gives its values; two or more raise decision 4's row error, eagerly, and only for keys
  that probe rows bind.
- **Its orientation:** it builds the subquery, as every subquery join does, and counts each probe row's passing
  partners: [NLK17]'s right variant (p. 44), as DuckDB's single join runs, with no marks on build rows and no atomics.
- **Its traits:** `outputs_right` (the right input's columns follow the left's), `pads_right` (an unmatched left row
  comes out once with NULL right columns), `residuals`, and a new column: a second match is an error. The readers are
  audited as for the marks. Aggregate placement's `Exchangeable` must refuse it, since a partial aggregate on its
  build would collapse the very duplicates that must fail; `EmitsBuildRows` is true for a left build through the
  padding, so `PositionMismatch` must also refuse a left build for it (today it refuses only one-row kinds and kinds
  with `pads_left`); `ProbeDropsUnmatchedRows` is false, so D5 does not apply.
- **The executor:** the LEFT join's probe with a count per row, and no check where the build's keys are unique
  (`JoinTable::unique()`) and there is no residual.
- **No shortcut:** never an aggregate over the inner groups, nor D5's filter, whose superset of keys and false
  positives would raise errors DuckDB does not. A subquery grouped by exactly its correlated keys has at most one row
  per binding and becomes a LEFT join, as in DuckDB ([NLK17] §4.4).
- **Its plan dependence:** DuckDB's error depends on which outer rows reach the join: filter pushdown, the
  deliminator, statistics propagation, run-time join filters and the order of conjuncts (each probed by disabling
  it). antb1 checks the outer rows that remain after what stands below the join: the block's pushed conjuncts, and
  the subquery joins written before it. SQ4b registers that divergence, and its records and the generator use only
  shapes that fail in every DuckDB plan or in none.

### Errors, exit codes and divergences

**23. Exit codes.** Refusals exit 4, row errors 1, planner bugs 70 ([ADR 0005](0005-error-boundary.md)). Each class
fix lands with the item that touches its code.

An outer aggregate below is an aggregate whose arguments are only enclosing-block columns. Anywhere in a subquery it
is the enclosing block's aggregate, so DuckDB's outcome depends on the enclosing clause that holds the subquery: where
the subquery stands in the enclosing WHERE, ON or GROUP BY, DuckDB raises a bind error; where it stands in the
enclosing HAVING or select list, DuckDB answers it, which antb1 does from SQ13 on and refuses (exit 4) until then.

| Shape | Today | After | Item |
| --- | --- | --- | --- |
| A scalar subquery's row error | 4 (refused) | 1, with DuckDB's first sentence | SQ4a |
| The one-row join's own check, a planner bug | 1 (`Invalid`) | 70 | SQ4a |
| A scalar subquery of two columns that is not an aggregate | 4 | 1, DuckDB's bind error: the column count is checked first | SQ4a |
| EXISTS whose select list has a column that is not grouped | 4 | 1, DuckDB's bind error: uncorrelated, the binder rejects the block once SQ4a binds it; correlated, once SQ8 does | SQ4a; SQ8 |
| An outer aggregate in a subquery's WHERE, the subquery in the enclosing HAVING or select list | 1, a parse error | 4 until SQ13 answers it: the check moves from the parser to the binder, which knows whose columns the aggregate reads and which clause holds the subquery. In the enclosing WHERE, ON or GROUP BY it stays 1, and so does an aggregate of inner columns in WHERE | SQ1 |
| An outer aggregate in a subquery's select list, the subquery in the enclosing WHERE, ON or GROUP BY | 4 | 1, DuckDB's bind error | SQ10, with SQL's rule |
| An outer aggregate in a subquery's HAVING, the subquery in the enclosing WHERE, ON or GROUP BY | 4 | 1, DuckDB's bind error | SQ13 |
| IN of VARCHAR against INTEGER or DATE | 4 | 1, DuckDB's bind error | SQ7 |
| A single join's row error | 4 (refused) | 1 | SQ4b |
| A dependent join the pass leaves | 70 | 70 | unchanged |

**24. Error visibility: one divergence family, and no reordering.**

- antb1 reports the errors of the builds it prepares. Within a pipeline it prepares them outermost first, a build whose
  input probes builds of its own after those, and it stops at the first build that empties its join (`PrepareBuilds`
  in `src/exec/hash_join.cc`; ADR 0022's J1b update): the builds below never run.
- DuckDB prepares every build, whatever turns out empty: over `read_parquet` views, as the oracle registers tables,
  an empty file, a filter outside a column's range and join keys outside the other side's range delete nothing. Its
  optimizer deletes a failing subquery only where its filter combiner proves a condition never true in the block the
  subquery is written in (a contradiction pair such as `a = 1 AND a = 2`, an empty BETWEEN, a constant false; also
  inside an IN subquery there), where an IN subquery there has `LIMIT 0`, or where nothing reads the derived-table or
  CTE column that holds it.
- So whether a subquery's error fires (an overflow today, and from SQ4a the row error) can differ in both directions:
  - antb1 answers, and DuckDB fails, where a build prepared before the failing one empties its join: in one block,
    when the IN or EXISTS that turns out empty is written after the failing subquery; across blocks, for a failing
    subquery inside a derived table, a CTE body, a UNION ALL branch or an IN subquery's WHERE next to a join that
    turns out empty; and across the aggregation, for a WHERE subquery under a HAVING IN that turns out empty;
  - antb1 fails, and DuckDB answers, where DuckDB's optimizer deleted the subquery.

  It is live today: an overflowing scalar aggregate next to an IN or EXISTS that turns out empty fails in antb1 when
  the IN or EXISTS is written first and answers when it is written last, while DuckDB fails in either order unless
  its optimizer deletes the subquery as above.
- The family is registered once, with this ADR, as divergence D35 in [docs/sql-subset.md](../sql-subset.md), rather
  than as extensions of D32 (columns nothing reads) and D34 (contradictions) for subqueries.
- No join is reordered for it. Stacking a block's one-row joins outermost would change answered plans and still not
  reach the cases across blocks and across the aggregation; preparing them before an emptied join would be a special
  case in the executor; and DuckDB's side depends on its filter combiner and on which columns are read either way.
- SQ4a adds records that pin both engines' outcomes, as for the other error divergences, and from then on the
  generator writes neither contradiction pairs next to a subquery nor subquery joins that can empty around one that
  fails (decision 28).

**25. The other divergences.**

- D24's DuckDB column is corrected with this ADR: where the subquery has a condition besides the correlation, or the
  outer side has a filter of its own, DuckDB keeps D and computes the aggregate's argument only for the keys the outer
  side binds, where antb1 computes it for every inner key.
- The items register the divergences they create: SQ1 adds to D16 that subquery values are computed before an AND, OR
  or CASE decides, as in DuckDB; SQ2 extends D8 to the names of subquery items; SQ6 extends D24 to the LEFT join back,
  where DuckDB keeps D for every COUNT shape; SQ8 extends D23 to the residuals of marks; SQ4b registers which outer
  rows its check sees.
- DuckDB's own wrong answers are not antb1's divergences (decision 29).

### Memory, budget and determinism

**26. What the new joins hold, and the guards.**

- The single-value state is constant in size. A mark builds the subquery's keys and its residuals' columns, as a semi
  join does. A LEFT join back builds every group of the inner side that the reduction leaves. A single join builds the
  subquery's rows, as a LEFT join does.
- **PF4**, one aggregation for one-row subqueries over the same rows, is measured in SQ2 on Q9 and pulled in only if
  ADR 0037's budget gate fires. The memo of decision 10 merges only subqueries written alike, never different
  aggregates over the same rows, which are PF4's target.
- **PF16, a new guard: one-row values as run-time constants of the probe pipeline.** Today a one-row join switches
  off late materialization ([ADR 0016](0016-late-materialization.md)) and top-N part pruning in its probe pipeline: in
  one synthetic probe the scan read 400,000 rows and 26.54 MB under a one-row join, against 297,600 rows and 10.33 MB
  without it. The value is known before the probe starts, since builds come first, so a constant keeps both, and a
  comparison of a scan column with it can become a dynamic scan filter that prunes parts by their footers, as ADR
  0029's key filters do. SQ1 and SQ2 put values into every condition and select list, so they measure it.
- **PF7 stays a guard**, now for one-row, single and mark joins: each on the branch of the relation it reads. ADR
  0029's D0 rule generalizes to marks, since a mark acts on each left row alone, but never into a padded input. Q10
  and Q35 stack several marks above joins of many relations, and a mark keeps every probe row, so no dynamic filter
  applies to it.
- Each PR that completes a query gives its SF 1 seconds and peak memory (ADR 0037's decision 5), and
  `pixi run asan-tpc-sf1` runs before merging SQ4a (Q14) and SQ2 (Q9), the two of the nine likeliest to press on the
  budget (its decision 6).

**27. Determinism.**

- The single-value aggregate returns a value only for exactly one row, so its result is the same for any part order
  and thread count, and its error is DuckDB's sentence whichever rows it counts first.
- A mark depends only on the build's contents: its keys, whether it holds a NULL, and whether it is empty.
- The single join checks each probe row, so whether it fails depends on the rows that reach it, never on part order.
- Errors follow ADR 0022's order: builds first, outermost first and in post-order, and within a build the first
  failing part in part order; then the probe.
- EXPLAIN and every answer stay the same for any number of threads: the unnesting reads the plan and nothing else.

### Tests

**28. Tests per item.**

- **`.slt` records first.** Each item's records are written before its code, under `pending <id>` guards (ADR
  0037's decision 7), and run at 1 and 4 threads. The DuckDB-only guards of `tests/slt/cases/subqueries/` that wait
  for a subquery shape go with the items that answer them:

  | Guard, by its heading | Item |
  | --- | --- |
  | `scalar.slt`: correlated COUNT, and a constant over ORDER BY COUNT(*) | SQ6 |
  | `scalar.slt`: CASE over correlated aggregates | SQ6 |
  | `scalar.slt`: a correlation of two types | SQ10 |
  | `scalar.slt`: "until single joins get a PR", whose records are uncorrelated | SQ4a |
  | `exists.slt`: an aggregate inside EXISTS | SQ8 |
  | `exists.slt`: references further out than the enclosing block | SQ9 |
  | `exists.slt`: the uncorrelated EXISTS | SQ4a |
  | `exists.slt`: EXISTS under OR | SQ8 |
  | `in.slt`: correlated IN and NOT IN | SQ8 |
  | `in.slt`: DOUBLE join keys | DuckDB coverage (decision 3) |
  | `derived_tables.slt`: lateral references | DuckDB coverage (decision 3) |

- **The generator.** One Feature per item, drawn from a salted stream of its own, so that no other query of a seed
  changes. Today subqueries come only as top-level conjuncts; the items add them under conditions and as values.
- **Never generated:** a scalar subquery of two rows or more, outside a record made for the error; arithmetic that
  can overflow; NaN (D10); the shapes of decision 29; LIMIT without ORDER BY above a single join; contradiction pairs
  and sibling subquery joins that can empty around an error (decision 24); a correlated scalar in an ON that also
  reads a comma sibling; GROUPING() in correlated grouping sets.
- **Metamorphic relations:**
  - a one-row value equals a cross join with it as a one-row derived table;
  - a subquery of at most one row equals its MIN() form;
  - `x BETWEEN (q1) AND (q2)` equals `x >= (q1) AND x <= (q2)`;
  - `= ANY` equals IN, and `<> ALL` equals NOT IN;
  - `NOT (x IN (q))` equals `x NOT IN (q)`, and `NOT NOT (x IN (q))` equals `x IN (q)`;
  - the mark of `x IN (q)` equals a CASE over a LEFT JOIN to `SELECT DISTINCT` of q and over q's one-row counts, which
    checks `kNullAwareMark` against existing operators;
  - the rows of `p OR EXISTS (q)` are those of p and those of `EXISTS (q)` together, by row id, and the same for IN;
  - a correlated COUNT equals a LEFT JOIN, a GROUP BY and a COALESCE;
  - a correlated IN conjunct equals EXISTS with the IN pair as a second key, and a correlated NOT IN equals NOT EXISTS
    with decision 15's residual;
  - every answer is the same for any number of threads and any batch size;
  - **the self-check:** for each binding of a small outer table, the correlated value equals the uncorrelated
    subquery with the binding written as literals. It needs only SQ4a and no DuckDB, and it holds where DuckDB's
    decorrelation is wrong (decision 29).
- **Mutation checks**, each of which must fail a named record: drop the mark's rule for a NULL in the set; give an
  empty set's NULL probe NULL instead of FALSE; apply [NLK17] §5.7's rule to an EXISTS residual or to a correlated
  IN's WHERE; drop the COALESCE on COUNT; force INNER where the proof fails; pad a grouped COUNT with 0; check the
  single row per inner group, or by the build's uniqueness alone; skip NULL rows in the single-value state; prepare
  the one-row build lazily. The harness's answer mutations cover every new record.
- **Plans:** hand-built dependent joins for each rule, in the style of `optimizer_test`; EXPLAIN goldens for each new
  kind; the operator's property tests over every kind. The differential tests are `diff.random` and `diff.tpcds` at
  SF 0.01.

**29. DuckDB's wrong answers, recorded once.**

- A correlated static aggregate returns one row over no input, its calls at their empty values (decision 19), and
  DuckDB 1.5.6's decorrelation does not compute that row. It joins its domain D to the aggregation INNER where every
  aggregate and every select item propagates NULL (its check counts a constant as propagating, and does not read a
  HAVING), so such a binding gets NULL; otherwise it joins D LEFT and computes the items over a NULL-padded row, where
  only COUNT gets 0 and GROUPING() is NULL. So it answers three known shapes wrongly: a constant item over
  `ORDER BY MAX(x)`, a constant item under `HAVING SUM(x) IS NULL OR ...`, and GROUPING() in correlated grouping
  sets. That is one family, recorded once in [docs/testing.md](../testing.md); each item whose Feature could write a
  member adds it to the generator's never-generate list.
- Other wrong answers of DuckDB 1.5.6, kept out of `slt-complete`: a correlated scalar subquery in an ON that also
  reads a comma sibling; `x * -1 > -2147483648`; and `d / -0.0` next to `d / 0.0` in one select list, which its
  elimination of common subexpressions merges.
- The self-check of decision 28 needs no DuckDB, so it covers the correlated shapes among them in antb1.

### Open decisions

Questions the maintainer has not ruled on. Each is decided by a dated update before the item it concerns:

- **Phase 10's build order.** The ids of decision 2 name items; the order follows their needs and is set when phase 10
  is planned, with ADR 0037's Plan.
- **A size guard for a keyless D (SQ9).** The design discussion proposed refusing a keyless D over large inputs, which
  would leave relational division at scale refused; ADR 0040 rejected output-size guards for cross products.
- **When this ADR is accepted**, after SQ8 or after phase 10, and whether ADR 0037's closing items wait for phase 10.
- **Left builds** as guards for the budget gate: a mark built on its left input, and a LEFT join back built on the
  outer side, for which `ProbeDropsUnmatchedRows` already holds.
- **General features that no item plans,** which DuckDB answers and antb1 refuses (exit 4) with or without a
  subquery: LIMIT and OFFSET given by an expression, which a subquery there needs; constant aggregate arguments,
  which a subquery as the whole of an aggregate's argument needs; and an aggregate's FILTER clause, which SQ17 needs.

### Known issues without an item

Defects the design discussion found, to which no item is assigned:

- **Misleading messages and stale comments:** a DOUBLE correlation is refused as having no key; a correlation inside a
  derived table nested in a subquery is called "a lateral qualified column"; EXPLAIN prints Path 2's substituted inner
  columns under their outer names (`MapColumns` keeps `Expr::name`), which SQ0 keeps since it changes no golden; and
  comments in `src/plan/optimizer.cc` still describe U1's pass.
- **The semi-join reduction's clone** recomputes the outer side's one-row and mark joins, which never restrict its
  keys.
- **D4's text** does not name a DATE column against a VARCHAR operand, which a scalar subquery reaches.

## Update (2026-11-10, SQ4a): uncorrelated values, built

- **Built as decided** (decisions 4 to 7): Q14, Q54 and Q58 pass, and the ratchet is at 78 (ADR 0037's SQ4a update
  gives their SF 1 figures). Correlated subqueries are answered and refused as before.
- **The single value (decision 4)** is `AggKind::kSingleValue`, `SINGLE_VALUE` in EXPLAIN. Its state counts the
  selected rows, NULL ones included, and keeps a one-row copy of the first selected value, so no batch stays pinned;
  merging adds the counts. It raises the row error in Finalize only, after every part is merged, so a part that fails
  reports its own error first, in part order, on any number of threads. The factory takes only a result type equal
  to the argument's. `ExactlyOneRow` sees through Projects and Computes to an ungrouped aggregate, a CTE's root
  included, so Q14's subqueries over a CTE's aggregates need no wrapper.
- **The EXISTS block (decision 5).** Probed against DuckDB, its select list goes even under a LIMIT, while its ORDER BY
  keys are computed (a top-N under the LIMIT 1). So the Projects among the Limits and Sorts at the top of the block's
  plan go and the Sorts stay; a DISTINCT's list stays, being its rows, and so do the block's aggregates. Three cases
  still differ, registered under divergence D32 with records of both engines: the aggregates of an EXISTS block that
  nothing reads, grouped or not, which antb1 computes and DuckDB removes (so over an ungrouped aggregate an EXISTS is
  always true only where the aggregate cannot fail); the ORDER BY of a block that aggregates without GROUP BY, whose
  one row antb1 never sorts, as it never sorts a query's (ADR 0011), while DuckDB computes its keys; and a correlated
  EXISTS's ORDER BY, which ADR 0023's Path 1 drops and DuckDB computes. A star in the block stays unexpanded, but its
  columns go through the grouping checks, so a grouped or aggregate block's star is DuckDB's bind error (decision 23's
  row), and a GROUP BY position that falls in it exits 4, as an ORDER BY one does; under a DISTINCT, whose rows its
  columns are, it expands, and so it does in every arm of a set operation.
- **The BETWEEN split (decision 6)** takes one subquery bound or two. DuckDB's typing of the three values runs with
  each subquery bound standing for its block's column, and both comparisons compare in that type, which decides, as
  in DuckDB, whether a constant moves off the operand: with a DOUBLE bound, `i + 1 <= 100` keeps its sum (and an
  overflow in it) as DuckDB keeps the cast to DOUBLE above it.
- **Set operations (decision 7).** The arms of a scalar subquery's or an EXISTS's compound bind as the subquery's
  blocks, so an arm that reads an enclosing block makes the compound correlated, refused at the subquery as "a
  correlated subquery over a UNION ALL is not supported" (the compound's root operator named); SQ15 answers it.
- **The order of errors (decision 23).** The refusals that depend on correlation now come when the subquery's block
  is bound, not from the static check of its shape. A correlated EXISTS keeps its refusals and their messages through
  a gate that runs once the block's FROM list is bound and before any other clause of it binds, so those clauses'
  bind errors still never show; it asks every clause, grouping-set keys included, whether it reads an enclosing
  column, a name that a written select alias has being that alias. A compound's arms all bind before a correlated arm
  is refused. What the enclosing block binds before the subquery now comes first: its FROM list and inner ONs, the
  WHERE conjuncts written before it, its select list (which binds before its WHERE), and its grouping for a HAVING
  subquery; a conjunct written after it is never reached. A correlated scalar subquery's own bind errors come first
  too. Each is exit 1 where antb1 exited 4; DuckDB, which answers these subqueries, binds WHERE before the select list,
  so the first error each engine names can differ. Decision 23's SQ4a rows are built: the row error exits 1, the
  one-row join's own check 70, a scalar subquery of two columns 1 in any shape, and an uncorrelated EXISTS block's
  ungrouped select column 1, its star's columns included.
- **Error visibility (decision 24).** `tests/slt/cases/subqueries/values.slt` pins D35 for the row error in both
  engines: an IN written after the failing subquery, a contradiction pair, a HAVING conjunct antb1 folds to false
  (D14), whose filter ends the plan above the subquery's join, a `LIMIT 0` derived table in the block's FROM (which
  DuckDB's optimizer also deletes), and a WHERE subquery under a HAVING IN; D35's text gains the two new cases. An
  uncorrelated EXISTS that is false empties no join, its count being a row, so it hides no error.
- **Tests (decision 28).** The guards the table lists for SQ4a are lifted, and `setops/errors.slt`'s X6 records wait
  on SQ7 (IN) and SQ15 (a correlated arm). The generator's Feature `uncorrelated_value` draws from a stream of its
  own: value blocks of at most one row (the first value in either order or of its groups, an OFFSET, LIMIT 0, an
  aggregate under HAVING COUNT(*), INTERSECT or EXCEPT of two aggregates; never a UNION) under a comparison or as a
  BETWEEN's bounds, and EXISTS over blocks of any row count, compounds included. No block computes anything that can
  fail, so no generated query meets the row error or D35. The metamorphic relations of decision 28 hold, the
  self-check among them, over the six cities of the star schema for a correlated EXISTS, NOT EXISTS and aggregate.

## Consequences

- **The ratchet** climbs from 75 to 84 through phase 8, ADR 0037's figure for its end, and reaches 99 with phase 9.
  Phase 10 completes none of the 99: its measure is DuckDB's answers to general SQL. The window functions stay the
  last phase that completes queries, but no longer the last phase.
- **ADR 0037** changes with this ADR, in its dated update of 2026-11-10: phase 8's rows in the new order, SQ3
  absorbed, SQ0 and SQ4a new, phase 10 and its ids, PF16, and PF7 widened to marks.
- **ADR 0023:** Path 1 and Path 2 become rules of SQ0's table, its list of exit-4 shapes shrinks item by item,
  correlated IN needs per-binding flags only under OR, NOT or CASE (SQ12), and the general algorithm it deferred is
  SQ9. **ADR 0022:** the one-row join stays exact and its check exits 70 (SQ4a), and subqueries stand as values in
  every clause decision 8 lists after SQ2. **ADR 0036:** X6 is done for uncorrelated arms (SQ4a, SQ7). **ADR 0040:**
  decision 3 holds through phase 8, and SQ11 amends it.
- **Public headers change:** `src/plan/include/antb1/plan/logical_plan.h` (a new `AggKind`, `JoinNode`'s mark),
  `src/plan/include/antb1/plan/join_kind.h` (two mark kinds and a trait column; `kSingle` in SQ4b),
  `src/exec/include/antb1/exec/aggregate_state.h` (the single-value aggregate, which counts NULL values as rows against
  the header's contract that they are skipped; decision 4), and any public exec join header that the mark and
  single-join operators change, such as `src/exec/include/antb1/exec/join_table.h`. No module edge is added: the work
  lives in plan and exec, and the quantifier aliases in sql's parser.
- **Every correlated query of both ratchets is planned again by SQ0**, with an EXPLAIN comparison as its guard. The
  risk sits in one PR that changes no answer.
- **Six wrong exit-code classes are fixed, and the one-row join's check moves to exit 70** (decision 23), each with
  its item.
- **Errors next to joins that turn out empty differ from DuckDB in both directions** permanently, under one registered
  divergence, rather than being fixed one plan at a time.
- **The generator** writes subqueries under conditions and as values for the first time, and the self-check gives the
  correlated items an oracle that does not depend on DuckDB.
- **[docs/sql-subset.md](../sql-subset.md)** changes with each item, and its table of the queries derived from TPC-DS
  names the new items for the nine with ADR 0037's update.

## Alternatives considered

- **Design the single join and D, and build them only on a trigger** (the design discussion's recommendation).
  Rejected by the maintainer: full decorrelation is built, as phase 10, and phase 8 is designed as its subset.
- **A keyless single join for uncorrelated scalars.** Rejected: a check per probe row raises nothing over an empty
  outer side, where DuckDB raises the row error, and the one-row join, which one-row FROM items share, must not pad. A
  check at the build is the single-value aggregate under another name.
- **DuckDB's own spelling**, FIRST and COUNT(*) under a CASE with an error function. Rejected: two new pieces where
  the single-value aggregate is one.
- **SQ3's one-row proof through CTE and derived-table references.** Subsumed: it answers Q14 only, and Q58's
  subqueries are not aggregates. `ExactlyOneRow` survives as the shortcut that skips the wrapper.
- **`LIMIT 2` below the wrapper.** Rejected: DuckDB reads every row, and a limit would change which error a failing
  subquery reports.
- **DuckDB's setting that returns an arbitrary row instead of the error.** Not offered: antb1 follows the default.
- **Keeping the row error visible by order** (decision 24): stacking a block's one-row joins outermost, or preparing
  one-row builds before an emptied join. Rejected: the first reorders answered plans and does not reach errors across
  blocks or across the aggregation, the second is a special case in the executor, and DuckDB's side depends on its
  filter combiner and on which columns are read either way.
- **Values as hidden FROM items**, so that an equality with a value becomes a join key. Rejected for now: inequalities
  would still cross the item last, uses after the aggregation would still need a slot, and the join order could put a
  join that empties above the item and hide its row error. It remains a way to build PF7 if the budget gate asks.
- **An ordinal from parsing as the memo key**, which deep copies carry. Rejected: exact, but it merges nothing, where
  the structure merges subqueries written alike.
- **A deparser in DuckDB's style for names, now.** Deferred to DuckDB coverage: the tests compare values and types,
  and Q9's items are aliased.
- **Special paths instead of SQ0:** a LEFT variant of Path 2, a mark variant of Path 1, and later a single one. Cheaper
  now. Rejected: each is an exact match on a stack of nodes with an exactness argument of its own, which is how the
  constant-over-ORDER-BY wrong answer arose, and the maintainer requires the full framework.
- **D in phase 8.** Rejected: none of the nine needs it, and it costs the outer side twice, clones that need the left
  input unnested first, and pairs that grow with |D|. It is built in phase 10 (SQ9).
- **Keyless joins and marks in phase 8.** Rejected: ADR 0040's decision 3 stands through phase 8; they are SQ11.
- **A mark for uncorrelated EXISTS.** Rejected: a mark keeps every probe row and takes no dynamic filter, for a value
  that is the same for every row.
- **A composition instead of `kNullAwareMark`:** a LEFT JOIN to `SELECT DISTINCT` of q, q's one-row counts and a CASE.
  Rejected: it reads q twice and groups it, and it cannot express EXISTS with residuals, which SQ8 needs anyway. It
  stays as a metamorphic relation.
- **Another source for the consumer proof:** a flag the binder sets on `DependentJoinNode`, or PF3's general rule
  now. Rejected: the flag changes a public header and the optimizer would trust it, and PF3 now would plan user-written
  LEFT JOINs again.
- **The single join built on the outer side**, as [NLK17]'s List. 5 does, marking build rows. Rejected: antb1 builds
  the subquery side, as for every subquery join, and counts per probe row, with no marks and no atomics.
- **The single row checked per inner group, or through D5.** Rejected: both see a superset of the bound keys and raise
  errors DuckDB does not.
- **[NLK17] §5.7's NULL rule for EXISTS residuals.** Rejected: the probes confine it to a quantified comparison's own
  predicate.

This workload is derived from the TPC-DS Benchmark and is not comparable to published TPC-DS Benchmark results, as
this implementation does not comply with all requirements of the TPC-DS Benchmark. This workload is derived from the
TPC-H Benchmark and is not comparable to published TPC-H Benchmark results, as this implementation does not comply
with all requirements of the TPC-H Benchmark.
