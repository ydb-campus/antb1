"""Tests for tools/lint/sync_skills.py: .agents/skills (canonical) is mirrored to .claude/skills byte for byte."""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

import check_repo
import pytest
import sync_skills

SKILL = "---\nname: demo\ndescription: A demo skill.\n---\n\nRun `pixi run build`.\n"


def put(root: Path, rel: str, text: str, mtime: int | None = None) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    if mtime is not None:
        os.utime(path, ns=(mtime, mtime))
    return path


def r004(root: Path, use_git: bool = False) -> list[str]:
    return [str(f) for f in check_repo.run(root, only=["R004"], use_git=use_git)]


def test_copies_new_and_outdated_files(tmp_path: Path) -> None:
    put(tmp_path, ".agents/skills/demo/SKILL.md", SKILL, mtime=2_000_000_000)
    script = put(tmp_path, ".agents/skills/demo/scripts/run.sh", "#!/usr/bin/env bash\necho hi\n")
    script.chmod(0o755)
    put(tmp_path, ".claude/skills/demo/SKILL.md", "old\n", mtime=1_000_000_000)
    assert sync_skills.sync(tmp_path, use_git=False) == []
    copy = tmp_path / ".claude/skills/demo"
    assert (copy / "SKILL.md").read_bytes() == SKILL.encode()
    assert (copy / "scripts/run.sh").read_bytes() == script.read_bytes()
    assert os.access(copy / "scripts/run.sh", os.X_OK)
    assert r004(tmp_path) == []
    assert sync_skills.sync(tmp_path, use_git=False) == []  # idempotent


def test_keeps_a_copy_edited_after_the_canonical_file(tmp_path: Path) -> None:
    put(tmp_path, ".agents/skills/demo/SKILL.md", SKILL, mtime=1_000_000_000)
    put(tmp_path, ".claude/skills/demo/SKILL.md", SKILL + "edited copy\n", mtime=2_000_000_000)
    problems = sync_skills.sync(tmp_path, use_git=False)
    assert len(problems) == 1
    assert "move the edit to .agents/skills/demo/SKILL.md" in problems[0]
    assert (tmp_path / ".claude/skills/demo/SKILL.md").read_text(encoding="utf-8").endswith("edited copy\n")


def test_reports_untracked_extra_files_without_git(tmp_path: Path) -> None:
    put(tmp_path, ".agents/skills/demo/SKILL.md", SKILL)
    extra = put(tmp_path, ".claude/skills/new/SKILL.md", "---\nname: new\ndescription: d\n---\n")
    problems = sync_skills.sync(tmp_path, use_git=False)
    assert problems == [
        ".claude/skills/new/SKILL.md is not in .agents/skills: move it to .agents/skills/new/SKILL.md, then re-run"
    ]
    assert extra.is_file()


def test_nothing_to_do_without_skills(tmp_path: Path) -> None:
    assert sync_skills.sync(tmp_path, use_git=False) == []
    assert not (tmp_path / ".claude").exists()


@pytest.mark.skipif(shutil.which("git") is None, reason="needs git")
def test_deletes_tracked_copies_of_removed_skills(tmp_path: Path) -> None:
    def git(*args: str) -> None:
        subprocess.run(["git", "-C", str(tmp_path), *args], check=True, capture_output=True)

    git("init", "-q")
    put(tmp_path, ".gitignore", "*.local\n")
    put(tmp_path, ".agents/skills/demo/SKILL.md", SKILL)
    put(tmp_path, ".agents/skills/old/SKILL.md", SKILL.replace("demo", "old"))
    assert sync_skills.sync(tmp_path) == []
    git("add", "-A")
    (tmp_path / ".agents/skills/old/SKILL.md").unlink()
    put(tmp_path, ".claude/skills/demo/notes.local", "ignored by git\n")
    assert sync_skills.sync(tmp_path) == []
    assert not (tmp_path / ".claude/skills/old").exists()  # the tracked copy and its empty directory are gone
    assert (tmp_path / ".claude/skills/demo/notes.local").is_file()  # ignored files are left alone
    assert r004(tmp_path, use_git=True) == []  # git ignores notes.local


def test_main_exit_status(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    put(tmp_path, ".agents/skills/demo/SKILL.md", SKILL)
    assert sync_skills.main(["--root", str(tmp_path)]) == 0
    assert "sync_skills: .agents/skills/demo/SKILL.md -> .claude/skills/demo/SKILL.md" in capsys.readouterr().out
    put(tmp_path, ".claude/skills/stray.md", "x\n")
    assert sync_skills.main(["--root", str(tmp_path)]) == 1
    assert ".claude/skills/stray.md is not in .agents/skills" in capsys.readouterr().err
