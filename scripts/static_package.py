#!/usr/bin/env python3
"""Licence texts and sources of the standalone package (docs/adr/0034-licence-and-static-sources.md), run in env
`static`.

  notices  LICENSE and NOTICE (the repository's) and THIRD_PARTY_NOTICES.txt into --out, for scripts/static.sh: the
           licence of every library linked into the binary, one section each, labelled with the library and its
           version. On Linux the file starts with the notice that the GNU C Library is linked statically under the
           LGPL and where its source is. Exits 1 when the environment's sysroot build has no line in
           cmake/static/glibc-source.lock, so a package never names another source than the one linked.
  glibc    prints `<sysroot build> <source file>` of the environment (Linux), for BUILDINFO.json and README.txt.
  sources  `pixi run static-sources` (NETWORK once; Linux), after `pixi run static`: the sources of everything linked
           into the packaged binary but the GCC runtime that comes with the compiler, as LGPL-2.1 section 6 asks, in
           build/static/sources/antb1-<target>.sources.tar with its .sha256: glibc's source RPM, antb1's source at
           the packaged commit and the third-party sources (the pins of cmake/static/arrow-sources.lock and the conda
           packages compiled in). Every download goes through scripts/static-sources.sh into $ANTB1_ARROW_DIR/src,
           where a file that matches its pin is kept. With --check-urls it only checks every copy of glibc's source
           RPMs and of the conda packages (scripts/static-sources.sh --check-urls).
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import platform
import re
import subprocess
import sys
import tarfile
import tempfile
import textwrap
import time
from pathlib import Path
from typing import NoReturn

ROOT = Path(__file__).resolve().parent.parent
ARROW_LOCK = ROOT / "cmake" / "static" / "arrow-sources.lock"
GLIBC_LOCK = ROOT / "cmake" / "static" / "glibc-source.lock"
LICENSES = ROOT / "cmake" / "static" / "licenses"
TARGETS = {
    ("Linux", "x86_64"): "linux-x86_64",
    ("Linux", "aarch64"): "linux-aarch64",
    ("Darwin", "arm64"): "darwin-arm64",
}
# Header-only libraries from conda-forge that are compiled into the binary: src/cli uses CLI11, and Arrow's bundled
# Thrift includes Boost's headers. Their packages go into the sources.
COMPILED_PACKAGES = ("cli11", "libboost-headers")
# The first words of THIRD_PARTY_NOTICES.txt on Linux; scripts/static_gates.py checks for them.
LGPL_NOTICE_START = "This program contains the GNU C Library"
NAMES = {"arrow": "Apache Arrow", "thrift": "Apache Thrift"}


def fail(message: str, tool: str = "static-package") -> NoReturn:
    print(f"{tool}: {message}", file=sys.stderr)
    sys.exit(1)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def conda_meta(env: Path, name: str) -> dict:
    """The one installed package whose name is `name`, or whose name starts with it when it ends with `_`."""
    found = []
    for path in sorted((env / "conda-meta").glob(f"{name}*.json")):
        meta = json.loads(path.read_text())
        if meta.get("name") == name or (name.endswith("_") and meta.get("name", "").startswith(name)):
            found.append(meta)
    if len(found) != 1:
        fail(f"{len(found)} installed packages named {name}* in {env}/conda-meta, expected one")
    return found[0]


def arrow_pins() -> list[str]:
    return [line.split()[0] for line in ARROW_LOCK.read_text().splitlines() if line.strip() and line[0] != "#"]


def glibc_pins() -> dict[str, tuple[str, str, int, list[str]]]:
    """cmake/static/glibc-source.lock: sysroot build -> (source file, sha256, bytes, URLs)."""
    pins: dict[str, tuple[str, str, int, list[str]]] = {}
    for no, line in enumerate(GLIBC_LOCK.read_text().splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split()
        ok = len(fields) >= 5 and fields[0] not in pins and "/" not in fields[1]
        ok = ok and bool(re.fullmatch(r"[0-9a-f]{64}", fields[2])) and fields[3].isdigit()
        if not ok or not all(url.startswith("https://") for url in fields[4:]):
            fail(f"{GLIBC_LOCK.name}:{no}: malformed or repeated line (expected: <sysroot build> <source file> "
                 "<sha256> <bytes> <https url> [<https url>...])")  # fmt: skip
        pins[fields[0]] = (fields[1], fields[2], int(fields[3]), fields[4:])
    return pins


def glibc_source(env: Path) -> tuple[str, str, str, int, list[str]]:
    """The environment's sysroot build and its pinned source: (build, file, sha256, bytes, URLs)."""
    builds = sorted(path.stem for path in (env / "conda-meta").glob("sysroot_linux-*.json"))
    if len(builds) != 1:
        fail(f"{len(builds)} sysroot_linux-* packages in {env}/conda-meta, expected one")
    pins = glibc_pins()
    if builds[0] not in pins:
        fail(f"the sysroot build {builds[0]} has no line in cmake/static/glibc-source.lock: pin the source RPM of "
             "the glibc it contains there (docs/recipes/dependency-update.md)")  # fmt: skip
    return (builds[0], *pins[builds[0]])


def prefix_label(path: Path, pins: list[str]) -> str:
    """`brotli-LICENSE` -> `brotli 1.0.9: LICENSE`, by the pinned tarball of the same library."""
    library, _, member = path.name.partition("-")
    start = "apache-arrow-" if library == "arrow" else f"{library}-"
    tarballs = [pin for pin in pins if pin.startswith(start)]
    if not member or len(tarballs) != 1:
        fail(f"{path}: no single pin of cmake/static/arrow-sources.lock starts with {start}")
    version = tarballs[0][len(start) :].removesuffix(".tar.gz").removeprefix("v")
    if re.fullmatch(r"[0-9a-f]{40}", version):
        version = version[:12]
    return f"{NAMES.get(library, library)} {version}: {member}"


def lgpl_notice(target: str, source: str, gcc: str) -> str:
    version = source.split("-")[1]
    return paragraph(
        f"{LGPL_NOTICE_START} (glibc) {version}, linked statically: AlmaLinux's build from the source package "
        f"{source}. glibc is licensed under the GNU Lesser General Public License, version 2.1 or (at your option) "
        "any later version; its text is the COPYING.LIB section below. The complete source code of glibc, of antb1 and "
        f"of every other library linked into this program, except the GCC {gcc} runtime libraries (libstdc++, libgcc), "
        f"is in antb1-{target}.sources.tar, published next to this package (in the same release or workflow run as "
        f"antb1-{target}.tar.gz). The GCC runtime comes with the compiler: the pixi.lock in antb1's source pins it "
        f"(conda-forge's libstdcxx-devel and libgcc-devel {gcc}), `pixi run static` links that build again, and its "
        "licence is the RUNTIME.LIBRARY.EXCEPTION section below. You may modify glibc and relink this program with "
        "it: the README.txt in that file says how. The licences of antb1 and of every library linked into it permit "
        "modification of this program for your own use and reverse engineering to debug such modifications."
    )


def paragraph(text: str) -> str:
    return textwrap.fill(text, width=116, break_long_words=False, break_on_hyphens=False) + "\n\n"


def cmd_notices(args: argparse.Namespace) -> None:
    env, prefix, out = Path(args.env), Path(args.prefix), Path(args.out)
    sections = [
        (prefix_label(path, arrow_pins()), path) for path in sorted((prefix / "share/antb1-licenses").iterdir())
    ]
    sections.append((f"CLI11 {conda_meta(env, 'cli11')['version']}: LICENSE", LICENSES / "CLI11-LICENSE.txt"))
    boost = conda_meta(env, "libboost-headers")["version"]  # Arrow's bundled Thrift compiles Boost's headers in
    sections.append((f"Boost {boost}: LICENSE_1_0.txt", LICENSES / "Boost-LICENSE_1_0.txt"))
    head = ""
    if args.target.startswith("linux-"):
        source = glibc_source(env)[1]
        nevr = source.removesuffix(".src.rpm")
        sysroots = sorted(env.glob("*-conda-linux-gnu/sysroot"))
        if len(sysroots) != 1:
            fail(f"{len(sysroots)} sysroots in {env}, expected one")
        for name in ("COPYING", "COPYING.LIB", "LICENSES"):
            sections.append((f"GNU C Library ({nevr}): {name}", sysroots[0] / "usr/share/licenses/glibc" / name))
        gcc = conda_meta(env, "libstdcxx-devel_")["version"]  # the libstdc++ and libgcc that are linked
        exception = env / "share/licenses/libstdc++/RUNTIME.LIBRARY.EXCEPTION"
        sections.append((f"GCC {gcc} runtime libraries (libstdc++, libgcc): RUNTIME.LIBRARY.EXCEPTION", exception))
        head = lgpl_notice(args.target, source, gcc)
    else:
        libcxx = conda_meta(env, "libcxx-devel")["version"]
        sections.append((f"libc++ {libcxx}: LICENSE.TXT", LICENSES / "libcxx-LICENSE.txt"))
    text = (
        head
        + paragraph(
            f"antb1 ({args.target}) is linked statically with the libraries below; their licences follow, one section "
            "each. antb1's own licence is in LICENSE and NOTICE next to this file. Apache Arrow's LICENSE and NOTICE "
            "also cover the code that Arrow bundles."
        ).rstrip("\n")
        + "\n"
    ).encode()
    for label, path in sections:
        if not path.is_file() or path.stat().st_size == 0:
            fail(f"licence file missing or empty: {path}")
        text += f"\n==== {label} ====\n\n".encode() + path.read_bytes()
    out.mkdir(parents=True, exist_ok=True)
    for name in ("LICENSE", "NOTICE"):
        (out / name).write_bytes((ROOT / name).read_bytes())
    (out / "THIRD_PARTY_NOTICES.txt").write_bytes(text)
    print(f"static-package: {out}: LICENSE, NOTICE, THIRD_PARTY_NOTICES.txt ({len(sections)} sections)")


def cmd_glibc(args: argparse.Namespace) -> None:
    build, source = glibc_source(Path(args.env))[:2]
    print(build, source)


# ---------------------------------------------------------------------------------------------------------------------
# sources


def git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(ROOT), *args], check=True, capture_output=True, text=True).stdout.strip()


def arrow_src() -> Path:
    """$ANTB1_ARROW_DIR/src, the download cache of scripts/static-arrow.sh (resolved as it resolves it)."""
    arrow_dir = Path(os.environ.get("ANTB1_ARROW_DIR") or Path.home() / ".cache/antb1/arrow")
    if not arrow_dir.is_absolute():
        arrow_dir = ROOT / arrow_dir
    arrow_dir.mkdir(parents=True, exist_ok=True)
    arrow_dir = arrow_dir.resolve()
    inside = arrow_dir == ROOT.resolve() or ROOT.resolve() in arrow_dir.parents
    if inside and subprocess.run(["git", "-C", str(ROOT), "check-ignore", "-q", str(arrow_dir / "src")]).returncode:
        fail(f"ANTB1_ARROW_DIR={arrow_dir} is inside the repository but not ignored by git", "static-sources")
    return arrow_dir / "src"


def fetch(lock: Path, cache: Path) -> None:
    """Every file of `lock` into `cache`; one more pass after a failure, which downloads only what is missing."""
    cmd = ["bash", str(ROOT / "scripts/static-sources.sh"), str(lock), "--fetch", str(cache)]
    if subprocess.run(cmd).returncode == 0:
        return
    time.sleep(float(os.environ.get("ANTB1_SOURCES_RETRY_DELAY", "3")))
    if subprocess.run(cmd).returncode != 0:
        fail(f"could not download every file of {lock.name}; its copies are tried in order, so retry later or add "
             "a copy", "static-sources")  # fmt: skip


def conda_pin(meta: dict) -> str:
    """A conda package as a pin of scripts/static-sources.sh: conda-forge's file, then prefix.dev's copy."""
    copy = f"https://prefix.dev/conda-forge/{meta['subdir']}/{meta['fn']}"
    return f"{meta['fn']} {meta['sha256']} {meta['size']} - {meta['url']} {copy}"


def check_urls(env: Path, linux: bool) -> None:
    """Every copy of every source RPM in cmake/static/glibc-source.lock and, on Linux, of the conda packages."""
    urls: dict[str, tuple[str, int, list[str]]] = {}
    for source, sha, size, copies in glibc_pins().values():
        known = urls.setdefault(source, (sha, size, []))
        known[2].extend(url for url in copies if url not in known[2])
    lines = [f"{source} {sha} {size} - {' '.join(copies)}" for source, (sha, size, copies) in sorted(urls.items())]
    if linux:
        lines += [conda_pin(conda_meta(env, name)) for name in COMPILED_PACKAGES]
    with tempfile.TemporaryDirectory(prefix="antb1-static-sources-") as tmp:
        lock = Path(tmp) / "package-sources.lock"
        lock.write_text("".join(line + "\n" for line in lines))
        cmd = ["bash", str(ROOT / "scripts/static-sources.sh"), str(lock), "--check-urls"]
        if subprocess.run(cmd).returncode != 0:
            fail("a copy of a source RPM in cmake/static/glibc-source.lock or of a conda package is dead or serves "
                 "other bytes", "static-sources")  # fmt: skip


def cmd_sources(args: argparse.Namespace) -> None:
    target = args.target or TARGETS.get((platform.system(), platform.machine()))
    if target is None:
        fail(f"unsupported platform {platform.system()} {platform.machine()}", "static-sources")
    env = Path(os.environ.get("CONDA_PREFIX", ""))
    if args.check_urls:
        check_urls(env, target.startswith("linux-"))
        return
    if not target.startswith("linux-"):
        print(f"static-sources: {target} links no library under the LGPL: no sources to package")
        return
    infos = sorted(Path(args.pkg).glob(f"antb1-*-{target}/BUILDINFO.json"))
    if len(infos) != 1:
        fail(f"{len(infos)} packages for {target} in {args.pkg}: run `pixi run static` first", "static-sources")
    info = json.loads(infos[0].read_text())
    commit, version = info.get("commit", ""), info.get("version", "")
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or not version:
        fail(f"{infos[0]} names commit '{commit}': package a clean checkout, then run this again", "static-sources")
    head = os.environ.get("GITHUB_SHA") or git("rev-parse", "HEAD")
    if commit != head:
        fail(f"the package is from {commit}, the checkout is at {head}: run `pixi run static` again", "static-sources")
    dirty = subprocess.run(["git", "-C", str(ROOT), "diff", "--quiet", "HEAD"]).returncode != 0
    if dirty or git("ls-files", "--others", "--exclude-standard"):
        fail("the work tree has changes, so antb1's source would not be what was built", "static-sources")

    build, srpm, srpm_sha, srpm_size, srpm_urls = glibc_source(env)
    files = {srpm: (srpm_sha, f"{srpm} {srpm_sha} {srpm_size} - {' '.join(srpm_urls)}")}
    third_party = {}
    for line in ARROW_LOCK.read_text().splitlines():
        if line.strip() and line[0] != "#":
            third_party[line.split()[0]] = line.split()[1]
    for name in COMPILED_PACKAGES:
        meta = conda_meta(env, name)
        third_party[meta["fn"]] = meta["sha256"]
        files[meta["fn"]] = (meta["sha256"], conda_pin(meta))
    cache = arrow_src()
    fetch(ARROW_LOCK, cache)
    with tempfile.TemporaryDirectory(prefix="antb1-static-sources-") as tmp:
        lock = Path(tmp) / "package-sources.lock"
        lock.write_text("".join(line + "\n" for _, line in files.values()))
        fetch(lock, cache)
        archive = Path(tmp) / "antb1-source.tar.gz"
        git("archive", "--format=tar.gz", f"--prefix=antb1-{version}/", "-o", str(archive), commit)

        top = f"antb1-{target}-sources"
        members = [(srpm, cache / srpm, srpm_sha), ("antb1-source.tar.gz", archive, sha256(archive))]
        members += [(f"third-party/{name}", cache / name, digest) for name, digest in sorted(third_party.items())]
        for _, path, digest in members:
            if sha256(path) != digest:
                fail(f"{path} is not the pinned file (sha256 differs)", "static-sources")
        sums = "".join(f"{digest}  {arcname}\n" for arcname, _, digest in sorted(members))
        readme = sources_readme(target, version, commit, build, srpm)
        mtime = int(git("show", "-s", "--format=%ct", commit))

        out = Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        tarball = out / f"antb1-{target}.sources.tar"
        for stale in (tarball, out / f"{tarball.name}.sha256"):
            stale.unlink(missing_ok=True)
        partial = out / f"{tarball.name}.partial"
        with tarfile.open(partial, "w", format=tarfile.PAX_FORMAT) as tar:
            for directory in (top, f"{top}/third-party"):
                tar.addfile(tar_info(directory, 0, mtime, tarfile.DIRTYPE, 0o755))
            for arcname, data in (("README.txt", readme.encode()), ("SHA256SUMS", sums.encode())):
                tar.addfile(tar_info(f"{top}/{arcname}", len(data), mtime), io.BytesIO(data))
            for arcname, path, _ in members:
                with path.open("rb") as f:
                    tar.addfile(tar_info(f"{top}/{arcname}", path.stat().st_size, mtime), f)
        partial.rename(tarball)
    (out / f"{tarball.name}.sha256").write_text(f"{sha256(tarball)}  {tarball.name}\n")
    size = tarball.stat().st_size / 1e6
    print(f"static-sources: OK: {tarball} ({size:.0f} MB): {srpm}, antb1-source.tar.gz ({commit[:12]}) and "
          f"{len(third_party)} third-party files")  # fmt: skip


def tar_info(name: str, size: int, mtime: int, kind: bytes = tarfile.REGTYPE, mode: int = 0o644) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.size, info.mtime, info.type, info.mode = size, mtime, kind, mode
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    return info


def sources_readme(target: str, version: str, commit: str, build: str, srpm: str) -> str:
    glibc = srpm.split("-")[1]
    return f"""\
The sources of antb1-{target}.tar.gz: antb1 {version}, commit {commit}.

That binary links the GNU C Library (glibc) statically, which the GNU LGPL 2.1 (section 6) allows when the sources
of the library and of the program that uses it come with it. They are here, except the GCC runtime libraries
(libstdc++, libgcc), which come with the compiler that pixi installs from antb1's pixi.lock. SHA256SUMS lists their
digests.

  {srpm}
      glibc, licensed under the LGPL 2.1 or later: AlmaLinux's source package of the static archives in the binary.
      The sysroot build {build} repackages them unchanged
      (cmake/static/glibc-source.lock).
  antb1-source.tar.gz
      antb1 at commit {commit}, licensed under the Apache License 2.0.
  third-party/
      the other libraries compiled into the binary: Apache Arrow and the libraries it bundles, pinned in
      cmake/static/arrow-sources.lock, and the conda-forge packages of CLI11 and Boost's headers.

Rebuild the binary on Linux (pixi 0.81.0 or newer installs the toolchain; the Arrow sources come from here). The
environment it installs, `static`, also contains duckdb-extension-tpch: TPC's dbgen under the TPC End User License
Agreement v2.2. Only antb1's tests use it, but the build configures with it, and installing the environment installs
it and accepts that EULA (CONTRIBUTING.md, docs/adr/0006-test-strategy-and-data-policy.md).

  tar -xzf antb1-source.tar.gz && cd antb1-{version}
  mkdir -p ~/.cache/antb1/arrow/src && cp ../third-party/*.tar.gz ~/.cache/antb1/arrow/src/
  pixi run static          # the package is then in build/static/package

Relink it with a modified glibc: build glibc from the source package (rpmbuild --rebuild), then copy every static
archive and start file of its glibc-static and glibc-devel packages (usr/lib64: libc.a, libpthread.a, librt.a,
libdl.a, crt1.o, crti.o, crtn.o, and libm.a with what it names: on x86_64 a linker script for libm-{glibc}.a and
libmvec.a) into the environment's sysroot (.pixi/envs/static/<arch>-conda-linux-gnu/sysroot/usr/lib64) with
`cp --remove-destination`: pixi hard-links those files to its package cache, so a plain `cp` would write the
modified glibc into that cache and into every environment installed from it. Then run `pixi run static` again.
"""


def cmd_buildinfo(args: argparse.Namespace) -> None:
    """BUILDINFO.json of the package (scripts/static.sh): what the binary is and how it was built. A tag run (the
    Release workflow; GitHub sets GITHUB_REF_TYPE=tag) records its tag, so a release candidate's package names its
    release; .github/scripts/release.py's assemble requires it on a tag push."""
    tag = os.environ.get("GITHUB_REF_NAME", "") if os.environ.get("GITHUB_REF_TYPE") == "tag" else ""
    info = {
        "name": "antb1",
        "version": args.version,
        "commit": args.commit,
        "target": args.target,
        "flavor": "static-thinlto",
        "license": "Apache-2.0",
        "source": args.source,
        "arrow_prefix": args.arrow_prefix,
        "arrow_sources": "cmake/static/arrow-sources.lock",
        "sysroot": args.sysroot,
        "glibc_source": args.glibc_source,
        "pixi_lock_sha256": sha256(ROOT / "pixi.lock"),
        "compiler": args.compiler,
        "runner": args.runner,
        "tag": tag,
    }
    for key in ("source", "sysroot", "glibc_source", "tag"):  # no source URL for a -dirty build; no glibc on macOS
        if not info[key]:
            del info[key]
    Path(args.out).write_text(json.dumps(info, indent=2))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("buildinfo")
    b.add_argument("--out", required=True)
    b.add_argument("--target", required=True, choices=sorted(TARGETS.values()))
    for name in ("version", "commit", "arrow-prefix", "compiler", "runner"):
        b.add_argument(f"--{name}", required=True)
    for name in ("source", "sysroot", "glibc-source"):  # empty for a -dirty build, and for macOS
        b.add_argument(f"--{name}", default="")
    n = sub.add_parser("notices")
    n.add_argument("--target", required=True, choices=sorted(TARGETS.values()))
    n.add_argument("--prefix", required=True, help="the static Arrow prefix (scripts/static-arrow.sh)")
    n.add_argument("--env", required=True, help="the pixi environment ($CONDA_PREFIX)")
    n.add_argument("--out", required=True)
    g = sub.add_parser("glibc")
    g.add_argument("--env", required=True)
    s = sub.add_parser("sources")
    s.add_argument("--target", choices=sorted(TARGETS.values()), help="default: this host's")
    s.add_argument("--pkg", default=str(ROOT / "build/static/package"), help="the output of `pixi run static`")
    s.add_argument("--out", default=str(ROOT / "build/static/sources"))
    s.add_argument("--check-urls", action="store_true", help="only check every copy of the downloaded sources")
    args = parser.parse_args()
    {"buildinfo": cmd_buildinfo, "notices": cmd_notices, "glibc": cmd_glibc, "sources": cmd_sources}[args.cmd](args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
