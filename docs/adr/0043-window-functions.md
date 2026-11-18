# 43. Window functions: phase 9 and the remaining standard SQL syntax

Date: 2026-11-18

## Status

Proposed (2026-11-18). A design ADR requested ahead of the code, under the ADR README's exception. It owns
[ADR 0037](0037-tpcds-roadmap.md)'s phase 9 and the window work beyond the queries derived from TPC-DS. It lands
as a separate documentation commit and PR. Implementation starts only after that PR merges and phases 0 to 8
are complete; SQ8c completed phase 8 on 2026-11-18. This record changes no SQL support or ratchet.

There are two completion milestones: W1-W4 answer all 99 workload queries; W5-W13 complete the window feature
inventory below, subject to its explicit general SQL prerequisites. Passing 99 queries does not mean full window
support. This ADR's status and the closing items CL1/CL2 are not changed automatically at either milestone.

## Context

- The ratchet is at 84 of 99. The remaining queries are Q12, Q20, Q36, Q44, Q47, Q49, Q51, Q53, Q57, Q63,
  Q67, Q70, Q86, Q89 and Q98. Their requirements were checked against the pinned workload; no query text,
  fragments, data or answers are reproduced here ([ADR 0006](0006-test-strategy-and-data-policy.md)).
- Eight need whole-partition aggregates: SUM in Q12, Q20 and Q98; AVG in Q47, Q53, Q57, Q63 and Q89. Each
  consumes an ordinary aggregate of its query block. Eight need RANK: Q36, Q44, Q47, Q49, Q57, Q67, Q70 and
  Q86. Q47 and Q57 belong to both groups. Q51 needs running SUM and MAX in successive query blocks, with
  explicit ROWS frames from the partition start through the current row.
- CASE expressions, grouped aggregates and GROUPING() occur in partition or ordering expressions. A design
  restricted to bare columns or to windows over ungrouped input would not complete the workload.
- `kWindowFunctions` is the harness's permanently unsupported canary. The marker, redaction query, pending
  self-tests and metamorphic relation must move together before a window query can succeed.
- OJ5 supplies NULL-equal join keys. ADR 0037 therefore left whole-partition aggregation joined back to its input
  as an option. But joins still exclude DOUBLE and BOOLEAN keys, and shared sub-plans neither cover every window
  input nor provide an unrestricted spool: `MaterializedTable` has a 256 MiB cap, and sharing excludes important
  shapes, including grouping sets as the reducing root. Repeating an input can repeat expensive joins and grouping.
- `GroupTable::Consume` already exposes each row's group ID. Grouping treats NULLs as equal and normalizes DOUBLE
  NaNs and signed zero; its states already implement the aggregate result types and arithmetic. This permits
  whole-partition results to be attached to input rows without a relational join.

## Decision

### Scope, compatibility and scheduling

1. **General capabilities in two lists.** W1-W4 are phase 9, in that order. They are categories of SQL, not rules
   for particular queries. W5-W13 are an independently scheduled backlog, with no additional workload completions.
   They need not all precede phase 10. A dependency needed there is pulled forward: SQ14 needs W5's ROW_NUMBER,
   even though none of the 15 queries does.
2. **The completeness target is standard SQL windows.** The inventory uses SQL:2023's window features, including
   the advanced features below. It records missing general SQL facilities as prerequisites; it does not imply that
   windows introduce every SQL type, aggregate, parameter or user-defined routine. DuckDB-specific extensions have
   a separate inventory. Neither PostgreSQL's nor DuckDB's implemented subset defines the whole target.
3. **The existing semantic contract still applies.** The pinned DuckDB 2.0.0 oracle remains the reference for
   common supported forms, types, NULL handling and errors ([ADR 0004](0004-types-null-overflow-semantics.md)).
   A standard spelling or behavior it cannot execute needs an explicit reference or equivalence strategy before
   its implementation; any intentional semantic difference is documented in the SQL subset's divergence table.
4. **Refusals stay specific.** A valid deferred form exits 4 with a source span and its owning W/SQ item. Malformed
   syntax and invalid uses of an implemented form are syntax/bind errors, exit 1. Internal errors retain exit 70.
   Enabling one window Feature never declares every window form supported. W1 parses the conventional window
   structure and identifies deferred constructs; W12/W13's detailed grammars belong to their later designs.

### Frontend, binding and logical interface

1. **A window call is a distinct expression.** The SQL AST keeps the function, arguments, modifiers, window
   specification and spans separately from an ordinary aggregate. A specification represents partition expressions,
   ordering keys and an explicit frame, or a named reference. Frame representation distinguishes unit, bounds and
   exclusion; normalization never loses whether the source omitted a frame. The AST, equality and unparser support
   the same forms. `common` and `sql` remain Arrow-free.
2. **Ordinary aggregates inside a window belong to its block.** Discover them in arguments, partition expressions
   and ordering expressions. The enclosing window aggregate does not itself turn a raw-row block into a grouped
   block. Nested ordinary aggregates, ordinary window calls inside window calls, and the special standard nested
   constructs of W12 remain distinct cases. A window in WHERE, ON, GROUP BY or HAVING is not made legal by W1.
3. **A separate binding stage follows HAVING.** Compute the window's arguments and keys from the surviving rows or
   groups, then append window results, then compute expressions consuming them. ROLLUP output NULLs and GROUPING()
   are the actual values at this stage. Support hidden windows used only by final ORDER BY, repeated calls,
   windows in larger expressions, multiple specifications, derived tables, CTEs and set-operation branches.
   DISTINCT, final ORDER BY and LIMIT/OFFSET remain above the window stage. A window's ordering does not promise
   the final row order.
4. **`WindowNode` preserves input cardinality and appends result columns.** Its bound calls carry argument columns,
   partition keys, ordering, normalized frames and result types. Update every exhaustive plan visitor: column
   remapping/pruning, estimates, EXPLAIN, sharing, optimizer rewrites, physical planning and unnesting. Preserve all
   argument/key dependencies. Initially it is a barrier to filter, top-N and aggregate-placement rewrites unless a
   dedicated rule proves equivalence.
5. **Subquery stages are explicit.** Uncorrelated values in currently supported compositions keep their evaluation
   and error rules; expressions consuming windows get the post-window stage reserved by ADR 0041's decision 9.
   Extending legal operand positions belongs to W11f. Correlated values and window-containing correlated blocks
   need W11g's proofs and the relevant phase 10 rules; unnesting must not treat a window as a transparent unary
   node. QUALIFY, mentioned as a future consumer in ADR 0041, belongs to WX1 below, not W1-W4.

### Execution

1. **One dedicated window operator, one upstream execution.** Retain selected input rows in chunked batches with
   stable row identities, keeping only columns needed above the stage or by its calls. All specifications use that
   input; they do not rerun upstream joins, aggregates or expressions. Start with serial window evaluation over
   the gathered upstream batches; existing upstream parallel work may still run.
2. **Whole partitions use grouping states.** For compatible calls, feed batches to `GroupTable` while recording each
   row's group ID; representative key copies are unnecessary. Finalize each partition's aggregate once and gather
   its value by those IDs into output batches. Without partition keys there is one partition. Empty input emits
   zero rows, including an unpartitioned window: it must not manufacture an ordinary global aggregate's row.
3. **Ordered calls share a layout.** Group compatible partition/order specifications, build a stable permutation of
   row references and compute partition and peer boundaries. Reuse the existing sort comparison semantics for
   supported types: NULL placement, VARCHAR byte order, equal NaNs and equal signed zeros. A tie-breaking row
   identity used for stable traversal must not split peers. Different frames may share the layout. Scatter results
   back by row identity so specifications with different orders remain aligned and duplicate rows remain distinct.
4. **RANK counts rows before a peer group.** Tied rows get the same BIGINT rank; the next rank includes the size of
   the tie. Boundaries survive batch changes. With no ordering all rows in a partition are peers and rank 1, as in
   DuckDB. Ranking uses partition/order semantics, not an aggregate's default frame calculation.
5. **W4 uses batch prefix kernels.** Carry aggregate state across batches and reset it at each partition. Reuse
   existing arithmetic and finalization rules through typed loops, not a virtual Consume/Finalize call or array
   allocation for each output row. Integer SUM and AVG accumulate in Int128; output types, overflow checks and
   all-NULL behavior follow the corresponding aggregate. AVG uses its own finalizer: a valid average must not
   fail because the intermediate sum could not be returned as a 38-digit HUGEINT SUM. Validate the blocking
   stage's results before emitting its first batch. Sliding frames later require removal, range-summary or other
   appropriate state machinery;
   the current append-only states do not suffice by themselves.
6. **Keep frame categories separate.** W2 answers omitted frames without window ORDER BY. W4 answers explicit
   `ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW`, including its equivalent single-bound spelling, with or
   without ordering. An ordered aggregate with an omitted frame requires W6's peer-inclusive RANGE and exits 4
   until then; it must never silently become a ROWS prefix. Explicit whole-partition frames, with or without
   ordering, belong to W8a/W8b/W8c. W3's ordinary ordered RANK is not refused for lacking an explicit frame.
7. **Budget every owned buffer.** Retained input, group IDs, permutations, peer metadata, states, result arrays and
   sorting scratch all use the query pool or charged reservations. Finalize and gather variable-length results in
   chunks, with checked row/index widths. Release each layout's temporary state after storing its results. Every
   failure, cancellation and Close releases ownership; reopening starts cleanly. Initially nothing spills, and
   exceeding the query budget returns the existing memory-limit error, without a separate 256 MiB spool limit.

### Phase 9 plan: the features needed for 99 queries

Every row is planned. The query lists are completions after preceding rows have landed.

| Id | Item | Completes | Ratchet after item |
| --- | --- | --- | --- |
| W1 | Distinct window AST, parser/unparser, binding stages, future-form refusals and replacement of the permanent canary; no answered window SQL | | 84 |
| W2 | WindowNode and the dedicated operator; whole-partition windows for the existing non-DISTINCT aggregate family over raw rows or grouped results | 12, 20, 53, 63, 89, 98 | 90 |
| W3 | RANK, including computed partition/order keys, GROUPING() and grouped aggregates, multiple specifications and peer groups | 36, 44, 47, 49, 57, 67, 70, 86 | 98 |
| W4 | Running ROWS windows for the existing non-DISTINCT aggregate family, from unbounded preceding through current row, including windows in successive query blocks | 51 | 99 |

W2 and W4 cover existing aggregates and their currently supported argument types, not just the SUM, AVG and MAX
needed by the workload. New operand/type capabilities are W11. DISTINCT and FILTER remain W10. W1 may introduce
the logical interface, but W2 supplies execution and answers the first window SQL.

### Beyond TPC-DS: the standard window backlog

Every item below is planned, completes none of the 99, and is owned by this ADR. IDs describe capabilities and
dependencies, not a commitment to put all of them before phase 10. Each implementation updates its own feature
coverage and SQL contract. A parent such as W8 is complete only when every listed child is complete.

| Id | Capability and acceptance boundary | Dependencies |
| --- | --- | --- |
| W5 | ROW_NUMBER, DENSE_RANK, PERCENT_RANK, CUME_DIST and NTILE; bucket arguments, ties, singleton partitions and partitions smaller than the bucket count | W3; new argument-expression forms use W11e; ROW_NUMBER is required by SQ14 |
| W6 | Implicit ordered RANGE and explicit unbounded-preceding/current-row peer prefixes; peer groups may span batches, and without ordering all rows are peers | W3, W4; no temporal offset dependency |
| W7 | Named WINDOW definitions, direct references and inherited specifications; chains, local name scope, quoted identifiers, duplicate names and invalid overrides; distinguish OVER name from OVER (name ...) | W1; frame execution follows the corresponding W item |
| W8a | General ROWS: every legal bound direction, single-bound and BETWEEN forms, explicit full-partition frames with or without ordering, bounded sliding and suffix frames, empty frames and clipping at partition ends | W4; bound-expression extensions use W11e |
| W8b | General RANGE: all legal bounds, explicit full partitions and numeric/temporal distances; ordering direction, peers, ordering-key restrictions, offset typing and checked boundary arithmetic | W6; temporal offsets need W11c; expressions need W11e |
| W8c | General GROUPS: every legal bound and shorthand, offsets measured in peer groups, full partitions and empty/clipped frames | W3, W6; bound-expression extensions use W11e |
| W8d | EXCLUDE CURRENT ROW, GROUP, TIES and NO OTHERS; interactions with every applicable frame/function, including a frame that already omits the current row | The applicable W8 frame and W9 function |
| W9a | LEAD and LAG: value, optional offset and default, common typing, omitted arguments and out-of-partition results; navigate the partition rather than accidentally restricting navigation to an aggregate frame | W3; added operand forms use W11a/W11e/W11f |
| W9b | FIRST_VALUE, LAST_VALUE and NTH_VALUE: frame-sensitive selection, index validation, empty frames and an index beyond the frame | W8 for general frames; W11 for added operand types/forms |
| W9c | Explicit RESPECT NULLS and IGNORE NULLS for all applicable W9 functions, including all-NULL frames/partitions and interaction with offset counting | W9a, W9b; standard spelling and oracle coverage checked separately |
| W9d | NTH_VALUE FROM FIRST and FROM LAST, with null treatment and frame/exclusion interactions | W9b, W9c; reference strategy if the pinned oracle lacks the form |
| W10a | ALL and DISTINCT for every eligible aggregate; whole, ordered, moving and excluded frames, including SQL:2023 T627 framed COUNT DISTINCT; duplicate removal and NULL behavior remain correct when rows leave a frame | Corresponding W2/W4/W6/W8 frame; shared aggregate grammar and state changes |
| W10b | FILTER for eligible window aggregates and their ALL/DISTINCT forms, with NULL predicates, empty filtered frames and exclusions | General aggregate FILTER grammar/binding is owned here; W10a for DISTINCT; correlated predicates use SQ17/W11g |
| W12 | SQL feature T619: special nested row-number and value-access constructs, row markers, offsets/defaults and legality; not arbitrary recursively nested OVER calls | Follow-up design and reference strategy before implementation; W8 and W11 as required by that design |
| W13 | R020 window row-pattern recognition and applicable R030 aggregate support: MEASURES, PATTERN, DEFINE, SUBSET, AFTER MATCH SKIP, INITIAL/SEEK, pattern navigation/classification and aggregate expressions; empty/unmatched/skipped rows and ordinary windows over matches | Follow-up design and reference strategy before implementation; W8 and W11 as required by that design |

W12 and W13 remain explicit obligations in the completeness inventory even when their implementation designs are
deferred. A later ADR supplies their algorithms and detailed standard grammar and links back here. Supporting
only the forms DuckDB answers is not enough to close these items.

### W11: general SQL prerequisites and integration

These are part of this ADR's backlog, not unnamed work hidden behind "full syntax". They complete no workload
query. A dependency already owned by another ADR keeps that owner; W11 records the window integration and tests.
Each new type or aggregate family still needs its own general SQL contract outside windows.

| Id | Required capability | Ownership and boundary |
| --- | --- | --- |
| W11a | General NULL values and contextual typing; constant aggregate arguments, including COUNT of a constant, NULL arguments and a scalar subquery as the whole argument; explicit navigation defaults | Extends the existing literal/aggregate contract. ADR 0041's unscheduled constant-argument prerequisite is assigned here; subqueries retain their cardinality/error rules |
| W11b | BOOLEAN values, ordering and expression keys; consistent partition equality and peer comparison for every admitted scalar type, with NULL, NaN and signed zero handled consistently | Existing sort code excludes BOOLEAN. DOUBLE partitioning must work without depending on a DOUBLE hash-join key. Full Boolean scalar support remains a general capability, not a window-only exception |
| W11c | INTERVAL types/literals, compatible DATE/TIMESTAMP distance arithmetic, temporal RANGE boundaries and overflow behavior | General type/arithmetic work precedes the affected W8b forms; reading additional temporal storage types is a separate I/O prerequisite where needed |
| W11d | Collation-aware character partition equality, hashing, ordering and peers | Requires a shared collation design; current bytewise VARCHAR semantics remain the implemented contract until then |
| W11e | Standard-permitted value expressions and parameters in partition/order specifications, frame bounds, NTILE bucket counts, navigation offsets/defaults and NTH_VALUE indices | Separate expression rules from final ORDER BY aliases/ordinals; general parameter support is a prerequisite, and broader dialect-only expression forms are not implied |
| W11f | Uncorrelated subqueries in legal window operands/specifications and expressions consuming window results | Bind each in its proper pre/post-window stage; retain eager evaluation and cardinality errors. Standard placement restrictions remain enforced |
| W11g | Correlated window operands and FILTER predicates, grouped outer references, and window-containing correlated blocks | SQ13 owns remapping above grouping; SQ17 owns correlated FILTER/window arguments; SQ9/SQ10 and the unnesting rule table own their remaining shapes. W11g records their integration obligations, not an extra prerequisite to SQ17 itself. Add explicit window rules and consumer proofs; SQ14 consumes W5's partitioned ROW_NUMBER |
| W11h | Integration of additional standard aggregate families and scalar/collection types, including ordered aggregate and ordered-set forms where the standard permits window use | Maintain a capability matrix for each added general aggregate/type; this ADR does not create an unbounded aggregate catalog. Check standard applicability before classifying a form as an extension: vendor support or refusal alone is not proof |

The matrix distinguishes syntax, binding, execution and reference-test support. It names any external prerequisite
for a deferred combination, so "W8 is implemented" cannot hide missing temporal RANGE and "W10 is implemented"
cannot hide framed DISTINCT. Missing general types, aggregate families or user-defined aggregate facilities remain
explicit boundaries of the completion claim, not silently implemented by accepting OVER.

### Optional dialect extensions

These are deferred and not required for the standard window milestone. They stay visible here rather than being
mixed with the work needed for 99 queries.

| Id | Extension | Dependency |
| --- | --- | --- |
| WX1 | QUALIFY, aliases/window calls in its condition and its subquery evaluation stage | W1's post-window stage and the applicable window/subquery capabilities; define its order relative to DISTINCT and the final tail |
| WX2 | DuckDB's function-argument ORDER BY extensions, separate from the window order and standard ordered-aggregate forms | The applicable function/frame; separate evaluation, ordering and peer semantics |
| WX3 | fill and its interpolation/extrapolation behavior | Ordered partitions, compatible numeric/temporal arithmetic and an explicit function contract |
| WX4 | DuckDB aliases, alternative modifier spellings and permissive operand forms beyond the selected standard grammar | The corresponding standard capability; enumerate accepted forms and differences in the implementing PR |

### Performance follow-ups and budget guards

These are conditional implementation work, not syntax milestones. Do not claim a benchmark win for the selected
architecture before measuring it.

| Id | Work and trigger |
| --- | --- |
| WP1 | Parallel execution of independent partitions when serial window work prevents a feature from meeting the SF 1 time budget; preserve deterministic peer and error rules |
| WP2 | Reduce retained columns/lifetimes and, where needed, design spilling or bounded partition processing when the retained input/state threatens the memory budget; spilling has no implementation in W1-W4 |
| WP3 | Stream eligible already ordered or order-free windows when measured blocking/buffering cost warrants it; prove ordering and frame requirements first |
| WP4 | Share compatible sort prefixes/layouts more broadly, and consider aggregate-plus-join-back as a costed rewrite when types, evaluation counts and memory behavior are proved equivalent |
| WP5 | Safe filter, top-N or aggregate-placement rules across windows when a profile justifies them; whole-partition removal and rank limits require specific proofs |

ADR 0037's gates remain: every newly answered query stays below about 1.5 GiB peak and every SF 1 shard below
about 60% of its inner timeout, within the existing 2 GiB query cap. If a feature misses a gate, the guard needed to
fix its cause lands first. No query is skipped and no timeout, memory limit or gate is relaxed. Each completing PR
records the prescribed CI-leg costs and disclaimer; heavy-query validation includes the Linux ASan SF 1 run.

## Tests and the canary

- **W1 moves the permanent canary to `kRecursiveCte` / `recursive_cte`.** Use an actually recursive, bounded
  query over a synthetic fixture: seed it from a bounded fixture count and decrease the value until it stops.
  Do not use an independently unsupported SELECT-without-FROM anchor. Verify it with the oracle. Migrate the
  Feature enum/count/name and never-generated set, redacted canary query, both pending SLT self-test records
  (including the missing-table case), and pending metamorphic relation together. WITH RECURSIVE is currently
  refused before name resolution, which preserves that self-test. Future recursion work must replace the canary
  before enabling it.
- **Per item:** parser/unparser round trips and spans; syntax/bind/unsupported distinctions; alias and named-window
  scope; ordinary aggregates feeding windows; illegal placements; output types; EXPLAIN and every plan visitor.
  Each supported slice gets its own Feature and independent random stream, or a named generator-pending owner.
- **Results:** DuckDB-completed SLTs with result type names, own synthetic fixtures, and metamorphic checks:
  whole-partition aggregates agree with an independently grouped result; equivalent explicit/default frames agree;
  named and inline specifications agree; adding an independent window preserves existing columns. Use NULL-equal
  join-back as a reference relation only for key types supported by the reference expression.
- **Execution boundaries:** empty input, singleton/all-NULL partitions, duplicate rows, rolled-up NULLs, GROUPING(),
  computed keys, multiple incompatible specifications, hidden ORDER BY windows, selection masks, peers crossing
  batches, NULL placement, NaNs, signed zero, prefix resets, overflow and variable-length results. Cover repeated
  Open/Close, failures, cancellation and memory-budget cleanup. Test with existing serial/parallel harnesses;
  tied ROWS/navigation results need a total order or a comparison accepting SQL's unspecified tie order.
- **Backlog combinations:** all boundary/exclusion forms; empty/clipped frames; DISTINCT state as rows enter and
  leave; FILTER plus DISTINCT/exclusion; navigation offsets/default typing, null treatment and FROM LAST;
  named-window inheritance; temporal/month boundaries; collation-consistent peers; correlated versus uncorrelated
  placement. A valid feature combination cannot disappear between the owners of its parts.
- **Reference gaps:** W9d, W12, W13 and any other form the pinned DuckDB cannot execute need a documented reference
  strategy before implementation. An equivalent supported query may supply oracle expectations only after its
  equivalence is justified. Otherwise use an independently validated model or separately approved reference
  infrastructure. Never hand-write SLT expectations, silently change their oracle or add test network access.
- **Workload acceptance:** existing redacted serial/parallel ratchets and SF 1 shards, with only query IDs and cost
  diagnostics published. Canary migration and runtime changes happen in implementation PRs, not this ADR commit.
  Run `pixi run check` before each PR; operator memory/ownership work also requires `pixi run check-full`.

## Consequences

- Phase 9 has three answer-changing steps and reaches 99 without waiting for every window feature. The full
  standard backlog remains in this ADR and independently testable after that point.
- Whole-partition execution reuses aggregate algorithms while consuming arbitrary upstream shapes once. It does
  retain the window input, which can cost more memory than recomputation; that tradeoff is measured under the
  existing budget rather than hidden behind a sharing assumption.
- A dedicated logical boundary makes unsafe optimizer and decorrelation traversal visible. It adds work to every
  plan visitor, but future frame, navigation and pattern features have an explicit home.
- The canary continues to test a real unsupported capability after windows start passing. Its migration cannot
  weaken pending/redaction checks or mark the whole window grammar supported at once.
- The ADR's merge authorizes starting its implementation sequence, not merging future code without tests and
  review. This documentation change leaves existing ADR statuses and ratchets unchanged.

## Alternatives considered

- **Whole-partition aggregation plus a relational join-back as the default.** OJ5 makes it possible for supported
  key types, and it remains WP4's optimization candidate. It is not the baseline: DOUBLE/BOOLEAN join-key limits,
  duplicate upstream execution and incomplete shared-subplan support would add unrelated prerequisites or reject
  otherwise valid partitioning. Reusing GroupTable inside the operator retains the existing aggregate algorithms.
- **A sorted implementation for every whole-partition aggregate.** Unnecessary ordering work where hashing and
  row-to-group IDs suffice. Keep sorting for functions and frames that need an order.
- **Answer only the functions and expression shapes present in the 15 queries.** Rejected: W2/W4 cover the existing
  non-DISTINCT aggregate family, and all core forms compose with grouped and ungrouped inputs.
- **Complete all standard windows before phase 10.** Rejected by the maintainer. Keep the backlog independently
  scheduled, pulling forward the capabilities its consumers require.
- **Treat DuckDB's window list as the whole standard.** Rejected. T619, row-pattern recognition and missing standard
  spellings remain recorded obligations; dialect extensions do not replace them.

## References

- [SQL:2023, Part 2: Foundation](https://www.iso.org/standard/76584.html) and the
  [ISO OLAP guide](https://www.iso.org/standard/83319.html): the standard family and OLAP feature inventory.
- [PostgreSQL window syntax](https://www.postgresql.org/docs/18/sql-expressions.html#SYNTAX-WINDOW-FUNCTIONS) and
  [window functions](https://www.postgresql.org/docs/18/functions-window.html): conventional forms and standard
  modifiers that are not all implemented there.
- PostgreSQL's SQL:2023 [supported](https://www.postgresql.org/docs/18/features-sql-standard.html) and
  [unsupported](https://www.postgresql.org/docs/18/unsupported-features-sql-standard.html) feature inventories:
  T627, T619 and R020/R030 remain distinguishable capabilities.
- [DuckDB 2.0 window functions](https://duckdb.org/docs/2.0/sql/functions/window_functions): the pinned oracle's
  public behavior, defaults and extensions, subject to verification against the installed version.
- [Trino window row-pattern recognition](https://trino.io/docs/current/sql/pattern-recognition-in-window.html):
  a concrete implementation reference for W13, not a substitute for its standard grammar or future design.

This workload is derived from the TPC-DS Benchmark and is not comparable to published TPC-DS Benchmark results, as
this implementation does not comply with all requirements of the TPC-DS Benchmark.
