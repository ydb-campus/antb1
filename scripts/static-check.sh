#!/usr/bin/env bash
# `pixi run static-check [BIN]` (env static): the checks of a standalone antb1 (scripts/static_gates.py binary):
# linkage (Linux: static ELF without loader or dynamic section, pthread linked whole, no NSS; macOS: only system
# libraries, no LC_RPATH, deployment target 13.0, a valid signature), build identity, the allocator restart, and
# smoke tests of the five codecs, threads and a truncated file on Parquet files that DuckDB writes at run time.
# BIN defaults to the binary that `pixi run static` packaged; for that one the link line in build/static is
# checked too. Any binary works, e.g. a release asset or a perf campaign's copy.
set -euo pipefail
[ "${1:-}" = "--" ] && shift

root="${PIXI_PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$root"
if [ $# -gt 1 ]; then
  echo "static-check: at most one argument, the binary" >&2
  exit 2
fi
args=()
if [ $# -eq 1 ]; then
  bin="$1"
else
  bin=""
  if [ -d build/static/package ]; then
    bin=$(find build/static/package -mindepth 2 -maxdepth 2 -name antb1 -type f | head -1)
  fi
  if [ -z "$bin" ]; then
    echo "static-check: no packaged binary in build/static/package: run \`pixi run static\` or pass one" >&2
    exit 2
  fi
  args=(--build-dir build/static)
fi
exec python scripts/static_gates.py binary --bin "$bin" --env "$CONDA_PREFIX" "${args[@]}"
