# Recipes

Step-by-step procedures for recurring tasks, for humans and AI agents alike. Each recipe backs the skill of the same
name in `.agents/skills/<name>/SKILL.md`: the skill is the short version an agent loads automatically, the recipe
has the details. [AGENTS.md](../../AGENTS.md) remains the source of the rules; recipes only put the steps in order.

| Recipe | Use it for |
| --- | --- |
| [add-sql-feature](add-sql-feature.md) | a change to which SQL antb1 accepts or how it answers |
| [write-slt-test](write-slt-test.md) | SQL logic tests whose expected results DuckDB writes |
| [fix-ci-failure](fix-ci-failure.md) | a red check on a pull request |
| [clickbench-data](clickbench-data.md) | anything that touches the ClickBench dataset, its data policy or the pass ratchet |
| [dependency-update](dependency-update.md) | a new or updated package, a `pixi.lock` refresh or conflict, an action pin |

## The hand-off rule

Every recipe and skill follows it: if a step needs a path listed under "Ask a human first" in AGENTS.md, stop and
hand off instead of making the change. Describe the exact change (the file, the diff and why it is needed) in your
reply or in the PR description, so a maintainer can apply or approve it. In Claude Code these paths prompt; in
GitHub Actions they are denied.

## Where the files live

- `.agents/skills/<name>/SKILL.md` is canonical (Codex and Copilot read it). `.claude/skills/` is a byte-identical
  copy for Claude Code, written by `tools/lint/sync_skills.py` when you run `pixi run fmt`; never edit the copy.
- A skill's frontmatter has only `name` (equal to its directory name) and `description`. The description says what
  the skill does and when to use it, because agents pick skills by it.
- Keep a skill between 30 and 60 lines and link its recipe; put the details here.
- `pixi run lint` checks both: the copy matches, commands in backticks name existing tasks, and backticked paths and
  links exist. Mention a task or file that a later PR adds in plain words, without backticks.
- Skills are an "Ask a human first" path (`.agents/`), recipes are not: a recipe can change in any PR, as long as it
  stays consistent with the skill that links it.
