#!/usr/bin/env python3
"""Mirror .agents/skills (canonical; Codex and Copilot read it) to .claude/skills (Claude Code reads it), byte for byte.

Run by `pixi run fmt` (scripts/fmt.sh) and by the Claude Code PostToolUse hook after a skill edit; `pixi run lint`
(check_repo.py R004) fails while the copy differs. Edit skills only under .agents/skills.

- A copy that is missing, or that differs from its canonical file and is not newer than it, is (re)written.
- A copy that differs and is newer than its canonical file was edited in the wrong place: it is kept and reported.
- A file only in .claude/skills is deleted when git tracks it (the copy of a removed skill); an untracked one is
  kept and reported, because it may be new work that belongs in .agents/skills.

Exit status: 0 when the copy is in sync afterwards, 1 when a reported file needs a manual decision.

Usage: python tools/lint/sync_skills.py [--root DIR]
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

CANONICAL = ".agents/skills"
COPY = ".claude/skills"
SKIP_DIRS = frozenset({"__pycache__", ".pytest_cache", ".ruff_cache"})
SKIP_FILES = frozenset({".DS_Store"})


def git_list(root: Path, prefix: str, untracked: bool) -> set[str] | None:
    """Paths under `prefix` (relative to it) that git tracks, plus untracked non-ignored ones; None without git."""
    cmd = ["git", "-C", str(root), "ls-files", "-z", "--cached"]
    if untracked:
        cmd += ["--others", "--exclude-standard"]
    try:
        out = subprocess.run([*cmd, "--", prefix], capture_output=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    names = (n for n in out.decode("utf-8", "replace").split("\0") if n)
    return {n.removeprefix(prefix + "/") for n in names}


def walk(base: Path) -> set[str]:
    found: set[str] = set()
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        rel = Path(dirpath).relative_to(base)
        found.update((rel / f).as_posix() for f in filenames if f not in SKIP_FILES)
    return found


def files_under(root: Path, prefix: str, use_git: bool) -> set[str]:
    """The files check_repo.py sees under `prefix`: git-known (tracked + untracked, not ignored) when possible."""
    listed = git_list(root, prefix, untracked=True) if use_git else None
    names = listed if listed is not None else walk(root / prefix)
    return {n for n in names if (root / prefix / n).is_file()}


def remove_empty_dirs(path: Path, stop: Path) -> None:
    while path != stop and path.is_dir() and not any(path.iterdir()):
        path.rmdir()
        path = path.parent


def sync(root: Path, use_git: bool = True) -> list[str]:
    """Mirrors the skills; returns the problems that need a manual decision (empty when the copy is in sync)."""
    src_dir, dst_dir = root / CANONICAL, root / COPY
    wanted = files_under(root, CANONICAL, use_git)
    present = files_under(root, COPY, use_git) if dst_dir.is_dir() else set()
    tracked = (git_list(root, COPY, untracked=False) if use_git else None) or set()
    problems: list[str] = []
    for rel in sorted(wanted):
        src, dst = src_dir / rel, dst_dir / rel
        if dst.is_file():
            if src.read_bytes() == dst.read_bytes():
                continue
            if dst.stat().st_mtime_ns > src.stat().st_mtime_ns:
                problems.append(
                    f"{COPY}/{rel} was edited after {CANONICAL}/{rel}: move the edit to {CANONICAL}/{rel} "
                    "(the canonical file) or delete the copy, then re-run"
                )
                continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        shutil.copymode(src, dst)
        print(f"sync_skills: {CANONICAL}/{rel} -> {COPY}/{rel}")
    for rel in sorted(present - wanted):
        dst = dst_dir / rel
        if rel in tracked:
            dst.unlink()
            remove_empty_dirs(dst.parent, dst_dir)
            print(f"sync_skills: removed {COPY}/{rel} (not in {CANONICAL})")
        else:
            problems.append(f"{COPY}/{rel} is not in {CANONICAL}: move it to {CANONICAL}/{rel}, then re-run")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2], help="repository root")
    args = parser.parse_args(argv)
    problems = sync(args.root.resolve())
    for p in problems:
        print(f"sync_skills: {p}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
