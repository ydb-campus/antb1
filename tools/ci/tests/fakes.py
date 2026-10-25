"""Fakes for the tests of the static build's scripts (test_static_sources.py, test_static_package.py): a curl that
answers from a table and a cmake that can only hash, so nothing reaches the network.
"""

from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

# The fake curl answers each URL from the JSON table $FAKE_CURL_MAP: {"file": path} serves those bytes (with -I only
# their size), {"exit": n, "code": status} fails like curl -f, {"nofile": true} exits 0 without writing, and
# {"turns": [answer, ...]} gives the n-th request of that URL the n-th answer (the last one from then on). It honours
# --max-filesize (exit 63), prints the HTTP status for -w, and logs "GET <url>" or "HEAD <url>" to $FAKE_CURL_LOG.
FAKE_CURL = """#!{python} -I
import json, os, shutil, sys
args = sys.argv[1:]
url, head = args[-1], "-I" in args or any(a.startswith("-") and not a.startswith("--") and "I" in a for a in args)
with open(os.environ["FAKE_CURL_LOG"]) as log:
    turn = sum(1 for line in log if line.split(" ", 1)[1] == url + "\\n")
with open(os.environ["FAKE_CURL_LOG"], "a") as log:
    log.write(("HEAD " if head else "GET ") + url + "\\n")
with open(os.environ["FAKE_CURL_MAP"]) as f:
    answer = json.load(f).get(url, {{"exit": 6, "code": 0}})
if "turns" in answer:
    answer = answer["turns"][min(turn, len(answer["turns"]) - 1)]
code = answer.get("code", 200 if "file" in answer or answer.get("nofile") else 0)
if "-w" in args:
    sys.stdout.write("%03d" % code)
if "file" in answer:
    size = os.path.getsize(answer["file"])
    if head:
        with open(args[args.index("-D") + 1], "w") as h:
            h.write("HTTP/1.1 %d OK\\r\\nContent-Length: %d\\r\\n\\r\\n" % (code, size))
    else:
        if "--max-filesize" in args and size > int(args[args.index("--max-filesize") + 1]):
            sys.exit(63)
        shutil.copyfile(answer["file"], args[args.index("-o") + 1])
sys.exit(answer.get("exit", 0))
"""
FAKE_CMAKE = """#!{python} -I
import hashlib, sys
assert sys.argv[1:3] == ["-E", "sha256sum"], sys.argv
with open(sys.argv[3], "rb") as f:
    print(hashlib.sha256(f.read()).hexdigest(), sys.argv[3])
"""


def install(bin_dir: Path, fakes: dict[str, str]) -> None:
    """Writes each fake (name -> script text with a {python} placeholder) as an executable into bin_dir."""
    bin_dir.mkdir(parents=True, exist_ok=True)
    for name, text in fakes.items():
        path = bin_dir / name
        path.write_text(text.format(python=sys.executable))
        path.chmod(0o755)


class FakeNet:
    """What each URL answers the fake curl, kept in a temporary directory."""

    def __init__(self, tmp: Path) -> None:
        self.tmp = tmp
        self.answers: dict[str, dict[str, object]] = {}
        self.served = 0

    def bytes_answer(self, data: bytes) -> dict[str, object]:
        self.served += 1
        path = self.tmp / f"served-{self.served}"
        path.write_bytes(data)
        return {"file": str(path)}

    def serve(self, url: str, data: bytes) -> str:
        self.answers[url] = self.bytes_answer(data)
        return url

    def fail(self, url: str, curl_exit: int, code: int = 0) -> str:
        self.answers[url] = {"exit": curl_exit, "code": code}
        return url

    def no_file(self, url: str) -> str:
        self.answers[url] = {"nofile": True}
        return url

    def in_turn(self, url: str, *answers: dict[str, object]) -> str:
        self.answers[url] = {"turns": list(answers)}
        return url

    @staticmethod
    def pin(name: str, data: bytes, variable: str, *urls: str) -> str:
        return f"{name} {hashlib.sha256(data).hexdigest()} {len(data)} {variable} {' '.join(urls)}".rstrip()

    def curl_env(self) -> dict[str, str]:
        """The fake curl's variables, with the table written and an empty request log."""
        (self.tmp / "map.json").write_text(json.dumps(self.answers))
        (self.tmp / "curl.log").write_text("")
        return {"FAKE_CURL_MAP": str(self.tmp / "map.json"), "FAKE_CURL_LOG": str(self.tmp / "curl.log")}

    def requested(self) -> list[str]:
        return (self.tmp / "curl.log").read_text().splitlines()
