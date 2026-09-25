---
name: write-slt-test
description: Write SQL logic tests (sqllogictest .slt files) for antb1 whose expected results come from DuckDB, never by hand. Use when adding or changing SQL behavior tests, or when an .slt expectation or oracle test fails. The runner arrives with the test-harness PR; until then SQL behavior is covered by unit tests.
---

# Write a SQL logic test

Step-by-step details and a worked example: [docs/recipes/write-slt-test.md](../../../docs/recipes/write-slt-test.md).

## First check that the harness exists

The sqllogictest runner, its fixture tables and the task that completes expectations with DuckDB are added by the
test-harness PR ([docs/testing.md](../../../docs/testing.md#test-layers) lists the layer as planned). If the
repository has no tests/slt directory yet, cover the behavior with unit tests instead (the Claude Code
`test-author` subagent does that) and say in the PR that the SQL logic tests follow with the harness.

## Rules

- Expected results are written by DuckDB, the oracle ([ADR 0006](../../../docs/adr/0006-test-strategy-and-data-policy.md)).
  Never type or edit an expected block by hand; regenerate it and review the diff.
- Tables come from the committed fixture generator only (synthetic, deterministic data). Never use ClickBench data,
  samples or query text, and never commit data files.
- Engine-specific records (`onlyif antb1`, `skipif duckdb`) are allowed only for a divergence registered in
  docs/sql-subset.md, with its ID in a comment.
- Test the rejections too: SQL outside the subset must fail with the unsupported error, not return rows.
- If a step needs an "Ask a human first" path (AGENTS.md), for example a new pixi task, a CMake preset or the
  ClickBench ratchet: stop and hand off with the exact change (file, diff, reason) for a maintainer.

## Workflow (after the harness PR)

1. Pick the area file for the feature (one file per feature area, several small records rather than one big one).
2. Write records in standard sqllogictest form: `statement ok` or `statement error <regex>`, and
   `query <types> <sort mode>` followed by the SQL. Use `rowsort` for multi-row results (there is no ORDER BY yet).
3. Leave the expected block empty and run the completion task (it runs the SQL on DuckDB and writes the results
   below `----`); records that only antb1 runs are flagged for review instead of completed.
4. Review the generated results: do they match docs/sql-subset.md? A surprising value is a bug in the test or a
   semantic difference to register, never something to edit away.
5. Run the SQL logic tests against antb1 and against DuckDB (ctest labels `slt` and `oracle`), for example
   `pixi run test -L slt`, then `pixi run check` before the PR.
6. A failing record prints file:line, the SQL, expected vs actual and a repro command: fix the engine, not the
   expectation.
