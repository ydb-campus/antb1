# Recipe: add a SQL feature

Use this for any change to which SQL antb1 accepts or how it answers: a new clause, predicate, aggregate, literal
form or type rule. The skill `.agents/skills/add-sql-feature/SKILL.md` is the short version.

The path through the code follows the query lifecycle in [architecture.md](../architecture.md#query-lifecycle):
parse, bind, plan, execute, format. Work through it in that order and add tests at every step, so a failure points
at one layer.

## Before you start

- Read [sql-subset.md](../sql-subset.md) (the contract), the "Where to add things" table in
  [architecture.md](../architecture.md#where-to-add-things), [ADR 0004](../adr/0004-types-null-overflow-semantics.md)
  (types, NULL, overflow), [ADR 0005](../adr/0005-error-boundary.md) (errors and exit codes) and
  [ADR 0008](../adr/0008-parser-and-unparser.md) (parser and unparser).
- Branch `feat/<slug>`. Keep one feature per PR.
- Plan first (plan mode in Claude Code) when the change touches a public header (`src/*/include/`) or more than one
  module, which most SQL features do.
- Hand-off rule: a new module edge or external library (`cmake/Antb1Modules.cmake`), a new dependency or task
  (`pixi.toml`), a preset (`CMakePresets.json`) or the ClickBench ratchet (`tests/data/clickbench_status.json`) is
  an "Ask a human first" path. Stop and describe the exact change for a maintainer instead of making it.

## 1. Specify

- Update [sql-subset.md](../sql-subset.md) first: the grammar, the semantics (NULLs, empty input, types, overflow),
  the errors and exit codes, and move the construct out of the "rejected" list.
- DuckDB is the reference. To see what it does on a small file you generated yourself, run
  `pixi run -e default duckdb -c "SELECT ..."` (it prompts in Claude Code). An intentional difference goes into the
  divergence table with an ID and a note on how the tests handle it.

## 2. Front end (`src/sql/`, Arrow-free)

- Tokens: `src/sql/include/antb1/sql/token.h` and `src/sql/lexer.cc`. Keywords are not reserved by the lexer; the
  parser recognizes them.
- AST: `src/sql/include/antb1/sql/ast.h`. Every node carries its `SourceSpan`. Extend `EqualIgnoringSpans` in
  `src/sql/ast.cc` for every new field.
- Parser: `src/sql/parser.cc` (recursive descent). Valid-looking SQL outside the subset returns
  `ParseError::Kind::kUnsupported` with the span of the first offending token; malformed SQL returns `kSyntax`.
- Unparser: `src/sql/unparse.cc`. `ToSql` prints the canonical form, and `Parse(ToSql(ast))` must be equal to `ast`
  ignoring spans (the parser fuzzer added with the test harness checks this property).
- Tests in `src/sql/tests/`: accepted forms and their spans (`parser_test.cc`), canonical text and round trips
  (`unparse_test.cc`), new tokens (`lexer_test.cc`), and the error kind and span of rejected neighbors.

## 3. Binder (`src/plan/`)

- `src/plan/binder.cc` resolves names ASCII case-insensitively through the catalog, checks types and builds the
  logical plan. `src/plan/types.cc` holds the logical types and their Arrow mapping.
- Compare column and literal exactly at bind time: an out-of-range literal becomes constant true or false for
  non-NULL values (NULLs stay NULL), and a decimal literal against an integer column becomes an equivalent integer
  comparison ([sql-subset.md](../sql-subset.md#semantics)).
- Errors are `arrow::Status` values with a `SqlErrorDetail` (`src/plan/include/antb1/plan/sql_status.h`): `kBind`
  for a wrong query, `kUnsupported` for a valid one outside the subset, each with the span of the offending node.
- Tests in `src/plan/tests/` (`binder_test.cc`, `types_test.cc`), including every new error.

## 4. Logical plan and EXPLAIN

- Add the node struct to the `LogicalNode` variant in `src/plan/include/antb1/plan/logical_plan.h`. Every
  `std::visit` over it is exhaustive, so the build fails until `src/plan/explain.cc` and
  `src/exec/physical_planner.cc` handle it.
- Keep EXPLAIN output stable and readable; test it next to the binder tests.

## 5. Execution (`src/exec/`)

- Operators implement `Open`/`Next`/`Close` from `src/exec/include/antb1/exec/operator.h` and pull batches through
  `plan::Table::Scan` (never through `io`). `src/exec/row_count.cc` is the smallest example.
- Aggregates: integer SUM and AVG accumulate in `antb1::Int128` (`src/common/include/antb1/common/int128.h`);
  integer SUM returns HUGEINT. Skip NULLs; over zero rows COUNT is 0 and the others are NULL.
- Arrow compute kernels need `arrow::compute::Initialize()`; `engine::Session::Make` calls it, a test that uses
  kernels without a session must call it itself. Check every `arrow::Status` and `arrow::Result`.
- Keep the hot loops lean: no per-row virtual calls, allocations or string copies; decode only referenced columns;
  prefer a selection mask to materializing filtered data.
- Tests in `src/exec/tests/`: NULLs, empty input, several batches (a small `batch_size`), overflow edges and every
  comparison operator. Build inputs with a small in-test `plan::Table` or a Parquet file written under
  `::testing::TempDir()`.

## 6. Engine, formatter and CLI

- `src/engine/session.cc` runs parse, bind, plan and execute; `src/engine/format.cc` is the one canonical value
  formatter for every output format. New result types need formatter cases and tests in `src/engine/tests/`.
- `src/cli/cli.cc` maps errors to exit codes: 4 only for `kUnsupported`, any other `NotImplemented` is 70. Test new
  flags, formats and exit codes in `src/cli/tests/cli_test.cc`.

## 7. SQL logic tests and ClickBench

- After the test-harness PR, add `.slt` cases for the feature ([write-slt-test](write-slt-test.md)); DuckDB writes
  the expected results.
- If the feature changes which ClickBench queries pass, follow [clickbench-data](clickbench-data.md): the ratchet
  and the status table in sql-subset.md change together, and the ratchet needs a maintainer.

## 8. Verify and open the PR

```bash
pixi run test -R '^sql\.'        # while iterating, one module at a time
pixi run test                    # all hermetic tests
pixi run check                   # required before every PR: lint + Clang Debug -Werror + tests
pixi run check-full              # also ASan/UBSan, clang-tidy and GCC: for io/exec memory or ownership changes
```

- Run the `reviewer` subagent (Claude Code) or review the diff against the AGENTS.md Code Review Rules yourself.
- Title: `feat(sql): <lowercase subject>` (or the module you changed most). Fill in the verification section of the
  PR template with the commands above and their final status lines.
