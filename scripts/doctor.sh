#!/usr/bin/env bash
# Prints the environment/build/data status. `--json` for machine-readable output.
set -uo pipefail
json=0
[ "${1:-}" = "--" ] && shift
[ "${1:-}" = "--json" ] && json=1
root="${PIXI_PROJECT_ROOT:-$(pwd)}"
pixi_version=$(pixi --version 2>/dev/null | awk '{print $2}')
cxx=$(${CXX:-c++} --version 2>/dev/null | head -1)
cmake_v=$(cmake --version 2>/dev/null | head -1 | awk '{print $3}')
# The environment's own header first, and pkg-config only as a fallback. The other order reports the
# *host's* Arrow: pkg-config searches its default path before the environment's, so on a machine with
# Arrow installed by Homebrew it answered 26.0.0 for a build linking the environment's 25.0.0 --
# which is the one number here nobody can sanity-check against something else.
arrow_v=$(awk -F'"' '/ARROW_VERSION_STRING/{print $2}' "${CONDA_PREFIX:-}/include/arrow/util/config.h" 2>/dev/null)
[ -n "$arrow_v" ] || arrow_v=$(pkg-config --modversion arrow 2>/dev/null)
envs=""
for d in "$root"/.pixi/envs/*/; do
  [ -d "$d" ] && envs="${envs:+$envs }$(basename "$d")"
done
lock_version=$(head -1 "$root/pixi.lock" 2>/dev/null | awk '{print $2}')
dev_build=$([ -f "$root/build/dev/build.ninja" ] && echo configured || echo not-configured)
last_pass=$([ -f "$root/build/dev/.antb1-last-pass" ] && date -r "$root/build/dev/.antb1-last-pass" -u +%FT%TZ || echo never)
data_dir="${ANTB1_DATA_DIR:-$HOME/.cache/antb1/clickbench}"
hits0=$([ -f "$data_dir/hits_0.parquet" ] && echo present || echo absent)
# TPC's dbgen for the TPC-H-derived test data (ADR 0006), from the pinned duckdb-extension-tpch package.
tpch_ext=""
for f in "${CONDA_PREFIX:-}"/duckdb/extensions/v*/*/tpch.duckdb_extension; do
  [ -f "$f" ] && tpch_ext="$f" && break
done
tpch=$([ -n "$tpch_ext" ] && echo present || echo absent)
# TPC's dsdgen for the TPC-DS-derived test data (ADR 0006), from the pinned duckdb-extension-tpcds package.
tpcds_ext=""
for f in "${CONDA_PREFIX:-}"/duckdb/extensions/v*/*/tpcds.duckdb_extension; do
  [ -f "$f" ] && tpcds_ext="$f" && break
done
tpcds=$([ -n "$tpcds_ext" ] && echo present || echo absent)
if [ $json -eq 1 ]; then
  printf '{"pixi":"%s","env":"%s","envs_installed":"%s","lock_version":"%s","cxx":"%s","cmake":"%s","arrow":"%s","build_dev":"%s","last_full_test_pass":"%s","hits_0":"%s","data_dir":"%s","tpch_extension":"%s","tpcds_extension":"%s"}\n' \
    "$pixi_version" "${PIXI_ENVIRONMENT_NAME:-}" "$envs" "$lock_version" "$cxx" "$cmake_v" "$arrow_v" \
    "$dev_build" "$last_pass" "$hits0" "$data_dir" "$tpch" "$tpcds"
else
  cat <<EOT
pixi            $pixi_version (lock file version $lock_version)
environment     ${PIXI_ENVIRONMENT_NAME:-?}; installed: $envs
compiler        $cxx
cmake           $cmake_v
arrow           $arrow_v
build/dev       $dev_build; last full passing 'pixi run test': $last_pass
hits_0.parquet  $hits0 ($data_dir)
tpch extension  $tpch${tpch_ext:+ ($tpch_ext)}
tpcds extension $tpcds${tpcds_ext:+ ($tpcds_ext)}
EOT
fi
