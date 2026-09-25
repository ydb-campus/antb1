# Copilot instructions for antb1

[AGENTS.md](../AGENTS.md) is the canonical guide (commands, module boundaries, conventions, "Ask a human first",
Code Review Rules). Read it before you change anything; these lines only repeat the most important rules.

antb1 is an experimental C++23 analytics engine over local Parquet files (Apache Arrow 25). Clang 23 is the primary
compiler; GCC 15 runs a compatibility leg.

## The five commands

- Build: `pixi run build`
- Test: `pixi run test` (allowlisted ctest arguments only, e.g. `pixi run test -R '^sql\.'`)
- Format: `pixi run fmt`
- Lint: `pixi run lint`
- Required before every PR: `pixi run check`

## Top rules

- The environment is managed by pixi: use only `pixi run <task>`. Never use host compilers, host `cmake`, `apt`,
  `pip`, `conda`, `brew` or `sudo`. Every task lives in exactly one environment, so `-e` is not needed.
- Every behavior change ships with tests; unit tests live in `src/<module>/tests/`.
- Never commit or quote ClickBench-derived data, query text or result values, or any file over 1 MiB. Never
  hand-edit `pixi.lock`.
- `src/common` and `src/sql` never include Arrow; `exec` never uses `io`; module edges live in
  `cmake/Antb1Modules.cmake`.
- Do not edit the paths listed under "Ask a human first" in AGENTS.md; describe the change you need instead.
- PR titles are Conventional Commits with a lowercase subject; fill in the PR template's verification section.
- When reviewing, apply the AGENTS.md "Code Review Rules": report concrete bugs, missing tests and boundary
  violations. Never approve.
