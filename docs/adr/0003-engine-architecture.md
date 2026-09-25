# 3. Engine architecture

Date: 2026-09-25

## Status

Proposed. The shape of the first SQL slice is decided; the questions listed under Consequences stay open until
real workloads beyond the slice exist.

## Context

- The first goal is a thin, correct slice: `SELECT` with global aggregates (COUNT, SUM, AVG, MIN, MAX), a
  conjunction of `column <op> literal` predicates, projection and `LIMIT`, over one table of Parquet files. It must
  pass ClickBench Q0, Q1, Q2, Q3 and Q6 with results identical to DuckDB.
- Apache Arrow provides columnar arrays, Parquet reading and compute kernels. Arrow's own execution engine (Acero)
  would hide the operator design that this project wants to explore and control.
- The engine must be easy to test in layers (parser without Arrow, binder against in-memory tables, operators
  without files) and easy for agents to extend safely.

## Decision

For the slice:

- Seven modules with an enforced allow-list (`cmake/Antb1Modules.cmake`, documented in
  [architecture.md](../architecture.md)): `common`, `sql` (Arrow-free), `plan`, `io`, `exec`, `engine`, `cli`.
  `exec` sees tables only through the `plan::Table` interface and never depends on `io`.
- Our own intermediate representation: an AST from a hand-written parser, then a logical plan whose nodes are
  immutable structs in a `std::variant`. Every consumer uses an exhaustive `std::visit`, so adding a node fails to
  compile until the physical planner and EXPLAIN handle it.
- Pull-based, batch-at-a-time physical operators (`Open`, `Next` returning a record batch or end of stream,
  `Close`), built by a physical planner and drained into an Arrow table.
- Arrow compute kernels for comparisons, filters and min/max; no Acero. Integer SUM and AVG use our own 128-bit
  accumulators because Arrow's `sum` wraps at 64 bits.
- Single-threaded execution, reading files and row groups in order, in batches of 64Ki rows. Metadata-only answers
  are their own logical node and operator: `COUNT(*)` without `WHERE` is a row count from the Parquet footers.
- Planned operators for the slice: table scan of referenced columns only, filter with a selection mask, scalar
  aggregate, projection, limit (stops early) and row count.

## Consequences

- The slice stays small and easy to reason about, and each layer can be tested on its own.
- Performance is limited to one core until parallelism is designed.
- Open questions, to be settled by a later ADR that moves this one to Accepted or supersedes it:
  - intra-query parallelism (for example morsel-driven scheduling) and how operators share work;
  - the GROUP BY strategy (hash aggregation, memory limits, spilling);
  - the expression IR beyond `column <op> literal` (arithmetic, functions, `OR`, `NOT`);
  - pull versus push execution once pipelines get longer;
  - memory accounting and the use of Arrow memory pools.
