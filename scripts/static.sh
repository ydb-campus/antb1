#!/usr/bin/env bash
# `pixi run static` and `pixi run static-test` (env static; docs/adr/0033-standalone-static-binary.md). Both run
# `pixi run static-arrow` first and configure build/static (preset `static`) against the prefix it built.
#
#   pixi run static        builds antb1 (Release, -O3, ThinLTO; fully static on Linux) and packages it into
#                          build/static/package: antb1-<target>.tar.gz (the binary, its third-party notices,
#                          BUILDINFO.json, README.txt), on Linux antb1-<target>.debug.tar.gz (its line tables), and
#                          SHA256SUMS. Then it checks the packaged file, not the build tree's: static_gates.py
#                          (linkage, link flags, identity, allocator restart, smoke tests) and every CLI golden
#                          (ANTB1_CLI_UNDER_TEST).
#   pixi run static-test   builds every target of build/static and runs all hermetic tests against the static
#                          Arrow -> build/static/junit.xml (nightly static-test).
#
# The binary records its commit (antb1 version --verbose): GITHUB_SHA in CI, else git's HEAD, with "-dirty" when the
# work tree has changes.
set -euo pipefail
[ "${1:-}" = "--" ] && shift

test=0
case "${1:-}" in
  "") ;;
  --test) test=1 ;;
  *) echo "static: unknown option '$1' (expected none, or --test)" >&2; exit 2 ;;
esac

root="${PIXI_PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$root"
prefix=$(bash scripts/static-arrow.sh --print-prefix)
if [ ! -f "$(dirname "$prefix")/.complete" ]; then
  echo "static: no complete Arrow prefix at $prefix: run \`pixi run static-arrow\`" >&2
  exit 2
fi
export ANTB1_ARROW_PREFIX="$prefix"

sha="${GITHUB_SHA:-}"
if [ -z "$sha" ] && sha=$(git rev-parse HEAD 2>/dev/null); then
  if ! git diff --quiet HEAD 2>/dev/null || [ -n "$(git ls-files --others --exclude-standard 2>/dev/null)" ]; then
    sha="$sha-dirty"
  fi
fi
cmake --preset static -DANTB1_GIT_SHA="$sha"

if [ "$test" = 1 ]; then
  cmake --build --preset static
  exec ctest --preset static
fi

cmake --build --preset static --target antb1 antb1-fixturegen
build=build/static
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) target=linux-x86_64 ;;
  Linux-aarch64) target=linux-aarch64 ;;
  Darwin-arm64) target=darwin-arm64 ;;
  *) echo "static: unsupported platform $(uname -s) $(uname -m)" >&2; exit 2 ;;
esac
version=$("$build/bin/antb1" version | sed -n 's/^antb1 \([^ ]*\) (Apache Arrow .*)$/\1/p')
if [ -z "$version" ]; then
  echo "static: cannot read the version of $build/bin/antb1" >&2
  exit 1
fi

# ---- package ----
pkg="$build/package"
name="antb1-$version-$target"
rm -rf "$pkg"
mkdir -p "$pkg/$name"
if [ "$target" = darwin-arm64 ]; then
  cp -p "$build/bin/antb1" "$pkg/$name/antb1" # stripping would break the linker's ad-hoc signature
else
  # The line tables go to a separate asset; the binary keeps its symbols and build-id for perf.
  llvm-objcopy --only-keep-debug "$build/bin/antb1" "$pkg/antb1.debug"
  llvm-objcopy --strip-debug --add-gnu-debuglink="$pkg/antb1.debug" "$build/bin/antb1" "$pkg/$name/antb1"
fi

notices="$pkg/$name/THIRD_PARTY_NOTICES.txt"
licenses=("$prefix"/share/antb1-licenses/* cmake/static/licenses/*)
if [ "$target" != darwin-arm64 ]; then
  sysroot=$(find "$CONDA_PREFIX" -maxdepth 3 -type d -path "*-conda-linux-gnu/sysroot" | head -1)
  licenses+=("$sysroot/usr/share/licenses/glibc/"* "$CONDA_PREFIX/share/licenses/libstdc++/RUNTIME.LIBRARY.EXCEPTION")
fi
{
  echo "antb1 $version ($target) is linked statically with the libraries below; their licences follow."
  echo "Apache Arrow's LICENSE and NOTICE also cover the code it bundles (docs/adr/0033-standalone-static-binary.md)."
} >"$notices"
for f in "${licenses[@]}"; do
  if [ ! -s "$f" ]; then
    echo "static: licence file missing or empty: $f" >&2
    exit 1
  fi
  printf '\n==== %s ====\n\n' "$(basename "$f")" >>"$notices"
  cat "$f" >>"$notices"
done

python - "$pkg/$name/BUILDINFO.json" <<EOF
import json, sys
json.dump({
    "name": "antb1",
    "version": "$version",
    "commit": "$sha",
    "target": "$target",
    "flavor": "static-thinlto",
    "arrow_prefix": "$(basename "$(dirname "$prefix")")",
    "arrow_sources": "cmake/static/arrow-sources.lock",
    "pixi_lock_sha256": "$(cmake -E sha256sum pixi.lock | cut -c1-64)",
    "compiler": "$(clang++ --version | head -1)",
    "runner": "${ImageOS:-$(uname -s)} ${ImageVersion:-$(uname -r)}",
}, open(sys.argv[1], "w"), indent=2)
EOF
cat >"$pkg/$name/README.txt" <<EOF
antb1 $version for $target: one standalone executable (no pixi, no repository needed).

  ./antb1 version --verbose      what this build is: commit, flavor, compiler, CPU baseline, Arrow, codecs
  ./antb1 query -c "SELECT COUNT(*) FROM t" --table t=/path/to/file.parquet

Keep the file name antb1: at start it re-executes itself once with MIMALLOC_PURGE_DELAY=-1 (the allocator keeps
its memory, ADR 0017), and a renamed copy or a symlink of another name skips that and runs about 5% slower.
The CPU baseline is in \`version --verbose\`; an older CPU gets a message instead of a crash.
Check the download against SHA256SUMS of the same release. No licence for antb1 itself is granted yet; the
libraries linked into it are listed with their licences in THIRD_PARTY_NOTICES.txt.
EOF

(
  cd "$pkg"
  tar -czf "antb1-$target.tar.gz" "$name"
  assets=("antb1-$target.tar.gz")
  if [ -f antb1.debug ]; then
    tar -czf "antb1-$target.debug.tar.gz" antb1.debug
    assets+=("antb1-$target.debug.tar.gz")
  fi
  cmake -E sha256sum "${assets[@]}" >SHA256SUMS
)
echo "static: packaged $pkg/antb1-$target.tar.gz"

# ---- check the packaged file ----
python scripts/static_gates.py binary --bin "$pkg/$name/antb1" --env "$CONDA_PREFIX" --build-dir "$build"
ANTB1_CLI_UNDER_TEST="$root/$pkg/$name/antb1" ctest --preset static -L '^cli$'
echo "static: OK: $pkg"
