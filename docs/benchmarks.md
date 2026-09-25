# Benchmarks

antb1 has two kinds of benchmarks: Google Benchmark micro benchmarks of the hot paths, and ClickBench runs of the
whole engine through `antb1 bench`. Both are advisory: benchmark numbers never gate a pull request. The micro
benchmark series of `main` is published to the `gh-pages` branch, and a regression of more than 50% gets a commit
comment.

## Micro benchmarks

```bash
pixi run bench                                    # all of them -> build/bench/micro.json
pixi run bench --benchmark_filter=Sum             # a subset; any Google Benchmark option works
pixi run bench --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

`pixi run bench` (`scripts/bench-micro.sh`) configures and builds the `bench` preset (Release, `-Werror`, tests off,
`build/bench`), runs `build/bench/bin/antb1-bench-micro`, prints the table and writes the JSON report to
`build/bench/micro.json`. The benchmarks are in `bench/micro_bench.cc`; their inputs are synthetic (splitmix64 values,
1Mi rows), never ClickBench data.

| Benchmark | Measures |
| --- | --- |
| `BM_ParseSmallAggQuery` | lexing and parsing a small aggregate query with a `WHERE` conjunction (`sql::Parse`) |
| `BM_SumInt16_Exact` | integer `SUM` over a SMALLINT column in antb1's exact 128-bit aggregate state |
| `BM_SumInt16_ArrowKernel` | the same sum with Arrow's `sum` kernel, which wraps at 64 bits and is never used by the engine: the price of exactness |
| `BM_NotEqualTrueCount` | `COUNT(*) ... WHERE x <> 0`: Arrow's `not_equal` kernel and the true count of the selection |
| `BM_Int128AvgAccumulate` | `AVG` over a BIGINT column: exact 128-bit accumulation, one division at the end |
| `BM_ScanColumn` | decoding one SMALLINT column of a Parquet file through `io::ParquetTable::Scan` in 64Ki-row batches |

The `ci-release` preset (`pixi run release`, the `macos-release` CI leg) builds the benchmarks too and runs
`bench.micro.smoke` (label `bench-smoke`): every benchmark for one iteration (`--benchmark_dry_run`), so they keep
building and running on every PR.

## ClickBench runs

`antb1 bench` runs a query file and writes the result JSON of [ClickBench](https://github.com/ClickHouse/ClickBench):

```bash
antb1 bench --queries queries.sql --table hits=/data/hits_0.parquet --clickbench --tries 3 --out result.json
```

- `--queries FILE`: one query per line; blank lines are skipped. Query `Qn` is the n-th non-blank line, counted from
  0 as ClickBench does.
- `--table NAME=PATH[,PATH|GLOB]`, `--clickbench` and `--column-type` work as for `antb1 query`.
- `--tries N` (default 3) runs of every query; `--out FILE` (`-` for stdout) the JSON.
- `--machine TEXT` describes the machine (default: operating system, architecture and CPU count of the host);
  `--git-sha SHA` records the commit of the build.
- `--drop-caches` (Linux) drops the page cache before the first run of every query, with
  `sudo -n sh -c 'sync && echo 3 > /proc/sys/vm/drop_caches'`; without it the runs are lukewarm.

Each run times `Session::Execute` (parse, bind, optimize and execute, without printing the result). `load_time` is the
time to register the tables (reading the Parquet footers) and `data_size` the sum of their Parquet file sizes. A
query that fails gets `null` for every run and is listed in `antb1.failed` with its error kind. Progress goes to
stderr: one line per query with its number and timings, or its error kind; never query text or results.

The JSON (timings shortened):

```json
{
  "system": "antb1",
  "date": "2026-09-25",
  "machine": "Linux x86_64, 4 CPUs",
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
    "git_sha": null,
    "compiler": "Clang 23.1.2",
    "arrow": "25.0.0",
    "build_type": "Release",
    "cache": "lukewarm",
    "tries": 3,
    "batch_size": 65536,
    "failed": [{"query": 1, "kind": "unsupported"}]
  }
}
```

Timings are plain fixed-point seconds. The exit code is 0 when every query ran or failed as outside the subset
(`parse`, `bind` or `unsupported`); any other failure (`io`, `execution`, `internal`) sets the exit code of the first
such failure, after the JSON is written. An unreadable query file, a table that cannot be registered or a page cache
that cannot be dropped stop the run (exit code 3).

### `pixi run bench-clickbench`

`scripts/bench-clickbench.sh` builds `antb1` in the `bench` preset and runs `antb1 bench --clickbench` with 3 runs per
query into `build/bench/clickbench.json`. It needs no network: the ClickBench files must already be on disk, outside
the repository.

| Variable | Default | Meaning |
| --- | --- | --- |
| `ANTB1_DATA_DIR` | `~/.cache/antb1/clickbench` | where the ClickBench files are (a relative path is relative to the repository) |
| `ANTB1_HITS_FILES` | `$ANTB1_DATA_DIR/hits_0.parquet` | the `hits` Parquet files: a path, a glob or a comma-separated list |
| `ANTB1_QUERIES_FILE` | `$ANTB1_DATA_DIR/queries.sql` | ClickBench's DuckDB/Parquet query file |
| `ANTB1_BENCH_TRIES` | `3` | runs of every query |
| `ANTB1_BENCH_COLD` | `auto` | `auto`: cold runs when `sudo -n true` works; `0`: never drop the page cache |

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

## CI: `bench.yml`

| Job | Runs on | What it does |
| --- | --- | --- |
| `bench-run (pixi run bench && pixi run bench-clickbench)` | pushes to `main` that touch `src/`, `bench/`, `tools/fixturegen/` or `pixi.lock`; pull requests with the `performance` label; manual runs | runs the micro benchmarks and, when the ClickBench data is in the CI cache, `bench-clickbench`; writes the micro benchmark table to the job summary and uploads `micro.json` and `clickbench.json` as the `bench-results` artifact (timings only, 30 days). Read-only token |
| `bench-publish` | pushes to `main` only | appends `micro.json` to the `googlecpp` series on the `gh-pages` branch (`dev/bench`) with benchmark-action/github-action-benchmark, comments on the commit when a benchmark gets 50% slower (alert threshold 150%), never fails. The only job with `contents: write`; one publish at a time |

The ClickBench data cache is filled by the data tests on `main`; `bench.yml` only restores it and skips
`bench-clickbench` when it is empty. To compare a change locally, run `pixi run bench` on both commits (or
`--benchmark_repetitions=10` for noisy machines) and compare the two `micro.json` files.
