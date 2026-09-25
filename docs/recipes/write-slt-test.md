# Recipe: write a SQL logic test

SQL logic tests pin down what antb1 answers for a query, with expected results written by DuckDB, the test oracle
([ADR 0006](../adr/0006-test-strategy-and-data-policy.md)). The skill `.agents/skills/write-slt-test/SKILL.md` is
the short version.

## Status: the runner arrives with the test-harness PR

The sqllogictest runner, the fixture tables and the task that completes expectations with DuckDB are added by the
test-harness PR; [testing.md](../testing.md#test-layers) lists the layer as planned and the ctest labels `slt` and
`oracle` as reserved. Until that PR has landed (there is no tests/slt directory yet):

- cover SQL behavior with unit tests in `src/sql/tests/`, `src/plan/tests/`, `src/exec/tests/` and
  `src/engine/tests/` (the Claude Code `test-author` subagent writes them);
- say in the PR description that the SQL logic tests follow with the harness.

The rest of this recipe describes the workflow the harness implements. Where it names a command that does not exist
yet, it describes it in words; use the exact task name from the AGENTS.md command table once the harness is merged.

## How the harness works

- Test files use the sqllogictest format known from SQLite and DuckDB, one file per feature area. Each file runs
  twice: against antb1 (label `slt`) and against DuckDB (label `oracle`), so every expectation is also re-checked
  against the oracle on every PR.
- Tables come from the committed fixture generator: deterministic, synthetic, hits-shaped data plus edge-case tables
  (NULLs, several files, empty tables). They are identical on Linux and macOS. Nothing comes from ClickBench.
- Expected results are written by DuckDB through a completion task. Nobody types or edits them.
- Integers and text compare exactly; floating-point values compare with a tight relative tolerance.
- A failure prints the file and line, the SQL, the expected and actual rows and a command that reproduces it.

## Record format

```text
# SUM over an integer column returns HUGEINT and skips NULLs
query I nosort
SELECT SUM(a) FROM t_nulls WHERE b >= 2
----
(written by DuckDB)

# GROUP BY is outside the subset: rejected, never answered
statement error unsupported
SELECT a, COUNT(*) FROM t GROUP BY a
```

- `query <types> <sort mode>`: one type letter per result column (`I` integer, `R` floating point, `T` text and
  dates), then `nosort` for a single row or a defined order, `rowsort` for several rows (there is no ORDER BY yet).
- `statement ok` for SQL that must succeed, `statement error <regex>` for SQL that must fail with a matching error.
- `onlyif antb1` or `skipif duckdb` before a record limits it to one engine. Use them only for a divergence
  registered in [sql-subset.md](../sql-subset.md#divergences-from-duckdb), and put its ID in a comment.

## Steps (after the harness PR)

1. Pick the area file for the feature, or add one for a new area. Prefer several small records, each with a comment
   that says what it checks, to one big query.
2. Write the records without expected results.
3. Run the completion task. It executes each query on DuckDB and writes the results below `----`; records limited to
   antb1 are flagged for review instead of completed.
4. Review `git diff` of the generated results line by line against [sql-subset.md](../sql-subset.md). A surprising
   value means a wrong test, an engine bug or a semantic difference to register; it is never edited away.
5. Run the SQL logic tests on both engines, for example `pixi run test -L slt` and `pixi run test -L oracle`, then
   `pixi run check` before the PR.
6. When a record fails later, fix the engine (or the registered divergence), not the expectation. If the oracle
   itself changed its answer after a DuckDB update, regenerate with the completion task and explain it in the PR.

## What to cover for a feature

- Every new construct on the plain table and on the NULL-heavy table; the empty table; a table split over several
  files.
- Boundary literals: the minimum and maximum of the column type, values just outside the range, a decimal literal
  against an integer column, a negative zero.
- Every comparison operator, both operand orders (`5 < c` and `c > 5`), `AND` chains, `LIMIT 0`.
- Identifier rules: other letter case, quoted identifiers, an unknown column (error record).
- The rejected neighbors of the feature: SQL that looks similar but is outside the subset must fail with the
  unsupported error.

## Never

- Hand-edit an expected block, or copy results from anywhere other than the completion task.
- Use ClickBench data, samples or query text, or commit data files.
- Delete, skip or loosen a failing record to get green.
- Change the fixture generator's output casually: every expectation depends on it. The fixture schema and the
  generator's digest are protected by tests; a change there is a separate, reviewed PR.
