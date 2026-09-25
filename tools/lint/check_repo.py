#!/usr/bin/env python3
"""antb1 repository drift and policy checks R001-R015 (run by `pixi run lint` through pre-commit).

Every finding is printed as ``path:line: Rxxx message (fix: ...)`` and makes the exit status 1. A check that needs a
file which does not exist yet (skills, CLAUDE.md, .claude/settings.json, the ClickBench ratchet, ...) is skipped.

Usage: python tools/lint/check_repo.py [--root DIR] [--only R001,R007]

  R001  `pixi run <task>` references name existing tasks (in the given `-e` env); every task lives in exactly one
        environment; no tasks in the default feature; `depends-on` targets exist.
  R002  every non-hidden task is documented in the AGENTS.md command table or docs/ci.md.
  R003  CLAUDE.md line 1 is `@AGENTS.md`; AGENTS.md size and `## Code Review Rules`; copilot-instructions size.
  R004  .claude/skills is a byte-identical copy of .agents/skills; SKILL.md frontmatter.
  R005  one pixi version everywhere (requires-pixi, workflows, scripts, docs); pixi.lock starts with `version: 7`.
  R006  `--preset X` and `build/<X>` references (workflow matrix values too) name CMake presets of the right kind.
  R007  docs/architecture.md module table == cmake/Antb1Modules.cmake; `#include` scan of src/ (a module may include
        itself, its allowed deps and what they re-export through PUBLIC_DEPS; tests: every module it can reach).
  R008  ctest labels comment in cmake/Antb1Testing.cmake == docs/testing.md labels table.
  R009  no data files (Parquet, ClickBench queries, ...) and no file over 1 MiB except pixi.lock.
  R010  workflow hardening (permissions, timeouts, SHA pins, persist-credentials, triggers, setup-pixi cache key,
        no `${{ github.event.* }}`/`${{ github.head_ref }}` in `run:`); `data` tests use `--redact`.
  R011  ClickBench ratchet `pass` list == docs/sql-subset.md status table.
  R012  CODEOWNERS, AGENTS.md "Ask a human first" and .claude/settings.json cover the governance path set G.
  R013  docs/adr/README.md lists every ADR.
  R014  relative links and backticked repo paths in agent docs and docs/** exist.
  R015  every action used (and the known nested ones) is allowed by tools/github/allowed-actions.json.
"""

from __future__ import annotations

import argparse
import fnmatch
import itertools
import json
import os
import re
import subprocess
import sys
import tomllib
from collections.abc import Callable, Iterable, Iterator
from dataclasses import dataclass, field
from functools import cached_property
from pathlib import Path
from typing import Any

try:
    import yaml
except ImportError:  # pragma: no cover - the lint env always has PyYAML
    sys.exit("check_repo.py needs PyYAML: run it through `pixi run lint`")

# ---------------------------------------------------------------------------------------------------------------------
# Policy constants
# ---------------------------------------------------------------------------------------------------------------------

# Governance path set G (R012). CODEOWNERS, the AGENTS.md "Ask a human first" section and the .claude/settings.json
# ask/deny Edit rules must each cover every entry. A trailing "/" marks a directory.
GOVERNANCE_PATHS: tuple[str, ...] = (
    "/.github/",
    "/.githooks/",
    "/.claude/",
    "/.agents/",
    "/AGENTS.md",
    "/CLAUDE.md",
    "/pixi.toml",
    "/CMakePresets.json",
    "/cmake/",
    "/tools/github/",
    "/tools/lint/",
    "/tools/data/",
    "/tools/ci/coverage_thresholds.json",
    "/.pre-commit-config.yaml",
    "/.clang-format",
    "/.clang-tidy",
    "/tests/.clang-tidy",
    "/scripts/ctest.sh",
    "/scripts/agent-setup.sh",
    "/tests/data/clickbench_status.json",
    "/scripts/lint.sh",
    "/scripts/fmt.sh",
    "/tools/sanitizers/",
    "/.markdownlint-cli2.yaml",
    "/_typos.toml",
    "/ruff.toml",
    "/.gersemirc",
    "/tombi.toml",
)
# Owned in CODEOWNERS in addition to G.
CODEOWNERS_EXTRA_PATHS: tuple[str, ...] = ("/pixi.lock", "/docs/adr/")
# Must be an Edit *deny* rule in .claude/settings.json.
SETTINGS_DENY_PATHS: tuple[str, ...] = ("/pixi.lock",)

# Actions that other actions run internally (R015): allowed-actions.json must permit them as well.
NESTED_ACTIONS: dict[str, tuple[str, ...]] = {
    "anthropics/claude-code-action": ("oven-sh/setup-bun",),
    "openai/codex-action": ("actions/setup-node",),
    "zizmorcore/zizmor-action": ("github/codeql-action/upload-sarif",),
}
GITHUB_OWNED_OWNERS = frozenset({"actions", "github"})
REQUIRED_MERGE_GROUP_WORKFLOWS = ("ci.yml", "pr-title.yml")  # they produce required checks
SETUP_PIXI_CACHE_KEY = "pixi-${{ hashFiles('pixi.lock') }}-"

LOCK_FIRST_LINE = "version: 7"
MAX_FILE_BYTES = 1024 * 1024
LARGE_FILES_ALLOWED = frozenset({"pixi.lock"})
DATA_FILE_GLOBS = ("*.parquet", "*.arrow", "*.feather", "*.csv.gz", "queries.sql")
DATA_FILE_RE = re.compile(r"q\d\d\.csv")
AGENTS_MAX_LINES = 150
AGENTS_MAX_BYTES = 32 * 1024
COPILOT_MAX_LINES = 40

# Commands that docs may run inside an explicit environment although they are not tasks (`pixi run -e lint pytest`).
ENV_COMMANDS = frozenset(
    {
        "actionlint",
        "bash",
        "ccache",
        "clang",
        "clang-format",
        "clang-tidy",
        "cmake",
        "ctest",
        "duckdb",
        "gcc",
        "gersemi",
        "llvm-cov",
        "llvm-profdata",
        "markdownlint-cli2",
        "ninja",
        "pre-commit",
        "pytest",
        "python",
        "ruff",
        "run-clang-tidy",
        "shellcheck",
        "tombi",
        "typos",
        "zizmor",
    }
)

# External CMake packages (the part of the target name before "::") -> include prefixes they provide (R007).
EXTERNAL_INCLUDE_PREFIXES: dict[str, tuple[str, ...]] = {
    "Arrow": ("arrow/",),
    "ArrowCompute": ("arrow/",),
    "Parquet": ("parquet/",),
    "CLI11": ("CLI/", "CLI11.hpp"),
}
KNOWN_EXTERNAL_PREFIXES = ("arrow/", "parquet/", "CLI/", "CLI11.hpp")
NEVER_IN_SRC_RE = re.compile(r"^duckdb(\.h|\.hpp|/)")  # DuckDB is a test oracle only

# Markdown files whose commands (R001), links and paths (R014) are checked; docs/** and skills are added on top.
AGENT_DOCS = (
    "AGENTS.md",
    "CLAUDE.md",
    "README.md",
    "CONTRIBUTING.md",
    "SECURITY.md",
    ".github/copilot-instructions.md",
    ".github/pull_request_template.md",
)
WALK_SKIP_DIRS = frozenset({".git", ".pixi", "build", ".cache", "__pycache__", ".pytest_cache", ".ruff_cache"})

# ---------------------------------------------------------------------------------------------------------------------
# Findings and repository access
# ---------------------------------------------------------------------------------------------------------------------


@dataclass(frozen=True, order=True)
class Finding:
    path: str
    line: int
    rule: str
    message: str
    fix: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.rule} {self.message} (fix: {self.fix})"


class Repo:
    """Files of the work tree (tracked plus untracked-but-not-ignored) and the findings collected so far."""

    def __init__(self, root: Path, use_git: bool = True) -> None:
        self.root = root
        self.files: list[str] = self._list_files(use_git)
        self.file_set = frozenset(self.files)
        self.top_level = frozenset(f.split("/", 1)[0] for f in self.files)
        self.findings: list[Finding] = []
        self._text: dict[str, str | None] = {}

    def _list_files(self, use_git: bool) -> list[str]:
        names: set[str] = set()
        listed = False
        if use_git:
            try:
                out = subprocess.run(
                    ["git", "-C", str(self.root), "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                    capture_output=True,
                    check=True,
                ).stdout
                names = {n for n in out.decode("utf-8", "replace").split("\0") if n}
                listed = True
            except (OSError, subprocess.CalledProcessError):
                listed = False
        if not listed:
            for dirpath, dirnames, filenames in os.walk(self.root):
                dirnames[:] = sorted(d for d in dirnames if d not in WALK_SKIP_DIRS)
                rel = Path(dirpath).relative_to(self.root)
                names.update((rel / f).as_posix() for f in filenames)
        return sorted(n for n in names if (self.root / n).is_file())

    def exists(self, rel: str) -> bool:
        return (self.root / rel).exists()

    def text(self, rel: str) -> str | None:
        if rel not in self._text:
            try:
                self._text[rel] = (self.root / rel).read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                self._text[rel] = None
        return self._text[rel]

    def under(self, prefix: str, suffixes: tuple[str, ...] = ()) -> list[str]:
        return [f for f in self.files if f.startswith(prefix) and (not suffixes or f.endswith(suffixes))]

    def add(self, rule: str, path: str, line: int, message: str, fix: str) -> None:
        self.findings.append(Finding(path, max(line, 1), rule, message, fix))


def line_of(text: str, pattern: str, flags: int = re.MULTILINE) -> int:
    """1-based line of the first regex match in text (1 when absent)."""
    m = re.search(pattern, text, flags)
    return text.count("\n", 0, m.start()) + 1 if m else 1


# ---------------------------------------------------------------------------------------------------------------------
# Markdown helpers
# ---------------------------------------------------------------------------------------------------------------------

FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})")
CODE_SPAN_RE = re.compile(r"(`+)(.+?)(?<!`)\1(?!`)")
TABLE_DELIM_RE = re.compile(r"^\s*\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)*\|?\s*$")
LINK_RE = re.compile(r"!?\[[^\]]*\]\(\s*(<[^>]*>|[^)\s]+)(?:\s+\"[^\"]*\")?\s*\)")
REF_DEF_RE = re.compile(r"^\s{0,3}\[[^\]]+\]:\s*(<[^>]*>|\S+)")


def md_lines(text: str) -> Iterator[tuple[int, str, bool]]:
    """(line number, line, inside a fenced code block); fence lines themselves count as fenced."""
    fence: str | None = None
    for no, line in enumerate(text.splitlines(), 1):
        m = FENCE_RE.match(line)
        if fence is None:
            if m:
                fence = m.group(1)
                yield no, line, True
                continue
            yield no, line, False
        else:
            stripped = line.strip()
            if m and stripped[0] == fence[0] and len(stripped) >= len(fence) and set(stripped) == {fence[0]}:
                fence = None
            yield no, line, True


def code_spans(line: str) -> list[str]:
    return [m.group(2).strip() for m in CODE_SPAN_RE.finditer(line)]


def md_code_contexts(text: str) -> Iterator[tuple[int, str]]:
    """Inline code spans and fenced code lines: where Markdown docs put commands."""
    for no, line, fenced in md_lines(text):
        if fenced:
            yield no, line
        else:
            for span in code_spans(line):
                yield no, span


@dataclass
class MdTable:
    line: int
    header: list[str]
    rows: list[tuple[int, list[str]]] = field(default_factory=list)

    def column(self, pattern: str) -> int | None:
        for i, cell in enumerate(self.header):
            if re.search(pattern, clean_cell(cell), re.IGNORECASE):
                return i
        return None


def split_row(line: str) -> list[str]:
    s = line.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|") and not s.endswith("\\|"):
        s = s[:-1]
    return [c.strip() for c in re.split(r"(?<!\\)\|", s)]


def clean_cell(cell: str) -> str:
    return re.sub(r"[`*]", "", cell).strip()


def md_tables(text: str) -> list[MdTable]:
    tables: list[MdTable] = []
    current: MdTable | None = None
    previous: tuple[int, str] | None = None
    for no, line, fenced in md_lines(text):
        if fenced:
            current, previous = None, None
            continue
        if current is not None:
            if line.strip().startswith("|"):
                current.rows.append((no, split_row(line)))
                continue
            current = None
        if previous is not None and "|" in previous[1] and TABLE_DELIM_RE.match(line):
            current = MdTable(previous[0], split_row(previous[1]))
            tables.append(current)
            previous = None
            continue
        previous = (no, line) if line.strip() else None
    return tables


def md_section(text: str, heading: str) -> tuple[int, str] | None:
    """(line of the heading, body) of the `## heading` section, up to the next heading of the same or higher level."""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = re.match(r"^(#{1,6})\s+(.*?)\s*#*\s*$", line)
        if m and m.group(2).strip().lower() == heading.lower():
            level = len(m.group(1))
            body: list[str] = []
            for nxt in lines[i + 1 :]:
                n = re.match(r"^(#{1,6})\s", nxt)
                if n and len(n.group(1)) <= level:
                    break
                body.append(nxt)
            return i + 1, "\n".join(body)
    return None


def words_of(cell: str) -> list[str]:
    """Names listed in a table cell: backticked tokens when present, otherwise comma/space separated words."""
    cell = re.sub(r"\([^)]*\)", " ", cell)
    spans = CODE_SPAN_RE.findall(cell)
    tokens = [s[1].strip() for s in spans] if spans else re.split(r"[\s,;/·]+", cell)
    out = []
    for token in tokens:
        token = token.strip("*_ .")
        if token and token.lower() not in {"none", "-", "\u2013", "\u2014", "n/a"}:
            out.append(token)
    return out


# ---------------------------------------------------------------------------------------------------------------------
# YAML with line numbers
# ---------------------------------------------------------------------------------------------------------------------


class YMap(dict):
    """A YAML mapping that remembers its own line and the line of each key (1-based)."""

    def __init__(self) -> None:
        super().__init__()
        self.line = 1
        self.key_lines: dict[Any, int] = {}

    def kline(self, key: Any) -> int:
        return self.key_lines.get(key, self.line)


class _LineLoader(yaml.SafeLoader):
    pass


def _construct_map(loader: _LineLoader, node: yaml.MappingNode) -> YMap:
    loader.flatten_mapping(node)
    result = YMap()
    result.line = node.start_mark.line + 1
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=True)
        result[key] = loader.construct_object(value_node, deep=True)
        result.key_lines[key] = key_node.start_mark.line + 1
    return result


_LineLoader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _construct_map)


def load_yaml(text: str) -> Any:
    return yaml.load(text, Loader=_LineLoader)


@dataclass
class Workflow:
    path: str
    text: str
    doc: YMap

    @property
    def name(self) -> str:
        return self.path.rsplit("/", 1)[-1]

    @property
    def triggers(self) -> set[str]:
        on = self.doc.get("on", self.doc.get(True))  # YAML 1.1 reads a bare `on` as true
        if isinstance(on, str):
            return {on}
        if isinstance(on, list):
            return {str(t) for t in on}
        if isinstance(on, dict):
            return {str(t) for t in on}
        return set()

    @property
    def jobs(self) -> list[tuple[str, YMap]]:
        jobs = self.doc.get("jobs")
        if not isinstance(jobs, YMap):
            return []
        return [(str(k), v) for k, v in jobs.items() if isinstance(v, YMap)]

    def job_line(self, job_id: str) -> int:
        jobs = self.doc.get("jobs")
        return jobs.kline(job_id) if isinstance(jobs, YMap) else 1

    def steps(self, job: YMap) -> list[YMap]:
        steps = job.get("steps")
        return [s for s in steps if isinstance(s, YMap)] if isinstance(steps, list) else []

    def env(self, *scopes: Any) -> dict[str, Any]:
        """Merged env of the workflow and the given job/step mappings (innermost last)."""
        merged: dict[str, Any] = {}
        for scope in (self.doc, *scopes):
            env = scope.get("env") if isinstance(scope, dict) else None
            if isinstance(env, dict):
                merged.update({str(k): v for k, v in env.items()})
        return merged

    def raw_line(self, needle: str, start: int = 1) -> int:
        lines = self.text.splitlines()
        for i in range(max(start, 1) - 1, len(lines)):
            if needle and needle in lines[i]:
                return i + 1
        return start


def matrix_items(job: YMap) -> list[dict[str, Any]]:
    strategy = job.get("strategy")
    matrix = strategy.get("matrix") if isinstance(strategy, dict) else None
    if not isinstance(matrix, dict):
        return [{}]
    include = matrix.get("include")
    if isinstance(include, list) and include:
        return [i for i in include if isinstance(i, dict)] or [{}]
    axes = {k: v for k, v in matrix.items() if isinstance(v, list) and k not in ("include", "exclude")}
    if not axes:
        return [{}]
    keys = list(axes)
    return [dict(zip(keys, combo, strict=True)) for combo in itertools.product(*(axes[k] for k in keys))]


# ---------------------------------------------------------------------------------------------------------------------
# pixi manifest model
# ---------------------------------------------------------------------------------------------------------------------


@dataclass
class TaskDef:
    name: str
    feature: str | None  # None = the default feature
    spec: Any
    line: int


class PixiManifest:
    def __init__(self, text: str) -> None:
        self.text = text
        self.data = tomllib.loads(text)
        self.defs: list[TaskDef] = []
        for name, spec in self._tasks_in(self.data):
            self.defs.append(TaskDef(name, None, spec, self._task_line(None, name)))
        features = self.data.get("feature", {})
        for fname, fdata in features.items() if isinstance(features, dict) else []:
            for name, spec in self._tasks_in(fdata):
                self.defs.append(TaskDef(name, fname, spec, self._task_line(fname, name)))
        self.envs: dict[str, list[str | None]] = {}
        declared = self.data.get("environments", {})
        for env, spec in declared.items() if isinstance(declared, dict) else []:
            if isinstance(spec, list):
                feats: list[str | None] = list(spec)
                no_default = False
            else:
                feats = list(spec.get("features", []))
                no_default = bool(spec.get("no-default-feature", False))
            self.envs[env] = [*feats, *([] if no_default else [None])]
        self.envs.setdefault("default", [None])
        self.task_envs: dict[str, set[str]] = {d.name: set() for d in self.defs}
        for d in self.defs:
            for env, feats in self.envs.items():
                if d.feature in feats:
                    self.task_envs[d.name].add(env)

    @staticmethod
    def _tasks_in(table: Any) -> Iterator[tuple[str, Any]]:
        if not isinstance(table, dict):
            return
        tasks = table.get("tasks", {})
        if isinstance(tasks, dict):
            yield from tasks.items()
        targets = table.get("target", {})
        for tdata in targets.values() if isinstance(targets, dict) else []:
            ttasks = tdata.get("tasks", {}) if isinstance(tdata, dict) else {}
            if isinstance(ttasks, dict):
                yield from ttasks.items()

    def _task_line(self, feature: str | None, name: str) -> int:
        prefix = rf"feature\.{re.escape(feature)}\." if feature else ""
        # [ \t]* rather than \s*: a leading \s* would start the match on the blank line before the header
        header = rf"^[ \t]*\[[ \t]*{prefix}(?:target\.[\w-]+\.)?tasks\.\"?{re.escape(name)}\"?[ \t]*\]"
        m = re.search(header, self.text, re.MULTILINE)
        if not m:
            m = re.search(rf"^[ \t]*\"?{re.escape(name)}\"?[ \t]*=", self.text, re.MULTILINE)
        return self.text.count("\n", 0, m.start()) + 1 if m else 1

    @property
    def visible_tasks(self) -> list[str]:
        return sorted({d.name for d in self.defs if not d.name.startswith("_")})


TASK_TOKEN_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.:-]*$")
PIXI_RUN_RE = re.compile(r"\bpixi\s+run\b([^\n`]*)")


def parse_pixi_run(rest: str) -> tuple[str | None, str | None]:
    """(env, task-or-command) from the text after `pixi run`; (None, None) when there is nothing to check."""
    rest = re.split(r"&&|\|\||[;|)]", rest, maxsplit=1)[0]
    tokens = rest.split()
    env: str | None = None
    i = 0
    while i < len(tokens):
        tok = tokens[i].strip("'\"")
        if tok in ("-x", "--executable"):
            return None, None  # an arbitrary command, not a task
        if tok in ("-e", "--environment", "-m", "--manifest-path"):
            if tok in ("-e", "--environment") and i + 1 < len(tokens):
                env = tokens[i + 1].strip("'\"")
            i += 2
            continue
        if tok.startswith("--environment="):
            env = tok.split("=", 1)[1]
        elif not tok.startswith("-"):
            return env, tok
        i += 1
    return env, None


def pixi_runs(s: str) -> Iterator[tuple[str | None, str]]:
    for m in PIXI_RUN_RE.finditer(s):
        env, task = parse_pixi_run(m.group(1))
        if task:
            yield env, task


def normalize_version(v: Any) -> str | None:
    m = re.search(r"\d+\.\d+\.\d+", str(v))
    return m.group(0) if m else None


# ---------------------------------------------------------------------------------------------------------------------
# Context shared by the checks
# ---------------------------------------------------------------------------------------------------------------------


class Ctx:
    def __init__(self, repo: Repo) -> None:
        self.repo = repo

    @cached_property
    def pixi(self) -> PixiManifest | None:
        text = self.repo.text("pixi.toml")
        if text is None:
            return None
        try:
            return PixiManifest(text)
        except tomllib.TOMLDecodeError as e:
            self.repo.add("R001", "pixi.toml", getattr(e, "lineno", 1), f"pixi.toml is not valid TOML: {e}", "fix it")
            return None

    @cached_property
    def workflows(self) -> list[Workflow]:
        out = []
        for path in self.repo.under(".github/workflows/", (".yml", ".yaml")):
            text = self.repo.text(path) or ""
            try:
                doc = load_yaml(text)
            except yaml.YAMLError as e:
                mark = getattr(e, "problem_mark", None)
                self.repo.add("R010", path, mark.line + 1 if mark else 1, "not valid YAML", "fix the syntax")
                continue
            if isinstance(doc, YMap):
                out.append(Workflow(path, text, doc))
        return out

    @cached_property
    def presets(self) -> dict[str, dict[str, bool]] | None:
        """kind (configure/build/test/workflow/package) -> {preset name: hidden}."""
        if not self.repo.exists("CMakePresets.json"):
            return None
        kinds: dict[str, dict[str, bool]] = {k: {} for k in ("configure", "build", "test", "workflow", "package")}
        seen: set[Path] = set()

        def load(path: Path) -> None:
            if path in seen or not path.is_file():
                return
            seen.add(path)
            try:
                data = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as e:
                rel = path.relative_to(self.repo.root).as_posix()
                self.repo.add("R006", rel, 1, f"cannot parse presets: {e}", "fix the JSON")
                return
            for inc in data.get("include", []):
                load((path.parent / inc).resolve())
            for kind in kinds:
                for p in data.get(f"{kind}Presets", []):
                    if isinstance(p, dict) and "name" in p:
                        kinds[kind][p["name"]] = bool(p.get("hidden", False))

        load(self.repo.root / "CMakePresets.json")
        return kinds

    def markdown_docs(self) -> list[str]:
        """Agent docs, docs/** and skills (.claude/skills is a byte copy of .agents/skills, see R004)."""
        docs = [p for p in AGENT_DOCS if p in self.repo.file_set]
        docs += self.repo.under("docs/", (".md",))
        docs += self.repo.under(".agents/skills/", (".md",))
        docs += self.repo.under(".claude/agents/", (".md",))
        docs += self.repo.under(".github/codex/", (".md",))
        return sorted(set(docs))


# ---------------------------------------------------------------------------------------------------------------------
# R001-R002: tasks
# ---------------------------------------------------------------------------------------------------------------------


def check_task_ref(ctx: Ctx, path: str, line: int, env: str | None, task: str) -> None:
    pixi = ctx.pixi
    if pixi is None or not TASK_TOKEN_RE.match(task):
        return
    if env is not None and not TASK_TOKEN_RE.match(env):
        env = None  # computed at run time (`-e "$PIXI_ENV"`)
    if env is not None and env not in pixi.envs:
        ctx.repo.add(
            "R001", path, line, f"`pixi run -e {env} {task}`: no environment `{env}`", f"use one of {sorted(pixi.envs)}"
        )
        return
    if task in pixi.task_envs:
        envs = pixi.task_envs[task]
        if env is not None and env not in envs:
            ctx.repo.add(
                "R001",
                path,
                line,
                f"task `{task}` is not in environment `{env}` (it is in {sorted(envs) or 'no environment'})",
                "use the task's environment or drop `-e`",
            )
        return
    if env is not None and task in ENV_COMMANDS:
        return
    ctx.repo.add(
        "R001", path, line, f"`pixi run {task}` names no pixi task", "use a task from pixi.toml (`pixi task list`)"
    )


def check_r001(ctx: Ctx) -> None:
    pixi = ctx.pixi
    if pixi is None:
        return
    repo = ctx.repo
    names: dict[str, list[TaskDef]] = {}
    for d in pixi.defs:
        names.setdefault(d.name, []).append(d)
        if d.feature is None:
            repo.add(
                "R001",
                "pixi.toml",
                d.line,
                f"task `{d.name}` is defined in the default feature (it would exist in every environment)",
                "move it to [feature.<env feature>.tasks]",
            )
    for name, defs in sorted(names.items()):
        envs = pixi.task_envs.get(name, set())
        features = sorted({d.feature or "default" for d in defs})  # per-platform variants share a feature
        if len(features) > 1:
            where = ", ".join(features)
            repo.add(
                "R001",
                "pixi.toml",
                defs[1].line,
                f"task `{name}` is defined in more than one feature ({where})",
                "keep one definition",
            )
        if len(envs) != 1:
            repo.add(
                "R001",
                "pixi.toml",
                defs[0].line,
                f"task `{name}` must exist in exactly one environment, found {sorted(envs) or 'none'}",
                "define it in a feature used by exactly one environment",
            )
    # depends-on targets
    for d in pixi.defs:
        if not isinstance(d.spec, dict):
            continue
        deps = d.spec.get("depends-on", d.spec.get("depends_on", []))
        if not isinstance(deps, list):
            deps = [deps]
        for dep in deps:
            if isinstance(dep, str):
                dep_task, dep_env = dep, None
            elif isinstance(dep, dict):
                dep_task, dep_env = str(dep.get("task", "")), dep.get("environment")
            else:
                continue
            available = pixi.task_envs.get(dep_task)
            if available is None:
                repo.add(
                    "R001", "pixi.toml", d.line, f"task `{d.name}` depends on unknown task `{dep_task}`", "fix the name"
                )
            elif dep_env is not None and dep_env not in available:
                repo.add(
                    "R001",
                    "pixi.toml",
                    d.line,
                    f"task `{d.name}` depends on `{dep_task}` in environment `{dep_env}`, "
                    f"but it is in {sorted(available)}",
                    "fix the environment",
                )
            elif dep_env is None and not pixi.task_envs.get(d.name, set()) <= available:
                repo.add(
                    "R001",
                    "pixi.toml",
                    d.line,
                    f"task `{d.name}` depends on `{dep_task}`, which is not in the same environment",
                    f'use {{ task = "{dep_task}", environment = "<env>" }}',
                )
    # references in docs, templates, workflows and settings
    for path in ctx.markdown_docs():
        for no, snippet in md_code_contexts(repo.text(path) or ""):
            for env, task in pixi_runs(snippet):
                check_task_ref(ctx, path, no, env, task)
    for path in repo.under(".github/ISSUE_TEMPLATE/", (".yml", ".yaml", ".md")):
        for no, line in enumerate((repo.text(path) or "").splitlines(), 1):
            for env, task in pixi_runs(line):
                check_task_ref(ctx, path, no, env, task)
    for wf in ctx.workflows:
        check_workflow_task_refs(ctx, wf)
    settings = repo.text(".claude/settings.json")
    if settings is not None:
        try:
            perms = json.loads(settings).get("permissions", {})
        except (json.JSONDecodeError, AttributeError):
            perms = {}
        for kind in ("allow", "ask", "deny"):
            for rule in perms.get(kind, []) if isinstance(perms, dict) else []:
                m = re.fullmatch(r"Bash\((.*)\)", str(rule))
                if m:
                    no = line_of(settings, re.escape(json.dumps(rule)))
                    for env, task in pixi_runs(m.group(1).replace(":*", " *")):
                        check_task_ref(ctx, ".claude/settings.json", no, env, task)


def _resolve(token: str, env: dict[str, Any], item: dict[str, Any]) -> str | None:
    """Literal value of a task/env token in a workflow: `$VAR` from `env:`, `${{matrix.key}}` from a matrix item."""
    token = token.strip("'\"")
    m = re.fullmatch(r"\$\{?(\w+)\}?", token)
    if m:
        if m.group(1) not in env:
            return None
        token = str(env[m.group(1)]).strip()
    m = re.fullmatch(r"\$\{\{\s*matrix\.([\w-]+)\s*\}\}", token)
    if m:
        value = item.get(m.group(1))
        return None if value is None else str(value)
    return None if re.search(r"[${<]", token) else token


def check_workflow_task_refs(ctx: Ctx, wf: Workflow) -> None:
    for _, job in wf.jobs:
        texts: list[tuple[int, str, dict[str, Any]]] = []
        if isinstance(job.get("name"), str):
            texts.append((job.kline("name"), job["name"], wf.env(job)))
        for step in wf.steps(job):
            run = step.get("run")
            if isinstance(run, str):
                start = step.kline("run")
                texts.extend((wf.raw_line(part.strip(), start), part, wf.env(job, step)) for part in run.splitlines())
        for no, text, env in texts:
            compact = re.sub(r"\$\{\{\s*(.*?)\s*\}\}", r"${{\1}}", text)  # one token per expression
            for m in PIXI_RUN_RE.finditer(compact):
                raw_env, raw_task = parse_pixi_run(m.group(1))
                if not raw_task:
                    continue
                for item in matrix_items(job):
                    task = _resolve(raw_task, env, item)
                    run_env = _resolve(raw_env, env, item) if raw_env else None
                    if task:
                        check_task_ref(ctx, wf.path, no, run_env, task)
    for no, line in enumerate(wf.text.splitlines(), 1):
        for m in re.finditer(r"Bash\((pixi run[^)]*)\)", line):
            for env, task in pixi_runs(m.group(1).replace(":*", " *")):
                check_task_ref(ctx, wf.path, no, env, task)


def documented_tasks(ctx: Ctx) -> set[str] | None:
    repo = ctx.repo
    agents, ci_doc = repo.text("AGENTS.md"), repo.text("docs/ci.md")
    if agents is None and ci_doc is None:
        return None
    found: set[str] = set()

    def collect(snippet: str) -> None:
        runs = list(pixi_runs(snippet))
        if runs:
            found.update(task for _, task in runs)
        elif re.fullmatch(r"[\w-]+", snippet):
            found.add(snippet)

    for _, line, fenced in md_lines(agents or ""):
        if not fenced and line.strip().startswith("|"):
            for span in code_spans(line):
                collect(span)
    for _, snippet in md_code_contexts(ci_doc or ""):
        collect(snippet)
    return found


def check_r002(ctx: Ctx) -> None:
    pixi = ctx.pixi
    documented = documented_tasks(ctx)
    if pixi is None or documented is None:
        return
    for name in pixi.visible_tasks:
        if name not in documented:
            line = next(d.line for d in pixi.defs if d.name == name)
            ctx.repo.add(
                "R002",
                "pixi.toml",
                line,
                f"task `{name}` is not documented in the AGENTS.md command table or docs/ci.md",
                f"add `pixi run {name}` to the AGENTS.md command table (or docs/ci.md)",
            )


# ---------------------------------------------------------------------------------------------------------------------
# R003-R005: agent docs, skills, pixi version
# ---------------------------------------------------------------------------------------------------------------------


def check_r003(ctx: Ctx) -> None:
    repo = ctx.repo
    claude = repo.text("CLAUDE.md")
    if claude is not None and (claude.splitlines() or [""])[0].rstrip() != "@AGENTS.md":
        repo.add("R003", "CLAUDE.md", 1, "line 1 must be `@AGENTS.md`", "make `@AGENTS.md` the first line")
    agents = repo.text("AGENTS.md")
    if agents is not None:
        lines = agents.splitlines()
        if "## Code Review Rules" not in (line.rstrip() for line in lines):
            repo.add(
                "R003", "AGENTS.md", 1, "no `## Code Review Rules` section", "add the section (AI reviewers use it)"
            )
        if len(lines) > AGENTS_MAX_LINES:
            repo.add(
                "R003",
                "AGENTS.md",
                AGENTS_MAX_LINES + 1,
                f"{len(lines)} lines (max {AGENTS_MAX_LINES})",
                "move detail into docs/ and link it",
            )
        size = len(agents.encode("utf-8"))
        if size >= AGENTS_MAX_BYTES:
            repo.add("R003", "AGENTS.md", 1, f"{size} bytes (must stay < {AGENTS_MAX_BYTES})", "move detail to docs/")
    copilot = repo.text(".github/copilot-instructions.md")
    if copilot is not None:
        n = len(copilot.splitlines())
        if n > COPILOT_MAX_LINES:
            repo.add(
                "R003",
                ".github/copilot-instructions.md",
                COPILOT_MAX_LINES + 1,
                f"{n} lines (max {COPILOT_MAX_LINES})",
                "point to AGENTS.md instead of repeating it",
            )
        if "AGENTS.md" not in copilot:
            repo.add("R003", ".github/copilot-instructions.md", 1, "does not mention AGENTS.md", "point to AGENTS.md")


def check_r004(ctx: Ctx) -> None:
    repo = ctx.repo
    canonical = repo.under(".agents/skills/")
    if not canonical:
        return
    copies = repo.under(".claude/skills/")
    want = {p.removeprefix(".agents/skills/") for p in canonical}
    have = {p.removeprefix(".claude/skills/") for p in copies}
    for rel in sorted(want - have):
        repo.add("R004", f".agents/skills/{rel}", 1, "missing from .claude/skills", "run `pixi run fmt` (sync_skills)")
    for rel in sorted(have - want):
        repo.add("R004", f".claude/skills/{rel}", 1, "not in .agents/skills (canonical)", "delete or move it")
    for rel in sorted(want & have):
        if (repo.root / ".agents/skills" / rel).read_bytes() != (repo.root / ".claude/skills" / rel).read_bytes():
            repo.add("R004", f".claude/skills/{rel}", 1, "differs from .agents/skills", "run `pixi run fmt`")
    for path in canonical:
        if not path.endswith("/SKILL.md"):
            continue
        text = repo.text(path) or ""
        m = re.match(r"---\n(.*?)\n---\n", text, re.DOTALL)
        meta: Any = None
        if m:
            try:
                meta = yaml.safe_load(m.group(1))
            except yaml.YAMLError:
                meta = None
        if not isinstance(meta, dict):
            repo.add("R004", path, 1, "no YAML frontmatter", "start with ---/name/description/---")
            continue
        for key in ("name", "description"):
            if not meta.get(key):
                repo.add("R004", path, 1, f"frontmatter has no `{key}`", f"add `{key}:`")
        if "allowed-tools" in meta:
            repo.add(
                "R004",
                path,
                line_of(text, r"^allowed-tools:"),
                "`allowed-tools` in a shared skill",
                "remove it (permissions live in .claude/settings.json)",
            )


VERSION_PATTERNS = (
    re.compile(r"\bPIXI_VERSION\b\s*[:=]\s*[\"']?v?(\d+\.\d+\.\d+)"),
    re.compile(r"\bpixi-version:\s*[\"']?v?(\d+\.\d+\.\d+)"),
    re.compile(r"(?<![\w-])pixi(?:\s*(?:>=|≥)\s*|[\s_-]+v?)(\d+\.\d+\.\d+)", re.IGNORECASE),
    re.compile(r"pixi\.prefix\.dev/v(\d+\.\d+\.\d+)/"),
    re.compile(r"prefix-dev/pixi/releases/download/v(\d+\.\d+\.\d+)"),
)


def check_r005(ctx: Ctx) -> None:
    repo = ctx.repo
    lock = repo.text("pixi.lock")
    if lock is not None and (lock.splitlines() or [""])[0].rstrip() != LOCK_FIRST_LINE:
        repo.add(
            "R005",
            "pixi.lock",
            1,
            f"line 1 must be `{LOCK_FIRST_LINE}`",
            "re-lock with pixi 0.81.0 (`pixi lock`); never hand-edit pixi.lock",
        )
    pixi = ctx.pixi
    if pixi is None:
        return
    workspace = pixi.data.get("workspace", pixi.data.get("project", {}))
    requires = workspace.get("requires-pixi") if isinstance(workspace, dict) else None
    expected = normalize_version(requires) if requires else None
    if expected is None:
        repo.add("R005", "pixi.toml", 1, "no `requires-pixi` in [workspace]", 'add requires-pixi = ">=X.Y.Z"')
        return
    fix = f"use pixi {expected} (pixi.toml requires-pixi) everywhere"
    docs = [
        d for d in ctx.markdown_docs() if not d.startswith("docs/adr/")
    ]  # ADRs are history: they may cite old versions
    files = ["pixi.toml", "scripts/agent-setup.sh", *docs]
    files += repo.under(".claude/hooks/", (".sh",)) + repo.under(".github/workflows/", (".yml", ".yaml"))
    for path in files:
        text = repo.text(path)
        if text is None:
            continue
        for no, line in enumerate(text.splitlines(), 1):
            if path == "pixi.toml" and re.match(r"\s*requires-pixi\s*=", line):
                continue
            for pattern in VERSION_PATTERNS:
                for m in pattern.finditer(line):
                    if m.group(1) != expected:
                        repo.add("R005", path, no, f"pixi version {m.group(1)} != {expected}", fix)
    setup = repo.text("scripts/agent-setup.sh")
    if setup is not None and not re.search(r"^\s*PIXI_VERSION=", setup, re.MULTILINE):
        repo.add("R005", "scripts/agent-setup.sh", 1, "no `PIXI_VERSION=` pin", f"add PIXI_VERSION={expected}")
    for wf in ctx.workflows:
        for _, job in wf.jobs:
            for step in wf.steps(job):
                uses = step.get("uses")
                if not (isinstance(uses, str) and uses.startswith("prefix-dev/setup-pixi@")):
                    continue
                with_ = step.get("with") if isinstance(step.get("with"), YMap) else YMap()
                value = with_.get("pixi-version")
                if value is None:
                    repo.add("R005", wf.path, step.line, "setup-pixi without `pixi-version`", f"add v{expected}")
                    continue
                m = re.fullmatch(r"\$\{\{\s*env\.(\w+)\s*\}\}", str(value).strip())
                if m and normalize_version(wf.env(job, step).get(m.group(1))) != expected:
                    repo.add("R005", wf.path, with_.kline("pixi-version"), f"`{value}` is not pixi {expected}", fix)


# ---------------------------------------------------------------------------------------------------------------------
# R006-R008: presets, modules, labels
# ---------------------------------------------------------------------------------------------------------------------

PRESET_RE = re.compile(r"--preset(?:\s+|=)[\"']?([^\s\"'`;&|)]+)")
BUILD_DIR_RE = re.compile(r"(?<![\w.-])build/([A-Za-z0-9_][A-Za-z0-9_.-]*)")


def build_dir_prefix_ok(prefix: str) -> bool:
    """Whether `build/` starts a repo-relative path: at a token start, after `=`/quote, or after `$root/`."""
    return (
        prefix == ""
        or prefix[-1] in "=:(\"'`"
        or re.fullmatch(r"[\"'=(]*(\$\{?\w+\}?|\$\{\{[^}]*\}\}|\.)/", prefix) is not None
    )


def preset_kind(segment: str) -> str | None:
    if "--workflow" in segment:
        return "workflow"
    if re.search(r"--build\b", segment):
        return "build"
    if re.search(r"\bctest\b", segment):
        return "test"
    if re.search(r"\bcmake\b", segment):
        return "configure"
    return None


MATRIX_REF_RE = re.compile(r"\$\{\{\s*matrix\.([\w-]+)\s*\}\}")


def resolve_matrix(line: str, item: dict[str, Any]) -> str:
    """`line` with every `${{ matrix.key }}` that the matrix item defines replaced by its value."""
    return MATRIX_REF_RE.sub(lambda m: str(item[m.group(1)]) if m.group(1) in item else m.group(0), line)


def check_preset_line(ctx: Ctx, presets: dict[str, dict[str, bool]], path: str, no: int, line: str) -> None:
    repo = ctx.repo
    for segment in re.split(r"&&|\|\||;|\|", line):
        for m in PRESET_RE.finditer(segment):
            name = m.group(1)
            if re.search(r"[${<]", name):
                continue
            kind = preset_kind(segment)
            kinds = [kind] if kind else [k for k in presets if name in presets[k]]
            if not kinds:
                repo.add("R006", path, no, f"no CMake preset `{name}`", "use a preset from CMakePresets.json")
                continue
            for k in kinds:
                if name not in presets[k]:
                    repo.add("R006", path, no, f"no {k} preset `{name}`", f"add it or use one of {sorted(presets[k])}")
                elif presets[k][name]:
                    repo.add("R006", path, no, f"{k} preset `{name}` is hidden", "use a visible preset")
    for m in BUILD_DIR_RE.finditer(line):
        before = line[: m.start()]
        prefix = before.split()[-1] if before and not before[-1].isspace() else ""
        if not build_dir_prefix_ok(prefix):
            continue  # e.g. "environment/build/data" in prose, or a URL
        name = m.group(1)
        if name not in presets["configure"] or presets["configure"][name]:
            repo.add(
                "R006",
                path,
                no,
                f"`build/{name}` is not the build dir of a configure preset",
                "build dirs are build/<configure preset>",
            )


def check_r006(ctx: Ctx) -> None:
    presets = ctx.presets
    if presets is None:
        return
    repo = ctx.repo
    files = ["pixi.toml", *repo.under("scripts/", (".sh",)), *repo.under(".claude/hooks/", (".sh",))]
    files += repo.under(".github/workflows/", (".yml", ".yaml"))
    for path in files:
        for no, line in enumerate((repo.text(path) or "").splitlines(), 1):
            check_preset_line(ctx, presets, path, no, line)
    # `--preset ${{ matrix.preset }}` and `build/${{ matrix.preset }}/junit.xml`: check every matrix value.
    for wf in ctx.workflows:
        lines = wf.text.splitlines()
        starts = sorted(wf.job_line(job_id) for job_id, _ in wf.jobs)
        for job_id, job in wf.jobs:
            first = wf.job_line(job_id)
            last = min((s for s in starts if s > first), default=len(lines) + 1)
            items = [item for item in matrix_items(job) if item]
            for no in range(first, last):
                if items and MATRIX_REF_RE.search(lines[no - 1]):
                    for item in items:
                        check_preset_line(ctx, presets, wf.path, no, resolve_matrix(lines[no - 1], item))


def parse_modules_cmake(text: str) -> tuple[list[str], dict[str, list[str]], dict[str, list[str]]]:
    code = re.sub(r"#[^\n]*", "", text)

    def values(raw: str) -> list[str]:
        return [v for v in re.split(r"[\s;]+", raw.replace('"', " ")) if v]

    modules: list[str] = []
    m = re.search(r"set\(\s*ANTB1_MODULES\s+([^)]*)\)", code)
    if m:
        modules = values(m.group(1))
    deps = {k: values(v) for k, v in re.findall(r"set\(\s*ANTB1_DEPS_(\w+)\s*([^)]*)\)", code)}
    ext = {k: values(v) for k, v in re.findall(r"set\(\s*ANTB1_EXT_(\w+)\s*([^)]*)\)", code)}
    return modules, deps, ext


def closure(module: str, deps: dict[str, list[str]]) -> set[str]:
    seen: set[str] = set()
    stack = list(deps.get(module, []))
    while stack:
        d = stack.pop()
        if d not in seen:
            seen.add(d)
            stack.extend(deps.get(d, []))
    return seen


MODULE_ARG_KEYWORDS = frozenset({"SOURCES", "PUBLIC_DEPS", "PRIVATE_DEPS", "PUBLIC_LIBS", "PRIVATE_LIBS"})


def public_deps(repo: Repo, module: str, deps: dict[str, list[str]]) -> list[str]:
    """PUBLIC_DEPS of `antb1_add_module(<module> ...)` in src/<module>/CMakeLists.txt (all allowed deps without it)."""
    text = repo.text(f"src/{module}/CMakeLists.txt")
    m = re.search(r"\bantb1_add_module\s*\(([^)]*)\)", re.sub(r"#[^\n]*", "", text or ""))
    if m is None:
        return deps.get(module, [])
    out: list[str] = []
    keyword = None
    for token in m.group(1).split()[1:]:
        if token in MODULE_ARG_KEYWORDS:
            keyword = token
        elif keyword == "PUBLIC_DEPS":
            out.append(token)
    return out


def visible_modules(repo: Repo, module: str, deps: dict[str, list[str]]) -> set[str]:
    """Modules whose headers `module` may include: itself, its allowed deps and what they re-export (PUBLIC_DEPS).

    A PRIVATE dep of a dep is an implementation detail: `cli` may name `plan` types that `engine` exposes, but it
    must not reach into `io` or `exec` behind `engine`'s back.
    """
    seen = {module}
    stack = list(deps.get(module, []))
    while stack:
        d = stack.pop()
        if d not in seen:
            seen.add(d)
            stack.extend(public_deps(repo, d, deps))
    return seen


CXX_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".tpp")
INCLUDE_RE = re.compile(r"^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"]+)[>\"]", re.MULTILINE)  # no \s: it spans lines


def check_r007(ctx: Ctx) -> None:
    repo = ctx.repo
    text = repo.text("cmake/Antb1Modules.cmake")
    if text is None:
        return
    modules, deps, ext = parse_modules_cmake(text)
    ext_names = {m: {lib.split("::", 1)[0] for lib in libs} for m, libs in ext.items()}
    arch = repo.text("docs/architecture.md")
    if arch is not None:
        check_module_table(ctx, arch, modules, deps, ext_names)
    for module in modules:
        visible = visible_modules(repo, module, deps)
        reachable = {module} | closure(module, deps)  # tests may link any module their module may reach
        prefixes = {p for lib in ext_names.get(module, set()) for p in EXTERNAL_INCLUDE_PREFIXES.get(lib, ())}
        for path in repo.under(f"src/{module}/", CXX_SUFFIXES):
            is_test = path.startswith(f"src/{module}/tests/")
            allowed = reachable if is_test else visible
            src = repo.text(path) or ""
            for m in INCLUDE_RE.finditer(src):
                inc, no = m.group(1), src.count("\n", 0, m.start()) + 1
                own = re.match(r"antb1/([^/]+)/", inc)
                if own and own.group(1) not in allowed:
                    repo.add(
                        "R007",
                        path,
                        no,
                        f"module `{module}` includes `{inc}`; it may include antb1 modules {sorted(allowed)}",
                        "respect cmake/Antb1Modules.cmake and the PUBLIC_DEPS of src/*/CMakeLists.txt "
                        "(a new edge needs an ADR)",
                    )
                elif NEVER_IN_SRC_RE.match(inc):
                    repo.add("R007", path, no, f"`{inc}` in src/: DuckDB is a test oracle only", "remove the include")
                elif inc.startswith(KNOWN_EXTERNAL_PREFIXES) and not inc.startswith(tuple(prefixes)):
                    if is_test and ext_names.get(module):
                        continue  # tests may link extra libraries; only Arrow-free modules stay Arrow-free in tests
                    repo.add(
                        "R007",
                        path,
                        no,
                        f"module `{module}` includes `{inc}` but does not link that library",
                        "move the code to a module that may use it (cmake/Antb1Modules.cmake ANTB1_EXT_*)",
                    )


def check_module_table(
    ctx: Ctx, arch: str, modules: list[str], deps: dict[str, list[str]], ext: dict[str, set[str]]
) -> None:
    repo, path = ctx.repo, "docs/architecture.md"
    fix = "make the docs/architecture.md module table match cmake/Antb1Modules.cmake"
    for table in md_tables(arch):
        mcol, dcol = table.column(r"^modules?$"), table.column(r"depend|allowed|edge")
        if mcol is None or dcol is None:
            continue
        ecol = table.column(r"extern|librar")
        rows: dict[str, tuple[int, list[str]]] = {}
        for no, cells in table.rows:
            if mcol < len(cells) and clean_cell(cells[mcol]):
                name = clean_cell(cells[mcol]).split()[0]
                rows[name] = (no, cells)
                documented = words_of(cells[dcol]) if dcol < len(cells) else []
                if name in modules and sorted(documented) != sorted(deps.get(name, [])):
                    want = ", ".join(deps.get(name, [])) or "none"
                    repo.add("R007", path, no, f"`{name}` dependencies differ from the allow-list ({want})", fix)
                if name in modules and ecol is not None and ecol < len(cells):
                    libs = {w.lower() for w in words_of(cells[ecol])}
                    if libs != {e.lower() for e in ext.get(name, set())}:
                        want = ", ".join(sorted(ext.get(name, set()))) or "none"
                        repo.add("R007", path, no, f"`{name}` external libraries differ ({want})", fix)
        for name in modules:
            if name not in rows:
                repo.add("R007", path, table.line, f"module `{name}` is missing from the module table", fix)
        for name, (no, _) in rows.items():
            if name not in modules:
                repo.add("R007", path, no, f"`{name}` is not a module in cmake/Antb1Modules.cmake", fix)
        return
    repo.add("R007", path, 1, "no module table (| Module | Depends on | ...)", fix)


def check_r008(ctx: Ctx) -> None:
    repo = ctx.repo
    testing_cmake, testing_md = repo.text("cmake/Antb1Testing.cmake"), repo.text("docs/testing.md")
    if testing_cmake is None or testing_md is None:
        return
    comment = " ".join(
        line.lstrip()[1:].strip() for line in testing_cmake.splitlines() if line.lstrip().startswith("#")
    )
    m = re.search(r"\bLabels\b[^:]*:\s*(.+?)\.(?:\s|$)", comment)
    if not m:
        repo.add(
            "R008", "cmake/Antb1Testing.cmake", 1, "no `Labels (...): a b c.` comment", "list the ctest labels there"
        )
        return
    labels = set(m.group(1).replace(",", " ").split())
    for table in md_tables(testing_md):
        col = table.column(r"^(ctest\s+)?labels?$")
        if col is None:
            continue
        documented: dict[str, int] = {}
        for no, cells in table.rows:
            if col < len(cells):
                for word in words_of(cells[col]):
                    documented.setdefault(word, no)
        for label in sorted(labels - documented.keys()):
            repo.add(
                "R008", "docs/testing.md", table.line, f"ctest label `{label}` is not in the labels table", "add it"
            )
        for label in sorted(documented.keys() - labels):
            repo.add(
                "R008",
                "docs/testing.md",
                documented[label],
                f"`{label}` is not a ctest label (cmake/Antb1Testing.cmake)",
                "fix the table or the labels comment",
            )
        return
    repo.add("R008", "docs/testing.md", 1, "no labels table (| Label | ...)", "add one row per ctest label")


# ---------------------------------------------------------------------------------------------------------------------
# R009-R010: data files, workflows
# ---------------------------------------------------------------------------------------------------------------------


def check_r009(ctx: Ctx) -> None:
    repo = ctx.repo
    for path in repo.files:
        base = path.rsplit("/", 1)[-1].lower()
        if any(fnmatch.fnmatchcase(base, g) for g in DATA_FILE_GLOBS) or DATA_FILE_RE.fullmatch(base):
            repo.add(
                "R009",
                path,
                1,
                "data file in the repository (ClickBench-derived or large data is never committed)",
                "remove it; tests generate data, `pixi run fetch-data` downloads ClickBench",
            )
        elif path not in LARGE_FILES_ALLOWED and (repo.root / path).stat().st_size > MAX_FILE_BYTES:
            repo.add("R009", path, 1, "file larger than 1 MiB", "do not commit it")


USES_LINE_RE = re.compile(r"^\s*(?:-\s+)?uses:\s*[\"']?([^\s\"'#]+)[\"']?\s*(#.*)?$")


def check_r010(ctx: Ctx) -> None:
    repo = ctx.repo
    for wf in ctx.workflows:
        doc = wf.doc
        perms = doc.get("permissions", None)
        if "permissions" not in doc:
            repo.add("R010", wf.path, 1, "no top-level `permissions: {}`", "add `permissions: {}`; grant per job")
        elif perms != {}:
            repo.add(
                "R010", wf.path, doc.kline("permissions"), "top-level permissions must be `{}`", "grant per job instead"
            )
        on_line = doc.kline("on") if "on" in doc else doc.kline(True)
        for bad in ("pull_request_target", "workflow_run"):
            if bad in wf.triggers:
                repo.add("R010", wf.path, wf.raw_line(bad, on_line), f"`{bad}` trigger", "use pull_request/push")
        if wf.name in REQUIRED_MERGE_GROUP_WORKFLOWS and "merge_group" not in wf.triggers:
            repo.add("R010", wf.path, on_line, "no `merge_group` trigger (required check)", "add `merge_group:`")
        for job_id, job in wf.jobs:
            if "uses" not in job and "timeout-minutes" not in job:
                repo.add("R010", wf.path, wf.job_line(job_id), f"job `{job_id}` has no `timeout-minutes`", "add one")
            for step in wf.steps(job):
                check_step(ctx, wf, step)
        for no, line in enumerate(wf.text.splitlines(), 1):
            m = USES_LINE_RE.match(line)
            if not m or m.group(1).startswith("./"):
                continue
            ref, comment = m.group(1), m.group(2) or ""
            if ref.startswith("docker://"):
                if "@sha256:" not in ref:
                    repo.add("R010", wf.path, no, f"`{ref}` is not pinned by digest", "pin @sha256:<digest>")
            elif not re.fullmatch(r"[\w.-]+/[\w./-]+@[0-9a-f]{40}", ref):
                repo.add(
                    "R010", wf.path, no, f"`{ref}` is not pinned to a full commit SHA", "pin @<40-hex sha> # vX.Y.Z"
                )
            elif not re.match(r"#\s*v?\d", comment):
                repo.add("R010", wf.path, no, f"`{ref}` has no `# vX.Y.Z` comment", "add the version comment")
    data_cmake = repo.text("tests/data/CMakeLists.txt")
    if data_cmake is not None:
        for m in re.finditer(r"\badd_test\s*\(", data_cmake):
            depth, i = 1, m.end()
            while i < len(data_cmake) and depth:
                depth += {"(": 1, ")": -1}.get(data_cmake[i], 0)
                i += 1
            if "--redact" not in data_cmake[m.start() : i]:
                no = data_cmake.count("\n", 0, m.start()) + 1
                repo.add("R010", "tests/data/CMakeLists.txt", no, "data test without `--redact`", "add `--redact`")


def check_step(ctx: Ctx, wf: Workflow, step: YMap) -> None:
    repo = ctx.repo
    uses = step.get("uses")
    with_ = step.get("with") if isinstance(step.get("with"), YMap) else YMap()
    if isinstance(uses, str):
        action = uses.split("@", 1)[0]
        if action == "actions/checkout" and str(with_.get("persist-credentials", "")).lower() != "false":
            repo.add(
                "R010", wf.path, step.line, "checkout without `persist-credentials: false`", "add it under `with:`"
            )
        if action == "prefix-dev/setup-pixi":
            key = re.sub(r"\s+", " ", str(with_.get("cache-key", "")))
            if key != SETUP_PIXI_CACHE_KEY:
                line = with_.kline("cache-key") if "cache-key" in with_ else step.line
                repo.add("R010", wf.path, line, f"setup-pixi cache-key must be `{SETUP_PIXI_CACHE_KEY}`", "use it")
        if action == "openai/codex-action" and "codex-version" not in with_:
            repo.add("R010", wf.path, step.line, "codex-action without `codex-version`", "pin the Codex CLI version")
    run = step.get("run")
    m = re.search(r"\$\{\{[^}]*\bgithub\.(event\.|head_ref\b)", run) if isinstance(run, str) else None
    if m:
        what = "github.event.*" if m.group(1) == "event." else "github.head_ref"
        repo.add(
            "R010",
            wf.path,
            step.kline("run"),
            f"`${{{{ {what} }}}}` inside `run:` (script injection)",
            "pass it through `env:` and use the shell variable",
        )


# ---------------------------------------------------------------------------------------------------------------------
# R011-R015: ratchet, governance, ADRs, links, allowed actions
# ---------------------------------------------------------------------------------------------------------------------


def query_number(text: str) -> int | None:
    m = re.search(r"\bQ?(\d+)\b", clean_cell(text), re.IGNORECASE)
    return int(m.group(1)) if m else None


def check_r011(ctx: Ctx) -> None:
    repo = ctx.repo
    status_text = repo.text("tests/data/clickbench_status.json")
    if status_text is None:
        return
    path = "tests/data/clickbench_status.json"
    try:
        raw = json.loads(status_text).get("pass", [])
        ratchet = {q for q in (query_number(str(v)) for v in raw) if q is not None}
    except (json.JSONDecodeError, AttributeError):
        repo.add("R011", path, 1, "not a JSON object with a `pass` list", "fix the JSON")
        return
    doc = repo.text("docs/sql-subset.md")
    if doc is None:
        repo.add("R011", "docs/sql-subset.md", 1, "missing (it must carry the ClickBench status table)", "add it")
        return
    for table in md_tables(doc):
        qcol, scol = table.column(r"query"), table.column(r"status|result")
        if qcol is None or scol is None:
            continue
        passing = set()
        for _, cells in table.rows:
            if max(qcol, scol) < len(cells):
                q, status = query_number(cells[qcol]), clean_cell(cells[scol]).lower()
                if q is not None and "pass" in status and "fail" not in status:
                    passing.add(q)
        if passing != ratchet:
            repo.add(
                "R011",
                "docs/sql-subset.md",
                table.line,
                f"ClickBench status table passes {sorted(passing)}, the ratchet ({path}) {sorted(ratchet)}",
                "update both in the same PR",
            )
        return
    repo.add("R011", "docs/sql-subset.md", 1, "no ClickBench status table (| Query | Status |)", "add it")


def norm_gov(path: str) -> str:
    p = path.strip().strip("`\"'")
    for suffix in ("/**", "/*"):
        if p.endswith(suffix):
            p = p[: -len(suffix) + 1]
    p = p.removeprefix("./")
    return "/" + p.lstrip("/")


def covers(pattern: str, target: str) -> bool:
    p, t = norm_gov(pattern), norm_gov(target)
    return p == t or p + "/" == t or (p.endswith("/") and t.startswith(p))


def check_coverage(ctx: Ctx, path: str, line: int, patterns: Iterable[str], targets: Iterable[str], what: str) -> None:
    patterns = list(patterns)
    for target in targets:
        if not any(covers(p, target) for p in patterns):
            ctx.repo.add("R012", path, line, f"{what} does not cover governance path `{target}`", f"add `{target}`")


def check_r012(ctx: Ctx) -> None:
    repo = ctx.repo
    owners = repo.text(".github/CODEOWNERS")
    if owners is not None:
        patterns = []
        for no, line in enumerate(owners.splitlines(), 1):
            parts = line.split("#", 1)[0].split()
            if not parts:
                continue
            patterns.append(parts[0])
            if len(parts) == 1:
                repo.add("R012", ".github/CODEOWNERS", no, f"`{parts[0]}` has no owner", "add @owners")
            elif not all(p.startswith("@") or "@" in p for p in parts[1:]):
                repo.add("R012", ".github/CODEOWNERS", no, "more than one pattern on a line", "one pattern per line")
        check_coverage(ctx, ".github/CODEOWNERS", 1, patterns, GOVERNANCE_PATHS + CODEOWNERS_EXTRA_PATHS, "CODEOWNERS")
    agents = repo.text("AGENTS.md")
    if agents is not None:
        section = md_section(agents, "Ask a human first")
        if section is None:
            repo.add("R012", "AGENTS.md", 1, "no `## Ask a human first` section", "add it with the governance paths")
        else:
            line, body = section
            tokens = [t.lstrip("(").rstrip(".,;:)") for t in re.split(r"[\s,]+", body.replace("`", " "))]
            tokens = [t for t in tokens if t and ("/" in t or "." in t)]
            check_coverage(ctx, "AGENTS.md", line, tokens, GOVERNANCE_PATHS, '"Ask a human first"')
    settings = repo.text(".claude/settings.json")
    if settings is not None:
        try:
            perms = json.loads(settings).get("permissions", {})
        except (json.JSONDecodeError, AttributeError):
            repo.add("R012", ".claude/settings.json", 1, "not a JSON object", "fix the JSON")
            return

        def edit_paths(kind: str) -> list[str]:
            out = []
            for rule in perms.get(kind, []) if isinstance(perms, dict) else []:
                m = re.fullmatch(r"(?:Edit|Write)\((.+)\)", str(rule))
                if m:
                    out.append(m.group(1))
            return out

        line = line_of(settings, r'"ask"')
        check_coverage(
            ctx,
            ".claude/settings.json",
            line,
            edit_paths("ask") + edit_paths("deny"),
            GOVERNANCE_PATHS,
            "ask/deny Edit",
        )
        check_coverage(
            ctx, ".claude/settings.json", line_of(settings, r'"deny"'), edit_paths("deny"), SETTINGS_DENY_PATHS, "deny"
        )


def check_r013(ctx: Ctx) -> None:
    repo = ctx.repo
    adrs = [p for p in repo.under("docs/adr/", (".md",)) if re.fullmatch(r"docs/adr/\d{4}-[^/]+\.md", p)]
    if not adrs:
        return
    index = repo.text("docs/adr/README.md")
    if index is None:
        repo.add("R013", "docs/adr/README.md", 1, "missing ADR index", "add docs/adr/README.md listing every ADR")
        return
    for adr in adrs:
        name = adr.rsplit("/", 1)[-1]
        if name not in index:
            repo.add("R013", "docs/adr/README.md", 1, f"ADR `{name}` is not listed", f"add a link to {name}")


def governance_exempt(token: str) -> bool:
    t = norm_gov(token).rstrip("/")
    return any(norm_gov(g).rstrip("/") == t for g in GOVERNANCE_PATHS + CODEOWNERS_EXTRA_PATHS)


def check_r014(ctx: Ctx) -> None:
    repo = ctx.repo
    root = repo.root.resolve()
    for path in ctx.markdown_docs():
        text = repo.text(path) or ""
        base = (repo.root / path).parent
        for no, line, fenced in md_lines(text):
            if fenced:
                continue
            targets = [m.group(1) for m in LINK_RE.finditer(line)] + [m.group(1) for m in REF_DEF_RE.finditer(line)]
            for target in targets:
                target = target.strip("<>")
                if re.match(r"^[a-z][a-z0-9+.-]*:", target, re.IGNORECASE) or target.startswith("#"):
                    continue
                target = re.split(r"[#?]", target, maxsplit=1)[0]
                if not target or re.search(r"[<*{$]", target):
                    continue
                resolved = (repo.root / target.lstrip("/")) if target.startswith("/") else (base / target)
                resolved = Path(os.path.normpath(resolved))
                if not resolved.is_relative_to(root) and not resolved.is_relative_to(repo.root):
                    continue  # GitHub-relative links such as ../../issues
                if not resolved.exists():
                    repo.add("R014", path, no, f"broken link `{target}`", "fix the path or remove the link")
            for span in code_spans(line):
                for word in span.split():
                    word = word.strip("\"'(),;")
                    word = re.sub(r":\d+(?::\d+)?$", "", word.split("#", 1)[0])
                    if not word or re.search(r"[<>*{}$…=@~]|://|\.\.\.", word) or word.startswith(("-", "//")):
                        continue
                    rel = word.removeprefix("/")  # `/cmake/` is repo-rooted (CODEOWNERS style)
                    first = rel.split("/", 1)[0]
                    if first not in repo.top_level or not ("/" in rel or "." in rel):
                        continue
                    if governance_exempt(rel) or repo.exists(rel):
                        continue
                    repo.add(
                        "R014",
                        path,
                        no,
                        f"`{word}` does not exist",
                        "fix the path; mention files that a later PR adds without backticks",
                    )


def action_allowed(ref: str, allowed: dict[str, Any]) -> bool:
    name, _, version = ref.partition("@")
    owner = name.split("/", 1)[0]
    if allowed.get("github_owned_allowed") and owner in GITHUB_OWNED_OWNERS:
        return True
    parts = name.split("/")
    candidates = {f"{name}@{version or 'x'}", f"{'/'.join(parts[:2])}@{version or 'x'}"}
    return any(fnmatch.fnmatchcase(c, p) for c in candidates for p in allowed.get("patterns_allowed", []))


def check_r015(ctx: Ctx) -> None:
    repo = ctx.repo
    raw = repo.text("tools/github/allowed-actions.json")
    if raw is None:
        return
    try:
        allowed = json.loads(raw)
    except json.JSONDecodeError as e:
        repo.add("R015", "tools/github/allowed-actions.json", e.lineno, "not valid JSON", "fix it")
        return
    fix = "add it to tools/github/allowed-actions.json (patterns_allowed) and re-run tools/github/apply-settings.sh"
    for wf in ctx.workflows:
        for no, line in enumerate(wf.text.splitlines(), 1):
            m = USES_LINE_RE.match(line)
            if not m or m.group(1).startswith(("./", "docker://")):
                continue
            ref = m.group(1)
            if not action_allowed(ref, allowed):
                repo.add("R015", wf.path, no, f"action `{ref.split('@', 1)[0]}` is not allowed", fix)
            parent = "/".join(ref.split("@", 1)[0].split("/")[:2])
            for nested in NESTED_ACTIONS.get(parent, ()):
                if not action_allowed(f"{nested}@x", allowed):
                    repo.add("R015", wf.path, no, f"`{parent}` runs `{nested}`, which is not allowed", fix)


CHECKS: dict[str, Callable[[Ctx], None]] = {
    "R001": check_r001,
    "R002": check_r002,
    "R003": check_r003,
    "R004": check_r004,
    "R005": check_r005,
    "R006": check_r006,
    "R007": check_r007,
    "R008": check_r008,
    "R009": check_r009,
    "R010": check_r010,
    "R011": check_r011,
    "R012": check_r012,
    "R013": check_r013,
    "R014": check_r014,
    "R015": check_r015,
}


def run(root: Path, only: Iterable[str] | None = None, use_git: bool = True) -> list[Finding]:
    repo = Repo(root, use_git=use_git)
    ctx = Ctx(repo)
    selected = set(only) if only else set(CHECKS)
    for rule, check in CHECKS.items():
        if rule in selected:
            check(ctx)
    return sorted({f for f in repo.findings if f.rule in selected})


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2], help="repository root")
    parser.add_argument("--only", default="", help="comma-separated rule ids, e.g. R001,R007")
    args = parser.parse_args(argv)
    only = [r.strip().upper() for r in args.only.split(",") if r.strip()]
    unknown = [r for r in only if r not in CHECKS]
    if unknown:
        parser.error(f"unknown rule(s): {', '.join(unknown)}")
    findings = run(args.root.resolve(), only)
    for f in findings:
        print(f)
    if findings:
        print(f"check_repo: {len(findings)} finding(s)", file=sys.stderr)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
