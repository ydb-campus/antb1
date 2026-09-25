#!/usr/bin/env bash
# Runs ctest for a test preset with an allowlist of options (so `pixi run test <args>` cannot execute
# arbitrary commands via ctest -S / --build-and-test). Usage: scripts/ctest.sh <preset> [ctest args]
set -uo pipefail
preset="${1:?usage: scripts/ctest.sh <preset> [ctest args]}"
shift
[ "${1:-}" = "--" ] && shift
allowed_with_value=(-R -E -L -j --timeout --repeat)
allowed_flags=(-N -V -VV --rerun-failed --output-on-failure --schedule-random --stop-on-failure)
args=()
filtered=0
while [ $# -gt 0 ]; do
  a="$1"
  if printf '%s\n' "${allowed_with_value[@]}" | grep -qxF -- "$a"; then
    [ $# -ge 2 ] || { echo "ctest.sh: option $a needs a value" >&2; exit 2; }
    args+=("$a" "$2")
    case "$a" in -R|-E|-L|--rerun-failed) filtered=1 ;; esac
    shift 2
  elif printf '%s\n' "${allowed_flags[@]}" | grep -qxF -- "$a"; then
    args+=("$a")
    case "$a" in -N|--rerun-failed) filtered=1 ;; esac
    shift
  else
    echo "ctest.sh: unsupported ctest option '$a' (allowed: ${allowed_with_value[*]} ${allowed_flags[*]})" >&2
    exit 2
  fi
done
build_dir="build/$preset"
ctest --preset "$preset" "${args[@]}"
rc=$?
if [ $rc -eq 0 ] && [ $filtered -eq 0 ]; then
  touch "$build_dir/.antb1-last-pass"
fi
if [ $rc -ne 0 ]; then
  echo "Reproduce: pixi run test --rerun-failed   |   one test: pixi run test -R '^<name>\$' -V" >&2
fi
status=$([ $rc -eq 0 ] && echo PASS || echo FAIL)
echo "ANTB1-TESTS: $status preset=$preset junit=$build_dir/junit.xml"
exit $rc
