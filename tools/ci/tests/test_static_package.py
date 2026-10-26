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
import subprocess
import tarfile
from pathlib import Path

import pytest
from fake_packages import BOOST, CLI11, GLIBC, SRPM, SYSROOT, TARGET, VERSION, Box, make_package

LGPL_NOTICE_START = "This program contains the GNU C Library"


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
# static_package.py buildinfo

BUILDINFO_KEYS = ["name", "version", "commit", "target", "flavor", "license", "source", "arrow_prefix", "arrow_sources",
                  "sysroot", "glibc_source", "pixi_lock_sha256", "compiler", "runner"]  # fmt: skip


def buildinfo(box: Box, *extra: str, **env: str) -> dict[str, str]:
    (box.root / "pixi.lock").write_text("version: 7\n")
    out = box.tmp / "BUILDINFO.json"
    args = ["buildinfo", "--out", str(out), "--version", VERSION, "--commit", box.commit, "--target", TARGET,
            "--source", f"https://github.com/ydb-campus/antb1/tree/{box.commit}", "--arrow-prefix", "1a64a30529ed3973",
            "--sysroot", SYSROOT, "--glibc-source", SRPM, "--compiler", "clang version 23.1.3",
            "--runner", "ubuntu24 20261020.1", *extra]  # fmt: skip
    result = box.run("static_package.py", *args, **env)
    assert result.returncode == 0, result.stderr
    return json.loads(out.read_text())


@pytest.mark.parametrize(
    ("env", "tag"),
    [({"GITHUB_REF_TYPE": "tag", "GITHUB_REF_NAME": "v0.1.0-rc.1"}, "v0.1.0-rc.1"),
     ({"GITHUB_REF_TYPE": "branch", "GITHUB_REF_NAME": "main"}, None), ({}, None)],
)  # fmt: skip
def test_buildinfo_records_the_tag_only_on_a_tag_run(box: Box, env: dict[str, str], tag: str | None) -> None:
    info = buildinfo(box, **env)
    assert info.get("tag") == tag  # .github/scripts/release.py's assemble requires it on a tag push
    assert list(info) == BUILDINFO_KEYS + (["tag"] if tag else [])
    assert info["pixi_lock_sha256"] == hashlib.sha256(b"version: 7\n").hexdigest()
    assert (info["commit"], info["target"], info["flavor"], info["license"]) == (box.commit, TARGET, "static-thinlto",
                                                                                "Apache-2.0")  # fmt: skip


def test_buildinfo_leaves_out_what_a_dirty_or_macos_build_has_not(box: Box) -> None:
    info = buildinfo(box, "--source", "", "--sysroot", "", "--glibc-source", "")
    assert not {"source", "sysroot", "glibc_source", "tag"} & set(info)


# ---------------------------------------------------------------------------------------------------------------------
# static_gates.py package


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
        ("staged-differs", "antb1-linux-x86_64.tar.gz holds the files that were checked"),
        ("debug-staged-differs", "antb1-linux-x86_64.debug.tar.gz holds the files that were checked"),
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
