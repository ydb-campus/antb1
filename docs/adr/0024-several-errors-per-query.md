# 24. Several errors per query, scheduled for later

Date: 2026-10-11

## Status

Proposed (deferred: the maintainer asked for the design ahead of the code, and no roadmap PR implements it yet)

## Context

- **Today a query reports one error and stops.** `plan::SqlErrorDetail` (`src/plan/include/antb1/plan/sql_status.h`)
  carries one `Kind` and one `SourceSpan`, errors travel as an `arrow::Status` built by `BindError`,
  `UnsupportedError` (`src/plan/sql_status.cc:34`, `:39`) or `Reject` (`src/plan/binder.cc:805`), and the binder
  returns at the first failure through 98 `ARROW_RETURN_NOT_OK`/`ARROW_ASSIGN_OR_RAISE` sites. The CLI prints the
  message, `at line L, column C` and a caret of the span's length (`src/cli/cli.cc:167`).
- **So does DuckDB**, the oracle: `SELECT nope1, nope2 FROM t` reports only `nope1`, and
  `with c(x) AS (SELECT a FROM nope) SELECT x FROM missing` reports only `missing`. Matching the oracle therefore
  does not require reporting several errors, and ADR 0006 makes the oracle the contract for *answers*, not for how
  many diagnostics accompany a refusal.
- **What the single error costs.** A query with several independent mistakes — three misspelled select items, a
  missing table in each of two derived tables — takes one edit-and-rerun cycle per mistake. The errors are
  independent: none of them is a consequence of another, and each has its own span.
- **What makes this cheap to add without disturbing the gates.** The `.slt` harness compares an expected error by
  `std::regex_search` over the message (`tests/slt/runner/runner.cc:250`), so text appended after the first error
  still matches. The CLI's error goldens are byte-exact, so they would change — but only if the extra errors are
  printed, which is a decision this ADR can scope.
- **What has no rule yet.** Exit codes are per kind: 4 for `kUnsupported`, 1 for a parse or bind error
  (docs/sql-subset.md's exit-code table), and 70 for any other `NotImplemented`. With several errors of different
  kinds in one query there is no rule for which decides.
- **A related question that is not this one.** An unreferenced CTE's body is never bound, as in DuckDB, so the
  errors inside it do not exist to be reported: `WITH c AS (SELECT a FROM nope) SELECT COUNT(*) FROM t` answers.
  Reporting `nope` would mean binding bodies DuckDB never binds, which is a change to what antb1 *accepts*, not to
  how it reports. It is out of scope here and argued against under the alternatives.

## Decision

- **One list, not a second channel.** `SqlErrorDetail` gains the errors after the first, each with its own kind,
  span and message. `arrow::Status` keeps one detail, so nothing about how errors propagate changes.
- **The first error is unchanged, in message, span, kind and position.** It is the error the query reports today:
  the first one in query order, by the walk the binder already performs. Every existing expectation — the `.slt`
  records, the CLI goldens, `plan.*/BindErrorTest`, the exit codes — therefore holds without edits, and the
  oracle comparison is unaffected.
- **Only independent errors are collected**, which means errors from sibling subtrees whose binding does not
  consume each other's results:
  - one per select item;
  - one per `WHERE`, `HAVING` and `ON` conjunct;
  - one per `GROUP BY` and `ORDER BY` item;
  - one per FROM item, and one per block for a sub-plan (ADR 0022's J4).
  Inside one expression the binder stops at the first error, as it does today: once a sub-expression has no type,
  every error above it is a consequence, and reporting those is noise rather than help.
- **The binder continues only where it can do so without using a failed result.** The sibling loops above take a
  collector that records an error and skips that sibling; the 98 early returns within an expression stay. A
  collector that is not empty makes the statement fail, whatever the rest of the binding produced, so no plan is
  ever built from a partially bound block.
- **The exit code is the first error's kind**, so exit codes never change. A query with a bind error first and an
  unsupported construct later exits 1 and reports both; the reverse exits 4 and reports both.
- **At most ten errors are reported**, the first plus nine, so a pathological statement cannot print thousands.
  The count of the ones not shown is printed.
- **The CLI prints the first error as it does today**, then the others, each as its own message with its own line,
  column and caret. `--format json` (ADR 0015's shape) gains an array.

## Consequences

- No query answer changes, so neither ratchet can move and no `.slt` expectation is rewritten. This is a
  diagnostics change and can land in any roadmap order.
- The CLI's error goldens gain cases for a statement with several errors; the existing ones keep their bytes,
  because a statement with one error prints exactly what it prints today.
- `plan::Bind`'s signature is unchanged: the errors ride on the detail of the status it already returns.
- The binder gains a collector threaded through its sibling loops. That is the bulk of the work and the reason
  this is its own roadmap id rather than a rider on another: the loops are in the select list, `WHERE`, `GROUP BY`,
  `HAVING`, `ORDER BY`, the FROM list and `BindBlock`'s recursion.
- A divergence row is **not** needed: DuckDB reports one error and antb1 reports that same error first, plus more.
  The `.slt` error records match by search, and no record asserts that nothing follows the message.
- `engine::Session` and the `antb1-slt` harness read the message and the kind, both of which keep their meaning;
  the harness ignores the list, so the differential and ClickBench comparisons are untouched.

## Alternatives considered

- **A separate "notes" channel beside the status.** The extra diagnostics are errors, not notes — each would fail
  the query on its own — and `arrow::Status` carries one detail, so a second channel would need a second return
  value through every binder function. Rejected.
- **Collect errors inside an expression too.** `strlen(nope1, nope2)` would report the unknown column, the second
  unknown column and the arity. The second and third are consequences of a sub-expression with no type, and
  experience with compilers is that cascading diagnostics cost more attention than they save. Rejected.
- **Bind unreferenced CTE bodies, so their errors can be reported.** This is the case that prompted the ADR:
  `with c(x) AS (SELECT a FROM nope) SELECT x FROM missing` holds two unbound names, and only `missing` is
  reported, because `c` is referenced by nothing and DuckDB never binds it. Reporting `nope` would make antb1
  refuse a statement the oracle answers — `WITH c AS (SELECT a FROM nope) SELECT COUNT(*) FROM t` answers in
  DuckDB — which is a divergence in what antb1 accepts, not in how it reports, and docs/sql-subset.md promises
  that an unregistered difference from the oracle is a bug. It would also undo three things ADR 0022's J4
  deliberately pinned to the oracle and remove the relation counter's skip of unreachable bodies, which is what
  keeps the chain of 30 self-joining CTEs cheap. PostgreSQL does validate unreferenced CTEs, so the behaviour is
  not unreasonable in the abstract — it is simply not the oracle's. Rejected; if it is ever wanted it belongs in
  its own ADR with a divergence row.
- **Report every error the parser finds, too.** The parser stops at the first syntax error and resynchronising a
  SQL grammar is its own design problem (ADR 0008). Out of scope: this ADR is about the binder, and a parse error
  stays single.

## Plan

One roadmap PR, after the work of ADR 0022 and ADR 0023, since it touches the binder broadly and would otherwise
conflict with every slice of them:

- **G1, feat(plan): several errors per query.** The list on `SqlErrorDetail`, the collector through the sibling
  loops named above, the ten-error cap, the CLI's repeated blocks and its JSON array, and goldens for a statement
  with several errors of one kind and of both kinds. No `.slt` record changes; the existing CLI goldens keep their
  bytes.
