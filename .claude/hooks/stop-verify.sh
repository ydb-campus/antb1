#!/usr/bin/env bash
# Claude Code Stop hook (.claude/settings.json). Active only in cloud sessions (CLAUDE_CODE_REMOTE=true) or with
# ANTB1_STOP_HOOK=1; otherwise it exits 0 at once. It blocks the stop ONCE (exit 2, reason on stderr) when
# build-relevant files changed on this branch (committed or not, since the merge-base with origin/main) were modified
# during this session (after the marker session-start.sh creates; without a marker every such file counts) and are
# newer than the last passing full test run:
#   build/dev/.antb1-last-pass  touched by scripts/ctest.sh after an unfiltered passing `pixi run test`, or
#   build/ci/junit.xml          written by `pixi run ci` / `pixi run check`, if it records no failures.
# Self-contained: git and coreutils only (no pixi tasks, no host python); works with bash 3.2 (macOS).
set -uo pipefail
[ "${CLAUDE_CODE_REMOTE:-}" = true ] || [ "${ANTB1_STOP_HOOK:-}" = 1 ] || exit 0
input=$(cat 2>/dev/null || true)
if printf '%s' "$input" | grep -q '"stop_hook_active" *: *true'; then
  exit 0 # Claude is already continuing because of this hook: never block twice in a row
fi
session_id=$(printf '%s' "$input" | grep -o '"session_id" *: *"[A-Za-z0-9_-]*"' | head -n 1 |
  sed 's/.*"\([A-Za-z0-9_-]*\)"$/\1/')
marker=""
if [ -n "$session_id" ] && [ -f "${TMPDIR:-/tmp}/antb1-claude-session-$session_id" ]; then
  marker="${TMPDIR:-/tmp}/antb1-claude-session-$session_id" # created by session-start.sh
fi
cd "${CLAUDE_PROJECT_DIR:-.}" 2>/dev/null || exit 0
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || exit 0

junit_passed() { # $1: a ctest JUnit file whose <testsuite> (attributes on several lines) has tests and no failures
  local suite
  suite=$(head -c 4096 "$1" 2>/dev/null | tr '\n\t' '  ' | grep -o '<testsuite [^>]*>' | head -n 1)
  [[ $suite =~ \ tests=\"[1-9] ]] && ! [[ $suite =~ \ (failures|errors)=\"[1-9] ]]
}
stamp="" # the newer of the two proofs of a passing full test run
[ -f build/dev/.antb1-last-pass ] && stamp=build/dev/.antb1-last-pass
if junit_passed build/ci/junit.xml && { [ -z "$stamp" ] || [ build/ci/junit.xml -nt "$stamp" ]; }; then
  stamp=build/ci/junit.xml
fi

paths=(src tests tools cmake CMakeLists.txt CMakePresets.json pixi.toml pixi.lock
  ':(exclude)tools/lint' ':(exclude)tools/github')
base=$(git merge-base HEAD origin/main 2>/dev/null || git merge-base HEAD main 2>/dev/null ||
  git rev-parse HEAD 2>/dev/null) || exit 0
changed=$({
  git diff --name-only "$base" -- "${paths[@]}"
  git ls-files --others --exclude-standard -- "${paths[@]}"
} 2>/dev/null | sort -u)
[ -n "$changed" ] || exit 0

newer=()
while IFS= read -r p; do
  [ -f "$p" ] || continue # deleted files have no time stamp
  if [ -n "$marker" ] && ! [ "$p" -nt "$marker" ]; then continue; fi # not modified during this session
  if [ -z "$stamp" ] || [ "$p" -nt "$stamp" ]; then newer+=("$p"); fi
done <<<"$changed"
[ ${#newer[@]} -gt 0 ] || exit 0

list="${newer[*]:0:5}"
[ ${#newer[@]} -le 5 ] || list="$list (+$((${#newer[@]} - 5)) more)"
last=${stamp:-none}
echo "antb1: ${#newer[@]} build-relevant file(s) changed after the last passing full test run ($last): $list." \
  "Run 'pixi run test' (no -R/-L filter) or 'pixi run check', and 'pixi run lint' before you finish;" \
  "if you cannot, say which checks you did not run and why." >&2
exit 2
