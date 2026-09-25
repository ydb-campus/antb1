"""Make tools/ci/coverage.py importable as `coverage_py` (not `coverage`, which is a well-known PyPI package)."""

import importlib.util
import sys
from pathlib import Path

_path = Path(__file__).resolve().parents[1] / "coverage.py"
_spec = importlib.util.spec_from_file_location("coverage_py", _path)
assert _spec is not None and _spec.loader is not None
coverage_py = importlib.util.module_from_spec(_spec)
sys.modules["coverage_py"] = coverage_py
_spec.loader.exec_module(coverage_py)
