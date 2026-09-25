"""Make tools/lint/check_repo.py importable as `check_repo`."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
