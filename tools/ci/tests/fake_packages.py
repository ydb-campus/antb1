"""The sandbox and the packages of the static build's tests (test_static_package.py, test_static_verify.py): a
committed copy of the scripts and locks, a pixi environment and an Arrow prefix of a few files, and packages packed
as scripts/static.sh packs them, with the staged files it leaves next to them.
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

from fakes import FAKE_CMAKE, FAKE_CURL, FakeNet, install

REPO = Path(__file__).resolve().parents[3]
TARGET = "linux-x86_64"
VERSION = "0.1.0"
SYSROOT = "sysroot_linux-64-2.28-hfake_9"
SRPM = "glibc-2.28-1.el8_9.1.src.rpm"
ARROW, THRIFT = b"arrow source", b"thrift source"
GLIBC = b"glibc source rpm"
CLI11, BOOST = b"cli11 conda archive", b"boost headers conda archive"
SCRIPTS = ("static_package.py", "static_gates.py", "static-sources.sh", "static-check.sh")


class Box(FakeNet):
    """A committed copy of the scripts and locks, a pixi environment and an Arrow prefix with licence files."""

    def __init__(self, tmp: Path) -> None:
        super().__init__(tmp)
        self.root = tmp / "root"
        for name in SCRIPTS:
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
        self.tmpdir = tmp / "tmpdir"  # TMPDIR of every script run here
        self.tmpdir.mkdir()

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

    def script_env(self, **env: str) -> dict[str, str]:
        """The environment of a script run here: the fakes first on PATH, no Docker daemon, TMPDIR in the box."""
        return {
            "PATH": f"{self.bin}{os.pathsep}{os.environ['PATH']}",
            "CONDA_PREFIX": str(self.env),
            "ANTB1_ARROW_DIR": str(self.arrow_dir),
            "ANTB1_SOURCES_RETRY_DELAY": "0",
            "DOCKER_HOST": "unix:///nonexistent",
            "TMPDIR": str(self.tmpdir),
            **self.git_env,
            **self.curl_env(),
            **env,
        }

    def run(self, script: str, *args: str, **env: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-I", str(self.root / "scripts" / script), *args],
            cwd=self.root,
            env=self.script_env(**env),
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )

    def bash(self, script: str, *args: str, **env: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", str(self.root / "scripts" / script), *args],
            cwd=self.root,
            env=self.script_env(PIXI_PROJECT_ROOT=str(self.root), **env),
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


def pack(
    path: Path,
    top: str,
    files: dict[str, bytes],
    extra: tarfile.TarInfo | None = None,
    exe: int = 0o755,
    pax: dict[str, str] | None = None,
) -> None:
    """A .tar.gz of `top`/ and `files` (antb1 with the mode `exe` and the pax headers `pax`), then `extra`."""
    with tarfile.open(path, "w:gz", format=tarfile.PAX_FORMAT) as tar:
        info = tarfile.TarInfo(top)
        info.type, info.mode = tarfile.DIRTYPE, 0o755
        tar.addfile(info)
        for name, data in files.items():
            info = tarfile.TarInfo(f"{top}/{name}")
            info.size, info.mode = len(data), exe if name == "antb1" else 0o644
            if name == "antb1" and pax:
                info.pax_headers = pax
            tar.addfile(info, io.BytesIO(data))
        if extra is not None:
            tar.addfile(extra, io.BytesIO(b"x" * extra.size) if extra.isreg() else None)


def stage(directory: Path, files: dict[str, bytes], exe: int = 0o755) -> None:
    """The files as scripts/static.sh leaves them next to the package, after packing them."""
    directory.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (directory / name).write_bytes(data)
        (directory / name).chmod(exe if name in ("antb1", "antb1.debug") else 0o644)


def make_package(
    box: Box, break_it: str = "", target: str = TARGET, antb1: bytes = b"\x7fELF binary", where: Path | None = None
) -> Path:
    """A package as scripts/static.sh packs it, from the real notices, broken in one way if asked; `antb1` is the
    packaged binary's bytes."""
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
    files = {"antb1": antb1, **texts, "BUILDINFO.json": json.dumps(info).encode(), "README.txt": readme}
    if break_it == "no-license":
        del files["LICENSE"]
    top = f"antb1-{VERSION}-{target}"
    extra = None
    if break_it in ("symlink", "dotdot", "extra", "duplicate"):
        name = {"symlink": "link", "dotdot": "../../evil", "extra": "notes.txt", "duplicate": "antb1"}[break_it]
        extra = tarfile.TarInfo(f"{top}/{name}")
        if break_it == "symlink":
            extra.type, extra.linkname = tarfile.SYMTYPE, "/etc/passwd"
        else:
            extra.size = 1
    pkg = where or box.tmp / "package"
    pkg.mkdir(exist_ok=True)
    assets = [f"antb1-{target}.tar.gz"]
    exe = 0o644 if break_it == "not-executable" else 0o755
    pax = {"SCHILY.xattr.com.apple.provenance": "x"} if break_it == "xattr" else None
    pack(pkg / assets[0], top, files, extra, exe, pax)
    stage(pkg / top, files | ({"antb1": antb1 + b"!"} if break_it == "staged-differs" else {}), exe)
    if linux:
        debug = {"antb1.debug": b"line tables", **{name: texts[name] for name in ("LICENSE", "NOTICE")}}
        if break_it != "debug-without-notices":
            debug["THIRD_PARTY_NOTICES.txt"] = notices + (b"changed\n" if break_it == "debug-other-notices" else b"")
        assets.append(f"antb1-{target}.debug.tar.gz")
        pack(pkg / assets[1], top, debug)
        stage(pkg / "debug" / top, debug)
    sums = "".join(f"{hashlib.sha256((pkg / name).read_bytes()).hexdigest()}  {name}\n" for name in assets)
    if break_it == "bad-sum":
        sums = "00000000" + sums[8:]
    if break_it == "extra-sum":
        sums += f"{'0' * 64}  antb1-{target}.sources.tar\n"
    (pkg / "SHA256SUMS").write_text(sums)
    return pkg
