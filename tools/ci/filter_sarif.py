#!/usr/bin/env python3
"""Drop SARIF results that are located in third-party code before they are uploaded to code scanning.

CodeQL's C++ extractor traces every header the compiler reads, including the toolchain's libstdc++ headers under
`.pixi/`, and reports results there that we cannot act on. For compiled languages the CodeQL config's
`paths-ignore` does not apply, so `.github/workflows/codeql.yml` runs the analysis with `upload: never`, filters
the SARIF with this script and uploads the rest with `upload-sarif`.

Usage: python3 tools/ci/filter_sarif.py <file.sarif | directory> ... --exclude '.pixi/*' [--exclude 'build/*']
Rewrites each SARIF file in place (a directory means every *.sarif file in it) and prints what it dropped.
Patterns use fnmatch syntax (`*` also matches `/`) and match the result's primary location URI, relative to the
repository root. Standard library only: it runs on a bare GitHub runner.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import sys
from pathlib import Path


def primary_uri(result: dict) -> str | None:
    """The URI of a result's first location, without a file:// scheme, or None."""
    locations = result.get("locations") or []
    if not locations:
        return None
    uri = locations[0].get("physicalLocation", {}).get("artifactLocation", {}).get("uri")
    if not isinstance(uri, str):
        return None
    return uri.removeprefix("file://")


def excluded(uri: str | None, patterns: list[str]) -> bool:
    """True if the URI matches a pattern, relative (`.pixi/x`) or anywhere inside an absolute path."""
    if uri is None:
        return False
    return any(fnmatch.fnmatchcase(uri, p) or fnmatch.fnmatchcase(uri, "*/" + p) for p in patterns)


def filter_document(doc: dict, patterns: list[str]) -> tuple[int, int]:
    """Remove excluded results from every run of a SARIF document; returns (kept, dropped)."""
    kept = dropped = 0
    for run in doc.get("runs", []):
        results = run.get("results")
        if not isinstance(results, list):
            continue
        remaining = [r for r in results if not excluded(primary_uri(r), patterns)]
        dropped += len(results) - len(remaining)
        kept += len(remaining)
        run["results"] = remaining
    return kept, dropped


def sarif_files(paths: list[Path]) -> list[Path]:
    files: list[Path] = []
    for p in paths:
        if p.is_dir():
            files.extend(sorted(p.glob("*.sarif")))
        else:
            files.append(p)
    return files


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paths", nargs="+", type=Path, help="SARIF files or directories of *.sarif files")
    parser.add_argument("--exclude", action="append", required=True, help="fnmatch pattern of URIs to drop")
    args = parser.parse_args(argv)
    files = sarif_files(args.paths)
    if not files:
        print("filter_sarif: no SARIF files found", file=sys.stderr)
        return 1
    for path in files:
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as e:
            print(f"filter_sarif: cannot read {path}: {e}", file=sys.stderr)
            return 1
        kept, dropped = filter_document(doc, args.exclude)
        path.write_text(json.dumps(doc), encoding="utf-8")
        print(f"filter_sarif: {path}: kept {kept} result(s), dropped {dropped} in {', '.join(args.exclude)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
