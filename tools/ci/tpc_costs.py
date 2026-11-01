#!/usr/bin/env python3
"""Report the costs of the queries derived from TPC-H and TPC-DS that a CI leg ran (ADR 0037's HC3).

The ratchets of those queries (`antb1-slt tpch` and `tpcds --costs`, tests/tpch and tests/tpcds) write each query's
seconds and antb1's peak memory to build/<preset>/tpc-costs/<test>.jsonl (RatchetCosts in tests/slt/runner/ratchet.h).
This script reads only those files. It prints a table per test to the step log and, when GITHUB_STEP_SUMMARY is set,
a Markdown summary. With GITHUB_ACTIONS=true it also warns once for each scale-factor-1 shard of the queries derived
from TPC-DS that misses ADR 0037's Decision 6: more than 60% of its 870-second inner timeout, or a passing query that
peaks above 1.5 GiB at one thread, or a shard that did not finish (a run without its end record: a timeout or a
crash). It never fails on a budget, since timings on shared runners vary several-fold.

It fails closed on anything else: a file name or a record outside the schema is reported by file and line only,
without its content, and the exit code is 1, so that even a broken redaction cannot publish text through it. Every line
it prints is also checked against the patterns tests/tpch/run_redacted.cmake withholds.

Usage: python3 -I tools/ci/tpc_costs.py build/<preset>
Exit codes: 0, also when there are no files (the macOS leg registers no TPC tests); 1 input outside the schema; 2 usage.
Standard library only: it runs on a bare GitHub runner.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# The ctest TIMEOUT of every TPC ratchet test is 900 seconds; run_redacted.cmake stops the command 30 seconds earlier.
INNER_TIMEOUT_SECONDS = 870
BUDGET_SHARE = 0.60  # ADR 0037, Decision 6: of the inner timeout
BUDGET_PEAK_BYTES = 3 * 2**29  # 1.5 GiB
MIB = 2**20

DISCLAIMERS = {
    "tpch": (
        "This workload is derived from the TPC-H Benchmark and is not comparable to published TPC-H Benchmark "
        "results, as this implementation does not comply with all requirements of the TPC-H Benchmark."
    ),
    "tpcds": (
        "This workload is derived from the TPC-DS Benchmark and is not comparable to published TPC-DS Benchmark "
        "results, as this implementation does not comply with all requirements of the TPC-DS Benchmark."
    ),
}
DERIVED = {"tpch": "TPC-H", "tpcds": "TPC-DS"}

TEST_NAME = re.compile(r"^(parallel\.)?tpc(h|ds)\.status\.[0-9a-z_.]+$")
SF1_SHARD = re.compile(r"^tpcds\.status\.sf1\.")
SHARD = re.compile(r"^[0-9]{1,2}/[0-9]{1,2}$")
OUTCOMES = {
    "pass",
    "unsupported",
    "wrong answer",
    "DuckDB error",
    "parse error",
    "bind error",
    "io error",
    "execution error",
    "memory error",
    "internal error",
}
VERDICTS = {"ok", "unexpected pass", "unexpected fail", "fail"}
KEYS = {
    "run": {"record", "workload", "test", "shard", "threads", "same_as_threads", "memory_limit_bytes"},
    "running": {"record", "query"},
    "query": {"record", "query", "outcome", "ratchet", "seconds", "antb1_seconds", "peak_memory_bytes"},
    "end": {"record", "queries", "passed", "failed", "wall_seconds"},
}
# What tests/tpch/run_redacted.cmake withholds: an unredacted antb1-slt report.
FORBIDDEN = [
    re.compile(r"\n  SQL:\n"),
    re.compile(r"\n    row [0-9]+: "),
    re.compile(r"[sS][eE][lL][eE][cC][tT][ \t\n]"),
]


class SchemaError(Exception):
    """A file or record outside the schema; the message never holds the content."""


@dataclass
class Query:
    number: int
    outcome: str
    ratchet: str
    seconds: float
    antb1_seconds: float | None
    peak_bytes: int | None


@dataclass
class Run:
    test: str
    workload: str
    shard: str
    threads: int
    same_as_threads: int | None
    memory_limit: int | None
    queries: list[Query] = field(default_factory=list)
    running: int | None = None  # the query that started last without its outcome
    wall_seconds: float | None = None  # None: the run did not end (a timeout or a crash)
    end_counts: tuple[int, int] | None = None  # passed and failed, from the end record

    @property
    def complete(self) -> bool:
        return self.wall_seconds is not None

    @property
    def one_thread(self) -> bool:
        return self.threads == 1 and self.same_as_threads is None

    @property
    def seconds(self) -> float:
        """The run's wall time, or for a run that did not end the sum of its queries': a lower bound."""
        return self.wall_seconds if self.wall_seconds is not None else sum(q.seconds for q in self.queries)

    @property
    def passed(self) -> int:
        return self.end_counts[0] if self.end_counts else sum(q.outcome == "pass" for q in self.queries)

    @property
    def failed(self) -> int:
        return self.end_counts[1] if self.end_counts else sum(q.ratchet != "ok" for q in self.queries)

    @property
    def peak(self) -> int | None:
        peaks = [q.peak_bytes for q in self.queries if q.peak_bytes is not None]
        return max(peaks) if peaks else None


INT64_MAX = 2**63 - 1


def is_int(value: object, low: int = 0) -> bool:
    return type(value) is int and low <= value <= INT64_MAX


def is_seconds(value: object) -> bool:
    if type(value) is int:
        return 0 <= value <= INT64_MAX
    return type(value) is float and math.isfinite(value) and value >= 0


def no_duplicate_keys(pairs: list[tuple[str, object]]) -> dict:
    record = dict(pairs)
    if len(record) != len(pairs):
        raise ValueError("a repeated key")
    return record


def check(condition: bool, where: str, rule: str) -> None:
    if not condition:
        raise SchemaError(f"{where}: {rule}")


def read_run(path: Path) -> Run:
    """The run of one costs file; SchemaError names the file and line of the first violation only."""
    test = path.name.removesuffix(".jsonl")
    # Not named in the error: only a name that passes is known to be one of ours.
    check(TEST_NAME.fullmatch(test) is not None, "a costs file", "a name other than a TPC ratchet test's")
    workload = test.removeprefix("parallel.").split(".status.", 1)[0]
    run: Run | None = None
    with path.open("rb") as lines:
        for number, line in enumerate(lines, start=1):
            where = f"{path.name}:{number}"
            check(run is None or not run.complete, where, "a record after the end record")
            try:  # ValueError: not UTF-8, not JSON, a repeated key, an integer too long to read
                record = json.loads(line.decode("utf-8"), object_pairs_hook=no_duplicate_keys)
            except (ValueError, RecursionError):
                raise SchemaError(f"{where}: not one JSON object") from None
            check(isinstance(record, dict), where, "not one JSON object")
            kind = record.get("record")
            check(isinstance(kind, str) and kind in KEYS, where, "an unknown record")
            check(set(record) == KEYS[kind], where, f"not the keys of a {kind} record")
            if kind == "run":
                check(run is None, where, "a second run record")
                check(record["workload"] == workload, where, "a workload other than the test's")
                check(record["test"] == test, where, "a test other than the file's")
                check(
                    isinstance(record["shard"], str) and SHARD.fullmatch(record["shard"]) is not None, where, "a shard"
                )
                check(is_int(record["threads"], 1), where, "threads")
                check(record["same_as_threads"] is None or is_int(record["same_as_threads"], 1), where, "threads")
                check(record["memory_limit_bytes"] is None or is_int(record["memory_limit_bytes"], 1), where, "limit")
                run = Run(
                    test=test,
                    workload=record["workload"],
                    shard=record["shard"],
                    threads=record["threads"],
                    same_as_threads=record["same_as_threads"],
                    memory_limit=record["memory_limit_bytes"],
                )
                continue
            check(run is not None, where, "a record before the run record")
            assert run is not None
            if kind == "running":
                check(is_int(record["query"], 1) and record["query"] <= 99, where, "a query number")
                check(run.running is None, where, "a query started before the last one ended")
                run.running = record["query"]
            elif kind == "query":
                check(
                    is_int(record["query"], 1) and record["query"] == run.running,
                    where,
                    "an outcome without its running record",
                )
                check(record["outcome"] in OUTCOMES, where, "an unknown outcome")
                check(record["ratchet"] in VERDICTS, where, "an unknown verdict")
                check(is_seconds(record["seconds"]), where, "seconds")
                check(record["antb1_seconds"] is None or is_seconds(record["antb1_seconds"]), where, "seconds")
                check(record["peak_memory_bytes"] is None or is_int(record["peak_memory_bytes"]), where, "a peak")
                run.queries.append(
                    Query(
                        number=record["query"],
                        outcome=record["outcome"],
                        ratchet=record["ratchet"],
                        seconds=float(record["seconds"]),
                        antb1_seconds=record["antb1_seconds"],
                        peak_bytes=record["peak_memory_bytes"],
                    )
                )
                run.running = None
            else:
                check(run.running is None, where, "an end record while a query runs")
                check(all(is_int(record[k]) for k in ("queries", "passed", "failed")), where, "counts")
                check(record["queries"] == len(run.queries), where, "a query count other than the records'")
                check(is_seconds(record["wall_seconds"]), where, "seconds")
                run.end_counts = (record["passed"], record["failed"])
                run.wall_seconds = float(record["wall_seconds"])
    check(run is not None, path.name, "no run record")
    assert run is not None
    return run


def mib(value: int | None) -> str:
    return "-" if value is None else f"{value / MIB:.1f}"


def seconds_text(value: float | None) -> str:
    return "-" if value is None else f"{value:.3f}"


def problems(run: Run) -> list[str]:
    """What misses Decision 6, for the scale-factor-1 shards of TPC-DS, or what keeps the run from its end."""
    notes = []
    if not run.complete:
        where = f"Q{run.running} was running when it stopped" if run.running else "it stopped between queries"
        notes.append(f"did not finish: {where} (a timeout or a crash: see its failing test)")
    if SF1_SHARD.match(run.test):
        share = run.seconds / INNER_TIMEOUT_SECONDS
        if run.complete and share > BUDGET_SHARE:
            notes.append(f"{share:.0%} of the {INNER_TIMEOUT_SECONDS} s inner timeout")
        heavy = [q.number for q in run.queries if q.outcome == "pass" and (q.peak_bytes or 0) > BUDGET_PEAK_BYTES]
        if heavy and run.one_thread:
            notes.append("above 1.5 GiB: " + ", ".join(f"Q{n}" for n in heavy))
    return notes


def sort_key(run: Run) -> tuple:
    """TPC-H before TPC-DS, then the 1-thread runs by scale factor and shard, then the parallel ones."""
    shard = [int(p) for p in re.findall(r"[0-9]+", run.test.split("status.", 1)[1])]
    return (run.workload != "tpch", not run.one_thread, shard)


def run_seconds(run: Run) -> str:
    """The run's seconds, as a lower bound when it did not end."""
    return f"{run.seconds:.1f}" if run.complete else f">= {run.seconds:.1f}"


def run_share(run: Run) -> str:
    """The run's share of the inner timeout, or "-" when it did not end."""
    return f"{run.seconds / INNER_TIMEOUT_SECONDS:.0%}" if run.complete else "-"


def log_lines(runs: list[Run], build_dir: str, runner: str) -> list[str]:
    lines = [f"costs of the queries derived from TPC-H and TPC-DS in {build_dir} (test-build diagnostics, {runner}):"]
    for run in runs:
        threads = "" if run.one_thread else f", {run.threads} threads"
        lines.append(
            f"  {run.test}: {len(run.queries)} queries, {run.passed} passed, {run.failed} failed, "
            f"{run_seconds(run)} s ({run_share(run)} of {INNER_TIMEOUT_SECONDS} s), peak {mib(run.peak)} MiB{threads}"
        )
        lines.extend(f"    note: {note}" for note in problems(run))
        for q in run.queries:
            lines.append(
                f"    Q{q.number}: {q.outcome}, {q.ratchet}, {q.seconds:.3f} s, "
                f"antb1 {seconds_text(q.antb1_seconds)} s, peak {mib(q.peak_bytes)} MiB"
            )
    for workload in sorted({run.workload for run in runs}, reverse=True):
        lines.append(f"  {DISCLAIMERS[workload]}")
    return lines


def summary_markdown(runs: list[Run], build_dir: str, runner: str) -> str:
    rows = [
        f"### Costs of the queries derived from TPC-H and TPC-DS ({build_dir})",
        "",
        f"Test-build diagnostics for ADR 0037's scale factor 1 budget, not performance numbers: {runner}, {build_dir}.",
        "",
        f"| Test | Queries | Passed | Failed | Seconds | Of {INNER_TIMEOUT_SECONDS} s | Peak (MiB) | Notes |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |",
    ]
    for run in runs:
        notes = problems(run) + ([] if run.one_thread else [f"{run.threads} threads: peaks vary"])
        rows.append(
            f"| {run.test} | {len(run.queries)} | {run.passed} | {run.failed} | {run_seconds(run)} | "
            f"{run_share(run)} | {mib(run.peak)} | {'; '.join(notes)} |"
        )
    shards = [run for run in runs if SF1_SHARD.match(run.test)]
    passing = sorted(
        ((q, run) for run in shards for q in run.queries if q.outcome == "pass"), key=lambda p: p[0].number
    )
    if passing:
        rows += [
            "",
            "Passing queries derived from TPC-DS at scale factor 1 (ADR 0037, Decision 5):",
            "",
            "| Query | Test | Seconds | antb1 seconds | Peak (MiB) |",
            "| ---: | --- | ---: | ---: | ---: |",
        ]
        rows += [
            f"| Q{q.number} | {run.test} | {q.seconds:.3f} | {seconds_text(q.antb1_seconds)} | {mib(q.peak_bytes)} |"
            for q, run in passing
        ]
    for run in runs:
        rows += [
            "",
            f"<details><summary>{run.test}: every query</summary>",
            "",
            "| Query | Outcome | Ratchet | Seconds | antb1 seconds | Peak (MiB) |",
            "| ---: | --- | --- | ---: | ---: | ---: |",
        ]
        rows += [
            f"| Q{q.number} | {q.outcome} | {q.ratchet} | {q.seconds:.3f} | {seconds_text(q.antb1_seconds)} | "
            f"{mib(q.peak_bytes)} |"
            for q in run.queries
        ]
        rows += ["", "</details>"]
    rows.append("")
    rows += [DISCLAIMERS[w] for w in sorted({run.workload for run in runs}, reverse=True)]
    return "\n".join(rows) + "\n"


def annotations(runs: list[Run], build_dir: str, runner: str) -> list[str]:
    """At most one warning per test, for the scale-factor-1 shards that miss Decision 6 or did not finish."""
    out = []
    for run in runs:
        notes = problems(run) if SF1_SHARD.match(run.test) else []
        if notes:
            text = (
                f"{run.test} (the queries derived from TPC-DS, scale factor 1; test-build diagnostics, {build_dir}, "
                f"{runner}) misses ADR 0037's budget: {'; '.join(notes)}. {DISCLAIMERS[run.workload]}"
            )
            text = text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
            out.append(f"::warning title=SF 1 budget::{text}")
    return out


def withheld(text: str) -> bool:
    return any(p.search(text) for p in FORBIDDEN)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("build_dir", help="the build tree, e.g. build/ci-gcc")
    args = parser.parse_args(argv)
    costs_dir = Path(args.build_dir) / "tpc-costs"
    paths = sorted(costs_dir.glob("*.jsonl")) if costs_dir.is_dir() else []
    if not paths:
        print(f"tpc_costs: no costs of the queries derived from TPC-H or TPC-DS in {args.build_dir}", flush=True)
        return 0
    try:
        runs = sorted((read_run(path) for path in paths), key=sort_key)
    except SchemaError as e:
        print(f"tpc_costs: {e}: nothing is reported", file=sys.stderr, flush=True)
        return 1
    runner = " ".join(filter(None, [os.environ.get("RUNNER_OS"), os.environ.get("RUNNER_ARCH")])) or "a local run"
    log = log_lines(runs, args.build_dir, runner)
    markdown = summary_markdown(runs, args.build_dir, runner)
    warnings = annotations(runs, args.build_dir, runner) if os.environ.get("GITHUB_ACTIONS") == "true" else []
    if any(withheld(line + "\n") for line in [*log, *warnings]) or withheld(markdown):
        print("tpc_costs: the report matches a pattern of unredacted output: it is withheld", file=sys.stderr)
        return 1
    for line in log:
        print(f"tpc_costs: {line}")  # never a line that starts with `::` (a workflow command)
    for line in warnings:
        print(line)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            f.write(markdown)
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
