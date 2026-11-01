# 36. UNION ALL: compound queries and an n-ary plan node

Date: 2026-10-30

## Status

Accepted (2026-10-30, with X3). X1, X2 and X3 are built: the plan node and its operator, the grammar, and the
binding, which answers UNION ALL in derived tables, CTE bodies and statements. X4 to X7 stay on the Plan below.

## Context

- **The queries derived from TPC-DS need set operations.** 23 of the 99 use one, counted from the pinned package's
  texts ([ADR 0006](0006-test-strategy-and-data-policy.md); none is quoted here): UNION ALL in 17, UNION in 3,
  INTERSECT in 3 and EXCEPT in 1, one query using two kinds.
  - Every compound is the query of a derived table (19 queries) or of a CTE (4). None is the whole statement, and
    none has ORDER BY or LIMIT on the compound.
  - Every compound has two or three branches, and a branch may GROUP BY (7 queries).
  - Only queries with INTERSECT, EXCEPT or UNION parenthesize a branch (8, 14, 49 and 87). Correction
    (2026-10-31, [ADR 0037](0037-tpcds-roadmap.md)'s survey): only 87 does, all three of its branches. In 8, 14 and 49
    the parenthesis before an operator closes something inside the branch (a condition, a list or a subquery).
  - UNION ALL is the only feature queries 4, 11, 33, 56, 60, 66, 71 and 74 still lack.
- **[ADR 0022](0022-joins-and-query-blocks.md) recorded that the queries derived from TPC-H never need set
  operations, and the engine has none.**
  - The parser refuses `UNION`, `INTERSECT` and `EXCEPT` with exit code 4 (`kUnsupportedClauses`).
  - `sql::SelectStatement` is one block.
  - Every plan node but the two joins has at most one input.
  - The only operator that concatenates is `PartUnionOperator`, and only over the parts of one pipeline.
- **The executor casts only exact widenings** (`plan::CastExpr`, `plan::IsExactWidening`). A union of branches of
  different types is therefore answerable only where each branch widens exactly to the union's type.
- **DuckDB's typing of a set operation is not CASE's** (checked against the pinned 1.5.6):
  - Each column's type is a left fold over the branches in written order. `USMALLINT ∪ SMALLINT ∪ DECIMAL(9,2)` is
    DECIMAL(12,2), but `SMALLINT ∪ DECIMAL(9,2) ∪ USMALLINT` is DECIMAL(9,2).
  - Each branch keeps its own types, a literal's included: `1 ∪ SMALLINT` is INTEGER, and a string literal next to a
    number is VARCHAR.
  - Every type mix binds; a cast that cannot work fails per row at run time.
  - Only a column count that differs is a bind error: "Set operations can only apply to expressions with the same
    number of result columns".
- **Under a LIMIT, DuckDB evaluates at least the first vector of every branch.** An error in a later branch
  therefore surfaces even when the limit is met in the first.

## Decision

1. **The plan node is `UnionAllNode`, and it is N-ary.**
   - **Fields:**
     - `inputs`: two or more, as written.
     - `columns`: per input, one `BoundColumn` per output column.
     - `ids`: per output column. These are new columns: no input defines a union's rows, so none passes through.
     - `span`.
   - **Rows:** every row of input 0 in its order, then input 1's, and so on. Duplicates and NULLs are kept.
   - **Types:** column i has one type in every input. The binder ensures it by casting below the union.
   - **The arity invariant becomes:** a Join and a DependentJoin have two inputs, a UnionAll two or more, every other
     node at most one. Every walk visits every input in order (`InputsOf`, `WithInputs`).
   - **Every exhaustive visitor handles the node:**
     - its name, its inputs and its output ids;
     - the ids it reads and defines (`MaxColumnId`, `FreeVariables`);
     - the position resolver: each input's columns resolve against that input, each position has one type, and the
       union's ids are defined after its inputs;
     - `CloneWithFreshIds`;
     - pruning: the union keeps every output, as a Project does, and asks each input for the columns it maps;
     - `EstimateRows`: the saturated sum of the inputs, unknown where any input's is;
     - the shared sub-plans' origins and keys: keyed by the inputs in order, since their order is the rows' order;
     - EXPLAIN: `UnionAll inputs=n columns=[...]`, with the inputs one level deeper, in order.
   - **Sharing: nothing under a union is ever a member.** A union repeated whole (a CTE referenced twice) is, when
     it has a key, one [ADR 0030](0030-shared-sub-plans.md) group headed by the union, and `WorthSharing` refuses it.
     Below a union the pass neither collects nor reports anything, so a branch is never shared on its own either,
     whatever the copies hold (a node without a key, such as a Sort) and in whatever order they list their branches;
     a group above a union, such as a repeated aggregation of one, is found as before. That is deliberate: an
     aggregate-topped branch held on its own can outgrow `MaterializedTable::kMaxBytes` at large scale factors, and a
     materialization past the cap fails the query rather than declining. X7 revisits this once the cap falls back to
     recomputation.
   - **The semi-join reduction's gate** ([ADR 0023](0023-unnesting.md)) reads "a join below the aggregation" as
     "D5 cannot reach the scan". A union stops the offer too, so the gate now reads a join or a union
     (`ReadsAJoinOrUnion`).
2. **The operator is a serial `UnionAllOperator`.**
   - **Lazy opening.** Input k opens when the union first needs it and closes at its end, before k+1 opens. So:
     - peak memory is the largest branch's, not the sum;
     - a met LIMIT never opens the remaining inputs;
     - ADR 0022's "one part scheduler at a time" holds.
   - **Threads.** Every `Next` runs on the consumer's thread. An input that is a part pipeline runs its own parts, so
     each branch is still parallel inside.
   - **Batches.** Each batch is rebuilt in the union's schema: input 0's names, the plan's types, nullable fields.
     Drain, the sort buffer and a join build compare schemas, so the rebuild is required. The selection is kept and
     no row is copied.
   - **Errors.** The first failing input in order decides the error.
   - **Planning.** The physical planner plans each input on its own. No part pipeline goes through a union, and no
     filter a join offers ([ADR 0029](0029-dynamic-join-filters.md)'s D5) travels into a branch.
   - **Determinism** ([ADR 0013](0013-parallel-execution.md)): rows come in input order, then each input's part
     order, the same on any number of threads.
   - **Errors under a LIMIT:** a branch that is never opened never reports its error. This is divergence D26
     (errors only), recorded with X3, where it becomes reachable.
3. **The AST (X2) is a flat list of branches on `sql::SelectStatement`,** as ADR 0022's FROM list is flat.
   - When the list is set, the block fields are empty, and the statement's WITH, ORDER BY, LIMIT and OFFSET belong
     to the compound.
   - For the depth limit ([ADR 0008](0008-parser-and-unparser.md)), each branch is one level below the compound.
   - The compiler cannot see a walker that ignores the branches. Two checks stand in for it:
     - a field-count tripwire (`kSelectStatementFields`, with a `static_assert` in every walker);
     - a test matrix whose constructs sit only in a last branch.
4. **The grammar and its refusals (X2).**
   - `UNION ALL` joins blocks. A tail of ORDER BY, LIMIT or OFFSET after the last branch belongs to the compound.
   - Until X3, the binder refuses every compound it checks with exit code 4; like DuckDB, it checks no CTE that
     nothing references. It does so before any check that assumes a FROM list; otherwise a compound would reach
     `CheckFromList`'s assertion and abort with exit code 70.
   - Update (2026-10-30, X3): the static check of a compound is now its tail alone (X5), and it still returns
     before `CheckFromList`; a compound as a subquery's query is refused where the subquery conjunct is checked (X6).
   - These stay exit 4 until X4: `UNION` without `ALL`, `BY NAME`, `INTERSECT`, `EXCEPT`, and a parenthesized or
     non-SELECT branch.
   - ORDER BY, LIMIT or OFFSET before a `UNION ALL` is a syntax error, as in DuckDB.
5. **The binding (X3).**
   - **Scope.** A compound's WITH list is visible to every branch. Each branch binds as a block of its own with
     `BindBlock`, from the query's one id source, in the compound's CTE frame and scope. A derived table's branches
     bind against the FROM items before it, so a lateral reference is refused by ADR 0022's rule 9.
   - **Column counts.** A mismatch is a bind error, exit code 1, with DuckDB's message, at the UNION ALL before the
     first branch that differs. Every branch binds first, as in DuckDB (checked: a bind error in a third branch wins
     over a count mismatch at the second).
   - **The type fold** runs left to right in written order over each branch's own types (`UnionAllType`):
     - equal types stay, whatever their kind;
     - integers combine by CASE's integer rule (USMALLINT ∪ SMALLINT is INTEGER), and an integer or DECIMAL with a
       DECIMAL by `CaseDecimal` -- never by the rest of `CaseCommon`, whose DOUBLE results and exit-1 errors are
       CASE's;
     - FLOAT ∪ FLOAT is FLOAT (D11), and FLOAT ∪ DOUBLE is DOUBLE;
     - every other mix exits 4, naming FLOAT as FLOAT, and D11 where a FLOAT meets an integer or a DECIMAL.
   - **Exactness.** Each branch must widen exactly to the result, else exit 4 (for example, a DECIMAL that DuckDB
     would round past 38 digits).
   - **The type refusal** is "UNION ALL of T1 and T2 values is not supported", T2 being the refused item's type, at
     the first select item in the query (the branch, then the column) that fails either check, or at its branch's
     `*`. Update (2026-10-31): at the `*` or `t.*` the column expands from, wherever the star stands among the
     items.
   - **Casts.** A branch whose type differs gets one `Compute` of `WidenTo` casts, straight to the final type.
   - **Names and flags.** Names come from branch 0. `stored_as_float` is the AND over the branches.
   - **Answered:** a compound as a derived table's query, as a CTE's query, or as the whole statement.
   - **Refused with exit code 4:** ORDER BY, LIMIT or OFFSET on a compound (X5), at the first of them; and a compound
     as the query of IN, EXISTS or a scalar subquery (X6), at the subquery's operator once the conjunct's other
     side is checked.
   - **D26** is recorded in docs/sql-subset.md: a LIMIT met without ORDER BY never runs the branches it does not
     need, so their errors are never reported, where DuckDB fails (checked with an overflow in a second branch).

## Consequences

- The eight queries derived from TPC-DS that lacked only UNION ALL pass (X3), which takes the ratchet from 38 to 46.
  Three of them (4, 11 and 74) recompute a union CTE per reference, which nearly doubles the run at scale factor 1:
  under AddressSanitizer one run of the 99 would outlast its timeout, so it runs in four shards of every fourth
  query (tests/tpcds/CMakeLists.txt). X7's filter pushdown into the branches is what would cut that time.
  Update (2026-10-31): eight shards of every eighth query. On CI runners about half as fast as before, the shards
  holding queries 4, 28 and 88 and queries 11 and 23 took 87% and 89% of the timeout once 28, 88 and 23 joined the
  ratchet. In eight, the one holding query 4 still took 79%. So the same day the shards left the PR's ASan leg and the
  nightly ThreadSanitizer run, where one of four had outlasted the timeout; a nightly job runs them under ASan
  ([ADR 0006](0006-test-strategy-and-data-policy.md)).
  Update (2026-11-01, ADR 0037's HC4): the eight shards run the queries of a committed map split by measured cost
  (`tests/tpcds/sf1_shards.json`), where query 4 has a shard to itself.
- Every `std::visit` over the plan grows by one alternative. A walk that compiles for any node but assumes a shape
  must be read whenever a node is added ([architecture](../architecture.md), "Where to add things").
- A union's consumer is serial. A large union under a GROUP BY, a join or a sort runs at the consumer's
  single-thread rate; X7's part pipeline over all branches lifts that.
- No rule moves anything through a union yet. A filter above a union runs above it, and a CTE whose body is a union
  is computed once per reference. X7's filter pushdown, with constant-branch pruning, is what queries 4, 11 and 74
  want at large scale.

## Plan

| Id | Item | Status |
| --- | --- | --- |
| X1 | The plan node and `UnionAllOperator`; every visitor; the reduction's gate. No SQL. | Built |
| X2 | `UNION ALL` in the grammar, the AST, the unparser and the walkers, refused by the binder with exit code 4; the random generator writes it (a pending feature) | Built |
| X3 | The binder answers it in derived tables, CTE bodies and statements; `.slt` from DuckDB; the TPC-DS ratchet; D26 | Built |
| X4 | `UNION` [DISTINCT], `INTERSECT` and `EXCEPT` as UNION ALL plus a branch marker plus a GROUP BY over every column (a `GroupTable` treats NULLs as equal); parenthesized branches and INTERSECT's precedence | Trigger: X3 merged; queries 8, 14, 36, 38, 49, 75 and 87. Update (2026-10-31): [ADR 0037](0037-tpcds-roadmap.md) schedules these as X4a-X4d in its phase 6; `BY NAME` and non-SELECT branches stay on X4, unscheduled, and keep their `pending X4` guards |
| X5 | ORDER BY, LIMIT and OFFSET on a compound, with the harness's query rewrites | Trigger: a query that needs it |
| X6 | A compound as the query of IN, EXISTS or a scalar subquery | Trigger: a query that needs it |
| X7 | Performance: filters into branches with constant-branch pruning; one part pipeline over every branch; dropping a union's unread columns (and a derived table's); sharing union-topped sub-plans once the cap falls back to recomputation | Trigger: a profile at a large scale factor. Update (2026-10-31): [ADR 0037](0037-tpcds-roadmap.md) schedules the filters and pruning as X7a and the sharing as its guard PF5 |

## Alternatives considered

- **Binary nested unions.** They keep the two-input bound and match SQL's binary operators. Rejected:
  - k − 1 nodes and k − 1 AST levels, so a 256-branch union meets the depth limit;
  - a cast between levels wherever the fold changes a type;
  - an ADR 0030 key that depends on the nesting;
  - and X7's part pipeline needs a flat list again.
- **A variant holder** (`std::variant<Block, Compound>` in the four places a query is held). It would give the
  compiler's exhaustiveness to the AST walkers. Rejected for now:
  - it rewrites about 55 sites;
  - it splits WITH, ORDER BY and LIMIT, which belong to both shapes;
  - the tripwire gives the same signal at a fraction of the churn.

  Revisit when a third shape arrives (a VALUES list, [ADR 0025](0025-values-table-constructor.md)).
- **A join kind, or a multi-input Project, as the union.** A join's keys, residual and build side mean nothing here,
  and a Project's single input is what every walk over it assumes.
- **Running branches in parallel, each with its own part scheduler at once.** Rejected for now:
  - each branch already runs its parts on the pool;
  - running several at once keeps their builds and windows alive together under the memory budget;
  - it breaks ADR 0022's "one scheduler at a time";
  - a worker that waits on pool tasks can deadlock the pool (ADR 0022).

  X7's part pipeline over every branch is the parallel consumer instead.
- **Refusing top-level compounds.** That would need a refusal path, its docs and its tests, where answering one is
  the same binding (X3).

This workload is derived from the TPC-DS Benchmark and is not comparable to published TPC-DS Benchmark results, as
this implementation does not comply with all requirements of the TPC-DS Benchmark.
