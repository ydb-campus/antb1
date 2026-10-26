"""The security properties of .github/workflows/release.yml that actionlint, R010 and zizmor do not check
(docs/adr/0035-github-releases.md; run by `pixi run lint`): its triggers, the exact permissions of each job, the guard
on the jobs that sign, draft and publish, a cold build that reads and writes no cache, the `needs` wiring that carries
the attested digests as a job output, what each writing job checks out, and that no other workflow starts on a tag.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

import yaml

ROOT = Path(__file__).resolve().parents[3]
WORKFLOWS = ROOT / ".github" / "workflows"
GUARD = (
    "${{ github.repository == 'ydb-campus/antb1' && github.event_name == 'push' && startsWith(github.ref, "
    "'refs/tags/v') && needs.preflight.outputs.publish == 'true' }}"
)
PERMISSIONS = {
    "preflight": {"contents": "read", "actions": "read"},
    "build": {"contents": "read"},
    "assemble": {"contents": "read"},
    "verify": {"contents": "read"},
    "attest": {"id-token": "write", "attestations": "write"},
    "draft": {"contents": "write"},
    "publish": {"contents": "write", "attestations": "read"},
    "check-published": {"contents": "read", "attestations": "read"},
}
# The RELEASE_* variables each subcommand of release.py reads, and the job output each comes from; and the outputs each
# job declares, which test_release.py checks the subcommand writes.
BASE = {"TAG": "preflight.tag", "SHA": "preflight.sha", "VERSION": "preflight.version",
        "PRERELEASE": "preflight.prerelease", "PUBLISH": "preflight.publish"}  # fmt: skip
INPUTS = {
    "assemble": BASE,
    "draft": BASE | {"SUBJECTS": "assemble.subjects"},
    "publish": BASE | {"SUBJECTS": "assemble.subjects", "ID": "draft.release_id", "TAG_OBJECT": "preflight.tag_object"},
    "check-published": BASE | {"LATEST": "publish.latest"},
}
OUTPUTS = {
    "preflight": {"sha", "version", "tag", "tag_object", "prerelease", "publish"},
    "assemble": {"subjects"},
    "draft": {"release_id"},
    "publish": {"latest"},
}
NEEDS = {
    "preflight": [],
    "build": ["preflight"],
    "assemble": ["preflight", "build"],
    "verify": ["preflight", "assemble"],
    "attest": ["preflight", "assemble", "verify"],
    "draft": ["preflight", "assemble", "attest"],
    "publish": ["preflight", "assemble", "draft"],
    "check-published": ["preflight", "publish"],
}


def load(name: str) -> dict[str, Any]:
    return yaml.safe_load((WORKFLOWS / name).read_text())


def triggers(workflow: dict[str, Any]) -> dict[str, Any]:
    on = workflow.get("on", workflow.get(True))  # YAML 1.1 reads a bare `on` as True
    return on if isinstance(on, dict) else {name: None for name in ([on] if isinstance(on, str) else on)}


RELEASE = load("release.yml")
JOBS = RELEASE["jobs"]


def steps(job: str) -> list[dict[str, Any]]:
    return JOBS[job]["steps"]


def uses(job: str) -> list[str]:
    return [step["uses"].split("@", 1)[0] for step in steps(job) if "uses" in step]


def test_it_runs_on_v_tags_and_by_hand_and_is_never_cancelled() -> None:
    on = triggers(RELEASE)
    assert on == {"push": {"tags": ["v*"]}, "workflow_dispatch": None}  # a dispatch has no inputs
    assert RELEASE["permissions"] == {}
    assert RELEASE["concurrency"] == {"group": "${{ github.workflow }}-${{ github.event_name }}",
                                      "cancel-in-progress": False}  # fmt: skip


def test_each_job_has_exactly_its_permissions_and_needs() -> None:
    assert sorted(JOBS) == sorted(PERMISSIONS)
    for name, job in JOBS.items():
        assert job["permissions"] == PERMISSIONS[name], name
        needs = job.get("needs", [])
        assert ([needs] if isinstance(needs, str) else needs) == NEEDS[name], name
        assert isinstance(job["timeout-minutes"], int), name


def test_only_a_tag_push_of_this_repository_signs_drafts_or_publishes() -> None:
    for name, job in JOBS.items():
        guarded = " ".join(str(job.get("if", "")).split())
        assert (guarded == GUARD) == (name in ("attest", "draft", "publish", "check-published")), name
    assert [name for name, job in JOBS.items() if "environment" in job] == ["publish"]
    assert JOBS["publish"]["environment"]["name"] == "release"


def test_the_build_is_cold_and_no_job_reads_or_writes_a_cache() -> None:
    for name in JOBS:
        assert not [u for u in uses(name) if u.startswith("actions/cache")], name
        for step in steps(name):
            if step.get("uses", "").startswith("prefix-dev/setup-pixi"):
                assert step["with"]["cache"] is False and step["with"]["cache-write"] is False, name
                assert "cache-key" not in step["with"]
    build = JOBS["build"]
    assert build["env"]["CCACHE_DISABLE"] == "1" and build["env"]["ANTB1_ARROW_DIR"].endswith("/.cache/arrow")
    assert any('test ! -e "$ANTB1_ARROW_DIR"' in step.get("run", "") for step in steps("build"))
    static = load("static.yml")["jobs"]["static"]["strategy"]["matrix"]["include"]
    ours = build["strategy"]["matrix"]["include"]
    assert [(e["target"], e["os"]) for e in ours] == [(e["target"], e["os"]) for e in static]


def test_the_signing_job_runs_no_repository_code() -> None:
    assert uses("attest") == ["actions/attest", "actions/upload-artifact"]  # no checkout
    attest = next(step for step in steps("attest") if step.get("uses", "").startswith("actions/attest"))
    assert attest["with"] == {"subject-checksums": "${{ runner.temp }}/subjects.sha256"}
    subjects = next(step for step in steps("attest") if "SUBJECTS" in step.get("env", {}))
    assert subjects["env"]["SUBJECTS"] == "${{ needs.assemble.outputs.subjects }}"  # a job output, not an artifact


def test_every_job_checks_out_the_commit_of_the_run() -> None:
    # checkout's default is GITHUB_SHA, the commit preflight checked (a re-run keeps it); a `ref` taken from a job
    # output is what CodeQL's actions/untrusted-checkout flags, and there is nothing else it could name.
    for name in JOBS:
        for step in steps(name):
            if step.get("uses", "").startswith("actions/checkout"):
                assert "ref" not in step.get("with", {}), name
                assert step["with"]["persist-credentials"] is False, name


def test_the_writing_jobs_check_out_only_the_release_script() -> None:
    for name in ("draft", "publish"):
        checkout = next(step for step in steps(name) if step.get("uses", "").startswith("actions/checkout"))
        assert checkout["with"]["sparse-checkout"].strip() == "/.github/scripts/release.py", name
        assert checkout["with"]["persist-credentials"] is False
    for name in ("draft", "publish"):
        env = next(step for step in steps(name) if "run" in step)["env"]
        assert env["RELEASE_SUBJECTS"] == "${{ needs.assemble.outputs.subjects }}", name
    assert not [u for u in uses("publish") if u.startswith("actions/download-artifact")]  # publish trusts no artifact


def test_verify_runs_without_pixi_on_fresh_runners() -> None:
    assert "prefix-dev/setup-pixi" not in uses("verify")
    labels = {(e["target"], e["os"], e["engine"]) for e in JOBS["verify"]["strategy"]["matrix"]["include"]}
    assert labels == {("linux-x86_64", "ubuntu-24.04", "docker"), ("linux-aarch64", "ubuntu-24.04-arm", "docker"),
                      ("darwin-arm64", "macos-15", "none"), ("darwin-arm64", "macos-26", "none")}  # fmt: skip
    run = next(step for step in steps("verify") if "run" in step)["run"]
    assert run.startswith("python3 -I scripts/static_gates.py verify ") and '--container "$ENGINE"' in run


def test_no_expression_reaches_a_shell_and_no_job_moves_a_tag() -> None:
    for name in JOBS:
        for step in steps(name):
            run = step.get("run", "")
            assert "${{" not in run, (name, step.get("name"))
            assert not re.search(r"\bgh (release|api|workflow)\b", run), (name, step.get("name"))  # release.py only
    script = (ROOT / ".github/scripts/release.py").read_text()
    create = script[script.index('create = ["release", "create"') : script.index("proc = gh(*create")]
    create = re.sub(r"#[^\n]*", "", create)  # the code, not its comments
    assert '"--verify-tag", "--draft"' in create
    for flag in ("--target", "--clobber", "--cleanup-tag", "--generate-notes", "--latest", "--notes-from-tag"):
        assert flag not in create, flag
    # Sending target_commitish makes the Releases API apply its workflow-file rule to the release.
    assert '"target_commitish"' not in script and "target_commitish=" not in script


def test_every_job_name_ends_in_the_command_that_reproduces_it() -> None:
    for name, job in JOBS.items():
        assert re.fullmatch(r".+ \((pixi run .+|python3 -I .+|actions/attest)\)", job["name"]), name


def test_no_other_workflow_starts_on_a_tag_push() -> None:
    for path in sorted(WORKFLOWS.glob("*.yml")):
        if path.name == "release.yml":
            continue
        push = triggers(yaml.safe_load(path.read_text())).get("push", "absent")
        if push == "absent":
            continue
        push = push or {}
        assert "tags" not in push and "tags-ignore" not in push, path.name
        assert "branches" in push or "branches-ignore" in push, (
            f"{path.name}: a push trigger without branches runs on tags"
        )


def release_step(job: str) -> dict[str, Any]:
    (step,) = [step for step in steps(job) if ".github/scripts/release.py" in step.get("run", "")]
    return step


def test_each_release_step_gets_what_its_subcommand_reads() -> None:
    script = (ROOT / ".github/scripts/release.py").read_text()
    for job in ("preflight", *INPUTS):
        step = release_step(job)
        assert f"release.py {job}" in step["run"], job
        assert step["env"]["GH_TOKEN"] == "${{ github.token }}", job
        for flag in re.findall(r"(--[a-z-]+)", step["run"]):
            assert f'add_argument("{flag}"' in script, (job, flag)
    for job, inputs in INPUTS.items():
        env = release_step(job)["env"]
        for name, source in inputs.items():
            producer, key = source.split(".")
            assert env[f"RELEASE_{name}"] == f"${{{{ needs.{producer}.outputs.{key} }}}}", (job, name)
            assert producer in NEEDS[job], (job, producer)


def test_each_job_output_comes_from_its_release_step() -> None:
    for job, keys in OUTPUTS.items():
        step = release_step(job)
        assert JOBS[job]["outputs"] == {key: f"${{{{ steps.{step['id']}.outputs.{key} }}}}" for key in keys}, job
