"""Grok reviews every ready same-repository pull request and posts one advisory comment."""

import importlib.util
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("grok_review", REPO / ".github" / "scripts" / "grok_review.py")
assert SPEC is not None and SPEC.loader is not None
grok_review = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(grok_review)


def test_grok_review_runs_on_every_ready_pull_request() -> None:
    workflow = yaml.load((REPO / ".github" / "workflows" / "grok-review.yml").read_text(), Loader=yaml.BaseLoader)
    assert workflow["on"] == {"pull_request": {"types": ["opened", "ready_for_review", "reopened"]}}
    assert workflow["permissions"] == {}
    job = workflow["jobs"]["review"]
    condition = " ".join(job["if"].split())
    assert "github.event.pull_request.draft == false" in condition
    assert "github.event.pull_request.head.repo.full_name == github.repository" in condition
    assert "github.event.pull_request.user.login == 'Copilot'" in condition
    assert job["permissions"] == {"contents": "read"}
    steps = {step["id"]: step for step in job["steps"] if "id" in step}
    assert 'git show "$BASE_SHA:.github/scripts/grok_review.py"' in steps["material"]["run"]
    assert 'python3 -I "$RUNNER_TEMP/grok_review.py"' in steps["grok"]["run"]
    post = workflow["jobs"]["post"]
    assert post["permissions"] == {"pull-requests": "write"}
    assert post["needs"] == "review"


def test_extract_text_reads_a_responses_message() -> None:
    text = grok_review.extract_text(
        {
            "output": [
                {"type": "reasoning"},
                {
                    "type": "message",
                    "content": [{"type": "output_text", "text": "**Grok review:** no problems found"}],
                },
            ]
        }
    )
    assert text == "**Grok review:** no problems found"


def test_user_message_truncates_the_patch() -> None:
    message = grok_review.user_message("prompt", "rules", "a.cc", "x" * (grok_review.MAX_PATCH + 10))
    assert "truncated to 100000 characters" in message
    assert "x" * (grok_review.MAX_PATCH + 1) not in message
