#!/usr/bin/env bash
# `pixi run lint` (CI job "lint (pixi run lint)"). READ-ONLY: format checks, linters, zizmor --offline and the repo
# drift/policy checks (.pre-commit-config.yaml), then the tooling tests (tools/lint/tests, tools/ci/tests). `pixi run fmt` fixes
# formatting. The hooks see every file git tracks plus untracked files that are not ignored (the same set as
# `pixi run fmt` and check_repo.py), so new files are checked before they are committed; in CI that is exactly
# `--all-files`.
set -uo pipefail
cd "${PIXI_PROJECT_ROOT:-$(git rev-parse --show-toplevel)}" || exit 2
export PRE_COMMIT_HOME="${PIXI_PROJECT_ROOT:-$PWD}/.cache/pre-commit"

files=()
while IFS= read -r -d '' f; do
  files+=("$f")
done < <(git ls-files -z --cached --others --exclude-standard)

args=()
# The hooks never modify files, so a diff can only come from a hook that is wrongly not read-only. Locally the work
# tree is usually dirty, and pre-commit would print all of it as "changes made by hooks": show it only in CI.
if [ -n "${CI:-}" ]; then
  args+=(--show-diff-on-failure)
fi
if [ ${#files[@]} -gt 0 ]; then
  args+=(--files "${files[@]}")
else
  args+=(--all-files)
fi

rc=0
pre-commit run "${args[@]}" || rc=1
python -m pytest -q -p no:cacheprovider tools/lint/tests tools/ci/tests || rc=1

if [ $rc -ne 0 ]; then
  echo "lint: FAIL (formatting findings: run \`pixi run fmt\`; everything else needs a manual fix)" >&2
else
  echo "lint: PASS"
fi
exit $rc
