"""Tests for tools/ci/filter_sarif.py (run by `pixi run lint`)."""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import filter_sarif


def result(uri: str | None) -> dict:
    if uri is None:
        return {"ruleId": "r", "message": {"text": "no location"}}
    return {
        "ruleId": "r",
        "message": {"text": uri},
        "locations": [{"physicalLocation": {"artifactLocation": {"uri": uri, "uriBaseId": "%SRCROOT%"}}}],
    }


def document(*uris: str | None) -> dict:
    return {
        "version": "2.1.0",
        "runs": [{"tool": {"driver": {"name": "CodeQL"}}, "results": [result(u) for u in uris]}],
    }


def uris(doc: dict) -> list[str | None]:
    return [filter_sarif.primary_uri(r) for r in doc["runs"][0]["results"]]


def test_drops_only_matching_results() -> None:
    doc = document(
        "src/sql/parser.cc",
        ".pixi/envs/gcc/lib/gcc/x86_64-conda-linux-gnu/15.3.0/include/c++/bits/unicode.h",
        "build/codeql/generated.cc",
        None,
        "tools/fixturegen/fixtures.cc",
    )
    assert filter_sarif.filter_document(doc, [".pixi/*", "build/*"]) == (3, 2)
    assert uris(doc) == ["src/sql/parser.cc", None, "tools/fixturegen/fixtures.cc"]


def test_absolute_and_file_uris_match_inside_the_checkout() -> None:
    doc = document("file:///home/runner/work/antb1/antb1/.pixi/envs/gcc/include/c++/format", "/abs/src/x.cc")
    assert filter_sarif.filter_document(doc, [".pixi/*"]) == (1, 1)
    assert uris(doc) == ["/abs/src/x.cc"]


def test_a_source_directory_named_like_a_pattern_prefix_is_kept() -> None:
    doc = document("src/pixi/x.cc", "docs/build.md")
    assert filter_sarif.filter_document(doc, [".pixi/*", "build/*"]) == (2, 0)


def test_runs_without_results_are_untouched() -> None:
    doc = {"version": "2.1.0", "runs": [{"tool": {"driver": {"name": "CodeQL"}}}]}
    assert filter_sarif.filter_document(doc, [".pixi/*"]) == (0, 0)
    assert "results" not in doc["runs"][0]


def test_cli_rewrites_every_sarif_file_in_a_directory(tmp_path: Path, capsys) -> None:
    (tmp_path / "cpp.sarif").write_text(json.dumps(document("src/a.cc", ".pixi/b.h")), encoding="utf-8")
    (tmp_path / "actions.sarif").write_text(json.dumps(document(".github/workflows/ci.yml")), encoding="utf-8")
    (tmp_path / "notes.txt").write_text("not sarif", encoding="utf-8")
    assert filter_sarif.main([str(tmp_path), "--exclude", ".pixi/*"]) == 0
    cpp = json.loads((tmp_path / "cpp.sarif").read_text(encoding="utf-8"))
    assert uris(cpp) == ["src/a.cc"]
    out = capsys.readouterr().out
    assert "cpp.sarif: kept 1 result(s), dropped 1" in out
    assert "actions.sarif: kept 1 result(s), dropped 0" in out


def test_cli_errors(tmp_path: Path) -> None:
    assert filter_sarif.main([str(tmp_path), "--exclude", ".pixi/*"]) == 1  # no *.sarif files
    bad = tmp_path / "bad.sarif"
    bad.write_text("{", encoding="utf-8")
    assert filter_sarif.main([str(bad), "--exclude", ".pixi/*"]) == 1
