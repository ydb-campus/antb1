---
name: write-slt-test
description: Write SQL logic tests (sqllogictest .slt files) for antb1 whose expected results come from DuckDB, never by hand. Use when adding or changing SQL behavior tests, or when an .slt expectation or oracle test fails.
---

# Write a SQL logic test

Step-by-step details and a worked example: [docs/recipes/write-slt-test.md](../../../docs/recipes/write-slt-test.md).

## The harness

The sqllogictest runner (`tests/slt/runner`, binary `antb1-slt`), its fixture tables (`tests/slt/tables.txt`) and
`pixi run slt-complete`, which writes the expected blocks from DuckDB, are all in the repository; the cases live in
`tests/slt/cases/<area>/*.slt` and run under the ctest labels `slt` (antb1) and `oracle` (DuckDB)
([docs/testing.md](../../../docs/testing.md#test-layers)).

## Rules

- Expected results are written by DuckDB, the oracle ([ADR 0006](../../../docs/adr/0006-test-strategy-and-data-policy.md)).
  Never type or edit an expected block by hand; regenerate it and review the diff.
- Tables come from the committed fixture generator only (synthetic, deterministic data). Never use ClickBench data,
  samples or query text, never write TPC-H query text or fragments of it, and never commit data files.
- Engine-specific records (`onlyif antb1`, `skipif duckdb`) are allowed only for a divergence registered in
  docs/sql-subset.md, with its ID in a comment.
- Test the rejections too: SQL outside the subset must fail with the unsupported error, not return rows.
- If a step needs an "Ask a human first" path (AGENTS.md), for example a new pixi task, a CMake preset or a
  ratchet (ClickBench or TPC-H-derived): stop and hand off with the exact change (file, diff, reason) for a maintainer.

## Workflow

1. Pick the area file for the feature (one file per feature area, several small records rather than one big one).
2. Write records in standard sqllogictest form: `statement ok` or `statement error <regex>`, and
   `query <types> <sort mode>` followed by the SQL. Use `nosort` with an `ORDER BY` whose keys end with a unique
   column (rows with equal keys may come in any order), otherwise `rowsort` for multi-row results.
3. Leave the expected block empty and run the completion task (it runs the SQL on DuckDB and writes the results
   below `----`); records that only antb1 runs are flagged for review instead of completed.
4. Review the generated results: do they match docs/sql-subset.md? A surprising value is a bug in the test or a
   semantic difference to register, never something to edit away.
5. Run the SQL logic tests against antb1 and against DuckDB (ctest labels `slt` and `oracle`), for example
   `pixi run test -L slt`, then `pixi run check` before the PR.
6. A failing record prints file:line, the SQL, expected vs actual and a repro command: fix the engine, not the
   expectation.
