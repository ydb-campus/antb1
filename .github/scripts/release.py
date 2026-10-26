#!/usr/bin/env python3
"""GitHub releases of the standalone static binary (docs/adr/0035-github-releases.md, docs/ci.md#releases): the logic
of .github/workflows/release.yml. Standard library only (Python 3.11 or newer, ruff's py311 target), run as
`python3 -I .github/scripts/release.py <command>` with each runner's own Python, and by the lint env's tests
(tools/ci/tests/test_release.py) against a fake gh. Every GitHub access goes through `gh` (never `--jq`: the JSON is
parsed here), so the fake gh is the only seam; nothing here reads $GH_TOKEN.

  preflight        (ubuntu-slim; contents, checks and actions: read) the tag is v<VERSION> or v<VERSION>-rc.<N> of
                   CMakeLists.txt's project VERSION, which equals pixi.toml's; it is annotated and peels to
                   $GITHUB_SHA; that commit is on main; its newest `CI OK` check run of GitHub Actions (app 15368) is
                   completed and green and comes from .github/workflows/ci.yml; no published release has the tag;
                   the `release` environment and the two tag rulesets match their files in tools/github/. A tag push
                   fails at the first miss; a manual run (workflow_dispatch) is a dry run of its ref: every miss is a
                   warning and publish=false. Writes sha, version, tag, tag_object, prerelease and publish.
  assemble         (contents: read) the build artifacts as static.yml names them (antb1-<target> and, on Linux,
                   antb1-<target>-sources, one directory each) -> --out: SHA256SUMS of the seven tarballs and
                   notes.md (the release-meta artifact). Checks every per-target SHA256SUMS and .sha256, the exact
                   file sets, each package's BUILDINFO.json against the commit and the tag, and every member of the
                   sources tarballs against the committed pins (cmake/static/arrow-sources.lock,
                   cmake/static/glibc-source.lock, pixi.lock). Writes the attestation's subjects (the tarballs and
                   SHA256SUMS) as the job output `subjects`: any later job of the run can replace an artifact, none
                   can replace a finished job's output, so attest signs, and draft and publish check, these digests.
  draft            (contents: write) checks the build artifacts against $RELEASE_SUBJECTS and the Sigstore bundle,
                   refuses a published release of the tag, deletes every draft of the tag, runs `gh release create TAG
                   --verify-tag --draft --notes-file` (never --target, --latest or generated notes), then compares the
                   draft's asset names, sizes and digests with the files. Writes release_id.
  publish          (contents: write, attestations: read; environment `release`) by release id: the tag is still the
                   annotated tag object preflight saw on the commit; downloads every asset of the draft and checks it
                   against the API's digest, $RELEASE_SUBJECTS and the draft's own bundle (no artifact is trusted);
                   PATCHes draft=false with an explicit make_latest; then requires immutable: true and checks the
                   release attestation (`gh release verify`). A re-run on a published release only verifies. Writes
                   latest.
  check-published  (contents and attestations: read) the published release as a user gets it, on each target's own
                   runner: README.md's install block run with `sh -e` (for the tag, and as written when the release is
                   the latest), then gh release verify, gh release verify-asset and gh attestation verify (online and
                   with the bundle) of the tarball it downloaded. Maintainers can run it on any published release.

Inputs come only from the environment (checked by regex; never `${{ }}` in a run block): GitHub's GITHUB_REPOSITORY,
GITHUB_EVENT_NAME, GITHUB_REF, GITHUB_SHA, GITHUB_OUTPUT, GITHUB_STEP_SUMMARY, GH_TOKEN; preflight's outputs as
RELEASE_TAG, RELEASE_SHA, RELEASE_VERSION, RELEASE_PRERELEASE, RELEASE_PUBLISH and RELEASE_TAG_OBJECT; assemble's as
RELEASE_SUBJECTS; draft's as RELEASE_ID; publish's as RELEASE_LATEST. draft and publish refuse to run outside a
tag-push run of ydb-campus/antb1.

Every asset is first-party: the packages, their .debug tarballs, the Linux sources tarballs (which release.yml's
build packs, holding third-party files that assemble checks against the committed pins) and SHA256SUMS are the
attested subjects; antb1.sigstore.json is the bundle itself, neither a subject nor in SHA256SUMS.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import secrets
import subprocess
import sys
import tarfile
import time
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Any, NoReturn

ROOT = Path(__file__).resolve().parents[2]

UPSTREAM = "ydb-campus/antb1"  # the only repository a tag push publishes from
MAIN = "main"
CI_WORKFLOW = ".github/workflows/ci.yml"
CI_CHECK = "CI OK"  # ci.yml's aggregator job; tools/github/ruleset-main.json requires it from app 15368
ACTIONS_APP_ID = 15368  # GitHub Actions
CI_EVENTS = ("push", "merge_group", "schedule", "workflow_dispatch")  # not pull_request: that tests a merge commit
RELEASE_WORKFLOW = ".github/workflows/release.yml"
REQUIRE_SIGNED_TAG = False  # annotated now; verification.verified once both maintainers have registered keys
TARGETS = ("linux-x86_64", "linux-aarch64", "darwin-arm64")  # static.yml's matrix
CONDA_SUBDIR = {"linux-x86_64": "linux-64", "linux-aarch64": "linux-aarch64"}  # the Linux targets
COMPILED_PACKAGES = ("cli11", "libboost-headers")  # scripts/static_package.py
ENVIRONMENT = "release"
ENVIRONMENT_FILE = "tools/github/environment-release.json"
TAG_POLICIES = [("v*", "tag")]  # the release environment's deployment policy (tools/github/apply-settings.sh)
RULESET_FILES = {"tags": "tools/github/ruleset-tags.json", "release-tags": "tools/github/ruleset-release-tags.json"}
INSTALL_BEGIN, INSTALL_END = "<!-- install:begin -->", "<!-- install:end -->"  # README.md's install block
SUMS = "SHA256SUMS"
BUNDLE = "antb1.sigstore.json"
SLSA_PROVENANCE = "https://slsa.dev/provenance/v1"
RELEASE_PREDICATE = "https://in-toto.io/attestation/release/v0.2"
NOTES_LIMIT = 125_000  # characters of a release body
ASSET_LIMIT = 2 * 1024**3  # bytes per release asset
RETRIES = 6
RETRY_DELAY = float(os.environ.get("ANTB1_RELEASE_RETRY_DELAY", "10"))  # the tests set 0
WORKFLOW_RULE = (
    "the Releases API refuses GITHUB_TOKEN a release whose target commit changes .github/workflows/ relative to "
    "main: tag the head of main and merge no workflow change before publishing (docs/ci.md#releases)"
)

SEMVER = r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"
VERSION_RE = re.compile(SEMVER)
TAG_RE = re.compile(rf"v(?P<version>{SEMVER})(?:-rc\.(?P<rc>[1-9][0-9]*))?")
SHA_RE = re.compile(r"[0-9a-f]{40}")
ID_RE = re.compile(r"[1-9][0-9]{0,18}")
REPO_RE = re.compile(r"[A-Za-z0-9-]+/[A-Za-z0-9._-]+")
DIGEST_RE = re.compile(r"sha256:[0-9a-f]{64}")
SUM_RE = re.compile(r"(?P<digest>[0-9a-f]{64}) [ *](?P<name>\S+)")
GH_ENV = {"GH_PROMPT_DISABLED": "1", "GH_NO_UPDATE_NOTIFIER": "1", "GH_SPINNER_DISABLED": "1", "NO_COLOR": "1"}


def release_files() -> list[str]:
    """The seven tarballs, in the order of the notes."""
    names = []
    for target in TARGETS:
        names.append(f"antb1-{target}.tar.gz")
        if target in CONDA_SUBDIR:
            names += [f"antb1-{target}.debug.tar.gz", f"antb1-{target}.sources.tar"]
    return names


SUBJECTS = [*release_files(), SUMS]  # what actions/attest signs
ASSETS = [*SUBJECTS, BUNDLE]  # what the release holds, exactly
ARTIFACTS = sorted([f"antb1-{t}" for t in TARGETS] + [f"antb1-{t}-sources" for t in CONDA_SUBDIR])


class ReleaseError(Exception):
    pass


def fail(message: str) -> NoReturn:
    raise ReleaseError(message)


def log(message: str) -> None:
    for line in message.splitlines() or [""]:
        print(f"release: {line}", flush=True)  # never a line that starts with `::` (a workflow command)


def annotate(level: str, message: str) -> None:
    text = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print(f"::{level}::{text}", flush=True)


def summary(markdown: str) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(markdown.rstrip("\n") + "\n")


def outputs(**values: str) -> None:
    for key, value in values.items():
        if "\n" in value or "\r" in value:
            fail(f"output {key} has a line break")
        log(f"output {key}={value}")
    path = os.environ.get("GITHUB_OUTPUT")
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.writelines(f"{key}={value}\n" for key, value in values.items())


def output_lines(key: str, text: str) -> None:
    """A multi-line output (GITHUB_OUTPUT's heredoc form) with a delimiter no line can be."""
    delimiter = f"EOF_{secrets.token_hex(16)}"
    if delimiter in text:
        fail(f"output {key} holds its delimiter")
    log(f"output {key}:\n{text}")
    path = os.environ.get("GITHUB_OUTPUT")
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(f"{key}<<{delimiter}\n{text.rstrip(chr(10))}\n{delimiter}\n")


def env(name: str, pattern: re.Pattern[str] | None = None) -> str:
    value = os.environ.get(name, "")
    if not value or (pattern is not None and not pattern.fullmatch(value)):
        fail(f"${name} is {value!r}" + (f", expected {pattern.pattern}" if pattern else ""))
    return value


def flag(name: str) -> bool:
    value = env(name)
    if value not in ("true", "false"):
        fail(f"${name} is {value!r}, expected true or false")
    return value == "true"


def sha256_stream(stream: Any) -> str:
    digest = hashlib.sha256()
    for block in iter(lambda: stream.read(1 << 20), b""):
        digest.update(block)
    return digest.hexdigest()


def sha256_file(path: Path) -> str:
    with path.open("rb") as f:
        return sha256_stream(f)


def version_key(version: str) -> tuple[int, int, int]:
    major, minor, patch = (int(part) for part in version.split("."))
    return major, minor, patch


# ---------------------------------------------------------------------------------------------------------------------
# gh


def gh(*args: str, stdout: Any = None, check: bool = True, timeout: float = 300) -> subprocess.CompletedProcess[bytes]:
    proc = subprocess.run(
        ["gh", *args],
        env={**os.environ, **GH_ENV},
        stdout=stdout if stdout is not None else subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
    )
    if check and proc.returncode != 0:
        fail(f"`gh {' '.join(args)}` exited with {proc.returncode}: {proc.stderr.decode(errors='replace').strip()}")
    return proc


@dataclass
class Response:
    method: str
    path: str
    status: int
    body: bytes

    def ok(self) -> Response:
        if not 200 <= self.status < 300:
            fail(f"gh api --method {self.method} {self.path}: HTTP {self.status} {self.message()}")
        return self

    def json(self) -> Any:
        return json.loads(self.body) if self.body.strip() else None

    def message(self) -> str:
        try:
            return str(self.json().get("message", ""))
        except (ValueError, AttributeError):
            return self.body[:200].decode(errors="replace")


def parse_included(out: bytes) -> tuple[int, bytes] | None:
    """`gh api --include` output: `HTTP/2.0 404 Not Found\\n`, `Name: value\\r\\n` lines, `\\r\\n`, the body."""
    first, sep, rest = out.partition(b"\n")
    match = re.fullmatch(rb"HTTP/[0-9.]+ ([0-9]{3})(?: .*)?", first.rstrip(b"\r"))
    if not sep or match is None:
        return None
    while True:
        line, sep, rest = rest.partition(b"\r\n")
        if not sep:
            return None
        if not line:
            return int(match[1]), rest


def api(method: str, path: str, fields: dict[str, bool | str] | None = None) -> Response:
    """One REST call. Only 2xx is success; a 404 is returned to the caller, which decides what it means."""
    args = ["api", "--include", "--method", method, path]
    for key, value in (fields or {}).items():  # -F true/false is a JSON boolean, -f a string
        args += ["-F", f"{key}={str(value).lower()}"] if isinstance(value, bool) else ["-f", f"{key}={value}"]
    proc = gh(*args, check=False)
    parsed = parse_included(proc.stdout)
    if parsed is None:
        fail(f"gh api --method {method} {path} gave no HTTP response (exit {proc.returncode}): "
             f"{proc.stderr.decode(errors='replace').strip()}")  # fmt: skip
    status, body = parsed
    if status < 400 and proc.returncode != 0:
        fail(f"gh api --method {method} {path}: HTTP {status} but exit {proc.returncode}")
    return Response(method, path, status, body)


def pages(path: str, key: str | None = None, limit: int = 50) -> list[dict[str, Any]]:
    """Every item of a paginated list (100 per page, until a short page)."""
    items: list[dict[str, Any]] = []
    sep = "&" if "?" in path else "?"
    for page in range(1, limit + 1):
        data = api("GET", f"{path}{sep}per_page=100&page={page}").ok().json()
        chunk = data.get(key) if key and isinstance(data, dict) else data
        if not isinstance(chunk, list):
            fail(f"GET {path}: expected a list{' in ' + key if key else ''}")
        items += chunk
        if len(chunk) < 100:
            return items
    fail(f"GET {path}: more than {limit} pages")


def download_asset(repo: str, asset_id: int, dest: Path) -> None:
    """A release asset by id (a draft's too): gh follows the redirect; a binary body is streamed to a file as is."""
    with dest.open("wb") as f:
        gh("api", "-H", "Accept: application/octet-stream", f"repos/{repo}/releases/assets/{asset_id}", stdout=f)


# ---------------------------------------------------------------------------------------------------------------------
# the repository at the tagged commit


def project_version(root: Path) -> tuple[str, str]:
    """(CMakeLists.txt project VERSION, pixi.toml's version)."""
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    call = re.search(r"(?im)^[ \t]*project[ \t]*\((?P<args>[^)]*)\)", cmake)
    version = re.search(r"(?<![A-Z_])VERSION\s+(\S+)", call["args"]) if call else None
    pixi = tomllib.loads((root / "pixi.toml").read_text(encoding="utf-8"))
    table = pixi.get("workspace") or pixi.get("project") or {}
    return (version[1] if version else ""), str(table.get("version", ""))


def arrow_pins(root: Path) -> dict[str, tuple[str, int]]:
    pins = {}
    for line in (root / "cmake/static/arrow-sources.lock").read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            name, sha, size = line.split()[:3]
            pins[name] = (sha, int(size))
    return pins


def glibc_pins(root: Path) -> dict[str, tuple[str, str, int]]:
    """sysroot build -> (source RPM, sha256, bytes)."""
    pins = {}
    for line in (root / "cmake/static/glibc-source.lock").read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            build, source, sha, size = line.split()[:4]
            pins[build] = (source, sha, int(size))
    return pins


def conda_pins(lock: Path, subdir: str) -> dict[str, str]:
    """pixi.lock (version 7): file name -> sha256 of every conda package record of `subdir` (or noarch)."""
    pins: dict[str, str] = {}
    current = None
    for line in lock.read_text(encoding="utf-8").splitlines():
        if line.startswith("- "):  # a package record; the environments' lists are indented
            current = None
            if line.startswith("- conda: "):
                parts = line[len("- conda: ") :].strip().rsplit("/", 2)
                if len(parts) == 3 and parts[1] in (subdir, "noarch"):
                    current = parts[2]
        elif current and line.startswith("  sha256: "):
            pins[current] = line.split(":", 1)[1].strip()
            current = None
    return pins


# ---------------------------------------------------------------------------------------------------------------------
# GitHub state


@dataclass
class Tag:
    kind: str  # the ref's object: "tag" (annotated) or "commit" (lightweight)
    object_sha: str  # that object: the tag object, or the commit
    target_kind: str  # what the tag object points at ("commit" unless it is a tag of a tag)
    target_sha: str
    name: str  # the tag object's own name
    message: str  # the annotation without its signature
    verified: bool


def get_tag(repo: str, tag: str) -> Tag | None:
    ref = api("GET", f"repos/{repo}/git/ref/tags/{tag}")  # exact match or 404 (git/refs/ would match prefixes)
    if ref.status == 404:
        return None
    obj = ref.ok().json()["object"]
    if obj["type"] == "commit":
        return Tag("commit", obj["sha"], "commit", obj["sha"], tag, "", False)
    if obj["type"] != "tag":
        fail(f"refs/tags/{tag} points at a {obj['type']}")
    data = api("GET", f"repos/{repo}/git/tags/{obj['sha']}").ok().json()
    verification = data.get("verification") or {}
    message, signature = data.get("message") or "", verification.get("signature") or ""
    if signature and message.endswith(signature):  # the API's message ends with the PGP or SSH signature
        message = message[: -len(signature)]
    target = data.get("object") or {}
    return Tag("tag", obj["sha"], target.get("type", ""), target.get("sha", ""), data.get("tag", ""),
               message.strip(), verification.get("verified") is True)  # fmt: skip


def list_releases(repo: str) -> list[dict[str, Any]]:
    """Every release, drafts included when the token has push access (contents: write)."""
    return pages(f"repos/{repo}/releases")


def release_by_id(repo: str, release_id: str) -> dict[str, Any]:
    got = api("GET", f"repos/{repo}/releases/{release_id}")
    if got.status == 404:
        fail(f"release {release_id} no longer exists (deleted by hand?): re-run the failed jobs of this run")
    return got.ok().json()


def delete_drafts(repo: str, tag: str, releases: list[dict[str, Any]]) -> None:
    """Drafts are not unique per tag and get-by-tag never returns one: delete every draft of the tag."""
    for release in releases:
        if release.get("draft") and release.get("tag_name") == tag:
            log(f"deleting draft {release['id']} of {tag} ({len(release.get('assets', []))} assets)")
            api("DELETE", f"repos/{repo}/releases/{release['id']}").ok()


# ---------------------------------------------------------------------------------------------------------------------
# preflight


class Checks:
    """On a tag push the first miss fails the job; on a dry run every miss is a warning. A warning is one either way."""

    def __init__(self, dry_run: bool) -> None:
        self.dry_run = dry_run
        self.rows: list[tuple[str, str, str]] = []

    def ok(self, what: str, detail: str) -> None:
        self.rows.append(("ok", what, detail))
        log(f"ok    {what}: {detail}")

    def miss(self, what: str, why: str) -> None:
        self.rows.append(("warn" if self.dry_run else "FAIL", what, why))
        if not self.dry_run:
            fail(f"{what}: {why}")
        log(f"warn  {what}: {why} (a tag push would stop here)")
        annotate("warning", f"{what}: {why}")

    def warn(self, what: str, why: str) -> None:
        self.rows.append(("warn", what, why))
        log(f"warn  {what}: {why}")
        annotate("warning", f"{what}: {why}")

    def table(self) -> str:
        rows = "".join(f"| {a} | {b} | {c} |\n" for a, b, c in self.rows)
        return "| | check | result |\n| --- | --- | --- |\n" + rows


def check_versions(root: Path, checks: Checks) -> str:
    cmake, pixi = project_version(root)
    if not VERSION_RE.fullmatch(cmake):
        fail(f"CMakeLists.txt: project() has no VERSION <major>.<minor>.<patch> (found {cmake!r})")
    if pixi != cmake:  # a dry run fails here too: the binary and BUILDINFO.json say CMake's
        fail(f"CMakeLists.txt says {cmake}, pixi.toml says {pixi!r}: bump both in one PR")
    checks.ok("version", f"{cmake} in CMakeLists.txt and pixi.toml")
    return cmake


def check_tag(repo: str, tag: str, sha: str, checks: Checks, pushed: bool) -> Tag | None:
    found = get_tag(repo, tag)
    if found is None:
        if pushed:
            checks.miss("tag", f"refs/tags/{tag} does not exist any more")
        else:
            log(f"note  tag: {tag} does not exist yet")
        return None
    if found.kind != "tag":
        checks.miss("tag", f"{tag} is a lightweight tag: push an annotated one (git tag -a or -s)")
    elif found.name != tag or found.target_kind != "commit":
        checks.miss("tag", f"the tag object of {tag} is named {found.name!r} and points at a {found.target_kind}")
    elif found.target_sha != sha:
        checks.miss("tag", f"{tag} peels to {found.target_sha}, not to the commit of this run {sha}")
    elif REQUIRE_SIGNED_TAG and not found.verified:
        checks.miss("tag", f"{tag} has no signature that GitHub verifies")
    else:
        checks.ok("tag", f"{tag} is annotated ({found.object_sha[:12]}) and peels to {sha[:12]}")
    return found


def check_on_main(repo: str, sha: str, checks: Checks) -> None:
    """The commit is main or an ancestor of it. A warning when main has changed .github/workflows/ since: the Releases
    API's workflow-file rule should not apply to a release on an existing tag (the docs resolve its target to the
    tag's commit only when target_commitish is sent, and release.py never sends it), but if it does it shows only at
    the draft, after the build."""
    data = api("GET", f"repos/{repo}/compare/{sha}...{MAIN}?per_page=1").ok().json()
    status = data.get("status")
    if status not in ("identical", "ahead"):  # main is the commit or contains it
        checks.miss("main", f"{sha[:12]} is not on {MAIN} (compare: {status}, {data.get('behind_by')} behind)")
        return
    checks.ok("main", f"{sha[:12]} is on {MAIN} ({status}, {data.get('ahead_by', 0)} commits behind it)")
    files = data.get("files") or []  # page 1 lists up to 300 changed files of the whole comparison
    keys = ("filename", "previous_filename")
    workflows = sorted({f.get("filename", "") for f in files
                        if any(str(f.get(key, "")).startswith(".github/workflows/") for key in keys)})  # fmt: skip
    if workflows or len(files) >= 300:
        checks.warn("main", f"{MAIN} changed {workflows or 'possibly'} .github/workflows/ since {sha[:12]}: "
                            f"{WORKFLOW_RULE}")  # fmt: skip


def check_ci_ok(repo: str, sha: str, checks: Checks) -> None:
    path = f"repos/{repo}/commits/{sha}/check-runs?check_name=CI%20OK&app_id={ACTIONS_APP_ID}&filter=all"
    runs = [run for run in pages(path, "check_runs")
            if run.get("name") == CI_CHECK and (run.get("app") or {}).get("id") == ACTIONS_APP_ID]  # fmt: skip
    if not runs:
        checks.miss("CI OK", f"has not reported on {sha[:12]} yet: re-run this workflow once it is green")
        return
    newest = max(runs, key=lambda run: (run.get("started_at") or "", run["id"]))
    if newest.get("status") != "completed" or newest.get("conclusion") != "success":
        state = newest.get("conclusion") or newest.get("status")
        checks.miss("CI OK", f"the newest run on {sha[:12]} is {state} ({newest.get('details_url')})")
        return
    suite = (newest.get("check_suite") or {}).get("id")
    found = api("GET", f"repos/{repo}/actions/runs?check_suite_id={suite}").ok().json().get("workflow_runs") or []
    run = found[0] if len(found) == 1 else {}
    if run.get("path") != CI_WORKFLOW or run.get("head_sha") != sha or run.get("event") not in CI_EVENTS:
        checks.miss("CI OK", f"the newest green one is not from a {CI_WORKFLOW} run on {sha[:12]}: "
                             f"{[(r.get('path'), r.get('event'), r.get('head_sha')) for r in found]}")  # fmt: skip
        return
    checks.ok("CI OK", f"green in {CI_WORKFLOW} ({run.get('event')}, {run.get('html_url', run.get('id'))})")


def check_unreleased(repo: str, tag: str, checks: Checks) -> None:
    got = api("GET", f"repos/{repo}/releases/tags/{tag}")  # published releases only
    if got.status == 404:  # only a 404 means none: a 403 or 5xx fails below
        checks.ok("release", f"no published release has {tag}")
        return
    release = got.ok().json()
    checks.miss("release", f"{tag} is already released ({release.get('html_url')}); published releases are "
                           "immutable and their tags can never be reused: bump the version")  # fmt: skip


def check_environment(repo: str, root: Path, checks: Checks) -> None:
    """The `release` environment as tools/github/environment-release.json sets it. A workflow that names a missing
    environment creates it without protection, so a missing one fails a tag push here, before the build."""
    want = json.loads((root / ENVIRONMENT_FILE).read_text())
    got = api("GET", f"repos/{repo}/environments/{ENVIRONMENT}")
    fix = "apply it with `bash tools/github/apply-settings.sh --apply --release`"
    if got.status == 404:
        checks.miss("environment", f"there is no `{ENVIRONMENT}` environment: {fix}")
        return
    data = got.ok().json()
    rules = {rule.get("type"): rule for rule in data.get("protection_rules") or []}
    reviewers = rules.get("required_reviewers") or {}
    seen = {
        "reviewers": sorted(
            (r.get("type"), (r.get("reviewer") or {}).get("id")) for r in reviewers.get("reviewers", [])
        ),
        "prevent_self_review": reviewers.get("prevent_self_review", False),
        "wait_timer": (rules.get("wait_timer") or {}).get("wait_timer", 0),
        "can_admins_bypass": data.get("can_admins_bypass"),
        "deployment_branch_policy": data.get("deployment_branch_policy"),
    }
    expected = {
        "reviewers": sorted((r["type"], r["id"]) for r in want["reviewers"]),
        "prevent_self_review": want["prevent_self_review"],
        "wait_timer": want["wait_timer"],
        "can_admins_bypass": want["can_admins_bypass"],
        "deployment_branch_policy": want["deployment_branch_policy"],
    }
    differ = {key: seen[key] for key in expected if seen[key] != expected[key]}
    policies = api("GET", f"repos/{repo}/environments/{ENVIRONMENT}/deployment-branch-policies").ok().json() or {}
    found = sorted((p.get("name"), p.get("type")) for p in policies.get("branch_policies", []))
    if found != sorted(TAG_POLICIES):
        differ["deployment policies"] = found
    if differ:
        checks.miss("environment", f"`{ENVIRONMENT}` differs from {ENVIRONMENT_FILE} in {differ}: {fix}")
    else:
        checks.ok("environment", f"`{ENVIRONMENT}` as {ENVIRONMENT_FILE} sets it, deployments from v* tags only")


def ruleset_differs(want: dict[str, Any], got: dict[str, Any]) -> dict[str, Any]:
    """The fields of a tag ruleset file that the live ruleset does not have. Server-side defaults the file does not set
    are ignored, and so are the bypass actors, which a token without admin access cannot see."""
    differ = {key: got.get(key) for key in ("target", "enforcement", "conditions") if got.get(key) != want[key]}
    live = {rule.get("type"): rule.get("parameters") or {} for rule in got.get("rules") or []}
    if sorted(live) != sorted(rule["type"] for rule in want["rules"]):
        differ["rules"] = sorted(live)
    for rule in want["rules"]:
        params = live.get(rule["type"], {})
        wrong = {k: params.get(k) for k, v in (rule.get("parameters") or {}).items() if params.get(k) != v}
        if wrong:
            differ[rule["type"]] = wrong
    return differ


def check_rulesets(repo: str, root: Path, checks: Checks) -> None:
    listed = {r.get("name"): r for r in pages(f"repos/{repo}/rulesets") if r.get("target") == "tag"}
    for name, path in RULESET_FILES.items():
        want = json.loads((root / path).read_text())
        if name not in listed:
            checks.miss("tag rulesets", f"there is no tag ruleset `{name}` ({path}): apply it with "
                                        "`bash tools/github/apply-settings.sh --apply --release`")  # fmt: skip
            continue
        got = api("GET", f"repos/{repo}/rulesets/{listed[name]['id']}").ok().json()
        differ = ruleset_differs(want, got)
        if differ:
            checks.miss("tag rulesets", f"`{name}` differs from {path} in {differ}")
        else:
            checks.ok("tag rulesets", f"`{name}` as {path} sets it (bypass actors are not visible to this token)")


def cmd_preflight(args: argparse.Namespace) -> None:
    repo, event = env("GITHUB_REPOSITORY", REPO_RE), env("GITHUB_EVENT_NAME")
    ref, sha = env("GITHUB_REF"), env("GITHUB_SHA", SHA_RE)
    if event not in ("push", "workflow_dispatch"):
        fail(f"release.yml runs on tag pushes and by hand, not on {event}")
    pushed = event == "push"
    if pushed and repo != UPSTREAM:
        fail(f"a tag push publishes only from {UPSTREAM}; run release.yml by hand in {repo} (a dry run)")
    root = Path(args.root)
    checks = Checks(dry_run=not pushed)
    version = check_versions(root, checks)
    if ref.startswith("refs/tags/"):
        tag = ref.removeprefix("refs/tags/")
    elif pushed:
        fail(f"$GITHUB_REF {ref} is not a tag")
    else:
        tag = f"v{version}"  # a dry run of a branch rehearses the release of its version
    match = TAG_RE.fullmatch(tag)
    valid = match is not None and match["version"] == version
    tag_object = ""
    if not valid:
        checks.miss("tag", f"{tag} is not v{version} or v{version}-rc.<N> (CMakeLists.txt's VERSION)")
        tag = f"v{version}"  # what a dry run rehearses instead
    else:
        found = check_tag(repo, tag, sha, checks, pushed)
        tag_object = found.object_sha if found and found.kind == "tag" else ""
    check_on_main(repo, sha, checks)
    check_ci_ok(repo, sha, checks)
    check_unreleased(repo, tag, checks)
    check_environment(repo, root, checks)
    check_rulesets(repo, root, checks)
    publish = pushed  # a tag push that got here passed every check
    prerelease = valid and match is not None and bool(match["rc"])
    summary(f"### Release preflight: {tag} at {sha[:12]} ({'release' if publish else 'dry run'})\n\n{checks.table()}")
    outputs(sha=sha, version=version, tag=tag, tag_object=tag_object, prerelease=str(prerelease).lower(),
            publish=str(publish).lower())  # fmt: skip


# ---------------------------------------------------------------------------------------------------------------------
# what preflight decided, for the later jobs


@dataclass(frozen=True)
class Release:
    repo: str
    tag: str
    sha: str
    version: str
    prerelease: bool
    publish: bool

    @property
    def title(self) -> str:
        return f"antb1 {self.tag.removeprefix('v')}"


def release_from_env() -> Release:
    repo, tag = env("GITHUB_REPOSITORY", REPO_RE), env("RELEASE_TAG", TAG_RE)
    version, sha = env("RELEASE_VERSION", VERSION_RE), env("RELEASE_SHA", SHA_RE)
    prerelease, publish = flag("RELEASE_PRERELEASE"), flag("RELEASE_PUBLISH")
    match = TAG_RE.fullmatch(tag)
    assert match is not None
    if match["version"] != version or bool(match["rc"]) != prerelease:
        fail(f"RELEASE_TAG {tag}, RELEASE_VERSION {version} and RELEASE_PRERELEASE {prerelease} disagree")
    return Release(repo, tag, sha, version, prerelease, publish)


def subjects_from_env() -> dict[str, str]:
    """$RELEASE_SUBJECTS: assemble's job output, the digests that attest signed (name -> sha256, all of SUBJECTS)."""
    subjects = read_sums(env("RELEASE_SUBJECTS").rstrip("\n") + "\n")
    if sorted(subjects) != sorted(SUBJECTS):
        fail(f"$RELEASE_SUBJECTS lists {sorted(subjects)}, expected {sorted(SUBJECTS)}")
    return subjects


def require_tag_push(rel: Release) -> None:
    """draft and publish run only in the tag-push run that preflight passed, in the upstream repository."""
    run = (os.environ.get("GITHUB_EVENT_NAME"), os.environ.get("GITHUB_REF"), os.environ.get("GITHUB_SHA"))
    if not rel.publish or rel.repo != UPSTREAM or run != ("push", f"refs/tags/{rel.tag}", rel.sha):
        fail(f"only a tag push of {rel.tag} in {UPSTREAM} that preflight passed drafts or publishes a release")


# ---------------------------------------------------------------------------------------------------------------------
# assemble


def read_sums(text: str, paths: bool = False) -> dict[str, str]:
    """A sha256sum file: one `<sha256>  <name>` line each (` *` is accepted), no repeats; `paths` allows a/b."""
    sums: dict[str, str] = {}
    lines = text.split("\n")
    if lines[-1] != "":
        fail("a checksum file does not end with a line break")
    for no, line in enumerate(lines[:-1], 1):
        match = SUM_RE.fullmatch(line)
        name = match["name"] if match else ""
        safe = bool(name) and not name.startswith("/") and ".." not in name.split("/") and (paths or "/" not in name)
        if match is None or not safe or name in sums:
            fail(f"line {no} of a checksum file is malformed or repeated: {line!r}")
        sums[name] = match["digest"]
    return sums


def exact_files(directory: Path, names: list[str]) -> None:
    if directory.is_symlink() or not directory.is_dir():
        fail(f"{directory} is not a directory")
    found = sorted(path.name for path in directory.iterdir())
    if found != sorted(names) or any((directory / name).is_symlink() or not (directory / name).is_file()
                                     for name in names):  # fmt: skip
        fail(f"{directory} holds {found}, expected exactly the regular files {sorted(names)}")


def artifact_files(src: Path) -> dict[str, Path]:
    """The seven tarballs in the downloaded build artifacts (one directory each, static.yml's names), each checked
    against its own target's SHA256SUMS or .sha256 sidecar."""
    if src.is_symlink() or not src.is_dir():
        fail(f"{src} is not a directory")
    found_dirs = sorted(path.name for path in src.iterdir())
    if found_dirs != ARTIFACTS or any((src / name).is_symlink() for name in ARTIFACTS):
        fail(f"{src} holds {found_dirs}, expected the artifacts {ARTIFACTS}")
    paths: dict[str, Path] = {}
    for target in TARGETS:
        pkg = src / f"antb1-{target}"
        names = [f"antb1-{target}.tar.gz"] + ([f"antb1-{target}.debug.tar.gz"] if target in CONDA_SUBDIR else [])
        exact_files(pkg, [*names, SUMS])
        sums = read_sums((pkg / SUMS).read_text())
        if sorted(sums) != sorted(names):
            fail(f"{pkg.name}/{SUMS} lists {sorted(sums)}, expected exactly {sorted(names)}")
        located = {name: pkg / name for name in names}
        if target in CONDA_SUBDIR:
            sources = f"antb1-{target}.sources.tar"
            exact_files(src / f"{pkg.name}-sources", [sources, f"{sources}.sha256"])
            sidecar = read_sums((src / f"{pkg.name}-sources" / f"{sources}.sha256").read_text())
            if sorted(sidecar) != [sources]:
                fail(f"{sources}.sha256 lists {sorted(sidecar)}, expected only {sources}")
            sums |= sidecar
            located[sources] = src / f"{pkg.name}-sources" / sources
        for name, path in located.items():
            if path.stat().st_size >= ASSET_LIMIT or sha256_file(path) != sums[name]:
                fail(f"{path} does not match its checksum (or is 2 GiB or larger)")
        paths |= located
    if sorted(paths) != sorted(release_files()):
        fail(f"the artifacts hold {sorted(paths)}, expected {sorted(release_files())}")
    return paths


def buildinfo(package: Path, rel: Release, target: str) -> dict[str, Any]:
    member = f"antb1-{rel.version}-{target}/BUILDINFO.json"
    with tarfile.open(package, "r:gz") as tar:
        try:
            info = tar.getmember(member)
        except KeyError:
            fail(f"{package.name} has no {member}")
        stream = tar.extractfile(info) if info.isreg() else None
        if stream is None:
            fail(f"{package.name}: {member} is not a regular file")
        return json.loads(stream.read())


def check_buildinfo(info: dict[str, Any], rel: Release, target: str, root: Path, glibc: dict) -> None:
    server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
    want = {"name": "antb1", "version": rel.version, "commit": rel.sha, "target": target, "flavor": "static-thinlto",
            "license": "Apache-2.0", "source": f"{server}/{rel.repo}/tree/{rel.sha}",
            "pixi_lock_sha256": sha256_file(root / "pixi.lock")}  # fmt: skip
    if rel.publish or "tag" in info:  # scripts/static.sh records the tag on tag runs only
        want["tag"] = rel.tag
    wrong = {key: info.get(key) for key, value in want.items() if info.get(key) != value}
    if wrong:
        fail(f"antb1-{target}.tar.gz: BUILDINFO.json has {wrong}, expected { ({k: want[k] for k in wrong}) }")
    if target in CONDA_SUBDIR:
        build = info.get("sysroot", "")
        if not build.startswith(f"sysroot_{CONDA_SUBDIR[target]}-") or build not in glibc:
            fail(f"antb1-{target}.tar.gz: sysroot build {build!r} has no line in cmake/static/glibc-source.lock")
        if info.get("glibc_source") != glibc[build][0]:
            fail(f"antb1-{target}.tar.gz names glibc source {info.get('glibc_source')!r}, the lock {glibc[build][0]}")
    elif "sysroot" in info or "glibc_source" in info:
        fail(f"antb1-{target}.tar.gz: a macOS package names a glibc")


def check_sources(path: Path, rel: Release, target: str, info: dict[str, Any], root: Path, glibc: dict) -> None:
    """The members of antb1-<target>.sources.tar (scripts/static_package.py sources) against the committed pins."""
    top = f"antb1-{target}-sources"
    srpm, srpm_sha, srpm_size = glibc[info["sysroot"]]
    arrow = arrow_pins(root)
    conda = conda_pins(root / "pixi.lock", CONDA_SUBDIR[target])
    with tarfile.open(path, "r:") as tar:
        members: dict[str, tarfile.TarInfo] = {}
        for member in tar.getmembers():
            name = member.name
            if (
                name in members
                or name.startswith("/")
                or ".." in name.split("/")
                or not (member.isdir() or member.isreg())
            ):
                fail(f"{path.name}: {name} is repeated, outside the tarball or not a directory or regular file")
            members[name] = member
        dirs = {name for name, member in members.items() if member.isdir()}
        files = {name: member for name, member in members.items() if member.isreg()}
        if dirs != {top, f"{top}/third-party"}:
            fail(f"{path.name} has the directories {sorted(dirs)}, expected {top}/ and {top}/third-party/")
        want = {f"{top}/README.txt", f"{top}/SHA256SUMS", f"{top}/{srpm}", f"{top}/antb1-source.tar.gz"}
        want |= {f"{top}/third-party/{name}" for name in arrow}
        packages = {}
        for name in sorted(set(files) - want):
            base = name.removeprefix(f"{top}/third-party/")
            package = next((p for p in COMPILED_PACKAGES if base.startswith(p + "-") and base.endswith(".conda")), "")
            if base == name or not package or package in packages:
                fail(f"{path.name}: unexpected member {name}")
            packages[package] = base
        if sorted(packages) != sorted(COMPILED_PACKAGES) or not want <= set(files):
            fail(f"{path.name} misses {sorted(want - set(files))} or a conda archive of {COMPILED_PACKAGES}")
        digests = {name: sha256_stream(tar.extractfile(member)) for name, member in files.items()}
        listed = read_sums(tar.extractfile(files[f"{top}/SHA256SUMS"]).read().decode(), paths=True)
        expected = {name.removeprefix(f"{top}/"): digest for name, digest in digests.items()
                    if name not in (f"{top}/README.txt", f"{top}/SHA256SUMS")}  # fmt: skip
        if listed != expected:
            fail(f"{path.name}: its SHA256SUMS does not list exactly its files with their digests")
        pinned = {srpm: (srpm_sha, srpm_size)} | {f"third-party/{n}": pin for n, pin in arrow.items()}
        for name, (sha, size) in pinned.items():
            if digests[f"{top}/{name}"] != sha or files[f"{top}/{name}"].size != size:
                fail(f"{path.name}: {name} is not the file the lock pins (sha256 {sha}, {size} bytes)")
        for base in packages.values():
            if conda.get(base) != digests[f"{top}/third-party/{base}"]:
                fail(f"{path.name}: third-party/{base} is not the {CONDA_SUBDIR[target]} archive that pixi.lock pins")
        check_antb1_source(tar.extractfile(files[f"{top}/antb1-source.tar.gz"]).read(), rel, root, path.name)
        readme = tar.extractfile(files[f"{top}/README.txt"]).read().decode(errors="replace")
        if rel.sha not in readme or srpm not in readme:
            fail(f"{path.name}: README.txt does not name commit {rel.sha} and {srpm}")


def check_antb1_source(data: bytes, rel: Release, root: Path, where: str) -> None:
    """`git archive --prefix=antb1-<version>/ <commit>`: git records the commit in the pax global header. (A deeper
    check would compare every member's git blob id with `git ls-tree -r <commit>`; git archive reads the commit, never
    the work tree, and the repository has no export-ignore or export-subst attribute, so the header suffices.)"""
    top = f"antb1-{rel.version}"
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        if tar.pax_headers.get("comment") != rel.sha:
            fail(f"{where}: antb1-source.tar.gz is not `git archive` of {rel.sha} ({tar.pax_headers.get('comment')})")
        names = tar.getnames()
        if any(name != top and not name.startswith(f"{top}/") for name in names):
            fail(f"{where}: antb1-source.tar.gz has members outside {top}/")
        licence = tar.extractfile(f"{top}/LICENSE")
        if licence is None or licence.read() != (root / "LICENSE").read_bytes():
            fail(f"{where}: antb1-source.tar.gz has another LICENSE than the commit")


def render_notes(rel: Release, files: dict[str, tuple[str, int]], tag: Tag | None) -> str:
    """The release body: the annotation, the assets, how to install and verify, the platforms and the licences.
    Nothing measured goes here (no benchmark figures, nothing derived from ClickBench or TPC-H)."""
    base = f"https://github.com/{rel.repo}/releases"
    server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
    run = f"{server}/{rel.repo}/actions/runs/{os.environ.get('GITHUB_RUN_ID', '?')}"
    what = {
        "tar.gz": "antb1 for {p}: the executable, LICENSE, NOTICE, THIRD_PARTY_NOTICES.txt, BUILDINFO.json, README.txt",
        "debug.tar.gz": "its line tables (`antb1.debug`): unpack it over the package, next to the binary",
        "sources.tar": "the sources that go with it (LGPL-2.1 section 6): glibc's source RPM, antb1's source and every "
        "other library linked in, but the GCC runtime; its README.txt says how to rebuild and relink",
    }
    platform = {"linux-x86_64": "Linux x86-64", "linux-aarch64": "Linux AArch64", "darwin-arm64": "macOS (arm64)"}
    out = []
    if not rel.publish:
        out += [f"**Dry run** of {rel.sha}: not a release; nothing is attested, drafted or published.", ""]
    if tag is not None and tag.message:
        out += [tag.message, ""]
    if rel.prerelease:
        out += [f"A release candidate of antb1 {rel.version}: a prerelease, never the latest release.", ""]
    out += [f"Built from commit {rel.sha} by `{RELEASE_WORKFLOW}` ({run}).", ""]
    out += ["| Asset | What | Bytes | SHA-256 |", "| --- | --- | ---: | --- |"]
    for name in release_files():
        target = name.removeprefix("antb1-").split(".", 1)[0]
        kind = name.split(".", 1)[1]
        digest, size = files[name]
        out.append(f"| [`{name}`]({base}/download/{rel.tag}/{name}) | {what[kind].format(p=platform[target])} "
                   f"| {size} | `{digest}` |")  # fmt: skip
    digest, size = files[SUMS]
    out.append(f"| [`{SUMS}`]({base}/download/{rel.tag}/{SUMS}) | the SHA-256 of every tarball | {size} | `{digest}` |")
    out.append(f"| [`{BUNDLE}`]({base}/download/{rel.tag}/{BUNDLE}) | the build provenance of the assets above "
               "(a Sigstore bundle) | | |")  # fmt: skip
    latest = "" if rel.prerelease else f" (or `{base}/latest/download/<asset>` for the latest release)"
    sample = "antb1-linux-x86_64.tar.gz"
    out += ["", "## Install", "", f"Download an asset from `{base}/download/{rel.tag}/<asset>`{latest}:", "", "```sh",
            f"curl -fsSLO {base}/download/{rel.tag}/{sample}", f"curl -fsSLO {base}/download/{rel.tag}/{SUMS}",
            f"grep ' {sample}$' {SUMS} | sha256sum -c -    # macOS: shasum -a 256 -c -",
            f"tar -xzf {sample} && ./antb1-{rel.version}-linux-x86_64/antb1 version --verbose", "```", "",
            "On macOS download with curl, not a browser (a browser adds the quarantine attribute).", "",
            "## Verify", "", "```sh",
            f"gh attestation verify {sample} --repo {rel.repo} --signer-workflow {rel.repo}/{RELEASE_WORKFLOW} \\",
            f"  --source-ref refs/tags/{rel.tag} --deny-self-hosted-runners    # offline: see SECURITY.md",
            f"gh release verify {rel.tag} --repo {rel.repo}",
            f"gh release verify-asset {rel.tag} {sample} --repo {rel.repo}", "```", "",
            "## Platforms", "",
            "- Linux x86-64: any distribution with kernel 3.2 or newer and an x86-64-v3 CPU; an older CPU gets a "
            "message.", "- Linux AArch64: kernel 3.7 or newer, Armv8.2-A.", "- macOS 13 or newer on Apple silicon.",
            "", "## Licences", "",
            "antb1 is licensed under the Apache License 2.0 (LICENSE and NOTICE in every tarball). "
            "THIRD_PARTY_NOTICES.txt lists every library linked into the binary, with its licence. The Linux binaries "
            "contain the GNU C Library 2.28, linked statically, under the GNU LGPL 2.1 or later: "
            "`antb1-linux-<arch>.sources.tar` in this release holds its source, antb1's source and the sources of "
            "every other library linked in, except the GCC runtime libraries (libstdc++, libgcc) that come with the "
            "compiler."]  # fmt: skip
    text = "\n".join(out) + "\n"
    if len(text) > NOTES_LIMIT:
        fail(f"notes.md has {len(text)} characters; a release body takes {NOTES_LIMIT}")
    return text


def cmd_assemble(args: argparse.Namespace) -> None:
    rel = release_from_env()
    root, src, out = Path(args.root), Path(args.input), Path(args.out)
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        fail(f"{out} is not a new or empty directory")
    glibc = glibc_pins(root)
    paths = artifact_files(src)
    digests = {name: sha256_file(path) for name, path in paths.items()}
    for target in TARGETS:
        info = buildinfo(paths[f"antb1-{target}.tar.gz"], rel, target)
        check_buildinfo(info, rel, target, root, glibc)
        if target in CONDA_SUBDIR:
            check_sources(paths[f"antb1-{target}.sources.tar"], rel, target, info, root, glibc)
    sums_text = "".join(f"{digests[name]}  {name}\n" for name in sorted(digests))
    out.mkdir(parents=True, exist_ok=True)
    (out / SUMS).write_text(sums_text)
    sums_digest = sha256_file(out / SUMS)
    subjects = sums_text + f"{sums_digest}  {SUMS}\n"
    output_lines("subjects", subjects)
    tag = None
    if rel.publish:
        tag = get_tag(rel.repo, rel.tag)
        if tag is None or tag.kind != "tag" or tag.target_sha != rel.sha:
            fail(f"{rel.tag} no longer peels to {rel.sha}")
    files = {name: (digest, paths[name].stat().st_size) for name, digest in digests.items()}
    files[SUMS] = (sums_digest, (out / SUMS).stat().st_size)
    (out / "notes.md").write_text(render_notes(rel, files, tag))
    summary(f"### Release assets of {rel.tag}\n\n```\n{sums_text}```")
    log(f"checked {len(paths)} tarballs; wrote {SUMS} and notes.md to {out}")


# ---------------------------------------------------------------------------------------------------------------------
# provenance


def attested_subjects(path: Path, rel: Release, bundle: Path | None) -> list[list[tuple[str, str]]]:
    """`gh attestation verify` of one file: the Sigstore signature, the certificate identity (this repository's
    release.yml at refs/tags/TAG on commit SHA, a GitHub-hosted runner) and that the file's sha256 is a subject.
    Returns the subjects of every verified attestation as sorted (name, sha256) lists (a repeated name stays
    repeated); the statement is signed, so its subject list is the attest job's."""
    args = ["attestation", "verify", str(path), "--repo", rel.repo,
            "--signer-workflow", f"{rel.repo}/{RELEASE_WORKFLOW}", "--source-ref", f"refs/tags/{rel.tag}",
            "--source-digest", rel.sha, "--deny-self-hosted-runners", "--format", "json"]  # fmt: skip
    if bundle is not None:
        args += ["--bundle", str(bundle)]
    proc = gh(*args, check=False)
    if proc.returncode != 0:
        fail(f"{path.name} does not verify{' against ' + bundle.name if bundle else ''}: "
             f"{proc.stderr.decode(errors='replace').strip()}")  # fmt: skip
    results = json.loads(proc.stdout)
    statements = [entry["verificationResult"]["statement"] for entry in results] if isinstance(results, list) else []
    if not statements or any(st.get("predicateType") != SLSA_PROVENANCE for st in statements):
        fail(f"{path.name}: gh attestation verify returned no SLSA provenance statement")
    return [sorted((s.get("name", ""), (s.get("digest") or {}).get("sha256", "")) for s in st.get("subject", []))
            for st in statements]  # fmt: skip


def check_against_bundle(paths: dict[str, Path], bundle: Path, rel: Release, digests: dict[str, str]) -> None:
    """Every subject file verifies against the bundle, and the bundle attests exactly these files."""
    for path in paths.values():
        for subjects in attested_subjects(path, rel, bundle):
            if subjects != sorted(digests.items()):
                fail(f"{bundle.name} attests {sorted(subjects)} with other digests or names than the release assets")
    log(f"{len(paths)} assets verified against {bundle.name}")


# ---------------------------------------------------------------------------------------------------------------------
# draft


def check_release(got: dict[str, Any], rel: Release, files: dict[str, tuple[str, int]], draft: bool) -> None:
    """The release has the tag, the state, and exactly these assets: uploaded, with these sizes and digests."""
    if got.get("tag_name") != rel.tag or got.get("draft") is not draft or got.get("prerelease") is not rel.prerelease:
        fail(f"release {got.get('id')}: tag {got.get('tag_name')}, draft {got.get('draft')}, prerelease "
             f"{got.get('prerelease')}; expected {rel.tag}, {draft}, {rel.prerelease}")  # fmt: skip
    assets = {asset.get("name"): asset for asset in got.get("assets", [])}
    if len(assets) != len(got.get("assets", [])) or sorted(assets) != sorted(files):
        fail(f"release {got.get('id')} holds {sorted(assets)}, expected exactly {sorted(files)}")
    for name, (digest, size) in files.items():
        asset = assets[name]
        if asset.get("state") != "uploaded" or asset.get("digest") != f"sha256:{digest}" or asset.get("size") != size:
            fail(f"release {got.get('id')}: {name} is {asset.get('state')}, {asset.get('size')} bytes, "
                 f"{asset.get('digest')}; expected uploaded, {size} bytes, sha256:{digest}")  # fmt: skip


def drafts_of(repo: str, tag: str) -> list[dict[str, Any]]:
    return [r for r in list_releases(repo) if r.get("draft") and r.get("tag_name") == tag]


def draft_with_digests(rel: Release, release_id: str, files: dict[str, tuple[str, int]]) -> dict[str, Any]:
    """The draft once every asset has its digest: GitHub computes it at upload, but the first read may lag."""
    for attempt in range(RETRIES):
        got = release_by_id(rel.repo, release_id)
        if all(DIGEST_RE.fullmatch(a.get("digest") or "") for a in got.get("assets", [])) or attempt + 1 == RETRIES:
            break
        time.sleep(RETRY_DELAY)
    check_release(got, rel, files, draft=True)
    return got


def cmd_draft(args: argparse.Namespace) -> None:
    rel = release_from_env()
    require_tag_push(rel)
    meta, bundle = Path(args.meta), Path(args.bundle)
    paths = artifact_files(Path(args.artifacts))
    exact_files(meta, [SUMS, "notes.md"])
    paths[SUMS] = meta / SUMS
    files = {name: (sha256_file(path), path.stat().st_size) for name, path in paths.items()}
    if {name: digest for name, (digest, _) in files.items()} != subjects_from_env():
        fail("the artifacts are not what assemble checked (its job output `subjects`): an artifact was replaced")
    if read_sums((meta / SUMS).read_text()) != {name: files[name][0] for name in release_files()}:
        fail(f"{meta}/{SUMS} does not list exactly the tarballs with their digests")
    if bundle.name != BUNDLE or bundle.is_symlink() or not bundle.is_file():
        fail(f"{bundle} is not the bundle {BUNDLE}")
    check_against_bundle(paths, bundle, rel, {n: d for n, (d, _) in files.items()})
    files[BUNDLE] = (sha256_file(bundle), bundle.stat().st_size)
    releases = list_releases(rel.repo)
    published = [r for r in releases if r.get("tag_name") == rel.tag and not r.get("draft")]
    if published:
        fail(f"{rel.tag} is already released ({published[0].get('html_url')})")
    delete_drafts(rel.repo, rel.tag, releases)
    create = ["release", "create", rel.tag, "--repo", rel.repo, "--verify-tag", "--draft", "--title", rel.title,
              "--notes-file", str(meta / "notes.md")]  # fmt: skip
    if rel.prerelease:
        create.append("--prerelease")
    create += [str(paths[name]) for name in SUBJECTS] + [str(bundle)]  # never --target, --latest or generated notes
    proc = gh(*create, check=False, timeout=1800)
    if proc.returncode != 0:  # gh leaves a --draft release with the assets it uploaded so far
        delete_drafts(rel.repo, rel.tag, list_releases(rel.repo))
        error = proc.stderr.decode(errors="replace").strip()
        hint = f" ({WORKFLOW_RULE})" if re.search(r"HTTP (403|404)", error) else ""
        fail(f"gh release create failed: {error}{hint}")
    drafts = drafts_of(rel.repo, rel.tag)
    if len(drafts) != 1:
        fail(f"{len(drafts)} drafts of {rel.tag} after gh release create, expected one")
    got = draft_with_digests(rel, str(drafts[0]["id"]), files)
    summary(f"### Draft {rel.tag}\n\n{got.get('html_url')}: review it, then approve the `{ENVIRONMENT}` environment.")
    outputs(release_id=str(got["id"]))


# ---------------------------------------------------------------------------------------------------------------------
# publish


def make_latest(rel: Release, releases: list[dict[str, Any]]) -> str:
    """Sent explicitly (the API's default is true): true only for a stable version above every published stable."""
    if rel.prerelease:
        return "false"
    stable = []
    for release in releases:
        match = TAG_RE.fullmatch(release.get("tag_name", ""))
        if match and not match["rc"] and not release.get("draft") and not release.get("prerelease"):
            stable.append(version_key(match["version"]))
    return "true" if all(version_key(rel.version) > other for other in stable) else "false"


def download_and_verify(
    rel: Release, got: dict[str, Any], work: Path, expected: dict[str, str] | None
) -> dict[str, tuple[str, int]]:
    """Every asset of the release, downloaded by id; the subjects checked against `expected` (assemble's job output,
    when there is one) and against the release's own antb1.sigstore.json, which authenticates itself (signature and
    identity): no artifact of this run is trusted here."""
    assets = {asset.get("name"): asset for asset in got.get("assets", [])}
    if len(assets) != len(got.get("assets", [])) or sorted(assets) != sorted(ASSETS):
        fail(f"release {got.get('id')} holds {sorted(assets)}, expected exactly {sorted(ASSETS)}")
    work.mkdir(parents=True, exist_ok=True)
    files = {}
    for name in ASSETS:
        asset = assets[name]
        if asset.get("state") != "uploaded" or not DIGEST_RE.fullmatch(asset.get("digest") or ""):
            fail(f"{name} is {asset.get('state')} with digest {asset.get('digest')}")
        download_asset(rel.repo, asset["id"], work / name)
        digest, size = sha256_file(work / name), (work / name).stat().st_size
        if f"sha256:{digest}" != asset["digest"] or size != asset.get("size"):
            fail(f"{name}: the download ({size} bytes, sha256:{digest}) is not the asset {asset['digest']}")
        files[name] = (digest, size)
    subjects = {name: files[name][0] for name in SUBJECTS}
    if expected is not None and subjects != expected:
        fail(f"release {got.get('id')} does not hold the assets that assemble checked and attest signed")
    check_against_bundle({name: work / name for name in SUBJECTS}, work / BUNDLE, rel, subjects)
    if read_sums((work / SUMS).read_text()) != {name: files[name][0] for name in release_files()}:
        fail(f"{SUMS} of the release does not list exactly the tarballs with their digests")
    return files


def wait_published(rel: Release, release_id: str) -> dict[str, Any]:
    for attempt in range(RETRIES):
        got = release_by_id(rel.repo, release_id)
        if not got.get("draft") and got.get("immutable") is True:
            return got
        if attempt + 1 < RETRIES:
            time.sleep(RETRY_DELAY)
    fail(f"release {release_id} is draft={got.get('draft')} immutable={got.get('immutable')}: immutable releases "
         "are off (they cover only releases published after they are enabled); the release is public and mutable "
         "now: see docs/ci.md#releases")  # fmt: skip


def check_release_attestation(
    rel: Release, tag_object: str, release_id: str, files: dict[str, tuple[str, int]]
) -> None:
    """`gh release verify`: GitHub's own signed statement that the release of the tag holds exactly these assets."""
    for attempt in range(RETRIES):
        proc = gh("release", "verify", rel.tag, "--repo", rel.repo, "--format", "json", check=False)
        if proc.returncode == 0:
            break
        if attempt + 1 < RETRIES:
            time.sleep(RETRY_DELAY)
    else:
        fail(f"gh release verify {rel.tag}: {proc.stderr.decode(errors='replace').strip()}")
    statement = json.loads(proc.stdout)["verificationResult"]["statement"]
    predicate = statement.get("predicate") or {}
    about = (statement.get("predicateType"), predicate.get("tag"), predicate.get("repository"),
             str(predicate.get("databaseId")))  # fmt: skip
    if about != (RELEASE_PREDICATE, rel.tag, rel.repo, release_id):  # databaseId is the release id
        fail(f"the release attestation of {rel.tag} is about {about}")
    refs = [s for s in statement.get("subject", []) if "uri" in s]
    named = {s["name"]: (s.get("digest") or {}).get("sha256") for s in statement.get("subject", []) if "name" in s}
    tag_ok = len(refs) == 1 and refs[0]["uri"] == f"pkg:github/{rel.repo}@{rel.tag}"
    if not tag_ok or (refs[0].get("digest") or {}).get("sha1") != tag_object:  # the ref's object: the tag object
        fail(f"the release attestation of {rel.tag} names {refs}, expected the tag object {tag_object}")
    if named != {name: digest for name, (digest, _) in files.items()}:
        fail(f"the release attestation of {rel.tag} lists other assets: {sorted(named)}")
    log(f"gh release verify {rel.tag}: {len(named)} assets as published")


def cmd_publish(args: argparse.Namespace) -> None:
    rel = release_from_env()
    require_tag_push(rel)
    release_id, tag_object = env("RELEASE_ID", ID_RE), env("RELEASE_TAG_OBJECT", SHA_RE)
    subjects, work = subjects_from_env(), Path(args.work)
    got = release_by_id(rel.repo, release_id)
    if got.get("tag_name") != rel.tag:
        fail(f"release {release_id} is for {got.get('tag_name')}, not {rel.tag}")
    if got.get("draft"):
        tag = get_tag(rel.repo, rel.tag)
        seen = (tag.kind, tag.object_sha, tag.target_kind, tag.target_sha) if tag else None
        if seen != ("tag", tag_object, "commit", rel.sha):
            fail(f"{rel.tag} is no longer the annotated tag {tag_object} on {rel.sha} that preflight checked: {seen}")
        other = api("GET", f"repos/{rel.repo}/releases/tags/{rel.tag}")
        if other.status != 404:
            other.ok()
            fail(f"another release of {rel.tag} is published: {other.json().get('html_url')}")
        files = download_and_verify(rel, got, work, subjects)
        latest = make_latest(rel, list_releases(rel.repo))
        check_release(release_by_id(rel.repo, release_id), rel, files, draft=True)  # the last look before the PATCH
        patched = api("PATCH", f"repos/{rel.repo}/releases/{release_id}",
                      {"draft": False, "prerelease": rel.prerelease, "make_latest": latest})  # fmt: skip
        if patched.status in (403, 404):
            fail(f"publishing {rel.tag}: HTTP {patched.status} {patched.message()} ({WORKFLOW_RULE})")
        patched.ok()
        log(f"published {rel.tag} (make_latest={latest})")
    else:
        log(f"release {release_id} of {rel.tag} is already published (a re-run): verifying it only")
        files = download_and_verify(rel, got, work, subjects)
        latest = "true" if got.get("id") == latest_release_id(rel.repo) else "false"
    done = wait_published(rel, release_id)
    check_release(done, rel, files, draft=False)
    check_release_attestation(rel, tag_object, release_id, files)
    summary(f"### Published {rel.tag}\n\n{done.get('html_url')} (immutable)")
    outputs(latest=latest)


def latest_release_id(repo: str) -> int | None:
    got = api("GET", f"repos/{repo}/releases/latest")
    return None if got.status == 404 else got.ok().json().get("id")


# ---------------------------------------------------------------------------------------------------------------------
# check-published


def install_block(root: Path) -> str:
    """README.md's install block: the one ```sh fence between the install markers, which a user pastes."""
    text = (root / "README.md").read_text(encoding="utf-8")
    if text.count(INSTALL_BEGIN) != 1 or text.count(INSTALL_END) != 1:
        fail(f"README.md has not exactly one {INSTALL_BEGIN} ... {INSTALL_END} block")
    inner = text.split(INSTALL_BEGIN, 1)[1].split(INSTALL_END, 1)[0]
    match = re.fullmatch(r"\s*```sh\n(?P<code>.*?)```\s*", inner, re.DOTALL)
    if match is None:
        fail("README.md's install block is not one ```sh fence")
    return match["code"]


def cmd_check_published(args: argparse.Namespace) -> None:
    rel = release_from_env()
    target, work, root = args.target, Path(args.work), Path(args.root)
    latest = flag("RELEASE_LATEST")
    if work.exists() and (not work.is_dir() or any(work.iterdir())):
        fail(f"{work} is not a new or empty directory")
    got = api("GET", f"repos/{rel.repo}/releases/tags/{rel.tag}").ok().json()
    if got.get("draft") or got.get("immutable") is not True or got.get("prerelease") is not rel.prerelease:
        fail(f"{rel.tag}: draft={got.get('draft')} immutable={got.get('immutable')} prerelease={got.get('prerelease')}")
    tag = get_tag(rel.repo, rel.tag)
    if tag is None or tag.kind != "tag" or tag.target_sha != rel.sha:
        fail(f"{rel.tag} is not the annotated tag on {rel.sha}")
    block = install_block(root)
    written = f"/{rel.repo}/releases/latest/download"
    if written not in block:
        fail(f"README.md's install block does not download from {written}")
    runs = {"tag": block.replace(written, f"/{rel.repo}/releases/download/{rel.tag}")}
    if latest:
        runs["latest"] = block  # as a user pastes it
    tarball = f"antb1-{target}.tar.gz"
    for label, code in runs.items():
        directory = work / label
        directory.mkdir(parents=True)
        user = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(directory), "LC_ALL": "C"}  # no token
        proc = subprocess.run(["sh", "-e", "-c", code], cwd=directory, env=user, capture_output=True, text=True,
                              timeout=900, check=False)  # fmt: skip
        said = proc.stdout + proc.stderr
        if proc.returncode != 0:
            fail(f"README.md's install block ({label}) exited with {proc.returncode}: {said.strip()[-1500:]}")
        for want in (f"antb1 {rel.version} ", f"commit: {rel.sha}", f"target: {target}", "flavor: static-thinlto"):
            if want not in said:
                fail(f"README.md's install block ({label}) ran a binary that does not say {want!r}: {said[-1500:]}")
        log(f"README.md's install block ({label}): antb1 {rel.version} for {target} installed and runs")
    path = work / "tag" / tarball
    assets = {a.get("name"): a for a in got.get("assets", [])}
    if not path.is_file() or f"sha256:{sha256_file(path)}" != (assets.get(tarball) or {}).get("digest"):
        fail(f"{tarball} that the install block downloaded is not the release's asset")
    files = {name: (a["digest"].removeprefix("sha256:"), a.get("size")) for name, a in assets.items()}
    check_release_attestation(rel, tag.object_sha, str(got["id"]), files)
    gh("release", "verify-asset", rel.tag, str(path), "--repo", rel.repo)
    bundle = work / BUNDLE
    download_asset(rel.repo, assets[BUNDLE]["id"], bundle)
    attested_subjects(path, rel, None)  # online, as a user without the bundle: the attestations API
    attested_subjects(path, rel, bundle)  # offline, with the release's bundle
    log(f"{rel.tag}: {tarball} verifies (release attestation, asset, build provenance online and offline)")


# ---------------------------------------------------------------------------------------------------------------------


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("preflight")
    p.add_argument("--root", default=str(ROOT), help="the checkout of the tagged commit")
    a = sub.add_parser("assemble")
    a.add_argument("--in", dest="input", required=True, help="the downloaded build artifacts, one directory each")
    a.add_argument("--out", required=True, help="a new directory: SHA256SUMS and notes.md (release-meta)")
    a.add_argument("--root", default=str(ROOT))
    d = sub.add_parser("draft")
    d.add_argument("--artifacts", required=True, help="the downloaded build artifacts, as for assemble --in")
    d.add_argument("--meta", required=True, help="assemble's --out (release-meta)")
    d.add_argument("--bundle", required=True, help=f"the attest job's {BUNDLE}")
    u = sub.add_parser("publish")
    u.add_argument("--work", required=True, help="an empty directory for the downloaded assets")
    c = sub.add_parser("check-published")
    c.add_argument("--target", required=True, choices=TARGETS)
    c.add_argument("--work", required=True, help="an empty directory")
    c.add_argument("--root", default=str(ROOT), help="a checkout with README.md")
    args = parser.parse_args(argv)
    commands = {"preflight": cmd_preflight, "assemble": cmd_assemble, "draft": cmd_draft, "publish": cmd_publish,
                "check-published": cmd_check_published}  # fmt: skip
    try:
        commands[args.cmd](args)
    except (ReleaseError, KeyError, TypeError, ValueError) as error:  # an unexpected response shape fails closed too
        message = str(error)
        if not isinstance(error, ReleaseError):
            message = f"unexpected data ({type(error).__name__}: {error})"
        annotate("error", message)
        summary(f"**release.py {args.cmd} failed:** {message}")
        print(f"release: FAIL: {message}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
