#!/usr/bin/env python3
"""One advisory Grok review of a pull request diff (docs/agents.md, docs/ci.md).

Standard library only. The workflow passes the prompt and AGENTS.md from the base commit, the changed-file list and
the patch, and XAI_API_KEY. This posts nothing: it writes the comment body to $GITHUB_OUTPUT as `message`.
"""

from __future__ import annotations

import json
import os
import secrets
import sys
import urllib.error
import urllib.request
from pathlib import Path

API_URL = "https://api.x.ai/v1/responses"
MODEL = "grok-4.7"
MAX_PATCH = 100_000
MAX_COMMENT = 60_000


def user_message(prompt: str, rules: str, names: str, patch: str) -> str:
    note = ""
    if len(patch) > MAX_PATCH:
        patch = patch[:MAX_PATCH]
        note = "\n\nThe patch above is truncated to 100000 characters. Review only what is shown, and say so.\n"
    return (
        f"{prompt.rstrip()}\n\n"
        "## AGENTS.md from the base commit\n\n"
        f"{rules.rstrip()}\n\n"
        "## Changed files\n\n"
        f"{names.rstrip()}\n\n"
        "## Diff\n\n"
        f"{patch}{note}"
    )


def extract_text(payload: dict) -> str:
    direct = payload.get("output_text")
    if isinstance(direct, str) and direct.strip():
        return direct.strip()
    parts: list[str] = []
    for item in payload.get("output") or []:
        if not isinstance(item, dict):
            continue
        if item.get("type") == "output_text" and isinstance(item.get("text"), str):
            parts.append(item["text"])
        if item.get("type") != "message":
            continue
        for block in item.get("content") or []:
            if isinstance(block, dict) and block.get("type") in {"output_text", "text"}:
                text = block.get("text")
                if isinstance(text, str):
                    parts.append(text)
    text = "\n".join(part.strip() for part in parts if part.strip()).strip()
    if not text:
        raise RuntimeError("Grok returned no review text")
    return text


def request_review(message: str, api_key: str) -> str:
    body = json.dumps(
        {
            "model": MODEL,
            "input": [{"role": "user", "content": message}],
        }
    ).encode()
    request = urllib.request.Request(
        API_URL,
        data=body,
        headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=540) as response:
            payload = json.load(response)
    except urllib.error.HTTPError as exc:
        detail = exc.read(500).decode("utf-8", "replace")
        raise RuntimeError(f"xAI API HTTP {exc.code}: {detail}") from None
    if not isinstance(payload, dict):
        raise RuntimeError("xAI API returned a non-object response")
    text = extract_text(payload)
    if len(text) > MAX_COMMENT:
        text = text[:MAX_COMMENT] + "\n\n(review truncated)"
    return text


def write_output(path: str, message: str) -> None:
    delimiter = "GROK_" + secrets.token_hex(16)
    with open(path, "a", encoding="utf-8") as handle:
        handle.write(f"message<<{delimiter}\n{message}\n{delimiter}\n")


def read_env_file(name: str) -> str:
    return Path(os.environ[name]).read_text(encoding="utf-8")


def main() -> int:
    api_key = os.environ.get("XAI_API_KEY", "")
    output = os.environ.get("GITHUB_OUTPUT", "")
    if not api_key or not output:
        print("XAI_API_KEY and GITHUB_OUTPUT are required", file=sys.stderr)
        return 2
    message = user_message(
        read_env_file("GROK_PROMPT_FILE"),
        read_env_file("GROK_RULES_FILE"),
        read_env_file("GROK_NAMES_FILE"),
        read_env_file("GROK_DIFF_FILE"),
    )
    write_output(output, request_review(message, api_key))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, json.JSONDecodeError) as exc:
        print(f"Grok review failed: {exc}", file=sys.stderr)
        raise SystemExit(1) from None
