#!/usr/bin/env bash
# Claude Code PostToolUse hook for Edit|Write (.claude/settings.json): format the edited file in place with the same
# tools and settings as `pixi run fmt`, and keep .claude/skills in sync after a skill edit. Never blocks: always
# exits 0, and a missing tool just means no formatting (`pixi run lint` still checks everything).
# Self-contained: binaries of the pixi `lint` environment only (no pixi tasks, no pixi solve, no host python).
# Only files inside an antb1 checkout (the main one or a git worktree) that git does not ignore are touched.
set -uo pipefail

input=$(cat 2>/dev/null || true)
bin=""
for d in "${CLAUDE_PROJECT_DIR:-}" "$PWD"; do
  if [ -n "$d" ] && [ -x "$d/.pixi/envs/lint/bin/python" ]; then
    bin="$d/.pixi/envs/lint/bin"
    break
  fi
done
[ -n "$bin" ] || exit 0

f=$(printf '%s' "$input" | "$bin/python" -c '
import json, sys
try:
    print(json.load(sys.stdin).get("tool_input", {}).get("file_path", ""))
except Exception:
    pass
' 2>/dev/null)
[ -n "$f" ] && [ -f "$f" ] || exit 0
dir=$(dirname "$f")
root=$(git -C "$dir" rev-parse --show-toplevel 2>/dev/null) || exit 0
[ -f "$root/.claude/hooks/format-edited.sh" ] || exit 0 # not an antb1 checkout
git -C "$dir" check-ignore -q -- "$(basename "$f")" 2>/dev/null && exit 0 # build/, .pixi/, local files
[ -x "$root/.pixi/envs/lint/bin/python" ] && bin="$root/.pixi/envs/lint/bin" # the checkout's own tool versions
cd "$root" || exit 0
export PATH="$bin:$PATH" # markdownlint-cli2 starts with `#!/usr/bin/env node`: use the env's node

run() { # run <lint-env tool> [args]: quietly, ignoring failures
  local tool=$1
  shift
  if [ -x "$bin/$tool" ]; then "$bin/$tool" "$@" >/dev/null 2>&1; fi
  return 0
}
case "$f" in
  */.claude/skills/*)
    # A generated copy: tell Claude where the canonical file is (additionalContext is non-blocking).
    printf '%s\n' '{"hookSpecificOutput":{"hookEventName":"PostToolUse","additionalContext":".claude/skills is a generated copy of .agents/skills (sync_skills.py, run by pixi run fmt): make skill edits in .agents/skills instead, or pixi run lint fails (R004)."}}'
    ;;
  *.c | *.cc | *.cpp | *.cxx | *.h | *.hh | *.hpp | *.hxx | *.inl | *.ipp | *.tpp) run clang-format -i --style=file "$f" ;;
  */CMakeLists.txt | *.cmake) run gersemi -i --no-cache -q "$f" ;;
  *.py) run ruff format -q "$f" ;; # no `ruff check --fix`: it would drop an import that the next edit uses
  *.toml) run tombi format --offline --quiet "$f" ;;
  *.md | *.markdown)
    run markdownlint-cli2 --fix ":$f"
    case "$f" in */.agents/skills/*) run python tools/lint/sync_skills.py ;; esac
    ;;
  */.agents/skills/*) run python tools/lint/sync_skills.py ;;
esac
exit 0
