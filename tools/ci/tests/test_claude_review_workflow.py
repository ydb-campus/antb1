"""The read-only Claude review starts only when a person applies its label to a PR."""

from pathlib import Path

import yaml


def test_claude_review_requires_an_explicit_label_request() -> None:
    path = Path(__file__).resolve().parents[3] / ".github" / "workflows" / "claude-review.yml"
    workflow = yaml.load(path.read_text(), Loader=yaml.BaseLoader)
    assert workflow["on"] == {"pull_request": {"types": ["labeled"]}}
    required = {"github.event.label.name == 'claude'", "github.event.sender.type == 'User'"}
    assert required.issubset(workflow["jobs"]["review"]["if"].split(" && "))
