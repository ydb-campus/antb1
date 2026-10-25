#!/usr/bin/env bash
# `pixi run static-arrow [--print-key | --print-prefix | --check-urls] [--keep-build]` (NETWORK on the first run):
# builds Apache Arrow 25 and the dependencies it bundles as static libraries, with the Clang of env `static`, -O3,
# ThinLTO and the CPU baseline of cmake/static/toolchain.cmake, into $ANTB1_ARROW_DIR/<key>/prefix (default
# ~/.cache/antb1/arrow; a relative path is relative to the repository root). `pixi run static` and
# `pixi run static-test` run it first and link antb1 against that prefix (docs/adr/0033-standalone-static-binary.md).
#
# - The sources are pinned by sha256 and size in cmake/static/arrow-sources.lock, each with several byte-identical
#   copies, and downloaded once into $ANTB1_ARROW_DIR/src (a file that matches its pin is kept): scripts/static-sources.sh
#   tries a file's URLs in order, each attempt bounded, until one serves the pinned bytes. Arrow then reads every
#   bundled dependency from that local copy (ARROW_<DEP>_URL), so its build needs no network.
# - The key hashes what decides the result: the pins (not their URLs), cmake/static/*, this script, the environment's
#   packages (conda-meta), the target and the CPU overrides. A complete prefix with the same key is reused; a new key
#   always builds from scratch. The prefix survives `pixi run clean`.
# - Before the prefix is marked complete, scripts/static_gates.py checks it: static archives only; every C and C++
#   file of Arrow and of each bundled dependency compiled with -O3 and -flto=thin; no header of a bundled library
#   read from the environment (zstd.h only while it is the pinned version); every download a local file; Arrow's
#   option set; a report of the bitcode and native members of each archive.
#
#   --print-key     print the key and exit (the CI cache key)
#   --print-prefix  print the prefix directory and exit
#   --check-urls    check every URL of every pin against its pin (scripts/static-sources.sh) and exit 1 when one is
#                   dead or serves other bytes, when another failure is still there on a second pass (a timeout or
#                   an error that may pass only warns), or when a file has fewer than two good URLs (CI: on PRs that
#                   change the lock, and nightly); it builds nothing and leaves src alone
#   --keep-build    keep the build tree ($ANTB1_ARROW_DIR/<key>/build) after a successful build
#
#   ANTB1_STATIC_CPU_FLAGS    compiler flags instead of the target's CPU baseline, e.g. "-march=x86-64"
#   ANTB1_STATIC_SIMD_LEVEL   Arrow's ARROW_SIMD_LEVEL instead of the target's (AVX2 on x86-64, DEFAULT elsewhere)
set -euo pipefail
[ "${1:-}" = "--" ] && shift

usage() { awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "${BASH_SOURCE[0]}"; }

print=""
check=0
keep_build=0
while [ $# -gt 0 ]; do
  case "$1" in
    --print-key) print=key ;;
    --print-prefix) print=prefix ;;
    --check-urls) check=1 ;;
    --keep-build) keep_build=1 ;;
    -h | --help) usage; exit 0 ;;
    *) echo "static-arrow: unknown option '$1'" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
lock="$root/cmake/static/arrow-sources.lock"
sources="$root/scripts/static-sources.sh"
if [ "$check" = 1 ]; then
  exec bash "$sources" "$lock" --check-urls
fi
pins=$(bash "$sources" "$lock" --pins) # also validates the lock
if [ -z "${CONDA_PREFIX:-}" ] || [ ! -d "$CONDA_PREFIX/conda-meta" ]; then
  echo "static-arrow: run it through pixi (\`pixi run static-arrow\`): CONDA_PREFIX is not a pixi environment" >&2
  exit 2
fi
if [ -d "$CONDA_PREFIX/include/arrow" ]; then
  echo "static-arrow: $CONDA_PREFIX has conda's Arrow headers, which would shadow the bundled ones;" \
    "run it in env static (\`pixi run static-arrow\`)" >&2
  exit 2
fi

case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) target=linux-x86_64 simd=AVX2 ;;
  Linux-aarch64) target=linux-aarch64 simd=DEFAULT ;;
  Darwin-arm64) target=darwin-arm64 simd=DEFAULT ;;
  *) echo "static-arrow: unsupported platform $(uname -s) $(uname -m)" >&2; exit 2 ;;
esac
simd="${ANTB1_STATIC_SIMD_LEVEL:-$simd}"

arrow_dir="${ANTB1_ARROW_DIR:-$HOME/.cache/antb1/arrow}"
case "$arrow_dir" in
  /*) ;;
  *) arrow_dir="$root/$arrow_dir" ;;
esac
mkdir -p "$arrow_dir"
arrow_dir=$(cd "$arrow_dir" && pwd -P)
root_phys=$(cd "$root" && pwd -P)
case "$arrow_dir/" in
  "$root_phys"/*)
    rel="${arrow_dir#"$root_phys"}"
    rel="${rel#/}"
    if ! git -C "$root_phys" check-ignore -q "${rel:+$rel/}src" 2>/dev/null; then
      echo "static-arrow: ANTB1_ARROW_DIR=$arrow_dir is inside the repository but not ignored by git;" \
        "use a directory under .cache/ or outside the repository" >&2
      exit 2
    fi
    ;;
esac

sha256_of() { cmake -E sha256sum "$1" | awk '{ print $1 }'; }

# The key: one line per input, hashed; 16 hex digits name the prefix directory.
manifest=$(mktemp)
trap 'rm -f "$manifest"' EXIT
{
  echo "target $target"
  echo "cpu-flags ${ANTB1_STATIC_CPU_FLAGS-<default>}"
  echo "simd $simd"
  echo "$pins"
  for f in "$root"/cmake/static/*.cmake "$root/scripts/static-arrow.sh"; do
    echo "file ${f#"$root"/} $(sha256_of "$f")"
  done
  for f in "$CONDA_PREFIX"/conda-meta/*.json; do
    echo "package $(basename "$f" .json)"
  done
} >"$manifest"
key=$(sha256_of "$manifest" | cut -c1-16)
work="$arrow_dir/$key"
prefix="$work/prefix"
case "$print" in
  key) echo "$key"; exit 0 ;;
  prefix) echo "$prefix"; exit 0 ;;
esac

if [ -f "$work/.complete" ]; then
  echo "static-arrow: ok (cached): $prefix"
  exit 0
fi

# ---- sources ----
src="$arrow_dir/src"
bash "$sources" "$lock" --fetch "$src"
arrow_tarball=""
variables=$(bash "$sources" "$lock" --variables "$src")
while IFS='=' read -r var path; do
  if [ "$var" = "-" ]; then
    arrow_tarball=$path
  else
    export "$var=$path"
  fi
done <<<"$variables"

# ---- build ----
rm -rf "$work"
mkdir -p "$work/source"
tar -xzf "$arrow_tarball" -C "$work/source" --strip-components=1
if [ "$target" = darwin-arm64 ]; then
  export MACOSX_DEPLOYMENT_TARGET=13.0 # cmake/static/toolchain.cmake; Arrow does not forward it
fi
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu)
start=$(date +%s)
echo "static-arrow: building Arrow (key $key, $target, ARROW_SIMD_LEVEL=$simd) in $work/build"
cmake -S "$work/source/cpp" -B "$work/build" -G Ninja \
  -C "$root/cmake/static/arrow.cmake" \
  -DCMAKE_TOOLCHAIN_FILE="$root/cmake/static/toolchain.cmake" \
  -DCMAKE_PROJECT_INCLUDE="$root/cmake/static/project-include.cmake" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON \
  -DBoost_ROOT="$CONDA_PREFIX" \
  -DARROW_SIMD_LEVEL="$simd"
cmake --build "$work/build" --parallel "$jobs"
cmake --install "$work/build"

# ---- licences (packaged with the binary by scripts/static.sh) ----
licenses="$prefix/share/antb1-licenses"
mkdir -p "$licenses"
cp "$work/source/LICENSE.txt" "$licenses/arrow-LICENSE.txt"
cp "$work/source/NOTICE.txt" "$licenses/arrow-NOTICE.txt"
# <tarball name> <file inside its top directory>: each bundled library's own licence (lz4: the library's, not the
# programs'). The member is looked up first, so GNU tar and macOS' bsdtar both work.
while read -r name file; do
  out="$licenses/${name%%-[0-9v]*}-$(basename "$file")"
  member=$(tar -tzf "$src/$name" | awk -v f="$file" \
    'm == "" && index($0, "/" f) == length($0) - length(f) && split($0, p, "/") == split(f, q, "/") + 1 { m = $0 }
     END { print m }')
  if [ -z "$member" ] || ! tar -xOzf "$src/$name" "$member" >"$out" || [ ! -s "$out" ]; then
    echo "static-arrow: no $file in $name" >&2
    exit 1
  fi
done <<'EOF'
brotli-v1.0.9.tar.gz LICENSE
lz4-v1.10.0.tar.gz lib/LICENSE
mimalloc-v3.3.1.tar.gz LICENSE
rapidjson-232389d4f1012dddec4ef84861face2d2ba85709.tar.gz license.txt
re2-2023-03-01.tar.gz LICENSE
snappy-1.2.2.tar.gz COPYING
thrift-0.22.0.tar.gz LICENSE
thrift-0.22.0.tar.gz NOTICE
utf8proc-v2.10.0.tar.gz LICENSE.md
xsimd-14.2.0.tar.gz LICENSE
zlib-1.3.1.tar.gz LICENSE
zstd-1.5.7.tar.gz LICENSE
EOF

# ---- gates, then the marker ----
python "$root/scripts/static_gates.py" prefix --build "$work/build" --prefix "$prefix" \
  --env "$CONDA_PREFIX" --lock "$lock" --report "$prefix/share/antb1-lto-report.txt"
cp "$manifest" "$prefix/share/antb1-arrow-key.txt"
if [ "$keep_build" = 0 ]; then
  rm -rf "$work/build" "$work/source"
fi
touch "$work/.complete"
echo "static-arrow: OK: $prefix (key $key, $(($(date +%s) - start)) s)"
