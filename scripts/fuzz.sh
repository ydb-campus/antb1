#!/usr/bin/env bash
# `pixi run fuzz [libFuzzer options]`: a long libFuzzer run of the SQL parser round-trip property
# (fuzz/sql_parser_property.h) on the `fuzz` preset (clang, ASan+UBSan, build/fuzz). `pixi run fuzz-smoke` is the short,
# deterministic CI version.
#
#   ANTB1_FUZZ_SECONDS  total fuzzing time in seconds (default 600)
#   ANTB1_FUZZ_SEED     libFuzzer seed (default: a random one, printed by libFuzzer as "INFO: Seed: <n>")
#
# The work corpus build/fuzz/corpus/sql_parser persists between runs; the committed seeds (fuzz/corpus/sql_parser) are
# read, never modified. Crash, leak and timeout inputs are written to build/fuzz/artifacts/; turn one into a regression
# test with fuzz/regressions/README.md. Extra arguments go to libFuzzer, e.g. -jobs=8 -workers=8 or -max_len=512.
set -euo pipefail
[ "${1:-}" = "--" ] && shift

root="${PIXI_PROJECT_ROOT:-$(pwd)}"
cd "$root"
seconds="${ANTB1_FUZZ_SECONDS:-600}"
if [[ ! "$seconds" =~ ^[1-9][0-9]*$ ]]; then
  echo "fuzz: ANTB1_FUZZ_SECONDS must be a positive integer, got '$seconds'" >&2
  exit 2
fi
seed_args=()
if [ -n "${ANTB1_FUZZ_SEED:-}" ]; then
  if [[ ! "$ANTB1_FUZZ_SEED" =~ ^[0-9]+$ ]]; then
    echo "fuzz: ANTB1_FUZZ_SEED must be a non-negative integer, got '$ANTB1_FUZZ_SEED'" >&2
    exit 2
  fi
  seed_args=("-seed=$ANTB1_FUZZ_SEED")
fi

cmake --preset fuzz
cmake --build --preset fuzz --target antb1-sql-parser-fuzzer

fuzzer="$root/build/fuzz/bin/antb1-sql-parser-fuzzer"
work="$root/build/fuzz/corpus/sql_parser"
artifacts="$root/build/fuzz/artifacts"
mkdir -p "$work" "$artifacts"

# The sanitizer options of the `fuzz` test preset (CMakePresets.json: sanitizer-env); the caller's settings win.
asan="detect_leaks=1:abort_on_error=1:detect_stack_use_after_return=1:strict_string_checks=1"
asan+=":check_initialization_order=1"
export ASAN_OPTIONS="${ASAN_OPTIONS:-$asan}"
ubsan="halt_on_error=1:print_stacktrace=1:suppressions=$root/tools/sanitizers/ubsan.supp"
export UBSAN_OPTIONS="${UBSAN_OPTIONS:-$ubsan}"
export LSAN_OPTIONS="${LSAN_OPTIONS:-suppressions=$root/tools/sanitizers/lsan.supp}"

echo "fuzz: ${seconds}s, work corpus build/fuzz/corpus/sql_parser, artifacts build/fuzz/artifacts/"
cd "$root/build/fuzz" # -jobs writes its fuzz-<n>.log files into the working directory
rc=0
"$fuzzer" \
  -max_total_time="$seconds" \
  -dict="$root/fuzz/sql.dict" \
  -timeout=10 \
  -rss_limit_mb=2048 \
  -artifact_prefix="$artifacts/" \
  -print_final_stats=1 \
  "${seed_args[@]}" \
  "$@" \
  "$work" "$root/fuzz/corpus/sql_parser" || rc=$?

if [ "$rc" -ne 0 ]; then
  {
    echo "fuzz: FAIL (libFuzzer exit $rc). Inputs in build/fuzz/artifacts/:"
    ls -1 "$artifacts" || true
    echo "Reproduce: build/fuzz/bin/antb1-sql-parser-fuzzer build/fuzz/artifacts/<file>"
    echo "Then minimize it and commit it to fuzz/regressions/ with the fix (fuzz/regressions/README.md)."
  } >&2
  exit "$rc"
fi
echo "fuzz: PASS"
