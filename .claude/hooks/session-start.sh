#!/usr/bin/env bash
# Claude Code SessionStart hook (.claude/settings.json). Never blocks: always exits 0.
# - Cloud sessions (CLAUDE_CODE_REMOTE=true), on startup and resume only: bootstrap pixi and the locked `default` and
#   `lint` environments with scripts/agent-setup.sh (idempotent; it puts pixi on PATH through $CLAUDE_ENV_FILE).
# - On startup and resume: create the session marker that stop-verify.sh compares file times with, so the Stop hook
#   only asks for tests when files changed during the session (not for a branch that was merely checked out).
# - Always: print a short status for Claude's context (stdout). Self-contained: no pixi tasks, no host python.
set -uo pipefail

input=$(cat 2>/dev/null || true)
source_kind=$(printf '%s' "$input" | grep -o '"source" *: *"[a-z]*"' | head -n 1 | sed 's/.*"\([a-z]*\)"$/\1/')
session_id=$(printf '%s' "$input" | grep -o '"session_id" *: *"[A-Za-z0-9_-]*"' | head -n 1 |
  sed 's/.*"\([A-Za-z0-9_-]*\)"$/\1/')
if [ -n "$session_id" ] && { [ "$source_kind" = startup ] || [ "$source_kind" = resume ]; }; then
  marker="${TMPDIR:-/tmp}/antb1-claude-session-$session_id" # same path in stop-verify.sh
  [ -e "$marker" ] || touch "$marker" 2>/dev/null || true
fi
cd "${CLAUDE_PROJECT_DIR:-.}" 2>/dev/null || exit 0

if [ "${CLAUDE_CODE_REMOTE:-}" = true ] && { [ "$source_kind" = startup ] || [ "$source_kind" = resume ]; }; then
  if ! bash scripts/agent-setup.sh --envs "default lint" >&2; then
    echo "antb1: scripts/agent-setup.sh failed; re-run it: bash scripts/agent-setup.sh --envs \"default lint\""
  fi
fi

pixi_bin=$(command -v pixi 2>/dev/null || true)
private_pixi="${ANTB1_PIXI_DIR:-$HOME/.cache/antb1/pixi-0.81.0}/pixi" # where agent-setup.sh installs it
if [ -z "$pixi_bin" ] && [ -x "$private_pixi" ]; then
  pixi_bin=$private_pixi
fi
pixi_state=missing
if [ -n "$pixi_bin" ]; then
  pixi_state=$("$pixi_bin" --version 2>/dev/null | awk '{print $2}')
  pixi_dir=$(dirname "$pixi_bin")
  if [ "$pixi_bin" != "$(command -v pixi 2>/dev/null)" ] && ! grep -qsF "$pixi_dir" "${CLAUDE_ENV_FILE:-/dev/null}"; then
    pixi_state="$pixi_state (not on PATH: export PATH=\"$pixi_dir:\$PATH\")"
  fi
fi
envs=""
for d in .pixi/envs/*/; do
  [ -d "$d" ] && envs="${envs:+$envs,}$(basename "$d")"
done
build=not-configured
[ -f build/dev/build.ninja ] && build=configured
last_pass=never
[ -f build/dev/.antb1-last-pass ] && last_pass=$(date -u -r build/dev/.antb1-last-pass +%FT%TZ 2>/dev/null || echo yes)
branch=$(git branch --show-current 2>/dev/null)

echo "antb1: pixi=${pixi_state:-unknown} envs=[${envs:-none}] build/dev=$build last-full-test-pass=$last_pass" \
  "branch=${branch:-detached}"
if [ "$pixi_state" = missing ]; then
  echo "antb1: pixi is missing: run 'bash scripts/agent-setup.sh' (AGENTS.md, Setup)."
fi
echo "antb1: inner loop 'pixi run test [-R regex]'; before a PR 'pixi run check'; AGENTS.md is the guide."
exit 0
