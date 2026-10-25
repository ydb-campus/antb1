"""Tests for the benchmark tasks on the static build: scripts/bench-clickbench.sh, scripts/bench-micro.sh and the
`--build` mode of scripts/static.sh (run by `pixi run lint`; docs/benchmarks.md).

They cover what each script builds and runs, ANTB1_BENCH_OUT, a given binary (ANTB1_BENCH_BIN) and the checks it
must pass, cold and lukewarm runs, and that every bad setting fails before anything is built. They are hermetic: the
real scripts run in a sandbox repository whose static-arrow.sh is a fake, with fake `cmake`, `antb1`, `sudo` and
`uname` on PATH, and an environment built from scratch (PIXI_PROJECT_ROOT included), so nothing builds Arrow, touches
the real build tree or reaches the network.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]
SCRIPTS = ["bench-clickbench.sh", "bench-micro.sh", "static.sh"]
SHA = "0123456789abcdef0123456789abcdef01234567"

# Every fake appends one line to $FAKE_LOG, so a test sees what ran and in which order.
FAKE_STATIC_ARROW = """#!/usr/bin/env bash
echo "static-arrow $*" >>"$FAKE_LOG"
if [ "${1:-}" = --print-prefix ]; then echo "$FAKE_PREFIX"; fi
"""
# For `--build ... --target X...`, the fake cmake puts the fake binary of each target into build/static/bin.
FAKE_CMAKE = """#!{python} -I
import os, shutil, sys
args = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write("cmake " + " ".join(args) + "\\n")
if "--build" in args and "--target" in args:
    for target in args[args.index("--target") + 1 :]:
        os.makedirs("build/static/bin", exist_ok=True)
        shutil.copy(os.path.join(os.environ["FAKE_DIR"], target), os.path.join("build/static/bin", target))
"""
# The fake antb1 prints a `version --verbose` from $FAKE_FLAVOR, $FAKE_BUILD_TYPE and $FAKE_PURGE, and writes the
# --out file of `bench`.
FAKE_ANTB1 = """#!{python} -I
import os, sys
args = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    purge = os.environ.get("MIMALLOC_PURGE_DELAY", "")
    log.write("antb1 argv0=%s purge=%s %s\\n" % (sys.argv[0], purge, " ".join(args)))
if args == ["version", "--verbose"] and os.environ.get("FAKE_VERSION_EXIT"):
    print("The following argument was not expected: --verbose", file=sys.stderr)
    sys.exit(int(os.environ["FAKE_VERSION_EXIT"]))
if args == ["version", "--verbose"]:
    print("antb1 0.1.0 (Apache Arrow 25.0.0)")
    print("commit: " + os.environ.get("FAKE_COMMIT", "abc123"))
    print("build type: " + os.environ.get("FAKE_BUILD_TYPE", "Release"))
    print("flavor: " + os.environ.get("FAKE_FLAVOR", "static-thinlto"))
    print("memory pool: mimalloc")
    print("mimalloc purge delay: " + os.environ.get("FAKE_PURGE", "-1"))
    sys.exit(0)
if args[:1] == ["bench"]:
    with open(args[args.index("--out") + 1], "w") as f:
        f.write("{}\\n")
    sys.exit(0)
sys.exit(70)
"""
FAKE_MICRO = """#!{python} -I
import os, sys
args = sys.argv[1:]
with open(os.environ["FAKE_LOG"], "a") as log:
    log.write("micro " + " ".join(args) + "\\n")
for arg in args:
    if arg.startswith("--benchmark_out="):
        with open(arg.split("=", 1)[1], "w") as f:
            f.write("{}\\n")
"""
# The fake sudo logs to its own file and fails unless asked non-interactively (`sudo -n`).
FAKE_SUDO = """#!/bin/sh
echo "sudo $*" >>"$FAKE_SUDO_LOG"
[ "${1:-}" = -n ] || exit 99
exit "${FAKE_SUDO_EXIT:-1}"
"""
FAKE_UNAME = """#!/bin/sh
case "$1" in
  -s) echo "${FAKE_UNAME_S:-Linux}" ;;
  -m) echo x86_64 ;;
  *) echo "${FAKE_UNAME_S:-Linux}" ;;
esac
"""


class Sandbox:
    """A fake repository with copies of the real scripts, a complete fake Arrow prefix and fakes on PATH."""

    def __init__(self, tmp: Path) -> None:
        self.tmp = tmp
        self.root = tmp / "root"
        (self.root / "scripts").mkdir(parents=True)
        for name in SCRIPTS:
            shutil.copy(REPO / "scripts" / name, self.root / "scripts" / name)
        self.write(self.root / "scripts" / "static-arrow.sh", FAKE_STATIC_ARROW)
        self.prefix = tmp / "arrow" / "0123456789abcdef" / "prefix"
        self.prefix.mkdir(parents=True)
        (self.prefix.parent / ".complete").write_text("")
        self.fakes = tmp / "fakes"
        self.fakes.mkdir()
        self.write(self.fakes / "antb1", FAKE_ANTB1)
        self.write(self.fakes / "antb1-bench-micro", FAKE_MICRO)
        self.bin = tmp / "bin"
        self.bin.mkdir()
        for name, text in (("cmake", FAKE_CMAKE), ("sudo", FAKE_SUDO), ("uname", FAKE_UNAME)):
            self.write(self.bin / name, text)
        self.data = tmp / "data"
        self.data.mkdir()
        (self.data / "queries.sql").write_text("-- stub, never run\n")
        (self.data / "hits_0.parquet").write_bytes(b"stub")
        self.log = tmp / "fake.log"
        self.sudo_log = tmp / "sudo.log"

    @staticmethod
    def write(path: Path, text: str) -> None:
        path.write_text(text.replace("{python}", sys.executable))
        path.chmod(0o755)

    def run(self, script: str, *args: str, **env: str) -> subprocess.CompletedProcess[str]:
        self.log.write_text("")
        self.sudo_log.write_text("")
        base = {
            "PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}",
            "HOME": str(self.tmp),
            "PIXI_PROJECT_ROOT": str(self.root),
            "GITHUB_SHA": SHA,
            "ANTB1_DATA_DIR": str(self.data),
            "FAKE_LOG": str(self.log),
            "FAKE_SUDO_LOG": str(self.sudo_log),
            "FAKE_PREFIX": str(self.prefix),
            "FAKE_DIR": str(self.fakes),
        }
        bash = shutil.which("bash")
        assert bash is not None
        return subprocess.run(
            [bash, str(self.root / "scripts" / script), *args],
            cwd=self.tmp,
            env={**base, **env},
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )

    def ran(self) -> list[str]:
        return self.log.read_text().splitlines()

    def sudo_calls(self) -> list[str]:
        return self.sudo_log.read_text().splitlines()

    def antb1_runs(self) -> list[str]:
        return [line for line in self.ran() if line.startswith("antb1 ")]

    @property
    def results(self) -> Path:
        return self.root / "build" / "static" / "bench-results"


@pytest.fixture
def box(tmp_path: Path) -> Sandbox:
    return Sandbox(tmp_path)


def test_bench_clickbench_builds_antb1_in_build_static_and_records_it(box: Sandbox) -> None:
    r = box.run("bench-clickbench.sh", "--threads", "4")
    assert r.returncode == 0, r.stderr
    built = str(box.root / "build" / "static" / "bin" / "antb1")
    assert box.ran()[:4] == [
        "static-arrow ",
        "static-arrow --print-prefix",
        f"cmake --preset static -DANTB1_GIT_SHA={SHA}",
        "cmake --build --preset static --target antb1",
    ]
    version, bench = box.antb1_runs()
    assert version == f"antb1 argv0={built} purge= version --verbose"
    assert bench.startswith(f"antb1 argv0={built} purge= bench --clickbench --queries {box.data}/queries.sql")
    assert f"--table hits={box.data}/hits_0.parquet --tries 3 --out {box.results}/clickbench.json --threads 4" in bench
    assert "--git-sha" not in bench and "--drop-caches" not in bench  # the binary records its own commit
    assert "flavor: static-thinlto" in (box.results / "version.txt").read_text()
    assert (box.results / "clickbench.json").exists()
    assert "lukewarm runs" in r.stdout


@pytest.mark.parametrize(
    ("system", "sudo", "cold"), [("Linux", "0", True), ("Linux", "1", False), ("Darwin", "0", False)]
)
def test_cold_runs_only_on_linux_with_sudo(box: Sandbox, system: str, sudo: str, cold: bool) -> None:
    r = box.run("bench-clickbench.sh", FAKE_UNAME_S=system, FAKE_SUDO_EXIT=sudo)
    assert r.returncode == 0, r.stderr
    assert ("--drop-caches" in box.antb1_runs()[-1]) == cold
    assert ("cold runs" in r.stdout) == cold
    assert box.sudo_calls() == (["sudo -n true"] if system == "Linux" else [])
    assert box.run("bench-clickbench.sh", ANTB1_BENCH_COLD="0", FAKE_SUDO_EXIT="0").returncode == 0
    assert "--drop-caches" not in box.antb1_runs()[-1]
    assert box.sudo_calls() == []


def test_bench_out_keeps_the_results_of_each_arm_apart(box: Sandbox) -> None:
    assert box.run("bench-clickbench.sh", ANTB1_BENCH_OUT="arms/a").returncode == 0
    assert box.run("bench-clickbench.sh", ANTB1_BENCH_OUT=str(box.tmp / "b")).returncode == 0
    for out in (box.results / "arms" / "a", box.tmp / "b"):
        assert sorted(p.name for p in out.iterdir()) == ["clickbench.json", "version.txt"]
    assert sorted(p.name for p in box.root.iterdir()) == ["build", "scripts"]  # nothing in the work tree


def test_a_given_binary_runs_as_its_resolved_file_without_any_build(box: Sandbox) -> None:
    link = box.tmp / "links" / "other-name"
    link.parent.mkdir()
    link.symlink_to(box.fakes / "antb1")
    r = box.run("bench-clickbench.sh", ANTB1_BENCH_BIN=str(link), FAKE_COMMIT="given-commit")
    assert r.returncode == 0, r.stderr
    resolved = str((box.fakes / "antb1").resolve())
    assert [line.split(" ", 2)[1] for line in box.ran()] == [f"argv0={resolved}"] * 2  # version, then bench
    assert (box.results / "clickbench.json").exists()
    version = (box.results / "version.txt").read_text()
    assert "commit: given-commit" in version and "flavor: static-thinlto" in version


@pytest.mark.parametrize(
    ("problem", "message"),
    [
        ("relative", "must be an absolute path, got"),
        ("missing", "is not an executable file"),
        ("not-executable", "is not an executable file"),
    ],
)
def test_a_given_binary_must_be_an_absolute_executable_file(box: Sandbox, problem: str, message: str) -> None:
    plain = box.tmp / "antb1.txt"
    plain.write_text("not a program")
    # ../fakes/antb1 is the real fake, seen from the repository root the script works in.
    given = {"relative": "../fakes/antb1", "missing": str(box.tmp / "nope" / "antb1"), "not-executable": str(plain)}
    r = box.run("bench-clickbench.sh", ANTB1_BENCH_BIN=given[problem])
    assert r.returncode == 2
    assert message in r.stderr
    assert box.ran() == []


@pytest.mark.parametrize(
    ("flavor", "build_type"), [("dynamic", "Release"), ("static-thinlto", "Debug"), ("dynamic", "RelWithDebInfo")]
)
def test_another_build_is_refused_unless_any_build_is_set(box: Sandbox, flavor: str, build_type: str) -> None:
    given = {"ANTB1_BENCH_BIN": str(box.fakes / "antb1"), "FAKE_FLAVOR": flavor, "FAKE_BUILD_TYPE": build_type}
    r = box.run("bench-clickbench.sh", **given)
    assert r.returncode == 2
    assert f"not the standalone static Release build (flavor '{flavor}', build type '{build_type}')" in r.stderr
    assert "ANTB1_BENCH_ANY_BUILD=1 runs it anyway" in r.stderr
    assert len(box.antb1_runs()) == 1  # only version --verbose
    r = box.run("bench-clickbench.sh", ANTB1_BENCH_ANY_BUILD="1", **given)
    assert r.returncode == 0, r.stderr
    assert "warning:" in r.stderr and "running it anyway" in r.stderr
    assert box.antb1_runs()[-1].split(" ", 3)[3].startswith("bench --clickbench")


def test_a_failing_version_check_stops_the_run(box: Sandbox) -> None:
    r = box.run("bench-clickbench.sh", ANTB1_BENCH_BIN=str(box.fakes / "antb1"), FAKE_VERSION_EXIT="109")
    assert r.returncode == 2
    assert "version --verbose failed" in r.stderr
    assert "The following argument was not expected: --verbose" in r.stderr  # the binary's own words
    assert len(box.antb1_runs()) == 1
    assert not (box.results / "version.txt").exists() and not (box.results / "clickbench.json").exists()


def test_a_binary_whose_allocator_restart_failed_is_refused_on_linux(box: Sandbox) -> None:
    given = str(box.fakes / "antb1")
    # Without the restart and without a setting of its own, antb1 reports mimalloc's "default" (src/cli/cli.cc).
    r = box.run("bench-clickbench.sh", ANTB1_BENCH_BIN=given, FAKE_PURGE="default")
    assert r.returncode == 2
    assert "did not restart for its allocator (mimalloc purge delay 'default', not -1" in r.stderr
    # The pool of a mimalloc build still restarts, so naming it changes nothing.
    pool = {"ARROW_DEFAULT_MEMORY_POOL": "mimalloc"}
    assert box.run("bench-clickbench.sh", ANTB1_BENCH_BIN=given, FAKE_PURGE="default", **pool).returncode == 2
    # macOS has no restart (RestartForAllocator is Linux-only): nothing to check there.
    darwin = {"FAKE_PURGE": "default", "FAKE_UNAME_S": "Darwin"}
    assert box.run("bench-clickbench.sh", ANTB1_BENCH_BIN=given, **darwin).returncode == 0


@pytest.mark.parametrize("setting", [{"MIMALLOC_PURGE_DELAY": "1000"}, {"ARROW_DEFAULT_MEMORY_POOL": "system"}])
def test_allocator_settings_from_the_environment_are_printed_and_accepted(
    box: Sandbox, setting: dict[str, str]
) -> None:
    r = box.run("bench-clickbench.sh", ANTB1_BENCH_BIN=str(box.fakes / "antb1"), FAKE_PURGE="1000", **setting)
    assert r.returncode == 0, r.stderr
    assert "allocator settings from the environment" in r.stdout
    assert f"purge={setting.get('MIMALLOC_PURGE_DELAY', '')} bench" in box.antb1_runs()[-1]


@pytest.mark.parametrize(
    "env",
    [
        {"ANTB1_BENCH_TRIES": "0"},
        {"ANTB1_BENCH_COLD": "yes"},
        {"ANTB1_BENCH_ANY_BUILD": "yes"},
        {"ANTB1_QUERIES_FILE": "/nonexistent/queries.sql"},
    ],
)
def test_a_bad_setting_fails_before_anything_is_built(box: Sandbox, env: dict[str, str]) -> None:
    r = box.run("bench-clickbench.sh", **env)
    assert r.returncode == 2
    assert box.ran() == []


def test_hits_lists_and_outputs_are_checked_before_anything_is_built(box: Sandbox) -> None:
    hits = f"{box.data}/hits_0.parquet,{box.data}/missing.parquet"
    r = box.run("bench-clickbench.sh", ANTB1_HITS_FILES=hits)
    assert r.returncode == 2
    assert "missing ClickBench data" in r.stderr and box.ran() == []
    (box.tmp / "a-file").write_text("")
    for script in ("bench-clickbench.sh", "bench-micro.sh"):
        r = box.run(script, ANTB1_BENCH_OUT=str(box.tmp / "a-file"))
        assert r.returncode == 2
        assert "is not a writable directory" in r.stderr and box.ran() == []
    # A glob is antb1's to expand, so it is passed on unchecked.
    r = box.run("bench-clickbench.sh", ANTB1_HITS_FILES=f"{box.data}/hits_0.parquet,{box.data}/hits_*.parquet")
    assert r.returncode == 0, r.stderr
    assert f"--table hits={box.data}/hits_0.parquet,{box.data}/hits_*.parquet " in box.antb1_runs()[-1]


def test_bench_micro_builds_the_micro_benchmarks_and_writes_micro_json(box: Sandbox) -> None:
    r = box.run("bench-micro.sh", "--benchmark_filter=Sum")
    assert r.returncode == 0, r.stderr
    assert box.ran() == [
        "static-arrow ",
        "static-arrow --print-prefix",
        f"cmake --preset static -DANTB1_GIT_SHA={SHA}",
        "cmake --build --preset static --target antb1-bench-micro",
        f"micro --benchmark_out={box.results}/micro.json --benchmark_out_format=json --benchmark_filter=Sum",
    ]
    assert (box.results / "micro.json").exists()
    assert box.run("bench-micro.sh", ANTB1_BENCH_OUT="arms/a").returncode == 0
    assert (box.results / "arms" / "a" / "micro.json").exists()


def test_bench_micro_refuses_a_given_binary_before_building(box: Sandbox) -> None:
    r = box.run("bench-micro.sh", ANTB1_BENCH_BIN=str(box.fakes / "antb1"))
    assert r.returncode == 2
    assert "ANTB1_BENCH_BIN is for `pixi run bench-clickbench`" in r.stderr
    assert box.ran() == []


def test_static_build_mode_builds_only_the_named_targets(box: Sandbox) -> None:
    r = box.run("static.sh", "--build", "antb1", "antb1-bench-micro")
    assert r.returncode == 0, r.stderr
    assert box.ran() == [
        "static-arrow --print-prefix",
        f"cmake --preset static -DANTB1_GIT_SHA={SHA}",
        "cmake --build --preset static --target antb1 antb1-bench-micro",
    ]
    assert sorted(p.name for p in (box.root / "build" / "static" / "bin").iterdir()) == ["antb1", "antb1-bench-micro"]
    assert not (box.root / "build" / "static" / "package").exists()


@pytest.mark.parametrize("args", [["--build"], ["--bogus"]])
def test_static_rejects_a_build_without_targets_and_unknown_options(box: Sandbox, args: list[str]) -> None:
    r = box.run("static.sh", *args)
    assert r.returncode == 2
    assert box.ran() == []


def test_static_needs_a_complete_prefix(box: Sandbox) -> None:
    (box.prefix.parent / ".complete").unlink()
    r = box.run("static.sh", "--build", "antb1")
    assert r.returncode == 2
    assert "no complete Arrow prefix" in r.stderr
    assert box.ran() == ["static-arrow --print-prefix"]
