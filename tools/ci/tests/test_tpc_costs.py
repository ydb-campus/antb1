"""Tests for tools/ci/tpc_costs.py (run by `pixi run lint`): the report of each query's seconds and antb1's peak memory
in the ratchets of the queries derived from TPC-H and TPC-DS (ADR 0037's HC3), its budget warnings, that it fails
closed without printing what it refuses, and that every CI leg that runs those ratchets reports them.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

import pytest
import yaml

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import tpc_costs

ROOT = Path(__file__).resolve().parents[3]
GIB = 2**30


def header(test: str, workload: str = "tpcds", threads: int = 1, same: int | None = None) -> dict:
    return {
        "record": "run",
        "workload": workload,
        "test": test,
        "shard": "1/1",
        "threads": threads,
        "same_as_threads": same,
        "memory_limit_bytes": 2 * GIB,
    }


def query(n: int, outcome: str = "pass", ratchet: str = "ok", seconds: float = 1.0, peak: int | None = 2**20) -> list:
    answered = outcome == "pass"
    return [
        {"record": "running", "query": n},
        {
            "record": "query",
            "query": n,
            "outcome": outcome,
            "ratchet": ratchet,
            "seconds": seconds,
            "antb1_seconds": seconds / 2 if answered else None,
            "peak_memory_bytes": peak if answered else None,
        },
    ]


def end(queries: int, passed: int, failed: int = 0, wall: float = 10.0) -> dict:
    return {"record": "end", "queries": queries, "passed": passed, "failed": failed, "wall_seconds": wall}


def write(build: Path, test: str, records: list) -> None:
    """Records as JSON lines; a str is written as it is and bytes as raw bytes, for the malformed cases."""
    path = build / "tpc-costs" / f"{test}.jsonl"
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = []
    for r in records:
        line = r if isinstance(r, bytes) else (r if isinstance(r, str) else json.dumps(r)).encode()
        lines.append(line + b"\n")
    path.write_bytes(b"".join(lines))


def report(build: Path, monkeypatch, capsys, actions: bool = False) -> tuple[int, str, str, str]:
    """The exit code, stdout, stderr and the step summary of one run of the tool."""
    summary = build.parent / f"{build.name}.summary.md"
    summary.unlink(missing_ok=True)
    monkeypatch.setenv("GITHUB_STEP_SUMMARY", str(summary))
    monkeypatch.setenv("RUNNER_OS", "Linux")
    monkeypatch.setenv("RUNNER_ARCH", "X64")
    if actions:
        monkeypatch.setenv("GITHUB_ACTIONS", "true")
    else:
        monkeypatch.delenv("GITHUB_ACTIONS", raising=False)
    code = tpc_costs.main([str(build)])
    out, err = capsys.readouterr()
    return code, out, err, summary.read_text(encoding="utf-8") if summary.exists() else ""


def test_reports_each_test_its_sf1_passes_and_the_disclaimers(tmp_path, monkeypatch, capsys) -> None:
    write(tmp_path, "tpch.status.sf0_1", [header("tpch.status.sf0_1", "tpch"), *query(1), end(1, 1)])
    shard = "tpcds.status.sf1.2of8"
    write(tmp_path, shard, [header(shard), *query(2), *query(10, "unsupported"), end(2, 1, wall=30.0)])
    parallel = "parallel.tpcds.status.sf0_1"
    write(tmp_path, parallel, [header(parallel, threads=4, same=1), *query(2, peak=2 * GIB), end(1, 1)])
    code, out, err, summary = report(tmp_path, monkeypatch, capsys)
    assert (code, err) == (0, ""), err
    assert all(line.startswith("tpc_costs: ") for line in out.splitlines())  # never a workflow command
    assert "(test-build diagnostics, Linux X64):" in out.splitlines()[0]
    assert "tpch.status.sf0_1: 1 queries, 1 passed, 0 failed, 10.0 s (1% of 870 s), peak 1.0 MiB" in out
    assert f"{shard}: 2 queries, 1 passed, 0 failed, 30.0 s (3% of 870 s), peak 1.0 MiB" in out
    assert "Q10: unsupported, ok, 1.000 s, antb1 - s, peak - MiB" in out
    assert "::warning" not in out  # only on GitHub Actions, and nothing misses the budget here
    for workload in ("tpch", "tpcds"):
        assert out.count(tpc_costs.DISCLAIMERS[workload]) == 1
        assert summary.count(tpc_costs.DISCLAIMERS[workload]) == 1
    assert "### Costs of the queries derived from TPC-H and TPC-DS" in summary
    assert "not performance numbers: Linux X64" in summary
    assert f"| {shard} | 2 | 1 | 0 | 30.0 | 3% | 1.0 |  |" in summary
    assert f"| {parallel} | 1 | 1 | 0 | 10.0 | 1% | 2048.0 | 4 threads: peaks vary |" in summary
    # Decision 5's table holds the passing queries of the scale-factor-1 shards only.
    decision5 = summary.split("ADR 0037, Decision 5):", 1)[1].split("<details>", 1)[0]
    assert f"| Q2 | {shard} | 1.000 | 0.500 | 1.0 |" in decision5
    assert parallel not in decision5 and "Q10" not in decision5


def test_no_files_is_one_line_and_success(tmp_path, monkeypatch, capsys) -> None:
    code, out, err, summary = report(tmp_path, monkeypatch, capsys, actions=True)
    assert (code, err, summary) == (0, "", "")
    assert out.splitlines() == [f"tpc_costs: no costs of the queries derived from TPC-H or TPC-DS in {tmp_path}"]


def test_a_shard_that_did_not_finish_is_named_with_its_query_and_warned_once(tmp_path, monkeypatch, capsys) -> None:
    shard = "tpcds.status.sf1.4of8"
    write(tmp_path, shard, [header(shard), *query(4, seconds=500.0), {"record": "running", "query": 12}])
    sf0_1 = "tpcds.status.sf0_1"  # not a scale-factor-1 shard: noted, not warned
    write(tmp_path, sf0_1, [header(sf0_1), {"record": "running", "query": 1}])
    code, out, _, summary = report(tmp_path, monkeypatch, capsys, actions=True)
    assert code == 0
    # Counted from its records, its seconds a lower bound and its share unknown.
    assert f"{shard}: 1 queries, 1 passed, 0 failed, >= 500.0 s (- of 870 s), peak 1.0 MiB" in out
    assert "note: did not finish: Q12 was running when it stopped (a timeout or a crash" in out
    assert f"| {shard} | 1 | 1 | 0 | >= 500.0 | - | 1.0 | did not finish: Q12 was running" in summary
    assert f"| {sf0_1} | 0 | 0 | 0 | >= 0.0 | - | - | did not finish: Q1 was running" in summary
    warnings = [line for line in out.splitlines() if line.startswith("::")]
    assert len(warnings) == 1, warnings
    assert shard in warnings[0] and "did not finish: Q12 was running" in warnings[0]


def test_warns_once_per_sf1_shard_that_misses_the_budget(tmp_path, monkeypatch, capsys) -> None:
    slow = "tpcds.status.sf1.1of8"
    write(tmp_path, slow, [header(slow), *query(1), *query(9, peak=2 * GIB), end(2, 2, wall=600.0)])
    sf0_1 = "tpcds.status.sf0_1"  # not a scale-factor-1 shard: no budget
    write(tmp_path, sf0_1, [header(sf0_1), *query(1, peak=2 * GIB), end(1, 1, wall=700.0)])
    parallel = "parallel.tpcds.status.sf0_1"
    write(tmp_path, parallel, [header(parallel, threads=4, same=1), *query(1, peak=2 * GIB), end(1, 1)])
    code, out, _, summary = report(tmp_path, monkeypatch, capsys, actions=True)
    assert code == 0
    warnings = [line for line in out.splitlines() if line.startswith("::")]
    assert len(warnings) == 1, warnings
    assert warnings[0].startswith(f"::warning title=SF 1 budget::{slow} (the queries derived from TPC-DS")
    assert f"test-build diagnostics, {tmp_path}, Linux X64)" in warnings[0]  # the build and the runner
    assert "69%25 of the 870 s inner timeout; above 1.5 GiB: Q9" in warnings[0]  # % is escaped
    assert warnings[0].endswith(tpc_costs.DISCLAIMERS["tpcds"])
    assert "69% of the 870 s inner timeout; above 1.5 GiB: Q9" in summary
    _, quiet, _, _ = report(tmp_path, monkeypatch, capsys, actions=False)
    assert "::warning" not in quiet


CANARY = "SELECT antb1_canary_text FROM t"
RUNNING = {"record": "running", "query": 1}


@pytest.mark.parametrize(
    "records",
    [
        pytest.param([header("tpcds.status.sf0_1"), *query(1, outcome=CANARY)], id="an unknown outcome"),
        pytest.param([header("tpcds.status.sf0_1"), *query(1, outcome="::error::" + CANARY)], id="a command"),
        pytest.param([header("tpcds.status.sf0_1"), {**RUNNING, "note": CANARY}], id="an extra key"),
        pytest.param([header("tpcds.status.sf0_1"), CANARY], id="not JSON"),
        pytest.param([RUNNING, header("tpcds.status.sf0_1")], id="before the run record"),
        pytest.param([header("tpcds.status.sf0_1"), query(1)[1]], id="an outcome without its running record"),
        pytest.param([header("tpcds.status.sf0_1"), {**query(1)[1], "query": None}], id="a null query"),
        pytest.param([header("tpcds.status.sf0_1"), {**RUNNING, "query": True}], id="a boolean query"),
        pytest.param([header("tpcds.status.sf0_1"), header("tpcds.status.sf0_1")], id="a second run record"),
        pytest.param([header("tpcds.status.sf1.1of8")], id="another test's record"),
        pytest.param([header("tpcds.status.sf0_1", workload="tpch")], id="another workload's record"),
        pytest.param([header("tpcds.status.sf0_1", workload=CANARY)], id="an unknown workload"),
        pytest.param([{**header("tpcds.status.sf0_1"), "shard": "1/1\n"}], id="a shard and a newline"),
        pytest.param([{**header("tpcds.status.sf0_1"), "threads": True}], id="a boolean"),
        pytest.param(['{"record": "running", "query": 1, "query": 2}'], id="a repeated key"),
        pytest.param([header("tpcds.status.sf0_1"), *query(1, seconds=float("nan"))], id="NaN seconds"),
        pytest.param([header("tpcds.status.sf0_1"), *query(1, peak=10**400)], id="a 401-digit peak"),
        pytest.param(
            [
                header("tpcds.status.sf0_1"),
                RUNNING,
                '{"record": "query", "query": 1, "outcome": '
                '"pass", "ratchet": "ok", "seconds": ' + "9" * 400 + ', "antb1_seconds": null, '
                '"peak_memory_bytes": null}',
            ],
            id="400-digit seconds",
        ),
        pytest.param(
            [header("tpcds.status.sf0_1"), '{"record": "running", "query": ' + "1" * 4301 + "}"],
            id="a number too long to read",
        ),
        pytest.param([header("tpcds.status.sf0_1"), b'{"record": "running", "query": 1}\xff'], id="not UTF-8"),
        pytest.param([header("tpcds.status.sf0_1"), *query(1), end(1, 1), *query(2)], id="after the end"),
        pytest.param([header("tpcds.status.sf0_1"), *query(1), end(2, 1)], id="a wrong count"),
    ],
)
def test_fails_closed_without_printing_what_it_refuses(tmp_path, monkeypatch, capsys, records) -> None:
    write(tmp_path, "tpch.status.sf0_01", [header("tpch.status.sf0_01", "tpch"), *query(1), end(1, 1)])
    write(tmp_path, "tpcds.status.sf0_1", records)
    code, out, err, summary = report(tmp_path, monkeypatch, capsys, actions=True)
    assert code == 1
    assert (out, summary) == ("", "")  # nothing is reported, not even the good file
    assert re.fullmatch(r"tpc_costs: tpcds\.status\.sf0_1\.jsonl:[0-9]+: [^\n]+: nothing is reported\n", err), err
    assert "antb1_canary" not in err and "SELECT" not in err


@pytest.mark.parametrize("name", [f"harness.{CANARY}", "tpcds.status.sf0_1\n"])
def test_a_file_named_after_no_ratchet_test_is_refused_without_its_name(tmp_path, monkeypatch, capsys, name) -> None:
    write(tmp_path, name, [header("tpcds.status.sf0_1")])
    code, out, err, _ = report(tmp_path, monkeypatch, capsys)
    assert (code, out) == (1, "")
    assert err == "tpc_costs: a costs file: a name other than a TPC ratchet test's: nothing is reported\n"


def test_a_report_that_looks_unredacted_is_withheld(tmp_path, monkeypatch, capsys) -> None:
    build = tmp_path / "select x"  # the build directory is printed: a pattern of unredacted output
    write(build, "tpch.status.sf0_1", [header("tpch.status.sf0_1", "tpch"), *query(1), end(1, 1)])
    code, out, err, summary = report(build, monkeypatch, capsys, actions=True)
    assert (code, out, summary) == (1, "", "")
    assert err == "tpc_costs: the report matches a pattern of unredacted output: it is withheld\n"


def test_the_withheld_patterns_are_run_redacted_cmakes() -> None:
    text = (ROOT / "tests/tpch/run_redacted.cmake").read_text(encoding="utf-8")
    match = re.search(r'^set\(\s*_unredacted((?:\s+"[^"]*")+)\s*\)$', text, re.MULTILINE)
    assert match, "set(_unredacted ...) in tests/tpch/run_redacted.cmake"
    cmake = [s.encode().decode("unicode_escape") for s in re.findall(r'"([^"]*)"', match.group(1))]
    assert [p.pattern for p in tpc_costs.FORBIDDEN] == [s.replace("\n", r"\n").replace("\t", r"\t") for s in cmake]


def test_the_disclaimers_are_adr_0006s() -> None:
    adr = " ".join((ROOT / "docs/adr/0006-test-strategy-and-data-policy.md").read_text(encoding="utf-8").split())
    for workload, disclaimer in tpc_costs.DISCLAIMERS.items():
        assert f'"{disclaimer}"' in adr, workload


def test_the_budget_constants_match_the_registrations() -> None:
    for name in ("tpch", "tpcds"):
        text = (ROOT / "tests" / name / "CMakeLists.txt").read_text(encoding="utf-8")
        assert 'math(EXPR _inner_timeout "${ARG_TIMEOUT} - 30")' in text, name
        assert "TIMEOUT 900" in text, name
        assert '"${CMAKE_BINARY_DIR}/tpc-costs/${_name}.jsonl"' in text, name
        assert "2GiB" in text, name
    assert tpc_costs.INNER_TIMEOUT_SECONDS == 900 - 30
    assert tpc_costs.BUDGET_PEAK_BYTES == 3 * GIB // 2


# The pixi tasks whose tests include the TPC ratchets, and the build tree each writes (CMakePresets.json; `check`
# runs `ci`). `check-full` runs four trees: a workflow runs its legs instead.
TASK_BUILD_DIRS = {
    "asan": "ci-asan",
    "asan-tpc-sf1": "ci-asan",
    "check": "ci",
    "ci": "ci",
    "ci-gcc": "ci-gcc",
    "ci-shuffle": "ci",
    "coverage": "coverage",
    "release": "ci-release",
    "static-test": "static",
    "test": "dev",
    "tsan": "tsan",
}
CI_LEGS = {"asan", "asan-tpc-sf1", "ci-gcc", "ci-shuffle", "coverage", "release", "static-test", "test", "tsan"}
COST_STEP = re.compile(r"python3 -I tools/ci/tpc_costs\.py (\S+)$")


def pixi_tasks(run: str) -> list[str]:
    """The tasks of the `pixi run` commands that start a line of a step's script, whatever their options."""
    tasks = []
    for line in run.splitlines():
        words = line.split()
        if words[:2] != ["pixi", "run"]:
            continue
        rest, i = words[2:], 0
        while i < len(rest) and rest[i].startswith("-"):
            i += 2 if rest[i] in ("-e", "--environment") else 1
        if i < len(rest):
            tasks.append(rest[i])
    return tasks


def test_every_leg_that_runs_the_ratchets_reports_their_costs() -> None:
    checked = set()
    for path in sorted((ROOT / ".github/workflows").glob("*.yml")):
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
        for job_id, job in doc.get("jobs", {}).items():
            where = f"{path.name}: {job_id}"
            matrix = job.get("strategy", {}).get("matrix", {}).get("include", [])
            tasks: dict[str, str] = {}  # task -> the build dir its costs step must read
            last_run = -1  # the index of the last step that runs one of them
            for index, step in enumerate(job.get("steps", [])):
                for task in pixi_tasks(step.get("run", "")):
                    assert task != "check-full", f"{where}: run the legs of check-full instead"
                    if task == '"$TASK"':  # the job's TASK: a matrix entry's, or one task
                        env_task = job.get("env", {}).get("TASK", "")
                        if env_task == "${{ matrix.task }}":
                            legs = {leg["task"]: "$PRESET" for leg in matrix if leg["task"] in TASK_BUILD_DIRS}
                        else:
                            legs = {env_task: TASK_BUILD_DIRS[env_task]} if env_task in TASK_BUILD_DIRS else {}
                        if legs:
                            tasks.update(legs)
                            last_run = index
                    elif task in TASK_BUILD_DIRS:
                        tasks[task] = TASK_BUILD_DIRS[task]
                        last_run = index
            if not tasks:
                continue
            steps = [(i, s) for i, s in enumerate(job["steps"]) if COST_STEP.search(s.get("run", ""))]
            assert len(steps) == 1, f"{where} runs {sorted(tasks)} but has {len(steps)} costs steps"
            index, step = steps[0]
            assert index > last_run, f"{where}: the costs step must come after the tests"
            assert step.get("if") == "${{ !cancelled() }}", where
            target = COST_STEP.search(step["run"]).group(1)
            for task, build_dir in tasks.items():
                if build_dir == "$PRESET":
                    assert target == '"build/$PRESET"', where
                    assert step["env"]["PRESET"] == "${{ matrix.preset }}", where
                    presets = {leg["task"]: leg["preset"] for leg in matrix}
                    assert presets[task] == TASK_BUILD_DIRS[task], f"{where}: {task}"
                else:
                    assert target == f"build/{build_dir}", f"{where}: {task}"
                checked.add(task)
    assert checked >= CI_LEGS, sorted(CI_LEGS - checked)  # found every leg, so the guard cannot pass on nothing
