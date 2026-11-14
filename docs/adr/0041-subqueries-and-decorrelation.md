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
  columns under their outer names (`MapColumns` keeps `Expr::name`), which SQ0 keeps since it changes no golden.
  (Comments in `src/plan/optimizer.cc` that still described U1's pass went with SQ0.)
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
  column, and it reads a name that a written select alias also has as DuckDB reads it: the alias in WHERE, the select
  list, HAVING, ORDER BY and a bare GROUP BY key, outside the argument of an aggregate or of GROUPING(); the
  enclosing column in an ON, a GROUP BY expression and such an argument (DuckDB reads a GROUPING() argument as the
  alias where the block groups by it, a GROUPING() of an alias, which antb1 refuses in any case). Where DuckDB takes
  the alias in WHERE or the select list, the block's binder refuses the name, as it does in any block. A compound's
  arms all bind before a
  correlated arm
  is refused. What the enclosing block binds before the subquery now comes first: its FROM list and every ON, the
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
  uncorrelated EXISTS that is false empties no join, its count being a row, so it hides no error. Update
  (2026-11-10): HAVING now filters right above the aggregation, then joins its subquery conjuncts, before the select
  list is computed, as DuckDB does, so a HAVING subquery's join stands above a plain conjunct folded to false and its
  build runs: that record fails in both engines, and pins a WHERE subquery under such a conjunct instead (D35's text
  gives the other cases).
- **Tests (decision 28).** The guards the table lists for SQ4a are lifted, and `setops/errors.slt`'s X6 records wait
  on SQ7 (IN) and SQ15 (a correlated arm). The generator's Feature `uncorrelated_value` draws from a stream of its
  own: value blocks of at most one row (the first value in either order or of its groups, an OFFSET, LIMIT 0, an
  aggregate under HAVING COUNT(*), INTERSECT or EXCEPT of two aggregates; never a UNION) under a comparison or as a
  BETWEEN's bounds, and EXISTS over blocks of any row count, compounds included. No block computes anything that can
  fail, so no generated query meets the row error or D35. The metamorphic relations of decision 28 hold, the
  self-check among them, over the six cities of the star schema for a correlated EXISTS, NOT EXISTS and aggregate.

## Update (2026-11-10, SQ1a): values in WHERE and inner-ON conditions, built

At the maintainer's request SQ1 is two PRs: SQ1a, values in the conditions of WHERE and of an inner ON, with all the
machinery; and SQ1b, values in HAVING with decision 23's SQ1 row. A fix PR before them (#345) made HAVING filter
before the select list is computed, as DuckDB does, so that HAVING's slot can stand between the two. No query
completes, and the ratchet stays at 78.

- **Decision 8, as built in WHERE and an inner ON** (one inside a FULL JOIN's left input too): an uncorrelated
  scalar subquery is a value anywhere in a condition, and an uncorrelated EXISTS a condition there, under AND, OR, NOT
  and CASE. An IN list's values may be subqueries among literals, each reading no column, and the list is the OR of
  its equalities after DuckDB's one-type check of the list; a value may be LIKE's operand, not its pattern. A value
  as an IN subquery's probe, and an IN every branch of an OR shares, stay SQ7's (decision 14), as does any IN under
  OR, NOT or CASE. A conjunct of a correlated block that reads the enclosing block and holds a value is a residual
  of Path 1 or Path 2, whole, a BETWEEN included; an IN whose probe reads the enclosing block stays exit 4, and so
  does a correlated subquery in a conjunct that reads the enclosing block itself. A correlated comparison whose other
  side holds a value, or is one, is ADR 0023's dependent join with the value's join below it; two different correlated
  subqueries compared wait on SQ5. A value of the outer column's type that a correlation equates with it keys the
  block, as Path 1's semi key or Path 2's inner equivalent; one that needs a cast, or an expression over a value, has
  no inner equivalent, and Path 2 refuses it. Of decision 8's three bind-error examples, two wait on X5 and on LIMIT
  expressions, which no PR of this phase opens.
- **Decision 9, as built.** Each block has a per-row slot above its joins and below the filter of its correlated
  conjuncts, which Path 1 and Path 2 take with the block; the left input of a unit that pads it (a FULL JOIN) has one
  of its own, below the unit, which answers ADR 0037's OJ3 candidate (uncorrelated IN, NOT IN and EXISTS conjuncts
  there, and values); and HAVING's slot stands above the filter of HAVING's other conjuncts, below the select list's
  computed columns. "Filtered above its slot" is read as each conjunct that holds a value being a filter of its own
  after the joins of its values, in the order bound -- what it computes, then its predicate -- while a plain conjunct
  still goes onto its relation, as DuckDB's filter pushdown puts it below the values. One filter for the whole slot
  above all of its joins would compute later conjuncts' operands over rows that earlier ones drop.
- **Decision 10, as built.** A value is bound once per slot, keyed by the slot and the subquery's structure, its first
  binding planting its one-row join: the copies the binder makes of a conjunct (a BETWEEN's two comparisons, IS
  DISTINCT FROM's lowering, CASE x WHEN's equalities, the copy a folded or moved comparison makes, an IN list's
  equalities) and a subquery written twice in WHERE and the inner ONs all read one join. With an IN that turns out
  empty written between two occurrences, the first-occurrence placement hides the value's error (D35). OR
  factoring's dropped branch binds as a dry run, which binds each subquery's block for its bind errors and its own
  refusals (a correlated scalar subquery that is no aggregate over every row, say), plants nothing, and refuses no
  correlated subquery for where it stands; a correlated conjunct every branch of an OR shares is answered through ADR
  0023's paths.
- **Decision 3 is corrected:** DuckDB answers a subquery in a LEFT JOIN's ON only in a conjunct that reads the
  right input alone -- a value, an IN or an EXISTS, one correlated to that input too -- and refuses every other with
  "Cannot perform non-inner join on subquery!": any in a FULL JOIN's own ON, and in a LEFT JOIN's ON one whose
  conjunct reads the left input or no column at all. Those refusals are DuckDB's own, so no coverage item answers
  them; the conjuncts over the right input alone stay decision 3's coverage item.
- **Decision 20, a note for SQ5:** a conjunct's other value joins stand between a correlated value's join and the
  conjunct's filter, so the consumer proof's walk must pass uncorrelated one-row joins, or SQ5 plants a conjunct's
  correlated join last.
- **Decisions 23 and 25.** A correlated value outside a direct top-level comparison, and a correlated EXISTS outside
  a top-level conjunct, exit 4 once their blocks bind, after their own bind errors, with messages that name what
  answers them. D16 now says that a value is computed before an AND, OR or CASE decides, and an expression over it per
  row, where DuckDB computes a subquery's side of a comparison at its join's build and folds by statistics; D35 gains
  the first-occurrence placement and the dropped branch; D14 notes that a value's join still builds under a folded
  comparison; D22's lists of values are answered; D36 registers which error several failing subqueries report, live
  since SQ4a.
- **Decision 26, PF16.** On a synthetic file of 400,000 rows in 98 parts, SQ4a's shapes keep their counters (a top-N
  with a value reads 98 parts where the literal form prunes 97); a value under OR costs the top-N its late
  materialization; and a comparison with a value is no scan filter (400,000 rows leave the scan, 376,000 with the
  literal). No TPC query's plan regresses, so PF16 stays a guard, and these figures are SQ2's baseline.
- **Decision 28.** `tests/slt/cases/subqueries/conditions.slt` holds the forms, written first under `pending SQ1a`
  and checked against DuckDB, with the divergence pins and the DuckDB-only boundaries; the generator's Feature
  `condition_value` writes values inside WHERE conditions from a stream of its own, over SQ4a's blocks; the
  metamorphic relations include the self-check for decision 8's residual.

## Update (2026-11-10, SQ1b): values in HAVING conditions and decision 23's row, built

SQ1's second PR. With it SQ1 is built; no query completes, and no TPC-H or TPC-DS plan changes.

- **Decision 8 in HAVING.** An uncorrelated scalar subquery is a value anywhere in a HAVING condition, and an
  uncorrelated EXISTS a condition there, as SQ1a made them in WHERE. HAVING's slot (decision 9) stands above the
  aggregation and the filter of HAVING's conjuncts without one, below the select list's computed columns: a conjunct
  that holds a value is a filter of the slot after its value's join, computing what it reads over the groups there.
  SQ4a's scalar and EXISTS forms in HAVING read the memo too, so a subquery written twice in HAVING is one join; an IN
  keeps its semi or anti join in the slot; a BETWEEN that holds a value is its two comparisons, as in WHERE. A
  subquery in WHERE and the same text in HAVING are two values, one per slot.
- **No per-row join for a HAVING operand.** The binder's speculative binding of a HAVING operand as a GROUP BY key
  skips one that holds a value, which would plant its join per row, below the aggregation; an aggregate's computed
  argument in such a conjunct stays below the aggregation.
- **Decision 23's SQ1 row, as built.** The parser admits an aggregate in a WHERE or ON inside a subquery only where some
  subquery expression around it stands in HAVING (a derived table's or a CTE's query starts afresh); elsewhere its
  parse error stays, exit 1 as DuckDB's bind error. The binder judges such an aggregate by the block it belongs to, the
  nearest whose columns its argument reads, each block noting whether it binds a WHERE or ON condition while a
  subquery's block in it binds. An enclosing block's whose subquery stands in its HAVING is exit 4 until SQ13; any
  other -- of the subquery's own columns, of no column, a mix, or one owned through a WHERE or ON -- is a bind error,
  "WHERE clause cannot contain aggregates!" (DuckDB's text depends on where the subquery stands). A constant argument
  (`SUM(1)`) keeps the gate's refusal, exit 4, where DuckDB raises a bind error of its own; the parser refuses an
  aggregate anywhere inside a derived table or a CTE body within such a subquery (exit 4), which DuckDB may answer.
  The row's select-list half moves to SQ2, as the SQ1 plan put it: under a subquery in the select list or ORDER BY the
  parse error stays, where DuckDB answers an outer aggregate.
- **Decision 28.** `tests/slt/cases/subqueries/having_conditions.slt` holds about 200 records of HAVING values, each
  Unsupported before and checked against DuckDB, and decision 23's row, its outer aggregates pending SQ13; the
  generator's Feature `having_value` adds a HAVING conjunct with a value from a stream of its own; three metamorphic
  relations cover HAVING values.

## Update (2026-11-11, SQ2a): values in the select items and ORDER BY of blocks that do not aggregate, built

At the maintainer's request SQ2 is two PRs: SQ2a, values in the select items and ORDER BY of a block that neither
groups nor aggregates; and SQ2b, values in GROUP BY keys and aggregate arguments, in select items and ORDER BY over
aggregates, and decision 23's select-list half. SQ2a completes Q9, and the ratchet is at 79.

- **Decision 8, as built in blocks that do not aggregate** (no GROUP BY, no aggregate in the select list, HAVING or
  ORDER BY): an uncorrelated scalar subquery is a value anywhere in a select item or an ORDER BY item -- alone, in
  arithmetic, functions, COALESCE, casts and CASE, next to a star and under SELECT DISTINCT -- and an uncorrelated
  EXISTS, an IN list and a BETWEEN with values are conditions in a CASE WHEN there, as in a condition. That holds in
  such a block of an IN subquery, a scalar subquery, a derived table, a CTE and a compound's arm too. In a block that
  aggregates a value there is refused until SQ2b, with its own message. Still exit 4: EXISTS and an IN subquery as a
  value of their own (decision 3's BOOLEAN items; IN under a CASE is SQ7's), a correlated value or EXISTS outside a
  condition (SQ5, SQ8), LIMIT and OFFSET, a LEFT or FULL JOIN's ON, and a compound's own ORDER BY (ADR 0036's X5).
- **Decision 9, as built.** These values join in the per-row slot WHERE's values join in, below the computed items,
  the sort, DISTINCT and the projection; the assembly of a block does not change. **Decision 10:** the memo covers
  them, so a subquery written in a select item and in ORDER BY is one join.
- **The select list of an EXISTS block.** DuckDB drops it for an uncorrelated EXISTS too, so an item that holds a
  value and that its block's ORDER BY does not read binds as a dry run, as OR factoring's dropped branch does: its bind
  errors and refusals stay, it plants nothing, and a computed 0 keeps its place so the items after it keep their
  positions. The ORDER BY reads an item as it binds: by its position, the unexpanded stars' columns counted, by a bare
  name that the item is the last alias of, or inside an expression by a name that no column of the block has and the
  item is the last alias of. (A block with GROUP BY or HAVING aggregates, so SQ2b settles theirs; any position or name
  equal to the alias counts there for now.) A value there that would fail answers, as in DuckDB, and so does a
  correlated value of the shape Path 2 takes, an aggregate over every row; a correlated value of another shape keeps
  its refusal. Under SELECT DISTINCT and in a compound the list is the block's rows, and is computed. A value in the
  list of a derived table or a CTE right under the EXISTS block still joins in that block's slot, although D32 drops
  the list, so its error fails the query where DuckDB answers (D32 and D35 now say so).
- **Decision 11, as built.** An unaliased select item that holds a subquery is named by antb1's own text of the item,
  under D8. The column count of a value is DuckDB's class of bind error, "a scalar subquery must have one column, not
  N".
- **D37, a new divergence.** An ORDER BY expression over the alias of an item that holds a value binds the item's
  expression again, and the memo returns the same value, so antb1 answers where DuckDB raises a bind error ("Alias ...
  referenced in a SELECT clause - but the expression has a subquery"). The maintainer chose to answer it
  (2026-11-11), an exception to the rule that a newly opened place raises DuckDB's binder errors. The alias alone and
  the item's position answer in both engines.
- **D35** gains SQ2a's values: one next to a condition DuckDB's filter combiner proves never true, and one in a
  derived table's column that nothing reads (D32), fail in antb1 and answer in DuckDB. D16's eager values cover the
  select items and ORDER BY.
- **Decision 26, PF4 and PF16.** Q9 at SF 1 takes 3.4 s in the release build on Apple silicon and peaks at 9.8 MiB
  (ADR 0037's SQ2a update), so the budget gate does not fire and PF4 stays conditional, although Q9's values are
  fifteen scans of one table that PF4 would merge into one. On SQ1a's synthetic file of 400,000 rows in 98 parts, a
  value in a top-N's select item costs the top-N its late materialization, and under a WHERE that the literal form's
  part pruning reduces to one part, the pruning too: 98 parts and 376,000 rows read, against 1 part and 3,850 rows.
  No TPC query's plan regresses, so PF16 stays a guard, with these figures added to SQ1a's.
- **Decision 28.** `tests/slt/cases/subqueries/select_values.slt` holds about 250 records, written first under
  `pending SQ2a` and checked against DuckDB, with the D35, D32 and D37 pins; the generator's Feature `item_value`
  adds a select item that holds a value or a CASE over one, from a stream of its own; nine metamorphic relations
  cover select items, ORDER BY and an EXISTS block's dropped list.

## Update (2026-11-11, SQ2b): values in blocks that aggregate and decision 23's select-list half, built

SQ2's second PR. With it SQ2 is built; no query completes, the ratchet stays at 79, and no TPC-H or TPC-DS plan
changes.

- **Decision 8, as built in blocks that aggregate.** An uncorrelated scalar subquery is a value in a select item or an
  ORDER BY item over GROUP BY keys and aggregates, in grouped and global blocks; in a GROUP BY key; and in an
  aggregate's argument that reads a column, in the select list, HAVING and ORDER BY. HAVING reads the alias of a select
  item that holds one. Still exit 4, each with its own message: a subquery written inside a ROLLUP, CUBE or GROUPING
  SETS element (refused as a constant there is; a plain key next to them may hold one, and an alias or a position of a
  value item inside them binds as a key); an aggregate's argument of values and literals alone (`SUM((q))`, until
  constant aggregate arguments); and a value over the aggregate of a correlated subquery, whose join would stand above
  the aggregation Path 2 unnests (SQ5 and SQ0 teach it). A correlated subquery or EXISTS over a block's aggregates
  outside HAVING now names that place in its refusal.
- **Decision 9, as built.** A GROUP BY key's and an aggregate argument's values join in the per-row slot, below the
  aggregation; a value over the keys and aggregates joins in HAVING's slot, after it, and so does a HAVING conjunct
  that reads a value item's alias, as a filter of that slot.
- **Decision 10: the lookup-only speculation.** The binder's speculative binding of an output expression as a GROUP BY
  key only looks values up in the memo and never binds or plants one, so `k % 2 + (q)` in the select list, HAVING or
  ORDER BY matches the key written alike (or named by alias or position), one join below the aggregation; an
  expression that no key holds plants its value after the aggregation, as SQ1b required. The flag is saved and
  restored, so that a nested speculation could not end it.
- **Decision 23's select-list half.** The parser admits an aggregate in a subquery's WHERE or ON where some subquery
  around it stands in HAVING, the select list or ORDER BY; inside a derived table or a CTE body under such a subquery
  it is exit 4, as SQ1b made it under HAVING, since DuckDB answers an enclosing block's aggregate there (the plan kept
  the parse error, on the critique's P2-3, believing DuckDB failed both: its review found it answers the outer one).
  The binder judges the aggregate by the place in the owning
  block where the subquery stands (`Scope::SubqueryPlace`): HAVING, the select list or ORDER BY make it an outer
  aggregate, exit 4 until SQ13; GROUP BY gives DuckDB's "GROUP BY clause cannot contain aggregates!", an aggregate's
  argument "aggregate function calls cannot be nested", a WHERE or ON "WHERE clause cannot contain aggregates!". A
  constant argument (`SUM(1)`) there is the binder's bind error now, exit 1 as in DuckDB, since the gate leaves an
  aggregate in a WHERE or ON to the binder (SQ1b had it at exit 4 under HAVING).
- **D35** gains three members, in which antb1 answers and DuckDB fails: an aggregate item's value binds with the select
  list, before WHERE, so its join stands below a WHERE IN; a per-row value (a GROUP BY key's, an aggregate argument's)
  stands below the aggregation, so below every HAVING IN; and a value over the aggregates binds before HAVING, below a
  HAVING IN. When that IN turns out empty, the value's error is never reported (the maintainer chose to register such
  members rather than reorder the joins, 2026-11-11). **D38**, new: a GROUP BY key that holds a subquery is matched only
  by the subquery written alike as parsed, so another select alias or another case of a name inside it is a bind error
  where DuckDB answers; a looser comparison that merged two different subqueries would answer wrong. **D37** covers
  grouped items: DuckDB answers the alias of a key item, or of an aggregate over a value, inside an ORDER BY expression,
  and refuses one whose item still holds the value.
- **Decision 28.** `tests/slt/cases/subqueries/group_values.slt` holds about 390 records, written first under `pending
  SQ2b` and checked against DuckDB, with the D35, D32 and D37 pins and the DuckDB-only boundaries; the generator's
  Feature `group_value` adds a value block, or a COUNT of a CASE over a condition with values, to a block that
  aggregates; six metamorphic relations cover keys, arguments, values over aggregates, global blocks, HAVING aliases
  and ORDER BY.

## Update (2026-11-12, SQ7a): IN and NOT IN inside conditions through a null-aware mark join, built

SQ7 is two PRs at the maintainer's request: SQ7a, the mark end to end, and SQ7b, the IN forms (`= ANY`, `= SOME` and
`<> ALL`, NOT parity folded at the top level, constant and value probes, IN over a set operation, and decision 23's
type bind error). SQ7a completes Q45: the ratchet is at 80. No other TPC-H or TPC-DS plan changes.

- **Decisions 12 and 13, as built.** `JoinKind::kNullAwareMark` takes the null-aware anti join's trait and a new one,
  `appends_mark`: each left row comes out once, then the mark. `PaddedJoinRows` gives it the left input's rows, and
  `EmptyBuildEmptiesJoin` and `ProbeDropsUnmatchedRows` start with the `appends_mark` test, so they hold for SQ8's
  `kMark` too; every other reader of the traits is unchanged. `JoinNode::mark` is the column; `OutputIds`, the
  resolver, `IdsOfNode`, `CloneWithFreshIds` and the shared sub-plans' fingerprint carry it, and the resolver now
  fails a mark on any other kind, a kind that appends one without it, a left-built mark and a residual on a kind that
  takes none (unchecked before for every kind). The operator reads the build's `empty()` and `has_null()` once, at
  its start: FALSE throughout, NULL throughout (a set of NULL keys alone), or a lookup that appends TRUE, FALSE or
  NULL per row and keeps the probe's selection. A mark publishes no dynamic filter.
- **Decision 14, as built.** The gate admits an IN in a condition that takes values with the rules of a top-level IN
  conjunct: a probe that reads a column (an aggregate in HAVING and in a `CASE WHEN` over aggregates), no value in
  it, and no set operation (both SQ7b's). `BindBool` binds it through `MarkValue`, in this order: the probe's
  enclosing-block check (exit 4, as a top-level IN's), the probe itself, the memo, the lookup-only speculation, the
  refusal over a correlated block's aggregation (as a value's), the block, the IN key typing a semi join uses (shared,
  so the three kinds agree), the dry run, and the plant. IN as a BOOLEAN value outside a condition stays exit 4
  (decision 3).
- **Decision 10 for marks.** A mark is remembered by its slot, its set's query and its probe as bound, not as written:
  the probe binds first, so a text that resolves differently in two clauses of a slot (an inner ON sees fewer FROM
  items than WHERE does) is still that clause's bind error, and two spellings of one column are one mark, so a
  `GROUP BY` key that holds a mark is matched where D38 refuses a value. IN and NOT IN of one probe and set share one
  mark, NOT IN reading it under a NOT.
- **The slot's tail (the maintainer's choice, 2026-11-12).** The select list binds before WHERE, and its expressions
  over aggregates before HAVING, so a mark planted then would compute its probe below their filters, for rows and
  groups DuckDB never computes it for. Such a mark goes into its slot's tail, assembled after the slot's own entries
  and so above WHERE's and HAVING's filters, below the computed columns. A later clause whose memo lookup finds it
  moves it to the end of the slot's own entries, where that clause computes the probe anyway, and so does HAVING that
  reads it through the item's alias (found by the review); ORDER BY binds last.
  Values keep their places (their joins compute nothing per row).
- **OR factoring first.** An IN that every branch of an OR shares is a top-level conjunct after factoring: a semi
  join, no mark. Its error visibility is D35's new member (the maintainer's choice, 2026-11-12): over an empty set
  the semi join empties and a failing value written before it never builds, where DuckDB keeps a mark per branch and
  fails.
- **Divergences.** D16 gains a mark written before a value conjunct, whose probe is computed before the value's
  filter; D32 a mark's probe keys over an empty set; D35 the shared IN, a mark's set next to an IN conjunct that turns
  out empty, and a contradiction pair next to a failing set; D37 an ORDER BY expression over an item's alias that
  holds a mark. Each is pinned in `tests/slt/cases/subqueries/in_marks.slt`.
- **Decision 28.** `in_marks.slt` holds about 370 records, written first under `pending SQ7a` and checked against
  DuckDB, with the pins and the DuckDB-only boundaries; the guards of `conditions.slt` that SQ7a answers are lifted
  and the rest, with `setops/errors.slt`'s, wait for SQ7b. The generator's Feature `in_mark` writes a mark in WHERE,
  HAVING, a select item or an ORDER BY item, under OR, NOT and CASE, over sets with a NULL, of NULLs alone, empty and
  limited; 47 metamorphic relations hold it against NOT IN, the union of an OR's branches, the semi join's counts and
  its three-valued composition from a LEFT JOIN, at three batch sizes.
- **Q45.** On Apple silicon antb1 answers Q45 at SF 1 in 0.10 s in the release build (26 ms inside the engine on 14
  threads) and 0.16 s in the Debug build, and it peaks at 8.1 MB; shard 6 of 8, which holds it, takes 43 s under
  AddressSanitizer here, far inside the 870-second inner timeout.

## Update (2026-11-13, SQ7b): the IN forms, built

SQ7's second PR. With it SQ7 is built; no query completes, the ratchet stays at 80, and no TPC-H or TPC-DS plan
changes.

- **Decision 14's forms, as built.** The parser reads `= ANY` and `= SOME` over a subquery as IN and `<> ALL` and
  `!= ALL` as NOT IN, the node's operator spanning the comparison and the quantifier; every other quantified
  comparison, and a quantifier over anything but a query, is unsupported at the quantifier (decision 3). A top-level
  conjunct peels any number of NOTs over an IN and folds their parity with the IN's own, exact in three-valued logic, so
  `NOT (x NOT IN q)` and `NOT NOT (x IN q)` are semi joins; EXISTS keeps one NOT (a NOT NOT EXISTS is a value).
- **Probes.** An IN's probe is any operand a condition with values takes: a constant, which is a key computed in the
  join's own layer (a semi join with a constant key stays above the block's joins), or an expression holding a
  subquery's value, planted below the IN's join. A string literal against a DATE or TIMESTAMP set is parsed into that
  type, as a comparison parses it; against a number set it is D3's bind error, as in a comparison. A NULL literal
  probe stays exit 4.
- **Sets (ADR 0036's X6).** The set may be a set operation whose arms read no enclosing block, bound as a scalar's and
  an EXISTS's are since SQ4a, its column of the arms' unified type; a correlated arm stays SQ15's, and a compound's own
  ORDER BY, LIMIT or OFFSET X5's.
- **Decision 23's SQ7 row.** A VARCHAR against a value of another type family, and a DATE or a TIMESTAMP against a
  number, is DuckDB's bind error (exit 1), checked where the IN's key is typed (`InKeyType`, shared by the semi, anti
  and mark joins), so also under a dropped OR branch; a DATE against a TIMESTAMP, which DuckDB answers, stays a refusal
  (no key type holds both). The review added TIMESTAMP to the rule, since DuckDB's is the same for it.
- **Decision 28.** `tests/slt/cases/subqueries/in_forms.slt` holds about 300 records, written first under `pending
  SQ7b` and checked against DuckDB, with D3's pin; the guards of `conditions.slt`, `setops/errors.slt` and
  `in_marks.slt` that SQ7b answers are lifted. The generator's Feature `in_form` writes the quantified spellings,
  literal probes of the set's family and set operations of two or three arms, in a mark's places and as a conjunct
  under folded NOTs; `in_mark`'s NOT form keeps a mark (a NOT over an AND that holds it). 49 metamorphic relations
  check them against the three-valued composition from a LEFT JOIN, the OR or AND of a set operation's arms, a one-row
  column holding a constant probe, and an inner join for the folded NOTs.
- **From the review.** A mark is a value of the condition, so a CASE over `2 IN (q)` compared with a number is no
  comparison of two literals (`ReadsColumn` counts an IN's mark as it counts a scalar's or an EXISTS's value).

## Update (2026-11-13, SQ0): one rule table for the unnesting, built

SQ0 changes no answer, no plan of a shape answered today and no refusal message. No query completes and the ratchet
stays at 80.

- **Decisions 16 and 17, as built.** `src/plan/unnesting.{h,cc}` holds the pass, moved out of `optimizer.cc`.
  - A dependent join's kind picks its mode through `JoinKindTraits`: lift mode for a kind that filters its left input
    and takes residuals (semi, anti: ADR 0023's Path 1), value mode for a one-row join (Path 2). Every other kind keeps
    "a correlated subquery is not supported"; a null-aware anti join and a mark take no residual.
  - One exhaustive visitor, the descent, has a rule per node type and no other overload. It walks from the dependent
    join's right input to the correlated filter (the first Filter over a Compute):
    - lift mode drops a Project, a Compute or a Sort above it;
    - value mode keeps the maps, groups the static Aggregate and substitutes the Compute of its arguments;
    - every other node refuses the shape with today's message. **The Join rule is deferred to SQ8** (the maintainer's
      choice, 2026-11-13): no SQL reaches it, since the binder pulls every correlation above the block's joins.
  - The checks run in today's order -- shape, calls, maps and empty values, conjuncts, representatives -- so a shape
    with two defects keeps today's message. A test pins it.
- **Decision 18.** Substitution is the only terminal step. Lift mode moves every correlated conjunct into the join,
  so it builds no classes and leaves nothing to substitute; value mode replaces each outer reference by its
  representative, keys in ascending outer column id, as before.
- **Decision 19.** `EmptyValueOf` gives each call's empty value of the call's own type (0 for the COUNT kinds and a sum
  of counts, NULL otherwise); the static aggregate's rule carries them through the maps, replacing
  `StrictAggregates` and `NullOverNoRows` with the same outcomes.
- **Decision 20, read literally.** The proof follows the subquery's value from the join's parent up through Computes
  to the first Filter that reads it, which must be true for no row holding the empty value (`TruthOver`). A Filter
  that reads no value counts as a node on the way and leaves the join LEFT. The verdict is recorded and decides
  nothing: the join back stays INNER. Every SQL shape answered today passes, checked by making a failed proof fatal
  over the guard and the TPC-derived plans. **For SQ6:** the hand-built `optimizer_test` plans with no consumer above
  the join fail it, correctly; when the proof decides, they gain a consumer Filter or expect LEFT.
- **Decision 28.** The guard, outside the repository, held 570 correlated queries (every correlated `.slt` record,
  `binder_test`'s correlated SQL, the CLI golden and boundary shapes) with main's EXPLAIN, answers and exit codes:
  every commit showed no difference, and nor did the EXPLAIN of every TPC-derived query. A shadow test compared the
  rule table with a frozen copy of the paths, plan by plan and id by id, until the paths were gone. What only the guard
  saw is now pinned by CLI goldens: a key named by its last spelling, keys in outer-column order, cast order, and the
  order of refusals across nested and stacked subqueries.

## Update (2026-11-13, SQ6): the LEFT join back and the empty values, built

SQ5 and SQ6 split at the maintainer's request, SQ6 first. SQ6 completes Q41 and the ratchet goes from 80 to 81. Before
it, a separate fix (#363) kept ADR 0032's aggregate placement from moving a non-strict `Compute` below a join that pads
the aggregated side. That was a wrong answer on main, unrelated to subqueries, and SQ6 cannot reach it. It becomes
required once a whole-argument value `SUM((q))` opens, where a `COALESCE(count, 0)` over a LEFT join back would be such
a `Compute`.

- **Decision 19, as built.** The static aggregate's rule answers every call but the single value: COUNT in each
  spelling, a sum of counts and the variance family. The single value stays out, a deviation from decision 19's
  table: the binder puts none under a dependent join, and the executor has no grouped state for one. The strictness
  refusals are gone. The empty values carry through the maps as before, and through a HAVING: NULL where it rejects
  the empty row, the value where it keeps it, unknown otherwise. They fold no constants, so `COUNT(*) + 1 > 1`, which
  rejects the 1 over no rows, still joins back LEFT. That is correct, only slower; folding them is left as a refinement.
- **Decision 20 decides.** The proof's verdict picks the join back. The hand-built `optimizer_test` plans with no
  consumer gained the Filter their SQL has, and LEFT twins without one.
- **Decision 21, as built.** Where the proof fails, the outer side is joined to the GroupAggregate by a LEFT JOIN,
  built on its right input (`kLeft` has no `may_build_left`). Above the join:
  - a COUNT-kind call, computed under a fresh id, is read under its own as `COALESCE(fresh, 0)`;
  - the maps follow, the select list's Project as a Compute that defines its ids;
  - a HAVING is a Compute of one BOOLEAN condition, and every map above it reads `CASE WHEN condition THEN value END`.
    That holds on the INNER join back too, over the group: decision 17's "never a filter", which a binding whose group
    the HAVING rejects needs in order to read NULL. Where the value the join appends is defined below the HAVING (a
    bare `SELECT COUNT(*) ... HAVING`), its id is renamed below and redefined as the CASE above. A HAVING folded to
    FALSE reads a group key, false or NULL alike to a CASE;
  - an uncorrelated one-row join in the slot after the aggregation moves up with the maps, so a binding without rows
    reads `0 + value`. This is the narrow piece of decision 17's Join rule that SQ6 needs, landed with the
    maintainer's agreement (2026-11-13): a one-row join over the static aggregate whose right input reads no outer
    column. Every other join stays SQ8's and SQ10's;
  - a NULL-equal correlation reads the NULL group, as before.

  An IN or NOT IN conjunct in a correlated scalar subquery's HAVING, and a mark over its aggregate, stay refused: no
  rule takes a semi or mark join there. The IN conjunct binds in full and the unnesting refuses it, so its bind errors
  come first, as on main and in DuckDB.
- **The semi-join reduction.** D5's exception (one key over a bare scan) applies to the INNER join back only. D5 offers
  keys to the aggregation under a probe that drops unmatched rows, which a LEFT join's is not, so the LEFT join back is
  restricted over one key too. For a stack of dependent joins it no longer declines (the maintainer's choice,
  2026-11-13): it clones the base below the stack, through Filters, Computes that define no key, a dependent join's
  left input and a join that pads no left row. Each rewrite of a stack clones once, so the plan stays linear. A key
  computed above the base, or a base that holds a Limit, declines.
- **Lemma 3.1.** The check also verifies that what a rule puts above the join (maps, a HAVING, a moved one-row join)
  reads no free variable the dependent join did not.
- **Divergences.** D24 gains the LEFT join back, DuckDB's D for every COUNT shape (decision 25), and an error the
  INNER join back never computes: a HAVING operand or a select item that fails only over the empty values fails DuckDB
  alone. D35 notes that a LEFT join back never empties its join. D16 notes a HAVING's operands computed over the empty
  row too.
- **Decision 28.** `tests/slt/cases/subqueries/empty_values.slt` began as 130 records written as `pending SQ6` before
  the code, each answered by it; later records pin what the mutation checks and the review found the first ones
  missed (a bare call under a HAVING that rejects a real group, a chained HAVING, concat over no rows, a stack over a
  FULL JOIN, and both engines' outcomes of D16's HAVING operands). `scalar.slt`'s and `group_values.slt`'s SQ6 guards
  are on. DuckDB's own
  wrong answers in decision 29's family (a constant item, or a HAVING, over calls that are all NULL over no rows) have
  no record; the metamorphic self-checks cover them. The SQ0 guard showed each change to be a refusal turned into an
  answer equal to DuckDB's, a reworded refusal, or a stacked plan gaining its upper restriction with its answers
  unchanged. Of the TPC-derived plans, only Q41's changed.

## Update (2026-11-13, SQ5a): correlated values in conditions, built

SQ5 splits at the maintainer's request: SQ5a answers correlated values in conditions and completes Q6, and the ratchet
goes from 81 to 82; SQ5b answers them in select items, ORDER BY, GROUP BY keys and aggregate arguments.

- **Decision 20, amended.** With a value anywhere in a condition, other nodes stand between its join back and the
  condition that rejects its empty value. The proof's walk from the dependent join's parent now passes, besides a
  Compute, a Filter none of whose conjuncts decides and a join reached from its left input that pads no left row
  (inner, LEFT, semi, anti, null-aware anti, one-row, mark); it is INNER at the first Filter that rejects the empty
  value, and LEFT at any other node. Each node passed decides each of its rows by that row alone and keeps its
  columns, so the rows of a binding without a group all reach that Filter and fail there. The walker records the input
  of each node it came up from (`Ancestor`), so a join's inputs are told apart without comparing pointers. No verdict
  of a shape answered before changed.
- **Decisions 8 to 10 for a correlated value, as built.**
  - Its block's dependent join is planted in the slot as an uncorrelated value's one-row join is, in any expression of
    a condition of WHERE or an inner ON: under OR, NOT, CASE, arithmetic, COALESCE, BETWEEN, an IN list or a function,
    as an IN or mark probe, and two different correlated subqueries compared, the first a value below the second's
    join, in the order written.
  - The memo is keyed by binding. A correlated text binds to the same ids wherever the same FROM items are visible, so
    a second occurrence there is the remembered value without binding again. Inner ONs bind before WHERE, in FROM
    order, so an occurrence under other visibility is a later one: WHERE, which sees more FROM items than an inner ON
    (a name the ON resolved can be ambiguous there, which a memo keyed by text would hide), or the ON of another comma
    group, which resolves a name in its own group first. It binds again, for its own bind errors, and is a remembered
    value only if it reads the same ids. That comparison decides answers: in the ONs of two comma groups one text can
    read `ct_id` of two tables, two values. A lookup takes the entry of the same visibility first, then one of the same
    ids, so each value's later occurrences find its own join.
  - A direct comparison operand records its value too (the maintainer's choice), and the same text on both sides of
    one goes to the general path: one join per value and slot.
  - A place's refusals (a padded unit's left input, a conjunct that reads an enclosing block, EXISTS) come before the
    memo, so a value remembered from another conjunct carries no conjunct past them.
- **What stays exit 4.** A select item, ORDER BY, a GROUP BY key and an aggregate's argument, with a message of their
  own until SQ5b; a correlated subquery in a conjunct that also reads an enclosing block, whose records are `pending
  SQ8` (the maintainer's choice); and every boundary the plan lists (SQ13, decision 3, constant aggregate arguments).
  A shape the unnesting refuses now reports its own message where the binder's direct-operand one stood, and a bind
  error that a correlated value under OR hid now comes first, at exit 1, as in DuckDB.
- **Divergences (the maintainer accepted them).** D24: over an inner join back antb1 computes whatever stands between
  the join back and the condition that rejects the value over no rows only for the outer rows with a group, so an
  error raised only for the others fails DuckDB alone: an earlier condition whose operand fails only over the value
  over no rows, an operand of the deciding condition that reads no value, and a condition between the two that reads
  no value (an IN conjunct's probe, another value's comparison). The last also holds with the first value a direct
  operand, which main joined back LEFT apart from the second, failing as DuckDB does. D16 and D35 name correlated
  values with uncorrelated ones; with the direct operand remembered, an IN that turns out empty written between two
  occurrences of a value now hides its error, as D35 says of an uncorrelated value, where main planted a second join
  above the IN and failed, as DuckDB does.
- **Decision 28.** `correlated_values.slt` holds 204 records written as pending before the code (98 SQ5a, 97 SQ5b, 7
  SQ13, 2 SQ8); SQ5a answers its 98 and `conditions.slt`'s four, and pins D24's members with each engine's outcome. The
  review added eleven: the ONs of two comma groups, an EXISTS block's item that repeats a value of its WHERE, and D24's
  third member. Each of ten mutations fails named tests, among them the three the review asked for: one drops the ids
  comparison above, one takes the first entry written alike, and one admits a dry run's memo hit (an EXISTS block's
  dropped item). The SQ0 guard showed only refusals turned into answers equal to DuckDB's and reworded refusals; outside
  its corpus, the shapes of D24's third member and D35 above, which main failed at exit 1, now answer. Of the
  TPC-derived plans, only Q6's changed.
- **Follow-ups, outside SQ5.** The semi-join reduction above a nested value (`ReadsAJoinOrUnion` counts no dependent
  join, so D5's exception applies where D5 cannot reach); a join whose key reads a value NULL over no rows counted as
  deciding; constant folding in the proof.

## Update (2026-11-14, SQ5b): correlated values in select items, ORDER BY, GROUP BY keys and aggregate arguments, built

SQ5b answers a correlated value in a select item, an ORDER BY item, a GROUP BY key and an aggregate's argument that
reads a column, in any block. No query completes, the ratchet stays at 82, and no TPC-H or TPC-DS plan changes.

- **The place.** Admit loses its refusal of every clause but WHERE and an inner ON, and keeps three: a padded unit's
  left input, a conjunct that also reads an enclosing block, and a correlated EXISTS. Above a block's aggregation
  `BindSubqueryBlock` still refuses a correlated block (SQ13), so that boundary needs no new check.
- **The tail.** A block binds its bare aggregate items before every ON, WHERE and GROUP BY, and its other select items
  after them, all before HAVING and ORDER BY. A correlated value either pass binds goes to the slot's tail, above
  WHERE's filters, as a select-list mark does (SQ7a): its join back computes the subquery's maps, and a COUNT's
  COALESCE, per outer row, so only over the rows WHERE keeps. SQ7a's "values keep their places" now holds for
  uncorrelated values, whose plans are unchanged, and D35's SQ2b member stays theirs.
- **Promotion.** A later clause that reads a tail value moves it to the end of the slot's own entries, below that
  clause's filter, join or mark. `TakeFromTail`, factored out of SQ7a's mark promotion, finds the entry by its join's
  sub-plan; it runs at both memo hits, in the GROUP BY speculation and for marks. Every reader binds the value before
  it plants its own entry, so the promoted join stands below its reader and above the filters of the conjuncts written
  before it. A dry run promotes nothing. Without the move a WHERE filter, a BETWEEN, an IN or mark probe, an inner ON
  or the other side of a direct pair would stand below the join and read a column nothing defines there.
- **The GROUP BY speculation.** `BindOutput` only looks values up. A correlated value written alike is the entry some
  GROUP BY key reads (a key that is the value or an expression over it, `(q) % 2`), else the first written alike (a
  key that is a mark over the value reads the mark). A wrong pick matches no key, so the speculation fails into SQ13's
  refusal, never into a wrong answer. Two entries of one text need the ONs of two comma groups, a text that is
  ambiguous at full visibility, so the key step is defensive. HAVING's direct comparison over a correlated key, `(q) >
  0`, is read as that key through the same path (the maintainer's choice); with no key reading the value, SQ13's
  refusal follows, as before.
- **The proof and the passes, unchanged.** A tail value's walk meets the block's aggregation, sort or Project before
  any Filter that rejects its empty value, so it is joined back LEFT (`Join LEFT build=right`, keyed by unique group
  keys: each outer row comes out once). A value that a rejecting condition of WHERE or an inner ON reads too stands
  below that condition and gets its verdict, INNER. LimitBelowProject stops directly above a join back. Aggregate
  placement sees both sides of a join back in an argument that reads a value (one that reads no column is refused),
  declines a COUNT(*)-only call list and a COUNT-valued key, and groups a SUM-valued key early by the join key. Join
  ordering never moves a join back, and the dynamic filters skip a LEFT join built on its right.
- **Divergences (the maintainer accepted them).** D24 gains three members. Under a LIMIT without ORDER BY the subquery's
  own item, above the LEFT join back, is computed only for the rows kept, where DuckDB computes it for every binding. A
  WHERE or inner-ON comparison of a value that a bare aggregate item holds reads the item's value rather than being a
  direct operand, so over a SUM an operand that reads no value is computed only for the outer rows with a group. And a
  select item's value over keys no outer row binds fails antb1 alone where the semi-join reduction declines and DuckDB
  keeps D (the outer side or the query of its IN has a filter of its own). D35 gains the converse: where the reduction
  restricts the aggregation by an IN that turns out empty, antb1 answers where DuckDB, keeping no D, fails. D32: where a
  global block's or a correlated EXISTS block's ORDER BY is dropped, a correlated value's join back stays, but the
  subquery's own item above it is pruned, so its error never shows where DuckDB's does; a derived table's unread
  correlated value fails antb1 alone, as an uncorrelated one does. D16, D37 and D38 name correlated values.
- **Decision 28.** `correlated_values.slt` grows from 240 records to 361. Before the code came 100: 96 more pending SQ5b
  (the new places, the killers of the tail's and the speculation's mutations, the passes' records, HAVING over a key),
  one pending SQ13, one pending SQ8 and two errors both engines report. After it came 21 more: eight divergence pairs
  (D24 three, D32 three, D35, D37), two controls, a correlated D38 member pending SQ13 and three records the review
  asked for, less one duplicate. SQ5b answers its 192 pending records and `group_values.slt`'s two; 12 stay pending (9
  SQ13, 3 SQ8). The SQ0 guard, over 4,779 queries (every `.slt` record with a subquery and the review probes), showed
  only SQ5b refusals that changed: 507 now answer, as DuckDB does but for the pinned pairs; 51 are errors, which DuckDB
  reports too but for the D32 pair and the LIMIT record the PR replaced; and 12 refusals are reworded (SQ13's, the
  conjunct that reads an enclosing block, the unnesting's). The generator's `correlated_item_value` passes
  `diff-random`, and 25 metamorphic relations hold the places to LEFT JOINs of the inner groups and to their bindings
  written as literals. Of 27 mutations, 23 fail named tests: the WHERE-only refusal kept, no tail, a tail for
  uncorrelated values, no promotion at either memo hit or by a GROUP BY key, the first, the last or the front entry, a
  promotion while the select list binds or in a dry run, three speculations (as on main, without "else the first", by
  "is the key"), Admit skipped at a memo hit, the aggregate argument's dry-run exception dropped, HAVING's routing
  dropped or taking uncorrelated keys too, the proof INNER at any other node or at a comparison Compute,
  LimitBelowProject stopping at a Compute or crossing a LEFT join, and ExchangeAggregates without its read-above refusal
  (a SUM-valued key next to a computed key aborts). Three survive as the design says they must: no promotion in the
  speculation (a bind it would change fails with SQ13's refusal anyway), the speculation's first entry without the key
  step (two entries need the ONs of two comma groups), and Admit's dry-run guard (no dry run meets a refusal since
  SQ5b). One survives that is ADR 0032's: ExchangeAggregates without its any-above refusal, which the check that every
  Compute on the path reads only the aggregated side backs up over a join back; no test pins it on its own (a
  follow-up).
- **Follow-ups.** A dropped global ORDER BY aggregate's join back is built though nothing reads it; EXPLAIN notes a
  placement over a join back as worth doing ("groupable below the left of join ... worth") where ExchangeAggregates
  then declines it (a COUNT-valued key, or a key computed next to a value key); constant folding in the proof
  (`COUNT(*) + 1 > 1` stays LEFT); a select alias of the enclosing block read inside a correlated subquery, a bind
  error where DuckDB answers, as in any block.

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
