#!/usr/bin/env python3
"""Coverage report and gate of the clang source-based coverage build (`pixi run coverage`, CI clang-coverage-fuzz).

Usage: python tools/ci/coverage.py BUILD_DIR [--thresholds FILE] [--source-root DIR]

1. Merges BUILD_DIR/prof/*.profraw (LLVM_PROFILE_FILE of the `coverage` test preset) into
   BUILD_DIR/coverage.profdata with llvm-profdata.
2. Runs llvm-cov over every executable in BUILD_DIR/bin (all test binaries, the test tools and the antb1 CLI), ignoring
   tests/ (src/<m>/tests/ too), fuzz/, bench/, tools/, .pixi/ and build/: `export -format=lcov` writes
   BUILD_DIR/coverage.lcov, `report` writes BUILD_DIR/report.txt (and prints it).
3. Sums line and branch coverage per module (src/<module>/) and writes the markdown table BUILD_DIR/summary.md (the CI
   step summary and the sticky PR comment).
4. With --thresholds (tools/ci/coverage_thresholds.json: {"<module>": {"lines": <floor %>, "branches": <floor %>}}),
   fails when a module is below a floor. A module without branches (or lines) is N/A for that metric.

Exit codes: 0 every floor holds, 1 a module is below a floor (or has no floor, or no data), 2 usage or tool error.
llvm-profdata and llvm-cov come from PATH (the pixi `default` env); LLVM_PROFDATA and LLVM_COV override them.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

EXIT_OK = 0
EXIT_BELOW_FLOOR = 1
EXIT_ERROR = 2

# Source-root-relative directories that never count: tests (at any depth, e.g. src/sql/tests/), the fuzz targets,
# benchmarks, tools (fixture generator, CI scripts), the pixi environments and build trees.
IGNORED_TOP_DIRS = ("fuzz", "bench", "tools", ".pixi", "build")
IGNORED_ANY_DIR = "tests"
METRICS = ("lines", "branches")
METRIC_NOUN = {"lines": "line", "branches": "branch"}
THRESHOLDS_HINT = "tools/ci/coverage_thresholds.json"


class CoverageError(Exception):
    """A usage or tool error (exit code 2)."""


@dataclass
class Counts:
    covered: int = 0
    count: int = 0

    def add(self, other: Counts) -> None:
        self.covered += other.covered
        self.count += other.count

    @property
    def percent(self) -> float | None:
        """Covered share in percent; None (N/A) when there is nothing to cover."""
        return 100.0 * self.covered / self.count if self.count else None


@dataclass
class ModuleCoverage:
    lines: Counts = field(default_factory=Counts)
    branches: Counts = field(default_factory=Counts)
    files: int = 0

    def metric(self, name: str) -> Counts:
        return self.lines if name == "lines" else self.branches


@dataclass
class Result:
    module: str
    metric: str
    measured: float | None  # None: N/A (nothing to cover)
    floor: float | None  # None: no floor configured
    ok: bool


def ere_escape(text: str) -> str:
    """Escapes `text` for a POSIX extended regex (llvm-cov -ignore-filename-regex uses llvm::Regex)."""
    return re.sub(r"([.\[\]{}()\\*+?^$|])", r"\\\1", text)


def ignore_regexes(source_root: Path) -> list[str]:
    root = ere_escape(source_root.as_posix().rstrip("/"))
    top = "|".join(ere_escape(d) for d in IGNORED_TOP_DIRS)
    return [f"^{root}/({top})/", f"^{root}/(.*/)?{IGNORED_ANY_DIR}/"]


def module_of(filename: str, source_root: Path) -> str | None:
    """The module (src/<module>/...) that a covered file belongs to; None for files that do not count."""
    try:
        rel = Path(os.path.normpath(filename)).relative_to(source_root)
    except ValueError:
        return None
    parts = rel.parts
    if not parts or parts[0] in IGNORED_TOP_DIRS or IGNORED_ANY_DIR in parts[:-1]:
        return None
    if len(parts) >= 3 and parts[0] == "src":
        return parts[1]
    return None


def summarize(export: dict[str, Any], source_root: Path, order: list[str] | None = None) -> dict[str, ModuleCoverage]:
    """Per-module line and branch counts from `llvm-cov export -format=text -summary-only` JSON, in `order` (the
    thresholds file lists the modules in architecture order), then the other modules by name."""
    modules: dict[str, ModuleCoverage] = {}
    for data in export.get("data", []):
        for entry in data.get("files", []):
            module = module_of(entry["filename"], source_root)
            if module is None:
                continue
            summary = entry["summary"]
            cov = modules.setdefault(module, ModuleCoverage())
            cov.files += 1
            for metric in METRICS:
                counts = summary.get(metric, {})
                cov.metric(metric).add(Counts(int(counts.get("covered", 0)), int(counts.get("count", 0))))
    rank = {module: i for i, module in enumerate(order or [])}
    return dict(sorted(modules.items(), key=lambda item: (rank.get(item[0], len(rank)), item[0])))


def load_thresholds(path: Path) -> dict[str, dict[str, float]]:
    """{module: {metric: floor}}; keys that start with "_" (like "_comment") are documentation."""
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        raise CoverageError(f"cannot read the thresholds {path}: {e}") from e
    if not isinstance(raw, dict):
        raise CoverageError(f"{path}: expected a JSON object of modules")
    floors: dict[str, dict[str, float]] = {}
    for module, value in raw.items():
        if module.startswith("_"):
            continue
        if not isinstance(value, dict) or set(value) - set(METRICS):
            raise CoverageError(f'{path}: module {module!r} must be {{"lines": <floor>, "branches": <floor>}}')
        floors[module] = {}
        for metric in METRICS:
            floor = value.get(metric)
            if not isinstance(floor, int | float) or isinstance(floor, bool) or not 0 <= floor <= 100:
                raise CoverageError(f"{path}: {module}.{metric} must be a percentage between 0 and 100")
            floors[module][metric] = float(floor)
    return floors


def evaluate(
    modules: dict[str, ModuleCoverage],
    floors: dict[str, dict[str, float]] | None,
    thresholds_name: str = THRESHOLDS_HINT,
) -> tuple[list[Result], list[str]]:
    """Results per module and metric, plus the failure messages (empty when the gate passes or is off)."""
    results: list[Result] = []
    failures: list[str] = []
    for module, cov in modules.items():
        module_floors = floors.get(module) if floors is not None else None
        if floors is not None and module_floors is None:
            failures.append(
                f"module '{module}' has no floor in {thresholds_name}: add "
                f'"{module}": {{"lines": ..., "branches": ...}} (the measured value minus 2, see its _comment)'
            )
        for metric in METRICS:
            measured = cov.metric(metric).percent
            floor = module_floors[metric] if module_floors else None
            ok = measured is None or floor is None or measured >= floor
            results.append(Result(module, metric, measured, floor, ok))
            if not ok:
                failures.append(
                    f"module '{module}': {METRIC_NOUN[metric]} coverage {measured:.2f}% is below its floor "
                    f"{floor:.1f}% ({thresholds_name}); add tests for the uncovered code (build/coverage/report.txt)"
                )
    if floors is not None:
        for module in floors:
            if module not in modules:
                failures.append(
                    f"module '{module}' has a floor in {thresholds_name} but no coverage data "
                    "(no src/<module>/ file in any binary of build/coverage/bin)"
                )
    return results, failures


def fmt_percent(value: float | None) -> str:
    return "N/A" if value is None else f"{value:.2f}%"


def fmt_floor(value: float | None) -> str:
    return "-" if value is None else f"{value:.1f}%"


def fmt_counts(counts: Counts) -> str:
    return f"{fmt_percent(counts.percent)} ({counts.covered}/{counts.count})" if counts.count else "N/A"


def render_markdown(modules: dict[str, ModuleCoverage], results: list[Result], failures: list[str], gated: bool) -> str:
    by_key = {(r.module, r.metric): r for r in results}
    total = ModuleCoverage()
    lines = [
        "## Coverage",
        "",
        "| Module | Lines | Floor | Branches | Floor | Status |",
        "| --- | ---: | ---: | ---: | ---: | --- |",
    ]
    for module, cov in modules.items():
        total.lines.add(cov.lines)
        total.branches.add(cov.branches)
        line_r, branch_r = by_key[(module, "lines")], by_key[(module, "branches")]
        status = "ok" if line_r.ok and branch_r.ok else "**below floor**"
        if gated and line_r.floor is None:
            status = "**no floor**"
        lines.append(
            f"| `{module}` | {fmt_counts(cov.lines)} | {fmt_floor(line_r.floor)} "
            f"| {fmt_counts(cov.branches)} | {fmt_floor(branch_r.floor)} | {status} |"
        )
    lines.append(f"| **total** | {fmt_counts(total.lines)} | | {fmt_counts(total.branches)} | | |")
    lines.append("")
    if failures:
        lines.append("**Coverage gate: FAIL**")
        lines.append("")
        lines.extend(f"- {failure}" for failure in failures)
    elif gated:
        lines.append(f"Coverage gate: PASS (floors: `{THRESHOLDS_HINT}`; they only go up).")
    else:
        lines.append("Coverage gate: off (no `--thresholds`).")
    lines.append("")
    lines.append(
        "Clang source-based coverage of the hermetic tests over `src/<module>/` (module unit tests excluded); "
        "N/A: nothing to cover. Reproduce with `pixi run coverage` (details: `build/coverage/report.txt`)."
    )
    return "\n".join(lines) + "\n"


def find_tool(name: str, env: str) -> str:
    tool = os.environ.get(env) or shutil.which(name)
    if not tool:
        raise CoverageError(f"{name} not found (run through `pixi run coverage`, or set {env})")
    return tool


def run(cmd: list[str], stdout_path: Path | None = None) -> str:
    try:
        proc = subprocess.run(cmd, check=False, capture_output=True, text=True)
    except OSError as e:
        raise CoverageError(f"cannot run {cmd[0]}: {e}") from e
    if proc.stderr:
        sys.stderr.write(proc.stderr)
    if proc.returncode != 0:
        raise CoverageError(f"{Path(cmd[0]).name} {cmd[1]} failed with exit code {proc.returncode}")
    if stdout_path is not None:
        stdout_path.write_text(proc.stdout, encoding="utf-8")
    return proc.stdout


def find_objects(bin_dir: Path) -> list[Path]:
    """Every executable in bin/, with the antb1 CLI first (llvm-cov takes one positional binary + -object)."""
    objects = sorted(p for p in bin_dir.iterdir() if p.is_file() and os.access(p, os.X_OK))
    names = [p.name for p in objects]
    if "antb1" not in names:
        raise CoverageError(f"no antb1 binary in {bin_dir}: run `cmake --workflow --preset coverage` first")
    objects.insert(0, objects.pop(names.index("antb1")))
    return objects


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("build_dir", type=Path, help="coverage build tree, e.g. build/coverage")
    parser.add_argument("--thresholds", type=Path, help=f"per-module floors ({THRESHOLDS_HINT}); omit: no gate")
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2], help="repository root")
    args = parser.parse_args(argv)

    try:
        build_dir = args.build_dir.resolve()
        source_root = args.source_root.resolve()
        floors = load_thresholds(args.thresholds) if args.thresholds else None
        profraws = sorted((build_dir / "prof").glob("*.profraw"))
        if not profraws:
            raise CoverageError(
                f"no .profraw files in {build_dir / 'prof'}: run the tests of the `coverage` preset first "
                "(`pixi run coverage` does both)"
            )
        objects = find_objects(build_dir / "bin")
        profdata_tool = find_tool("llvm-profdata", "LLVM_PROFDATA")
        cov_tool = find_tool("llvm-cov", "LLVM_COV")

        profdata = build_dir / "coverage.profdata"
        inputs = build_dir / "profraw-files.txt"
        inputs.write_text("".join(f"{p}\n" for p in profraws), encoding="utf-8")
        print(f"coverage: merging {len(profraws)} raw profiles of {len(objects)} binaries", flush=True)
        run([profdata_tool, "merge", "-sparse", f"--input-files={inputs}", "-o", str(profdata)])

        common = [f"-instr-profile={profdata}", str(objects[0])]
        for obj in objects[1:]:
            common += ["-object", str(obj)]
        common += [f"-ignore-filename-regex={regex}" for regex in ignore_regexes(source_root)]
        run([cov_tool, "export", "-format=lcov", *common], build_dir / "coverage.lcov")
        report = run([cov_tool, "report", *common], build_dir / "report.txt")
        export = json.loads(run([cov_tool, "export", "-format=text", "-summary-only", *common]))
    except CoverageError as e:
        print(f"coverage: ERROR: {e}", file=sys.stderr)
        return EXIT_ERROR

    print(report)
    modules = summarize(export, source_root, list(floors) if floors else None)
    results, failures = evaluate(modules, floors, str(args.thresholds) if args.thresholds else THRESHOLDS_HINT)
    summary = render_markdown(modules, results, failures, gated=floors is not None)
    (build_dir / "summary.md").write_text(summary, encoding="utf-8")
    print(summary)
    print(f"coverage: wrote {build_dir / 'summary.md'}, {build_dir / 'coverage.lcov'}, {build_dir / 'report.txt'}")
    if failures:
        for failure in failures:
            print(f"coverage: FAIL {failure}", file=sys.stderr)
        return EXIT_BELOW_FLOOR
    print("coverage: PASS" if floors is not None else "coverage: done (no --thresholds, no gate)")
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
