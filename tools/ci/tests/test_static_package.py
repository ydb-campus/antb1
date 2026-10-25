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
        (static / "licenses" / "Boost-LICENSE_1_0.txt").write_text("Boost licence\n")
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


def sections(pairs: list[tuple[str, str]]) -> str:
    """How THIRD_PARTY_NOTICES.txt ends: each label, then the text of its file (one line each in the Box)."""
    return "".join(f"\n==== {label} ====\n\n{body}\n" for label, body in pairs)


PREFIX_SECTIONS = [
    ("Apache Arrow 25.0.0: LICENSE.txt", "arrow-LICENSE.txt text"),
    ("Apache Arrow 25.0.0: NOTICE.txt", "arrow-NOTICE.txt text"),
    ("Apache Thrift 0.22.0: LICENSE", "thrift-LICENSE text"),
    ("Apache Thrift 0.22.0: NOTICE", "thrift-NOTICE text"),
    ("CLI11 2.7.2: LICENSE", "CLI11 licence"),
    ("Boost 1.88.0: LICENSE_1_0.txt", "Boost licence"),
]


# ---------------------------------------------------------------------------------------------------------------------
# notices


def test_linux_notices_start_with_the_lgpl_notice_and_label_every_section(box: Box) -> None:
    result = box.notices()
    assert result.returncode == 0, result.stderr
    out = box.tmp / f"notices-{TARGET}"
    assert (out / "LICENSE").read_text() == "the licence of the repository\n"
    assert (out / "NOTICE").read_text() == "the notice of the repository\n"
    text = (out / "THIRD_PARTY_NOTICES.txt").read_text()
    notice = " ".join(text.split("\n\n", 1)[0].split())
    assert notice.startswith(LGPL_NOTICE_START) and SRPM in notice and f"antb1-{TARGET}.sources.tar" in notice
    assert "except the GCC 15.3.0 runtime libraries (libstdc++, libgcc)" in notice
    expected = [
        *PREFIX_SECTIONS,
        ("GNU C Library (glibc-2.28-1.el8_9.1): COPYING", "glibc COPYING"),
        ("GNU C Library (glibc-2.28-1.el8_9.1): COPYING.LIB", "glibc COPYING.LIB"),
        ("GNU C Library (glibc-2.28-1.el8_9.1): LICENSES", "glibc LICENSES"),
        ("GCC 15.3.0 runtime libraries (libstdc++, libgcc): RUNTIME.LIBRARY.EXCEPTION", "GCC exception"),
    ]
    assert labels(text) == [label for label, _ in expected]
    assert text.endswith(sections(expected))  # each file follows its label verbatim
    assert all(len(line) <= 116 for line in text.split("\n==== ", 1)[0].splitlines())


def test_macos_notices_carry_libcxx_and_no_glibc(box: Box) -> None:
    result = box.notices("darwin-arm64")
    assert result.returncode == 0, result.stderr
    text = (box.tmp / "notices-darwin-arm64" / "THIRD_PARTY_NOTICES.txt").read_text()
    expected = [*PREFIX_SECTIONS, ("libc++ 23.1.3: LICENSE.TXT", "libc++ licence")]
    assert labels(text) == [label for label, _ in expected]
    assert text.endswith(sections(expected))
    assert LGPL_NOTICE_START not in text and "GNU C Library" not in text


def test_notices_refuse_a_sysroot_build_without_a_pinned_source(box: Box) -> None:
    box.write_glibc_lock("sysroot_linux-64-2.28-hother_9")
    result = box.notices()
    assert result.returncode == 1
    assert f"{SYSROOT} has no line in cmake/static/glibc-source.lock" in result.stderr


def test_glibc_prints_the_sysroot_build_then_its_source_for_static_sh(box: Box) -> None:
    result = box.run("static_package.py", "glibc", "--env", str(box.env))
    assert result.returncode == 0, result.stderr
    assert result.stdout == f"{SYSROOT} {SRPM}\n"  # scripts/static.sh: read -r sysroot glibc_source
    box.write_glibc_lock("sysroot_linux-64-2.28-hother_9")
    result = box.run("static_package.py", "glibc", "--env", str(box.env))
    assert result.returncode == 1 and result.stdout == ""
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


ARROW_GETS = ["GET https://a.test/arrow.tgz", "GET https://a.test/t.tgz"]
CONDA_GETS = [
    f"GET https://conda.test/linux-64/{fn}" for fn in ("cli11-2.7.2-h0_0.conda", "libboost-headers-1.88.0-h0_0.conda")
]


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
    readme = " ".join(contents[f"{top}/README.txt"].decode().split())
    assert box.commit in readme and "except the GCC runtime libraries (libstdc++, libgcc)" in readme
    assert "They are all here" not in readme and "TPC End User License Agreement" in readme
    assert "cp --remove-destination" in readme and "libm-2.28.a" in readme
    with tarfile.open(fileobj=io.BytesIO(contents[f"{top}/antb1-source.tar.gz"])) as source:
        assert source.pax_headers.get("comment") == box.commit  # git archive records its commit
        assert f"antb1-{VERSION}/LICENSE" in source.getnames()
    out = box.root / "build" / "static" / "sources"
    tarball = out / f"antb1-{TARGET}.sources.tar"
    digest = hashlib.sha256(tarball.read_bytes()).hexdigest()
    assert (out / f"{tarball.name}.sha256").read_text() == f"{digest}  {tarball.name}\n"
    assert box.requested() == [*ARROW_GETS, f"GET {box.glibc_urls[0]}", *CONDA_GETS]  # the first copies served

    again = box.sources()
    assert again.returncode == 0, again.stderr
    assert box.requested() == []  # every file is in $ANTB1_ARROW_DIR/src now


def test_sources_retry_a_pass_once_after_a_stall(box: Box) -> None:
    box.in_turn(box.glibc_urls[0], {"exit": 28, "code": 0}, box.bytes_answer(GLIBC))
    box.package()
    result = box.sources()
    assert result.returncode == 0, result.stderr
    vault, mirror = (f"GET {url}" for url in box.glibc_urls)
    assert box.requested() == [*ARROW_GETS, vault, mirror, vault, *CONDA_GETS]  # a whole second pass


def test_sources_refuse_bytes_that_are_not_the_pin_after_two_passes(box: Box) -> None:
    box.serve(box.glibc_urls[0], b"another file")
    box.package()
    result = box.sources()
    assert result.returncode == 1
    assert "could not download every file of package-sources.lock" in result.stderr
    vault, mirror = (f"GET {url}" for url in box.glibc_urls)
    assert box.requested() == [*ARROW_GETS, vault, mirror, vault, mirror]  # two passes, not three
    assert not (box.root / "build" / "static" / "sources" / f"antb1-{TARGET}.sources.tar").exists()


def test_sources_use_a_cache_inside_the_checkout_only_where_git_ignores_it(box: Box) -> None:
    box.package()
    ignored = box.sources(ANTB1_ARROW_DIR=str(box.root / ".cache" / "arrow"))  # as static.yml sets it
    assert ignored.returncode == 0, ignored.stderr
    assert (box.root / ".cache" / "arrow" / "src" / SRPM).read_bytes() == GLIBC
    again = box.sources(ANTB1_ARROW_DIR=".cache/arrow")  # relative to the checkout, and the tree is still clean
    assert again.returncode == 0, again.stderr
    assert box.requested() == []
    tracked = box.sources(ANTB1_ARROW_DIR=str(box.root / "cache"))
    assert tracked.returncode == 1 and "not ignored by git" in tracked.stderr


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


def test_check_urls_needs_two_good_copies_of_every_source_rpm_and_conda_archive(box: Box) -> None:
    for name, data in (("cli11-2.7.2-h0_0.conda", CLI11), ("libboost-headers-1.88.0-h0_0.conda", BOOST)):
        box.serve(f"https://prefix.dev/conda-forge/linux-64/{name}", data)
    result = box.run("static_package.py", "sources", "--target", TARGET, "--check-urls")
    assert result.returncode == 1 and "glibc-source.lock" in result.stderr  # the mirror serves nothing
    assert f"GET {box.glibc_urls[1]}" in box.requested()
    box.serve(box.glibc_urls[1], GLIBC)
    result = box.run("static_package.py", "sources", "--target", TARGET, "--check-urls")
    assert result.returncode == 0, result.stdout + result.stderr
    assert {f"GET {url}" for url in box.glibc_urls} <= set(box.requested())  # every copy, not just the first
    assert not (box.root / "build" / "static" / "sources").exists()  # nothing is packaged


# ---------------------------------------------------------------------------------------------------------------------
# static_gates.py package


def pack(path: Path, top: str, files: dict[str, bytes], extra: tarfile.TarInfo | None = None, exe: int = 0o755) -> None:
    with tarfile.open(path, "w:gz") as tar:
        info = tarfile.TarInfo(top)
        info.type, info.mode = tarfile.DIRTYPE, 0o755
        tar.addfile(info)
        for name, data in files.items():
            info = tarfile.TarInfo(f"{top}/{name}")
            info.size, info.mode = len(data), exe if name == "antb1" else 0o644
            tar.addfile(info, io.BytesIO(data))
        if extra is not None:
            tar.addfile(extra, io.BytesIO(b"x" * extra.size) if extra.isreg() else None)


def make_package(box: Box, break_it: str = "", target: str = TARGET) -> Path:
    """A package as scripts/static.sh packs it, from the real notices, broken in one way if asked."""
    linux = target.startswith("linux-")
    assert box.notices("linux-x86_64" if break_it == "linux-notices" else target).returncode == 0
    made = box.tmp / f"notices-{'linux-x86_64' if break_it == 'linux-notices' else target}"
    texts = {name: (made / name).read_bytes() for name in ("LICENSE", "NOTICE")}
    notices = (made / "THIRD_PARTY_NOTICES.txt").read_bytes()
    if break_it == "no-lgpl-notice":
        notices = notices.split(b"\n\n", 1)[1]
    if break_it == "no-section":
        notices = notices.replace(b"==== CLI11 2.7.2: LICENSE ====\n", b"")
    texts["THIRD_PARTY_NOTICES.txt"] = notices
    if break_it == "changed-notice":
        texts["NOTICE"] = b"someone else's notice\n"
    info = {"name": "antb1", "version": VERSION, "commit": box.commit, "target": target, "flavor": "static-thinlto"}
    info |= {"license": "Apache-2.0"} | ({"sysroot": SYSROOT, "glibc_source": SRPM} if linux else {})
    if break_it == "other-srpm":
        info["glibc_source"] = "glibc-2.28-236.el8_9.13.src.rpm"
    if break_it in ("no-license-key", "no-sysroot"):
        del info[{"no-license-key": "license", "no-sysroot": "sysroot"}[break_it]]
    readme = b"antb1 is licensed under the Apache License 2.0.\n"
    if linux and break_it != "readme-without-sources":
        readme += f"Sources: antb1-{target}.sources.tar\n".encode()
    files = {"antb1": b"\x7fELF binary", **texts, "BUILDINFO.json": json.dumps(info).encode(), "README.txt": readme}
    if break_it == "no-license":
        del files["LICENSE"]
    top = f"antb1-{VERSION}-{target}"
    extra = None
    if break_it in ("symlink", "dotdot", "extra"):
        extra = tarfile.TarInfo(f"{top}/" + {"symlink": "link", "dotdot": "../../evil", "extra": "notes.txt"}[break_it])
        if break_it == "symlink":
            extra.type, extra.linkname = tarfile.SYMTYPE, "/etc/passwd"
        else:
            extra.size = 1
    pkg = box.tmp / "package"
    pkg.mkdir(exist_ok=True)
    assets = [f"antb1-{target}.tar.gz"]
    pack(pkg / assets[0], top, files, extra, 0o644 if break_it == "not-executable" else 0o755)
    if linux:
        debug = {"antb1.debug": b"line tables", **{name: texts[name] for name in ("LICENSE", "NOTICE")}}
        if break_it != "debug-without-notices":
            debug["THIRD_PARTY_NOTICES.txt"] = notices + (b"changed\n" if break_it == "debug-other-notices" else b"")
        assets.append(f"antb1-{target}.debug.tar.gz")
        pack(pkg / assets[1], top, debug)
    sums = "".join(f"{hashlib.sha256((pkg / name).read_bytes()).hexdigest()}  {name}\n" for name in assets)
    if break_it == "bad-sum":
        sums = "00000000" + sums[8:]
    if break_it == "extra-sum":
        sums += f"{'0' * 64}  antb1-{target}.sources.tar\n"
    (pkg / "SHA256SUMS").write_text(sums)
    return pkg


def gate(box: Box, pkg: Path, target: str = TARGET) -> subprocess.CompletedProcess[str]:
    return box.run("static_gates.py", "package", "--pkg", str(pkg), "--target", target)


@pytest.mark.parametrize("target", [TARGET, "darwin-arm64"])
def test_the_package_gate_passes_a_package_as_static_sh_packs_it(box: Box, target: str) -> None:
    result = gate(box, make_package(box, target=target), target)
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
        ("not-executable", "the packaged antb1 is executable"),
        ("no-lgpl-notice", "starts with the LGPL notice"),
        ("other-srpm", "starts with the LGPL notice naming glibc-2.28-236"),
        ("no-section", "has the sections this target needs"),
        ("no-license-key", "names the target, the licence and the flavor"),
        ("no-sysroot", "names the sysroot build"),
        ("readme-without-sources", "README.txt names the licence and antb1-linux-x86_64.sources.tar"),
        ("debug-without-notices", "debug.tar.gz holds exactly"),
        ("debug-other-notices", "carries the same licence texts"),
        ("bad-sum", "matches SHA256SUMS"),
        ("extra-sum", "SHA256SUMS lists exactly"),
    ],
)
def test_the_package_gate_fails_a_broken_package(box: Box, break_it: str, failure: str) -> None:
    result = gate(box, make_package(box, break_it))
    assert result.returncode == 1
    assert any(line.startswith("static-gates: FAIL") and failure in line for line in result.stdout.splitlines()), (
        result.stdout
    )


def test_the_package_gate_fails_a_macos_package_with_linux_notices(box: Box) -> None:
    result = gate(box, make_package(box, "linux-notices", "darwin-arm64"), "darwin-arm64")
    assert result.returncode == 1
    assert "static-gates: FAIL THIRD_PARTY_NOTICES.txt names no glibc" in result.stdout
