#!/usr/bin/env bash
# `pixi run antb1 ...`: runs the dev build of the CLI. SQL containing quotes should go through
# `-f file.sql` or `-c -` (stdin): pixi does not escape single quotes in forwarded arguments.
set -euo pipefail
[ "${1:-}" = "--" ] && shift
exec build/dev/bin/antb1 "$@"
