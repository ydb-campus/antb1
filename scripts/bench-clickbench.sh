#!/usr/bin/env bash
# `pixi run bench-clickbench [antb1 bench options]`: ClickBench's queries through `antb1 bench` on the Release
# `bench` build -> build/bench/clickbench.json in ClickBench's result format (docs/benchmarks.md). It needs no
# network: the data must already be on disk, outside the repository (never committed, never uploaded).
#
#   ANTB1_DATA_DIR      where the ClickBench files are (default ~/.cache/antb1/clickbench; relative to the repo root)
#   ANTB1_HITS_FILES    the hits Parquet files: a path, a glob or a comma-separated list
#                       (default $ANTB1_DATA_DIR/hits_0.parquet)
#   ANTB1_QUERIES_FILE  ClickBench's duckdb-parquet queries.sql, one query per line
#                       (default $ANTB1_DATA_DIR/queries.sql)
#   ANTB1_BENCH_TRIES   runs of every query (default 3)
#   ANTB1_BENCH_COLD    auto (default): drop the page cache when `sudo -n` works; 0: never (lukewarm runs)
#
# Cold runs, as ClickBench does them: on Linux, when `sudo -n true` works, the page cache is dropped before the first
# run of every query (`antb1 bench --drop-caches`, a machine-wide effect). Otherwise nothing is dropped and the JSON
# says "cache": "lukewarm". Only query numbers, timings and error kinds are printed: never query text or results.
set -euo pipefail
[ "${1:-}" = "--" ] && shift

root="${PIXI_PROJECT_ROOT:-$(pwd)}"
cd "$root"
data_dir="${ANTB1_DATA_DIR:-$HOME/.cache/antb1/clickbench}"
case "$data_dir" in
  /*) ;;
  *) data_dir="$root/$data_dir" ;;
esac
hits="${ANTB1_HITS_FILES:-$data_dir/hits_0.parquet}"
queries="${ANTB1_QUERIES_FILE:-$data_dir/queries.sql}"
tries="${ANTB1_BENCH_TRIES:-3}"
if [[ ! "$tries" =~ ^[1-9][0-9]*$ ]]; then
  echo "bench-clickbench: ANTB1_BENCH_TRIES must be a positive integer, got '$tries'" >&2
  exit 2
fi
if [ ! -f "$queries" ] || { [[ "$hits" != *[*?,[]* ]] && [ ! -f "$hits" ]; }; then
  {
    echo "bench-clickbench: missing ClickBench data: queries '$queries', hits '$hits'."
    echo "Put hits_0.parquet and queries.sql (pinned in docs/benchmarks.md) into $data_dir, or set"
    echo "ANTB1_HITS_FILES and ANTB1_QUERIES_FILE. Never copy them into the repository."
  } >&2
  exit 2
fi

cmake --preset bench
cmake --build --preset bench --target antb1
mkdir -p build/bench
args=(bench --clickbench --queries "$queries" --table "hits=$hits" --tries "$tries"
  --out build/bench/clickbench.json)
if sha=$(git -C "$root" rev-parse HEAD 2>/dev/null); then
  args+=(--git-sha "$sha")
fi
cold="${ANTB1_BENCH_COLD:-auto}"
if [ "$cold" != auto ] && [ "$cold" != 0 ]; then
  echo "bench-clickbench: ANTB1_BENCH_COLD must be auto or 0, got '$cold'" >&2
  exit 2
fi
if [ "$cold" = auto ] && [ "$(uname -s)" = Linux ] && sudo -n true 2>/dev/null; then
  echo "bench-clickbench: cold runs (the page cache is dropped before every query)"
  args+=(--drop-caches)
else
  echo "bench-clickbench: lukewarm runs (the page cache stays: ANTB1_BENCH_COLD=0 or no \`sudo -n\`)"
fi
exec build/bench/bin/antb1 "${args[@]}" "$@"
