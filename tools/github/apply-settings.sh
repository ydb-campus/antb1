#!/usr/bin/env bash
# Applies the antb1 GitHub configuration (docs/adr/0009-ci-and-governance.md, docs/adr/0035-github-releases.md)
# with `gh api`: repository, security and Actions settings, the `automation` environment, labels, gh-pages, the
# `main` ruleset and, for releases, the tag rulesets, the `release` environment and immutable releases.
#
# DRY RUN BY DEFAULT: every mutating command is printed, nothing is changed. Pass --apply to execute them.
# Reads (GET) always run, never fail the script and show the current state; they print "(not available yet)"
# when something does not exist yet or gh lacks access. --release then compares what GitHub reports with the
# files: a DIFF fails the run after --apply and is only shown in a dry run.
#
# Usage (repository root, repository admin, authenticated gh; python3 for labels.json and --release):
#   bash tools/github/apply-settings.sh [--apply] [--repo OWNER/NAME]               settings, environment, labels
#   bash tools/github/apply-settings.sh [--apply] [--repo OWNER/NAME] --gh-pages    orphan gh-pages + Pages
#   bash tools/github/apply-settings.sh [--apply] [--repo OWNER/NAME] --ruleset A|B the `main` ruleset
#   bash tools/github/apply-settings.sh [--apply] [--repo OWNER/NAME] --release     tag rulesets, `release`, immutable
# Order (rollout): settings on the empty repo -> root commit on main -> --gh-pages -> --ruleset A -> PR #1a-#1c
# -> checks `CI OK`, `PR title` and CodeQL have reported on main -> --ruleset B. Releases: --release right before
# the PR that adds .github/workflows/release.yml merges; the read-back must show no DIFF.
# Reading the org Actions policy needs `gh auth refresh -h github.com -s admin:org`.
set -euo pipefail

repo=ydb-campus/antb1
apply=0
mode=settings
stage=""
description="Experimental C++23 analytics engine: SQL-like queries over local Parquet files (Apache Arrow)"
topics=(cpp cpp23 parquet apache-arrow analytics query-engine clickbench pixi)

usage() { awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "${BASH_SOURCE[0]}"; }
one_mode() { echo "apply-settings: choose one of --gh-pages, --ruleset, --release" >&2; exit 2; }

while [ $# -gt 0 ]; do
  case "$1" in
    --apply) apply=1; shift ;;
    --repo)
      if [ $# -lt 2 ] || [ -z "$2" ]; then echo "apply-settings: --repo needs OWNER/NAME" >&2; exit 2; fi
      repo=$2; shift 2 ;;
    --gh-pages) [ "$mode" = settings ] || one_mode
      mode=gh-pages; shift ;;
    --ruleset) [ "$mode" = settings ] || one_mode
      mode=ruleset; stage="${2:-}"; shift $(($# >= 2 ? 2 : 1)) ;;
    --release) [ "$mode" = settings ] || one_mode
      mode=release; shift ;;
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

# The id of the repository's own ruleset NAME of TARGET (branch or tag); empty when there is none.
ruleset_id() {
  get "repos/$repo/rulesets" --paginate \
    --jq ".[] | select(.name == \"$1\" and .target == \"$2\" and .source_type == \"Repository\") | .id" | head -n 1
}

# Creates (POST) or replaces (PUT) the repository ruleset NAME of TARGET with FILE.
apply_ruleset() {
  local name=$1 target=$2 file=$3 id
  [ -f "$file" ] || { echo "apply-settings: missing $file" >&2; exit 1; }
  id=$(ruleset_id "$name" "$target")
  if [ -n "$id" ]; then
    echo "  ruleset '$name' exists (id $id): updating it"
    run gh api --silent -X PUT "repos/$repo/rulesets/$id" --input "$file"
  else
    echo "  no ruleset named '$name' yet: creating it"
    run gh api --silent -X POST "repos/$repo/rulesets" --input "$file"
  fi
}

ruleset() {
  local file id
  if [ "$stage" = A ]; then file=tools/github/ruleset-main-stage-a.json; else file=tools/github/ruleset-main.json; fi
  section "ruleset 'main', stage $stage ($file)"
  apply_ruleset main branch "$file"
  id=$(ruleset_id main branch)
  if [ -n "$id" ]; then
    show "ruleset main" "repos/$repo/rulesets/$id" --jq '{id, enforcement, rules: [.rules[].type]}'
  else
    echo "  ruleset main: (not available yet)"
  fi
}

# Read-back of --release: compares a GitHub response (stdin) with the file that set it and prints OK or DIFF lines.
# Kinds: ruleset FILE, environment FILE, policies (exactly one tag policy v*), immutable (enabled).
verify_py='
import json, sys
kind, path = sys.argv[1], sys.argv[2]
want = json.load(open(path, encoding="utf-8")) if path != "-" else None
raw = sys.stdin.read().strip()
try:
    got = json.loads(raw) if raw else None
except ValueError:
    got = None
diffs, notes = [], []
def check(what, have, should):
    if have != should:
        diffs.append("%s is %s, want %s" % (what, json.dumps(have), json.dumps(should)))
if not isinstance(got, dict):
    diffs.append("not found")
elif kind == "ruleset":
    for key in ("name", "target", "enforcement", "conditions"):
        check(key, got.get(key), want[key])
    def actors(items):
        return sorted(json.dumps([a.get("actor_type"), a.get("actor_id"), a.get("bypass_mode")]) for a in items or [])
    check("bypass_actors", actors(got.get("bypass_actors")), actors(want["bypass_actors"]))
    live = {r["type"]: r.get("parameters") or {} for r in got.get("rules", [])}
    check("rules", sorted(live), sorted(r["type"] for r in want["rules"]))
    for rule in want["rules"]:
        for key, value in (rule.get("parameters") or {}).items():
            check("%s.%s" % (rule["type"], key), live.get(rule["type"], {}).get(key), value)
    notes.append("you can bypass: %s" % got.get("current_user_can_bypass"))
elif kind == "environment":
    rules = {r["type"]: r for r in got.get("protection_rules", [])}
    review = rules.get("required_reviewers", {})
    people = review.get("reviewers", [])
    check("can_admins_bypass", got.get("can_admins_bypass"), want["can_admins_bypass"])
    check("deployment_branch_policy", got.get("deployment_branch_policy"), want["deployment_branch_policy"])
    check("wait_timer", rules.get("wait_timer", {}).get("wait_timer", 0), want["wait_timer"])
    check("prevent_self_review", review.get("prevent_self_review", False), want["prevent_self_review"])
    check("reviewers", sorted("%s:%s" % (p["type"], p["reviewer"]["id"]) for p in people),
          sorted("%s:%s" % (p["type"], p["id"]) for p in want["reviewers"]))
    notes.append("reviewers: %s" % ", ".join(p["reviewer"].get("login", "?") for p in people))
elif kind == "policies":
    check("policies", sorted([p["type"], p["name"]] for p in got.get("branch_policies", [])), [["tag", "v*"]])
elif kind == "immutable":
    check("enabled", got.get("enabled"), True)
for line in diffs:
    print("    DIFF " + line)
print("    " + ("OK" if not diffs else "%d difference(s)" % len(diffs)) + "".join("; " + n for n in notes))
sys.exit(1 if diffs else 0)
'

# `verify LABEL KIND FILE|- [gh-api-args...]`: reads (no args: nothing to read) and compares; see verify_py.
verify() {
  local label=$1 kind=$2 file=$3 live=""
  shift 3
  if [ $# -gt 0 ]; then live=$(get "$@"); fi
  printf '  %s\n' "$label"
  if ! printf '%s' "$live" | "$py" -I -c "$verify_py" "$kind" "$file"; then
    if [ "$apply" = 1 ]; then failures=$((failures + 1)); fi
  fi
}

release() {
  py=$(command -v python3 || command -v python || true)
  [ -n "$py" ] || { echo "apply-settings: python3 is required for --release" >&2; exit 1; }

  section "tag rulesets: only admins create, move or delete tags; nobody moves or deletes v* tags; CI OK first"
  apply_ruleset tags tag tools/github/ruleset-tags.json
  apply_ruleset release-tags tag tools/github/ruleset-release-tags.json

  section "environment 'release': a maintainer approves (self-approval allowed); v* tags only; no admin bypass"
  # The PUT replaces the protection rules: an omitted can_admins_bypass would turn back to true.
  run gh api --silent -X PUT "repos/$repo/environments/release" --input tools/github/environment-release.json
  if [ -n "$(get "repos/$repo/environments/release/deployment-branch-policies" \
    --jq '.branch_policies[] | select(.type == "tag" and .name == "v*") | .id')" ]; then
    echo "  deployment tag policy 'v*' already exists"
  else
    run gh api --silent -X POST "repos/$repo/environments/release/deployment-branch-policies" -f 'name=v*' -f type=tag
  fi

  section "immutable releases: a published release keeps its assets and tag, even if this is turned off later"
  run gh api --silent -X PUT "repos/$repo/immutable-releases"

  section "read-back (before the first --apply a dry run lists what is missing)"
  local id
  id=$(ruleset_id tags tag)
  verify "tag ruleset 'tags'${id:+ (id $id)}" ruleset tools/github/ruleset-tags.json ${id:+"repos/$repo/rulesets/$id"}
  id=$(ruleset_id release-tags tag)
  verify "tag ruleset 'release-tags'${id:+ (id $id)}" ruleset tools/github/ruleset-release-tags.json \
    ${id:+"repos/$repo/rulesets/$id"}
  verify "environment 'release'" environment tools/github/environment-release.json "repos/$repo/environments/release"
  verify "environment 'release': deployment refs" policies - \
    "repos/$repo/environments/release/deployment-branch-policies"
  verify "immutable releases" immutable - "repos/$repo/immutable-releases"
  show "tags" "repos/$repo/tags" --jq '[.[].name]'
  show "releases" "repos/$repo/releases" --jq '[.[] | {tag_name, draft, immutable}]'
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
  release) release ;;
esac

if [ "$failures" -gt 0 ]; then
  echo "apply-settings: $failures step(s) failed" >&2
  exit 1
fi
echo
echo "apply-settings: done ($([ "$apply" = 1 ] && echo applied || echo dry run))"
