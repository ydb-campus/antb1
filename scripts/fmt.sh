#!/usr/bin/env bash
# `pixi run fmt`: format files in place with the same tools and settings that `pixi run lint` checks
# (C/C++, CMake, Python, TOML, Markdown). Only files git knows about or would add (tracked + untracked, not ignored).
set -uo pipefail
cd "${PIXI_PROJECT_ROOT:-$(git rev-parse --show-toplevel)}" || exit 2

cxx=() cmake=() py=() toml=() md=()
while IFS= read -r -d '' f; do
  [ -f "$f" ] || continue # deleted but still tracked
  case "$f" in
    pixi.lock | .pixi/* | build/* | .cache/*) ;;
    *.c | *.cc | *.cpp | *.cxx | *.h | *.hh | *.hpp | *.hxx | *.inl | *.ipp | *.tpp) cxx+=("$f") ;;
    CMakeLists.txt | */CMakeLists.txt | *.cmake) cmake+=("$f") ;;
    *.py) py+=("$f") ;;
    *.toml) toml+=("$f") ;;
    *.md | *.markdown) md+=("$f") ;;
  esac
done < <(git ls-files -z --cached --others --exclude-standard)

rc=0
run() {
  "$@" || {
    echo "fmt: '$1' reported problems it cannot fix" >&2
    rc=1
  }
}
[ ${#cxx[@]} -eq 0 ] || run clang-format -i --style=file "${cxx[@]}"
[ ${#cmake[@]} -eq 0 ] || run gersemi -i --no-cache -q "${cmake[@]}"
if [ ${#py[@]} -gt 0 ]; then
  run ruff check -q --fix "${py[@]}" # fixes first: removing imports can leave blank lines for the formatter
  run ruff format -q "${py[@]}"
fi
[ ${#toml[@]} -eq 0 ] || run tombi format --offline --quiet "${toml[@]}"
[ ${#md[@]} -eq 0 ] || run markdownlint-cli2 --fix "${md[@]}"
if [ -f tools/lint/sync_skills.py ]; then
  run python tools/lint/sync_skills.py # .agents/skills -> .claude/skills (added with the skills)
fi

if [ $rc -eq 0 ]; then
  echo "fmt: done; review with \`git diff\`"
fi
exit $rc
