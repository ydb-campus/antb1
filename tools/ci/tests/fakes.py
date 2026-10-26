"""Fakes for the tests of the static build's scripts (test_static_sources.py, test_static_package.py,
test_static_verify.py): a curl that answers from a table and a cmake that can only hash, so nothing reaches the
network; a DuckDB that writes the smoke kit's stand-ins, a container engine that runs the binary of the image it
imported, a codesign, a logging python, and a standalone antb1 that answers the smoke tests, so no test needs a real
one or reaches a real Docker daemon.
"""

from __future__ import annotations

import hashlib
import json
import shutil
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


# The fake DuckDB of `static_gates.py kit`, which must pass -no-init first (no ~/.duckdbrc): `-c "COPY (...) TO
# '<path>' (... COMPRESSION <codec> ...)"` writes stand-in bytes, `-csv -noheader -c "SELECT ... FROM '<path>'"`
# answers the smoke tests' answer (another one for the files named in $FAKE_DUCKDB_WRONG), `-version` prints a
# version; anything else exits 99.
FAKE_DUCKDB = """#!{python} -I
import os, re, sys
args = sys.argv[1:]
if args[:1] != ["-no-init"]:
    sys.stderr.write("fake duckdb: no -no-init in " + repr(args) + "\\n")
    sys.exit(99)
args = args[1:]
if args == ["-version"]:
    print("v0.0.0 (fake)")
    sys.exit(0)
sql = args[-1] if args else ""
copy = re.fullmatch(r"COPY \\(.+\\) TO '(.+)' \\(FORMAT parquet, COMPRESSION (\\w+), ROW_GROUP_SIZE 10000\\)", sql)
select = re.fullmatch(r"SELECT .+ FROM '(.+)'", sql)
if args[:-1] == ["-c"] and copy:
    with open(copy.group(1), "wb") as f:
        f.write(("PAR1 " + copy.group(2) + " ").encode() * 64)
elif args[:-1] == ["-csv", "-noheader", "-c"] and select:
    wrong = os.environ.get("FAKE_DUCKDB_WRONG", "").split(",")
    print("1,2,3,4,5" if os.path.basename(select.group(1)) in wrong else "200000,19999900000,0,999,97")
else:
    sys.stderr.write("fake duckdb: unexpected " + repr(args) + "\\n")
    sys.exit(99)
"""
# The fake container engine of `static_gates.py verify --container`: every call's argv goes to
# $FAKE_DOCKER_DIR/calls.jsonl. `info` fails when $FAKE_DOCKER_DOWN is set; `import -` keeps the root file system it
# reads (rootfs.tar, unpacked in rootfs/) and prints the image id, or fails when $FAKE_DOCKER_IMPORT_FAIL is set;
# `run --rm ... [--env K=V]... <image> /antb1 ARGS` runs rootfs/antb1 with only those variables and /kit/ mapped to
# rootfs/kit/ (exit 125 when $FAKE_DOCKER_FAIL is set); `rm --force` and `rmi --force` succeed; anything else exits 99.
# With $FAKE_DOCKER_BANNER set, every call first prints the banner of podman's docker emulation to stderr.
FAKE_DOCKER_IMAGE = "sha256:" + "ab" * 32
FAKE_DOCKER = """#!{python} -I
import io, json, os, subprocess, sys, tarfile
state = os.environ["FAKE_DOCKER_DIR"]
args = sys.argv[1:]
with open(os.path.join(state, "calls.jsonl"), "a") as log:
    log.write(json.dumps(args) + "\\n")
if os.environ.get("FAKE_DOCKER_BANNER"):
    sys.stderr.write("Emulate Docker CLI using podman. Create /etc/containers/nodocker to quiet msg.\\n")
    sys.stderr.flush()
image, root = "sha256:" + "ab" * 32, os.path.join(state, "rootfs")
if args == ["info"]:
    sys.exit(1 if os.environ.get("FAKE_DOCKER_DOWN") else 0)
if args == ["import", "-"]:
    if os.environ.get("FAKE_DOCKER_IMPORT_FAIL"):
        sys.stderr.write("fake docker: no import\\n")
        sys.exit(1)
    data = sys.stdin.buffer.read()
    with open(os.path.join(state, "rootfs.tar"), "wb") as f:
        f.write(data)
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        tar.extractall(root, filter="data")
    print(image)
    sys.exit(0)
if args[:2] == ["run", "--rm"] and image in args:
    if os.environ.get("FAKE_DOCKER_FAIL"):
        sys.stderr.write("fake docker: the container failed\\n")
        sys.exit(125)
    at = args.index(image)
    env = dict(args[i + 1].split("=", 1) for i in range(2, at) if args[i] == "--env")
    command = [os.path.join(root, "antb1")] + [arg.replace("/kit/", root + "/kit/") for arg in args[at + 2 :]]
    sys.exit(subprocess.run(command, env=env, cwd=root).returncode)
if args[:2] in (["rm", "--force"], ["rmi", "--force"]) and len(args) == 3:
    sys.exit(0)
sys.stderr.write("fake docker: unexpected " + repr(args) + "\\n")
sys.exit(99)
"""
# A fake codesign: logs its argv to $FAKE_CODESIGN_LOG and exits with $FAKE_CODESIGN_EXIT (default 0).
FAKE_CODESIGN = """#!{python} -I
import json, os, sys
with open(os.environ["FAKE_CODESIGN_LOG"], "a") as log:
    log.write(json.dumps(sys.argv[1:]) + "\\n")
sys.exit(int(os.environ.get("FAKE_CODESIGN_EXIT", "0")))
"""
# The `python` of scripts/static-check.sh: logs its arguments to $FAKE_PYTHON_LOG, then runs the real interpreter,
# unless $FAKE_PYTHON_DRY is set.
FAKE_PYTHON = """#!/bin/sh
printf '%s\\n' "$*" >>"$FAKE_PYTHON_LOG"
[ -z "${{FAKE_PYTHON_DRY:-}}" ] || exit 0
exec {python} -I "$@"
"""
# A standalone antb1 as the smoke tests see it. verify and the binary gate run it with an empty environment, so its
# answers are fixed when it is made (fake_antb1); `wrong` names the one answer it gets wrong. As the real one
# (src/cli/allocator.cc), it restarts its allocator only on Linux.
FAKE_ANTB1 = """#!{python} -I
import json, os, sys
LOG, TARGET, VERSION, COMMIT, WRONG = {log!r}, {target!r}, {version!r}, {commit!r}, {wrong!r}
ANSWER = "200000,19999900000,0,999,97"
args = sys.argv[1:]
with open(LOG, "a") as f:
    f.write(json.dumps({{"argv": args, "env": sorted(os.environ), "cwd": os.getcwd()}}) + "\\n")


def say(out="", err="", code=0):
    sys.stdout.write(out)
    sys.stderr.write(err)
    sys.exit(code)


LINUX = TARGET.startswith("linux-")
if os.environ.get("MIMALLOC_VERBOSE") == "1":
    restarted = LINUX and WRONG != "restart"
    sys.stderr.write("mimalloc: option 'purge_delay': " + ("-1" if restarted else "10") + "\\n")
first = "antb1 " + VERSION + " (Apache Arrow 25.0.0)\\n"
if args == ["version"]:
    say(first)
if args == ["version", "--verbose"]:
    kind = "no package kind" if WRONG == "arrow" else "antb1-static"
    fields = [
        ("commit", COMMIT or "unknown"),
        ("flavor", "dynamic" if WRONG == "flavor" else "static-thinlto"),
        ("target", TARGET),
        ("arrow", "25.0.0 (headers 25.0.0, " + kind + ", git 0123456789ab, Clang 23.1.3)"),
        ("codecs", "snappy yes, gzip yes, brotli " + ("no" if WRONG == "codec" else "yes") + ", zstd yes, lz4 yes"),
        ("mimalloc purge delay", "-1" if LINUX and WRONG != "purge-field" else "default"),
    ]
    say(first + "".join(key + ": " + value + "\\n" for key, value in fields))
if args == ["version", "--bogus"]:
    say("", "The following argument was not expected: --bogus\\n", 0 if WRONG == "usage" else 2)
if args[:1] == ["query"] and "-c" in args and "--table" in args:
    sql = args[args.index("-c") + 1]
    table = args[args.index("--table") + 1].split("=", 1)[-1]
    if sql == "SELECT COUNT(*) FROM":
        say("", "antb1: parse error: expected a table name or a quoted file path\\n", 0 if WRONG == "parse" else 1)
    if not os.path.isfile(table) or table.endswith("truncated.parquet"):
        say("", "antb1: IO error: " + table + "\\n", 0 if WRONG == "truncated" else 3)
    answer = "1,2,3,4,5" if WRONG == "answer" and table.endswith("/zstd.parquet") else ANSWER
    if "--format" in args:
        say("count_star(),sum(a),min(b),max(b),count(DISTINCT c)\\n" + answer + "\\n")
    threads = args[args.index("--threads") + 1] if "--threads" in args else "1"
    say(answer + (" " + threads if WRONG == "threads" else "") + "\\n")
say("", "fake antb1: unexpected " + repr(args) + "\\n", 99)
"""


# A curl for README.md's install block (`curl -fsSLO URL`), which runs with a minimal environment: it serves each URL
# from served/<the URL without https://, "/" as "_">, writes it under the URL's base name like -O, logs every argv,
# and fails like `curl -f` on a URL it does not serve. Its paths are fixed when it is made (fake_download).
FAKE_DOWNLOAD = """#!{python} -I
import os, shutil, sys
SERVED, LOG = {served!r}, {log!r}
args = sys.argv[1:]
with open(LOG, "a") as log:
    log.write(" ".join(args) + "\\n")
flags = "".join(a[1:] for a in args[:-1] if a.startswith("-") and not a.startswith("--"))
url = args[-1] if args else ""
if "O" not in flags or "f" not in flags or not url.startswith("https://"):
    sys.stderr.write("fake curl: unexpected " + repr(args) + "\\n")
    sys.exit(99)
path = os.path.join(SERVED, url[len("https://") :].replace("/", "_"))
if not os.path.isfile(path):
    sys.stderr.write("curl: (22) The requested URL returned error: 404\\n")
    sys.exit(22)
shutil.copyfile(path, os.path.basename(url))
"""


def fake_download(served: Path, log: Path) -> str:
    """FAKE_DOWNLOAD serving from `served` and logging to `log` (install() then fills in the interpreter)."""
    return FAKE_DOWNLOAD.replace("{served!r}", repr(str(served))).replace("{log!r}", repr(str(log)))


def serve(served: Path, url: str, data: bytes) -> None:
    served.mkdir(parents=True, exist_ok=True)
    (served / url.removeprefix("https://").replace("/", "_")).write_bytes(data)


def fake_antb1(log: Path, target: str, version: str, commit: str = "", wrong: str = "") -> bytes:
    """The bytes of a FAKE_ANTB1 that logs each run ({argv, env, cwd}) to `log`."""
    script = FAKE_ANTB1.format(
        python=sys.executable, log=str(log), target=target, version=version, commit=commit, wrong=wrong
    )
    return script.encode()


def install(bin_dir: Path, fakes: dict[str, str]) -> None:
    """Writes each fake (name -> script text with a {python} placeholder) as an executable into bin_dir."""
    bin_dir.mkdir(parents=True, exist_ok=True)
    for name, text in fakes.items():
        path = bin_dir / name
        path.write_text(text.format(python=sys.executable))
        path.chmod(0o755)


def assert_fakes_first(path: str, bin_dir: Path, names: list[str]) -> None:
    """Each tool resolves to its fake on `path`, so a test can never reach a real one."""
    for name in names:
        found = shutil.which(name, path=path)
        assert found is not None and Path(found).parent == bin_dir, f"{name} resolves to {found}, not to a fake"


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
