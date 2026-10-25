#!/usr/bin/env python3
"""Gates of the static build (docs/adr/0033-standalone-static-binary.md), run in env `static`.

  prefix  the static Arrow prefix that scripts/static-arrow.sh built: static archives only; every C and C++ file of
          Arrow and of each bundled dependency compiled with -O3 and -flto=thin; no header of a bundled library
          read from the environment; every download a local file; Arrow's option set; a report of the LLVM bitcode
          and native members of each archive.
  binary  a standalone antb1 (scripts/static-check.sh): linkage (Linux: a static ELF executable without loader or
          dynamic section, pthread linked whole, no NSS; macOS: only system libraries, no LC_RPATH, the deployment
          target, a valid signature), the link flags when its build tree is given, build identity, the allocator
          restart, and smoke tests of codecs, threads and errors on Parquet files that DuckDB writes at run time.

Prints one line per check and exits 1 when any fails.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FAILURES: list[str] = []


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


def run_bin(binary: str, args: list[str], env: dict[str, str] | None = None, cwd: str | None = None):
    return subprocess.run(
        [binary, *args], capture_output=True, text=True, errors="replace", env=env or {}, cwd=cwd, timeout=600
    )


def gate_identity(binary: str, tmp: str) -> None:
    plain = run_bin(binary, ["version"], cwd=tmp)
    check(
        plain.returncode == 0 and re.fullmatch(r"antb1 \S+ \(Apache Arrow \d+\.\d+\.\d+\)\n", plain.stdout) is not None,
        "`env -i antb1 version` runs",
        (plain.stdout + plain.stderr).strip()[:200],
    )
    verbose = run_bin(binary, ["version", "--verbose"], cwd=tmp)
    fields = dict(ln.split(": ", 1) for ln in verbose.stdout.splitlines()[1:] if ": " in ln)
    check(
        verbose.returncode == 0 and fields.get("flavor") == "static-thinlto",
        "flavor static-thinlto",
        fields.get("flavor", verbose.stderr.strip()[:200]),
    )
    check(
        "antb1-static" in fields.get("arrow", ""),
        "Arrow is the static build (package kind antb1-static)",
        fields.get("arrow", ""),
    )
    codecs = fields.get("codecs", "")
    check(bool(codecs) and " no" not in codecs, "every codec is available", codecs)
    if platform.system() == "Linux":
        # mimalloc prints its options when it starts; after the restart (ADR 0017) purge_delay is -1.
        restart = run_bin(binary, ["version"], env={"MIMALLOC_VERBOSE": "1"}, cwd=tmp)
        check(
            re.search(r"option 'purge_delay': -1\b", restart.stderr) is not None,
            "the allocator restart sets MIMALLOC_PURGE_DELAY=-1",
            "no purge_delay -1 in mimalloc's output",
        )
        check(
            fields.get("mimalloc purge delay") == "-1",
            "version --verbose reports the purge delay -1",
            fields.get("mimalloc purge delay", ""),
        )


def gate_cpu_guard(env: Path, binary: str, tmp: str) -> None:
    """A raised CPU baseline comes with the guard of src/cli/cpu_guard.cc: GuardCpu must be an entry of .preinit_array
    (the section alone proves nothing: glibc 2.28's libpthread.a puts its own initializer there)."""
    verbose = run_bin(binary, ["version", "--verbose"], cwd=tmp).stdout
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


CODECS = ["uncompressed", "snappy", "gzip", "zstd", "lz4_raw", "brotli"]
SMOKE_SQL = "SELECT COUNT(*), SUM(a), MIN(b), MAX(b), COUNT(DISTINCT c) FROM t"


def gate_smoke(env: Path, binary: str, tmp: str) -> None:
    duckdb = tool(env, "duckdb")
    for codec in CODECS:
        sql = (
            f"COPY (SELECT range AS a, (range * 7) % 1000 AS b, 'k' || (range % 97) AS c FROM range(200000)) "
            f"TO '{tmp}/{codec}.parquet' (FORMAT parquet, COMPRESSION {codec}, ROW_GROUP_SIZE 10000)"
        )
        out = run([duckdb, "-c", sql])
        if not check(out.returncode == 0, f"DuckDB writes a {codec} file", out.stderr.strip()[:200]):
            continue
        res = run_bin(
            binary, ["query", "-c", SMOKE_SQL, "--table", f"t={tmp}/{codec}.parquet", "--format", "csv"], cwd=tmp
        )
        rows = res.stdout.strip().splitlines()
        check(
            res.returncode == 0 and rows[-1:] == ["200000,19999900000,0,999,97"],
            f"{codec}: query answer",
            (res.stdout + res.stderr).strip()[-200:],
        )
    one = run_bin(binary, ["query", "-c", SMOKE_SQL, "--table", f"t={tmp}/zstd.parquet", "--threads", "1"], cwd=tmp)
    four = run_bin(binary, ["query", "-c", SMOKE_SQL, "--table", f"t={tmp}/zstd.parquet", "--threads", "4"], cwd=tmp)
    check(
        one.returncode == 0 and one.stdout == four.stdout,
        "--threads 4 answers as --threads 1",
        (four.stdout + four.stderr).strip()[-200:],
    )
    data = Path(f"{tmp}/zstd.parquet").read_bytes()
    Path(f"{tmp}/truncated.parquet").write_bytes(data[: len(data) // 2])
    bad = run_bin(binary, ["query", "-c", SMOKE_SQL, "--table", f"t={tmp}/truncated.parquet"], cwd=tmp)
    check(
        bad.returncode == 3,
        "a truncated file exits with 3 (an exception through the static unwinder)",
        f"exit {bad.returncode}: {bad.stderr.strip()[:200]}",
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


def gate_binary(args: argparse.Namespace) -> None:
    env = Path(args.env)
    binary = os.path.abspath(args.bin)
    if not gate_linkage(env, binary) or args.linkage_only:
        return
    if args.build_dir:
        gate_link_flags(env, Path(args.build_dir))
    with tempfile.TemporaryDirectory(prefix="antb1-static-check-") as tmp:
        gate_identity(binary, tmp)
        gate_cpu_guard(env, binary, tmp)
        gate_smoke(env, binary, tmp)


def main() -> int:
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
    args = parser.parse_args()
    if args.cmd == "prefix":
        gate_prefix(args)
    else:
        gate_binary(args)
    if FAILURES:
        print(f"static-gates: FAIL ({len(FAILURES)}): " + "; ".join(FAILURES), file=sys.stderr)
        return 1
    print("static-gates: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
