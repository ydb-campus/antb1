"""Tests for the pinned sources of the static build (scripts/static-sources.sh, as scripts/static-arrow.sh uses it;
run by `pixi run lint`).

They cover the order in which a pin's URLs are tried, which errors get a second attempt, the size and sha256 check of
every download, the local copies the Arrow build gets, --check-urls, the lock format, a prefix key that ignores URLs,
and the repository's own lock. They are hermetic: `curl`, `cmake` and `tar` are fakes on PATH, and the fake curl
answers from a table, so nothing reaches the network.
"""

from __future__ import annotations

import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path
from urllib.parse import urlsplit

import pytest

REPO = Path(__file__).resolve().parents[3]
SCRIPTS = [REPO / "scripts" / "static-arrow.sh", REPO / "scripts" / "static-sources.sh"]
LOCK = REPO / "cmake" / "static" / "arrow-sources.lock"

pytestmark = pytest.mark.skipif(
    f"{platform.system()}-{platform.machine()}" not in {"Linux-x86_64", "Linux-aarch64", "Darwin-arm64"},
    reason="static-arrow.sh supports the three release targets only",
)

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
# The fake tar is the first command of static-arrow's build: it writes its arguments and the ARROW_*_URL variables it
# sees to $FAKE_TAR_LOG and exits 97, so a run that reaches the build ends there.
FAKE_TAR = """#!{python} -I
import json, os, sys
with open(os.environ["FAKE_TAR_LOG"], "w") as log:
    json.dump({{"args": sys.argv[1:], "urls": {{k: v for k, v in os.environ.items() if k.startswith("ARROW_")}}}}, log)
sys.exit(97)
"""
BUILD_STARTED = 97


class Sandbox:
    """Copies of the scripts in a fake repository, a fake pixi environment, a fake curl and cmake."""

    def __init__(self, tmp: Path) -> None:
        self.tmp = tmp
        self.root = tmp / "root"
        (self.root / "scripts").mkdir(parents=True)
        (self.root / "cmake" / "static").mkdir(parents=True)
        for script in SCRIPTS:
            shutil.copy(script, self.root / "scripts" / script.name)
        (self.root / "cmake" / "static" / "arrow.cmake").write_text("# input of the key\n")
        self.lock = self.root / "cmake" / "static" / "arrow-sources.lock"
        (tmp / "env" / "conda-meta").mkdir(parents=True)
        self.arrow_dir = tmp / "arrow"
        self.src = self.arrow_dir / "src"
        self.bin = tmp / "bin"
        self.bin.mkdir()
        for name, text in (("curl", FAKE_CURL), ("cmake", FAKE_CMAKE), ("tar", FAKE_TAR)):
            path = self.bin / name
            path.write_text(text.format(python=sys.executable))
            path.chmod(0o755)
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

    def write_lock(self, *lines: str) -> None:
        self.lock.write_text("# test lock\n" + "".join(line + "\n" for line in lines))

    def run(self, *args: str) -> subprocess.CompletedProcess[str]:
        (self.tmp / "map.json").write_text(json.dumps(self.answers))
        (self.tmp / "curl.log").write_text("")
        env = {
            "PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}",
            "HOME": str(self.tmp),
            "CONDA_PREFIX": str(self.tmp / "env"),
            "ANTB1_ARROW_DIR": str(self.arrow_dir),
            "ANTB1_SOURCES_RETRY_DELAY": "0",
            "FAKE_CURL_MAP": str(self.tmp / "map.json"),
            "FAKE_CURL_LOG": str(self.tmp / "curl.log"),
            "FAKE_TAR_LOG": str(self.tmp / "tar.json"),
        }
        bash = shutil.which("bash")
        assert bash is not None
        return subprocess.run(
            [bash, str(self.root / "scripts" / "static-arrow.sh"), *args],
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )

    def requested(self) -> list[str]:
        return (self.tmp / "curl.log").read_text().splitlines()

    def build_input(self) -> dict[str, object]:
        """The fake tar's record: the arguments and ARROW_*_URL variables that static-arrow's build started with."""
        return json.loads((self.tmp / "tar.json").read_text())


@pytest.fixture
def box(tmp_path: Path) -> Sandbox:
    return Sandbox(tmp_path)


ARROW = b"arrow source tarball"
ZLIB = b"zlib source tarball"
BUSY = {"exit": 22, "code": 503}
UNKNOWN_HOST = {"exit": 6, "code": 0}


def fetch(box: Sandbox) -> subprocess.CompletedProcess[str]:
    """Runs static-arrow up to its sources: the fake tar ends the run where the build starts (exit 97)."""
    return box.run()


def test_urls_are_tried_in_order_and_only_passing_errors_twice(box: Sandbox) -> None:
    box.write_lock(
        box.pin(
            "arrow.tar.gz",
            ARROW,
            "-",
            box.fail("https://dead.example/a", 22, 404),
            box.fail("https://busy.example/a", 22, 503),
            box.fail("https://slow.example/a", 28),
            box.serve("https://wrong.example/a", b"0123456789abcdefghij"),  # the pinned size, other bytes
            box.serve("https://good.example/a", ARROW),
            box.serve("https://never.example/a", ARROW),
        )
    )
    r = fetch(box)
    assert r.returncode == BUILD_STARTED, r.stderr
    assert box.requested() == [
        "GET https://dead.example/a",
        "GET https://busy.example/a",
        "GET https://busy.example/a",
        "GET https://slow.example/a",
        "GET https://wrong.example/a",
        "GET https://good.example/a",
    ]
    assert (box.src / "arrow.tar.gz").read_bytes() == ARROW
    assert "https://dead.example/a: dead: curl exit 22, dead (HTTP 404" in r.stderr
    assert "https://busy.example/a: passing: curl exit 22, server error (HTTP 503" in r.stderr
    assert "https://slow.example/a: timeout: curl exit 28, timeout" in r.stderr
    assert "https://wrong.example/a: wrong: 20 bytes, sha256" in r.stderr
    assert "downloaded from https://good.example/a" in r.stdout
    assert sorted(p.name for p in box.src.iterdir()) == ["arrow.tar.gz"]


def test_success_without_a_file_and_a_larger_file_fall_through(box: Sandbox) -> None:
    box.write_lock(
        box.pin(
            "arrow.tar.gz",
            ARROW,
            "-",
            box.no_file("https://empty.example/a"),
            box.serve("https://huge.example/a", ARROW * 1000),
            box.serve("https://good.example/a", ARROW),
        )
    )
    r = fetch(box)
    assert r.returncode == BUILD_STARTED, r.stderr
    assert box.requested() == [
        "GET https://empty.example/a",
        "GET https://huge.example/a",
        "GET https://good.example/a",
    ]
    assert "curl reported success but wrote no file" in r.stderr
    assert "https://huge.example/a: wrong: curl exit 63, larger than the pinned size" in r.stderr
    assert (box.src / "arrow.tar.gz").read_bytes() == ARROW


def test_a_matching_local_copy_is_kept_and_a_wrong_one_replaced(box: Sandbox) -> None:
    box.write_lock(
        box.pin("arrow.tar.gz", ARROW, "-", box.serve("https://a.example/arrow.tar.gz", ARROW)),
        box.pin("zlib.tar.gz", ZLIB, "ARROW_ZLIB_URL", box.serve("https://a.example/zlib.tar.gz", ZLIB)),
    )
    box.src.mkdir(parents=True)
    (box.src / "arrow.tar.gz").write_bytes(ARROW)
    (box.src / "zlib.tar.gz").write_bytes(b"truncated")
    assert fetch(box).returncode == BUILD_STARTED
    assert box.requested() == ["GET https://a.example/zlib.tar.gz"]
    assert (box.src / "zlib.tar.gz").read_bytes() == ZLIB


def test_a_file_that_no_url_serves_stops_the_run(box: Sandbox) -> None:
    box.write_lock(
        box.pin("arrow.tar.gz", ARROW, "-", box.serve("https://a.example/arrow.tar.gz", ARROW)),
        box.pin(
            "zlib.tar.gz",
            ZLIB,
            "ARROW_ZLIB_URL",
            box.fail("https://gone.example/zlib.tar.gz", 22, 410),
            box.serve("https://wrong.example/zlib.tar.gz", b"not zlib"),
        ),
    )
    box.src.mkdir(parents=True)
    (box.src / "zlib.tar.gz").write_bytes(b"stale")
    r = fetch(box)
    assert r.returncode == 1
    assert "zlib.tar.gz: none of its 2 URLs served the pinned file" in r.stderr
    assert sorted(p.name for p in box.src.iterdir()) == ["arrow.tar.gz"]


def test_the_build_gets_the_local_copies_even_from_a_last_line_without_a_newline(box: Sandbox) -> None:
    box.lock.write_text(
        box.pin("arrow.tar.gz", ARROW, "-", box.serve("https://a.example/arrow.tar.gz", ARROW))
        + "\n"
        + box.pin("zlib.tar.gz", ZLIB, "ARROW_ZLIB_URL", box.serve("https://a.example/zlib.tar.gz", ZLIB))
    )
    assert fetch(box).returncode == BUILD_STARTED
    src = box.src.resolve()  # static-arrow resolves ANTB1_ARROW_DIR (pwd -P)
    started = box.build_input()
    assert started["args"][:2] == ["-xzf", str(src / "arrow.tar.gz")]
    assert started["urls"] == {"ARROW_ZLIB_URL": str(src / "zlib.tar.gz")}
    assert (box.src / "zlib.tar.gz").read_bytes() == ZLIB


def test_check_urls_fails_on_dead_or_wrong_urls_and_heads_the_origin(box: Sandbox) -> None:
    box.write_lock(
        box.pin(
            "arrow.tar.gz",
            ARROW,
            "-",
            box.serve("https://a.example/arrow.tar.gz", ARROW),
            box.serve("https://archive.apache.org/dist/arrow.tar.gz", ARROW),
            box.serve("https://b.example/arrow.tar.gz", b"different"),
        ),
        box.pin(
            "zlib.tar.gz",
            ZLIB,
            "ARROW_ZLIB_URL",
            box.serve("https://a.example/zlib.tar.gz", ZLIB),
            box.fail("https://b.example/zlib.tar.gz", 22, 404),
        ),
    )
    r = box.run("--check-urls")
    assert r.returncode == 1
    assert box.requested() == [
        "GET https://a.example/arrow.tar.gz",
        "HEAD https://archive.apache.org/dist/arrow.tar.gz",
        "GET https://b.example/arrow.tar.gz",
        "GET https://a.example/zlib.tar.gz",
        "GET https://b.example/zlib.tar.gz",
    ]
    assert "https://archive.apache.org/dist/arrow.tar.gz: ok (HEAD only" in r.stdout
    assert "https://b.example/arrow.tar.gz: wrong (9 bytes" in r.stdout
    assert "https://b.example/zlib.tar.gz: dead" in r.stdout
    assert "zlib.tar.gz: only 1 good URL(s); every file needs two" in r.stderr
    assert "FAIL: 2 of 5 URLs are dead, serve other bytes or fail, 1 file(s) have fewer than two good URLs" in r.stderr
    assert not box.src.exists()


def test_check_urls_only_warns_on_timeouts_and_errors_that_may_pass(box: Sandbox) -> None:
    box.write_lock(
        box.pin(
            "arrow.tar.gz",
            ARROW,
            "-",
            box.serve("https://a.example/arrow.tar.gz", ARROW),
            box.fail("https://busy.example/arrow.tar.gz", 22, 429),
            box.fail("https://slow.example/arrow.tar.gz", 28),
            box.in_turn("https://blip.example/arrow.tar.gz", UNKNOWN_HOST, box.bytes_answer(ARROW)),
            box.serve("https://c.example/arrow.tar.gz", ARROW),
        )
    )
    r = box.run("--check-urls")
    assert r.returncode == 0, r.stderr
    assert box.requested().count("GET https://busy.example/arrow.tar.gz") == 4  # 2 attempts, then 2 in the recheck
    assert box.requested().count("GET https://slow.example/arrow.tar.gz") == 2  # a timeout gets no second attempt
    warning = "warning: arrow.tar.gz: https://{}.example/arrow.tar.gz: {} on the second pass (curl exit {},"
    assert warning.format("busy", "passing", 22) in r.stderr
    assert warning.format("slow", "timeout", 28) in r.stderr
    assert "https://blip.example/arrow.tar.gz: ok on the second pass" in r.stdout
    assert "OK: 3 of 5 URLs serve their pinned file; 2 warned: a timeout or an error that may pass on the" in r.stdout


def test_check_urls_fails_on_a_larger_file_and_on_what_the_second_pass_finds(box: Sandbox) -> None:
    box.write_lock(
        box.pin(
            "arrow.tar.gz",
            ARROW,
            "-",
            box.serve("https://a.example/arrow.tar.gz", ARROW),
            box.serve("https://b.example/arrow.tar.gz", ARROW),
            box.serve("https://big.example/arrow.tar.gz", ARROW * 2),
            box.fail("https://gone.example/arrow.tar.gz", 6),
            box.in_turn("https://flaky.example/arrow.tar.gz", BUSY, BUSY, box.bytes_answer(b"0123456789abcdefghij")),
        )
    )
    r = box.run("--check-urls")
    assert r.returncode == 1
    assert box.requested().count("GET https://big.example/arrow.tar.gz") == 1  # other bytes count at once
    assert "https://big.example/arrow.tar.gz: wrong (curl exit 63, larger than the pinned size)" in r.stdout
    assert "https://gone.example/arrow.tar.gz: failed on the second pass (curl exit 6, the host name" in r.stderr
    assert "https://flaky.example/arrow.tar.gz: wrong on the second pass (20 bytes" in r.stderr
    assert "FAIL: 3 of 5 URLs are dead, serve other bytes or fail, 0 file(s) have fewer than two" in r.stderr


def test_check_urls_passes_when_every_url_serves_the_pin(box: Sandbox) -> None:
    box.write_lock(
        box.pin(
            "arrow.tar.gz",
            ARROW,
            "-",
            box.serve("https://a.example/arrow.tar.gz", ARROW),
            box.serve("https://b.example/arrow.tar.gz", ARROW),
        )
    )
    r = box.run("--check-urls")
    assert r.returncode == 0, r.stderr
    assert "OK: 2 of 2 URLs serve their pinned file" in r.stdout


@pytest.mark.parametrize(
    "line",
    [
        "arrow.tar.gz {sha} {size} -",  # no URL
        "arrow.tar.gz {sha} {size} - http://a.example/arrow.tar.gz",  # not HTTPS
        "arrow.tar.gz {sha} {size} https://a.example/arrow.tar.gz -",  # the old order: URL before variable
        "arrow.tar.gz {sha} {size} ZLIB https://a.example/arrow.tar.gz",  # not an ARROW_<DEP>_URL
        "arrow.tar.gz SHA {size} - https://a.example/arrow.tar.gz",
        "dir/arrow.tar.gz {sha} {size} - https://a.example/arrow.tar.gz",
    ],
)
def test_malformed_lines_are_rejected(box: Sandbox, line: str) -> None:
    box.write_lock(line.format(sha=hashlib.sha256(ARROW).hexdigest(), size=len(ARROW)))
    r = box.run("--check-urls")
    assert r.returncode == 2
    assert "malformed or repeated line for" in r.stderr
    assert box.requested() == []


def test_a_repeated_name_and_a_lock_without_arrow_are_rejected(box: Sandbox) -> None:
    line = box.pin("zlib.tar.gz", ZLIB, "ARROW_ZLIB_URL", "https://a.example/zlib.tar.gz")
    box.write_lock(line)
    assert box.run("--check-urls").returncode == 2
    box.write_lock(box.pin("arrow.tar.gz", ARROW, "-", "https://a.example/arrow.tar.gz"), line, line)
    assert box.run("--check-urls").returncode == 2


def test_the_prefix_key_ignores_urls_comments_and_order_but_not_pins(box: Sandbox) -> None:
    arrow = box.pin("arrow.tar.gz", ARROW, "-", "https://a.example/arrow.tar.gz")
    zlib = box.pin("zlib.tar.gz", ZLIB, "ARROW_ZLIB_URL", "https://a.example/zlib.tar.gz")
    box.write_lock(arrow, zlib)
    key = box.run("--print-key").stdout
    assert re.fullmatch(r"[0-9a-f]{16}\n", key)
    box.lock.write_text(
        "# an edited comment\n"
        + box.pin("zlib.tar.gz", ZLIB, "ARROW_ZLIB_URL", "https://b.example/z", "https://c.example/z")
        + "\n"
        + box.pin("arrow.tar.gz", ARROW, "-", "https://b.example/x")
        + "\n"
    )
    assert box.run("--print-key").stdout == key
    box.write_lock(box.pin("arrow.tar.gz", ARROW + b"!", "-", "https://a.example/arrow.tar.gz"), zlib)
    assert box.run("--print-key").stdout != key


# Host names of one operator: two copies on them are one point of failure.
OPERATORS = {
    "github.com": "github",
    "codeload.github.com": "github",
    "objects.githubusercontent.com": "github",
    "release-assets.githubusercontent.com": "github",
    "packages.apache.org": "asf-jfrog",
    "apache.jfrog.io": "asf-jfrog",
    "deb.debian.org": "debian",
    "snapshot.debian.org": "debian",
    "archive.ubuntu.com": "canonical",
    "ports.ubuntu.com": "canonical",
}


def test_the_repository_lock_lists_two_operators_per_file() -> None:
    names, variables = set(), set()
    for no, line in enumerate(LOCK.read_text().splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        name, sha, size, variable, *urls = line.split()
        assert name not in names and "/" not in name, f"line {no}"
        assert re.fullmatch(r"[0-9a-f]{64}", sha) and int(size) > 0, f"line {no}"
        assert variable == "-" or re.fullmatch(r"ARROW_[A-Z0-9_]+_URL", variable), f"line {no}"
        assert variable not in variables, f"line {no}: {variable} twice"
        assert urls and all(u.startswith("https://") for u in urls), f"line {no}"
        assert len(set(urls)) == len(urls), f"line {no}: a URL twice"
        operators = {OPERATORS.get(urlsplit(u).hostname or "", urlsplit(u).hostname) for u in urls}
        assert len(operators) >= 2, f"line {no} ({name}): one operator is a single point of failure"
        names.add(name)
        variables.add(variable)
    assert "-" in variables
    # Every tarball whose licence static-arrow.sh packages is one of the pinned files.
    licensed = re.findall(r"^(\S+\.tar\.gz) \S+$", (REPO / "scripts" / "static-arrow.sh").read_text(), re.MULTILINE)
    assert licensed and set(licensed) <= names, set(licensed) - names
