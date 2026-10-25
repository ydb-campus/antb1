# Benchmarks

antb1 has two kinds of benchmarks: Google Benchmark micro benchmarks of the hot paths, and ClickBench runs of the
whole engine through `antb1 bench`. Both are advisory: benchmark numbers never gate a pull request. The micro
benchmark series of `main` is published to the `gh-pages` branch, and a regression of more than 50% gets a commit
comment.

**Performance numbers come only from the standalone static build** (`pixi run static`, [ADR 0033](adr/0033-standalone-static-binary.md)):
the micro benchmarks, ClickBench runs, timings of the queries derived from TPC-H and every arm of a campaign.

- Both tasks below build in `build/static`, against the static Arrow, at `-O3` with ThinLTO.
- `bench-clickbench` also runs a given package (`ANTB1_BENCH_BIN`). On a host without the repository, run the
  package directly ([below](#on-a-host-without-the-repository-or-pixi)).
- Profiles (`explain --analyze`) and the dev build's times of the queries derived from TPC-H (`ANTB1_TPCH_TIMES`)
  are diagnostics, for orientation only.
- The dynamic builds (`dev`, `ci-release`) are for tests.

## Micro benchmarks

```bash
pixi run bench                                    # all of them -> build/static/bench-results/micro.json
pixi run bench --benchmark_filter=Sum             # a subset; any Google Benchmark option works
pixi run bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

`pixi run bench` (`scripts/bench-micro.sh`, env `static`):

- **Builds:** it builds the static Arrow when its prefix is missing (`pixi run static-arrow`: network on the first
  run only), then `antb1-bench-micro` in `build/static` (`scripts/static.sh --build`).
- **Runs:** it runs the benchmarks, prints the table and writes the JSON report to `micro.json` in
  `$ANTB1_BENCH_OUT` (default `build/static/bench-results`).
- **Identity:** the report's context names the build: `antb1_commit`, `antb1_flavor`, `antb1_cpu_baseline` and
  `antb1_build_type`.
- **Inputs:** the benchmarks are in `bench/micro_bench.cc`. Their inputs are synthetic (splitmix64 values, 1Mi rows),
  never ClickBench data.
- **Prerequisites:** they need the repository and pixi. They link the environment's shared Google Benchmark and,
  unlike `antb1`, have no CPU check at start.

| Benchmark | Measures |
| --- | --- |
| `BM_ParseSmallAggQuery` | lexing and parsing a small aggregate query with a `WHERE` conjunction (`sql::Parse`) |
| `BM_SumInt16_Exact` | integer `SUM` over a SMALLINT column in antb1's exact 128-bit aggregate state |
| `BM_SumInt16_ArrowKernel` | the same sum with Arrow's `sum` kernel, which wraps at 64 bits and is never used by the engine: the price of exactness |
| `BM_NotEqualTrueCount` | `COUNT(*) ... WHERE x <> 0`: Arrow's `not_equal` kernel and the true count of the selection |
| `BM_Int128AvgAccumulate` | `AVG` over a BIGINT column: exact 128-bit accumulation, one division at the end |
| `BM_ScanColumn` | decoding one SMALLINT column of a Parquet file through `io::ParquetTable::Scan` in 64Ki-row batches |
| `BM_StringFilterAfterScan` | `io::ParquetTable::ScanPart` of a VARCHAR column, then the substring filter over the decoded `BinaryArray` |
| `BM_StringFilterInScan` | the same filter pushed into the scan as a `plan::ScanFilter`, matched on views into the decoded pages (ADR 0020) |
| `BM_SortRows` | `ORDER BY` a random BIGINT over 1Mi rows with a VARCHAR payload, no `LIMIT`: a full sort and the gather of its output (`exec::SortOperator`) |
| `BM_TopNRows` | the same with `LIMIT 10`: the top-N path, which keeps only the best rows while it reads |
| `BM_JoinTableBuild` | building a join hash table (`exec::JoinTableBuilder`, no executor) of 1Mi rows with a BIGINT key and a BIGINT payload in 16 parts: `/0` dense keys (the direct layout), `/1` random keys (the hashed layout); the `direct` counter shows the layout |
| `BM_JoinTableProbe` | `exec::JoinTable::Find` over that table with 1Mi probe keys in 64Ki-row batches, every other one absent: `/0` direct, `/1` hashed; the `matches` counter counts the matches |
| `BM_GroupInt64`, `BM_GroupInt64Int32`, `BM_GroupString`, `BM_GroupInt64String` | grouping every batch with one grouper, as `GroupTable::Consume` does per part, for the four key shapes of ADR 0019's gates (BIGINT; BIGINT and INT; VARCHAR; BIGINT and VARCHAR) at 1000 to 16Mi groups; the `groups` counter shows the count |

`bench.micro.smoke` (label `bench-smoke`) runs every benchmark for one iteration (`--benchmark_dry_run`), so they
keep building and running:

- on every PR, in the `ci-release` preset (`pixi run release`, the `macos-release` CI leg);
- every night, in `static-test`;
- every night on all three static targets: the `static-cold` jobs run `pixi run bench --benchmark_dry_run=true`.

`build/static` is shared with `pixi run static` and `static-test`. Every new commit relinks `antb1` and
`antb1-bench-micro` with ThinLTO, and a `static-test` run after it relinks its 20 test executables too. Never run two
of these tasks at the same time.

## ClickBench runs

`antb1 bench` runs a query file and writes the result JSON of [ClickBench](https://github.com/ClickHouse/ClickBench):

```bash
antb1 bench --queries queries.sql --table hits=/data/hits_0.parquet --clickbench --tries 3 --out result.json
```

- `--queries FILE`: one query per line; blank lines are skipped. Query `Qn` is the n-th non-blank line, counted from
  0 as ClickBench does.
- `--table NAME=PATH[,PATH|GLOB]`, `--clickbench` and `--column-type` work as for `antb1 query`.
- `--tries N` (default 3) runs of every query; `--out FILE` (`-` for stdout) the JSON.
- `--threads N` (default: the hardware threads) threads that run the row groups of every query, as for
  `antb1 query`; the JSON records it.
- `--memory-limit SIZE` (default: 80% of physical memory) as for `antb1 query`; the JSON records it in bytes.
- `--machine TEXT` describes the machine (default: operating system, architecture and CPU count of the host);
  `--git-sha SHA` records another commit than the one built into the binary.
- `--drop-caches` (Linux) drops the page cache before the first run of every query, with
  `sudo -n sh -c 'sync && echo 3 > /proc/sys/vm/drop_caches'`; without it the runs are lukewarm.
- The allocator: on Linux `antb1` runs with `MIMALLOC_PURGE_DELAY=-1` unless it is set (ADR 0017), so freed memory
  stays with the process between runs. Set it (e.g. to `1000`) to measure with mimalloc's default.

Each run times `Session::Execute` (parse, bind, optimize and execute, without printing the result). `load_time` is the
time to register the tables (reading the Parquet footers) and `data_size` the sum of their Parquet file sizes. A
query that fails gets `null` for every run and is listed in `antb1.failed` with its error kind. Progress goes to
stderr: one line per query with its number and timings, or its error kind; never query text or results.

To see where one query's time goes, run it with `antb1 explain --analyze` (same `--table`, `--clickbench`,
`--threads` and `--memory-limit`): every physical operator with its rows, times, part waits and merge phases
([sql-subset.md](sql-subset.md#explain---analyze), [ADR 0015](adr/0015-query-profiles.md)). `--format json` is for
scripts. The plan text shows column names and the query's constants: keep it out of anything committed for
ClickBench queries.

Static glibc has no vDSO, so in the static binary every clock read is a system call:

- **A profile** costs up to about 4% of a query's time on the worst cases measured: many batches, many operators.
- **A `bench` run** reads the clock 7 times per run of a query, about 2.4 µs
  ([ADR 0033](adr/0033-standalone-static-binary.md)).

The JSON (timings shortened):

```json
{
  "system": "antb1",
  "date": "2026-10-25",
  "machine": "Linux x86_64, 128 CPUs",
  "cluster_size": 1,
  "proprietary": "no",
  "hardware": "cpu",
  "tuned": "no",
  "tags": ["C++", "column-oriented", "embedded", "stateless"],
  "load_time": 0.004447,
  "data_size": 2241295,
  "concurrent_qps": null,
  "concurrent_error_ratio": null,
  "result": [
    [0.000185, 0.000073, 0.000066],
    [null, null, null]
  ],
  "antb1": {
    "version": "0.1.0",
    "git_sha": "ccb6f988af20eea7a821d0c0520ed3a46b71fda7",
    "compiler": "Clang 23.1.3",
    "arrow": "25.0.0",
    "build_type": "Release",
    "flavor": "static-thinlto",
    "cpu_baseline": "-march=x86-64-v3",
    "cache": "lukewarm",
    "tries": 3,
    "batch_size": 65536,
    "threads": 16,
    "memory_limit": 54975581388,
    "failed": [{"query": 1, "kind": "unsupported"}]
  }
}
```

`flavor` is `static-thinlto` for the standalone binary (the build every number comes from) and `dynamic` for a build
on conda's shared Arrow; `cpu_baseline` is the binary's CPU baseline (`null`: the compiler's default). Without
`--git-sha`, `git_sha` is the commit built into the binary (with `-dirty` for a build with uncommitted changes), so a
copy on a host without the repository still names its commit. `antb1 version --verbose` prints the same facts, plus
Arrow's own build, the SIMD level in use and the allocator's settings.

Timings are plain fixed-point seconds. The exit code is 0 when every query ran or failed as outside the subset
(`parse`, `bind` or `unsupported`); any other failure (`io`, `execution`, `internal`) sets the exit code of the first
such failure, after the JSON is written. An unreadable query file, a table that cannot be registered or a page cache
that cannot be dropped stop the run (exit code 3).

### `pixi run bench-clickbench`

`scripts/bench-clickbench.sh` (env `static`):

- **What it runs:** `antb1 bench --clickbench` with 3 runs per query. It builds `antb1` in `build/static`
  (`pixi run static-arrow` first, so network on the first run only), unless `ANTB1_BENCH_BIN` names the binary.
- **What it writes,** into `$ANTB1_BENCH_OUT`:
  - `clickbench.json`;
  - `version.txt`: the `antb1 version --verbose` of the binary that ran, kept with the timings because the JSON does
    not record the allocator or the SIMD level.
- **The data:** the ClickBench files must already be on disk, outside the repository.

| Variable | Default | Meaning |
| --- | --- | --- |
| `ANTB1_DATA_DIR` | `~/.cache/antb1/clickbench` | where the ClickBench files are (a relative path is relative to the repository) |
| `ANTB1_HITS_FILES` | `$ANTB1_DATA_DIR/hits_0.parquet` | the `hits` Parquet files: a path, a glob or a comma-separated list |
| `ANTB1_QUERIES_FILE` | `$ANTB1_DATA_DIR/queries.sql` | ClickBench's DuckDB/Parquet query file |
| `ANTB1_BENCH_TRIES` | `3` | runs of every query |
| `ANTB1_BENCH_COLD` | `auto` | `auto`: cold runs when `sudo -n true` works; `0`: never drop the page cache |
| `ANTB1_BENCH_OUT` | `build/static/bench-results` | the results directory (also for `pixi run bench`); a relative path is a directory inside the default one, so results never land in the work tree. Give every arm or commit its own |
| `ANTB1_BENCH_BIN` | none | absolute path of an `antb1` to run instead of building one: a downloaded package or a campaign arm |
| `ANTB1_BENCH_ANY_BUILD` | `0` | `1`: run a binary that is not the standalone static Release build anyway, with a warning |
| `ANTB1_ARROW_DIR` | `~/.cache/antb1/arrow` | where `static-arrow` keeps the static Arrow prefixes |

**A given binary** (`ANTB1_BENCH_BIN`):

- **Nothing is built,** so once env `static` is installed it needs no network.
- **The path must be absolute.** The script runs the file the path resolves to: started through a symlink of
  another name, `antb1` would skip its allocator restart.
- **The check on every run:**
  - `version --verbose` must name the standalone static Release build (flavor `static-thinlto`). Any other build is
    refused unless `ANTB1_BENCH_ANY_BUILD=1`, which is for a deliberate A/B of builds; the JSON records the flavour.
  - On Linux, `mimalloc purge delay: -1` must show, which means the restart happened. Without it the binary runs
    about 5% slower (ADR 0017). A `MIMALLOC_PURGE_DELAY` you set yourself, or a pool other than mimalloc, is printed
    and accepted.
- **Once per downloaded package,** `pixi run static-check <BIN>` checks it fully: linkage, identity, restart and smoke
  tests.

It drops the page cache (cold runs, a machine-wide effect) only on Linux when `sudo -n true` works and
`ANTB1_BENCH_COLD` is not `0`; otherwise the JSON records `"cache": "lukewarm"`. The pinned inputs (the ClickBench
data tests download and verify the same files):

| File | Bytes | sha256 | Source |
| --- | --- | --- | --- |
| `hits_0.parquet` | 122446530 | `fa134fe101e68324e0de851146fda69624f5cbb707d387141d1c2a88a219a16d` | <https://datasets.clickhouse.com/hits_compatible/athena_partitioned/hits_0.parquet> |
| `queries.sql` | 8320 | `e84ea26e1ed13a1dc1e46e76ddc9e49ead67a0444cdd5ae92085daad269c5508` | <https://raw.githubusercontent.com/ClickHouse/ClickBench/5a56398c975bfd9f328f544894bcb92533ed134c/duckdb-parquet/queries.sql> |

For the full dataset (100 partitions, about 14.7 GB, host only), point `ANTB1_HITS_FILES` at all of them, for example
`ANTB1_HITS_FILES="$HOME/.cache/antb1/clickbench/full/hits_*.parquet"`.

Never commit ClickBench data, query text, results or result files, and never upload them as CI artifacts
([ADR 0006](adr/0006-test-strategy-and-data-policy.md)); the result JSON holds only timings, so it may be uploaded.

### On a host without the repository or pixi

The package is one file, so a perf host needs only it, the data and a shell.

1. **The binary.** Fetch the newest package from `main`: a `static.yml` artifact named `antb1-<target>`, with
   targets `linux-x86_64`, `linux-aarch64` and `darwin-arm64`. Artifacts are kept 30 days; GitHub releases follow.
   A Linux run also has `antb1-<target>-sources`, the sources that go with the package
   ([ADR 0034](adr/0034-licence-and-static-sources.md)); a perf host does not need them. Use any machine where `gh`
   is logged in:

   ```bash
   run=$(gh run list -R ydb-campus/antb1 -w static.yml -b main -e push -s success -L 1 --json databaseId -q '.[0].databaseId')
   gh run download "$run" -R ydb-campus/antb1 -n antb1-linux-x86_64 -D pkg
   (cd pkg && grep ' antb1-linux-x86_64.tar.gz$' SHA256SUMS | sha256sum -c -)   # macOS: shasum -a 256 -c -
   tar -xzf pkg/antb1-linux-x86_64.tar.gz
   bin=$(cd antb1-*-linux-x86_64 && pwd)/antb1
   "$bin" version --verbose     # its commit, CPU baseline and "mimalloc purge delay: -1"
   ```

   SHA256SUMS comes from the same run, so it proves the download is intact, not who built it. `version --verbose`
   and the package's BUILDINFO.json name the commit. Fetch with `gh` or `curl`, not a browser: macOS quarantines
   browser downloads.
2. **The data.** Fetch the pinned files of `tools/data/clickbench.lock` at the binary's commit, and check each one's
   size and sha256 before use: a file is kept only when both match. A data directory already checked on another
   host can be copied instead.

   ```bash
   commit=$("$bin" version --verbose | sed -n 's/^commit: //p')
   mkdir -p data
   curl -fsSL --proto '=https' -o clickbench.lock \
     "https://raw.githubusercontent.com/ydb-campus/antb1/${commit%-dirty}/tools/data/clickbench.lock"
   grep -vE '^(#|$)' clickbench.lock | while read -r name sha bytes url; do
     part="data/$name.part"
     if curl -fL --proto '=https' --proto-redir '=https' --retry 5 -o "$part" "$url" &&
       [ "$(wc -c <"$part" | tr -d ' ')" = "$bytes" ] &&
       echo "$sha  $part" | sha256sum -c - >/dev/null; then # macOS: shasum -a 256 -c -
       mv "$part" "data/$name" && echo "data/$name: OK"
     else
       rm -f "$part" && echo "data/$name: FAILED (download, size or sha256): do not use it" >&2
     fi
   done
   ```

3. **The run,** with the settings of `pixi run bench-clickbench`: 3 tries, and cold only where `--drop-caches` may
   drop the machine's page cache. In a container, give `--threads` and `--memory-limit` explicitly: the defaults read
   the host, not the container's limits. On a shared host, run lukewarm.

   ```bash
   "$bin" version --verbose >version.txt
   "$bin" bench --clickbench --queries data/queries.sql --table hits=data/hits_0.parquet --tries 3 \
     --threads 16 --out clickbench.json
   ```

Keep the package, SHA256SUMS, version.txt and the result JSON together. Only the timings may be shared; the data,
the query text and query results stay on the host.

### The series break of 2026-10-25

Until 2026-10-25 every number came from the dynamic preset `bench`, which has since been removed: Release, `-O3` and
`-Werror` on conda's shared Arrow (GCC, `-O2`), without LTO across antb1 and Arrow. Its micro series stays on
gh-pages as <https://ydb-campus.github.io/antb1/dev/bench/>, no longer extended. The static build's series starts
with this change at <https://ydb-campus.github.io/antb1/dev/bench-static/>. Do not read one series against the other
point by point.

The A/B that decided the move (ADR 0033): with ClickBench on the 100 files, the static binary took 0.822 of the
dynamic build's summed hot time at 1 thread, 0.854 at 16, 0.940 at 64 and 0.950 at 128, with every answer equal.

The micro series' own break, measured on 2026-10-25:

- **Setup:** both micro benchmark binaries built from the same commit, on the two-socket Xeon Gold 6338 host, pinned
  to one NUMA node.
- **Runs:** four runs each in ABBA order behind the host's idle gate, 5 repetitions per run. Every run was clean.
- **Reading the table:** each ratio is the static build's median time over the dynamic one's; below 1 is faster. Run
  to run a benchmark's median moved by 1.1% (the median over benchmarks; at most 5.9%). The geometric mean over the
  38 benchmarks is 0.919.

| Benchmark | static / dynamic |
| --- | --- |
| `BM_ParseSmallAggQuery` | 0.924 |
| `BM_SumInt16_Exact` | 0.690 |
| `BM_SumInt16_ArrowKernel` | 0.878 |
| `BM_NotEqualTrueCount` | 0.571 |
| `BM_Int128AvgAccumulate` | 1.377 (slower) |
| `BM_ScanColumn` | 0.566 |
| `BM_StringFilterAfterScan` | 0.893 |
| `BM_StringFilterInScan` | 0.994 |
| `BM_SortRows` | 1.018 |
| `BM_TopNRows` | 0.982 |
| `BM_JoinTableBuild` `/0`, `/1` | 1.009, 0.982 |
| `BM_JoinTableProbe` `/0`, `/1` | 0.929, 1.012 |
| `BM_GroupInt64` at 1000, 16384, 262144, 1Mi, 4Mi and 16Mi groups | 0.981, 0.974, 0.945, 0.880, 0.842, 0.873 |
| `BM_GroupInt64Int32`, the same counts | 0.953, 0.947, 0.929, 0.885, 0.865, 0.883 |
| `BM_GroupString`, the same counts | 1.011, 1.005, 0.988, 0.930, 0.900, 0.911 |
| `BM_GroupInt64String`, the same counts | 0.985, 0.981, 0.974, 0.954, 0.933, 0.940 |

The Arrow kernels and the scan gain most: Arrow is built with Clang at `-O3` for x86-64-v3, and ThinLTO crosses into
antb1. `BM_Int128AvgAccumulate` is the one clear loss, 1.38 times slower and steady across runs, and needs its own
look. Large group counts gain 6-16%.

## Queries derived from TPC-H

No benchmark runs them yet. When one does, it uses the static build, and its numbers (per-query seconds and their
geometric mean) may be published like the ClickBench timings, but never as TPC-H Benchmark results: no TPC metrics
(QphH, QppH, QthH, $/kQphH), no comparison with official TPC results, the deviations and the machine listed, and next
to them this disclaimer ([ADR 0006](adr/0006-test-strategy-and-data-policy.md)): "This workload is derived from the
TPC-H Benchmark and is not comparable to published TPC-H Benchmark results, as this implementation does not comply
with all requirements of the TPC-H Benchmark."

## CI: `bench.yml`

| Job | Runs on | What it does |
| --- | --- | --- |
| `bench-run (pixi run bench && pixi run bench-clickbench)` | pushes to `main` that touch `src/`, `bench/`, `tools/fixturegen/`, `cmake/`, `CMakeLists.txt`, `CMakePresets.json`, `pixi.toml`, `pixi.lock`, the static and bench scripts or `bench.yml`; pull requests with the `performance` label; manual runs | in env `static`, restores `main`'s static Arrow prefix read-only. On a miss (a key `main` has not built yet), `static-arrow` builds it cold in the job, about 8 minutes, and nothing saves it. Runs the micro benchmarks and, when the ClickBench data is in the CI cache, `bench-clickbench`. The job summary gets the CPU, whether the prefix came from the cache, `version.txt` and the micro table. Uploads `build/static/bench-results` (`micro.json`, `clickbench.json`, `version.txt`: timings and build facts) as the `bench-results` artifact (30 days). Read-only token |
| `bench-publish` | pushes to `main` only | appends `micro.json` to the `googlecpp` series on the `gh-pages` branch (`dev/bench-static`) with benchmark-action/github-action-benchmark, comments on the commit when a benchmark gets 50% slower (alert threshold 150%), never fails. The only job with `contents: write`; one publish at a time |

Points on `dev/bench-static` can come from different runner CPUs (the summary names each one), and two pushes can
publish in the other order when one waits for a cold prefix. Only the `performance` label starts or cancels a run on
a pull request; other labels do neither.

The ClickBench data cache is filled by the data tests on `main`; `bench.yml` only restores it and skips
`bench-clickbench` when it is empty. To compare a change locally, run `pixi run bench` on both commits, each with its
own `ANTB1_BENCH_OUT` (and `--benchmark_repetitions=10` on a noisy machine), then compare the two `micro.json` files.
