"""Tests for the smoke kit and the fresh-runner check of a standalone package (`scripts/static_gates.py kit` and
`verify`, `scripts/static-check.sh TARBALL`; docs/adr/0033-standalone-static-binary.md; run by `pixi run lint`).

verify runs where neither pixi nor DuckDB is: the static workflow runs it with the runner's own python3. These tests
cover the kit, each check of verify with its own failure, the refusal of a tarball before anything is unpacked, the
exit 2 of a runner that cannot verify, the container's image and flags, the Python floor, static-check.sh's tarball
form and the workflow step. They are hermetic: the packages are packed here (fake_packages.py), the binary is a fake
that answers like antb1, and DuckDB, docker, codesign and python are fakes (fakes.py); verify runs in-process with
the host it checks set to the target's.
"""

from __future__ import annotations

import ast
import hashlib
import importlib.util
import json
import os
import platform
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path
from types import ModuleType

import pytest
import yaml
from fake_packages import REPO, VERSION, Box, make_package
from fakes import (
    FAKE_CODESIGN,
    FAKE_DOCKER,
    FAKE_DOCKER_IMAGE,
    FAKE_DUCKDB,
    FAKE_PYTHON,
    assert_fakes_first,
    fake_antb1,
    install,
)

GATES = REPO / "scripts" / "static_gates.py"
LINUX, MACOS = "linux-x86_64", "darwin-arm64"
HOSTS = {LINUX: ("Linux", "x86_64"), MACOS: ("Darwin", "arm64"), "linux-aarch64": ("Linux", "aarch64")}
PYTHON_SETS = {"LC_CTYPE", "__CF_USER_TEXT_ENCODING"}  # what the fake's own Python adds to an empty environment
# What docs/ci.md promises the container: no network, no capabilities, no privilege gain, read-only, user nobody.
HARDENING = [
    "--pull", "never", "--network", "none", "--read-only", "--cap-drop", "ALL",
    "--security-opt", "no-new-privileges", "--user", "65534:65534",
]  # fmt: skip


def load_gates() -> ModuleType:
    """A fresh copy of the script (its FAILURES list is a module global)."""
    spec = importlib.util.spec_from_file_location("static_gates_under_test", GATES)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


GATES_MODULE = load_gates()


class Sandbox:
    """A Box with the fakes of verify, and verify run in-process on a host made to look like the target's."""

    def __init__(self, tmp: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]) -> None:
        self.box, self.monkeypatch, self.capsys = Box(tmp), monkeypatch, capsys
        install(self.box.env / "bin", {"duckdb": FAKE_DUCKDB})
        install(self.box.bin, {"docker": FAKE_DOCKER, "codesign": FAKE_CODESIGN, "python": FAKE_PYTHON})
        self.docker_dir = tmp / "docker"
        self.docker_dir.mkdir()
        self.log = tmp / "antb1.log"
        self.fake_env = {
            "FAKE_DOCKER_DIR": str(self.docker_dir),
            "FAKE_CODESIGN_LOG": str(tmp / "codesign.log"),
            "FAKE_PYTHON_LOG": str(tmp / "python.log"),
        }
        path = f"{self.box.bin}{os.pathsep}{os.environ['PATH']}"
        monkeypatch.setenv("PATH", path)
        monkeypatch.setenv("DOCKER_HOST", "unix:///nonexistent")
        for key, value in self.fake_env.items():
            monkeypatch.setenv(key, value)
        monkeypatch.setattr(tempfile, "tempdir", str(self.box.tmpdir))
        assert_fakes_first(path, self.box.bin, ["docker", "python", "curl", "cmake"])
        # Every unpack of a tarball in this process, so a test can tell that nothing was unpacked.
        self.unpacked: list[str] = []
        for method in ("extractall", "extract"):
            original = getattr(tarfile.TarFile, method)

            def recorded(tar: tarfile.TarFile, *args: object, _original=original, **kw: object) -> object:
                self.unpacked.append(Path(str(tar.name)).name)
                return _original(tar, *args, **kw)

            monkeypatch.setattr(tarfile.TarFile, method, recorded)

    def kit(self) -> Path:
        """The smoke kit, written once per test by the real `kit` with the fake DuckDB."""
        out = self.box.tmp / "kit"
        if not out.exists():
            result = self.box.run("static_gates.py", "kit", "--env", str(self.box.env), "--out", str(out))
            assert result.returncode == 0, result.stdout + result.stderr
        return out

    def package(self, target: str = LINUX, break_it: str = "", fake_target: str = "", **fake: str) -> Path:
        """A package whose antb1 is a fake that answers as a good one does, unless `fake` says otherwise."""
        fake = {"version": VERSION, "commit": self.box.commit} | fake
        antb1 = fake_antb1(self.log, fake_target or target, **fake)
        return make_package(self.box, break_it, target, antb1)

    def verify(
        self, pkg: Path, kit: Path, target: str = LINUX, *args: str, container: str = "none", host: str = ""
    ) -> tuple[int, str, str]:
        gates = load_gates()
        system, machine = HOSTS[host or target]
        self.monkeypatch.setattr(platform, "system", lambda: system)
        self.monkeypatch.setattr(platform, "machine", lambda: machine)
        self.monkeypatch.setattr(gates, "CODESIGN", str(self.box.bin / "codesign"))
        argv = ["verify", "--assets", str(pkg), "--sums", str(pkg / "SHA256SUMS"), "--kit", str(kit)]
        argv += ["--target", target, "--version", VERSION, "--commit", self.box.commit, "--texts", str(self.box.root)]
        code = gates.main([*argv, "--container", container, *args])
        out = self.capsys.readouterr()
        return code, out.out, out.err

    def runs(self) -> list[dict[str, object]]:
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def docker_calls(self) -> list[list[str]]:
        calls = self.docker_dir / "calls.jsonl"
        return [json.loads(line) for line in calls.read_text().splitlines()] if calls.exists() else []


@pytest.fixture
def v(tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]) -> Sandbox:
    return Sandbox(tmp_path, monkeypatch, capsys)


def fails(out: str) -> list[str]:
    return [line for line in out.splitlines() if line.startswith("static-gates: FAIL")]


# ---------------------------------------------------------------------------------------------------------------------
# kit


def test_the_kit_holds_each_codec_file_a_truncated_copy_and_their_digests(v: Sandbox) -> None:
    kit = v.kit()
    assert sorted(path.name for path in kit.iterdir()) == sorted([*GATES_MODULE.KIT_FILES, "kit.json"])
    meta = json.loads((kit / "kit.json").read_text())
    assert (meta["kit"], meta["sql"], meta["answer"]) == (1, GATES_MODULE.SMOKE_SQL, GATES_MODULE.SMOKE_ANSWER)
    for name, entry in meta["files"].items():
        data = (kit / name).read_bytes()
        assert entry == {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}
    zstd = (kit / "zstd.parquet").read_bytes()
    assert (kit / "truncated.parquet").read_bytes() == zstd[: len(zstd) // 2]
    assert str(kit) not in (kit / "kit.json").read_text()  # no path: the kit works wherever it is copied


def test_the_kit_fails_when_duckdb_gives_another_answer(v: Sandbox) -> None:
    out = v.box.tmp / "kit"
    result = v.box.run(
        "static_gates.py", "kit", "--env", str(v.box.env), "--out", str(out), FAKE_DUCKDB_WRONG="zstd.parquet"
    )
    assert result.returncode == 1
    assert fails(result.stdout) == [
        "static-gates: FAIL DuckDB answers 200000,19999900000,0,999,97 on the zstd file: 1,2,3,4,5"
    ]


def test_the_kit_goes_only_into_a_new_or_empty_directory(v: Sandbox) -> None:
    out = v.box.tmp / "kit"
    out.mkdir()
    (out / "old.parquet").write_bytes(b"old")
    result = v.box.run("static_gates.py", "kit", "--env", str(v.box.env), "--out", str(out))
    assert result.returncode == 1 and "new or empty directory" in result.stdout
    assert [path.name for path in out.iterdir()] == ["old.parquet"]


# ---------------------------------------------------------------------------------------------------------------------
# verify


def test_verify_passes_a_package_on_the_runner_and_in_a_container(v: Sandbox) -> None:
    code, out, err = v.verify(v.package(), v.kit(), container="docker")
    assert code == 0, out + err
    assert not fails(out) and "static-gates: OK" in out
    calls = v.docker_calls()
    assert calls[0] == ["info"] and [call for call in calls if call[0] == "import"] == [["import", "-"]]
    runs = [call for call in calls if call[0] == "run"]
    for call in runs:
        assert call[:3] == ["run", "--rm", "--name"] and call[4 : 4 + len(HARDENING)] == HARDENING
        assert not {"-v", "--volume", "--mount", "--privileged"} & set(call)
        assert call[call.index(FAKE_DOCKER_IMAGE) + 1] == "/antb1"
    assert calls[-1] == ["rmi", "--force", FAKE_DOCKER_IMAGE]
    with tarfile.open(v.docker_dir / "rootfs.tar") as tar:
        rootfs = {info.name: (info.type, info.mode) for info in tar}
    kit_files = {f"kit/{name}": (tarfile.REGTYPE, 0o444) for name in GATES_MODULE.KIT_FILES}
    assert rootfs == {"kit": (tarfile.DIRTYPE, 0o755), "antb1": (tarfile.REGTYPE, 0o755), **kit_files}
    on_runner = [run for run in v.runs() if "rootfs" not in str(run["cwd"])]
    inside = [run for run in v.runs() if "rootfs" in str(run["cwd"])]
    assert len(on_runner) == len(inside) == len(runs)
    for run in on_runner + inside:
        assert set(run["env"]) <= {"MIMALLOC_VERBOSE", *PYTHON_SETS}, run  # nothing of the runner's environment
    assert {Path(str(run["cwd"])).name for run in on_runner} == {"cwd"}  # a new, empty directory
    assert v.unpacked == ["antb1-linux-x86_64.tar.gz"]  # once, after every check of the tarballs


def test_verify_without_an_engine_runs_the_binary_on_the_runner_only(v: Sandbox) -> None:
    code, out, err = v.verify(v.package(), v.kit())
    assert code == 0, out + err
    assert v.runs() and not v.docker_calls()


@pytest.mark.parametrize(
    ("fake", "failure"),
    [
        ({"wrong": "flavor"}, "flavor static-thinlto on this runner"),
        ({"wrong": "arrow"}, "Arrow is the static build (package kind antb1-static) on this runner"),
        ({"wrong": "purge-field"}, "version --verbose reports the purge delay -1 on this runner"),
        ({"wrong": "codec"}, "every codec is available on this runner"),
        ({"wrong": "restart"}, "the allocator restart sets MIMALLOC_PURGE_DELAY=-1 on this runner"),
        ({"wrong": "answer"}, "zstd: query answer on this runner"),
        ({"wrong": "threads"}, "--threads 4 answers as --threads 1 on this runner"),
        ({"wrong": "truncated"}, "a truncated file exits with 3"),
        ({"wrong": "parse"}, "a parse error exits with 1 on this runner"),
        ({"wrong": "usage"}, "an unknown option exits with 2 on this runner"),
        ({"version": "0.2.0"}, f"`antb1 version` runs on this runner and says {VERSION}"),
        ({"commit": "f" * 40}, "commit "),
        ({"fake_target": "linux-aarch64"}, "target linux-x86_64 on this runner"),
    ],
)
def test_verify_fails_each_wrong_answer_on_its_own(v: Sandbox, fake: dict[str, str], failure: str) -> None:
    code, out, _ = v.verify(v.package(**fake), v.kit())
    assert code == 1 and len(fails(out)) == 1 and failure in fails(out)[0], out


def test_verify_checks_the_digests_and_the_kit_before_it_unpacks_anything(v: Sandbox) -> None:
    code, out, _ = v.verify(v.package(break_it="bad-sum"), v.kit())
    assert code == 1 and fails(out) == [
        "static-gates: FAIL antb1-linux-x86_64.tar.gz matches SHA256SUMS: another digest"
    ]
    code, out, _ = v.verify(v.package(break_it="debug-bad-sum"), v.kit())
    assert code == 1
    assert fails(out) == ["static-gates: FAIL antb1-linux-x86_64.debug.tar.gz matches SHA256SUMS: another digest"]
    (v.box.tmp / "package" / "antb1-linux-x86_64.debug.tar.gz").unlink()
    code, out, _ = v.verify(v.box.tmp / "package", v.kit())
    assert code == 1 and fails(out) == [
        "static-gates: FAIL antb1-linux-x86_64.debug.tar.gz matches SHA256SUMS: missing"
    ]
    kit = v.box.tmp / "kit"
    (kit / "zstd.parquet").write_bytes(b"other bytes")
    code, out, _ = v.verify(v.package(), kit)
    assert code == 1 and fails(out) == ["static-gates: FAIL the kit's zstd.parquet matches kit.json"]
    assert not v.runs() and not v.unpacked


def test_verify_reads_only_sha256sum_lines(v: Sandbox) -> None:
    pkg = v.package()
    digest = hashlib.sha256((pkg / "antb1-linux-x86_64.tar.gz").read_bytes()).hexdigest()
    (pkg / "SHA256SUMS").write_text(f"SHA256 (antb1-linux-x86_64.tar.gz) = {digest}\n")  # shasum --tag
    code, out, _ = v.verify(pkg, v.kit())
    assert code == 1 and any("SHA256SUMS has one '<sha256>  <file>' line per file" in line for line in fails(out)), out
    assert not v.runs() and not v.unpacked


def test_verify_fails_a_tarball_it_cannot_read(v: Sandbox) -> None:
    pkg = v.package()
    (pkg / "antb1-linux-x86_64.tar.gz").write_bytes(b"not a gzip file at all")
    lines = [
        f"{hashlib.sha256((pkg / name).read_bytes()).hexdigest()}  {name}"
        for name in ("antb1-linux-x86_64.tar.gz", "antb1-linux-x86_64.debug.tar.gz")
    ]
    (pkg / "SHA256SUMS").write_text("\n".join(lines) + "\n")
    code, out, _ = v.verify(pkg, v.kit())
    assert code == 1 and any("antb1-linux-x86_64.tar.gz is a readable .tar.gz" in line for line in fails(out)), out
    assert not v.runs() and not v.unpacked


@pytest.mark.parametrize(
    ("break_it", "failure"),
    [
        ("symlink", "is a regular file inside"),
        ("dotdot", "is a regular file inside"),
        ("extra", "holds exactly"),
        ("duplicate", "holds each member once"),
        ("xattr", "carries no extended attributes"),
        ("no-license", "holds exactly"),
        ("debug-without-notices", "antb1-linux-x86_64.debug.tar.gz holds exactly"),
    ],
)
def test_verify_refuses_an_unsafe_or_incomplete_tarball_before_it_unpacks_it(
    v: Sandbox, break_it: str, failure: str
) -> None:
    code, out, _ = v.verify(v.package(break_it=break_it), v.kit())
    assert code == 1 and any(failure in line for line in fails(out)), out
    assert not v.runs() and not v.unpacked


@pytest.mark.parametrize(
    ("break_it", "failure"),
    [
        ("no-license-key", "BUILDINFO.json names linux-x86_64, 0.1.0, static-thinlto and Apache-2.0"),
        ("changed-notice", "the package's NOTICE is"),
        ("not-executable", "the unpacked antb1 is executable"),
    ],
)
def test_verify_checks_what_the_package_says_and_carries(v: Sandbox, break_it: str, failure: str) -> None:
    code, out, _ = v.verify(v.package(break_it=break_it), v.kit())
    assert code == 1 and any(failure in line for line in fails(out)), out


def test_verify_checks_the_version_it_is_given(v: Sandbox) -> None:
    code, out, _ = v.verify(v.package(), v.kit(), LINUX, "--version", "0.2.0")
    assert code == 1
    unpacks = "antb1-linux-x86_64.tar.gz unpacks into antb1-0.2.0-linux-x86_64/"
    assert fails(out) == [f"static-gates: FAIL {unpacks}: ['antb1-0.1.0-linux-x86_64']"]
    assert not v.runs() and not v.unpacked
    code, out, _ = v.verify(v.package(break_it="buildinfo-other-version"), v.kit())
    assert code == 1 and len(fails(out)) == 1
    assert "BUILDINFO.json names linux-x86_64, 0.1.0, static-thinlto and Apache-2.0" in fails(out)[0]


def test_verify_checks_the_commit_in_buildinfo_and_in_the_binary(v: Sandbox) -> None:
    other = "e" * 40
    code, out, _ = v.verify(v.package(), v.kit(), LINUX, "--commit", other)
    assert code == 1
    assert fails(out) == [
        f"static-gates: FAIL BUILDINFO.json names {other}",
        f"static-gates: FAIL commit {other} on this runner: {v.box.commit}",
    ]


def test_verify_cannot_run_without_the_data_filter(v: Sandbox, monkeypatch: pytest.MonkeyPatch) -> None:
    pkg, kit = v.package(), v.kit()
    monkeypatch.delattr(tarfile, "data_filter")
    code, _, err = v.verify(pkg, kit)
    assert code == 2 and "is not Python 3.11 or newer with tarfile.data_filter" in err and sys.executable in err


def test_verify_cannot_run_on_a_python_older_than_3_11(v: Sandbox, monkeypatch: pytest.MonkeyPatch) -> None:
    pkg, kit = v.package(), v.kit()
    monkeypatch.setattr(sys, "version_info", (3, 10, 12, "final", 0))  # has the backported data filter
    code, _, err = v.verify(pkg, kit)
    assert code == 2 and "is not Python 3.11 or newer" in err


def test_verify_cannot_run_another_targets_binary(v: Sandbox) -> None:
    code, _, err = v.verify(v.package(), v.kit(), host=MACOS)
    assert code == 2 and "does not run linux-x86_64" in err
    assert not v.runs()


def test_verify_cannot_run_when_the_engine_does_not_answer(v: Sandbox, monkeypatch: pytest.MonkeyPatch) -> None:
    pkg, kit = v.package(), v.kit()
    monkeypatch.setenv("FAKE_DOCKER_DOWN", "1")
    code, _, err = v.verify(pkg, kit, container="docker")
    assert code == 2 and "`docker info` fails" in err
    assert not v.runs()


def test_verify_has_no_container_for_macos(v: Sandbox) -> None:
    code, _, err = v.verify(v.package(MACOS), v.kit(), MACOS, container="docker")
    assert code == 2 and "is for the Linux targets" in err
    assert not v.docker_calls()


def test_a_container_that_fails_still_has_its_image_removed(v: Sandbox, monkeypatch: pytest.MonkeyPatch) -> None:
    pkg, kit = v.package(), v.kit()
    monkeypatch.setenv("FAKE_DOCKER_FAIL", "1")
    code, out, _ = v.verify(pkg, kit, container="docker")
    assert code == 1 and "static-gates: FAIL `antb1 version` runs in a container and says 0.1.0" in out
    assert v.docker_calls()[-1] == ["rmi", "--force", FAKE_DOCKER_IMAGE]


def test_verify_reads_past_what_the_engine_prints_first(v: Sandbox, monkeypatch: pytest.MonkeyPatch) -> None:
    pkg, kit = v.package(), v.kit()
    monkeypatch.setenv("FAKE_DOCKER_BANNER", "1")  # podman's docker emulation prints a banner on every call
    code, out, err = v.verify(pkg, kit, container="docker")
    assert code == 0, out + err


def test_a_container_that_hangs_is_removed(v: Sandbox, monkeypatch: pytest.MonkeyPatch) -> None:
    pkg, kit = v.package(), v.kit()
    real_run = subprocess.run

    def run(cmd: list[str], *args: object, **kw: object) -> subprocess.CompletedProcess:
        if Path(cmd[0]).name == "docker" and cmd[1] == "run":  # no sleep: the timeout itself is simulated
            raise subprocess.TimeoutExpired(cmd, 120)
        return real_run(cmd, *args, **kw)

    monkeypatch.setattr(subprocess, "run", run)
    code, out, _ = v.verify(pkg, kit, container="docker")
    assert code == 1
    assert "static-gates: FAIL `antb1 version` runs in a container and says 0.1.0: timed out after 120 s" in out
    removed = [call for call in v.docker_calls() if call[:2] == ["rm", "--force"]]
    assert removed and all(call[2].startswith("antb1-verify-") for call in removed)
    assert v.docker_calls()[-1] == ["rmi", "--force", FAKE_DOCKER_IMAGE]


def test_an_image_the_engine_refuses_is_never_run(v: Sandbox, monkeypatch: pytest.MonkeyPatch) -> None:
    pkg, kit = v.package(), v.kit()
    monkeypatch.setenv("FAKE_DOCKER_IMPORT_FAIL", "1")
    code, out, _ = v.verify(pkg, kit, container="docker")
    assert code == 1
    assert fails(out) == ["static-gates: FAIL the engine imports an image of /antb1 and /kit: fake docker: no import"]
    assert [call[0] for call in v.docker_calls()] == ["info", "import"]  # no run, nothing to remove


def test_verify_checks_the_signature_on_macos(v: Sandbox, monkeypatch: pytest.MonkeyPatch) -> None:
    pkg, kit = v.package(MACOS), v.kit()
    code, out, err = v.verify(pkg, kit, MACOS)
    assert code == 0, out + err
    calls = [json.loads(line) for line in Path(v.fake_env["FAKE_CODESIGN_LOG"]).read_text().splitlines()]
    assert len(calls) == 1 and calls[0][:2] == ["--verify", "--strict"]
    assert calls[0][2].endswith(f"/antb1-{VERSION}-{MACOS}/antb1")
    monkeypatch.setenv("FAKE_CODESIGN_EXIT", "1")
    code, out, _ = v.verify(pkg, kit, MACOS)
    assert code == 1 and fails(out) == ["static-gates: FAIL the signature verifies (codesign --verify --strict)"]


@pytest.mark.skipif(
    (platform.system(), platform.machine()) != HOSTS[LINUX], reason="runs verify for real on a linux-x86_64 host"
)
def test_verify_runs_as_the_workflow_runs_it(v: Sandbox) -> None:
    pkg, kit = v.package(), v.kit()
    args = ["--assets", str(pkg), "--sums", str(pkg / "SHA256SUMS"), "--kit", str(kit), "--target", LINUX]
    result = v.box.run(
        "static_gates.py", "verify", *args, "--commit", v.box.commit, "--container", "docker", **v.fake_env
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert v.docker_calls()[-1] == ["rmi", "--force", FAKE_DOCKER_IMAGE]


def test_verify_needs_only_python_3_11_and_the_standard_library() -> None:
    tree = ast.parse(GATES.read_text(), feature_version=(3, 11))
    imported = {
        alias.name.split(".")[0] for node in ast.walk(tree) if isinstance(node, ast.Import) for alias in node.names
    }
    imported |= {
        node.module.split(".")[0] for node in ast.walk(tree) if isinstance(node, ast.ImportFrom) and node.module
    }
    assert imported <= set(sys.stdlib_module_names) | {"__future__"}


def test_the_static_workflow_verifies_each_package_with_the_runners_python() -> None:
    job = yaml.safe_load((REPO / ".github" / "workflows" / "static.yml").read_text())["jobs"]["static"]
    engines = {entry["target"]: entry["engine"] for entry in job["strategy"]["matrix"]["include"]}
    assert engines == {"linux-x86_64": "docker", "linux-aarch64": "docker", "darwin-arm64": "none"}
    runs = [step.get("run", "") for step in job["steps"]]
    (at,) = [i for i, run in enumerate(runs) if "static_gates.py verify" in run]
    step = job["steps"][at]
    assert step["run"].startswith("python3 -I scripts/static_gates.py verify ")  # the runner's Python, not pixi's
    assert '--container "$ENGINE"' in step["run"] and step["env"]["ENGINE"] == "${{ matrix.engine }}"
    assert at < min(i for i, run in enumerate(runs) if "static-sources" in run)  # before the sources and uploads
    assert "pixi run static-check build/static/package/antb1-" in step["name"]  # the local repro
    assert "pixi run static-check build/static/package/antb1-${{ matrix.target }}.tar.gz" in job["name"]


# ---------------------------------------------------------------------------------------------------------------------
# scripts/static-check.sh


def python_calls(v: Sandbox) -> list[str]:
    log = Path(v.fake_env["FAKE_PYTHON_LOG"])
    return log.read_text().splitlines() if log.exists() else []


def test_static_check_takes_only_a_package_tarball_with_its_sums(v: Sandbox) -> None:
    pkg = v.package()
    result = v.box.bash("static-check.sh", str(pkg / "antb1-linux-x86_64.debug.tar.gz"), **v.fake_env)
    assert result.returncode == 2 and "is not a package" in result.stderr
    (pkg / "antb1-linux-x86_64.debug.tar.gz").rename(v.box.tmp / "debug.tar.gz")
    result = v.box.bash("static-check.sh", str(pkg / "antb1-linux-x86_64.tar.gz"), **v.fake_env)
    assert result.returncode == 2 and "no antb1-linux-x86_64.debug.tar.gz next to" in result.stderr
    (pkg / "SHA256SUMS").unlink()
    result = v.box.bash("static-check.sh", str(pkg / "antb1-linux-x86_64.tar.gz"), **v.fake_env)
    assert result.returncode == 2 and "no SHA256SUMS next to" in result.stderr
    assert not python_calls(v)


def test_static_check_of_a_tarball_writes_a_kit_then_verifies_it_without_docker(v: Sandbox) -> None:
    pkg = v.package()
    tarball = pkg / "antb1-linux-x86_64.tar.gz"
    result = v.box.bash("static-check.sh", str(tarball), FAKE_PYTHON_DRY="1", FAKE_DOCKER_DOWN="1", **v.fake_env)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "no container run" in result.stdout
    kit_call, verify_call = python_calls(v)
    assert kit_call.startswith(f"scripts/static_gates.py kit --env {v.box.env} --out {v.box.tmpdir}/antb1-smoke-kit.")
    kit = kit_call.rsplit(" ", 1)[1]
    assert verify_call == (
        f"scripts/static_gates.py verify --assets {pkg} --sums {pkg}/SHA256SUMS --kit {kit} --target linux-x86_64"
        f" --texts {v.box.root} --container none"
    )
    assert not Path(kit).exists()  # removed on exit


@pytest.mark.skipif(platform.system() != "Linux", reason="static-check.sh uses docker only on Linux")
def test_static_check_of_a_tarball_uses_docker_when_it_answers(v: Sandbox) -> None:
    pkg = v.package()
    result = v.box.bash("static-check.sh", str(pkg / f"antb1-{LINUX}.tar.gz"), FAKE_PYTHON_DRY="1", **v.fake_env)
    assert result.returncode == 0 and "no container run" not in result.stdout
    assert python_calls(v)[1].endswith(" --container docker") and v.docker_calls() == [["info"]]


def test_static_check_of_a_binary_runs_the_binary_gate_as_before(v: Sandbox) -> None:
    result = v.box.bash("static-check.sh", "/somewhere/antb1", FAKE_PYTHON_DRY="1", **v.fake_env)
    assert result.returncode == 0, result.stdout + result.stderr
    assert python_calls(v) == [f"scripts/static_gates.py binary --bin /somewhere/antb1 --env {v.box.env}"]
    assert not v.docker_calls()  # only the tarball form asks docker
