#!/usr/bin/env bash
# `pixi run static` and `pixi run static-test` (env static; docs/adr/0033-standalone-static-binary.md). It needs a
# complete prefix from `pixi run static-arrow` (both tasks depend on it; the bench scripts run it themselves) and
# configures build/static (preset `static`) against it; nothing else configures that build tree.
#
#   bash scripts/static.sh --build TARGET...
#                          configures build/static and builds only those targets (the bench tasks: antb1,
#                          antb1-bench-micro); nothing is packaged or checked.
#   pixi run static        builds antb1 (Release, -O3, ThinLTO; fully static on Linux) and packages it into
#                          build/static/package: antb1-<target>.tar.gz (the binary, LICENSE, NOTICE, its
#                          THIRD_PARTY_NOTICES.txt, BUILDINFO.json, README.txt), on Linux antb1-<target>.debug.tar.gz
#                          (its line tables, with the same licence texts), and SHA256SUMS. The licence texts come from
#                          scripts/static_package.py (docs/adr/0034-licence-and-static-sources.md); on Linux the
#                          sources that go with the package are `pixi run static-sources`. Then it checks the package
#                          (static_gates.py package) and the packaged file, not the build tree's: static_gates.py
#                          binary (linkage, link flags, identity, allocator restart, smoke tests) and every CLI golden
#                          (ANTB1_CLI_UNDER_TEST).
#   pixi run static-test   builds every target of build/static and runs all hermetic tests against the static
#                          Arrow -> build/static/junit.xml (nightly static-test).
#
# The binary records its commit (antb1 version --verbose): GITHUB_SHA in CI, else git's HEAD, with "-dirty" when the
# work tree has changes.
set -euo pipefail
[ "${1:-}" = "--" ] && shift

test=0
targets=()
case "${1:-}" in
  "") ;;
  --test) test=1 ;;
  --build)
    shift
    targets=("$@")
    if [ "${#targets[@]}" = 0 ]; then
      echo "static: --build needs at least one target" >&2
      exit 2
    fi
    ;;
  *) echo "static: unknown option '$1' (expected none, --test or --build TARGET...)" >&2; exit 2 ;;
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

if [ "${#targets[@]}" != 0 ]; then
  exec cmake --build --preset static --target "${targets[@]}"
fi
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
# The licence texts first, so a binary that fails a check below is kept with them (static.yml). On Linux this also
# refuses a sysroot build whose glibc source is not pinned.
notices="$build/notices"
rm -rf "$notices"
python scripts/static_package.py notices --target "$target" --prefix "$prefix" --env "$CONDA_PREFIX" --out "$notices"
sysroot="" glibc_source=""
if [ "$target" != darwin-arm64 ]; then
  glibc=$(python scripts/static_package.py glibc --env "$CONDA_PREFIX")
  read -r sysroot glibc_source <<<"$glibc"
fi
# The checks that need no run come first: a link that binds the wrong library fails here with the symbols, not
# with a crash at start.
python scripts/static_gates.py binary --bin "$build/bin/antb1" --env "$CONDA_PREFIX" --linkage-only
# The first run of the new binary: a binary that does not start says so here, with its own output and status.
status=0
first_run=$("$build/bin/antb1" version 2>&1) || status=$?
version=$(sed -n 's/^antb1 \([^ ]*\) (Apache Arrow .*)$/\1/p' <<<"$first_run")
if [ "$status" != 0 ] || [ -z "$version" ]; then
  echo "static: $build/bin/antb1 version exited with $status and printed:" >&2
  echo "$first_run" >&2
  exit 1
fi

# ---- package ----
pkg="$build/package"
name="antb1-$version-$target"
rm -rf "$pkg"
mkdir -p "$pkg/$name"
cp "$notices"/* "$pkg/$name/"
if [ "$target" = darwin-arm64 ]; then
  cp -p "$build/bin/antb1" "$pkg/$name/antb1" # stripping would break the linker's ad-hoc signature
else
  # The line tables go to a separate asset, which unpacks next to the binary (its debuglink); the binary keeps its
  # symbols and build-id for perf.
  debug="$pkg/debug/$name"
  mkdir -p "$debug"
  cp "$notices"/* "$debug/"
  llvm-objcopy --only-keep-debug "$build/bin/antb1" "$debug/antb1.debug"
  llvm-objcopy --strip-debug --add-gnu-debuglink="$debug/antb1.debug" "$build/bin/antb1" "$pkg/$name/antb1"
fi

case "$sha" in
  "" | *-dirty) source_url="" ;;
  *) source_url="${GITHUB_SERVER_URL:-https://github.com}/${GITHUB_REPOSITORY:-ydb-campus/antb1}/tree/$sha" ;;
esac
python - "$pkg/$name/BUILDINFO.json" <<EOF
import json, sys
info = {
    "name": "antb1",
    "version": "$version",
    "commit": "$sha",
    "target": "$target",
    "flavor": "static-thinlto",
    "license": "Apache-2.0",
    "source": "$source_url",
    "arrow_prefix": "$(basename "$(dirname "$prefix")")",
    "arrow_sources": "cmake/static/arrow-sources.lock",
    "sysroot": "$sysroot",
    "glibc_source": "$glibc_source",
    "pixi_lock_sha256": "$(cmake -E sha256sum pixi.lock | cut -c1-64)",
    "compiler": "$(clang++ --version | head -1)",
    "runner": "${ImageOS:-$(uname -s)} ${ImageVersion:-$(uname -r)}",
}
for key in ("source", "sysroot", "glibc_source"):  # no source URL for a -dirty build; no glibc on macOS
    if not info[key]:
        del info[key]
json.dump(info, open(sys.argv[1], "w"), indent=2)
EOF
glibc_note=""
if [ -n "$glibc_source" ]; then
  glibc_note="
It contains the GNU C Library (${glibc_source%.src.rpm}), linked statically and licensed under the GNU LGPL 2.1
or later. The sources of glibc, of antb1 and of every other library linked into it, except the GCC runtime
(libstdc++, libgcc) that comes with the compiler, are in antb1-$target.sources.tar, published next to this package;
the README.txt in that file says how to relink."
fi
cat >"$pkg/$name/README.txt" <<EOF
antb1 $version for $target: one standalone executable (no pixi, no repository needed).

  ./antb1 version --verbose      what this build is: commit, flavor, compiler, CPU baseline, Arrow, codecs
  ./antb1 query -c "SELECT COUNT(*) FROM t" --table t=/path/to/file.parquet

On Linux it re-executes itself once at start with MIMALLOC_PURGE_DELAY=-1 (the allocator keeps its memory, ADR
0017). A renamed copy does that too; started through a symlink of another name it does not, and runs about 5%
slower. macOS has no such restart.
The CPU baseline is in \`version --verbose\`; an older CPU gets a message instead of a crash.
Check the download against SHA256SUMS of the same release.

antb1 is licensed under the Apache License 2.0 (LICENSE, NOTICE). THIRD_PARTY_NOTICES.txt lists the libraries
linked into it, with their licences.$glibc_note
EOF

(
  cd "$pkg"
  tar -czf "antb1-$target.tar.gz" "$name"
  assets=("antb1-$target.tar.gz")
  if [ -d debug ]; then
    tar -czf "antb1-$target.debug.tar.gz" -C debug "$name"
    assets+=("antb1-$target.debug.tar.gz")
  fi
  cmake -E sha256sum "${assets[@]}" >SHA256SUMS
)
echo "static: packaged $pkg/antb1-$target.tar.gz"

# ---- check the package and the packaged file ----
python scripts/static_gates.py package --pkg "$pkg" --target "$target"
python scripts/static_gates.py binary --bin "$pkg/$name/antb1" --env "$CONDA_PREFIX" --build-dir "$build"
ANTB1_CLI_UNDER_TEST="$root/$pkg/$name/antb1" ctest --preset static -L '^cli$'
echo "static: OK: $pkg"
