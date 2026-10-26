"""Tests for the release settings in tools/github (docs/adr/0035-github-releases.md; run by `pixi run lint`): the tag
rulesets, the `release` environment and `apply-settings.sh --release`, which is a dry run unless asked and reads every
setting back. A fake gh answers the reads from a table and records every call, so nothing reaches GitHub.
"""

from __future__ import annotations

import ast
import importlib.util
import json
import os
import re
import subprocess
import sys
from pathlib import Path

import yaml
from fakes import assert_fakes_first

ROOT = Path(__file__).resolve().parents[3]
GITHUB = ROOT / "tools" / "github"
SCRIPT = GITHUB / "apply-settings.sh"
REPO = "owner/repo"

# The fake gh: `gh api [-X METHOD] ENDPOINT [--jq EXPR] [--input FILE] [-f k=v]...`. It logs each call to
# $FAKE_GH_LOG as JSON and answers from $FAKE_GH_MAP, keyed by "METHOD ENDPOINT" or, with --jq, by
# "METHOD ENDPOINT | EXPR" (the answer is what gh would print after the filter). Writes succeed silently; a read
# that is not in the table fails like a 404.
FAKE_GH = """#!{python} -I
import json, os, sys
args = sys.argv[1:]
assert args[0] == "api", args
method, endpoint, jq, body, fields, i = None, None, None, None, [], 1
while i < len(args):
    a = args[i]
    if a in ("-X", "--method"):
        method = args[i + 1]; i += 2; continue
    if a == "--jq":
        jq = args[i + 1]; i += 2; continue
    if a == "--input":
        body = open(args[i + 1], encoding="utf-8").read(); i += 2; continue
    if a in ("-f", "-F"):
        fields.append(args[i + 1]); i += 2; continue
    if a in ("--silent", "--paginate"):
        i += 1; continue
    endpoint = a; i += 1
method = method or ("POST" if body is not None or fields else "GET")
with open(os.environ["FAKE_GH_LOG"], "a", encoding="utf-8") as log:
    log.write(json.dumps({{"method": method, "endpoint": endpoint, "jq": jq, "body": body, "fields": fields}}) + "\\n")
if method != "GET":
    sys.exit(0)
table = json.load(open(os.environ["FAKE_GH_MAP"], encoding="utf-8"))
key = method + " " + endpoint + (" | " + jq if jq else "")
if key not in table:
    print('{{"message": "Not Found", "status": "404"}}')
    sys.exit(1)
sys.stdout.write(table[key])
"""

LIST_JQ = '.[] | select(.name == "{}" and .target == "{}" and .source_type == "Repository") | .id'
POLICY_JQ = '.branch_policies[] | select(.type == "tag" and .name == "v*") | .id'


def load(name: str) -> dict:
    return json.loads((GITHUB / name).read_text(encoding="utf-8"))


def live_ruleset(name: str, rid: int) -> str:
    """What GitHub returns for a ruleset created from the file (plus the fields it adds)."""
    data = {**load(name), "id": rid, "source_type": "Repository", "current_user_can_bypass": "always"}
    return json.dumps(data)


def live_environment(**changes: object) -> str:
    env = load("environment-release.json")
    reviewers = [{"type": r["type"], "reviewer": {"id": r["id"], "login": f"user{r['id']}"}} for r in env["reviewers"]]
    data = {
        "name": "release",
        "can_admins_bypass": env["can_admins_bypass"],
        "deployment_branch_policy": env["deployment_branch_policy"],
        "protection_rules": [
            {"id": 1, "type": "required_reviewers", "prevent_self_review": env["prevent_self_review"],
             "reviewers": reviewers},
            {"id": 2, "type": "branch_policy"},
        ],
    }  # fmt: skip
    data.update(changes)
    return json.dumps(data)


def applied_state(**env_changes: object) -> dict[str, str]:
    """The answers of a repository that already has every release setting."""
    r = f"repos/{REPO}"
    return {
        f"GET {r}/rulesets | {LIST_JQ.format('tags', 'tag')}": "101\n",
        f"GET {r}/rulesets | {LIST_JQ.format('release-tags', 'tag')}": "102\n",
        f"GET {r}/rulesets/101": live_ruleset("ruleset-tags.json", 101),
        f"GET {r}/rulesets/102": live_ruleset("ruleset-release-tags.json", 102),
        f"GET {r}/environments/release": live_environment(**env_changes),
        f"GET {r}/environments/release/deployment-branch-policies | {POLICY_JQ}": "7\n",
        f"GET {r}/environments/release/deployment-branch-policies": json.dumps(
            {"total_count": 1, "branch_policies": [{"id": 7, "name": "v*", "type": "tag"}]}
        ),
        f"GET {r}/immutable-releases": '{"enabled": true, "enforced_by_owner": false}',
        f"GET {r}/tags | [.[].name]": "[]\n",
        f"GET {r}/releases | [.[] | {{tag_name, draft, immutable}}]": "[]\n",
    }


def run(tmp: Path, table: dict[str, str], *flags: str) -> tuple[subprocess.CompletedProcess[str], list[dict]]:
    bin_dir = tmp / "bin"
    bin_dir.mkdir(exist_ok=True)
    gh = bin_dir / "gh"
    gh.write_text(FAKE_GH.format(python=sys.executable), encoding="utf-8")
    gh.chmod(0o755)
    (tmp / "map.json").write_text(json.dumps(table), encoding="utf-8")
    log = tmp / "calls.jsonl"
    log.write_text("", encoding="utf-8")
    path = f"{bin_dir}{os.pathsep}{os.path.dirname(sys.executable)}{os.pathsep}/usr/bin{os.pathsep}/bin"
    assert_fakes_first(path, bin_dir, ["gh"])  # never the host's own, authenticated gh
    env = {"PATH": path, "HOME": str(tmp), "GH_HOST": "gh.invalid", "GH_CONFIG_DIR": str(tmp / "gh-config"),
           "FAKE_GH_MAP": str(tmp / "map.json"), "FAKE_GH_LOG": str(log), "LC_ALL": "C"}  # fmt: skip
    proc = subprocess.run(["bash", str(SCRIPT), *flags, "--repo", REPO], cwd=ROOT, env=env, capture_output=True,
                          text=True, timeout=60, check=False)  # fmt: skip
    calls = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
    return proc, calls


def writes(calls: list[dict]) -> list[tuple[str, str]]:
    return [(c["method"], c["endpoint"]) for c in calls if c["method"] != "GET"]


def test_the_tag_rulesets_and_the_environment_say_what_adr_0035_decides() -> None:
    tags, release, env = load("ruleset-tags.json"), load("ruleset-release-tags.json"), load("environment-release.json")
    assert tags["enforcement"] == release["enforcement"] == "active"  # "evaluate" would only report
    assert (tags["target"], tags["conditions"]) == ("tag", {"ref_name": {"include": ["~ALL"], "exclude": []}})
    assert sorted(r["type"] for r in tags["rules"]) == ["creation", "deletion", "non_fast_forward", "update"]
    assert tags["bypass_actors"] == [{"actor_id": 5, "actor_type": "RepositoryRole", "bypass_mode": "always"}]
    v_tags = {"ref_name": {"include": ["refs/tags/v*"], "exclude": []}}
    assert (release["target"], release["conditions"]) == ("tag", v_tags)
    assert release["bypass_actors"] == []  # nobody, admins included, moves or deletes a v* tag
    rules = {r["type"]: r.get("parameters") for r in release["rules"]}
    assert sorted(rules) == ["deletion", "non_fast_forward", "required_status_checks", "update"]
    checks = rules["required_status_checks"]
    assert checks["do_not_enforce_on_create"] is False  # creating is the only change allowed: it needs CI OK
    main_checks = next(r for r in load("ruleset-main.json")["rules"] if r["type"] == "required_status_checks")
    ci_ok = [c for c in main_checks["parameters"]["required_status_checks"] if c["context"] == "CI OK"]
    assert checks["required_status_checks"] == ci_ok  # the same check, from the same App (GitHub Actions)
    # Both maintainers review; the one who pushed the tag may approve it (self-approval); admins cannot bypass.
    assert env["can_admins_bypass"] is False and env["prevent_self_review"] is False and env["wait_timer"] == 0
    assert env["reviewers"] == [{"type": "User", "id": 24967150}, {"type": "User", "id": 33665449}]  # the maintainers
    assert env["deployment_branch_policy"] == {"protected_branches": False, "custom_branch_policies": True}


def test_a_dry_run_changes_nothing_and_lists_the_writes(tmp_path: Path) -> None:
    table = {f"GET repos/{REPO}/immutable-releases": '{"enabled": false, "enforced_by_owner": false}'}
    proc, calls = run(tmp_path, table, "--release")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert writes(calls) == []
    planned = [line for line in proc.stdout.splitlines() if line.startswith("DRY-RUN: ")]
    assert [p.split(" -X ")[1].split()[:2] for p in planned] == [
        ["POST", f"repos/{REPO}/rulesets"],
        ["POST", f"repos/{REPO}/rulesets"],
        ["PUT", f"repos/{REPO}/environments/release"],
        ["POST", f"repos/{REPO}/environments/release/deployment-branch-policies"],
        ["PUT", f"repos/{REPO}/immutable-releases"],
    ]
    assert "--input tools/github/ruleset-tags.json" in planned[0]
    assert "--input tools/github/ruleset-release-tags.json" in planned[1]
    assert "--input tools/github/environment-release.json" in planned[2]
    assert "-f 'name=v*' -f type=tag" in planned[3]
    assert "DIFF enabled is false, want true" in proc.stdout


def test_apply_updates_in_place_and_reads_back_clean(tmp_path: Path) -> None:
    proc, calls = run(tmp_path, applied_state(), "--apply", "--release")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert writes(calls) == [
        ("PUT", f"repos/{REPO}/rulesets/101"),
        ("PUT", f"repos/{REPO}/rulesets/102"),
        ("PUT", f"repos/{REPO}/environments/release"),
        ("PUT", f"repos/{REPO}/immutable-releases"),
    ]  # the existing tag policy is not posted again
    bodies = [c["body"] for c in calls if c["method"] == "PUT"]
    names = ("ruleset-tags.json", "ruleset-release-tags.json", "environment-release.json")
    assert bodies[:3] == [(GITHUB / name).read_text(encoding="utf-8") for name in names]
    assert "DIFF" not in proc.stdout and proc.stdout.count("    OK") == 5


def test_apply_fails_on_any_difference_it_reads_back(tmp_path: Path) -> None:
    for change, diff in (
        ({"can_admins_bypass": True}, "DIFF can_admins_bypass is true, want false"),
        ({"deployment_branch_policy": {"protected_branches": True, "custom_branch_policies": False}},
         "DIFF deployment_branch_policy"),
    ):  # fmt: skip
        proc, _ = run(tmp_path, applied_state(**change), "--apply", "--release")
        assert proc.returncode == 1 and diff in proc.stdout, proc.stdout
    table = applied_state()
    key = f"GET repos/{REPO}/rulesets/102"
    admins = [{"actor_id": 5, "actor_type": "RepositoryRole", "bypass_mode": "always"}]
    table[key] = json.dumps({**json.loads(table[key]), "bypass_actors": admins})  # admins could move a v* tag
    proc, _ = run(tmp_path, table, "--apply", "--release")
    assert proc.returncode == 1 and "DIFF bypass_actors" in proc.stdout


def test_the_main_ruleset_is_applied_as_before(tmp_path: Path) -> None:
    proc, calls = run(tmp_path, {}, "--ruleset", "B")
    assert proc.returncode == 0 and writes(calls) == []
    assert "  no ruleset named 'main' yet: creating it" in proc.stdout.splitlines()
    assert (
        f"DRY-RUN: gh api --silent -X POST repos/{REPO}/rulesets --input tools/github/ruleset-main.json" in proc.stdout
    )
    table = {f"GET repos/{REPO}/rulesets | {LIST_JQ.format('main', 'branch')}": "55\n"}
    proc, calls = run(tmp_path, table, "--ruleset", "B")
    assert "  ruleset 'main' exists (id 55): updating it" in proc.stdout.splitlines()
    assert (
        f"DRY-RUN: gh api --silent -X PUT repos/{REPO}/rulesets/55 --input tools/github/ruleset-main.json"
        in proc.stdout
    )


def test_the_read_back_runs_on_the_admins_python() -> None:
    text = SCRIPT.read_text(encoding="utf-8")
    code = re.search(r"^verify_py='\n(?P<code>.*?)^'$", text, re.DOTALL | re.MULTILINE)
    assert code is not None
    ast.parse(code["code"], feature_version=(3, 8))  # apply-settings.sh runs the host's python3, maybe 3.8


def test_the_tag_pattern_and_the_environment_agree_everywhere() -> None:
    workflow = yaml.safe_load((ROOT / ".github/workflows/release.yml").read_text())
    on = workflow.get("on", workflow.get(True))
    assert on["push"] == {"tags": ["v*"]}
    assert load("ruleset-release-tags.json")["conditions"]["ref_name"]["include"] == ["refs/tags/v*"]
    assert "-f 'name=v*' -f type=tag" in SCRIPT.read_text(encoding="utf-8")
    spec = importlib.util.spec_from_file_location("release_for_settings", ROOT / ".github/scripts/release.py")
    assert spec is not None and spec.loader is not None
    release = importlib.util.module_from_spec(spec)
    sys.modules["release_for_settings"] = release
    spec.loader.exec_module(release)
    assert release.TAG_POLICIES == [("v*", "tag")] and release.ENVIRONMENT == "release"
    users = {
        (path.name, name)
        for path in (ROOT / ".github/workflows").glob("*.yml")
        for name, job in (yaml.safe_load(path.read_text()).get("jobs") or {}).items()
        if (job.get("environment") or {}) in ("release", {"name": "release"})
        or (isinstance(job.get("environment"), dict) and job["environment"].get("name") == "release")
    }
    assert users == {("release.yml", "publish")}  # only the publishing job waits for the release environment
