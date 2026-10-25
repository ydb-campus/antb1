"""Tests for the licence texts and the sources of the standalone package (scripts/static_package.py and
`scripts/static_gates.py package`; docs/adr/0034-licence-and-static-sources.md; run by `pixi run lint`).

They cover the notices (labels, the LGPL notice first on Linux, libc++ on macOS, the refusal of an unmapped sysroot
build), the sources tarball (its members and digests, the download cache, one retried pass, the refusal of a dirty
tree, of a package from another commit and of wrong bytes) and the package gate on tarballs packed here. They are
hermetic: the repository is a throwaway git repository, the pixi environment a few files, and `curl` and `cmake` are
fakes on PATH, so nothing reaches the network.
"""

from __future__ import annotations

import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

import pytest
from fakes import FAKE_CMAKE, FAKE_CURL, FakeNet, install

REPO = Path(__file__).resolve().parents[3]
TARGET = "linux-x86_64"
VERSION = "0.1.0"
SYSROOT = "sysroot_linux-64-2.28-hfake_9"
SRPM = "glibc-2.28-1.el8_9.1.src.rpm"
ARROW, THRIFT = b"arrow source", b"thrift source"
GLIBC = b"glibc source rpm"
CLI11, BOOST = b"cli11 conda archive", b"boost headers conda archive"
LGPL_NOTICE_START = "This program contains the GNU C Library"


class Box(FakeNet):
    """A committed copy of the scripts and locks, a pixi environment and an Arrow prefix with licence files."""

    def __init__(self, tmp: Path) -> None:
        super().__init__(tmp)
        self.root = tmp / "root"
        for name in ("static_package.py", "static_gates.py", "static-sources.sh"):
            (self.root / "scripts").mkdir(parents=True, exist_ok=True)
            shutil.copy(REPO / "scripts" / name, self.root / "scripts" / name)
        static = self.root / "cmake" / "static"
        (static / "licenses").mkdir(parents=True)
        (static / "licenses" / "CLI11-LICENSE.txt").write_text("CLI11 licence\n")
        (static / "licenses" / "libcxx-LICENSE.txt").write_text("libc++ licence\n")
        (static / "arrow-sources.lock").write_text(
            "# test lock\n"
            + self.pin("apache-arrow-25.0.0.tar.gz", ARROW, "-", self.serve("https://a.test/arrow.tgz", ARROW))
            + "\n"
            + self.pin("thrift-0.22.0.tar.gz", THRIFT, "ARROW_THRIFT_URL", self.serve("https://a.test/t.tgz", THRIFT))
            + "\n"
        )
        self.glibc_urls = [self.serve("https://vault.test/glibc.src.rpm", GLIBC), "https://mirror.test/glibc.src.rpm"]
        self.write_glibc_lock(SYSROOT)
        (self.root / "LICENSE").write_text("the licence of the repository\n")
        (self.root / "NOTICE").write_text("the notice of the repository\n")
        (self.root / ".gitignore").write_text("/build/\n/.cache/\n")
        self.git_env = {
            "HOME": str(tmp),
            "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_AUTHOR_NAME": "Test",
            "GIT_AUTHOR_EMAIL": "test@example.invalid",
            "GIT_AUTHOR_DATE": "2026-01-02T03:04:05Z",
            "GIT_COMMITTER_NAME": "Test",
            "GIT_COMMITTER_EMAIL": "test@example.invalid",
            "GIT_COMMITTER_DATE": "2026-01-02T03:04:05Z",
        }
        self.git("init", "-q", "-b", "main")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "test")
        self.commit = self.git("rev-parse", "HEAD")

        self.env = tmp / "env"
        meta = self.env / "conda-meta"
        meta.mkdir(parents=True)
        self.conda_package("cli11", "2.7.2", CLI11)
        self.conda_package("libboost-headers", "1.88.0", BOOST)
        self.conda_package("libstdcxx-devel_linux-64", "15.3.0", b"")
        self.conda_package("libcxx-devel", "23.1.3", b"")
        (meta / f"{SYSROOT}.json").write_text(json.dumps({"name": "sysroot_linux-64", "version": "2.28"}))
        glibc_licences = self.env / "x86_64-conda-linux-gnu" / "sysroot" / "usr" / "share" / "licenses" / "glibc"
        glibc_licences.mkdir(parents=True)
        for name in ("COPYING", "COPYING.LIB", "LICENSES"):
            (glibc_licences / name).write_text(f"glibc {name}\n")
        (self.env / "share" / "licenses" / "libstdc++").mkdir(parents=True)
        (self.env / "share" / "licenses" / "libstdc++" / "RUNTIME.LIBRARY.EXCEPTION").write_text("GCC exception\n")
        self.prefix = tmp / "prefix"
        licences = self.prefix / "share" / "antb1-licenses"
        licences.mkdir(parents=True)
        for name in ("arrow-LICENSE.txt", "arrow-NOTICE.txt", "thrift-LICENSE", "thrift-NOTICE"):
            (licences / name).write_text(f"{name} text\n")

        self.arrow_dir = tmp / "arrow"
        self.bin = tmp / "bin"
        install(self.bin, {"curl": FAKE_CURL, "cmake": FAKE_CMAKE})

    def git(self, *args: str) -> str:
        env = {"PATH": os.environ["PATH"], **self.git_env}
        result = subprocess.run(["git", "-C", str(self.root), *args], env=env, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        return result.stdout.strip()

    def conda_package(self, name: str, version: str, archive: bytes) -> None:
        fn = f"{name}-{version}-h0_0.conda"
        url = self.serve(f"https://conda.test/linux-64/{fn}", archive)
        meta = {"name": name, "version": version, "fn": fn, "url": url, "subdir": "linux-64"}
        meta |= {"sha256": hashlib.sha256(archive).hexdigest(), "size": len(archive)}
        (self.env / "conda-meta" / f"{fn.removesuffix('.conda')}.json").write_text(json.dumps(meta))

    def write_glibc_lock(self, build: str, data: bytes = GLIBC) -> None:
        line = f"{build} {SRPM} {hashlib.sha256(data).hexdigest()} {len(data)} {' '.join(self.glibc_urls)}"
        (self.root / "cmake" / "static" / "glibc-source.lock").write_text(f"# test lock\n{line}\n")

    def package(self, commit: str | None = None) -> None:
        """What `pixi run static` leaves for `sources`: the package directory with its BUILDINFO.json."""
        top = self.root / "build" / "static" / "package" / f"antb1-{VERSION}-{TARGET}"
        top.mkdir(parents=True, exist_ok=True)
        info = {"name": "antb1", "version": VERSION, "commit": commit or self.commit, "target": TARGET}
        (top / "BUILDINFO.json").write_text(json.dumps(info))

    def run(self, script: str, *args: str, **env: str) -> subprocess.CompletedProcess[str]:
        full_env = {
            "PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}",
            "CONDA_PREFIX": str(self.env),
            "ANTB1_ARROW_DIR": str(self.arrow_dir),
            "ANTB1_SOURCES_RETRY_DELAY": "0",
            **self.git_env,
            **self.curl_env(),
            **env,
        }
        return subprocess.run(
            [sys.executable, "-I", str(self.root / "scripts" / script), *args],
            cwd=self.root,
            env=full_env,
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )

    def notices(self, target: str = TARGET) -> subprocess.CompletedProcess[str]:
        out = self.tmp / f"notices-{target}"
        args = ["notices", "--target", target, "--prefix", str(self.prefix), "--env", str(self.env), "--out", str(out)]
        return self.run("static_package.py", *args)

    def sources(self, **env: str) -> subprocess.CompletedProcess[str]:
        return self.run("static_package.py", "sources", "--target", TARGET, **env)


@pytest.fixture
def box(tmp_path: Path) -> Box:
    return Box(tmp_path)


def labels(text: str) -> list[str]:
    return [line[5:-5] for line in text.splitlines() if line.startswith("==== ") and line.endswith(" ====")]


# ---------------------------------------------------------------------------------------------------------------------
# notices


def test_linux_notices_start_with_the_lgpl_notice_and_label_every_section(box: Box) -> None:
    result = box.notices()
    assert result.returncode == 0, result.stderr
    out = box.tmp / f"notices-{TARGET}"
    assert (out / "LICENSE").read_text() == "the licence of the repository\n"
    assert (out / "NOTICE").read_text() == "the notice of the repository\n"
    text = (out / "THIRD_PARTY_NOTICES.txt").read_text()
    notice = text.split("\n\n", 1)[0]
    assert notice.startswith(LGPL_NOTICE_START) and SRPM in notice and f"antb1-{TARGET}.sources.tar" in notice
    assert labels(text) == [
        "Apache Arrow 25.0.0: LICENSE.txt",
        "Apache Arrow 25.0.0: NOTICE.txt",
        "Apache Thrift 0.22.0: LICENSE",
        "Apache Thrift 0.22.0: NOTICE",
        "CLI11 2.7.2: LICENSE",
        "GNU C Library (glibc-2.28-1.el8_9.1): COPYING",
        "GNU C Library (glibc-2.28-1.el8_9.1): COPYING.LIB",
        "GNU C Library (glibc-2.28-1.el8_9.1): LICENSES",
        "GCC 15.3.0 runtime libraries (libstdc++, libgcc): RUNTIME.LIBRARY.EXCEPTION",
    ]
    assert "\n==== CLI11 2.7.2: LICENSE ====\n\nCLI11 licence\n" in text  # each file follows its label verbatim


def test_macos_notices_carry_libcxx_and_no_glibc(box: Box) -> None:
    result = box.notices("darwin-arm64")
    assert result.returncode == 0, result.stderr
    text = (box.tmp / "notices-darwin-arm64" / "THIRD_PARTY_NOTICES.txt").read_text()
    assert labels(text)[-2:] == ["CLI11 2.7.2: LICENSE", "libc++ 23.1.3: LICENSE.TXT"]
    assert LGPL_NOTICE_START not in text and "GNU C Library" not in text


def test_notices_refuse_a_sysroot_build_without_a_pinned_source(box: Box) -> None:
    box.write_glibc_lock("sysroot_linux-64-2.28-hother_9")
    result = box.notices()
    assert result.returncode == 1
    assert f"{SYSROOT} has no line in cmake/static/glibc-source.lock" in result.stderr


def test_notices_refuse_a_licence_without_a_pin_and_an_empty_one(box: Box) -> None:
    (box.prefix / "share" / "antb1-licenses" / "zlib-LICENSE").write_text("zlib\n")
    result = box.notices()
    assert result.returncode == 1 and "no single pin" in result.stderr and "zlib-" in result.stderr
    (box.prefix / "share" / "antb1-licenses" / "zlib-LICENSE").unlink()
    (box.root / "cmake" / "static" / "licenses" / "CLI11-LICENSE.txt").write_text("")
    result = box.notices()
    assert result.returncode == 1 and "licence file missing or empty" in result.stderr


# ---------------------------------------------------------------------------------------------------------------------
# sources


def sources_tarball(box: Box) -> tuple[dict[str, tarfile.TarInfo], dict[str, bytes]]:
    members, contents = {}, {}
    with tarfile.open(box.root / "build" / "static" / "sources" / f"antb1-{TARGET}.sources.tar") as tar:
        for info in tar:
            members[info.name] = info
            if info.isreg():
                contents[info.name] = tar.extractfile(info).read()
    return members, contents


def test_sources_hold_glibc_antb1_and_the_third_party_files_with_their_digests(box: Box) -> None:
    box.package()
    result = box.sources()
    assert result.returncode == 0, result.stderr
    top = f"antb1-{TARGET}-sources"
    members, contents = sources_tarball(box)
    files = [SRPM, "antb1-source.tar.gz", "third-party/apache-arrow-25.0.0.tar.gz", "third-party/thrift-0.22.0.tar.gz"]
    files += ["third-party/cli11-2.7.2-h0_0.conda", "third-party/libboost-headers-1.88.0-h0_0.conda"]
    assert set(members) == {top, f"{top}/third-party", f"{top}/README.txt", f"{top}/SHA256SUMS"} | {
        f"{top}/{name}" for name in files
    }
    assert all(info.uid == 0 and info.gid == 0 and not info.uname for info in members.values())
    assert contents[f"{top}/{SRPM}"] == GLIBC and contents[f"{top}/third-party/cli11-2.7.2-h0_0.conda"] == CLI11
    sums = {line.split("  ")[1]: line.split("  ")[0] for line in contents[f"{top}/SHA256SUMS"].decode().splitlines()}
    assert sums == {name: hashlib.sha256(contents[f"{top}/{name}"]).hexdigest() for name in files}
    assert box.commit in contents[f"{top}/README.txt"].decode()
    with tarfile.open(fileobj=io.BytesIO(contents[f"{top}/antb1-source.tar.gz"])) as source:
        assert source.pax_headers.get("comment") == box.commit  # git archive records its commit
        assert f"antb1-{VERSION}/LICENSE" in source.getnames()
    out = box.root / "build" / "static" / "sources"
    tarball = out / f"antb1-{TARGET}.sources.tar"
    assert (
        out / f"{tarball.name}.sha256"
    ).read_text() == f"{hashlib.sha256(tarball.read_bytes()).hexdigest()}  {tarball.name}\n"
    assert "GET https://mirror.test/glibc.src.rpm" not in box.requested()  # the first copy served it

    again = box.sources()
    assert again.returncode == 0, again.stderr
    assert box.requested() == []  # every file is in $ANTB1_ARROW_DIR/src now


def test_sources_retry_a_pass_once_after_a_stall(box: Box) -> None:
    box.in_turn(box.glibc_urls[0], {"exit": 28, "code": 0}, box.bytes_answer(GLIBC))
    box.package()
    result = box.sources()
    assert result.returncode == 0, result.stderr
    assert box.requested().count(f"GET {box.glibc_urls[0]}") == 2


def test_sources_refuse_bytes_that_are_not_the_pin(box: Box) -> None:
    box.serve(box.glibc_urls[0], b"another file")
    box.package()
    result = box.sources()
    assert result.returncode == 1
    assert "could not download every file of package-sources.lock" in result.stderr
    assert not (box.root / "build" / "static" / "sources" / f"antb1-{TARGET}.sources.tar").exists()


@pytest.mark.parametrize("change", ["tracked", "untracked"])
def test_sources_refuse_a_work_tree_with_changes(box: Box, change: str) -> None:
    box.package()
    (box.root / ("NOTICE" if change == "tracked" else "new.txt")).write_text("changed\n")
    result = box.sources()
    assert result.returncode == 1 and "the work tree has changes" in result.stderr
    assert box.requested() == []


@pytest.mark.parametrize("commit", ["dirty", "other", "github"])
def test_sources_refuse_a_package_from_another_commit(box: Box, commit: str) -> None:
    box.package({"dirty": box.commit + "-dirty", "other": "0" * 40, "github": box.commit}[commit])
    result = box.sources(**({"GITHUB_SHA": "1" * 40} if commit == "github" else {}))
    assert result.returncode == 1, result.stdout
    assert ("clean checkout" if commit == "dirty" else "run `pixi run static` again") in result.stderr


def test_sources_need_the_package_and_do_nothing_on_macos(box: Box) -> None:
    result = box.sources()
    assert result.returncode == 1 and "run `pixi run static` first" in result.stderr
    result = box.run("static_package.py", "sources", "--target", "darwin-arm64")
    assert result.returncode == 0 and "no sources to package" in result.stdout
    assert not (box.root / "build" / "static" / "sources").exists()


# ---------------------------------------------------------------------------------------------------------------------
# static_gates.py package


def pack(path: Path, top: str, files: dict[str, bytes], extra: tarfile.TarInfo | None = None) -> None:
    with tarfile.open(path, "w:gz") as tar:
        info = tarfile.TarInfo(top)
        info.type, info.mode = tarfile.DIRTYPE, 0o755
        tar.addfile(info)
        for name, data in files.items():
            info = tarfile.TarInfo(f"{top}/{name}")
            info.size, info.mode = len(data), 0o755 if name == "antb1" else 0o644
            tar.addfile(info, io.BytesIO(data))
        if extra is not None:
            tar.addfile(extra, io.BytesIO(b"x" * extra.size) if extra.isreg() else None)


def make_package(box: Box, break_it: str = "") -> Path:
    """A package as scripts/static.sh packs it, from the real notices, broken in one way if asked."""
    assert box.notices().returncode == 0
    texts = {name: (box.tmp / f"notices-{TARGET}" / name).read_bytes() for name in ("LICENSE", "NOTICE")}
    notices = (box.tmp / f"notices-{TARGET}" / "THIRD_PARTY_NOTICES.txt").read_bytes()
    if break_it == "no-lgpl-notice":
        notices = notices.split(b"\n\n", 1)[1]
    texts["THIRD_PARTY_NOTICES.txt"] = notices
    if break_it == "changed-notice":
        texts["NOTICE"] = b"someone else's notice\n"
    info = {"name": "antb1", "version": VERSION, "commit": box.commit, "target": TARGET, "flavor": "static-thinlto"}
    info |= {"license": "Apache-2.0", "sysroot": SYSROOT, "glibc_source": SRPM}
    readme = f"antb1 is licensed under the Apache License 2.0. Sources: antb1-{TARGET}.sources.tar\n".encode()
    files = {"antb1": b"\x7fELF binary", **texts, "BUILDINFO.json": json.dumps(info).encode(), "README.txt": readme}
    if break_it == "no-license":
        del files["LICENSE"]
    extra = None
    if break_it in ("symlink", "dotdot", "extra"):
        extra = tarfile.TarInfo({"symlink": "x/link", "dotdot": "x/../../evil", "extra": "x/notes.txt"}[break_it])
        extra.name = extra.name.replace("x/", f"antb1-{VERSION}-{TARGET}/", 1)
        if break_it == "symlink":
            extra.type, extra.linkname = tarfile.SYMTYPE, "/etc/passwd"
        else:
            extra.size = 1
    pkg = box.tmp / "package"
    pkg.mkdir(exist_ok=True)
    top = f"antb1-{VERSION}-{TARGET}"
    pack(pkg / f"antb1-{TARGET}.tar.gz", top, files, extra)
    debug = {"antb1.debug": b"line tables", **{name: texts[name] for name in ("LICENSE", "NOTICE")}}
    if break_it != "debug-without-notices":
        debug["THIRD_PARTY_NOTICES.txt"] = notices
    pack(pkg / f"antb1-{TARGET}.debug.tar.gz", top, debug)
    sums = "".join(
        f"{hashlib.sha256((pkg / name).read_bytes()).hexdigest()}  {name}\n"
        for name in (f"antb1-{TARGET}.tar.gz", f"antb1-{TARGET}.debug.tar.gz")
    )
    (pkg / "SHA256SUMS").write_text(sums.replace(sums[:8], "00000000", 1) if break_it == "bad-sum" else sums)
    return pkg


def gate(box: Box, pkg: Path) -> subprocess.CompletedProcess[str]:
    return box.run("static_gates.py", "package", "--pkg", str(pkg), "--target", TARGET)


def test_the_package_gate_passes_a_package_as_static_sh_packs_it(box: Box) -> None:
    result = gate(box, make_package(box))
    assert result.returncode == 0, result.stdout + result.stderr
    assert "FAIL" not in result.stdout and "static-gates: OK" in result.stdout


@pytest.mark.parametrize(
    ("break_it", "failure"),
    [
        ("no-license", "holds exactly"),
        ("changed-notice", "the package's NOTICE is the repository's"),
        ("extra", "holds exactly"),
        ("symlink", "is a regular file inside"),
        ("dotdot", "is a regular file inside"),
        ("no-lgpl-notice", "starts with the LGPL notice"),
        ("debug-without-notices", "debug.tar.gz holds exactly"),
        ("bad-sum", "matches SHA256SUMS"),
    ],
)
def test_the_package_gate_fails_a_broken_package(box: Box, break_it: str, failure: str) -> None:
    result = gate(box, make_package(box, break_it))
    assert result.returncode == 1
    assert any(line.startswith("static-gates: FAIL") and failure in line for line in result.stdout.splitlines()), (
        result.stdout
    )
