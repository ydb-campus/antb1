"""Tests for tools/ci/coverage.py: path classification, per-module sums, the floors gate, the summary, and main() end
to end with fake llvm-profdata/llvm-cov tools."""

from __future__ import annotations

import json
import os
import re
import stat
import textwrap
from pathlib import Path

import coverage_py as cov
import pytest

ROOT = Path("/work/antb1")


def file_entry(path: str, lines: tuple[int, int], branches: tuple[int, int]) -> dict:
    return {
        "filename": path,
        "summary": {
            "lines": {"covered": lines[0], "count": lines[1]},
            "branches": {"covered": branches[0], "count": branches[1]},
        },
    }


def export_of(*entries: dict) -> dict:
    return {"data": [{"files": list(entries)}], "type": "llvm.coverage.json.export"}


EXPORT = export_of(
    file_entry(f"{ROOT}/src/sql/parser.cc", (90, 100), (30, 40)),
    file_entry(f"{ROOT}/src/sql/lexer.cc", (10, 20), (10, 10)),
    file_entry(f"{ROOT}/src/sql/tests/parser_test.cc", (5, 50), (1, 10)),
    file_entry(f"{ROOT}/src/common/include/antb1/common/check.h", (3, 4), (0, 0)),
    file_entry(f"{ROOT}/src/cli/cli.cc", (60, 100), (5, 10)),
    file_entry(f"{ROOT}/tools/fixturegen/fixtures.cc", (1, 100), (1, 100)),
    file_entry(f"{ROOT}/fuzz/replay_main.cc", (1, 100), (1, 100)),
    file_entry("/elsewhere/include/arrow/status.h", (1, 100), (1, 100)),
)


@pytest.mark.parametrize(
    ("path", "module"),
    [
        ("src/sql/parser.cc", "sql"),
        ("src/common/include/antb1/common/check.h", "common"),
        ("src/sql/tests/parser_test.cc", None),
        ("src/engine/tests/support/helpers.h", None),
        ("tests/slt/runner/runner.cc", None),
        ("tools/fixturegen/fixtures.cc", None),
        ("fuzz/replay_main.cc", None),
        ("bench/scan_bench.cc", None),
        (".pixi/envs/default/include/arrow/api.h", None),
        ("build/coverage/generated.cc", None),
        ("src/version.cc", None),
    ],
)
def test_module_of(path: str, module: str | None) -> None:
    assert cov.module_of(f"{ROOT}/{path}", ROOT) == module


def test_module_of_outside_the_source_root() -> None:
    assert cov.module_of("/usr/include/c++/15/vector", ROOT) is None
    assert cov.module_of("/work/antb1-other/src/sql/parser.cc", ROOT) is None


def test_ignore_regexes_are_anchored_at_the_escaped_root() -> None:
    root = Path("/w/a+b (1)/antb1")
    regexes = [re.compile(r) for r in cov.ignore_regexes(root)]

    def ignored(rel: str) -> bool:
        return any(r.search(f"{root}/{rel}") for r in regexes)

    assert ignored("tests/slt/runner.cc")
    assert ignored("src/sql/tests/parser_test.cc")
    assert ignored("tools/ci/x.cc")
    assert ignored("fuzz/replay_main.cc")
    assert ignored(".pixi/envs/default/include/x.h")
    assert not ignored("src/sql/parser.cc")
    assert not ignored("src/plan/testsuite.cc")
    # A checkout that lives under a directory named like an ignored one is still measured.
    other_root = [re.compile(r) for r in cov.ignore_regexes(Path("/home/tools/antb1"))]
    assert not any(r.search("/home/tools/antb1/src/sql/parser.cc") for r in other_root)


def test_summarize_sums_files_per_module_in_the_given_order() -> None:
    modules = cov.summarize(EXPORT, ROOT, ["common", "sql"])
    assert list(modules) == ["common", "sql", "cli"]
    sql = modules["sql"]
    assert (sql.lines.covered, sql.lines.count) == (100, 120)
    assert (sql.branches.covered, sql.branches.count) == (40, 50)
    assert sql.files == 2
    assert modules["common"].branches.percent is None


def floors_of(**modules: tuple[float, float]) -> dict[str, dict[str, float]]:
    return {m: {"lines": lines, "branches": branches} for m, (lines, branches) in modules.items()}


def test_evaluate_passes_at_and_above_the_floor_and_na_branches() -> None:
    modules = cov.summarize(EXPORT, ROOT)
    floors = floors_of(sql=(83.3, 80.0), common=(75.0, 99.0), cli=(60.0, 40.0))
    results, failures = cov.evaluate(modules, floors)
    assert failures == []
    common_branches = next(r for r in results if r.module == "common" and r.metric == "branches")
    assert common_branches.measured is None
    assert common_branches.ok


def test_evaluate_names_module_measured_and_floor() -> None:
    modules = cov.summarize(EXPORT, ROOT)
    floors = floors_of(sql=(90.0, 75.0), common=(75.0, 75.0), cli=(60.0, 40.0))
    _, failures = cov.evaluate(modules, floors)
    assert len(failures) == 1
    assert "module 'sql'" in failures[0]
    assert "line coverage 83.33%" in failures[0]
    assert "floor 90.0%" in failures[0]
    _, failures = cov.evaluate(modules, floors_of(sql=(0.0, 81.0), common=(75.0, 75.0), cli=(60.0, 40.0)), "t.json")
    assert failures == [
        "module 'sql': branch coverage 80.00% is below its floor 81.0% (t.json); "
        "add tests for the uncovered code (build/coverage/report.txt)"
    ]


def test_evaluate_requires_a_floor_for_every_module_and_data_for_every_floor() -> None:
    modules = cov.summarize(EXPORT, ROOT)
    floors = floors_of(sql=(0.0, 0.0), common=(0.0, 0.0), exec=(85.0, 70.0))
    _, failures = cov.evaluate(modules, floors)
    assert any("module 'cli' has no floor" in f for f in failures)
    assert any("module 'exec' has a floor" in f and "no coverage data" in f for f in failures)


def test_evaluate_without_thresholds_never_fails() -> None:
    _, failures = cov.evaluate(cov.summarize(EXPORT, ROOT), None)
    assert failures == []


def test_load_thresholds_skips_comments_and_validates(tmp_path: Path) -> None:
    good = tmp_path / "good.json"
    good.write_text(json.dumps({"_comment": ["x"], "sql": {"lines": 90, "branches": 75.5}}))
    assert cov.load_thresholds(good) == {"sql": {"lines": 90.0, "branches": 75.5}}
    for bad in (
        {"sql": {"lines": 90}},
        {"sql": {"lines": 90, "branches": 101}},
        {"sql": {"lines": "90", "branches": 75}},
        {"sql": {"lines": 90, "branches": 75, "regions": 1}},
        ["sql"],
    ):
        path = tmp_path / "bad.json"
        path.write_text(json.dumps(bad))
        with pytest.raises(cov.CoverageError):
            cov.load_thresholds(path)


def test_render_markdown() -> None:
    modules = cov.summarize(EXPORT, ROOT, ["common", "sql", "cli"])
    results, failures = cov.evaluate(modules, floors_of(sql=(90.0, 75.0), common=(75.0, 75.0), cli=(60.0, 40.0)))
    text = cov.render_markdown(modules, results, failures, gated=True)
    assert "| `sql` | 83.33% (100/120) | 90.0% | 80.00% (40/50) | 75.0% | **below floor** |" in text
    assert "| `common` | 75.00% (3/4) | 75.0% | N/A | 75.0% | ok |" in text
    assert "| **total** | 72.77% (163/224) | | 75.00% (45/60) | |" in text
    assert "**Coverage gate: FAIL**" in text


FAKE_TOOL = """\
#!/usr/bin/env bash
# Fake llvm-profdata / llvm-cov for the tests: `merge ... -o FILE` creates FILE, `export -format=text` prints the
# canned JSON next to this script, `export -format=lcov` and `report` print a marker.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
echo "$*" >> "$here/calls.log"
case "$1" in
  merge) while [ $# -gt 0 ]; do if [ "$1" = "-o" ]; then : > "$2"; fi; shift; done ;;
  export) if [[ " $* " == *" -format=lcov "* ]]; then echo "SF:fake"; else cat "$here/export.json"; fi ;;
  report) echo "TOTAL fake report" ;;
  *) exit 9 ;;
esac
"""


@pytest.fixture
def fake_build(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    tools = tmp_path / "tools"
    tools.mkdir()
    tool = tools / "fake-llvm"
    tool.write_text(textwrap.dedent(FAKE_TOOL))
    tool.chmod(tool.stat().st_mode | stat.S_IXUSR)
    (tools / "export.json").write_text(json.dumps(EXPORT))
    monkeypatch.setenv("LLVM_PROFDATA", str(tool))
    monkeypatch.setenv("LLVM_COV", str(tool))

    build = tmp_path / "build" / "coverage"
    (build / "prof").mkdir(parents=True)
    (build / "prof" / "1-2.profraw").write_bytes(b"raw")
    (build / "bin").mkdir()
    for name in ("antb1_sql_tests", "antb1", "antb1-slt"):
        binary = build / "bin" / name
        binary.write_text("")
        binary.chmod(0o755)
    (build / "bin" / "not-executable.txt").write_text("")
    return build


def run_main(build: Path, floors: dict | None) -> int:
    args = [str(build), "--source-root", str(ROOT)]
    if floors is not None:
        thresholds = build / "thresholds.json"
        thresholds.write_text(json.dumps({"_comment": "test", **floors}))
        args += ["--thresholds", str(thresholds)]
    return cov.main(args)


def test_main_passes_and_writes_the_outputs(fake_build: Path) -> None:
    rc = run_main(fake_build, floors_of(common=(75.0, 75.0), sql=(80.0, 75.0), cli=(60.0, 40.0)))
    assert rc == cov.EXIT_OK
    summary = (fake_build / "summary.md").read_text()
    assert summary.index("`common`") < summary.index("`sql`") < summary.index("`cli`")
    assert "Coverage gate: PASS" in summary
    assert (fake_build / "coverage.lcov").read_text() == "SF:fake\n"
    assert (fake_build / "report.txt").read_text() == "TOTAL fake report\n"
    calls = (fake_build.parents[1] / "tools" / "calls.log").read_text().splitlines()
    assert calls[0].startswith("merge -sparse --input-files=")
    export = next(c for c in calls if c.startswith("export -format=lcov"))
    # antb1 is the positional binary; every other executable is an -object; non-executables are skipped.
    assert re.search(
        r"-instr-profile=\S+ \S+/bin/antb1 -object \S+/bin/antb1-slt -object \S+/bin/antb1_sql_tests ", export
    )
    assert "not-executable" not in export
    assert "-ignore-filename-regex=^/work/antb1/(fuzz|bench|tools|\\.pixi|build)/" in export


def test_main_fails_below_a_floor(fake_build: Path, capsys: pytest.CaptureFixture[str]) -> None:
    rc = run_main(fake_build, floors_of(common=(75.0, 75.0), sql=(90.0, 75.0), cli=(60.0, 40.0)))
    assert rc == cov.EXIT_BELOW_FLOOR
    err = capsys.readouterr().err
    assert "coverage: FAIL module 'sql': line coverage 83.33% is below its floor 90.0%" in err


def test_main_without_profiles_is_an_error(fake_build: Path, capsys: pytest.CaptureFixture[str]) -> None:
    for raw in (fake_build / "prof").iterdir():
        raw.unlink()
    assert run_main(fake_build, None) == cov.EXIT_ERROR
    assert "no .profraw files" in capsys.readouterr().err


def test_main_without_the_cli_binary_is_an_error(fake_build: Path, capsys: pytest.CaptureFixture[str]) -> None:
    os.remove(fake_build / "bin" / "antb1")
    assert run_main(fake_build, None) == cov.EXIT_ERROR
    assert "no antb1 binary" in capsys.readouterr().err
