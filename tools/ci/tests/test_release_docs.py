"""The release docs against the release (docs/adr/0035-github-releases.md; run by `pixi run lint`): README.md's install
block, run under `sh -e` as a user pastes it, for each target, against a fake curl, uname and antb1 and the host's
own sha256sum or shasum and tar; every download URL the docs give names an asset that releases have; every documented
`gh attestation verify` pins the workflow, the tag and GitHub-hosted runners; the versions agree; and docs/ci.md maps
every job and artifact of the Release workflow.
"""

from __future__ import annotations

import hashlib
import importlib.util
import io
import os
import re
import subprocess
import sys
import tarfile
import tomllib
from pathlib import Path
from types import ModuleType

import pytest
import yaml
from fakes import assert_fakes_first, fake_antb1, fake_download, install, serve

ROOT = Path(__file__).resolve().parents[3]
DOCS = ["README.md", "SECURITY.md", "CONTRIBUTING.md", "docs/ci.md", "docs/benchmarks.md", "docs/agents.md",
        "docs/adr/0035-github-releases.md"]  # fmt: skip
UNAME = {
    "linux-x86_64": ("Linux", "x86_64"),
    "linux-aarch64": ("Linux", "aarch64"),
    "darwin-arm64": ("Darwin", "arm64"),
}
LATEST = "https://github.com/ydb-campus/antb1/releases/latest/download"


def load_release() -> ModuleType:
    spec = importlib.util.spec_from_file_location("release_for_docs", ROOT / ".github/scripts/release.py")
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["release_for_docs"] = module
    spec.loader.exec_module(module)
    return module


RELEASE = load_release()


def text(name: str) -> str:
    return (ROOT / name).read_text(encoding="utf-8")


def package(target: str, log: Path) -> bytes:
    top = f"antb1-0.1.0-{target}"
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as tar:
        for name, data, mode in (
            (top, b"", 0o755),
            (f"{top}/antb1", fake_antb1(log, target, "0.1.0", "f" * 40), 0o755),
        ):
            info = tarfile.TarInfo(name)
            info.type = tarfile.DIRTYPE if not data else tarfile.REGTYPE
            info.size, info.mode = len(data), mode
            tar.addfile(info, io.BytesIO(data) if data else None)
    return buf.getvalue()


def run_install_block(tmp: Path, target: str, corrupt: bool = False) -> subprocess.CompletedProcess[str]:
    """README.md's install block on a host that looks like `target`: a fake uname, a fake curl that serves a release
    from releases/latest/download, and that release's packages, whose antb1 is a fake that says what it is."""
    served, bin_dir, work = tmp / "served", tmp / "bin", tmp / "work"
    work.mkdir()
    data = {name: package(name.removeprefix("antb1-").removesuffix(".tar.gz"), tmp / "antb1.log")
            for name in (f"antb1-{t}.tar.gz" for t in UNAME)}  # fmt: skip
    sums = "".join(f"{hashlib.sha256(d).hexdigest()}  {name}\n" for name, d in sorted(data.items()))
    for name, payload in data.items():
        serve(served, f"{LATEST}/{name}", payload + (b"\0" if corrupt and target in name else b""))
    serve(served, f"{LATEST}/SHA256SUMS", sums.encode())
    system, machine = UNAME[target]
    uname = f'#!/bin/sh\ncase "$1" in -s) echo {system} ;; -m) echo {machine} ;; *) exit 99 ;; esac\n'
    install(bin_dir, {"curl": fake_download(served, tmp / "curl.log"), "uname": uname})
    env = {"PATH": f"{bin_dir}{os.pathsep}/usr/bin{os.pathsep}/bin", "HOME": str(tmp), "LC_ALL": "C"}
    assert_fakes_first(env["PATH"], bin_dir, ["curl", "uname"])  # never the network, never the host's uname
    block = RELEASE.install_block(ROOT)
    return subprocess.run(["sh", "-e", "-c", block], cwd=work, env=env, capture_output=True, text=True, timeout=60,
                          check=False)  # fmt: skip


@pytest.mark.parametrize("target", list(UNAME))
def test_the_install_block_installs_each_target(tmp_path: Path, target: str) -> None:
    result = run_install_block(tmp_path, target)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "antb1 0.1.0 (Apache Arrow" in result.stdout and f"target: {target}" in result.stdout
    assert (tmp_path / "curl.log").read_text().splitlines() == [
        f"-fsSLO {LATEST}/antb1-{target}.tar.gz",
        f"-fsSLO {LATEST}/SHA256SUMS",
    ]


@pytest.mark.parametrize("target", list(UNAME))
def test_the_install_block_runs_nothing_it_did_not_check(tmp_path: Path, target: str) -> None:
    result = run_install_block(tmp_path, target, corrupt=True)
    assert result.returncode != 0
    assert not (tmp_path / "antb1.log").exists() and not list((tmp_path / "work").glob("antb1-0.1.0-*"))


def test_the_install_block_refuses_another_platform(tmp_path: Path) -> None:
    served, bin_dir = tmp_path / "served", tmp_path / "bin"
    install(bin_dir, {"curl": fake_download(served, tmp_path / "curl.log"),
                      "uname": '#!/bin/sh\ncase "$1" in -s) echo FreeBSD ;; -m) echo amd64 ;; esac\n'})  # fmt: skip
    env = {"PATH": f"{bin_dir}{os.pathsep}/usr/bin{os.pathsep}/bin", "HOME": str(tmp_path), "LC_ALL": "C"}
    assert_fakes_first(env["PATH"], bin_dir, ["curl", "uname"])
    result = subprocess.run(["sh", "-e", "-c", RELEASE.install_block(ROOT)], cwd=tmp_path, env=env, text=True,
                            capture_output=True, timeout=60, check=False)  # fmt: skip
    assert result.returncode != 0 and "no antb1 release for FreeBSD amd64" in result.stderr
    assert not (tmp_path / "curl.log").exists()


def expand(name: str) -> list[str]:
    """An asset name as the docs write it, with its placeholders filled in for every target."""
    names = [name]
    for placeholder, values in ((r"\$t\b|\$\{t\}|<target>|<t>", RELEASE.TARGETS), (r"<arch>", ("x86_64", "aarch64"))):
        names = [re.sub(placeholder, v, n) for n in names for v in values] if re.search(placeholder, name) else names
    linux_only = ("debug", "sources")
    return [n for n in names if not (any(k in n for k in linux_only) and "darwin" in n)]


def test_every_documented_download_is_an_asset_of_a_release() -> None:
    found = 0
    for doc in DOCS:
        body = text(doc)
        urls = re.findall(r"releases/(?:latest/download|download/[^/\s`)]+)/([A-Za-z0-9.$_{}<>-]+)", body)
        urls += re.findall(r'"\$url/([A-Za-z0-9.$_{}<>-]+)"', body)  # the install block's url=.../latest/download
        for name in urls:
            if name in ("<asset>", "<file>"):
                continue
            for expanded in expand(name):
                found += 1
                assert expanded in RELEASE.ASSETS, f"{doc}: {name} -> {expanded}"
    assert found >= 4


def test_every_documented_attestation_check_pins_the_release() -> None:
    commands = []
    for doc in DOCS:
        joined = re.sub(r"\\\n\s*", " ", text(doc))  # backslash continuation lines
        commands += [(doc, m) for m in re.findall(r"gh attestation verify [^\n`]*", joined)]
    assert len(commands) >= 3
    for doc, command in commands:
        assert "--repo ydb-campus/antb1" in command, (doc, command)
        assert "--signer-workflow ydb-campus/antb1/.github/workflows/release.yml" in command, (doc, command)
        assert "--source-ref refs/tags/" in command and "--deny-self-hosted-runners" in command, (doc, command)


def test_the_two_version_sources_agree() -> None:
    cmake, pixi = RELEASE.project_version(ROOT)
    assert cmake == pixi == tomllib.loads(text("pixi.toml"))["workspace"]["version"]


def anchors(doc: str) -> set[str]:
    return {
        re.sub(r"[^\w\- ]", "", h.strip().lower()).replace(" ", "-") for h in re.findall(r"^#+ (.+)$", text(doc), re.M)
    }


def test_the_release_anchors_exist() -> None:
    assert "releases" in anchors("docs/ci.md") and "verifying-a-release" in anchors("SECURITY.md")
    for doc in DOCS:
        for target, anchor in re.findall(r"\]\(([^)#]*\.md)#([\w-]+)\)", text(doc)):
            path = (ROOT / doc).parent / target
            if path.name in ("ci.md", "SECURITY.md") and path.exists():
                assert anchor in anchors(str(path.relative_to(ROOT))), (doc, target, anchor)


def test_docs_ci_maps_every_job_and_artifact_of_the_release_workflow() -> None:
    workflow = yaml.safe_load(text(".github/workflows/release.yml"))
    rows = [line for line in text("docs/ci.md").splitlines() if line.startswith("| `")]
    for job in workflow["jobs"].values():
        name = re.sub(r"\$\{\{ matrix\.(\w+) \}\}", r"<\1>", job["name"])
        assert any(row.startswith(f"| `{name}`") for row in rows), name
    artifacts = {step["with"]["name"].replace("${{ matrix.target }}", "<target>")
                 for job in workflow["jobs"].values() for step in job["steps"]
                 if step.get("uses", "").startswith("actions/upload-artifact")}  # fmt: skip
    for artifact in artifacts:
        assert any(row.startswith(f"| `{artifact}`") for row in rows), artifact
