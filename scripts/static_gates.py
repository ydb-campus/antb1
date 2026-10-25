#!/usr/bin/env python3
"""Gates of the static build (docs/adr/0033-standalone-static-binary.md), run in env `static`.

  prefix  the static Arrow prefix that scripts/static-arrow.sh built: static archives only; every C and C++ file of
          Arrow and of each bundled dependency compiled with -O3 and -flto=thin; no header of a bundled library
          read from the environment; every download a local file; Arrow's option set; a report of the LLVM bitcode
          and native members of each archive.
  binary  a standalone antb1 (scripts/static-check.sh): linkage (Linux: a static ELF executable without loader or
          dynamic section, pthread linked whole, no NSS; macOS: only system libraries, no LC_RPATH, the deployment
          target, a valid signature), the link flags when its build tree is given, then the runs every standalone
          antb1 must pass (build identity, the allocator restart, the smoke kit's codecs, threads, and the exit codes
          of a truncated file, a parse error and an unknown option) on the smoke kit (`--kit`, or one it writes).
  package the tarballs that scripts/static.sh packed: their exact members (regular files under one directory, each
          the file the binary gate ran), the licence texts of docs/adr/0034-licence-and-static-sources.md (LICENSE
          and NOTICE equal to the repository's, the notice sections each target needs, on Linux the LGPL notice
          first), BUILDINFO.json, README.txt, and SHA256SUMS.
  kit     the smoke kit (scripts/static.sh writes build/static/smoke-kit): Parquet files that DuckDB writes from
          range() with each codec, a truncated copy, and kit.json with their digests; DuckDB must give the answer
          the binary must give.
  verify  a package as a user gets it, on a runner without pixi, DuckDB or the LLVM tools (the static workflow, the
          release's fresh runners; `pixi run static-check <tarball>`). Standard library only, Python 3.11 or newer
          with tarfile's data filter (exit 2 without it, on a runner of another target, or when the container engine
          does not answer). The target's tarballs against SHA256SUMS and the kit against kit.json; each tarball
          before anything is unpacked (one directory, exactly the package's members, regular files only, no
          extended attributes); then unpacked with the data filter: BUILDINFO.json, LICENSE and NOTICE, on macOS the
          signature, and the binary gate's runs with an empty environment in an empty directory. On Linux,
          `--container docker|podman` repeats the runs in an image that holds only the binary and the kit: no
          network, no capabilities, a read-only root, user nobody.

Prints one line per check and exits 1 when any fails.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zlib
from pathlib import Path
from typing import BinaryIO

FAILURES: list[str] = []
TARGETS = ["darwin-arm64", "linux-aarch64", "linux-x86_64"]


def check(ok: bool, what: str, detail: str = "") -> bool:
    print(f"static-gates: {'ok  ' if ok else 'FAIL'} {what}{(': ' + detail) if detail and not ok else ''}")
    if not ok:
        FAILURES.append(what)
    return ok


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, errors="replace", **kw)


def tool(env: Path, name: str) -> str:
    path = env / "bin" / name
    return str(path) if path.exists() else (shutil.which(name) or name)


# ---------------------------------------------------------------------------------------------------------------------
# prefix

# Bundled dependencies whose sources are compiled (xsimd and rapidjson are header-only); each must show up in a
# compilation database, so a dependency built outside the -O3 and ThinLTO flags cannot go unnoticed.
COMPILED_DEPS = ["brotli", "lz4", "mimalloc", "re2", "snappy", "thrift", "utf8proc", "zlib", "zstd"]
ARROW_ARCHIVES = ["libarrow.a", "libarrow_compute.a", "libparquet.a"]
BUNDLED_ARCHIVE = "libarrow_bundled_dependencies.a"
OPTIONS_ON = [
    "ARROW_BUILD_STATIC", "ARROW_COMPUTE", "ARROW_IPC", "ARROW_PARQUET", "ARROW_MIMALLOC", "ARROW_WITH_SNAPPY",
    "ARROW_WITH_ZSTD", "ARROW_WITH_LZ4", "ARROW_WITH_ZLIB", "ARROW_WITH_BROTLI", "ARROW_WITH_RE2",
    "ARROW_WITH_UTF8PROC",
]  # fmt: skip
OPTIONS_OFF = [
    "ARROW_BUILD_SHARED", "ARROW_JEMALLOC", "ARROW_FILESYSTEM", "ARROW_DATASET", "ARROW_ACERO", "ARROW_CSV",
    "ARROW_JSON", "ARROW_S3", "ARROW_GCS", "ARROW_AZURE", "ARROW_HDFS", "ARROW_FLIGHT", "ARROW_GANDIVA", "ARROW_ORC",
    "ARROW_SUBSTRAIT", "ARROW_USE_GLOG", "ARROW_WITH_BACKTRACE", "ARROW_WITH_OPENTELEMETRY", "ARROW_WITH_BZ2",
    "PARQUET_REQUIRE_ENCRYPTION",
]  # fmt: skip
SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx")


def compile_args(entry: dict) -> list[str]:
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def archive_members(path: Path) -> list[tuple[str, bytes]]:
    """(name, first 4 bytes) of every member of a GNU or BSD ar archive, without its symbol table."""
    return parse_archive(path.read_bytes(), str(path))


def parse_archive(data: bytes, what: str) -> list[tuple[str, bytes]]:
    if not data.startswith(b"!<arch>\n"):
        raise ValueError(f"{what} is not an ar archive")
    pos, names, members = 8, b"", []
    while pos + 60 <= len(data):
        header = data[pos : pos + 60]
        name = header[:16].decode("ascii", "replace").rstrip()
        size = int(header[48:58].decode("ascii").strip() or 0)
        body = pos + 60
        payload = data[body : body + size]
        if name.startswith("#1/"):  # BSD: the name precedes the data and counts towards its size
            n = int(name[3:])
            name, payload = payload[:n].rstrip(b"\0").decode("utf-8", "replace"), payload[n:]
        elif name == "//":  # GNU long-name table
            names = payload
            name = ""
        elif name.startswith("/") and name[1:].isdigit():
            off = int(name[1:])
            name = names[off : names.index(b"\n", off)].decode("utf-8", "replace").rstrip("/")
        elif name in ("/", "/SYM64/"):  # GNU symbol tables
            name = ""
        else:
            name = name.rstrip("/")
        if name and not name.startswith("__.SYMDEF"):  # BSD symbol table, short or #1/ named (macOS ranlib)
            members.append((name, payload[:4]))
        pos = body + size + (size & 1)
    return members


def is_bitcode(magic: bytes) -> bool:
    return magic in (b"BC\xc0\xde", b"\xde\xc0\x17\x0b")


def archive_member(name: str, body: bytes) -> bytes:
    """One ar member: its 60-byte header, then the body padded to an even size."""
    header = f"{name:<16}{0:<12}{0:<6}{0:<6}{644:<8}{len(body):<10}`\n".encode()
    return header + body + b"\n" * (len(body) & 1)


def check_archive_parser() -> None:
    """The parser on both archive formats: GNU (Linux: symbol table "/", long names through "//") and BSD (macOS:
    names after "#1/<length>", the symbol table as "__.SYMDEF SORTED"), each with a bitcode and a native member."""
    bitcode, native = b"BC\xc0\xde" + b"\0" * 4, b"\x7fELF" + b"\0" * 4
    long_name = "a_long_member_name.cc.o"
    gnu = b"!<arch>\n" + archive_member("/", b"\0" * 4) + archive_member("//", f"{long_name}/\n".encode())
    gnu += archive_member("/0", bitcode) + archive_member("short.o/", native)
    bsd = b"!<arch>\n"
    for name, body in (("__.SYMDEF SORTED", b"\0" * 8), (long_name, bitcode), ("short.o", native)):
        padded = name.encode() + b"\0" * (-len(name) % 8)
        bsd += archive_member(f"#1/{len(padded)}", padded + body)
    want = [(long_name, True), ("short.o", False)]
    for kind, data in (("GNU", gnu), ("BSD", bsd)):
        got = [(name, is_bitcode(magic)) for name, magic in parse_archive(data, kind)]
        check(got == want, f"the archive parser reads a {kind} archive", f"{got}")


def gate_prefix(args: argparse.Namespace) -> None:
    build, prefix, env = Path(args.build), Path(args.prefix), Path(args.env)
    lib = prefix / "lib"
    check_archive_parser()

    # 1. Static archives only.
    shared = sorted(str(p.relative_to(prefix)) for p in lib.rglob("*") if re.search(r"\.(so(\.\d+)*|dylib)$", p.name))
    check(not shared, "no shared library in the prefix", ", ".join(shared[:5]))
    missing = [a for a in [*ARROW_ARCHIVES, BUNDLED_ARCHIVE] if not (lib / a).is_file()]
    check(not missing, "Arrow's static archives are installed", ", ".join(missing))

    # 2. Every C and C++ translation unit: last -O is -O3, -flto=thin present.
    databases = sorted(build.rglob("compile_commands.json"))
    tus, bad, seen_deps = 0, [], set()
    for db in databases:
        for entry in json.loads(db.read_text()):
            source = entry.get("file", "")
            if not source.endswith(SOURCE_SUFFIXES):
                continue
            tus += 1
            argv = compile_args(entry)
            levels = [a for a in argv if re.fullmatch(r"-O([0-3sz]|fast)?", a)]
            if not levels or levels[-1] != "-O3" or "-flto=thin" not in argv:
                bad.append(
                    f"{source} ({levels[-1] if levels else 'no -O'}{'' if '-flto=thin' in argv else ', no -flto=thin'})"
                )
            path = source.replace("\\", "/").lower()
            for dep in COMPILED_DEPS:
                if f"/{dep}" in path or f"{dep}_ep" in path:
                    seen_deps.add(dep)
    check(
        tus > 0 and not bad,
        f"{tus} C/C++ files in {len(databases)} compilation databases: -O3 and -flto=thin",
        "; ".join(bad[:5]),
    )
    unseen = [d for d in COMPILED_DEPS if d not in seen_deps]
    check(not unseen, "every compiled bundled dependency is in a compilation database", ", ".join(unseen))

    # 3. Headers: nothing of a bundled library from the environment. Boost's headers come from it on purpose;
    # zstd.h only while the environment's zstd is the pinned version.
    zstd_pin = re.search(r"^zstd-(\d+\.\d+\.\d+)\.tar\.gz ", Path(args.lock).read_text(), re.MULTILINE)
    zstd_env = env / "include" / "zstd.h"
    zstd_ok = False
    if zstd_pin and zstd_env.exists():
        text = zstd_env.read_text(errors="replace")
        parts = [re.search(rf"#define ZSTD_VERSION_{k}\s+(\d+)", text) for k in ("MAJOR", "MINOR", "RELEASE")]
        zstd_ok = all(parts) and ".".join(m.group(1) for m in parts) == zstd_pin.group(1)
    include = os.path.realpath(env / "include") + os.sep
    allowed = re.compile(r"^(boost/|c\+\+/v1/" + (r"|zstd\.h$|zstd_errors\.h$|zdict\.h$" if zstd_ok else "") + ")")
    ninja = tool(env, "ninja")
    foreign: set[str] = set()
    ninja_dirs = sorted({p.parent for p in build.rglob("build.ninja")})
    for d in ninja_dirs:
        out = run([ninja, "-C", str(d), "-t", "deps"])
        for line in out.stdout.splitlines():
            if not line.startswith("    "):
                continue
            real = os.path.realpath(os.path.join(d, line.strip()))
            if real.startswith(include) and not allowed.match(real[len(include) :]):
                foreign.add(real[len(include) :])
    check(
        not foreign,
        f"no bundled library's header from {include} ({len(ninja_dirs)} ninja trees)",
        ", ".join(sorted(foreign)[:8]),
    )

    # 4. Every download a local file (ExternalProject and FetchContent record their URLs in *-urlinfo.txt).
    urls = []
    for info in build.rglob("*-urlinfo.txt"):
        for line in info.read_text(errors="replace").splitlines():
            if line.startswith("url(s)="):
                urls += [u for u in line[len("url(s)=") :].split(";") if u]
    remote = [u for u in urls if not (u.startswith("/") or u.startswith("file://"))]
    check(bool(urls) and not remote, f"{len(urls)} bundled sources read from local files", ", ".join(remote[:5]))

    # 5. Arrow's option set.
    options_file = lib / "cmake" / "Arrow" / "ArrowOptions.cmake"
    options = dict(re.findall(r'set\((\w+) "([^"]*)"\)', options_file.read_text())) if options_file.exists() else {}
    wrong = [f"{o}={options.get(o)}" for o in OPTIONS_ON if options.get(o) != "ON"]
    wrong += [f"{o}={options.get(o)}" for o in OPTIONS_OFF if options.get(o, "OFF") != "OFF"]
    check(bool(options) and not wrong, "Arrow's option set (ArrowOptions.cmake)", ", ".join(wrong))

    # 6. LTO report: bitcode against native members per archive. Arrow's own archives are all bitcode.
    lines = []
    for archive in sorted(lib.glob("*.a")):
        members = archive_members(archive)
        native = [n for n, magic in members if not is_bitcode(magic)]
        lines.append(f"{archive.name}: {len(members) - len(native)} bitcode, {len(native)} native")
        lines += [f"  native: {n}" for n in native]
        if archive.name in ARROW_ARCHIVES:
            check(not native and bool(members), f"{archive.name} is all LLVM bitcode", f"{len(native)} native members")
        elif archive.name == BUNDLED_ARCHIVE:
            check(
                len(native) < len(members),
                f"{archive.name} holds LLVM bitcode",
                f"{len(members) - len(native)} of {len(members)}",
            )
    Path(args.report).write_text("\n".join(lines) + "\n")
    print(f"static-gates: LTO report: {args.report}")


# ---------------------------------------------------------------------------------------------------------------------
# binary

# The NSS lookups of glibc: a static glibc cannot load the host's NSS modules for them.
NSS_FUNCTIONS = {
    f"{base}{suffix}"
    for base in ("getpwnam", "getpwuid", "getpwent", "getgrnam", "getgrgid", "getgrent", "getaddrinfo",
                 "gethostbyname", "gethostbyname2", "gethostbyaddr", "getservbyname", "getservbyport")
    for suffix in ("", "_r")
}  # fmt: skip


def symbols(nm: str, binary: str) -> dict[str, str]:
    """name -> nm type letter (llvm-nm, all symbols, no demangling)."""
    out = run([nm, "--no-sort", binary])
    table: dict[str, str] = {}
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            table[parts[-1]] = parts[-2]
    return table


def gate_linux_linkage(env: Path, binary: str) -> None:
    readelf = tool(env, "llvm-readelf")
    header = run([readelf, "--file-header", binary]).stdout
    check(
        "EXEC (Executable file)" in header,
        "ELF type EXEC (static, not PIE)",
        (re.search(r"Type:\s+(.*)", header) or [None, "?"])[1],
    )
    segments = run([readelf, "--program-headers", binary]).stdout
    check(not re.search(r"^\s+INTERP\b", segments, re.MULTILINE), "no program interpreter (PT_INTERP)")
    check(not re.search(r"^\s+DYNAMIC\b", segments, re.MULTILINE), "no dynamic segment")
    notes = run([readelf, "--notes", binary]).stdout
    check("Build ID" in notes, "a GNU build-id")
    syms = symbols(tool(env, "llvm-nm"), binary)
    for name in ("__pthread_key_create", "pthread_once"):
        kind = syms.get(name, "")
        # Below glibc 2.34 libstdc++ asks for these weakly: undefined (w, U), it would run as single-threaded.
        check(
            kind.upper() in ("T", "W") and kind != "w",
            f"{name} is defined (libpthread linked whole)",
            kind or "missing",
        )
    nss = sorted(n for n, k in syms.items()
                 if (n in NSS_FUNCTIONS or n.startswith("__nss_")) and k.upper() in ("T", "W"))  # fmt: skip
    check(not nss, "no NSS function linked (static glibc cannot load NSS modules)", ", ".join(nss[:6]))
    for name in ("dlopen", "iconv_open"):
        if syms.get(name, "").upper() in ("T", "W"):
            print(f"static-gates: note {name} is linked (static glibc loads modules only from the host's glibc)")


def gate_macos_linkage(env: Path, binary: str) -> None:
    otool = tool(env, "llvm-otool")
    libs = [ln.split()[0] for ln in run([otool, "-L", binary]).stdout.splitlines()[1:] if ln.strip()]
    allowed = {"/usr/lib/libSystem.B.dylib", "/usr/lib/libc++abi.dylib"}
    check(
        bool(libs) and set(libs) <= allowed,
        "only system libraries (libSystem, libc++abi)",
        ", ".join(lib for lib in libs if lib not in allowed),
    )
    commands = run([otool, "-l", binary]).stdout
    check("LC_RPATH" not in commands, "no LC_RPATH")
    minos = re.search(r"minos (\S+)", commands)
    check(bool(minos) and minos.group(1) == "13.0", "deployment target 13.0", minos.group(1) if minos else "none")
    codesign = shutil.which("codesign", path="/usr/bin")
    if codesign:
        out = run([codesign, "--verify", "--strict", binary])
        check(out.returncode == 0, "the signature verifies (codesign --verify --strict)", out.stderr.strip())
    # The libc++abi symbols antb1 imports must also be imported by conda's libc++.1.dylib, whose macOS floor
    # conda-forge already guarantees.
    nm = tool(env, "llvm-nm")

    def abi_imports(path: str) -> set[str]:
        out = run([nm, "-m", "-u", path]).stdout
        return {ln.split()[-3] for ln in out.splitlines() if "(from libc++abi)" in ln}

    ours, conda = abi_imports(binary), abi_imports(str(env / "lib" / "libc++.1.dylib"))
    check(ours <= conda, "libc++abi imports within those of conda's libc++", ", ".join(sorted(ours - conda)[:6]))
    # The static libc++ must be the only one this binary calls. Every macOS process also loads the system's
    # libc++ with the same std::__1 names, and any bind of one of them (regular, lazy, weak or a chained fixup)
    # lets dyld resolve it there: an iostream built by one libc++ and freed by the other aborts at start.
    objdump = tool(env, "llvm-objdump")
    binds = run([objdump, "--macho", "--bind", "--lazy-bind", "--weak-bind", "--dyld-info", binary]).stdout
    std_binds = sorted({ln.split()[-1] for ln in binds.splitlines() if ln.split() and "St3__1" in ln.split()[-1]})
    check(
        not std_binds,
        "no libc++ symbol (std::__1) is bound outside the binary",
        f"{len(std_binds)}: " + ", ".join(std_binds[:8]),
    )
    exports = run([objdump, "--macho", "--exports-trie", binary]).stdout
    std_exports = [ln for ln in exports.splitlines() if "St3__1" in ln]
    check(not std_exports, "no libc++ symbol (std::__1) is exported", f"{len(std_exports)} exported")


def gate_link_flags(env: Path, build_dir: Path) -> None:
    out = run([tool(env, "ninja"), "-C", str(build_dir), "-t", "commands", "bin/antb1"])
    link = out.stdout.strip().splitlines()[-1] if out.stdout.strip() else ""
    argv = shlex.split(link)
    cpu = shlex.split(os.environ.get("ANTB1_STATIC_CPU_FLAGS", "")) or {
        ("Linux", "x86_64"): ["-march=x86-64-v3"],
        ("Linux", "aarch64"): ["-march=armv8.2-a"],
    }.get((platform.system(), platform.machine()), [])
    want = ["-flto=thin", "-O3", *cpu] + (["-static"] if platform.system() == "Linux" else [])
    missing = [w for w in want if w not in argv]
    check(bool(argv) and not missing, f"antb1's link line has {' '.join(want)}", "missing " + " ".join(missing))


# The smoke kit: Parquet files that DuckDB writes from range() with each codec, a truncated copy, and kit.json with
# their digests. `pixi run static` writes it to build/static/smoke-kit; verify takes it to runners without DuckDB.
CODECS = ["uncompressed", "snappy", "gzip", "zstd", "lz4_raw", "brotli"]
SMOKE_SELECT = "SELECT COUNT(*), SUM(a), MIN(b), MAX(b), COUNT(DISTINCT c)"
SMOKE_SQL = f"{SMOKE_SELECT} FROM t"
SMOKE_ANSWER = "200000,19999900000,0,999,97"
KIT_COPY = (
    "COPY (SELECT range AS a, (range * 7) % 1000 AS b, 'k' || (range % 97) AS c FROM range(200000)) "
    "TO '{path}' (FORMAT parquet, COMPRESSION {codec}, ROW_GROUP_SIZE 10000)"
)
KIT_FILES = [*(f"{codec}.parquet" for codec in CODECS), "truncated.parquet"]
KIT_JSON = "kit.json"
PARSE_ERROR_SQL = "SELECT COUNT(*) FROM"


def sha256_stream(stream: BinaryIO) -> str:
    digest = hashlib.sha256()
    for block in iter(lambda: stream.read(1 << 20), b""):
        digest.update(block)
    return digest.hexdigest()


def sha256_file(path: Path) -> str:
    with open(path, "rb") as f:
        return sha256_stream(f)


def gate_kit(env: Path, out: Path) -> None:
    """Writes the smoke kit into `out`, a new or empty directory. DuckDB must give SMOKE_ANSWER on each codec file."""
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        check(False, f"the kit goes into a new or empty directory: {out}")
        return
    out.mkdir(parents=True, exist_ok=True)
    duckdb = tool(env, "duckdb")  # -no-init: a ~/.duckdbrc must not change what DuckDB prints
    for codec in CODECS:
        path = out / f"{codec}.parquet"
        wrote = run([duckdb, "-no-init", "-c", KIT_COPY.format(path=path, codec=codec)])
        if not check(wrote.returncode == 0 and path.is_file(), f"DuckDB writes a {codec} file", wrote.stderr[:200]):
            continue
        answer = run([duckdb, "-no-init", "-csv", "-noheader", "-c", f"{SMOKE_SELECT} FROM '{path}'"])
        check(
            answer.returncode == 0 and answer.stdout.strip() == SMOKE_ANSWER,
            f"DuckDB answers {SMOKE_ANSWER} on the {codec} file",
            (answer.stdout + answer.stderr).strip()[:200],
        )
    if (out / "zstd.parquet").is_file():
        data = (out / "zstd.parquet").read_bytes()
        (out / "truncated.parquet").write_bytes(data[: len(data) // 2])
    files = {
        name: {"sha256": sha256_file(out / name), "bytes": (out / name).stat().st_size}
        for name in KIT_FILES
        if (out / name).is_file()
    }
    writer = run([duckdb, "-no-init", "-version"]).stdout.strip() or "duckdb"
    meta = {"kit": 1, "writer": f"DuckDB {writer}", "sql": SMOKE_SQL, "answer": SMOKE_ANSWER, "files": files}
    (out / KIT_JSON).write_text(json.dumps(meta, indent=2) + "\n")


def read_kit(kit: Path) -> bool:
    """Checks a smoke kit against its kit.json and this script's query and answer; false when it cannot be used."""
    try:
        meta = json.loads((kit / KIT_JSON).read_text())
    except (OSError, ValueError) as e:
        return check(False, f"the kit's {KIT_JSON} is readable", str(e))
    if not check(
        isinstance(meta, dict)
        and (meta.get("kit"), meta.get("sql"), meta.get("answer")) == (1, SMOKE_SQL, SMOKE_ANSWER),
        "the kit is version 1 with this script's query and answer",
        str({key: meta.get(key) for key in ("kit", "sql", "answer")} if isinstance(meta, dict) else meta)[:200],
    ):
        return False
    files = meta.get("files")
    names = sorted(path.name for path in kit.iterdir() if path.name != KIT_JSON)
    ok = check(
        isinstance(files, dict) and sorted(files) == names == sorted(KIT_FILES),
        f"the kit holds exactly {', '.join(KIT_FILES)}",
        str(names),
    )
    for name in KIT_FILES:
        want = files.get(name) if isinstance(files, dict) else None
        path = kit / name
        got = {"sha256": sha256_file(path), "bytes": path.stat().st_size} if path.is_file() else None
        ok = check(got is not None and got == want, f"the kit's {name} matches {KIT_JSON}") and ok
    return ok


def timed_out(cmd: list[str], timeout: int) -> subprocess.CompletedProcess:
    return subprocess.CompletedProcess(cmd, -1, "", f"timed out after {timeout} s")


class Host:
    """Runs the binary on this host with an empty environment (unless one is given), in an empty directory, with
    /dev/null as stdin; `kit` is the smoke kit's directory as the binary sees it."""

    def __init__(self, binary: str, cwd: str, kit: str, timeout: int = 600, label: str = "") -> None:
        self.binary, self.cwd, self.kit, self.timeout, self.label = binary, cwd, kit, timeout, label

    def __call__(self, args: list[str], env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
        cmd = [self.binary, *args]
        try:
            return subprocess.run(
                cmd, capture_output=True, text=True, errors="replace", env=env or {}, cwd=self.cwd,
                stdin=subprocess.DEVNULL, timeout=self.timeout,
            )  # fmt: skip
        except subprocess.TimeoutExpired:
            return timed_out(cmd, self.timeout)
        except OSError as e:  # not executable, or no such file
            return subprocess.CompletedProcess(cmd, -1, "", f"cannot run: {e}")


# A container that can do nothing but run the binary: no network, no capabilities, no privilege gain, a read-only
# root file system, user nobody (verify --container).
CONTAINER_FLAGS = [
    "--pull", "never", "--network", "none", "--read-only", "--cap-drop", "ALL",
    "--security-opt", "no-new-privileges", "--user", "65534:65534",
]  # fmt: skip


class Container:
    """Runs /antb1 in a new container of an image that holds only it and the kit (import_image)."""

    def __init__(self, engine: str, image: str, timeout: int) -> None:
        self.engine, self.image, self.timeout, self.kit, self.label = engine, image, timeout, "/kit", " in a container"
        self.runs = 0

    def __call__(self, args: list[str], env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
        self.runs += 1
        name = f"antb1-verify-{os.getpid()}-{self.runs}"
        cmd = [self.engine, "run", "--rm", "--name", name, *CONTAINER_FLAGS]
        for key, value in (env or {}).items():
            cmd += ["--env", f"{key}={value}"]
        cmd += [self.image, "/antb1", *args]
        try:
            return subprocess.run(
                cmd, capture_output=True, text=True, errors="replace", stdin=subprocess.DEVNULL, timeout=self.timeout
            )
        except subprocess.TimeoutExpired:
            run([self.engine, "rm", "--force", name])
            return timed_out(cmd, self.timeout)


def gate_runs(antb1: Host | Container, target: str, version: str = "", commit: str = "") -> None:
    """What every standalone antb1 must do wherever it runs: say what it is; restart its allocator (Linux); answer
    each file of the smoke kit, with one thread as with four; exit with the CLI's codes (src/cli/include/antb1/cli/
    cli.h) on a truncated file (3), a parse error (1) and an unknown option (2)."""
    at, kit = antb1.label, antb1.kit
    plain = antb1(["version"])
    first = re.fullmatch(r"antb1 (\S+) \(Apache Arrow \d+\.\d+\.\d+\)\n", plain.stdout)
    check(
        plain.returncode == 0 and first is not None and version in ("", first.group(1)),
        f"`antb1 version` runs{at}" + (f" and says {version}" if version else ""),
        (plain.stdout + plain.stderr).strip()[:200],
    )
    verbose = antb1(["version", "--verbose"])
    fields = dict(ln.split(": ", 1) for ln in verbose.stdout.splitlines()[1:] if ": " in ln)
    check(
        verbose.returncode == 0 and fields.get("flavor") == "static-thinlto",
        f"flavor static-thinlto{at}",
        fields.get("flavor", verbose.stderr.strip()[:200]),
    )
    check(
        "antb1-static" in fields.get("arrow", ""),
        f"Arrow is the static build (package kind antb1-static){at}",
        fields.get("arrow", ""),
    )
    codecs = fields.get("codecs", "")
    check(bool(codecs) and " no" not in codecs, f"every codec is available{at}", codecs)
    check(fields.get("target") == target, f"target {target}{at}", fields.get("target", ""))
    if commit:
        check(fields.get("commit") == commit, f"commit {commit}{at}", fields.get("commit", ""))
    if target.startswith("linux-"):
        # mimalloc prints its options when it starts; after the restart (ADR 0017) purge_delay is -1.
        restart = antb1(["version"], env={"MIMALLOC_VERBOSE": "1"})
        check(
            re.search(r"option 'purge_delay': -1\b", restart.stderr) is not None,
            f"the allocator restart sets MIMALLOC_PURGE_DELAY=-1{at}",
            "no purge_delay -1 in mimalloc's output",
        )
        check(
            fields.get("mimalloc purge delay") == "-1",
            f"version --verbose reports the purge delay -1{at}",
            fields.get("mimalloc purge delay", ""),
        )
    for codec in CODECS:
        res = antb1(["query", "-c", SMOKE_SQL, "--table", f"t={kit}/{codec}.parquet", "--format", "csv"])
        rows = res.stdout.strip().splitlines()
        check(
            res.returncode == 0 and rows[-1:] == [SMOKE_ANSWER],
            f"{codec}: query answer{at}",
            (res.stdout + res.stderr).strip()[-200:],
        )
    one = antb1(["query", "-c", SMOKE_SQL, "--table", f"t={kit}/zstd.parquet", "--threads", "1"])
    four = antb1(["query", "-c", SMOKE_SQL, "--table", f"t={kit}/zstd.parquet", "--threads", "4"])
    check(
        one.returncode == 0 and one.stdout == four.stdout,
        f"--threads 4 answers as --threads 1{at}",
        (four.stdout + four.stderr).strip()[-200:],
    )
    bad = antb1(["query", "-c", SMOKE_SQL, "--table", f"t={kit}/truncated.parquet"])
    check(
        bad.returncode == 3,
        f"a truncated file exits with 3 (an exception through the static unwinder){at}",
        f"exit {bad.returncode}: {bad.stderr.strip()[:200]}",
    )
    parse = antb1(["query", "-c", PARSE_ERROR_SQL, "--table", f"t={kit}/zstd.parquet"])
    check(
        parse.returncode == 1 and re.search(r"^antb1: parse error: ", parse.stderr, re.MULTILINE) is not None,
        f"a parse error exits with 1{at}",
        f"exit {parse.returncode}: {parse.stderr.strip()[:200]}",
    )
    usage = antb1(["version", "--bogus"])
    check(
        usage.returncode == 2 and not usage.stdout and "--bogus" in usage.stderr,
        f"an unknown option exits with 2{at}",
        f"exit {usage.returncode}: {(usage.stdout + usage.stderr).strip()[:200]}",
    )


def gate_cpu_guard(env: Path, binary: str, antb1: Host) -> None:
    """A raised CPU baseline comes with the guard of src/cli/cpu_guard.cc: GuardCpu must be an entry of .preinit_array
    (the section alone proves nothing: glibc 2.28's libpthread.a puts its own initializer there)."""
    verbose = antb1(["version", "--verbose"]).stdout
    baseline = re.search(r"^cpu baseline: (.*)$", verbose, re.MULTILINE)
    if platform.system() != "Linux" or not baseline or baseline.group(1) in ("compiler default", "-march=x86-64"):
        return
    guard = None
    for line in run([tool(env, "llvm-nm"), "--defined-only", "--demangle", binary]).stdout.splitlines():
        parts = line.split(maxsplit=2)
        if len(parts) == 3 and parts[2].startswith("(anonymous namespace)::GuardCpu("):
            guard = int(parts[0], 16)
    dump = run([tool(env, "llvm-readelf"), "--hex-dump=.preinit_array", binary]).stdout
    data = b"".join(
        bytes.fromhex(word)
        for line in dump.splitlines()
        if line.strip().startswith("0x")
        for word in line.split()[1:5]
        if re.fullmatch(r"[0-9a-f]{8}", word)
    )
    entries = [int.from_bytes(data[i : i + 8], "little") for i in range(0, len(data) - 7, 8)]
    check(
        guard is not None and guard in entries,
        f"the CPU guard of {baseline.group(1)} runs from .preinit_array",
        f"GuardCpu {hex(guard) if guard is not None else 'missing'}, entries {[hex(e) for e in entries]}",
    )


def gate_linkage(env: Path, binary: str) -> bool:
    """The checks that need no run of the binary; false when it is no executable at all."""
    if not check(os.access(binary, os.X_OK), f"{binary} is an executable"):
        return False
    if platform.system() == "Linux":
        gate_linux_linkage(env, binary)
    elif platform.system() == "Darwin":
        gate_macos_linkage(env, binary)
    return True


def host_target() -> str:
    """This host's target, as static.sh names it; empty for any other host."""
    machine = (platform.system(), platform.machine())
    return {("Linux", "x86_64"): "linux-x86_64", ("Linux", "aarch64"): "linux-aarch64",
            ("Darwin", "arm64"): "darwin-arm64"}.get(machine, "")  # fmt: skip


def gate_binary(args: argparse.Namespace) -> None:
    env = Path(args.env)
    binary = os.path.abspath(args.bin)
    if not gate_linkage(env, binary) or args.linkage_only:
        return
    if args.build_dir:
        gate_link_flags(env, Path(args.build_dir))
    with tempfile.TemporaryDirectory(prefix="antb1-static-check-") as tmp:
        kit = Path(args.kit).resolve() if args.kit else Path(tmp, "kit")
        if not args.kit:
            gate_kit(env, kit)
        cwd = Path(tmp, "cwd")
        cwd.mkdir()
        antb1 = Host(binary, str(cwd), str(kit))
        gate_cpu_guard(env, binary, antb1)
        if read_kit(kit):
            gate_runs(antb1, host_target())


# ---------------------------------------------------------------------------------------------------------------------
# package

REPO = Path(__file__).resolve().parent.parent
LGPL_NOTICE_START = "This program contains the GNU C Library"  # as scripts/static_package.py writes it
PACKAGE_TEXTS = ["LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.txt"]
# The notice sections each target needs (`==== <label> ====`), besides those of every library Arrow bundles.
SECTIONS_ALL = [
    r"Apache Arrow \S+: LICENSE\.txt",
    r"Apache Arrow \S+: NOTICE\.txt",
    r"Apache Thrift \S+: NOTICE",
    r"CLI11 \S+: LICENSE",
    r"Boost \S+: LICENSE_1_0\.txt",
]
SECTIONS_LINUX = [
    r"GNU C Library \(glibc-\S+\): COPYING", r"GNU C Library \(glibc-\S+\): COPYING\.LIB",
    r"GNU C Library \(glibc-\S+\): LICENSES",
    r"GCC \S+ runtime libraries \(libstdc\+\+, libgcc\): RUNTIME\.LIBRARY\.EXCEPTION",
]  # fmt: skip
SECTIONS_DARWIN = [r"libc\+\+ \S+: LICENSE\.TXT"]


def read_tarball(path: Path, texts: list[str]) -> tuple[dict[str, tarfile.TarInfo], dict[str, bytes]]:
    """The members of a .tar.gz by name, and the contents of those whose base name is in `texts`."""
    members: dict[str, tarfile.TarInfo] = {}
    contents: dict[str, bytes] = {}
    with tarfile.open(path, "r:gz") as tar:
        for info in tar:
            members[info.name] = info
            if info.isreg() and info.name.rsplit("/", 1)[-1] in texts:
                contents[info.name.rsplit("/", 1)[-1]] = tar.extractfile(info).read()
    return members, contents


def check_members(path: Path, members: dict[str, tarfile.TarInfo], top: str, files: list[str]) -> None:
    expected = {top, *(f"{top}/{name}" for name in files)}
    check(set(members) == expected, f"{path.name} holds exactly {top}/ with {', '.join(files)}",
          f"extra {sorted(set(members) - expected)}, missing {sorted(expected - set(members))}")  # fmt: skip
    for name, info in members.items():
        safe = not name.startswith("/") and ".." not in name.split("/")
        kind_ok = info.isdir() if name == top else info.isreg()
        check(safe and kind_ok, f"{path.name}: {name} is a {'directory' if name == top else 'regular file'} inside")


def check_staged(path: Path, staged: Path) -> None:
    """Every file of a packed tarball holds the bytes of the staged file it was packed from, so the files that the
    binary gate ran are the shipped ones. Unsafe or unexpected members are check_members' to fail."""
    differ = []
    with tarfile.open(path, "r:gz") as tar:
        for info in tar:
            parts = info.name.split("/")
            if not info.isreg() or len(parts) != 2 or parts[1] in ("", ".", ".."):
                continue
            source = staged / parts[1]
            member = tar.extractfile(info)
            if member is None or not source.is_file() or sha256_stream(member) != sha256_file(source):
                differ.append(parts[1])
    check(not differ, f"{path.name} holds the files that were checked in {staged}", "differ: " + ", ".join(differ))


def gate_package(args: argparse.Namespace) -> None:
    pkg, target = Path(args.pkg), args.target
    linux = target.startswith("linux-")
    assets = [f"antb1-{target}.tar.gz"] + ([f"antb1-{target}.debug.tar.gz"] if linux else [])
    sums = dict(reversed(line.split(maxsplit=1)) for line in (pkg / "SHA256SUMS").read_text().splitlines())
    check(sorted(sums) == sorted(assets), f"SHA256SUMS lists exactly {', '.join(assets)}", str(sorted(sums)))
    for asset in assets:
        digest = hashlib.sha256((pkg / asset).read_bytes()).hexdigest() if (pkg / asset).is_file() else ""
        check(digest == sums.get(asset), f"{asset} matches SHA256SUMS")

    texts = [*PACKAGE_TEXTS, "BUILDINFO.json", "README.txt"]
    members, contents = read_tarball(pkg / assets[0], texts)
    tops = sorted({name.split("/", 1)[0] for name in members})
    try:
        info = json.loads(contents.get("BUILDINFO.json", b""))
    except ValueError:
        info = {}
    top = f"antb1-{info.get('version', '?')}-{target}"
    check(tops == [top], f"{assets[0]} unpacks into {top}/", str(tops))
    check_members(pkg / assets[0], members, top, ["antb1", *texts])
    check_staged(pkg / assets[0], pkg / top)
    check(
        bool(members.get(f"{top}/antb1")) and members[f"{top}/antb1"].mode & 0o111 != 0,
        "the packaged antb1 is executable",
    )
    for name in ("LICENSE", "NOTICE"):
        check(contents.get(name) == (REPO / name).read_bytes(), f"the package's {name} is the repository's")

    notices = contents.get("THIRD_PARTY_NOTICES.txt", b"").decode(errors="replace")
    labels = re.findall(r"^==== (.+) ====$", notices, re.MULTILINE)
    needed = SECTIONS_ALL + (SECTIONS_LINUX if linux else SECTIONS_DARWIN)
    missing = [pattern for pattern in needed if not any(re.fullmatch(pattern, label) for label in labels)]
    check(not missing, "THIRD_PARTY_NOTICES.txt has the sections this target needs", f"none matches {missing}")
    glibc_labels = [label for label in labels if label.startswith("GNU C Library")]
    if linux:
        source = info.get("glibc_source", "")
        check(notices.startswith(LGPL_NOTICE_START) and bool(source) and source in notices.split("\n\n", 1)[0],
              f"THIRD_PARTY_NOTICES.txt starts with the LGPL notice naming {source or 'the glibc source'}")  # fmt: skip
        check(bool(info.get("sysroot")), "BUILDINFO.json names the sysroot build")
    else:
        check(not glibc_labels and LGPL_NOTICE_START not in notices, "THIRD_PARTY_NOTICES.txt names no glibc")
    named = (info.get("target"), info.get("license"), info.get("flavor")) == (target, "Apache-2.0", "static-thinlto")
    check(named, "BUILDINFO.json names the target, the licence and the flavor", str(info))
    readme = contents.get("README.txt", b"").decode(errors="replace")
    sources = f"antb1-{target}.sources.tar"
    check("Apache License 2.0" in readme and (not linux or sources in readme),
          f"README.txt names the licence{' and ' + sources if linux else ''}")  # fmt: skip

    if linux:
        members, debug = read_tarball(pkg / assets[1], PACKAGE_TEXTS)
        check_members(pkg / assets[1], members, top, ["antb1.debug", *PACKAGE_TEXTS])
        check_staged(pkg / assets[1], pkg / "debug" / top)
        check(all(debug.get(name) == contents.get(name) for name in PACKAGE_TEXTS),
              f"{assets[1]} carries the same licence texts")  # fmt: skip


# ---------------------------------------------------------------------------------------------------------------------
# verify


class CannotVerify(Exception):
    """verify cannot check the package on this runner (exit 2), which is not a failed check."""


CODESIGN = "/usr/bin/codesign"
RUN_TIMEOUT = 120  # seconds for each run of the binary
PACKAGE_FILES = ["antb1", *PACKAGE_TEXTS, "BUILDINFO.json", "README.txt"]
DEBUG_FILES = ["antb1.debug", *PACKAGE_TEXTS]
XATTR_HEADERS = ("SCHILY.xattr.", "LIBARCHIVE.xattr.")  # the pax keys of extended attributes
SUM_LINE = re.compile(r"([0-9a-f]{64}) [ *]([A-Za-z0-9._-]+)")


def read_sums(path: Path) -> dict[str, str]:
    """name -> sha256 of a SHA256SUMS file in sha256sum's format (`cmake -E sha256sum` writes it)."""
    try:
        lines = path.read_text().splitlines()
    except OSError as e:
        check(False, f"{path} is readable", str(e))
        return {}
    matches = [SUM_LINE.fullmatch(line) for line in lines]
    sums = {m.group(2): m.group(1) for m in matches if m}
    check(
        bool(lines) and all(matches) and len(sums) == len(lines),
        f"{path.name} has one '<sha256>  <file>' line per file",
        "; ".join(line for line, m in zip(lines, matches, strict=True) if not m)[:200],
    )
    return sums


def check_tarball(path: Path, files: list[str], target: str, version: str) -> str:
    """Checks a package tarball before anything is extracted: one directory antb1-<version>-<target>/ holding exactly
    `files`, each a regular file, no member twice, no extended attribute. Returns the directory's version ("" when a
    check failed); `version` is the one it must have, if given."""
    before = len(FAILURES)
    try:
        with tarfile.open(path, "r:gz") as tar:
            infos = tar.getmembers()
    except (OSError, EOFError, tarfile.TarError, zlib.error) as e:
        check(False, f"{path.name} is a readable .tar.gz", str(e))
        return ""
    tops = sorted({info.name.split("/", 1)[0] for info in infos})
    found = re.fullmatch(rf"antb1-(\S+)-{re.escape(target)}", tops[0]) if len(tops) == 1 else None
    check(
        found is not None and version in ("", found.group(1)),
        f"{path.name} unpacks into antb1-{version or '<version>'}-{target}/",
        str(tops)[:200],
    )
    top = tops[0] if found else f"antb1-{version}-{target}"
    names = [info.name for info in infos]
    check(len(names) == len(set(names)), f"{path.name} holds each member once", str(sorted(names))[:200])
    check_members(path, {info.name: info for info in infos}, top, files)
    xattrs = sorted({key for info in infos for key in info.pax_headers if key.startswith(XATTR_HEADERS)})
    check(not xattrs, f"{path.name} carries no extended attributes", ", ".join(xattrs)[:200])
    return found.group(1) if found and len(FAILURES) == before else ""


def tar_entry(name: str, kind: bytes, mode: int, size: int = 0) -> tarfile.TarInfo:
    info = tarfile.TarInfo(name)
    info.type, info.mode, info.size = kind, mode, size
    return info


def import_image(engine: str, binary: Path, kit: Path) -> str:
    """Imports an image whose root holds only /antb1 and the kit's Parquet files in /kit (read-only); returns its id,
    or "" when the engine refused it."""
    rootfs = io.BytesIO()
    with tarfile.open(fileobj=rootfs, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        tar.addfile(tar_entry("kit", tarfile.DIRTYPE, 0o755))
        with open(binary, "rb") as f:
            tar.addfile(tar_entry("antb1", tarfile.REGTYPE, 0o755, binary.stat().st_size), f)
        for name in KIT_FILES:
            with open(kit / name, "rb") as f:
                tar.addfile(tar_entry(f"kit/{name}", tarfile.REGTYPE, 0o444, (kit / name).stat().st_size), f)
    try:
        out = subprocess.run([engine, "import", "-"], input=rootfs.getvalue(), capture_output=True, timeout=RUN_TIMEOUT)
    except subprocess.TimeoutExpired:
        check(False, "the engine imports an image of /antb1 and /kit", f"timed out after {RUN_TIMEOUT} s")
        return ""
    image = out.stdout.decode(errors="replace").split()
    ok = check(
        out.returncode == 0 and len(image) == 1,
        "the engine imports an image of /antb1 and /kit",
        out.stderr.decode(errors="replace").strip()[:200],
    )
    return image[0] if ok else ""


def gate_verify(args: argparse.Namespace) -> None:
    """The package as a user gets it, on a runner without pixi, DuckDB or a build: see the module docstring."""
    target, engine = args.target, args.container
    linux = target.startswith("linux-")
    if sys.version_info < (3, 11) or not hasattr(tarfile, "data_filter"):
        python = f"{sys.executable} (Python {platform.python_version()})"
        raise CannotVerify(f"{python} is not Python 3.11 or newer with tarfile.data_filter")
    if host_target() != target:
        raise CannotVerify(f"this runner ({platform.system()} {platform.machine()}) does not run {target}")
    exe = ""
    if engine != "none":
        if not linux:
            raise CannotVerify(f"--container {engine} is for the Linux targets")
        exe = shutil.which(engine) or ""
        probe = run([exe, "info"]) if exe else None
        if probe is None or probe.returncode != 0:
            raise CannotVerify(f"`{engine} info` fails: " + (probe.stderr.strip()[:200] if probe else "not on PATH"))
    assets, kit = Path(args.assets), Path(args.kit).resolve()
    files = [f"antb1-{target}.tar.gz"] + ([f"antb1-{target}.debug.tar.gz"] if linux else [])
    sums = read_sums(Path(args.sums))
    for name in files:
        path = assets / name
        digest = sha256_file(path) if path.is_file() else ""
        why = "missing" if not digest else "not listed" if name not in sums else "another digest"
        check(bool(digest) and sums.get(name) == digest, f"{name} matches {Path(args.sums).name}", why)
    read_kit(kit)
    if FAILURES:
        return  # nothing is unpacked from a file that is not the published one, nor run without its kit
    version = check_tarball(assets / files[0], PACKAGE_FILES, target, args.version or "")
    if linux and version:
        check_tarball(assets / files[1], DEBUG_FILES, target, version)
    if FAILURES:
        return
    with tempfile.TemporaryDirectory(prefix="antb1-verify-") as tmp:
        try:
            with tarfile.open(assets / files[0], "r:gz") as tar:
                tar.extractall(Path(tmp, "package"), filter="data")
        except (OSError, EOFError, tarfile.TarError, zlib.error) as e:
            check(False, f"{files[0]} unpacks with tarfile's data filter", str(e))
            return
        top = Path(tmp, "package", f"antb1-{version}-{target}")
        try:
            info = json.loads((top / "BUILDINFO.json").read_text())
        except (OSError, ValueError):
            info = {}
        named = [info.get(key) for key in ("target", "version", "flavor", "license")] if isinstance(info, dict) else []
        check(
            named == [target, version, "static-thinlto", "Apache-2.0"],
            f"BUILDINFO.json names {target}, {version}, static-thinlto and Apache-2.0",
            str(named),
        )
        if args.commit:
            check(isinstance(info, dict) and info.get("commit") == args.commit, f"BUILDINFO.json names {args.commit}")
        for name in ("LICENSE", "NOTICE") if args.texts else ():
            want = Path(args.texts, name)
            check(want.is_file() and (top / name).read_bytes() == want.read_bytes(), f"the package's {name} is {want}")
        binary = top / "antb1"
        check(os.access(binary, os.X_OK), "the unpacked antb1 is executable")
        if target == "darwin-arm64":
            signed = run([CODESIGN, "--verify", "--strict", str(binary)]) if os.path.exists(CODESIGN) else None
            check(signed is not None and signed.returncode == 0, "the signature verifies (codesign --verify --strict)",
                  signed.stderr.strip()[:200] if signed else f"no {CODESIGN}")  # fmt: skip
        cwd = Path(tmp, "cwd")
        cwd.mkdir()
        gate_runs(Host(str(binary), str(cwd), str(kit), RUN_TIMEOUT, " on this runner"), target, version, args.commit)
        if exe:
            image = import_image(exe, binary, kit)
            if image:
                try:
                    gate_runs(Container(exe, image, RUN_TIMEOUT), target, version, args.commit)
                finally:
                    run([exe, "rmi", "--force", image])


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prefix")
    p.add_argument("--build", required=True)
    p.add_argument("--prefix", required=True)
    p.add_argument("--env", required=True)
    p.add_argument("--lock", required=True)
    p.add_argument("--report", required=True)
    b = sub.add_parser("binary")
    b.add_argument("--bin", required=True)
    b.add_argument("--env", required=True)
    b.add_argument("--build-dir", help="the build tree of the binary: also check its link line")
    b.add_argument("--linkage-only", action="store_true", help="only the checks that need no run of the binary")
    b.add_argument("--kit", help="the smoke kit to query (default: one written with the env's DuckDB)")
    k = sub.add_parser("package")
    k.add_argument("--pkg", required=True, help="the package directory (build/static/package)")
    k.add_argument("--target", required=True, choices=TARGETS)
    s = sub.add_parser("kit")
    s.add_argument("--env", required=True)
    s.add_argument("--out", required=True, help="a new or empty directory")
    v = sub.add_parser("verify")
    v.add_argument("--assets", required=True, help="the directory of the target's tarballs")
    v.add_argument("--sums", required=True, help="a SHA256SUMS with the target's lines (other targets' may follow)")
    v.add_argument("--kit", required=True, help="the smoke kit (kit)")
    v.add_argument("--target", required=True, choices=TARGETS)
    v.add_argument("--version", help="the version the package must have")
    v.add_argument("--commit", default="", help="the commit the package must name")
    v.add_argument("--texts", help="a directory with the LICENSE and NOTICE the package must carry")
    v.add_argument("--container", choices=["docker", "podman", "none"], default="none",
                   help="Linux: also run the binary in an image that holds only it and the kit")  # fmt: skip
    args = parser.parse_args(argv)
    try:
        if args.cmd == "prefix":
            gate_prefix(args)
        elif args.cmd == "package":
            gate_package(args)
        elif args.cmd == "kit":
            gate_kit(Path(args.env), Path(args.out))
        elif args.cmd == "verify":
            gate_verify(args)
        else:
            gate_binary(args)
    except CannotVerify as e:
        print(f"static-gates: cannot verify: {e}", file=sys.stderr)
        return 2
    if FAILURES:
        print(f"static-gates: FAIL ({len(FAILURES)}): " + "; ".join(FAILURES), file=sys.stderr)
        return 1
    print("static-gates: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
