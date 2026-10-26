"""Tests for .github/scripts/release.py, the logic of the Release workflow (docs/adr/0035-github-releases.md; run by
`pixi run lint`).

One fake GitHub (fake_gh.py: refs, check runs, the release environment, the tag rulesets, releases and their
attestations) carries a release from preflight through assemble, draft and publish to check-published; then each
refusal runs on its own, with the job's own token scope. The repository is a temporary checkout with small locks and
the tools/github files of this one; gh, curl and the binary are fakes. Nothing reaches the network or GitHub.
"""

from __future__ import annotations

import copy
import hashlib
import importlib.util
import io
import json
import os
import platform
import subprocess
import sys
import tarfile
from pathlib import Path
from types import ModuleType

import pytest
import yaml
from fakes import assert_fakes_first, fake_antb1, fake_download, install, serve

REPO = Path(__file__).resolve().parents[3]
RELEASE = REPO / ".github" / "scripts" / "release.py"
UPSTREAM = "ydb-campus/antb1"
SHA, TAG_OBJECT, OTHER = "a" * 40, "b" * 40, "c" * 40
VERSION, TAG, RC = "0.1.0", "v0.1.0", "v0.1.0-rc.1"
TARGETS = ("linux-x86_64", "linux-aarch64", "darwin-arm64")
SUBDIR = {"linux-x86_64": "linux-64", "linux-aarch64": "linux-aarch64"}
SYSROOT = {target: f"sysroot_{subdir}-2.28-hfake_9" for target, subdir in SUBDIR.items()}
SRPM, GLIBC = "glibc-2.28-1.el8_9.1.src.rpm", b"glibc source rpm"
ARROW = {"apache-arrow-25.0.0.tar.gz": b"arrow source", "thrift-0.22.0.tar.gz": b"thrift source"}
CONDA = {"cli11": "2.7.2", "libboost-headers": "1.88.0"}
HOST = {("Linux", "x86_64"): "linux-x86_64", ("Linux", "aarch64"): "linux-aarch64", ("Darwin", "arm64"): "darwin-arm64"}
HOST_TARGET = HOST.get((platform.system(), platform.machine()), "")
GH_RUNNER = """#!{python} -I
import runpy, sys
sys.argv[0] = {path!r}
runpy.run_path({path!r}, run_name="__main__")
"""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def ci_run(run_id: int, status: str, started: str, path: str = ".github/workflows/ci.yml", event: str = "push") -> dict:
    """A run of a workflow on the commit, as GET actions/workflows/<file>/runs lists it."""
    return {"id": run_id, "path": path, "head_sha": SHA, "event": event, "status": status, "run_started_at": started,
            "html_url": f"https://ci.test/{run_id}"}  # fmt: skip


def ci_job(conclusion: str | None) -> dict:
    return {"name": "CI OK", "status": "completed" if conclusion else "in_progress", "conclusion": conclusion}


def load_release() -> ModuleType:
    spec = importlib.util.spec_from_file_location("release_under_test", RELEASE)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["release_under_test"] = module  # dataclasses look their module up here
    spec.loader.exec_module(module)
    return module


def conda_archive(name: str, subdir: str) -> bytes:
    return f"{name} conda archive for {subdir}".encode()


def tar_bytes(members: dict[str, bytes], mode: str = "w:gz", dirs: tuple[str, ...] = (), **kw: object) -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode=mode, format=tarfile.PAX_FORMAT, **kw) as tar:
        for name in dirs:
            info = tarfile.TarInfo(name)
            info.type, info.mode = tarfile.DIRTYPE, 0o755
            tar.addfile(info)
        for name, data in members.items():
            info = tarfile.TarInfo(name)
            info.size, info.mode = len(data), 0o755 if name.endswith("/antb1") else 0o644
            tar.addfile(info, io.BytesIO(data))
    return buf.getvalue()


class World:
    """A checkout of the tagged commit, the build artifacts of a release run, and the GitHub that the fake gh shows."""

    def __init__(self, tmp: Path) -> None:
        self.tmp = tmp
        self.root = tmp / "root"
        self.bin = tmp / "bin"
        self.blobs = tmp / "blobs"
        self.blobs.mkdir()
        self.served = tmp / "served"
        self.antb1_log = tmp / "antb1.log"
        self.make_root()
        install(self.bin, {"gh": GH_RUNNER.replace("{path!r}", repr(str(REPO / "tools/ci/tests/fake_gh.py")))})
        install(self.bin, {"curl": fake_download(self.served, tmp / "curl.log")})
        self.state = self.github()
        self.outputs: dict[str, str] = {}
        self.runs = 0

    # --- the checkout -------------------------------------------------------------------------------------------

    def make_root(self) -> None:
        root = self.root
        (root / "cmake" / "static").mkdir(parents=True)
        (root / "tools" / "github").mkdir(parents=True)
        (root / "CMakeLists.txt").write_text(f"project(\n  antb1\n  VERSION {VERSION}\n  LANGUAGES CXX\n)\n")
        (root / "pixi.toml").write_text(f'[workspace]\nname = "antb1"\nversion = "{VERSION}"\n')
        (root / "LICENSE").write_text("the licence of the repository\n")
        arrow = "".join(f"{name} {sha256(data)} {len(data)} - https://a.test/{name}\n" for name, data in ARROW.items())
        (root / "cmake/static/arrow-sources.lock").write_text("# test lock\n" + arrow)
        glibc = "".join(f"{build} {SRPM} {sha256(GLIBC)} {len(GLIBC)} https://vault.test/{SRPM}\n"
                        for build in SYSROOT.values())  # fmt: skip
        (root / "cmake/static/glibc-source.lock").write_text("# test lock\n" + glibc)
        lock = ["version: 7", "environments:", "  static:", "    packages:", "packages:"]
        for subdir in SUBDIR.values():
            for name, version in CONDA.items():
                archive = conda_archive(name, subdir)
                lock += [f"- conda: https://conda.anaconda.org/conda-forge/{subdir}/{name}-{version}-h0_0.conda",
                         f"  sha256: {sha256(archive)}", f"  size: {len(archive)}"]  # fmt: skip
        (root / "pixi.lock").write_text("\n".join(lock) + "\n")
        for name in ("environment-release.json", "ruleset-tags.json", "ruleset-release-tags.json"):
            (root / "tools/github" / name).write_bytes((REPO / "tools/github" / name).read_bytes())
        (root / "README.md").write_bytes((REPO / "README.md").read_bytes())

    # --- the build artifacts -------------------------------------------------------------------------------------

    def buildinfo(self, target: str, tag: str | None) -> dict[str, str]:
        info = {"name": "antb1", "version": VERSION, "commit": SHA, "target": target, "flavor": "static-thinlto",
                "license": "Apache-2.0", "source": f"https://github.com/{UPSTREAM}/tree/{SHA}",
                "pixi_lock_sha256": sha256((self.root / "pixi.lock").read_bytes())}  # fmt: skip
        if target in SUBDIR:
            info |= {"sysroot": SYSROOT[target], "glibc_source": SRPM}
        if tag:
            info["tag"] = tag
        return info

    def package(self, target: str, info: dict[str, str], commit: str = SHA) -> bytes:
        top = f"antb1-{VERSION}-{target}"
        binary = fake_antb1(self.antb1_log, target, VERSION, commit)
        return tar_bytes({f"{top}/antb1": binary, f"{top}/BUILDINFO.json": json.dumps(info).encode()}, dirs=(top,))

    def sources(self, target: str, commit: str = SHA, extra: dict[str, bytes] | None = None, change: str = "") -> bytes:
        """antb1-<target>.sources.tar as scripts/static_package.py sources packs it, broken in one way if asked."""
        top = f"antb1-{target}-sources"
        licence = b"another licence\n" if change == "licence" else (self.root / "LICENSE").read_bytes()
        source = tar_bytes({f"antb1-{VERSION}/LICENSE": licence}, pax_headers={"comment": commit})
        files = {SRPM: GLIBC, "antb1-source.tar.gz": source, **{f"third-party/{n}": d for n, d in ARROW.items()}}
        for name, version in CONDA.items():
            subdir = "linux-aarch64" if change == "conda-other-subdir" else SUBDIR[target]
            if not (change == "no-cli11" and name == "cli11"):
                files[f"third-party/{name}-{version}-h0_0.conda"] = conda_archive(name, subdir)
        files |= extra or {}
        sums = "".join(f"{sha256(data)}  {name}\n" for name, data in sorted(files.items()))
        if change == "inner-sums":
            sums = "0" * 64 + sums[64:]
        members = {f"{top}/{name}": data for name, data in files.items()}
        members[f"{top}/SHA256SUMS"] = sums.encode()
        named = OTHER if change == "readme" else commit
        members[f"{top}/README.txt"] = f"The sources of commit {named}: {SRPM} and more.\n".encode()
        return tar_bytes(members, mode="w", dirs=(top, f"{top}/third-party"))

    def artifacts(self, tag: str | None = TAG, *, change: str = "") -> Path:
        """The downloaded build artifacts (one directory each), as release.yml's build uploads them."""
        out = self.tmp / f"artifacts-{self.runs}"
        self.runs += 1
        for target in TARGETS:
            pkg = out / f"antb1-{target}"
            pkg.mkdir(parents=True)
            info = self.buildinfo(target, tag)
            if change == "commit" and target == "linux-x86_64":
                info["commit"] = OTHER
            if change == "glibc-on-macos" and target == "darwin-arm64":
                info["sysroot"] = SYSROOT["linux-x86_64"]
            if change == "lock" and target == "linux-aarch64":
                info["pixi_lock_sha256"] = "0" * 64
            if change == "glibc-source" and target == "linux-x86_64":
                info["glibc_source"] = "glibc-2.28-236.el8_9.13.src.rpm"
            if change == "sysroot" and target == "linux-x86_64":
                info["sysroot"] = "sysroot_linux-64-2.28-hother_1"
            names = {f"antb1-{target}.tar.gz": self.package(target, info)}
            if target in SUBDIR:
                names[f"antb1-{target}.debug.tar.gz"] = tar_bytes({f"antb1-{VERSION}-{target}/antb1.debug": b"tables"})
            for name, data in names.items():
                (pkg / name).write_bytes(data)
            sums = "".join(f"{sha256(data)}  {name}\n" for name, data in names.items())
            (pkg / "SHA256SUMS").write_text(sums if change != "package-sums" else "0" * 64 + sums[64:])
            if target in SUBDIR:
                src = out / f"antb1-{target}-sources"
                src.mkdir()
                extra = {"notes.txt": b"x"} if change == "sources-extra" and target == "linux-x86_64" else None
                if change == "srpm" and target == "linux-x86_64":
                    extra = {SRPM: b"another srpm"}
                inside = change if target == "linux-x86_64" else ""
                data = self.sources(target, OTHER if change == "source-commit" else SHA, extra, inside)
                name = f"antb1-{target}.sources.tar"
                (src / name).write_bytes(data)
                (src / f"{name}.sha256").write_text(f"{sha256(data)}  {name}\n")
        if change == "missing":
            for path in (out / "antb1-darwin-arm64").iterdir():
                path.unlink()
            (out / "antb1-darwin-arm64").rmdir()
        if change == "extra-file":
            (out / "antb1-linux-x86_64" / "notes.txt").write_text("x")
        return out

    # --- GitHub --------------------------------------------------------------------------------------------------

    def github(self) -> dict[str, object]:
        env_file = json.loads((REPO / "tools/github/environment-release.json").read_text())
        reviewers = [{"type": r["type"], "reviewer": {"id": r["id"], "login": f"user{r['id']}"}}
                     for r in env_file["reviewers"]]  # fmt: skip
        environment = {
            "name": "release",
            "can_admins_bypass": env_file["can_admins_bypass"],
            "protection_rules": [
                {"type": "required_reviewers", "prevent_self_review": env_file["prevent_self_review"],
                 "reviewers": reviewers},
                {"type": "branch_policy"},
            ],
            "deployment_branch_policy": env_file["deployment_branch_policy"],
        }  # fmt: skip
        rulesets = []
        for number, name in enumerate(("ruleset-tags.json", "ruleset-release-tags.json"), 1):
            body = json.loads((REPO / "tools/github" / name).read_text())
            body = {k: v for k, v in body.items() if k != "bypass_actors"}  # not visible to GITHUB_TOKEN
            rulesets.append({**body, "id": 1000 + number, "source_type": "Repository", "source": UPSTREAM})
        message = f"antb1 {VERSION}\n\nThe first release of the standalone binary.\n"
        return {
            "repo": UPSTREAM,
            "scope": "read",
            "immutable": True,
            "refs": {TAG: {"type": "tag", "sha": TAG_OBJECT}, RC: {"type": "tag", "sha": "d" * 40}},
            "tags": {
                TAG_OBJECT: {
                    "tag": TAG,
                    "object": {"type": "commit", "sha": SHA},
                    "message": message,
                    "verification": {"verified": False, "signature": None},
                },
                "d" * 40: {
                    "tag": RC,
                    "object": {"type": "commit", "sha": SHA},
                    "message": "a candidate\n",
                    "verification": {"verified": False, "signature": None},
                },
            },
            "compare": {f"{SHA}...main": {"status": "identical", "ahead_by": 0, "behind_by": 0, "files": []}},
            "workflow_runs": [ci_run(9, "completed", "2026-10-26T01:00:00Z")],
            "jobs": {"9": [ci_job("success")]},
            "environments": {"release": environment},
            "branch_policies": {"release": [{"id": 1, "name": "v*", "type": "tag"}]},
            "rulesets": rulesets,
            "releases": [],
            "latest": None,
            "next_id": 500,
            "attestations": [],
            "fail": {},
        }

    def gh_calls(self) -> list[list[str]]:
        log = self.tmp / "gh.log"
        return [json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []

    def run(self, cmd: str, *args: str, scope: str = "read", event: str = "push", ref: str = f"refs/tags/{TAG}",
            **extra: str) -> subprocess.CompletedProcess[str]:  # fmt: skip
        self.state["scope"] = scope
        (self.tmp / "state.json").write_text(json.dumps(self.state))
        output, summary = self.tmp / f"output-{self.runs}", self.tmp / f"summary-{self.runs}"
        self.runs += 1
        path = f"{self.bin}{os.pathsep}/usr/bin{os.pathsep}/bin"
        assert_fakes_first(path, self.bin, ["gh", "curl"])  # never the host's own gh or curl
        env = {
            "PATH": path,
            "HOME": str(self.tmp),
            "GH_HOST": "gh.invalid",  # a real gh, were it ever reached, could not talk to GitHub
            "GH_CONFIG_DIR": str(self.tmp / "gh-config"),
            "FAKE_GH_STATE": str(self.tmp / "state.json"),
            "FAKE_GH_DIR": str(self.blobs),
            "FAKE_GH_LOG": str(self.tmp / "gh.log"),
            "GITHUB_REPOSITORY": UPSTREAM,
            "GITHUB_EVENT_NAME": event,
            "GITHUB_REF": ref,
            "GITHUB_SHA": SHA,
            "GITHUB_RUN_ID": "4242",
            "GITHUB_OUTPUT": str(output),
            "GITHUB_STEP_SUMMARY": str(summary),
            "ANTB1_RELEASE_RETRY_DELAY": "0",
            **extra,
        }
        result = subprocess.run([sys.executable, "-I", str(RELEASE), cmd, *args], env=env, capture_output=True,
                                text=True, timeout=120, check=False)  # fmt: skip
        self.state = json.loads((self.tmp / "state.json").read_text())
        self.outputs = read_outputs(output)
        return result

    def release_env(self, outputs: dict[str, str]) -> dict[str, str]:
        keys = {
            "sha": "RELEASE_SHA",
            "version": "RELEASE_VERSION",
            "tag": "RELEASE_TAG",
            "prerelease": "RELEASE_PRERELEASE",
            "publish": "RELEASE_PUBLISH",
            "tag_object": "RELEASE_TAG_OBJECT",
        }
        return {name: outputs[key] for key, name in keys.items()}  # fmt: skip

    def bundle(self, subjects: str, **change: object) -> Path:
        """What actions/attest would sign: a fake Sigstore bundle of the subjects (fake_gh.py verifies it)."""
        names = [line.split("  ", 1) for line in subjects.splitlines()]
        body = {
            "fake": "signed",
            "repo": UPSTREAM,
            "workflow": f"{UPSTREAM}/.github/workflows/release.yml",
            "ref": f"refs/tags/{TAG}",
            "sha": SHA,
            "hosted": True,
            "subjects": [{"name": name, "digest": {"sha256": digest}} for digest, name in names],
        } | change
        path = self.tmp / f"provenance-{self.runs}" / "antb1.sigstore.json"
        self.runs += 1
        path.parent.mkdir()
        path.write_text(json.dumps(body))
        return path


def read_outputs(path: Path) -> dict[str, str]:
    """GITHUB_OUTPUT: `key=value` lines and `key<<DELIMITER` blocks."""
    values: dict[str, str] = {}
    lines = path.read_text().splitlines() if path.exists() else []
    i = 0
    while i < len(lines):
        if "<<" in lines[i] and "=" not in lines[i].split("<<", 1)[0]:
            key, delimiter = lines[i].split("<<", 1)
            end = lines.index(delimiter, i + 1)
            values[key] = "\n".join(lines[i + 1 : end])
            i = end + 1
        else:
            key, value = lines[i].split("=", 1)
            values[key] = value
            i += 1
    return values


@pytest.fixture
def world(tmp_path: Path) -> World:
    return World(tmp_path)


def failed(result: subprocess.CompletedProcess[str], text: str) -> bool:
    return result.returncode == 1 and text in result.stderr


class Run:
    """A release run through the jobs, so a test can stop or break it at any one of them."""

    def __init__(self, world: World, tag: str = TAG) -> None:
        self.world, self.tag = world, tag
        self.rel: dict[str, str] = {}
        self.subjects = ""
        self.meta = world.tmp / "meta"
        self.artifacts = world.tmp / "none"
        self.release_id = ""
        self.bundle_path = world.tmp / "none"

    def preflight(self) -> subprocess.CompletedProcess[str]:
        result = self.world.run("preflight", "--root", str(self.world.root), ref=f"refs/tags/{self.tag}")
        if result.returncode == 0:
            self.rel = self.world.release_env(self.world.outputs)
        return result

    def assemble(self, change: str = "") -> subprocess.CompletedProcess[str]:
        self.artifacts = self.world.artifacts(self.tag if self.rel.get("RELEASE_PUBLISH") == "true" else None,
                                              change=change)  # fmt: skip
        result = self.world.run("assemble", "--in", str(self.artifacts), "--out", str(self.meta), "--root",
                                str(self.world.root), ref=f"refs/tags/{self.tag}", **self.rel)  # fmt: skip
        self.subjects = self.world.outputs.get("subjects", "")
        return result

    def draft(self, bundle: Path | None = None, **env: str) -> subprocess.CompletedProcess[str]:
        bundle = bundle or self.world.bundle(self.subjects, ref=f"refs/tags/{self.tag}")
        self.bundle_path = bundle
        variables = {**self.rel, "RELEASE_SUBJECTS": self.subjects, **env}
        result = self.world.run("draft", "--artifacts", str(self.artifacts), "--meta", str(self.meta), "--bundle",
                                str(bundle), scope="write", ref=f"refs/tags/{self.tag}", **variables)  # fmt: skip
        self.release_id = self.world.outputs.get("release_id", "")
        return result

    def publish(self, **env: str) -> subprocess.CompletedProcess[str]:
        work = self.world.tmp / f"published-{self.world.runs}"
        variables = {**self.rel, "RELEASE_SUBJECTS": self.subjects, "RELEASE_ID": self.release_id, **env}
        return self.world.run("publish", "--work", str(work), scope="write", ref=f"refs/tags/{self.tag}",
                              **variables)  # fmt: skip

    def through(self, last: str) -> None:
        for step in ("preflight", "assemble", "draft", "publish"):
            result = getattr(self, step)()
            assert result.returncode == 0, f"{step}: {result.stdout}{result.stderr}"
            if step == last:
                return


# ---------------------------------------------------------------------------------------------------------------------
# the whole release


def test_a_release_runs_from_preflight_to_an_immutable_release(world: World) -> None:
    run = Run(world)
    run.through("preflight")
    assert world.outputs == {"sha": SHA, "version": VERSION, "tag": TAG, "tag_object": TAG_OBJECT,
                             "prerelease": "false", "publish": "true"}  # fmt: skip
    assert all(call[:4] == ["api", "--include", "--method", "GET"] for call in world.gh_calls())  # reads only

    assert run.assemble().returncode == 0
    lines = run.subjects.splitlines()
    names = [line.split("  ", 1)[1] for line in lines]
    assert names[-1] == "SHA256SUMS" and len(lines) == 8 and names[:-1] == sorted(names[:-1])
    assert sorted(path.name for path in run.meta.iterdir()) == ["SHA256SUMS", "notes.md"]
    assert lines[-1] == f"{sha256((run.meta / 'SHA256SUMS').read_bytes())}  SHA256SUMS"
    notes = (run.meta / "notes.md").read_text()
    assert notes.startswith(f"antb1 {VERSION}\n\nThe first release") and "Dry run" not in notes
    assert f"--source-ref refs/tags/{TAG} --deny-self-hosted-runners" in notes

    result = run.draft()
    assert result.returncode == 0, result.stderr
    drafts = [r for r in world.state["releases"] if r["draft"]]
    assert len(drafts) == 1 and str(drafts[0]["id"]) == run.release_id
    assert sorted(a["name"] for a in drafts[0]["assets"]) == sorted([*names, "antb1.sigstore.json"])
    create = next(call for call in world.gh_calls() if call[:2] == ["release", "create"])
    assert create[2:11] == [TAG, "--repo", UPSTREAM, "--verify-tag", "--draft", "--title", f"antb1 {VERSION}",
                            "--notes-file", str(run.meta / "notes.md")]  # fmt: skip
    assert not {"--target", "--latest", "--generate-notes", "--prerelease"} & set(create)

    result = run.publish()
    assert result.returncode == 0, result.stdout + result.stderr
    release = world.state["releases"][0]
    assert (release["draft"], release["immutable"], release["prerelease"]) == (False, True, False)
    assert world.state["latest"] == release["id"] and world.outputs == {"latest": "true"}
    patch = [call[4:] for call in world.gh_calls() if call[3:4] == ["PATCH"]]
    url = f"repos/{UPSTREAM}/releases/{run.release_id}"
    assert patch == [  # published first, the latest release only once it proved immutable and attested
        [url, "-F", "draft=false", "-F", "prerelease=false", "-f", "make_latest=false"],
        [url, "-f", "make_latest=true"],
    ]


def test_a_release_candidate_is_a_prerelease_that_is_never_latest(world: World) -> None:
    run = Run(world, RC)
    run.through("publish")
    assert run.rel["RELEASE_PRERELEASE"] == "true" and world.outputs == {"latest": "false"}
    release = world.state["releases"][0]
    assert release["prerelease"] is True and world.state["latest"] is None
    create = next(call for call in world.gh_calls() if call[:2] == ["release", "create"])
    assert "--prerelease" in create
    assert "A release candidate of antb1 0.1.0" in (run.meta / "notes.md").read_text()


def published(world: World) -> Run:
    """A release published by the run, its assets served at the tag's and the latest URLs, its bundle online."""
    run = Run(world)
    run.through("publish")
    world.state["attestations"] = [json.loads(run.bundle_path.read_text())]  # what the attestations API finds
    for asset in world.state["releases"][0]["assets"]:
        data = (world.blobs / str(asset["id"])).read_bytes()
        for base in (f"releases/download/{TAG}", "releases/latest/download"):
            serve(world.served, f"https://github.com/{UPSTREAM}/{base}/{asset['name']}", data)
    return run


def check_published(world: World, run: Run, latest: str = "false") -> subprocess.CompletedProcess[str]:
    return world.run("check-published", "--target", HOST_TARGET, "--work", str(world.tmp / f"check-{world.runs}"),
                     "--root", str(world.root), **run.rel, RELEASE_LATEST=latest)  # fmt: skip


def serve_package(world: World, base: str, commit: str = SHA, runner: str = "") -> None:
    """Another package for this host at `base` with a SHA256SUMS that matches it: what the install block downloads then
    passes its checksum, so only check-published's own checks can stop it."""
    info = world.buildinfo(HOST_TARGET, TAG) | ({"runner": runner} if runner else {})
    data = world.package(HOST_TARGET, info, commit)
    name = f"antb1-{HOST_TARGET}.tar.gz"
    serve(world.served, f"https://github.com/{UPSTREAM}/{base}/{name}", data)
    serve(world.served, f"https://github.com/{UPSTREAM}/{base}/SHA256SUMS", f"{sha256(data)}  {name}\n".encode())


@pytest.mark.skipif(not HOST_TARGET, reason="the install block runs only on a release target")
@pytest.mark.parametrize("latest", ["true", "false"])
def test_check_published_installs_and_verifies_as_a_user(world: World, latest: str) -> None:
    run = published(world)
    before = len(world.gh_calls())
    result = check_published(world, run, latest)
    assert result.returncode == 0, result.stdout + result.stderr
    downloads = (world.tmp / "curl.log").read_text()
    assert f"/releases/download/{TAG}/antb1-{HOST_TARGET}.tar.gz" in downloads
    assert ("/releases/latest/download/" in downloads) == (latest == "true")
    verify = [call for call in world.gh_calls()[before:] if call[0] in ("release", "attestation")]
    kinds = [call[:2] for call in verify]
    assert kinds.count(["attestation", "verify"]) == 2 and sum("--bundle" in call for call in verify) == 1
    assert ["release", "verify"] in kinds and ["release", "verify-asset"] in kinds


def break_published(world: World, how: str) -> str:
    """Breaks a published release in one way; returns RELEASE_LATEST for the check."""
    release = world.state["releases"][0]
    if how == "mutable":
        release["immutable"] = False
    elif how == "prerelease":
        release["prerelease"] = True
    elif how == "lightweight":
        world.state["refs"][TAG] = {"type": "commit", "sha": SHA}
    elif how == "no-latest-url":
        readme = world.root / "README.md"
        readme.write_text(readme.read_text().replace("releases/latest/download", f"releases/download/{TAG}"))
    elif how == "other-commit":
        serve_package(world, f"releases/download/{TAG}", commit=OTHER)
    elif how == "other-tarball":
        serve_package(world, f"releases/download/{TAG}", runner="elsewhere")
    elif how == "latest-other":
        serve_package(world, "releases/latest/download", runner="an older release")
        return "true"
    elif how == "verify-asset":
        world.state["fail"] = {"verify_asset": True}
    elif how == "release-attestation":
        world.state["fail"] = {"attestation": "extra-asset"}
    elif how == "no-provenance":
        world.state["attestations"] = []
    else:
        raise AssertionError(how)
    return "false"


@pytest.mark.skipif(not HOST_TARGET, reason="the install block runs only on a release target")
@pytest.mark.parametrize(
    ("how", "message"),
    [
        ("mutable", "immutable=False"),
        ("prerelease", "prerelease=True"),
        ("lightweight", "is not the annotated tag on"),
        ("no-latest-url", "does not download from /ydb-campus/antb1/releases/latest/download"),
        ("other-commit", f"does not say 'commit: {SHA}'"),
        ("other-tarball", "install block (tag) downloaded a"),
        ("latest-other", "install block (latest) downloaded a"),
        ("verify-asset", "release verify-asset"),
        ("release-attestation", "lists other assets"),
        ("no-provenance", "does not verify"),
    ],
)
def test_check_published_refuses_what_a_user_should_not_get(world: World, how: str, message: str) -> None:
    run = published(world)
    result = check_published(world, run, break_published(world, how))
    assert failed(result, message), result.stdout + result.stderr


# ---------------------------------------------------------------------------------------------------------------------
# preflight


def test_a_dry_run_reads_only_and_never_publishes(world: World) -> None:
    world.state["workflow_runs"] = []
    del world.state["environments"]["release"]
    result = world.run("preflight", "--root", str(world.root), event="workflow_dispatch", ref="refs/heads/main",
                       GITHUB_ACTIONS="true")  # fmt: skip
    assert result.returncode == 0, result.stderr
    assert world.outputs["publish"] == "false" and world.outputs["tag"] == TAG
    assert "::warning::CI OK: .github/workflows/ci.yml has not run on" in result.stdout
    assert "::warning::environment: there is no `release` environment" in result.stdout
    assert all(call[3] == "GET" for call in world.gh_calls())


def set_ci(world: World, *runs: tuple[str, str | None, str], workflow: str = ".github/workflows/ci.yml",
           event: str = "push") -> None:  # fmt: skip
    """ci.yml's runs on the commit: (run status, its CI OK job's conclusion, started at), oldest first."""
    world.state["workflow_runs"], world.state["jobs"] = [], {}
    for number, (status, conclusion, started) in enumerate(runs, 1):
        world.state["workflow_runs"].append(ci_run(number, status, started, workflow, event))
        world.state["jobs"][str(number)] = [ci_job(conclusion)] if conclusion != "no-job" else []


def break_github(world: World, how: str) -> None:
    s = world.state
    if how == "lightweight":
        s["refs"][TAG] = {"type": "commit", "sha": SHA}
    elif how == "elsewhere":
        s["tags"][TAG_OBJECT]["object"]["sha"] = OTHER
    elif how == "tag-of-tag":
        s["tags"][TAG_OBJECT]["object"]["type"] = "tag"
    elif how == "off-main":
        s["compare"][f"{SHA}...main"]["status"] = "diverged"
    elif how == "no-ci":
        set_ci(world)
    elif how == "ci-running":
        set_ci(world, ("in_progress", None, "2026-10-26T02:00:00Z"))
    elif how == "ci-failed":
        set_ci(
            world, ("completed", "success", "2026-10-26T01:00:00Z"), ("completed", "failure", "2026-10-26T02:00:00Z")
        )
    elif how == "ci-no-job":
        set_ci(world, ("completed", "no-job", "2026-10-26T01:00:00Z"))
    elif how == "ci-elsewhere":
        set_ci(world, ("completed", "success", "2026-10-26T01:00:00Z"), workflow=".github/workflows/other.yml")
    elif how == "ci-pull-request":
        set_ci(world, ("completed", "success", "2026-10-26T01:00:00Z"), event="pull_request")
    elif how == "environment-wait":
        s["environments"]["release"]["protection_rules"].append({"type": "wait_timer", "wait_timer": 5})
    elif how == "ruleset-include":
        s["rulesets"][1]["conditions"] = {"ref_name": {"include": ["refs/tags/*"], "exclude": []}}
    elif how == "ruleset-on-create":
        checks = next(r for r in s["rulesets"][1]["rules"] if r["type"] == "required_status_checks")
        checks["parameters"]["do_not_enforce_on_create"] = True
    elif how == "ruleset-extra-rule":
        s["rulesets"][0]["rules"].append({"type": "required_signatures"})
    elif how == "released":
        s["releases"].append({"id": 1, "tag_name": TAG, "draft": False, "prerelease": False, "immutable": True,
                              "assets": [], "html_url": "https://github.test/v0.1.0"})  # fmt: skip
    elif how == "no-environment":
        del s["environments"]["release"]
    elif how == "self-review-off":
        s["environments"]["release"]["protection_rules"][0]["prevent_self_review"] = True
    elif how == "admins-bypass":
        s["environments"]["release"]["can_admins_bypass"] = True
    elif how == "one-reviewer":
        s["environments"]["release"]["protection_rules"][0]["reviewers"].pop()
    elif how == "branch-policy":
        s["branch_policies"]["release"].append({"id": 2, "name": "main", "type": "branch"})
    elif how == "no-ruleset":
        s["rulesets"].pop()
    elif how == "ruleset-evaluate":
        s["rulesets"][1]["enforcement"] = "evaluate"
    elif how == "ruleset-no-ci":
        s["rulesets"][1]["rules"] = [r for r in s["rulesets"][1]["rules"] if r["type"] != "required_status_checks"]
    elif how == "released-403":
        s["fail"]["http"] = {f"GET releases/tags/{TAG}": 403}
    else:
        raise AssertionError(how)


@pytest.mark.parametrize(
    ("how", "message"),
    [
        ("lightweight", "is a lightweight tag"),
        ("elsewhere", f"peels to {OTHER}"),
        ("tag-of-tag", "points at a tag"),
        ("off-main", "is not on main"),
        ("no-ci", ".github/workflows/ci.yml has not run on"),
        ("ci-running", "is in_progress"),
        ("ci-failed", "has its CI OK job ['failure']"),
        ("ci-no-job", "has its CI OK job missing"),
        ("ci-elsewhere", ".github/workflows/ci.yml has not run on"),
        ("ci-pull-request", ".github/workflows/ci.yml has not run on"),
        ("environment-wait", "differs from tools/github/environment-release.json in {'wait_timer': 5}"),
        ("ruleset-include", "`release-tags` differs from tools/github/ruleset-release-tags.json in {'conditions'"),
        ("ruleset-on-create", "'required_status_checks': {'do_not_enforce_on_create': True}"),
        ("ruleset-extra-rule", "`tags` differs from tools/github/ruleset-tags.json in {'rules'"),
        ("released", "is already released"),
        ("released-403", "HTTP 403"),
        ("no-environment", "there is no `release` environment"),
        ("self-review-off", "differs from tools/github/environment-release.json"),
        ("admins-bypass", "differs from tools/github/environment-release.json"),
        ("one-reviewer", "differs from tools/github/environment-release.json"),
        ("branch-policy", "deployment policies"),
        ("no-ruleset", "there is no tag ruleset `release-tags`"),
        ("ruleset-evaluate", "`release-tags` differs"),
        ("ruleset-no-ci", "`release-tags` differs"),
    ],
)
def test_preflight_refuses_a_tag_push(world: World, how: str, message: str) -> None:
    break_github(world, how)
    result = world.run("preflight", "--root", str(world.root))
    assert failed(result, message), result.stdout + result.stderr
    assert not world.outputs


def test_preflight_takes_the_newest_ci_ok(world: World) -> None:
    set_ci(world, ("completed", "failure", "2026-10-26T01:00:00Z"), ("completed", "success", "2026-10-26T02:00:00Z"))
    assert world.run("preflight", "--root", str(world.root)).returncode == 0
    assert not [call for call in world.gh_calls() if "check-runs" in " ".join(call)]  # any workflow can forge those


@pytest.mark.parametrize("tag", ["v0.1", "v0.2.0", "v0.1.0-rc.0", "v0.1.0-beta.1", "0.1.0"])
def test_preflight_refuses_a_tag_that_is_not_the_version_before_any_call(world: World, tag: str) -> None:
    result = world.run("preflight", "--root", str(world.root), ref=f"refs/tags/{tag}")
    assert failed(result, "is not v0.1.0 or v0.1.0-rc.<N>") and not world.gh_calls()


@pytest.mark.parametrize("event", ["push", "workflow_dispatch"])
def test_preflight_refuses_versions_that_disagree_in_both_modes(world: World, event: str) -> None:
    (world.root / "pixi.toml").write_text('[workspace]\nversion = "0.2.0"\n')
    result = world.run("preflight", "--root", str(world.root), event=event)
    assert failed(result, "CMakeLists.txt says 0.1.0, pixi.toml says '0.2.0'")


def test_preflight_refuses_a_push_outside_the_upstream_repository(world: World) -> None:
    result = world.run("preflight", "--root", str(world.root), GITHUB_REPOSITORY="someone/antb1")
    assert failed(result, "a tag push publishes only from ydb-campus/antb1") and not world.gh_calls()


def test_preflight_only_warns_when_main_changed_workflows(world: World) -> None:
    world.state["compare"][f"{SHA}...main"] = {"status": "ahead", "ahead_by": 1, "behind_by": 0,
                                               "files": [{"filename": ".github/workflows/static.yml"}]}  # fmt: skip
    result = world.run("preflight", "--root", str(world.root), GITHUB_ACTIONS="true")
    assert result.returncode == 0, result.stderr
    assert "::warning::main: main changed ['.github/workflows/static.yml']" in result.stdout
    assert world.outputs["publish"] == "true"


# ---------------------------------------------------------------------------------------------------------------------
# assemble


@pytest.mark.parametrize(
    ("change", "message"),
    [
        ("missing", "expected the artifacts"),
        ("extra-file", "expected exactly the regular files"),
        ("package-sums", "does not match its checksum"),
        ("commit", "BUILDINFO.json has {'commit'"),
        ("lock", "BUILDINFO.json has {'pixi_lock_sha256'"),
        ("glibc-on-macos", "a macOS package names a glibc"),
        ("sources-extra", "unexpected member antb1-linux-x86_64-sources/notes.txt"),
        ("srpm", f"{SRPM} is not the file the lock pins"),
        ("source-commit", "is not `git archive` of"),
        ("conda-other-subdir", "third-party/cli11-2.7.2-h0_0.conda is not the linux-64 archive that pixi.lock pins"),
        ("no-cli11", "or a conda archive of ('cli11', 'libboost-headers')"),
        ("inner-sums", "its SHA256SUMS does not list exactly its files"),
        ("readme", "README.txt does not name commit"),
        ("licence", "has another LICENSE than the commit"),
        ("glibc-source", "names glibc source 'glibc-2.28-236.el8_9.13.src.rpm'"),
        ("sysroot", "sysroot build 'sysroot_linux-64-2.28-hother_1' has no line"),
    ],
)
def test_assemble_refuses_artifacts_that_are_not_the_release(world: World, change: str, message: str) -> None:
    run = Run(world)
    run.through("preflight")
    result = run.assemble(change)
    assert failed(result, message), result.stdout + result.stderr
    assert "subjects" not in world.outputs


def test_assemble_needs_the_tag_in_buildinfo_on_a_tag_push(world: World) -> None:
    run = Run(world)
    run.through("preflight")
    run.artifacts = world.artifacts(None)  # a package built on a branch
    result = world.run("assemble", "--in", str(run.artifacts), "--out", str(run.meta), "--root", str(world.root),
                       **run.rel)  # fmt: skip
    assert failed(result, "BUILDINFO.json has {'tag': None}")


def test_assemble_on_a_dry_run_writes_dry_run_notes(world: World) -> None:
    run = Run(world)
    result = world.run("preflight", "--root", str(world.root), event="workflow_dispatch", ref="refs/heads/main")
    assert result.returncode == 0, result.stderr
    run.rel = world.release_env(world.outputs)
    assert run.assemble().returncode == 0
    assert (run.meta / "notes.md").read_text().startswith(f"**Dry run** of {SHA}")


# ---------------------------------------------------------------------------------------------------------------------
# draft


def test_draft_runs_only_in_the_tag_push_that_preflight_passed(world: World) -> None:
    run = Run(world)
    run.through("assemble")
    for env in ({"RELEASE_PUBLISH": "false"}, {"GITHUB_EVENT_NAME": "workflow_dispatch"}, {"GITHUB_SHA": OTHER},
                {"GITHUB_REPOSITORY": "someone/antb1"}):  # fmt: skip
        result = run.draft(**env)
        assert failed(result, "only a tag push of v0.1.0 in ydb-campus/antb1"), (env, result.stderr)
    assert not world.state["releases"]


def test_draft_refuses_an_artifact_replaced_after_assemble(world: World) -> None:
    run = Run(world)
    run.through("assemble")
    path = run.artifacts / "antb1-darwin-arm64" / "antb1-darwin-arm64.tar.gz"
    path.write_bytes(path.read_bytes() + b"\0")
    sums = run.artifacts / "antb1-darwin-arm64" / "SHA256SUMS"
    sums.write_text(f"{sha256(path.read_bytes())}  antb1-darwin-arm64.tar.gz\n")  # consistent with itself
    result = run.draft()
    assert failed(result, "an artifact was replaced") and not world.state["releases"]


@pytest.mark.parametrize(
    "change",
    [{"workflow": f"{UPSTREAM}/.github/workflows/other.yml"}, {"sha": OTHER}, {"hosted": False},
     {"ref": "refs/heads/main"}, {"fake": "unsigned"}],
)  # fmt: skip
def test_draft_refuses_a_bundle_that_is_not_this_runs(world: World, change: dict[str, object]) -> None:
    run = Run(world)
    run.through("assemble")
    result = run.draft(world.bundle(run.subjects, **change))
    assert failed(result, "does not verify against antb1.sigstore.json") and not world.state["releases"]


def test_draft_refuses_a_bundle_of_other_digests(world: World) -> None:
    run = Run(world)
    run.through("assemble")
    bundle = world.bundle(run.subjects + "\n" + "0" * 64 + "  extra.tar.gz")  # these files and one more
    assert failed(run.draft(bundle), "with other digests or names than the release assets")


def test_draft_replaces_earlier_drafts_of_the_tag(world: World) -> None:
    run = Run(world)
    run.through("assemble")
    world.state["releases"] = [{"id": 1, "tag_name": TAG, "draft": True, "prerelease": False, "immutable": False,
                                "assets": [], "html_url": "x"}]  # fmt: skip
    assert run.draft().returncode == 0
    assert [r["id"] for r in world.state["releases"]] == [int(run.release_id)] and run.release_id != "1"


@pytest.mark.parametrize(
    ("fail", "message"),
    [
        ({"upload": "SHA256SUMS"}, "upload of SHA256SUMS failed"),
        ({"create": "403"}, "the Releases API refuses GITHUB_TOKEN"),
    ],
)
def test_a_failed_create_leaves_no_draft(world: World, fail: dict[str, str], message: str) -> None:
    run = Run(world)
    run.through("assemble")
    world.state["fail"] = fail
    result = run.draft()
    assert failed(result, message), result.stderr
    assert not world.state["releases"]


def test_draft_refuses_an_upload_that_is_not_the_file(world: World) -> None:
    run = Run(world)
    run.through("assemble")
    world.state["fail"] = {"corrupt": "antb1-linux-aarch64.sources.tar"}
    assert failed(run.draft(), "antb1-linux-aarch64.sources.tar is uploaded")


def test_draft_refuses_a_published_release_of_the_tag(world: World) -> None:
    run = Run(world)
    run.through("assemble")
    break_github(world, "released")
    assert failed(run.draft(), "is already released")


# ---------------------------------------------------------------------------------------------------------------------
# publish


def swap_asset(world: World, name: str, data: bytes) -> None:
    asset = next(a for a in world.state["releases"][0]["assets"] if a["name"] == name)
    (world.blobs / str(asset["id"])).write_bytes(data)
    asset["size"], asset["digest"] = len(data), f"sha256:{sha256(data)}"


def test_publish_refuses_a_draft_whose_asset_was_swapped(world: World) -> None:
    run = Run(world)
    run.through("draft")
    swap_asset(world, "antb1-linux-x86_64.tar.gz", b"someone else's binary")
    assert failed(run.publish(), "does not hold the assets that assemble checked")
    assert world.state["releases"][0]["draft"] is True


def test_publish_refuses_a_swapped_bundle(world: World) -> None:
    run = Run(world)
    run.through("draft")
    forged = json.loads(run.bundle_path.read_text()) | {"sha": OTHER}
    swap_asset(world, "antb1.sigstore.json", json.dumps(forged).encode())
    assert failed(run.publish(), "does not verify against antb1.sigstore.json")


def test_publish_refuses_an_extra_or_a_missing_asset(world: World) -> None:
    run = Run(world)
    run.through("draft")
    assets = world.state["releases"][0]["assets"]
    saved = copy.deepcopy(assets)
    assets.append({**assets[0], "id": 999, "name": "extra.txt"})
    assert failed(run.publish(), "expected exactly")
    world.state["releases"][0]["assets"] = saved[1:]
    assert failed(run.publish(), "expected exactly")


def test_publish_refuses_a_moved_tag_or_another_published_release(world: World) -> None:
    run = Run(world)
    run.through("draft")
    world.state["refs"][TAG] = {"type": "tag", "sha": "e" * 40}
    world.state["tags"]["e" * 40] = {**world.state["tags"][TAG_OBJECT]}
    assert failed(run.publish(), "is no longer the annotated tag")
    world.state["refs"][TAG] = {"type": "tag", "sha": TAG_OBJECT}
    break_github(world, "released")
    assert failed(run.publish(), "another release of v0.1.0 is published")


def test_publish_fails_loudly_when_releases_are_not_immutable(world: World) -> None:
    run = Run(world)
    run.through("draft")
    world.state["immutable"] = False
    assert failed(run.publish(), "immutable releases are off")


def test_publish_names_the_workflow_rule_when_the_patch_is_refused(world: World) -> None:
    run = Run(world)
    run.through("draft")
    world.state["fail"] = {"patch": 403}
    assert failed(run.publish(), "the Releases API refuses GITHUB_TOKEN")


def test_publish_waits_for_the_release_attestation(world: World) -> None:
    run = Run(world)
    run.through("draft")
    world.state["fail"] = {"verify": 2}
    assert run.publish().returncode == 0


def test_a_rerun_of_publish_only_verifies(world: World) -> None:
    run = Run(world)
    run.through("publish")
    calls = len(world.gh_calls())
    result = run.publish()
    assert result.returncode == 0, result.stderr
    assert "already published (a re-run): verifying it only" in result.stdout
    patched = [call for call in world.gh_calls()[calls:] if call[3:4] == ["PATCH"]]
    assert all("draft=false" not in call for call in patched)  # never published twice; latest again is harmless
    assert world.outputs == {"latest": "true"}


@pytest.mark.parametrize(
    ("fault", "message"),
    [
        ("tag-object", "expected the tag object"),
        ("extra-asset", "lists other assets"),
        ("predicate", "is about ('https://example.invalid/other'"),
    ],
)
def test_publish_refuses_a_release_attestation_that_is_not_this_release(world: World, fault: str, message: str) -> None:
    run = Run(world)
    run.through("draft")
    world.state["fail"] = {"attestation": fault}
    assert failed(run.publish(), message)
    assert world.state["latest"] is None  # published, but not served as the latest release


def test_a_release_that_is_not_immutable_never_becomes_the_latest(world: World) -> None:
    run = Run(world)
    run.through("draft")
    world.state["immutable"] = False
    assert failed(run.publish(), "immutable releases are off")
    assert world.state["releases"][0]["draft"] is False and world.state["latest"] is None


def test_publish_says_to_rerun_the_draft_job_when_its_draft_is_gone(world: World) -> None:
    run = Run(world)
    run.through("draft")
    world.state["releases"] = []
    assert failed(run.publish(), "re-run the draft job (Re-run this job)")


def test_make_latest_is_true_only_above_every_stable_release() -> None:
    release = load_release()
    stable = [{"tag_name": "v0.2.0", "draft": False, "prerelease": False}]
    candidate = [{"tag_name": "v0.3.0-rc.1", "draft": False, "prerelease": True}]

    def rel(tag: str) -> object:
        match = release.TAG_RE.fullmatch(tag)
        return release.Release(UPSTREAM, tag, SHA, match["version"], bool(match["rc"]), True)

    assert release.make_latest(rel("v0.3.0"), stable + candidate) == "true"
    assert release.make_latest(rel("v0.1.1"), stable) == "false"  # a backport never takes over latest
    assert release.make_latest(rel("v0.3.0-rc.2"), stable) == "false"
    assert release.make_latest(rel("v0.1.0"), []) == "true"


# ---------------------------------------------------------------------------------------------------------------------
# contracts


def test_the_constants_agree_with_the_repository() -> None:
    release = load_release()
    ruleset = json.loads((REPO / "tools/github/ruleset-main.json").read_text())
    checks = [rule for rule in ruleset["rules"] if rule["type"] == "required_status_checks"]
    contexts = {c["context"]: c.get("integration_id") for c in checks[0]["parameters"]["required_status_checks"]}
    assert contexts[release.CI_CHECK] == release.ACTIONS_APP_ID
    static = yaml.safe_load((REPO / ".github/workflows/static.yml").read_text())
    assert tuple(e["target"] for e in static["jobs"]["static"]["strategy"]["matrix"]["include"]) == release.TARGETS
    spec = importlib.util.spec_from_file_location("static_package_under_test", REPO / "scripts/static_package.py")
    assert spec is not None and spec.loader is not None
    package = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(package)
    assert tuple(package.COMPILED_PACKAGES) == release.COMPILED_PACKAGES
    for path in [release.ENVIRONMENT_FILE, *release.RULESET_FILES.values()]:
        assert (REPO / path).is_file(), path
    assert [*release.release_files(), "SHA256SUMS", "antb1.sigstore.json"] == release.ASSETS and len(
        release.ASSETS
    ) == 9


def test_the_conda_pins_come_from_the_real_lock() -> None:
    release = load_release()
    for subdir in SUBDIR.values():
        pins = release.conda_pins(REPO / "pixi.lock", subdir)
        for name in release.COMPILED_PACKAGES:
            assert len([fn for fn in pins if fn.startswith(f"{name}-") and fn.endswith(".conda")]) >= 1, (subdir, name)
