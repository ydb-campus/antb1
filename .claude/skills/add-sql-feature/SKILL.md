---
name: add-sql-feature
description: Add or change SQL support in antb1 end to end (a clause, predicate, aggregate, literal or type rule) - spec in docs/sql-subset.md, then lexer, parser, unparser, binder, logical plan, operators, engine and CLI, each with tests. Use for any change to which SQL antb1 accepts or how it answers.
---

# Add a SQL feature

Step-by-step details, file map and examples: [docs/recipes/add-sql-feature.md](../../../docs/recipes/add-sql-feature.md).

## Ground rules

- docs/sql-subset.md is the contract. Semantics follow DuckDB
  ([ADR 0004](../../../docs/adr/0004-types-null-overflow-semantics.md)); an intentional difference goes into its
  divergence table in the same PR.
- SQL outside the implemented subset fails with `kUnsupported` (exit code 4) and a source span; it never returns
  a wrong answer. Malformed SQL is a syntax error (exit code 1).
- `common` and `sql` stay Arrow-free and return `std::expected`; `exec` never uses `io`.
- Integer SUM and AVG accumulate in `antb1::Int128`; literals are folded exactly at bind time.
- If a step needs an "Ask a human first" path (AGENTS.md), for example a new module edge in
  `cmake/Antb1Modules.cmake`, a dependency or task in `pixi.toml`, or the ClickBench ratchet
  `tests/data/clickbench_status.json`: stop and hand off with the exact change (file, diff, reason) for a maintainer.

## Steps

1. Use plan mode when the change touches a public header (`src/*/include/`) or more than one module.
2. Spec: grammar, semantics, errors and exit codes in docs/sql-subset.md.
3. Front end, `src/sql/`: tokens, AST node with spans, parser, `ToSql` unparser, `EqualIgnoringSpans`. Tests in
   `src/sql/tests/`: accepted forms, the round trip `Parse(ToSql(ast))`, syntax errors and unsupported spans.
4. Binder, `src/plan/binder.cc` and `src/plan/types.cc`: case-insensitive names, type rules, literal folding,
   `SqlErrorDetail` kinds `kBind`/`kUnsupported` with spans. Tests in `src/plan/tests/`.
5. Logical plan: add a node to the `LogicalNode` variant in `src/plan/include/antb1/plan/logical_plan.h`; the
   compiler then points at every `std::visit` to extend (physical planner, EXPLAIN).
6. Execution, `src/exec/`: the operator or aggregate state. Test NULLs, empty input, several batches, overflow
   and selection masks in `src/exec/tests/`.
7. Engine and CLI: `src/engine/` (session, formatter) and `src/cli/` (flags, exit codes), with tests.
8. SQL logic tests: once the test-harness PR has landed, add `.slt` cases whose expectations DuckDB writes (skill
   write-slt-test). Until then the unit tests above carry the coverage.
9. ClickBench: a feature that changes which queries pass also changes the ratchet and the docs status table in the
   same PR. The ratchet is protected: hand off (skill clickbench-data).
10. Verify: `pixi run test -R '^(sql|plan|exec|engine|cli)\.'`, then `pixi run check`; add `pixi run check-full`
    for `io`/`exec` memory or ownership changes. Run the `reviewer` subagent (Claude Code) on the diff.
11. PR: title like `feat(sql): add IN lists`, verification commands and results in the template.
