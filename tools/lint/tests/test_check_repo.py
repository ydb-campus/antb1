"""Tests for tools/lint/check_repo.py: a small, fully consistent repository, then one violation per test."""

from __future__ import annotations

import json
import textwrap
from pathlib import Path

import check_repo
import pytest

GOVERNANCE = list(check_repo.GOVERNANCE_PATHS)
SHA_CHECKOUT = "actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1"
SHA_PIXI = "prefix-dev/setup-pixi@d3f436a425481402e6a95a1d1fc10331c708cd9e # v0.10.2"
SHA_GREEN = "re-actors/alls-green@b5b5b37504aa4183270bd3d855c52a67f212be35 # v1.3.0"
SHA_CODEX = "openai/codex-action@86365089eb2b84e0a8fb0717b304f8bdcb13b20e # v1.12"
SHA_CODEQL = "github/codeql-action/init@2892aa5e19bbd11bc0cff5427e3b750a04d9e3c2 # v4.38.2"

BASELINE: dict[str, str] = {
    "pixi.toml": """
        #:schema https://pixi.prefix.dev/v0.81.0/schema/manifest/schema.json
        [workspace]
        name = "demo"
        channels = ["conda-forge"]
        platforms = ["linux-64"]
        requires-pixi = ">=0.81.0"

        [dependencies]
        cmake = "*"

        [feature.clang.tasks.configure]
        cmd = "cmake --preset dev"

        [feature.clang.tasks.build]
        description = "Incremental build into build/dev"
        cmd = "cmake --build --preset dev"
        depends-on = ["configure"]

        [feature.clang.tasks.ci]
        cmd = "cmake --workflow --preset ci"

        [feature.clang.tasks.check]
        depends-on = [{ task = "lint", environment = "lint" }, "ci"]

        [feature.gcc.tasks.ci-gcc]
        cmd = "cmake -E rm -rf build/ci-gcc && cmake --workflow --preset ci-gcc"

        [feature.lint.tasks.lint]
        cmd = "bash scripts/lint.sh"

        [feature.lint.tasks._helper]
        cmd = "true"

        [environments]
        default = { features = ["clang"] }
        gcc = { features = ["gcc"] }
        lint = { features = ["lint"], no-default-feature = true }
    """,
    "pixi.lock": """
        version: 7
        environments: {}
    """,
    "CMakePresets.json": json.dumps(
        {
            "version": 8,
            "configurePresets": [
                {"name": "base", "hidden": True, "binaryDir": "${sourceDir}/build/${presetName}"},
                {"name": "dev", "inherits": ["base"]},
                {"name": "ci", "inherits": ["base"]},
                {"name": "ci-gcc", "inherits": ["base"]},
            ],
            "buildPresets": [{"name": "dev", "configurePreset": "dev"}, {"name": "ci", "configurePreset": "ci"}],
            "testPresets": [{"name": "dev", "configurePreset": "dev"}],
            "workflowPresets": [{"name": "ci", "steps": []}, {"name": "ci-gcc", "steps": []}],
        },
        indent=2,
    ),
    "cmake/Antb1Modules.cmake": """
        # allow-list
        set(
          ANTB1_MODULES
          common
          sql
          plan
        )
        set(ANTB1_DEPS_common "")
        set(ANTB1_DEPS_sql "common")
        set(ANTB1_DEPS_plan "common;sql") # plan sees the AST
        set(ANTB1_EXT_common "")
        set(ANTB1_EXT_sql "")
        set(ANTB1_EXT_plan "Arrow::arrow_shared")
    """,
    "cmake/Antb1Testing.cmake": """
        # Test helpers. Labels (docs/testing.md, R008): unit integration
        # data setup.
        include_guard(GLOBAL)
    """,
    "src/common/include/antb1/common/x.h": """
        #pragma once
        #include <cstdint>
    """,
    "src/sql/parser.cc": """
        #include "antb1/common/x.h"
        #include "antb1/sql/parser.h"
    """,
    "src/sql/tests/parser_test.cc": """
        #include <gtest/gtest.h>

        #include "antb1/sql/parser.h"
    """,
    "src/plan/binder.cc": """
        #include <arrow/api.h>

        #include "antb1/common/x.h"
        #include "antb1/sql/parser.h"
    """,
    "src/plan/tests/binder_test.cc": """
        #include <parquet/arrow/writer.h>
    """,
    "scripts/lint.sh": """
        #!/usr/bin/env bash
        # Prints the environment/build/data status.
        root="${PIXI_PROJECT_ROOT:-$PWD}"
        ls "$root/build/dev" build/ci/junit.xml
        ctest --preset dev
    """,
    "scripts/agent-setup.sh": """
        #!/usr/bin/env bash
        PIXI_VERSION=0.81.0
        url="https://github.com/prefix-dev/pixi/releases/download/v${PIXI_VERSION}/pixi.tar.gz"
        conda_file=linux-64/pixi-0.81.0-hf01adef_0.conda
    """,
    "AGENTS.md": """
        # AGENTS.md

        Maps: [architecture](docs/architecture.md) · [ADRs](docs/adr/README.md) · [site](https://example.org/x).
        Only use `pixi run <task>`; plain prose such as pixi run anything is not a command.
        Install pixi 0.81.0 via `bash scripts/agent-setup.sh`; `prefix-dev/setup-pixi` v0.10.2 runs in CI.

        ## Commands

        | Goal | Command |
        |---|---|
        | Build | `pixi run configure` · `pixi run build` |
        | Gates | `pixi run check` · `ci` · `pixi run -e lint lint` · `pixi run -e lint pytest -q tools/lint` |

        ## Ask a human first

        `/.github/`, `/.githooks/`, `/.claude/`, `/.agents/`, AGENTS.md, CLAUDE.md, `pixi.toml`, `CMakePresets.json`,
        `cmake/`, `tools/github/**`, `tools/lint/**`, `tools/data/**`, `tools/ci/coverage_thresholds.json`,
        `.pre-commit-config.yaml`, `.clang-format`, `.clang-tidy`, `tests/.clang-tidy`, `scripts/ctest.sh`,
        `scripts/agent-setup.sh`, `tests/data/clickbench_status.json`, `scripts/lint.sh`, `scripts/fmt.sh`,
        `tools/sanitizers/`, `.markdownlint-cli2.yaml`, `_typos.toml`, `ruff.toml`, `.gersemirc`, `tombi.toml`.

        ## Code Review Rules

        - Report concrete problems in `src/sql/parser.cc:12` style.
    """,
    "CLAUDE.md": """
        @AGENTS.md

        # Claude Code notes
    """,
    ".github/copilot-instructions.md": """
        # Copilot

        Follow AGENTS.md.
    """,
    ".github/CODEOWNERS": "\n".join(
        f"{p} @Hor911 @lll-phill-lll" for p in [*GOVERNANCE, *check_repo.CODEOWNERS_EXTRA_PATHS]
    ),
    ".claude/settings.json": json.dumps(
        {
            "permissions": {
                "allow": ["Bash(pixi run build)", "Bash(pixi run ci *)"],
                "ask": [f"Edit({g}**)" if g.endswith("/") else f"Edit({g})" for g in GOVERNANCE]
                + ["Bash(pixi run -x *)"],
                "deny": ["Edit(/pixi.lock)"],
            }
        },
        indent=2,
    ),
    ".agents/skills/demo/SKILL.md": """
        ---
        name: demo
        description: A demo skill.
        ---
        Run `pixi run build`.
    """,
    ".claude/skills/demo/SKILL.md": """
        ---
        name: demo
        description: A demo skill.
        ---
        Run `pixi run build`.
    """,
    "docs/architecture.md": """
        # Architecture

        | Module | May depend on | External libraries |
        |---|---|---|
        | `common` | none | none |
        | `sql` | `common` | none |
        | `plan` | `common`, `sql` | Arrow |

        See [ADR 1](adr/0001-first.md#context), [agents](../AGENTS.md) and `src/plan/binder.cc`.
        Placeholders such as `src/<module>/tests/*_test.cc` and `build/<preset>` are ignored.

        ```bash
        pixi run --frozen -e gcc ci-gcc
        cat src/does/not/exist.cc
        ```
    """,
    "docs/testing.md": """
        # Testing

        | Label | Status |
        |---|---|
        | `unit` | used |
        | `integration` | later |
        | `data` | later |
        | `setup` | later |
    """,
    "docs/ci.md": """
        # CI

        The gcc-compat leg runs `pixi run ci-gcc`.
    """,
    "docs/sql-subset.md": """
        # SQL subset

        | Query | Status |
        |---|---|
        | Q0 | pass |
        | Q1 | fail |
    """,
    "docs/adr/README.md": """
        # ADRs

        - [0001](0001-first.md)
    """,
    "docs/adr/0001-first.md": """
        # 1. First
    """,
    "tests/data/clickbench_status.json": '{"pass": [0]}\n',
    "tests/data/CMakeLists.txt": """
        add_test(NAME data.q0 COMMAND antb1-data --redact
          --query 0)
    """,
    "tools/github/allowed-actions.json": json.dumps(
        {
            "github_owned_allowed": True,
            "verified_allowed": False,
            "patterns_allowed": ["prefix-dev/setup-pixi@*", "re-actors/alls-green@*", "openai/codex-action@*"],
        }
    ),
    ".github/workflows/ci.yml": f"""
        name: CI
        on:
          pull_request:
            branches: [main]
          merge_group:
        permissions: {{}}
        env:
          PIXI_VERSION: v0.81.0
        jobs:
          lint:
            name: lint (pixi run lint)
            runs-on: ubuntu-24.04
            timeout-minutes: 15
            permissions:
              contents: read
            steps:
              - uses: {SHA_CHECKOUT}
                with:
                  persist-credentials: false
              - uses: {SHA_PIXI}
                with:
                  pixi-version: ${{{{ env.PIXI_VERSION }}}}
                  cache-key: pixi-${{{{ hashFiles('pixi.lock') }}}}-
              - run: pixi run --frozen -e lint lint
          build:
            name: ${{{{ matrix.name }}}} (pixi run ${{{{ matrix.task }}}})
            runs-on: ubuntu-24.04
            timeout-minutes: 60
            strategy:
              matrix:
                include:
                  - {{ name: clang, task: ci, pixi_env: default, preset: ci }}
                  - {{ name: gcc-compat, task: ci-gcc, pixi_env: gcc, preset: ci-gcc }}
            env:
              PIXI_ENV: ${{{{ matrix.pixi_env }}}}
              TASK: ${{{{ matrix.task }}}}
              TITLE: ${{{{ github.event.pull_request.title }}}}
            steps:
              - name: Build and test
                run: |
                  echo "$TITLE"
                  pixi run --frozen -e "$PIXI_ENV" "$TASK"
              - run: cat build/${{{{ matrix.preset }}}}/junit.xml
          ci-ok:
            name: CI OK
            if: always()
            needs: [lint, build]
            runs-on: ubuntu-slim
            timeout-minutes: 5
            permissions: {{}}
            steps:
              - uses: {SHA_GREEN}
                with:
                  jobs: ${{{{ toJSON(needs) }}}}
    """,
    ".github/workflows/pr-title.yml": """
        name: PR title
        on:
          pull_request:
          merge_group:
        permissions: {}
        jobs:
          title:
            name: PR title
            runs-on: ubuntu-slim
            timeout-minutes: 5
            steps:
              - run: echo ok
    """,
}


def write(root: Path, files: dict[str, str]) -> None:
    for rel, content in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(textwrap.dedent(content).lstrip("\n"), encoding="utf-8")


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    write(tmp_path, BASELINE)
    return tmp_path


def findings(root: Path, rule: str) -> list[check_repo.Finding]:
    return check_repo.run(root, only=[rule], use_git=False)


def messages(root: Path, rule: str) -> str:
    return "\n".join(str(f) for f in findings(root, rule))


def edit(root: Path, rel: str, old: str, new: str) -> None:
    path = root / rel
    text = path.read_text(encoding="utf-8")
    assert old in text, f"{old!r} not in {rel}"
    path.write_text(text.replace(old, new), encoding="utf-8")


def append(root: Path, rel: str, text: str, dedent: bool = True) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as f:
        f.write(textwrap.dedent(text).lstrip("\n") if dedent else text)


# --- the baseline is clean -------------------------------------------------------------------------------------------


def test_baseline_has_no_findings(repo: Path) -> None:
    assert [str(f) for f in check_repo.run(repo, use_git=False)] == []


def test_main_prints_findings_and_exits_1(repo: Path, capsys: pytest.CaptureFixture[str]) -> None:
    assert check_repo.main(["--root", str(repo)]) == 0
    (repo / "pixi.lock").write_text("version: 6\n", encoding="utf-8")
    assert check_repo.main(["--root", str(repo), "--only", "R005"]) == 1
    out = capsys.readouterr().out
    assert out.startswith("pixi.lock:1: R005 line 1 must be `version: 7` (fix: ")


def test_missing_optional_files_are_skipped(tmp_path: Path) -> None:
    write(tmp_path, {"README.md": "# x\n"})
    assert check_repo.run(tmp_path, use_git=False) == []


# --- R001 ------------------------------------------------------------------------------------------------------------


def test_r001_default_feature_task(repo: Path) -> None:
    append(repo, "pixi.toml", '[tasks]\nhello = "echo hi"\n')
    out = messages(repo, "R001")
    assert "task `hello` is defined in the default feature" in out
    assert "task `hello` must exist in exactly one environment" in out


def test_r001_task_in_two_environments(repo: Path) -> None:
    edit(repo, "pixi.toml", 'gcc = { features = ["gcc"] }', 'gcc = { features = ["gcc", "clang"] }')
    assert "task `build` must exist in exactly one environment, found ['default', 'gcc']" in messages(repo, "R001")


def test_r001_unknown_task_in_docs(repo: Path) -> None:
    append(repo, "docs/ci.md", "Run `pixi run tests -R x` locally.\n")
    out = messages(repo, "R001")
    assert "docs/ci.md:4: R001 `pixi run tests` names no pixi task" in out


def test_r001_platform_variants_share_a_feature(repo: Path) -> None:
    append(repo, "pixi.toml", '[feature.clang.target.linux-64.tasks.fast]\ncmd = "a"\n')
    append(repo, "pixi.toml", '[feature.clang.target.osx-arm64.tasks.fast]\ncmd = "b"\n')
    assert findings(repo, "R001") == []
    append(repo, "pixi.toml", '[feature.gcc.tasks.fast]\ncmd = "c"\n')
    assert "task `fast` is defined in more than one feature (clang, gcc)" in messages(repo, "R001")


def test_r001_wrong_environment(repo: Path) -> None:
    append(repo, "AGENTS.md", "Also `pixi run -e gcc lint`.\n")
    assert "task `lint` is not in environment `gcc`" in messages(repo, "R001")


def test_r001_unknown_environment_and_env_command(repo: Path) -> None:
    append(repo, "docs/ci.md", "`pixi run -e nope lint` and `pixi run -e lint pre-commit run --all-files`\n")
    out = messages(repo, "R001")
    assert "no environment `nope`" in out
    assert "pre-commit" not in out


def test_r001_workflow_matrix_resolution(repo: Path) -> None:
    edit(repo, ".github/workflows/ci.yml", "task: ci-gcc, pixi_env: gcc", "task: ci-gcc, pixi_env: default")
    out = messages(repo, "R001")
    assert "task `ci-gcc` is not in environment `default`" in out
    edit(repo, ".github/workflows/ci.yml", "task: ci-gcc, pixi_env: default", "task: ci-gcc2, pixi_env: gcc")
    assert "`pixi run ci-gcc2` names no pixi task" in messages(repo, "R001")


def test_r001_depends_on(repo: Path) -> None:
    edit(repo, "pixi.toml", 'depends-on = ["configure"]', 'depends-on = ["no-such-task"]')
    assert "task `build` depends on unknown task `no-such-task`" in messages(repo, "R001")
    edit(repo, "pixi.toml", '"no-such-task"', '"lint"')
    assert "task `build` depends on `lint`, which is not in the same environment" in messages(repo, "R001")


def test_r001_settings_allow_list(repo: Path) -> None:
    edit(repo, ".claude/settings.json", "Bash(pixi run build)", "Bash(pixi run build2)")
    assert ".claude/settings.json" in messages(repo, "R001")


# --- R002 ------------------------------------------------------------------------------------------------------------


def test_r002_undocumented_task(repo: Path) -> None:
    edit(repo, "docs/ci.md", "`pixi run ci-gcc`", "the gcc task")
    out = messages(repo, "R002")
    assert "pixi.toml:25: R002 task `ci-gcc` is not documented" in out
    assert "_helper" not in out  # hidden tasks need no docs


def test_r002_skipped_without_docs(repo: Path) -> None:
    (repo / "AGENTS.md").unlink()
    (repo / "docs/ci.md").unlink()
    assert findings(repo, "R002") == []


# --- R003 ------------------------------------------------------------------------------------------------------------


def test_r003_claude_first_line(repo: Path) -> None:
    (repo / "CLAUDE.md").write_text("# Claude\n@AGENTS.md\n", encoding="utf-8")
    assert "CLAUDE.md:1: R003 line 1 must be `@AGENTS.md`" in messages(repo, "R003")


def test_r003_agents_size_and_review_section(repo: Path) -> None:
    edit(repo, "AGENTS.md", "## Code Review Rules", "## Review")
    append(repo, "AGENTS.md", "- filler\n" * 150)
    out = messages(repo, "R003")
    assert "no `## Code Review Rules` section" in out
    assert "lines (max 150)" in out


def test_r003_copilot_instructions(repo: Path) -> None:
    (repo / ".github/copilot-instructions.md").write_text("# Copilot\n" + "- x\n" * 45, encoding="utf-8")
    out = messages(repo, "R003")
    assert "lines (max 40)" in out
    assert "does not mention AGENTS.md" in out


# --- R004 ------------------------------------------------------------------------------------------------------------


def test_r004_copy_differs_and_frontmatter(repo: Path) -> None:
    edit(repo, ".claude/skills/demo/SKILL.md", "A demo skill.", "An edited copy.")
    edit(repo, ".agents/skills/demo/SKILL.md", "name: demo\n", "allowed-tools: Bash\n")
    out = messages(repo, "R004")
    assert ".claude/skills/demo/SKILL.md:1: R004 differs from .agents/skills" in out
    assert "frontmatter has no `name`" in out
    assert "`allowed-tools` in a shared skill" in out


def test_r004_missing_copy(repo: Path) -> None:
    write(repo, {".agents/skills/other/SKILL.md": "---\nname: other\ndescription: d\n---\n"})
    assert ".agents/skills/other/SKILL.md:1: R004 missing from .claude/skills" in messages(repo, "R004")


def test_r004_frontmatter_keys_name_and_location(repo: Path) -> None:
    for rel in (".agents/skills/demo/SKILL.md", ".claude/skills/demo/SKILL.md"):
        edit(repo, rel, "name: demo\n", "name: other\nmodel: opus\n")
    nested = "---\nname: deep\ndescription: d\n---\n"
    write(repo, {".agents/skills/x/deep/SKILL.md": nested, ".claude/skills/x/deep/SKILL.md": nested})
    out = messages(repo, "R004")
    assert "frontmatter key(s) model: a shared skill has only `name` and `description`" in out
    assert ".agents/skills/demo/SKILL.md:2: R004 `name: other` differs from the skill directory `demo`" in out
    assert ".agents/skills/x/deep/SKILL.md:1: R004 not at .agents/skills/<name>/SKILL.md" in out
    assert ".claude/skills/demo" not in out  # the copy is identical, so only the canonical file is reported


# --- R005 ------------------------------------------------------------------------------------------------------------


def test_r005_lock_version(repo: Path) -> None:
    (repo / "pixi.lock").write_text("version: 6\n", encoding="utf-8")
    assert "pixi.lock:1: R005 line 1 must be `version: 7`" in messages(repo, "R005")


def test_r005_version_drift(repo: Path) -> None:
    edit(repo, ".github/workflows/ci.yml", "PIXI_VERSION: v0.81.0", "PIXI_VERSION: v0.80.0")
    edit(repo, "scripts/agent-setup.sh", "PIXI_VERSION=0.81.0", "PIXI_VERSION=0.81.1")
    edit(repo, "AGENTS.md", "Install pixi 0.81.0", "Install pixi 0.79.2")
    out = messages(repo, "R005")
    assert ".github/workflows/ci.yml:8: R005 pixi version 0.80.0 != 0.81.0" in out
    assert "scripts/agent-setup.sh:2: R005 pixi version 0.81.1 != 0.81.0" in out
    assert "AGENTS.md:5: R005 pixi version 0.79.2 != 0.81.0" in out
    assert "0.10.2" not in out  # setup-pixi's own version is not a pixi version
    write(repo, {"docs/adr/0002-toolchain.md": "We moved from pixi 0.79.0 to pixi 0.81.0.\n"})
    assert "docs/adr" not in messages(repo, "R005")  # ADRs are history


def test_r005_setup_pixi_without_version(repo: Path) -> None:
    edit(repo, ".github/workflows/ci.yml", "pixi-version: ${{ env.PIXI_VERSION }}", "locked: true")
    assert "setup-pixi without `pixi-version`" in messages(repo, "R005")


def test_r005_requires_pixi(repo: Path) -> None:
    edit(repo, "pixi.toml", 'requires-pixi = ">=0.81.0"\n', "")
    assert "no `requires-pixi`" in messages(repo, "R005")


# --- R006 ------------------------------------------------------------------------------------------------------------


def test_r006_unknown_and_wrong_kind_presets(repo: Path) -> None:
    edit(repo, "pixi.toml", "cmake --preset dev", "cmake --preset devel")
    edit(repo, "scripts/lint.sh", "ctest --preset dev", "ctest --preset ci")
    out = messages(repo, "R006")
    assert "pixi.toml:12: R006 no configure preset `devel`" in out
    assert "scripts/lint.sh:5: R006 no test preset `ci`" in out


def test_r006_hidden_preset_and_build_dir(repo: Path) -> None:
    edit(repo, "pixi.toml", "cmake --preset dev", "cmake --preset base")
    edit(repo, "scripts/lint.sh", "build/ci/junit.xml", "build/cov/junit.xml")
    out = messages(repo, "R006")
    assert "configure preset `base` is hidden" in out
    assert "`build/cov` is not the build dir of a configure preset" in out
    assert "build/data" not in out  # prose ("environment/build/data") is not a path


def test_r006_matrix_presets(repo: Path) -> None:
    edit(repo, ".github/workflows/ci.yml", "pixi_env: gcc, preset: ci-gcc", "pixi_env: gcc, preset: ci-gc")
    out = messages(repo, "R006")
    assert ".github/workflows/ci.yml:43: R006 `build/ci-gc` is not the build dir of a configure preset" in out
    assert "build/ci`" not in out  # the other matrix item is fine


# --- R007 ------------------------------------------------------------------------------------------------------------


def test_r007_table_drift(repo: Path) -> None:
    edit(repo, "docs/architecture.md", "| `plan` | `common`, `sql` | Arrow |", "| `plan` | `common` | Arrow, Parquet |")
    edit(repo, "docs/architecture.md", "| `sql` | `common` | none |\n", "")
    out = messages(repo, "R007")
    assert "`plan` dependencies differ from the allow-list (common, sql)" in out
    assert "`plan` external libraries differ (Arrow)" in out
    assert "module `sql` is missing from the module table" in out


def test_r007_missing_table(repo: Path) -> None:
    (repo / "docs/architecture.md").write_text("# Architecture\n", encoding="utf-8")
    assert "no module table" in messages(repo, "R007")


def test_r007_include_scan(repo: Path) -> None:
    append(
        repo, "src/sql/parser.cc", '#include "antb1/plan/binder.h"\n#include <arrow/api.h>\n\n#include <CLI11.hpp>\n'
    )
    append(repo, "src/sql/tests/parser_test.cc", "#include <parquet/api/reader.h>\n")
    append(repo, "src/plan/binder.cc", "#include <duckdb.h>\n#include <CLI/CLI.hpp>\n")
    out = messages(repo, "R007")
    assert "src/sql/parser.cc:3: R007 module `sql` includes `antb1/plan/binder.h`" in out
    assert "src/sql/parser.cc:4: R007 module `sql` includes `arrow/api.h` but does not link that library" in out
    assert "src/sql/parser.cc:6: R007 module `sql` includes `CLI11.hpp`" in out  # line of the include, not the blank
    assert "src/sql/tests/parser_test.cc:4: R007 module `sql` includes `parquet/api/reader.h`" in out
    assert "src/plan/binder.cc:5: R007 `duckdb.h` in src/" in out
    assert "src/plan/binder.cc:6: R007 module `plan` includes `CLI/CLI.hpp`" in out
    assert "binder_test.cc" not in out  # tests of Arrow modules may use more libraries


def test_r007_transitive_includes_are_allowed(repo: Path) -> None:
    edit(repo, "cmake/Antb1Modules.cmake", 'set(ANTB1_DEPS_plan "common;sql")', 'set(ANTB1_DEPS_plan "sql")')
    edit(repo, "docs/architecture.md", "| `plan` | `common`, `sql` |", "| `plan` | `sql` |")
    assert findings(repo, "R007") == []  # plan -> sql -> common (no src/sql/CMakeLists.txt: every dep is public)


def test_r007_private_deps_of_deps_are_not_visible(repo: Path) -> None:
    edit(repo, "cmake/Antb1Modules.cmake", 'set(ANTB1_DEPS_plan "common;sql")', 'set(ANTB1_DEPS_plan "sql")')
    edit(repo, "docs/architecture.md", "| `plan` | `common`, `sql` |", "| `plan` | `sql` |")
    write(repo, {"src/sql/CMakeLists.txt": "antb1_add_module(\n  sql\n  SOURCES parser.cc\n  PRIVATE_DEPS common\n)\n"})
    out = messages(repo, "R007")
    assert "src/plan/binder.cc:3: R007 module `plan` includes `antb1/common/x.h`; it may include" in out
    assert "src/sql/" not in out
    edit(repo, "src/sql/CMakeLists.txt", "PRIVATE_DEPS common", "PUBLIC_DEPS common # re-exported")
    assert findings(repo, "R007") == []


# --- R008 ------------------------------------------------------------------------------------------------------------


def test_r008_labels_drift(repo: Path) -> None:
    edit(repo, "docs/testing.md", "| `setup` | later |", "| `fuzz` | later |")
    out = messages(repo, "R008")
    assert "ctest label `setup` is not in the labels table" in out
    assert "`fuzz` is not a ctest label" in out


def test_r008_missing_comment_or_table(repo: Path) -> None:
    (repo / "docs/testing.md").write_text("# Testing\n", encoding="utf-8")
    assert "no labels table" in messages(repo, "R008")
    (repo / "cmake/Antb1Testing.cmake").write_text("include_guard(GLOBAL)\n", encoding="utf-8")
    assert "no `Labels (...): a b c.` comment" in messages(repo, "R008")


# --- R009 ------------------------------------------------------------------------------------------------------------


def test_r009_data_and_large_files(repo: Path) -> None:
    write(repo, {"tests/x/hits.parquet": "PAR1", "tools/data/queries.sql": "SELECT 1;", "q07.csv": "1"})
    write(repo, {"tests/x/HITS_1.Parquet": "PAR1", "tests/x/hits.csv": "1"})
    (repo / "big.bin").write_bytes(b"0" * (1024 * 1024 + 1))
    (repo / "pixi.lock").write_text("version: 7\n" + "#" * (2 * 1024 * 1024), encoding="utf-8")
    paths = {f.path for f in findings(repo, "R009")}
    assert paths == {"tests/x/hits.parquet", "tests/x/HITS_1.Parquet", "tools/data/queries.sql", "q07.csv", "big.bin"}


# --- R010 ------------------------------------------------------------------------------------------------------------


def test_r010_workflow_hardening(repo: Path) -> None:
    wf = ".github/workflows/ci.yml"
    edit(repo, wf, "permissions: {}\nenv:", "permissions: read-all\nenv:")
    edit(repo, wf, "  pull_request:\n    branches: [main]", "  pull_request_target:")
    edit(repo, wf, "    timeout-minutes: 15\n", "")
    edit(repo, wf, "persist-credentials: false", "fetch-depth: 0")
    edit(repo, wf, "cache-key: pixi-${{ hashFiles('pixi.lock') }}-", "cache-key: pixi-")
    edit(repo, wf, SHA_GREEN, "re-actors/alls-green@release/v1")
    edit(repo, wf, 'echo "$TITLE"', 'echo "${{ github.event.pull_request.title }}"')
    out = messages(repo, "R010")
    assert "top-level permissions must be `{}`" in out
    assert "`pull_request_target` trigger" in out
    assert "job `lint` has no `timeout-minutes`" in out
    assert "checkout without `persist-credentials: false`" in out
    assert "setup-pixi cache-key must be" in out
    assert "`re-actors/alls-green@release/v1` is not pinned to a full commit SHA" in out
    assert "`${{ github.event.* }}` inside `run:`" in out


def test_r010_missing_permissions_comment_and_merge_group(repo: Path) -> None:
    edit(repo, ".github/workflows/pr-title.yml", "permissions: {}\n", "")
    edit(repo, ".github/workflows/pr-title.yml", "  merge_group:\n", "")
    edit(repo, ".github/workflows/ci.yml", SHA_GREEN, SHA_GREEN.split(" #")[0])
    out = messages(repo, "R010")
    assert "pr-title.yml:1: R010 no top-level `permissions: {}`" in out
    assert "no `merge_group` trigger" in out
    assert "has no `# vX.Y.Z` comment" in out


def test_r010_head_ref_in_run(repo: Path) -> None:
    edit(repo, ".github/workflows/ci.yml", 'echo "$TITLE"', 'echo "${{ github.head_ref }}"')
    assert "ci.yml:40: R010 `${{ github.head_ref }}` inside `run:` (script injection)" in messages(repo, "R010")


def test_r010_data_tests_redact(repo: Path) -> None:
    append(repo, "tests/data/CMakeLists.txt", "add_test(NAME data.q1 COMMAND antb1-data --query 1)\n")
    assert "tests/data/CMakeLists.txt:3: R010 data test without `--redact`" in messages(repo, "R010")


def test_r010_codex_version(repo: Path) -> None:
    append(
        repo,
        ".github/workflows/pr-title.yml",
        f"      - uses: {SHA_CODEX}\n        with:\n          prompt-file: p.md\n",
        dedent=False,
    )
    assert "codex-action without `codex-version`" in messages(repo, "R010")


# --- R011 ------------------------------------------------------------------------------------------------------------


def test_r011_ratchet_drift(repo: Path) -> None:
    (repo / "tests/data/clickbench_status.json").write_text('{"pass": [0, 1]}', encoding="utf-8")
    assert "passes [0], the ratchet (tests/data/clickbench_status.json) [0, 1]" in messages(repo, "R011")


def test_r011_ratchet_commit_matches_the_lock(repo: Path) -> None:
    commit, other = "a" * 40, "b" * 40
    (repo / "tools/data").mkdir(parents=True)
    (repo / "tools/data/clickbench.lock").write_text(
        f"# pins\nqueries.sql {'0' * 64} 10 https://example.org/ClickBench/{commit}/duckdb-parquet/queries.sql\n",
        encoding="utf-8",
    )
    status = repo / "tests/data/clickbench_status.json"
    status.write_text(f'{{"clickbench_commit": "{other}", "pass": [0]}}', encoding="utf-8")
    assert f"`clickbench_commit` {other} is not the queries.sql commit {commit}" in messages(repo, "R011")
    status.write_text(f'{{"clickbench_commit": "{commit}", "pass": [0]}}', encoding="utf-8")
    assert findings(repo, "R011") == []


def test_r011_skipped_without_ratchet(repo: Path) -> None:
    (repo / "tests/data/clickbench_status.json").unlink()
    (repo / "docs/sql-subset.md").write_text("# SQL\n", encoding="utf-8")
    assert findings(repo, "R011") == []


# --- R012 ------------------------------------------------------------------------------------------------------------


def test_r012_codeowners(repo: Path) -> None:
    edit(repo, ".github/CODEOWNERS", "/tools/lint/ @Hor911 @lll-phill-lll\n", "")
    edit(repo, ".github/CODEOWNERS", "/docs/adr/ @Hor911 @lll-phill-lll", "/docs/adr/ /docs/x/ @Hor911")
    out = messages(repo, "R012")
    assert "CODEOWNERS does not cover governance path `/tools/lint/`" in out
    assert "more than one pattern on a line" in out


def test_r012_agents_md(repo: Path) -> None:
    edit(repo, "AGENTS.md", "`cmake/`,", "`cmake/Antb1Modules.cmake`,")
    edit(repo, "AGENTS.md", "`.clang-format`, ", "")
    out = messages(repo, "R012")
    assert '"Ask a human first" does not cover governance path `/cmake/`' in out
    assert '"Ask a human first" does not cover governance path `/.clang-format`' in out


def test_r012_settings(repo: Path) -> None:
    edit(repo, ".claude/settings.json", '"Edit(/scripts/ctest.sh)",', "")
    edit(repo, ".claude/settings.json", '"Edit(/pixi.lock)"', '"Edit(/other)"')
    out = messages(repo, "R012")
    assert "ask/deny Edit does not cover governance path `/scripts/ctest.sh`" in out
    assert "deny does not cover governance path `/pixi.lock`" in out


def test_r012_settings_write_rules_do_not_count(repo: Path) -> None:
    edit(repo, ".claude/settings.json", '"Edit(/cmake/**)"', '"Write(/cmake/**)"')
    assert "ask/deny Edit does not cover governance path `/cmake/`" in messages(repo, "R012")


@pytest.mark.parametrize(
    "rule",
    [
        "Bash",
        "Bash(*)",
        "Bash(pixi *)",
        "Bash(pixi:*)",
        "Bash(pixi run *)",
        "Bash(pixi run:*)",
        "Bash(pixi run -e lint *)",
        "Bash(pixi run --frozen *)",
        "Bash(pixi run -x python)",
        "Bash(pixi run --manifest-path=x.toml build)",
        "Bash(pixi exec cmake)",
    ],
)
def test_r012_settings_broad_allow_rule(repo: Path, rule: str) -> None:
    edit(repo, ".claude/settings.json", '"Bash(pixi run build)"', json.dumps(rule))
    assert f"allow rule `{rule}` approves arbitrary commands" in messages(repo, "R012")


@pytest.mark.parametrize(
    "rule",
    [
        "Bash(pixi run test *)",
        "Bash(pixi run --frozen -e default test)",
        "Bash(pixi install --locked -e *)",
        "Bash(pixi task list)",
        "Bash(git diff *)",
    ],
)
def test_r012_settings_specific_allow_rule(repo: Path, rule: str) -> None:
    edit(repo, ".claude/settings.json", '"Bash(pixi run build)"', json.dumps(rule))
    assert findings(repo, "R012") == []


# --- R013 ------------------------------------------------------------------------------------------------------------


def test_r013_adr_index(repo: Path) -> None:
    write(repo, {"docs/adr/0002-second.md": "# 2. Second\n"})
    assert "ADR `0002-second.md` is not listed" in messages(repo, "R013")
    (repo / "docs/adr/README.md").unlink()
    assert "missing ADR index" in messages(repo, "R013")


# --- R014 ------------------------------------------------------------------------------------------------------------


def test_r014_links_and_paths(repo: Path) -> None:
    append(
        repo,
        "docs/testing.md",
        """
        See [missing](missing.md), [root](/docs/nope.md), [ok](../AGENTS.md#commands) and [issues](../../issues).
        Files: `src/sql/nope.cc`, `docs/ci.md`, `tools/data/x.lock`, `tools/data/`, `unknown/x.md`, `x.y`.
        """,
    )
    out = messages(repo, "R014")
    assert "docs/testing.md:9: R014 broken link `missing.md`" in out
    assert "broken link `/docs/nope.md`" in out
    assert "`src/sql/nope.cc` does not exist" in out
    assert "`tools/data/x.lock` does not exist" in out  # only the governance paths themselves may be planned
    for fine in ("../AGENTS.md#commands", "../../issues", "docs/ci.md", "tools/data/", "unknown/x.md", "x.y"):
        assert f"`{fine}`" not in out
    assert "does/not/exist" not in out  # fenced code is not checked


def test_r014_rooted_paths_and_fragments(repo: Path) -> None:
    append(repo, "docs/ci.md", "See `/src/nope.cc`, `/cmake/`, `/usr/bin/env`, `docs/ci.md#ci` and `docs/nope.md#x`.\n")
    out = messages(repo, "R014")
    assert "docs/ci.md:4: R014 `/src/nope.cc` does not exist" in out
    assert "`docs/nope.md` does not exist" in out
    for fine in ("/cmake/", "/usr/bin/env", "docs/ci.md"):
        assert f"`{fine}`" not in out


def test_r014_governance_paths_may_be_planned(repo: Path) -> None:
    append(repo, "CLAUDE.md", "Ask before editing `tools/ci/coverage_thresholds.json` or `.agents/skills/`.\n")
    assert findings(repo, "R014") == []


# --- R015 ------------------------------------------------------------------------------------------------------------


def test_r015_allowed_actions(repo: Path) -> None:
    edit(repo, "tools/github/allowed-actions.json", '"re-actors/alls-green@*", ', "")
    append(repo, ".github/workflows/pr-title.yml", f"      - uses: {SHA_CODEX}\n      - uses: {SHA_CODEQL}\n", False)
    out = messages(repo, "R015")
    assert "action `re-actors/alls-green` is not allowed" in out
    assert "codex-action" not in out  # allowed, and its nested actions/setup-node is GitHub-owned
    assert "codeql-action" not in out


def test_r015_nested_action(repo: Path) -> None:
    allowed = '"openai/codex-action@*", "actions/checkout@*"'
    edit(repo, "tools/github/allowed-actions.json", '"github_owned_allowed": true', '"github_owned_allowed": false')
    edit(repo, "tools/github/allowed-actions.json", '"openai/codex-action@*"', allowed)
    append(repo, ".github/workflows/pr-title.yml", f"      - uses: {SHA_CODEX}\n", False)
    out = messages(repo, "R015")
    assert "actions/checkout" not in out
    assert "`openai/codex-action` runs `actions/setup-node`, which is not allowed" in out
