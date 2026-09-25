#!/usr/bin/env bash
# Applies the antb1 GitHub configuration (docs/adr/0009-ci-and-governance.md) with `gh api`:
# repository, security and Actions settings, the `automation` environment, labels, gh-pages and the `main` ruleset.
#
# DRY RUN BY DEFAULT: every mutating command is printed, nothing is changed. Pass --apply to execute them.
# Reads (GET) always run, never fail the script and show the current state; they print "(not available yet)"
# when something does not exist yet or gh lacks access.
#
# Usage (repository root, repository admin, authenticated gh; python3 for labels.json):
#   bash tools/github/apply-settings.sh [--apply] [--repo OWNER/NAME]               settings, environment, labels
#   bash tools/github/apply-settings.sh [--apply] [--repo OWNER/NAME] --gh-pages    orphan gh-pages + Pages
#   bash tools/github/apply-settings.sh [--apply] [--repo OWNER/NAME] --ruleset A|B the `main` ruleset
# Order (rollout): settings on the empty repo -> root commit on main -> --gh-pages -> --ruleset A -> PR #1a-#1c
# -> checks `CI OK`, `PR title` and CodeQL have reported on main -> --ruleset B.
# Reading the org Actions policy needs `gh auth refresh -h github.com -s admin:org`.
set -euo pipefail

repo=ydb-campus/antb1
apply=0
mode=settings
stage=""
description="Experimental C++23 analytics engine: SQL-like queries over local Parquet files (Apache Arrow)"
topics=(cpp cpp23 parquet apache-arrow analytics query-engine clickbench pixi)

usage() { awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "${BASH_SOURCE[0]}"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --apply) apply=1; shift ;;
    --repo)
      if [ $# -lt 2 ] || [ -z "$2" ]; then echo "apply-settings: --repo needs OWNER/NAME" >&2; exit 2; fi
      repo=$2; shift 2 ;;
    --gh-pages) [ "$mode" = settings ] || { echo "apply-settings: choose one of --gh-pages, --ruleset" >&2; exit 2; }
      mode=gh-pages; shift ;;
    --ruleset) [ "$mode" = settings ] || { echo "apply-settings: choose one of --gh-pages, --ruleset" >&2; exit 2; }
      mode=ruleset; stage="${2:-}"; shift $(($# >= 2 ? 2 : 1)) ;;
    -h|--help) usage; exit 0 ;;
    *) echo "apply-settings: unknown option '$1'" >&2; usage >&2; exit 2 ;;
  esac
done
if [ "$mode" = ruleset ] && [ "$stage" != A ] && [ "$stage" != B ]; then
  echo "apply-settings: --ruleset needs A or B" >&2
  exit 2
fi
case "$repo" in */*) ;; *) echo "apply-settings: --repo must be OWNER/NAME" >&2; exit 2 ;; esac
org=${repo%%/*}

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"

have_gh=0
if command -v gh >/dev/null 2>&1; then have_gh=1; fi
if [ "$apply" = 1 ] && [ "$have_gh" = 0 ]; then
  echo "apply-settings: gh (GitHub CLI) is required for --apply" >&2
  exit 1
fi

failures=0

# Prints a command with shell quoting that can be copied and pasted.
quote() {
  local out="" a safe='^[A-Za-z0-9_./:=@,+%-]+$'
  for a in "$@"; do
    if [[ $a =~ $safe ]]; then out+="$a "; else out+="'${a//\'/\'\\\'\'}' "; fi
  done
  printf '%s' "${out% }"
}

# Mutating command: printed in a dry run, executed with --apply (a failure is counted, the script continues).
run() {
  if [ "$apply" = 1 ]; then
    printf '+ %s\n' "$(quote "$@")"
    if ! "$@"; then
      echo "apply-settings: FAILED: $(quote "$@")" >&2
      failures=$((failures + 1))
    fi
  else
    printf 'DRY-RUN: %s\n' "$(quote "$@")"
  fi
}

# Tolerant read: `show LABEL gh-api-args...` prints the result or "(not available yet)".
show() {
  local label=$1 out
  shift
  if [ "$have_gh" = 1 ] && out=$(gh api "$@" 2>/dev/null); then
    printf '  %s: %s\n' "$label" "${out:-(empty)}"
  else
    printf '  %s: (not available yet)\n' "$label"
  fi
}

# Tolerant read that returns only the value (empty on any error; gh prints error bodies to stdout).
get() {
  [ "$have_gh" = 1 ] || return 0
  local out
  if out=$(gh api "$@" 2>/dev/null); then printf '%s\n' "$out"; fi
}

section() { printf '\n== %s\n' "$1"; }

settings() {
  section "prerequisites (reads; the org must allow repo-level 'selected' actions and SHA pinning)"
  show "org Actions permissions" "orgs/$org/actions/permissions"
  show "org workflow token defaults" "orgs/$org/actions/permissions/workflow"

  section "repository: squash only, auto-merge, update branch, no wiki, description, topics"
  run gh api --silent -X PATCH "repos/$repo" \
    -f "description=$description" \
    -F allow_squash_merge=true -F allow_merge_commit=false -F allow_rebase_merge=false \
    -f squash_merge_commit_title=PR_TITLE -f squash_merge_commit_message=PR_BODY \
    -F delete_branch_on_merge=true -F allow_auto_merge=true -F allow_update_branch=true -F has_wiki=false
  local topic_args=() t
  for t in "${topics[@]}"; do topic_args+=(-f "names[]=$t"); done
  run gh api --silent -X PUT "repos/$repo/topics" "${topic_args[@]}"

  section "security: secret scanning + push protection, Dependabot alerts + fixes, private vulnerability reporting"
  run gh api --silent -X PATCH "repos/$repo" \
    -f 'security_and_analysis[secret_scanning][status]=enabled' \
    -f 'security_and_analysis[secret_scanning_push_protection][status]=enabled'
  run gh api --silent -X PUT "repos/$repo/vulnerability-alerts"
  run gh api --silent -X PUT "repos/$repo/automated-security-fixes"
  run gh api --silent -X PUT "repos/$repo/private-vulnerability-reporting"
  show "CodeQL default setup (must stay not-configured: codeql.yml is the advanced setup)" \
    "repos/$repo/code-scanning/default-setup" --jq .state

  section "actions: selected + SHA-pinned actions, read-only token, no PR approvals, fork approval for outsiders"
  run gh api --silent -X PUT "repos/$repo/actions/permissions" \
    -F enabled=true -f allowed_actions=selected -F sha_pinning_required=true
  run gh api --silent -X PUT "repos/$repo/actions/permissions/selected-actions" \
    --input tools/github/allowed-actions.json
  run gh api --silent -X PUT "repos/$repo/actions/permissions/workflow" \
    -f default_workflow_permissions=read -F can_approve_pull_request_reviews=false
  run gh api --silent -X PUT "repos/$repo/actions/permissions/fork-pr-contributor-approval" \
    -f approval_policy=all_external_contributors

  section "environment 'automation' (bot App secrets; deployable from main only)"
  run gh api --silent -X PUT "repos/$repo/environments/automation" \
    -F 'deployment_branch_policy[protected_branches]=false' \
    -F 'deployment_branch_policy[custom_branch_policies]=true'
  local policy
  policy=$(get "repos/$repo/environments/automation/deployment-branch-policies" \
    --jq '.branch_policies[] | select(.name == "main" and .type == "branch") | .id')
  if [ -n "$policy" ]; then
    echo "  deployment branch policy 'main' already exists (id $policy)"
  else
    run gh api --silent -X POST "repos/$repo/environments/automation/deployment-branch-policies" \
      -f name=main -f type=branch
  fi

  section "labels (tools/github/labels.json; --force updates existing labels)"
  local py labels_tsv
  py=$(command -v python3 || command -v python || true)
  if [ -z "$py" ]; then
    echo "apply-settings: python3 is required to read tools/github/labels.json" >&2
    failures=$((failures + 1))
  elif ! labels_tsv=$("$py" -c '
import json, sys
for label in json.load(open(sys.argv[1], encoding="utf-8")):
    print(label["name"], label["color"], label.get("description", ""), sep="\t")
' tools/github/labels.json); then
    echo "apply-settings: cannot read tools/github/labels.json" >&2
    failures=$((failures + 1))
  else
    local name color desc
    while IFS=$'\t' read -r name color desc; do
      run gh label create "$name" --repo "$repo" --color "$color" --description "$desc" --force
    done <<<"$labels_tsv"
  fi

  section "current state (reads)"
  show "merge settings" "repos/$repo" \
    --jq '{allow_squash_merge, allow_merge_commit, allow_rebase_merge, squash_merge_commit_title,
           squash_merge_commit_message, delete_branch_on_merge, allow_auto_merge, allow_update_branch, has_wiki}'
  show "security_and_analysis" "repos/$repo" --jq .security_and_analysis
  show "private vulnerability reporting" "repos/$repo/private-vulnerability-reporting" --jq .enabled
  show "automated security fixes" "repos/$repo/automated-security-fixes" --jq .enabled
  show "Actions permissions" "repos/$repo/actions/permissions"
  show "workflow token defaults" "repos/$repo/actions/permissions/workflow"
  show "fork PR approval" "repos/$repo/actions/permissions/fork-pr-contributor-approval" --jq .approval_policy
}

gh_pages() {
  section "gh-pages: orphan branch with .nojekyll, then GitHub Pages from it (benchmark charts)"
  local head
  head=$(get "repos/$repo/commits?per_page=1" --jq '.[0].sha')
  if [ -z "$head" ]; then
    echo "  the repository has no commits yet: the Git database API refuses empty repositories."
    echo "  Push the root commit to main first."
    if [ "$apply" = 1 ]; then failures=$((failures + 1)); return; fi
  fi
  if [ -n "$(get "repos/$repo/branches/gh-pages" --jq .name)" ]; then
    echo "  branch gh-pages already exists"
  else
    local tree_json='{"tree":[{"path":".nojekyll","mode":"100644","type":"blob","content":"\n"}]}'
    local msg='message=chore: init gh-pages'
    if [ "$apply" = 1 ]; then
      local tree="" commit=""
      printf '+ %s <<< %s\n' "$(quote gh api -X POST "repos/$repo/git/trees" --input - --jq .sha)" "'$tree_json'"
      tree=$(gh api -X POST "repos/$repo/git/trees" --input - --jq .sha <<<"$tree_json") || tree=""
      if [ -n "$tree" ]; then
        printf '+ %s\n' "$(quote gh api -X POST "repos/$repo/git/commits" -f "$msg" -f "tree=$tree" --jq .sha)"
        commit=$(gh api -X POST "repos/$repo/git/commits" -f "$msg" -f "tree=$tree" --jq .sha) || commit=""
      fi
      if [ -n "$commit" ]; then
        run gh api --silent -X POST "repos/$repo/git/refs" -f ref=refs/heads/gh-pages -f "sha=$commit"
      else
        echo "apply-settings: FAILED to create the gh-pages tree or commit" >&2
        failures=$((failures + 1))
        return
      fi
    else
      # shellcheck disable=SC2016 # $TREE_SHA/$COMMIT_SHA are placeholders in the printed commands
      printf 'DRY-RUN: %s <<< %s   # -> TREE_SHA\n' \
        "$(quote gh api -X POST "repos/$repo/git/trees" --input - --jq .sha)" "'$tree_json'"
      # shellcheck disable=SC2016
      printf 'DRY-RUN: %s   # parentless commit -> COMMIT_SHA\n' \
        "$(quote gh api -X POST "repos/$repo/git/commits" -f "$msg" -f 'tree=$TREE_SHA' --jq .sha)"
      # shellcheck disable=SC2016
      printf 'DRY-RUN: %s\n' \
        "$(quote gh api --silent -X POST "repos/$repo/git/refs" -f ref=refs/heads/gh-pages -f 'sha=$COMMIT_SHA')"
    fi
  fi
  if [ -n "$(get "repos/$repo/pages" --jq .html_url)" ]; then
    echo "  GitHub Pages already enabled"
  else
    run gh api --silent -X POST "repos/$repo/pages" \
      -f build_type=legacy -f 'source[branch]=gh-pages' -f 'source[path]=/'
  fi
  show "Pages" "repos/$repo/pages" --jq '{html_url, build_type, source}'
}

ruleset() {
  local file
  if [ "$stage" = A ]; then file=tools/github/ruleset-main-stage-a.json; else file=tools/github/ruleset-main.json; fi
  section "ruleset 'main', stage $stage ($file)"
  [ -f "$file" ] || { echo "apply-settings: missing $file" >&2; exit 1; }
  local id
  id=$(get "repos/$repo/rulesets" --paginate \
    --jq '.[] | select(.name == "main" and .source_type == "Repository") | .id' | head -n 1)
  if [ -n "$id" ]; then
    echo "  ruleset 'main' exists (id $id): updating it"
    run gh api --silent -X PUT "repos/$repo/rulesets/$id" --input "$file"
  else
    echo "  no ruleset named 'main' yet: creating it"
    run gh api --silent -X POST "repos/$repo/rulesets" --input "$file"
    id=$(get "repos/$repo/rulesets" --paginate \
      --jq '.[] | select(.name == "main" and .source_type == "Repository") | .id' | head -n 1)
  fi
  if [ -n "$id" ]; then
    show "ruleset main" "repos/$repo/rulesets/$id" --jq '{id, enforcement, rules: [.rules[].type]}'
  else
    echo "  ruleset main: (not available yet)"
  fi
}

if [ "$apply" = 1 ]; then
  echo "apply-settings: APPLYING to $repo (mode: $mode${stage:+ $stage})"
else
  echo "apply-settings: DRY RUN for $repo (mode: $mode${stage:+ $stage}); nothing is changed. Re-run with --apply."
fi
if [ "$have_gh" = 0 ]; then echo "apply-settings: gh not found; reads are skipped"; fi

case "$mode" in
  settings) settings ;;
  gh-pages) gh_pages ;;
  ruleset) ruleset ;;
esac

if [ "$failures" -gt 0 ]; then
  echo "apply-settings: $failures step(s) failed" >&2
  exit 1
fi
echo
echo "apply-settings: done ($([ "$apply" = 1 ] && echo applied || echo dry run))"
