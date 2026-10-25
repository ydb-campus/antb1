#!/usr/bin/env bash
# `pixi run bench [Google Benchmark options]` (env static; NETWORK on the first run, which builds the static Arrow
# with `pixi run static-arrow`): the micro benchmarks of bench/micro_bench.cc in build/static (preset `static`:
# Release, -O3, ThinLTO against the static Arrow; docs/adr/0033-standalone-static-binary.md). The table goes to
# stdout, the JSON to $ANTB1_BENCH_OUT/micro.json (the series that bench.yml publishes from main). Extra arguments go
# to the benchmark binary, e.g. --benchmark_filter=Sum or --benchmark_repetitions=5. Numbers never gate a PR
# (docs/benchmarks.md).
#
#   ANTB1_BENCH_OUT   the results directory (default build/static/bench-results; a relative path is a directory
#                     inside it, so results never land in the work tree, e.g. one per commit)
#
# The micro benchmarks are built here and not shipped, so ANTB1_BENCH_BIN (a given antb1 for bench-clickbench) is
# refused before anything is built.
set -euo pipefail
[ "${1:-}" = "--" ] && shift

root="${PIXI_PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$root"
if [ -n "${ANTB1_BENCH_BIN:-}" ]; then
  echo "bench: ANTB1_BENCH_BIN is for \`pixi run bench-clickbench\`; the micro benchmarks are not in the package" >&2
  exit 2
fi
results="$root/build/static/bench-results"
case "${ANTB1_BENCH_OUT:-}" in
  "") out=$results ;;
  /*) out=$ANTB1_BENCH_OUT ;;
  */../* | ../* | */.. | ..)
    echo "bench: ANTB1_BENCH_OUT=$ANTB1_BENCH_OUT leaves build/static/bench-results; give an absolute path" >&2
    exit 2
    ;;
  *) out="$results/$ANTB1_BENCH_OUT" ;;
esac
if ! mkdir -p "$out" 2>/dev/null || [ ! -w "$out" ]; then
  echo "bench: ANTB1_BENCH_OUT: $out is not a writable directory" >&2
  exit 2
fi
bash scripts/static-arrow.sh
bash scripts/static.sh --build antb1-bench-micro
build/static/bin/antb1-bench-micro --benchmark_out="$out/micro.json" --benchmark_out_format=json "$@"
echo "bench: wrote $out/micro.json"
