#!/usr/bin/env bash
# `pixi run static-check [BIN | TARBALL]` (env static): the checks of a standalone antb1.
#   BIN      scripts/static_gates.py binary: linkage (Linux: static ELF without loader or dynamic section, pthread
#            linked whole, no NSS; macOS: only system libraries, no LC_RPATH, deployment target 13.0, a valid
#            signature), build identity, the allocator restart, and smoke tests of the codecs, threads and error exits
#            on Parquet files that DuckDB writes at run time. BIN defaults to the binary that `pixi run static`
#            packaged; for that one the link line in build/static is checked too. Any binary works, e.g. a perf
#            campaign's copy.
#   TARBALL  a package, antb1-<target>.tar.gz with its SHA256SUMS (and on Linux antb1-<target>.debug.tar.gz) next to
#            it (scripts/static_gates.py verify): the package as a user gets it, as the static workflow checks it on
#            fresh runners: the digests and members, then unpacked with tarfile's data filter, BUILDINFO.json, LICENSE
#            and NOTICE against this checkout and the same runs; on Linux also in a container of an image that holds
#            only the binary and the smoke kit, when `docker info` answers.
set -euo pipefail
[ "${1:-}" = "--" ] && shift

root="${PIXI_PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$root"
if [ $# -gt 1 ]; then
  echo "static-check: at most one argument, the binary or a package tarball" >&2
  exit 2
fi
case "${1:-}" in
  *.tar.gz)
    name=$(basename "$1")
    if [[ ! "$name" =~ ^antb1-(linux-x86_64|linux-aarch64|darwin-arm64)\.tar\.gz$ ]]; then
      echo "static-check: $name is not a package: expected antb1-<target>.tar.gz (its .debug tarball comes along)" >&2
      exit 2
    fi
    target="${BASH_REMATCH[1]}"
    dir=$(cd "$(dirname "$1")" && pwd)
    if [ ! -f "$dir/SHA256SUMS" ]; then
      echo "static-check: no SHA256SUMS next to $1: download it with the package" >&2
      exit 2
    fi
    if [[ "$target" == linux-* && ! -f "$dir/antb1-$target.debug.tar.gz" ]]; then
      echo "static-check: no antb1-$target.debug.tar.gz next to $1: a Linux package comes with it" >&2
      exit 2
    fi
    engine=none
    if [ "$(uname -s)" = Linux ] && docker info >/dev/null 2>&1; then
      engine=docker
    else
      echo "static-check: note: no container run (it needs docker on Linux); the binary runs on this host only"
    fi
    kit=$(mktemp -d "${TMPDIR:-/tmp}/antb1-smoke-kit.XXXXXX")
    trap 'rm -rf "$kit"' EXIT
    python scripts/static_gates.py kit --env "$CONDA_PREFIX" --out "$kit"
    status=0
    python scripts/static_gates.py verify --assets "$dir" --sums "$dir/SHA256SUMS" --kit "$kit" --target "$target" \
      --texts "$root" --container "$engine" || status=$?
    exit "$status"
    ;;
esac
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
