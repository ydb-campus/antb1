# Recipe: ClickBench data

ClickBench's `hits` table is antb1's first real workload. Its values, query texts and published answers must never
end up in the repository or in public places. The policy is [ADR 0006](../adr/0006-test-strategy-and-data-policy.md)
and the "Data policy" section of [testing.md](../testing.md#data-policy); the skill
`.agents/skills/clickbench-data/SKILL.md` is the short version.

## What may and may not be committed or shared

| Never (repository, PRs, issues, reviews, commit messages, CI logs, prompts) | Allowed |
| --- | --- |
| Parquet files or samples of `hits`, and any other data file | pins: URL, sha256 and size of a download |
| rows or values from `hits`, including values in error messages or test names | the hits-shaped schema used by our own fixture generator |
| ClickBench query text (refer to queries by number: Q0 is the first query of ClickBench's DuckDB/Parquet file) | our own queries |
| result values, including DuckDB's published answers | per-query pass or fail status |
| any file larger than 1 MiB | |

`pixi run lint` rejects Parquet and other data files and any file over 1 MiB; authors and reviewers check the rest.

## Where the data lives

- Outside the repository, in `~/.cache/antb1/clickbench` by default (set `ANTB1_DATA_DIR` to change it).
  `pixi run doctor` shows whether `hits_0.parquet` (the first partition, about 122 MB) is present.
- The pinned download task, the redacted data tests (ctest label `data`) and the CI job that runs them on
  `hits_0.parquet` are added by a later PR. Until then there is no supported download: do not add one to code,
  scripts or tests. A human can place a copy of the partition in the data directory by hand.
- The full dataset (100 partitions, about 14 GB) is for local host runs only, never CI.

## Agents and the data

Agents never look at the data. That keeps its values out of prompts, transcripts and anything an agent writes.

- Do not print rows, samples or query results over `hits`, and do not write ClickBench query text into files.
- Schema information (column names and types) is fine: `pixi run antb1 schema --table hits=<file>`.
- To check that antb1 runs on the real file, use your own query and discard the result so that only the exit code,
  errors and timing remain:

  ```bash
  pixi run antb1 query --clickbench -f my_query.sql --table hits=<data dir>/hits_0.parquet --timing >/dev/null
  ```

- Ask a human to run unredacted comparisons on their machine when you need details of a mismatch.

## Redacted data tests (after the data-test PR)

- Every data test runs redacted. A mismatch prints only the query number or our own file and line, column names and
  types, the row count on each side, the first differing row index and a hash of each engine's result, plus the
  local command that shows the details.
- CI never uploads data or results as artifacts. Copy only redacted output into PRs and issues.
- The data tests compare antb1 with DuckDB on the same file; DuckDB runs only the queries antb1 answers.

## The ClickBench ratchet

- `tests/data/clickbench_status.json` (added with the data tests) lists the queries verified to pass on
  `hits_0.parquet`. The status table in [sql-subset.md](../sql-subset.md#clickbench-status) must match it, and
  `pixi run lint` compares the two.
- A query that starts or stops passing fails the status test on purpose. Nothing is silently skipped: unsupported
  answers are counted.
- The PR that changes the pass set updates both files. The ratchet is an "Ask a human first" path, so hand off:
  give the maintainer the query numbers, the new `pass` list and the changed rows of the status table, and the
  commands you ran. The same applies to the download pins under `tools/data/` and to `pixi.toml` tasks.
- Status and query numbers only: never add values, expected answers or query text next to them.
