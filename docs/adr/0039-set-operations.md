# 39. Set operations: UNION, INTERSECT and EXCEPT in one n-ary node

Date: 2026-11-06

## Status

Proposed (2026-11-06). A design ADR the maintainer asked for ahead of the code (the ADR README's exception). It
replaces [ADR 0036](0036-union-all.md)'s X4 sketch for UNION [DISTINCT], INTERSECT and EXCEPT; UNION ALL stays ADR
0036's. Its items are [ADR 0037](0037-tpcds-roadmap.md)'s phase 6: X4a (the grammar), X4s (the plan node and a serial
operator), X4b (UNION), X4c (INTERSECT [ALL]) and X4d (EXCEPT [ALL]), with X4p and PF12 to PF15 as guards. It is
accepted when X4d is built.

## Context

- **ADR 0037's phase 6 answers set operations.** ADR 0036 sketched them as a UNION ALL with a marker per branch under
  a GROUP BY of every column. `BY NAME` and non-SELECT branches stay on ADR 0036's X4: no query of the 99 needs them.
- **The queries derived from TPC-DS.** Counted from the pinned package's texts, none quoted
  ([ADR 0006](0006-test-strategy-and-data-policy.md)):
  - 23 of the 99 use a set operation: UNION ALL 17, UNION 3 (Q36, Q49 and Q75), INTERSECT 3 (Q8, Q14 and Q38) and
    EXCEPT 1 (Q87). Q14 counts twice: it has an INTERSECT and UNION ALLs, in separate compounds.
  - No compound mixes two operators, none writes ALL with INTERSECT or EXCEPT, and no parenthesized branch has a WITH
    or a tail of its own. Only Q87 parenthesizes its branches.
  - Phase 6 completes Q75, Q8, Q38 and Q87. Q14 also needs SQ3, and Q36 and Q49 need window functions (phase 9).
- **DuckDB 1.5.6**, checked on synthetic data:
  - INTERSECT binds tighter than UNION and EXCEPT, which share a level and associate to the left. Each operator has
    its own quantifier, DISTINCT by default.
  - Rows compare as its GROUP BY compares keys: NULL equals NULL column by column, -0.0 equals 0.0, and every NaN
    equals every other.
  - It plans UNION as a union under a hash aggregate. INTERSECT and EXCEPT are semi and anti hash joins on
    `IS NOT DISTINCT FROM` over every column, always building the right operand, with two deduplications per
    operator. The ALL forms put a ROW_NUMBER window on each side. Only runs of UNION, or of UNION ALL, become one node.
  - It types a compound along its tree, not in written order (the Types rule below).
  - Its row order is unspecified. Which spelling of -0.0 or NaN a DISTINCT form prints depends on its plan and threads,
    and can change between runs even at one thread. INTERSECT ALL and EXCEPT ALL print the normalized value wherever
    its plan keeps the operator; UNION ALL prints each row's own.
- **antb1 today:**
  - `GroupTable` compares keys exactly as set operations compare rows: NULL is a key value, DOUBLE keys are normalized
    (`double_key.h`), and each group keeps the key it saw first.
  - Its hash joins never match a NULL key and refuse DOUBLE and BOOLEAN keys. NULL-equal keys are OJ5, in phase 7.
  - `UnionAllOperator` is serial, and no part pipeline goes through a union, so a GROUP BY over a UNION ALL runs on
    the consumer's thread.
  - Only an aggregate-topped result is shared ([ADR 0030](0030-shared-sub-plans.md)), and nothing under a union
    (ADR 0036). ADR 0036's sketch tops every UNION with a GROUP BY, which is why ADR 0037 put PF5's fallback in
    front of X4b.
- **The maintainer's direction.** UNION ALL stays out of the general design: it is a pass-through case, with its own
  node and operator, optimized on its own. Fusing it with the other operators into one node is at most an option.
- **A review on 2026-11-06** compared three designs: ADR 0036's sketch, DuckDB's joins, and a node of its own. The
  Alternatives give the reasons for the choice.

## Decision

### The forms and their meaning (DuckDB's)

1. **Grammar.**

   ```text
   query    := [WITH cte (',' cte)*] compound [tail]
   compound := primary ((UNION | INTERSECT | EXCEPT) [ALL | DISTINCT] primary)*
   primary  := block | '(' query ')'
   ```

   - A tail is a block's: ORDER BY, then LIMIT and OFFSET in either order.
   - A parenthesized query may hold its own WITH, compound or tail. A parenthesized block without them is a plain
     branch, and `((q))` is `q`.
   - These are syntax errors, as in DuckDB: an operator after a tail outside parentheses, `INTERSECT` or `EXCEPT`
     [ALL] `BY NAME`, `UNION ()`, an operator followed by `WITH` without parentheses, and ALL with DISTINCT.
     `SELECT * EXCEPT (c)` stays a syntax error (DuckDB spells it `EXCLUDE`), and `MINUS` stays a name.
2. **The tree.** INTERSECT [ALL] binds tighter; UNION and EXCEPT share a level and associate to the left; parentheses
   nest. `A UNION B INTERSECT C` is `A UNION (B INTERSECT C)`, and `A EXCEPT B UNION C` is `(A EXCEPT B) UNION C`.
   - The tree's **leaves** are blocks and parenthesized queries with their own WITH or tail.
   - A **run** is a chain of one operator with one quantifier in the tree, its operands in written order:
     `A EXCEPT B UNION C UNION D` is a run of UNION over `A EXCEPT B`, C and D.
3. **Rows.**
   - UNION gives each distinct row of either operand, INTERSECT each distinct row of the left operand that the right
     one holds, and EXCEPT each distinct row of the left operand that the right one lacks. The DISTINCT forms remove
     the left operand's duplicates even where nothing matches: `A EXCEPT B` over an empty B is `DISTINCT A`.
   - INTERSECT ALL gives min(m, n) copies of a row that the left operand holds m times and the right n times, and
     EXCEPT ALL gives max(m − n, 0).
   - Rows compare as GROUP BY keys: NULL equals NULL column by column, -0.0 equals 0.0, and every NaN equals every
     other. Next to FLOAT or DOUBLE values, a FLOAT compares through its exact widening to DOUBLE. DuckDB compares a
     FLOAT with integer or DECIMAL values in FLOAT, a mix antb1 keeps refusing (D11).
   - Row order is unspecified.
4. **Types.** Each column's type is a fold along the tree, with `UnionAllType` and NL1's `CommonValueType` for each
   pair:
   - A run of UNION, or of UNION ALL, folds as one list in written order. A run of the same operator and quantifier
     nested in it is spliced into the list, across parentheses too, unless it has its own WITH or tail. A compound in
     a CTE body or a derived table is never spliced: the block that reads it is a leaf.
   - Everything else folds as the tree's binary nodes: INTERSECT, EXCEPT, and a run of UNION next to one of UNION ALL.
     `SMALLINT UNION ALL USMALLINT INTERSECT DECIMAL(9,2)` and `USMALLINT INTERSECT (SMALLINT INTERSECT DECIMAL(9,2))`
     are DECIMAL(9,2), where written order would give DECIMAL(12,2).
   - Every edge of the tree must widen exactly, or the query exits with code 4 and a message naming the operator. Each
     leaf is then cast once, straight to the root's type: exact widenings compose and keep equality, so an inner node
     has the same rows in the root's type.
   - A NULL item is the fold's identity (ADR 0037's NL3), and a nested compound's column of NULL items reaches its
     parent as one. A root column that is NULL in every leaf stays exit code 4.

   ADR 0036's left fold in written order gives the same types wherever every operand after an operator's first, all
   through the tree, is a block, parenthesized or not, or a spliced run, as in every compound ADR 0036 answers.
5. **Names** come from leaf 0, and `stored_as_float` is the AND over the leaves.
6. **Column counts.** A mismatch is DuckDB's bind error, exit code 1, at the operator, in DuckDB's order: a spliced run
   binds every leaf before it checks, and a binary node checks right after its two operands. Type refusals come after
   binding.
7. **Where a compound stands:** a statement, a CTE body or a derived table, as UNION ALL does today. Refused with exit
   code 4:
   - the tail of an outermost compound that has an operator (X5);
   - a second tail after a parenthesized query that has its own, wherever it stands, such as `(q LIMIT 5) ORDER BY 1`
     or a branch `((q OFFSET 1) LIMIT 2)` (X5): it would need a compound of one branch, though DuckDB answers it;
   - a compound as the query of IN, EXISTS or a scalar subquery (X6);
   - `UNION [ALL | DISTINCT] BY NAME` and non-SELECT branches (X4).

   A parenthesized branch's own WITH or tail is answered.

### The AST (X4a)

1. **A flat list of branches**, as ADR 0036's decision 3 has it and as [ADR 0022](0022-joins-and-query-blocks.md)'s
   FROM list is flat. Each `SetBranch` gains the operator written before it (`kUnion`, `kIntersect` or `kExcept`) and
   an `all` flag: `UNION ALL` is `kUnion` with `all`, and `DISTINCT` is `all = false`. A `kSetBranchFields` tripwire
   stands in for the compiler, as `kSelectStatementFields` does.
2. **Precedence is not stored.** `sql::SetOperationTree`, a pure function of the `sql` module, builds DuckDB's tree
   from the list, for the binder and the tests. Round trips stay exact, and a chain of 3,000 branches stays one level
   deep.
3. **Parentheses are the only nesting:** a branch's query is a block or a parenthesized query.
4. **Walkers.** `Eq` compares the operators. The unparser prints them, with parentheses exactly where a branch has its
   own WITH, compound or tail. `relation_count`'s `Top` recurses into nested queries, so that their relations count
   against the limit of 256. `RejectNullItem` looks through nesting.
5. **What X4a answers.** UNION ALL with the nesting: parenthesized branches, with or without their own WITH or tail,
   typed by the Types rule (a nested run of UNION ALL spliced, a nested compound with its own WITH or tail folded on
   its own). The other operators exit with code 4 naming X4b, X4c or X4d, and a tree is answered once the items of
   all its operators have landed.

### The plan node (X4s)

1. **`SetOperationNode`.**
   - **Fields:**
     - `inputs`: one or more, in written order (the binder makes two or more; PF15 can leave one, which still
       deduplicates);
     - `columns`: per input, one `BoundColumn` per output column, every one of them a key;
     - `ids`: per output column, new columns, as a union's are;
     - `formula`: the region's operators (item 2) in postfix: `kInput` leaves and n-ary `kUnion`, `kIntersect`,
       `kIntersectAll`, `kExcept` and `kExceptAll` terms, the last term the root;
     - `span`: the first operator.
   - **EXPLAIN:** `SetOperation inputs=3 formula=(0 UNION (1 INTERSECT 2)) columns=[...]`, with the inputs one level
     deeper, in order.
2. **One node per region.** A region is a maximal connected part of the tree whose operators are UNION, INTERSECT,
   INTERSECT ALL, EXCEPT or EXCEPT ALL, and each region becomes one node: `A INTERSECT B INTERSECT C`,
   `A UNION B INTERSECT C` and `(A INTERSECT ALL B) EXCEPT ALL C` are one node each.
   - Its inputs, in written order, are blocks, UNION ALL subtrees (one `UnionAllNode` each) and parenthesized queries
     with their own WITH or tail.
   - Within a node, a left-deep run of one operator is one n-ary term: OR, AND and min associate, and
     max(max(a − b, 0) − c, 0) = max(a − b − c, 0) for counts.
3. **UNION ALL stays ADR 0036's,** in the maintainer's direction:
   - A compound of UNION ALL alone binds through `BindUnionAll` and keeps today's plan for every query antb1 answers
     today. X4a extends it to parenthesized branches and spliced runs, and moves its fold and its per-leaf casts into
     helpers that the new binding shares, with the operator's name in their messages; `UnionAllType` and `WidenTo` are
     reused as they are.
   - No set-operation plan holds a UnionAll that the query does not write. Below a node a UNION ALL is one input,
     whose count of a row is the row's copies over all its branches; above a node it passes the node's rows through.
     `(A UNION ALL B) EXCEPT C` is a `SetOperation` over `UnionAll (A, B)` and C, and `A INTERSECT B UNION ALL C` is a
     `UnionAll` over a `SetOperation` of A and B, and C.
   - `UnionAllNode` and `UnionAllOperator` do not change. X7a's pass keeps its behavior: its `ConstantColumn` gains
     the node's case, which reads no constant through the node.
4. **Visitors.** Every exhaustive visit handles the node, as ADR 0036's decision 1 lists them for a union, and:
   - the arity invariant becomes: a Join and a DependentJoin have two inputs, a UnionAll and a SetOperation one or
     more, every other node at most one (docs/architecture.md changes with X4s);
   - `EstimateRows` follows the formula: a UNION gives the saturated sum of its operands, an INTERSECT the smallest,
     an EXCEPT its first operand's estimate; it is unknown where an operand it reads is unknown;
   - the pruner keeps every column, since each is a key of the deduplication;
   - no rule that matches a GroupAggregate by its shape sees the node: not
     [ADR 0018](0018-dependent-group-keys.md)'s dependent keys, [ADR 0032](0032-aggregate-placement.md)'s placement
     or the partition top-N. [ADR 0038](0038-grouping-sets.md) gave grouping sets a node of their own for the same
     reason.

   Three walks are not exhaustive visits, so the compiler does not flag them; a test names the node for each:
   - the sharing pass's walks, which collect and emit nothing below the node, as they stop below a union (item 5):
     walks that missed the node would share an aggregate inside an input, which can fail the query at the cap;
   - [ADR 0023](0023-unnesting.md)'s semi-join reduction reads the node as it reads a union (`ReadsAJoinOrUnion`): no
     filter that a join offers travels into an input;
   - ADR 0018's pass treats the node as it treats a GroupAggregate: the node reads every input in full, so a LIMIT
     above it limits no input.

   Missing either of the last two loses only a rewrite.
5. **Sharing.** The node is keyed by its formula and its inputs in order. Nothing below it is collected, as nothing
   below a union is, for ADR 0036's reason: an aggregate-topped input held on its own can outgrow
   `MaterializedTable::kMaxBytes`, and a result past that cap fails the query. `WorthSharing` refuses the node at the
   top, as it refuses every top that is not an aggregate. So no result topped by a set operation is shared, and X4b does
   not wait for PF5. A GROUP BY above the node, such as the one that tops Q75's CTE, stays shareable, as a GROUP BY
   above a union is. PF5 may share set operations once the cap falls back to recomputation.

### The operator (X4s, serial)

1. **One hash table,** a `GroupTable` keyed on every column, with no aggregate calls. The inputs open one at a time in
   written order, each closed before the next opens, as in `UnionAllOperator`, so
   [ADR 0022](0022-joins-and-query-blocks.md)'s "one part scheduler at a time" holds.
2. **Starters insert, the other inputs look up.**
   - **Starters** are the inputs whose rows can reach the result. Per term: an input's starter is the input itself; a
     UNION's starters are all its operands' starters; an INTERSECT's or EXCEPT's starters, ALL or not, are its first
     operand's. The node's starters are its root's. In `(A UNION B) EXCEPT C` the starters are A and B: they insert,
     and C only looks up.
   - A starter's rows go into the table through `Consume`, which now returns the group ids it computes. Every other
     input only looks its rows up through a new `KeyTable::Lookup`, which gives no group for a key not seen and adds
     none: Arrow 25's `Grouper::Lookup` in today's grouper table, a probe of its own in a key table of
     [ADR 0019](0019-own-aggregation-hash-table.md)'s.
   - **Lookups miss nothing that matters.** An input that is not a starter lies in a later operand of some INTERSECT or
     EXCEPT, ALL or not; take the outermost one. The input changes the result only for a group that this operator's
     first operand holds. A starter of that operand inserted each such group: those starters are the node's, since no
     operator above holds the operand in a later operand, and they are written, and so read, before the input.
3. **Per group and input,** one presence bit, or a count where a term reads it. An EXCEPT ALL reads its operands'
   counts. An INTERSECT ALL reads them where its own count is read: at the root, or by an operator that reads counts.
   Every other operator reads presence only. A UNION ALL input's count is a row's copies over all its branches.
4. **The formula,** after the last input, gives each group its number of copies. With [x] = 1 for x > 0 and 0
   otherwise, and X1 to Xn the operands' values:
   - UNION: [X1] ∨ … ∨ [Xn]; INTERSECT: [X1] ∧ … ∧ [Xn]; EXCEPT: [X1] ∧ ¬[X2] ∧ … ∧ ¬[Xn];
   - INTERSECT ALL: min(X1, …, Xn); EXCEPT ALL: max(X1 − X2 − … − Xn, 0).

   DISTINCT formulas run as bitmaps, formulas with counts as integer loops. The root's value is a group's number of
   copies: 0 or 1 under a DISTINCT root.
5. **Output.** Under a DISTINCT root, each kept group once, with the key first seen among the starters: in written
   order, then in each input's part order. Under an ALL root, each group repeated by its copies (a take with repeated
   indices) with its normalized key. That is DuckDB's spelling wherever its plan keeps the ALL operator (D29), and it
   needs no first-seen copy.
6. **Errors.** Every input is read in full, in written order, so the first input that fails decides the error. The
   node is blocking: a LIMIT above it saves no input, and D26 stays UNION ALL's alone.
7. **Memory.** The table and the bits or counts are charged to the memory budget. They hold the starters' distinct
   rows only: an input in a later operand of an INTERSECT or EXCEPT, ALL or not, only looks up, and none of its rows
   is held. Nothing spills: past the budget the query fails, as a GROUP BY does.
8. **Determinism** ([ADR 0013](0013-parallel-execution.md)): the same rows and first-seen keys for any number of
   threads and any batch size.
9. **Profile:** each input's rows and lookup hits, and the groups.

### Guards

Each is pulled in front of the item it protects when ADR 0037's SF 1 budget gate or its own trigger fires.

- **X4p, parallel inputs.** It protects X4b, whose Q75 is the likeliest to miss its time: its three inputs are all
  starters.
  - A starter that is a part pipeline deduplicates each part (`PartGrouper`, with a COUNT(*) per group where its
    count is read), then merges into the 32 partitions' tables, partition by partition, in part order
    (`PartitionLanes`). `MergePartition`, like `Consume`, returns the group ids it computes, so that a lane sets the
    starters' bits.
  - An input that only looks up routes its rows by key hash as `GroupTable::RouteRows` does, DOUBLE keys normalized
    first, and each lane looks up its own partition, since a `Grouper` is single-threaded. A serial input is routed
    on the consumer's thread.
  - The rows and the first-seen keys stay the same on any number of threads.
- **PF12, an input's own DISTINCT dropped** where only its presence is read and no LIMIT or OFFSET of the input sits
  above it, so that its rows feed X4p's lanes. That saves the input's own deduplication. An input that only looks up
  then holds no table, where its DISTINCT's part tables and lanes stood beside the node's table; a starter's rows are
  deduplicated once, by the node, not twice. It needs X4p, and it protects X4c and X4d: Q38 and Q87 deduplicate every
  input on its own, the later ones while the node's table is held, and are likely to press on the memory limit.
- **PF13, the smallest input as the starter**, by estimate, in a region of INTERSECT and INTERSECT ALL alone, where a
  row needs every input. Under a DISTINCT root the region has no DOUBLE or FLOAT column, the only type that spells an
  equality class several ways; under an INTERSECT ALL root the keys print normalized, so any column is allowed. It
  opens the starter first and the other inputs after it in written order, so where several inputs fail, the first in
  that order decides the error, and D30 says so when PF13 lands. It protects X4c's memory where a later input is the
  smallest.
- **PF14, a node whose inputs a constant column makes pairwise disjoint**, all of them (the shape of Q36 and Q49).
  Each input is deduplicated on its own, kept where the formula with that input alone keeps it, and its table is
  freed before the next input opens. The constants are compared after widening, NULL equal to NULL, -0.0 to 0.0 and
  NaN to NaN. Every input is still read: cutting short an INTERSECT, or an EXCEPT's later operands, would answer where
  DuckDB fails. Its trigger is a profile of such a query once window functions let it pass.
- **PF15, conditions into every input.** A condition above a set operation moves into every input, the lookups' too,
  as DuckDB pushes it. No copy stays above, since DuckDB returns an EXCEPT ALL row that fails its own condition: the
  normalized value of a -0.0 that passed `1 / d < 0`.
  - An input that the condition rules out by its constants (X7a's check) becomes empty, and the formula simplifies. A
    UNION drops the input, and an INTERSECT is empty. An EXCEPT drops a ruled-out later operand, and is empty when its
    first operand is ruled out. The ALL forms simplify the same way.
  - A term left with one operand keeps its formula: a UNION or EXCEPT still removes duplicates, and an EXCEPT ALL root
    still prints normalized keys, where DuckDB's plan may drop it (D29).
  - It ends D29's condition case, and narrows D27's extension to D27's own cases. None of the four queries puts a
    condition above a set operation, so only a profile or the gate pulls it in.

### Divergences

Each goes into docs/sql-subset.md with the item that makes it reachable. Parts of D29 and D30 are reachable today,
through GROUP BY, SELECT DISTINCT and UNION ALL, so their rows are registered with this ADR; X4b extends both to the
set operations and pins them with records.

- **D29 (today, extended by X4b): which of -0.0 and 0.0, or which NaN, a deduplication keeps, both what it prints
  and what a condition above it sees.**
  - antb1 prints the value first seen among the starters, the same on any number of threads, and the normalized
    value under an ALL root.
  - DuckDB's DISTINCT forms print a value that depends on its plan and threads. At one thread:
    - a run of three or more UNION operands is read in D30's order: `E UNION P UNION N`, with E empty, P = {0.0} and
      N = {-0.0}, prints 0.0 in antb1 and -0.0 in DuckDB. SELECT DISTINCT over a UNION ALL already differs so today;
    - a starter whose rows an EXCEPT removed can decide antb1's value: `(B EXCEPT C) UNION A`, with B = {-0.0} and
      C = A = {0.0}, prints -0.0 in antb1 and 0.0 in DuckDB;
    - DuckDB passes the rows of an INTERSECT ALL or EXCEPT ALL below a DISTINCT root on normalized:
      `(N INTERSECT ALL N) UNION E`, with N = {-0.0} and E empty, prints -0.0 in antb1 and 0.0 in DuckDB.
  - Under an ALL root, DuckDB's planner drops an EXCEPT ALL whose later operands a LIMIT 0 or a constant condition
    empties, and its rows keep their own value: `N EXCEPT ALL (P LIMIT 0)` prints 0.0 in antb1 and -0.0 in DuckDB.
  - A condition above a deduplication that tells -0.0 from 0.0, such as `1 / d < 0`, can return other rows. DuckDB
    tests every input's own value, antb1 the value it kept. SELECT DISTINCT already behaves so today, and PF15 ends
    it for set operations.
- **D30 (today, extended by X4b): which error a compound reports, if any, when its inputs fail.**
  - antb1 reports the first failing input in written order.
  - DuckDB follows its pipeline order, which is not written order. At one thread:
    - it builds the right operand of an INTERSECT or EXCEPT before it reads the left one;
    - under an aggregate, a sort or a join's build side (a UNION's own deduplication included), plain blocks of UNION
      or UNION ALL, nested ones too, run the first block, then the others from the last back, so UNION ALL already
      differs today: `A UNION ALL B UNION ALL C` under a COUNT(*) reports C's error before B's;
    - a block with a sink of its own (a GROUP BY, a DISTINCT, an INTERSECT or an EXCEPT) changes that order:
      `(A EXCEPT B) UNION C` reports C's error first;
    - the blocks of a UNION ALL in the left operand of an INTERSECT or EXCEPT run in an order that changes between
      runs: `(A UNION ALL B) EXCEPT C` reports A's error in some runs and B's in others.
  - Above one thread every choice varies between runs.
  - DuckDB skips the other operand where a LIMIT 0, a constant condition or a constant pushed into it empties an
    operand of an INTERSECT, or the first operand of an EXCEPT. Where only file statistics prove an operand empty, it
    still builds the right operand and reads the left one's first vector (D31).
- **D31 (X4c): an INTERSECT whose right operand turns out empty.** DuckDB stops each pipeline that streams into the
  join (each UNION ALL branch, and at more than one thread each row group a thread starts) after the first vector of
  2,048 rows that reaches the join with a row, and returns no rows. Below an aggregate, a DISTINCT, a sort, a join's
  build side or a nested set operation it reads the left operand in full, so `A INTERSECT B INTERSECT C` with only C
  empty reads A and B in full. antb1 reads the left operand in full, so an error past those vectors fails the query.
  INTERSECT ALL, EXCEPT and EXCEPT ALL read the left operand in full in both engines.
- **D27 (X4b) extends to set operations.** Until PF15, a condition above a set operation stays above the node, so
  every input runs in full, and an error in its select list fails the query, even on rows the condition rejects.
  DuckDB pushes the condition below each input's select list and skips an input whose constants rule it out. PF15
  narrows this to D27's own cases.

### Tests

- `.slt` records completed from DuckDB with result type names, under `rowsort`. Hand-written D29 and D30 records put
  no UNION ALL below the left operand of an INTERSECT or EXCEPT, where DuckDB's answer changes between runs.
- **The generator.**
  - Features `kUnion`, `kIntersect`, `kExcept`, `kSetOpAll` and `kSetOpParentheses`, each pending until its item,
    drawn from a salted stream of their own, as RU3's grouping sets are.
  - The branches differ: the same FROM shape with conditions drawn independently. Identical copies would make UNION
    and INTERSECT a plain DISTINCT and EXCEPT empty.
  - No DOUBLE or FLOAT item (D29), and no arithmetic that can overflow (D27, D30, D31).
- **Metamorphic relations:**
  - A ∪ B = DISTINCT (A ∪all B);
  - A ∩ A = A ∪ A = DISTINCT A, and A − A = ∅;
  - (A − B) ∪all (A ∩ B) = DISTINCT A, and (A −all B) ∪all (A ∩all B) = A as bags;
  - |A ∪ B| + |A ∩ B| = |DISTINCT A| + |DISTINCT B|;
  - (A − B) − C = A − (B ∪ C) = A − (B ∪all C);
  - A ∩ (B ∪ C) = (A ∩ B) ∪ (A ∩ C), and A ∩ B = A − (A − B);
  - A ∪ B ∩ C = A ∪ (B ∩ C);
  - the same answer for any batch size and number of threads.
- **An oracle inside the engine:** ADR 0036's sketch written as SQL (a UNION ALL of the blocks with constant markers,
  a GROUP BY of every column, a HAVING over the markers), which antb1 answers today, against the node on random
  synthetic data. Under an ALL root the sketch gives each group's number of copies (a COUNT of 1-or-NULL markers per
  input, the formula in a CASE), and the node's rows are compared grouped, with COUNT(*).
- The relations and the oracle compare rows as equality classes, or keep -0.0 and NaN out of their data: the sketch
  prints the first value of any input.
- **X4s**, with no SQL:
  - plan tests that name the node in the three walks the compiler does not flag: a shared sub-plans case as the
    union's, the semi-join reduction's gate, and ADR 0018's pass;
  - exec tests on hand-built plans for each operator and mixed formulas, NULLs and partial NULLs, -0.0 and NaN first
    seen among the starters, empty inputs, a group count equal to the starters' distinct rows, ALL repetition, the
    first failing input in written order, and the memory budget;
  - a micro benchmark in bench/ of the node against the sketch, recorded as an update here, as ADR 0038's RU2 update
    records its operator.

## Update (2026-11-06, X4a): the grammar, built

- **Built as decided:** the flat list with `SetBranch::op` and `all` and the `kSetBranchFields` tripwire,
  `sql::SetOperationTree`, the unparser's parentheses, `relation_count`'s nested branches, and the binder's refusals
  of UNION, INTERSECT and EXCEPT naming X4b, X4c and X4d. UNION ALL answers parenthesized branches through
  `BindUnionAll`, which takes the tree's leaves, so a parenthesized run of UNION ALL is spliced and typed with its
  parent. The tree's tests come from DuckDB's `json_serialize_sql` on synthetic SQL.
- **A compound's own tail stays X5 at any depth** (the maintainer's decision at the item's start). Rule 7's last line
  narrows: X4a answers a parenthesized branch's own WITH list, and a parenthesized block's own ORDER BY, LIMIT and
  OFFSET, but not a parenthesized compound's, which needs X5's binding of a tail over a compound's output.
- **Syntax errors, as in DuckDB's parser:** a WITH list before a parenthesized query that has one of its own, and an
  ORDER BY, LIMIT or OFFSET after a parenthesized query that has the same clause. Another clause after it stays exit
  code 4 (X5), as rule 7 says.
- **Depth:** each pair of parentheses around a query counts a level against the parser's limit, so redundant
  parentheses cannot recurse without bound; the tree holds no level for them.
- **NULL items:** a parenthesized compound with a WITH list of its own binds on its own, and a column of NULL items in
  it is still exit code 4: no plan column holds DuckDB's NULL type yet, so rule 4's "reaches its parent as one" waits
  for the item that needs it.

## Update (2026-11-06, X4s): the node and the serial operator, built

- **Built as decided:** `plan::SetOperationNode` with `SetTerm` formulas in postfix, every exhaustive visit, the three
  walks the compiler does not flag (each named by a test), and `exec::SetOperationOperator` over one `GroupTable`
  with starters, lookups, bits and counts. Nothing binds the node yet; the physical planner plans it, so a
  hand-built plan runs end to end with its profile.
- **Helpers in `plan`,** shared by the operator and the tests: `CheckSetFormula` (each input once, in written order; a
  single root that is an operator; an operator of one operand allowed, so PF15 can leave a node of one input),
  `SetStarters`, `SetCounted` and `SetFormulaText`, the formula as EXPLAIN prints it.
- **`EstimateRows`** bounds an INTERSECT by its known operands, the smallest of them, where an unknown operand would
  have made the whole estimate unknown: a row of an INTERSECT is in each operand, so any one of them bounds it.
- **`GroupTable::Consume`** gives its group ids to a caller that asks for them, and `GroupTable::Lookup` normalizes
  DOUBLE keys as `Consume` does. `KeyTable::Lookup` wraps Arrow 25's `Grouper::Lookup`, with `Consume`'s workaround for
  a fixed-width key column without a values buffer.
- **The profile** counts over all inputs, not per input: the starters' rows, the lookups' rows and hits, and the
  groups. A per-input split can come with X4p, whose lanes report per input anyway.
- **Tests stand in for the oracle** until X4b gives the node SQL: a randomized test compares the operator with the
  forms' multiset definitions computed in the test, over random formulas of two to five inputs, NULLs included, at
  batch sizes 1 and 7.
- **The micro benchmark** is `bench/set_operation_bench.cc` (`antb1-bench-setops`): a three-input INTERSECT, a
  two-input EXCEPT and a three-input UNION, the node against the sketch, on one thread, with few or mostly distinct
  rows. Its numbers are recorded here once measured on a quiet machine.

## Evidence

Checked on synthetic data only, against DuckDB 1.5.6, during the review and while this ADR was written:

- **The node's model** (the formula over starters and lookups, with UNION ALL a node of its own) gave DuckDB's rows,
  as equality classes, on every tree tried: 3,000 random trees at one DuckDB thread, 1,500 at four, 240 over tables
  of 2,000 to 20,000 rows at eight, and 1,800 chains of 3 to 8 inputs without parentheses, which mixed all six
  operators, parentheses, NULLs and partial NULLs, -0.0, 0.0 and NaN, duplicates and empty tables; and 1,600 more,
  over INTEGER and VARCHAR columns only. The checks of this ADR's text added 5,000 and 4,800 random trees, with no
  row that differs.
- **The sketch's memory.** Two synthetic three-input INTERSECTs were written as ADR 0036's sketch. antb1's
  GroupAggregate held 257,356 and 280,000 groups (`explain --analyze`), where the first inputs have 131,907 and 60,000
  distinct rows, all the node would hold. The second case is built so that its last input holds every row of the
  others.
- **The sketch's answers.** antb1 answers it today, and its rows matched DuckDB's operators on three-input
  INTERSECT, EXCEPT and UNION and on two mixed trees, once the sign of zero is normalized.

## Consequences

- Phase 6 takes the ratchet from 69 to 73: Q75 with X4b, Q8 and Q38 with X4c, Q87 with X4d.
- Every `std::visit` over the plan grows by one alternative, each answered in X4s.
- A compound of UNION ALL alone keeps its plan, and no set operation is shared before PF5.
- The node is blocking: no LIMIT ends it early. It holds the starters' distinct rows at once and does not spill, so a
  set operation fails at the memory budget near that size. ADR 0036's sketch would have failed near the size of
  every input's distinct rows together.
- Q14's INTERSECT is computed once per reference of its CTE. The references sit under UNION ALL branches, where
  nothing is shared, and the CTE's top is a join, which ADR 0030 does not share. Lifting ADR 0036's rule for members
  below a union would not be enough: the sharing pass offers only the outermost repeated sub-plan, here the CTE's
  join, which `WorthSharing` refuses, so the set operation inside it would also have to be offered.

## Alternatives considered

- **ADR 0036's sketch:** a UNION ALL with a marker per branch, a GroupAggregate keyed on every column, a Filter over
  the markers and a Project. It is the smallest change, about 0.3k lines beyond the grammar and binding that every
  design shares, with no exec code, and antb1 answers it today. Rejected:
  - every set-operation plan would hold a UnionAll the query does not write, against the maintainer's direction;
  - its table holds every input's distinct rows, on the consumer's thread;
  - a GroupAggregate tops each UNION, which sharing collects, so PF5's fallback had to come first;
  - every rule that matches a GroupAggregate by its shape sees it;
  - the ALL forms need a node that repeats rows anyway;
  - it prints the value first seen in any input, a lookup's too: `(B INTERSECT C) UNION A`, with B empty,
    C = {-0.0} and A = {0.0}, prints -0.0 where DuckDB and the node print 0.0.

  It stays as the oracle above, and as the fallback for the DISTINCT forms if X4s is rejected: only the binder's
  lowering would change. The ALL forms would still need a node that repeats rows.
- **DuckDB's plan:** semi and anti joins on NULL-equal keys over every column, a DISTINCT above them (DuckDB itself
  runs two per operator), and ROW_NUMBER windows for the ALL forms. Rejected:
  - it needs OJ5 and DOUBLE and BOOLEAN join keys in the join table's hottest loops, which pulls phase 7 into phase 6
    against ADR 0037's decision 3;
  - its ALL forms wait for window functions (phase 9);
  - its builds hold the later operands whole, duplicates included;
  - its probes run in parallel only over a part pipeline.

  A join plan stays possible as a physical alternative once OJ5 exists.
- **One node per run of one operator,** DuckDB's tree. Rejected: one more hash pass per level, where the formula
  answers the whole tree in one.
- **A nested AST, one compound per precedence level.** Rejected: the depth grows with the tree, and the round trip
  needs a canonical nesting, where the flat list keeps both simple.
- **UNION ALL fused into the node,** as a sixth operator term, `kUnionAll`, the sum of counts. Below a node it would
  save one operator and run the branches in parallel before X7. Rejected:
  - the written UNION ALL would disappear from EXPLAIN and from profiles, and X7a's pass would no longer see it;
  - the same SQL would get different plans in different places;
  - above a node it would end the pass-through: rows hashed and repeated, and the lazy opening and D26 lost;
  - none of the 99 has a UNION ALL next to another set operator, and ADR 0036's X7, one part pipeline over every
    branch, gives the parallelism without it.

  Revisit only if a profile shows such an input dominating before X7 exists.
- **The ALL forms refused** under an id of their own. Rejected: the operator keeps counts from X4s on, and ADR 0037's
  decision 2 asks for DuckDB's grammar.
- **The lanes built in X4s.** Rejected for the order of the work: X4s proves the node serially, and X4p comes when the
  gate asks for it.
- **The first-seen spelling under an ALL root.** Rejected: DuckDB prints the normalized value there wherever its plan
  keeps the operator (D29), and the normalized key needs no first-seen copy.

This workload is derived from the TPC-DS Benchmark and is not comparable to published TPC-DS Benchmark results, as
this implementation does not comply with all requirements of the TPC-DS Benchmark.
