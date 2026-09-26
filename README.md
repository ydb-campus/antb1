# antb1

Experimental C++23 analytics engine: SQL-like queries over local Parquet files, built on Apache Arrow.

## Status

Early and experimental. The first SQL slice works: global aggregates (`COUNT`, exact 128-bit `SUM`, `AVG`, `MIN`,
`MAX`), projections, `WHERE` conjunctions of `column <op> literal` and `LIMIT` over Parquet files, single-threaded,
with DuckDB as the test oracle. It answers ClickBench Q0, Q1, Q2, Q3 and Q6 like DuckDB.
[docs/sql-subset.md](docs/sql-subset.md) lists exactly what works today.

## Quickstart

Everything (Clang 23, CMake, Arrow, linters) comes from [pixi](https://pixi.prefix.dev) and conda-forge; no system
packages or root access are needed.

```bash
git clone https://github.com/ydb-campus/antb1.git
cd antb1
bash scripts/agent-setup.sh   # Linux: installs pixi 0.81.0 if missing, then the locked environments
export PATH="$HOME/.cache/antb1/pixi-0.81.0:$PATH"   # only if the script had to install pixi
pixi run doctor               # toolchain and build status
pixi run test                 # Clang Debug build + hermetic tests
```

On macOS (Apple silicon), install pixi 0.81.0 or newer from <https://pixi.prefix.dev> instead of running
`scripts/agent-setup.sh`; the tasks are the same. The lock file covers linux-64, linux-aarch64 and osx-arm64; CI
builds on linux-64 and osx-arm64.

Query a Parquet file (or several: `--table NAME=PATH[,PATH|GLOB]`):

```bash
pixi run antb1 query -c "SELECT COUNT(*) FROM hits" --table hits=/data/clickbench/hits_0.parquet
pixi run antb1 explain -c "SELECT COUNT(*) FROM hits" --table "hits=/data/clickbench/hits_*.parquet"
pixi run antb1 schema --table hits=/data/clickbench/hits_0.parquet --clickbench
```

SQL that contains single quotes (for example `FROM '/data/t.parquet'`) must be passed with `-f query.sql` or on
stdin with `-c -`, because pixi does not escape quotes in forwarded arguments. `--format table|csv|json` selects the
output format and `--timing` prints the elapsed seconds as the last line on stderr. `antb1 bench` runs a query file
and writes ClickBench's result JSON ([docs/benchmarks.md](docs/benchmarks.md)).

Before opening a pull request, run:

```bash
pixi run check        # lint + Clang Debug -Werror build + tests
```

## Documentation

- [CONTRIBUTING.md](CONTRIBUTING.md): workflow, commands, commit and review rules
- [AGENTS.md](AGENTS.md): the canonical guide for AI coding agents (and a compact summary for humans)
- [docs/agents.md](docs/agents.md): Claude, Codex and Copilot setup, AI reviews, secrets
- [docs/architecture.md](docs/architecture.md): modules, query lifecycle, where to add things
- [docs/sql-subset.md](docs/sql-subset.md): grammar, types, semantics, exit codes, ClickBench status
- [docs/testing.md](docs/testing.md): test layers, labels, hermetic rules, data policy
- [docs/ci.md](docs/ci.md): workflows, required checks, reproducing CI locally
- [docs/benchmarks.md](docs/benchmarks.md): micro benchmarks, ClickBench runs, published results
- [docs/adr/README.md](docs/adr/README.md): architecture decision records
- [SECURITY.md](SECURITY.md): how to report a vulnerability

## License

License: not yet decided — no license is granted yet.
