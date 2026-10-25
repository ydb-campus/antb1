#!/usr/bin/env bash
# `pixi run bench-clickbench [antb1 bench options]` (env static): ClickBench's queries through `antb1 bench` on the
# standalone static binary (docs/adr/0033-standalone-static-binary.md) -> $ANTB1_BENCH_OUT/clickbench.json in
# ClickBench's result format, with version.txt (`version --verbose` of the binary that ran) next to it
# (docs/benchmarks.md). The data must already be on disk, outside the repository (never committed, never uploaded).
# It builds antb1 in build/static (NETWORK on the first run, which builds the static Arrow with
# `pixi run static-arrow`), unless ANTB1_BENCH_BIN names the binary to run.
#
#   ANTB1_DATA_DIR         where the ClickBench files are (default ~/.cache/antb1/clickbench; relative to the repo root)
#   ANTB1_HITS_FILES       the hits Parquet files: a path, a glob or a comma-separated list
#                          (default $ANTB1_DATA_DIR/hits_0.parquet)
#   ANTB1_QUERIES_FILE     ClickBench's duckdb-parquet queries.sql, one query per line
#                          (default $ANTB1_DATA_DIR/queries.sql)
#   ANTB1_BENCH_TRIES      runs of every query (default 3)
#   ANTB1_BENCH_COLD       auto (default): drop the page cache when `sudo -n` works; 0: never (lukewarm runs)
#   ANTB1_BENCH_OUT        the results directory (default build/static/bench-results; a relative path is a
#                          directory inside it, so results never land in the work tree, e.g. one per arm)
#   ANTB1_BENCH_BIN        absolute path of an antb1 to run instead of building one (a downloaded package, a campaign
#                          arm): nothing is built and nothing is downloaded
#   ANTB1_BENCH_ANY_BUILD  1: run a binary that is not the standalone static Release build anyway, with a warning
#                          (a deliberate flavour A/B; the JSON records the flavour); default 0: refuse it
#
# The binary must be the standalone static Release build (`version --verbose`: flavor static-thinlto), and on Linux
# its allocator restart must have happened ("mimalloc purge delay: -1", ADR 0017): without it antb1 runs about 5%
# slower. A user's own MIMALLOC_PURGE_DELAY, or a pool other than mimalloc, is printed and accepted.
#
# Cold runs, as ClickBench does them: on Linux, when `sudo -n true` works, the page cache is dropped before the first
# run of every query (`antb1 bench --drop-caches`, a machine-wide effect). Otherwise nothing is dropped and the JSON
# says "cache": "lukewarm". Only query numbers, timings and error kinds are printed: never query text or results.
set -euo pipefail
[ "${1:-}" = "--" ] && shift

root="${PIXI_PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$root"
in_root() {
  case "$1" in
    /*) echo "$1" ;;
    *) echo "$root/$1" ;;
  esac
}
data_dir=$(in_root "${ANTB1_DATA_DIR:-$HOME/.cache/antb1/clickbench}")
hits="${ANTB1_HITS_FILES:-$data_dir/hits_0.parquet}"
queries="${ANTB1_QUERIES_FILE:-$data_dir/queries.sql}"
tries="${ANTB1_BENCH_TRIES:-3}"
results="$root/build/static/bench-results"
case "${ANTB1_BENCH_OUT:-}" in
  "") out=$results ;;
  /*) out=$ANTB1_BENCH_OUT ;;
  */../* | ../* | */.. | ..)
    echo "bench-clickbench: ANTB1_BENCH_OUT=$ANTB1_BENCH_OUT leaves build/static/bench-results; give an absolute path" >&2
    exit 2
    ;;
  *) out="$results/$ANTB1_BENCH_OUT" ;;
esac
any_build="${ANTB1_BENCH_ANY_BUILD:-0}"
cold="${ANTB1_BENCH_COLD:-auto}"
if [[ ! "$tries" =~ ^[1-9][0-9]*$ ]]; then
  echo "bench-clickbench: ANTB1_BENCH_TRIES must be a positive integer, got '$tries'" >&2
  exit 2
fi
if [ "$any_build" != 0 ] && [ "$any_build" != 1 ]; then
  echo "bench-clickbench: ANTB1_BENCH_ANY_BUILD must be 0 or 1, got '$any_build'" >&2
  exit 2
fi
if [ "$cold" != auto ] && [ "$cold" != 0 ]; then
  echo "bench-clickbench: ANTB1_BENCH_COLD must be auto or 0, got '$cold'" >&2
  exit 2
fi
missing=""
IFS=, read -r -a hits_list <<<"$hits"
for h in "${hits_list[@]}"; do
  [[ "$h" == *[*?[]* ]] || [ -f "$h" ] || missing="$h" # a glob is antb1's to expand
done
if [ ! -f "$queries" ] || [ -n "$missing" ]; then
  {
    echo "bench-clickbench: missing ClickBench data: queries '$queries', hits '$hits'."
    echo "Put hits_0.parquet and queries.sql (pinned in docs/benchmarks.md) into $data_dir, or set"
    echo "ANTB1_HITS_FILES and ANTB1_QUERIES_FILE. Never copy them into the repository."
  } >&2
  exit 2
fi
if ! mkdir -p "$out" 2>/dev/null || [ ! -w "$out" ]; then
  echo "bench-clickbench: ANTB1_BENCH_OUT: $out is not a writable directory" >&2
  exit 2
fi

if [ -n "${ANTB1_BENCH_BIN:-}" ]; then
  case "$ANTB1_BENCH_BIN" in
    /*) ;;
    *) echo "bench-clickbench: ANTB1_BENCH_BIN must be an absolute path, got '$ANTB1_BENCH_BIN'" >&2; exit 2 ;;
  esac
  if [ ! -f "$ANTB1_BENCH_BIN" ] || [ ! -x "$ANTB1_BENCH_BIN" ]; then
    echo "bench-clickbench: ANTB1_BENCH_BIN=$ANTB1_BENCH_BIN is not an executable file" >&2
    exit 2
  fi
  # Run the file itself: started through a symlink of another name, antb1 skips its allocator restart.
  bin=$(readlink -f "$ANTB1_BENCH_BIN")
else
  bash scripts/static-arrow.sh
  bash scripts/static.sh --build antb1
  bin="$root/build/static/bin/antb1"
fi

if ! version=$("$bin" version --verbose 2>&1); then
  echo "bench-clickbench: $bin version --verbose failed:" >&2
  echo "$version" >&2
  exit 2
fi
field() { sed -n "s/^$1: //p" <<<"$version" | head -n 1; }
flavor=$(field flavor)
build_type=$(field "build type")
if [ "$flavor" != static-thinlto ] || [ "$build_type" != Release ]; then
  what="$bin is not the standalone static Release build (flavor '${flavor:-?}', build type '${build_type:-?}');"
  what="$what performance numbers come only from that build (docs/benchmarks.md)"
  if [ "$any_build" != 1 ]; then
    echo "bench-clickbench: $what. ANTB1_BENCH_ANY_BUILD=1 runs it anyway." >&2
    exit 2
  fi
  echo "bench-clickbench: warning: $what; running it anyway (ANTB1_BENCH_ANY_BUILD=1)" >&2
fi
if [ "$(uname -s)" = Linux ]; then
  purge=$(field "mimalloc purge delay")
  pool="${ARROW_DEFAULT_MEMORY_POOL:-}"
  if [ -n "${MIMALLOC_PURGE_DELAY:-}" ] || { [ -n "$pool" ] && [ "$pool" != mimalloc ]; }; then
    echo "bench-clickbench: allocator settings from the environment: MIMALLOC_PURGE_DELAY=${MIMALLOC_PURGE_DELAY:-}" \
      "ARROW_DEFAULT_MEMORY_POOL=$pool (mimalloc purge delay in effect: ${purge:-none})"
  elif [ "$purge" != -1 ]; then
    {
      echo "bench-clickbench: $bin did not restart for its allocator (mimalloc purge delay '${purge:-?}', not -1;"
      echo "an emulator, or no /proc?), so it would run about 5% slower (ADR 0017). Set MIMALLOC_PURGE_DELAY=-1"
      echo "to run it with the setting the restart would have made."
    } >&2
    exit 2
  fi
fi

printf '%s\n' "$version" >"$out/version.txt"
args=(bench --clickbench --queries "$queries" --table "hits=$hits" --tries "$tries" --out "$out/clickbench.json")
if [ "$cold" = auto ] && [ "$(uname -s)" = Linux ] && sudo -n true 2>/dev/null; then
  echo "bench-clickbench: cold runs (the page cache is dropped before every query)"
  args+=(--drop-caches)
else
  echo "bench-clickbench: lukewarm runs (the page cache stays: ANTB1_BENCH_COLD=0 or no \`sudo -n\`)"
fi
echo "bench-clickbench: $bin (commit $(field commit), $flavor) -> $out"
exec "$bin" "${args[@]}" "$@"
