<!--
PR title = squash commit subject on main: Conventional Commits with a lowercase subject, e.g. `feat(sql): parse LIMIT`
(types: feat fix perf refactor test docs build ci chore revert). The `PR title` check enforces it.
-->

## Summary

<!-- What changes and why, in a few sentences. Link the issue this PR resolves. -->

Closes #

## Type of change

- [ ] feat: new SQL, CLI or engine capability
- [ ] fix: bug fix
- [ ] perf: performance improvement
- [ ] refactor, test, docs, build, ci or chore
- [ ] Breaking change (CLI, output format or semantics); also add the `breaking-change` label

## Verification

<!--
Paste the exact commands you ran and the relevant tail of their output (for example the final
`ANTB1-TESTS: PASS|FAIL ...` line of `pixi run test`). Never paste values from ClickBench data.
-->

```text
$ pixi run check
<result>
```

## Checklist

- [ ] `pixi run check` passes locally (lint + clang Debug -Werror + hermetic tests)
- [ ] Tests cover the change (unit tests under `src/<module>/tests/`, or why none are needed)
- [ ] Docs updated where behavior, commands or architecture changed (AGENTS.md, `docs/`, an ADR), or not needed
- [ ] No ClickBench-derived data is committed: no Parquet files, query answers or values from `hits` (ADR-0006)
- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed with a maintainer

## AI assistance

<!-- AI reviews are advisory; a human is accountable for every line that merges. -->

- [ ] No AI assistance
- [ ] AI-assisted. Tools and what they did:
- Accountable human (has read and understands the whole diff): @
