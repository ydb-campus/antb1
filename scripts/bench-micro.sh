#!/usr/bin/env bash
# `pixi run bench [Google Benchmark options]`: the micro benchmarks of bench/micro_bench.cc on the Release `bench`
# preset (build/bench). The table goes to stdout, the JSON to build/bench/micro.json (the series that bench.yml
# publishes from main). Extra arguments go to the benchmark binary, e.g. --benchmark_filter=Sum or
# --benchmark_repetitions=5. Numbers never gate a PR (docs/benchmarks.md).
set -euo pipefail
[ "${1:-}" = "--" ] && shift

root="${PIXI_PROJECT_ROOT:-$(pwd)}"
cd "$root"
cmake --preset bench
cmake --build --preset bench --target antb1-bench-micro
build/bench/bin/antb1-bench-micro --benchmark_out=build/bench/micro.json --benchmark_out_format=json "$@"
echo "bench: wrote build/bench/micro.json"
