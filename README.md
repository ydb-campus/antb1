# antb1

Experimental C++23 analytics engine: SQL-like queries over local Parquet files, built on Apache Arrow.

## Status

Early and experimental. A specified subset of `SELECT` works: global and grouped aggregates (`COUNT`,
`COUNT(DISTINCT ...)`, exact 128-bit `SUM`, `AVG`, `MIN`, `MAX`), projections, `WHERE` (comparisons, `LIKE`, `IN`,
`BETWEEN`, combined with `AND`, `OR` and `NOT`), `GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT` and `OFFSET`, arithmetic,
`CASE`, DECIMAL and the date and timestamp functions, over one table of Parquet files or several joined by inner
and left outer joins, run in parallel over row groups with filter pushdown and late materialization, with DuckDB as
the test oracle. It answers all 43 ClickBench queries like DuckDB, and all 22 of the queries derived from TPC-H,
correlated subqueries included: they are unnested into ordinary joins ([ADR 0023](docs/adr/0023-unnesting.md)).
[docs/sql-subset.md](docs/sql-subset.md) lists exactly what works today. Releases are 0.x versions: until 1.0, any
minor version may change the CLI, its output formats and its exit codes.

## Install

From v0.1.0 on, every [GitHub release](https://github.com/ydb-campus/antb1/releases) has one standalone `antb1` per
target: a single executable that needs neither pixi nor this repository, and no library beyond the operating
system's ([ADR 0033](docs/adr/0033-standalone-static-binary.md)).

| Target | File | CPU floor |
| --- | --- | --- |
| Linux x86-64, kernel 3.2 or newer, any distribution | `antb1-linux-x86_64.tar.gz` | x86-64-v3: AVX2, BMI2 and FMA (Intel Haswell, AMD Excavator and newer) |
| Linux AArch64, kernel 3.7 or newer, any distribution | `antb1-linux-aarch64.tar.gz` | armv8.2-a (Arm Neoverse N1, AWS Graviton 2 and newer) |
| macOS 13.0 or newer | `antb1-darwin-arm64.tar.gz` | arm64 (Apple silicon) |

Run these commands in an empty directory. They pick the file for the machine they run on, download it and
`SHA256SUMS` from the latest release, check the file and, only when it matches, unpack it and run the binary. They
work in sh, bash and zsh, and need `curl` and `tar` but no root:

<!-- install:begin -->
```sh
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) t=linux-x86_64 ;;
  Linux-aarch64) t=linux-aarch64 ;;
  Darwin-arm64) t=darwin-arm64 ;;
  *) echo "no antb1 release for $(uname -s) $(uname -m)" >&2; false ;;
esac
url=https://github.com/ydb-campus/antb1/releases/latest/download
curl -fsSLO "$url/antb1-$t.tar.gz"
curl -fsSLO "$url/SHA256SUMS"
if command -v sha256sum >/dev/null; then
  sha256sum --ignore-missing -c SHA256SUMS
else
  shasum -a 256 --ignore-missing -c SHA256SUMS
fi && tar -xzf "antb1-$t.tar.gz" && ./antb1-*/antb1 version --verbose
```
<!-- install:end -->

- **The binary** is `antb1-<version>-<target>/antb1`, next to its `LICENSE`, `NOTICE`, `THIRD_PARTY_NOTICES.txt`,
  `BUILDINFO.json` (its commit and how it was built) and `README.txt`. Copy or move it anywhere: it needs no other
  file.
- **Another release:** the commands take the latest release, and a release candidate (`v<version>-rc.<n>`) is a
  prerelease, never the latest. For a given release, put `download/<tag>` in place of `latest/download` in `url`.
- **Provenance:** `SHA256SUMS` only shows that the download is intact. With [gh](https://cli.github.com) logged in,
  the build provenance shows that `release.yml` built the file from the release's tag on a GitHub-hosted runner.
  Give your file and its release's tag; [SECURITY.md](SECURITY.md#verifying-a-release) explains each check, offline
  ones included:

  ```sh
  gh attestation verify antb1-linux-x86_64.tar.gz --repo ydb-campus/antb1 \
    --signer-workflow ydb-campus/antb1/.github/workflows/release.yml \
    --source-ref refs/tags/v0.1.0 --deny-self-hosted-runners
  ```

- **macOS:** `curl` sets no quarantine attribute, so the binary runs. A browser does, and Gatekeeper then refuses
  the binary, which is signed ad hoc and not notarized, until you clear the attribute:
  `xattr -dr com.apple.quarantine antb1-<version>-darwin-arm64`.
- **Debugging (Linux):** `antb1-<target>.debug.tar.gz` holds the line tables, `antb1.debug`. Fetch it like the
  package (`curl -fsSLO "$url/antb1-$t.debug.tar.gz"`), check it against the same `SHA256SUMS` and unpack it in the
  same directory: `antb1.debug` lands next to the binary, where gdb finds it. The binary keeps its symbols for perf.
- **An older CPU** gets a message (`antb1: this CPU lacks ...`) and exit code 70 instead of a crash. In a checkout
  ([Quickstart](#quickstart)), `ANTB1_STATIC_CPU_FLAGS=-march=x86-64 pixi run static` builds a binary for any x86-64
  CPU (AArch64: `-march=armv8-a`).
- **Sources:** each Linux package has `antb1-<target>.sources.tar` next to it in the release, as the LGPL-2.1 of the
  statically linked GNU C Library asks ([License](#license), [ADR 0034](docs/adr/0034-licence-and-static-sources.md)).
  Running the binary does not need it.

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
- [docs/ci.md](docs/ci.md): workflows, required checks, reproducing CI locally, releases
- [docs/benchmarks.md](docs/benchmarks.md): micro benchmarks, ClickBench runs, published results
- [docs/adr/README.md](docs/adr/README.md): architecture decision records
- [SECURITY.md](SECURITY.md): supported versions, verifying a release, reporting a vulnerability

## License

antb1 is licensed under the [Apache License 2.0](LICENSE); see also [NOTICE](NOTICE). The standalone binaries
([ADR 0033](docs/adr/0033-standalone-static-binary.md)) contain third-party libraries, listed with their licences in
each package's `THIRD_PARTY_NOTICES.txt`. The Linux binaries link the GNU C Library statically (LGPL-2.1-or-later),
so each one comes with `antb1-<target>.sources.tar` in the same release or workflow run: the sources of glibc, of
antb1 and of every other library linked into it except the GCC runtime, which comes with the compiler
([ADR 0034](docs/adr/0034-licence-and-static-sources.md)).
