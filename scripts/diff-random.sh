#!/usr/bin/env bash
# `pixi run diff-random [antb1-slt diff options]`: the random differential test, antb1 vs the DuckDB
# oracle, on the dev build over the tables of tests/slt/tables.txt. Queries use the features declared
# in tests/slt/supported_features.h; a share of them samples the full target grammar (docs/sql-subset.md)
# to count Unsupported answers. See tests/slt/README.md.
#
#   ANTB1_DIFF_SEED   generator seed (default: a random one, printed first)
#   ANTB1_DIFF_COUNT  number of queries (default 2000)
#   ANTB1_DIFF_ONLY   run only this case: ANTB1_DIFF_SEED=<s> ANTB1_DIFF_ONLY=<case> pixi run diff-random
#
# Extra arguments go to `antb1-slt diff`, e.g. --redact, --table edge, --target-percent 50, --list.
set -euo pipefail
[ "${1:-}" = "--" ] && shift

build=build/dev
seed="${ANTB1_DIFF_SEED:-${SRANDOM:-$RANDOM$RANDOM}}"
count="${ANTB1_DIFF_COUNT:-2000}"
for pair in "ANTB1_DIFF_SEED=$seed" "ANTB1_DIFF_COUNT=$count" "ANTB1_DIFF_ONLY=${ANTB1_DIFF_ONLY:-0}"; do
  if [[ ! "${pair#*=}" =~ ^[0-9]+$ ]]; then
    echo "diff-random: ${pair%%=*} must be a non-negative integer, got '${pair#*=}'" >&2
    exit 2
  fi
done

# The fixtures are cheap to write and must match the generator that was just built.
"$build/bin/antb1-fixturegen" "$build/fixtures" >/dev/null

args=(
  diff
  --fixtures "$build/fixtures"
  --tables tests/slt/tables.txt
  --temp-dir "$build/slt-tmp/diff-random"
  --seed "$seed"
  --count "$count"
)
if [ -n "${ANTB1_DIFF_ONLY:-}" ]; then
  args+=(--only "$ANTB1_DIFF_ONLY")
fi
exec "$build/bin/antb1-slt" "${args[@]}" "$@"
