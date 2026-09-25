---
name: clickbench-data
description: Work with the ClickBench hits dataset in antb1 under its data policy - what may and may not be committed or pasted, where the data lives, how to run antb1 on it locally, redacted data tests and the ClickBench pass ratchet. Use whenever a task mentions ClickBench, hits, hits_0.parquet or query numbers like Q0-Q42.
---

# ClickBench data

Details, commands and the ratchet procedure: [docs/recipes/clickbench-data.md](../../../docs/recipes/clickbench-data.md).
Policy: [ADR 0006](../../../docs/adr/0006-test-strategy-and-data-policy.md).

## Never

- Commit, paste or quote anything derived from ClickBench: Parquet files or samples, rows or values, the query text,
  result values or DuckDB's published answers. This applies to code, tests, docs, commit messages, PRs, issues, review
  comments and prompts. Refer to queries only by number (Q0 is the first query of ClickBench's DuckDB/Parquet file).
- Commit any data file or any file over 1 MiB (`pixi run lint` rejects them).
- Upload data or unredacted results as CI artifacts, or copy unredacted data-test output anywhere public.

## Allowed

- Pins (URL, sha256, size), the hits-shaped schema of our own fixture generator, our own queries over it, and
  per-query pass/fail status.

## Local data

- The data lives outside the repository, by default in `~/.cache/antb1/clickbench` (override: `ANTB1_DATA_DIR`).
  `pixi run doctor` shows whether `hits_0.parquet` is there.
- The pinned download task and the data tests (ctest label `data`, always redacted) arrive in a later PR; until then
  do not download the data from code, scripts or tests. A human may place a copy there by hand.
- Agents never look at the data: do not print rows or query results over it and do not write ClickBench query text
  into files. Run your own queries and discard stdout, so only the exit code, errors and timing remain:
  `pixi run antb1 query --clickbench -f my.sql --table hits=<file> --timing >/dev/null`.
  Unredacted comparisons are for humans on their own machine.

## Ratchet

- `tests/data/clickbench_status.json` (added with the data tests) lists the queries verified to pass on hits_0, and
  the ClickBench status table in docs/sql-subset.md must match it (`pixi run lint` compares them).
- When a change makes a query start or stop passing, both change in the same PR. The ratchet is an "Ask a human
  first" path: stop and hand off with the exact change (the query numbers, the new `pass` list and the docs table
  rows) for a maintainer; the same applies to any other step that needs such a path (`tools/data/`, `pixi.toml`).
