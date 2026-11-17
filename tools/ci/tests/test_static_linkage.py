"""Darwin ABI compatibility gates, with synthetic LLVM tool output (run by `pixi run lint`)."""

from __future__ import annotations

import importlib.util
import subprocess
from pathlib import Path
from types import ModuleType

import pytest

REPO = Path(__file__).resolve().parents[3]
ABI_CHECK = "libc++abi imports within the macOS compatibility baseline"


def load_gates() -> ModuleType:
    spec = importlib.util.spec_from_file_location("static_linkage_under_test", REPO / "scripts/static_gates.py")
    assert spec is not None and spec.loader is not None
    gates = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gates)
    return gates


def check_linkage(monkeypatch: pytest.MonkeyPatch, imports: str, *, nm_status: int = 0) -> ModuleType:
    gates = load_gates()

    def run(cmd: list[str]) -> subprocess.CompletedProcess:
        tool = Path(cmd[0]).name
        output, status = "", 0
        if tool == "llvm-otool":
            output = (
                "antb1:\n /usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n"
                " /usr/lib/libc++abi.dylib (compatibility version 1.0.0)\n"
                if cmd[1] == "-L"
                else "cmd LC_BUILD_VERSION\n minos 13.0\n"
            )
        elif tool == "llvm-nm":
            if cmd[-1] == "antb1":
                output, status = imports, nm_status
            else:
                # New conda libc++ re-exports the entire system libc++abi, so its undefined
                # symbol table no longer lists ABI symbols it does not itself call.
                output = "(undefined) external ___cxa_throw (from libc++abi)\n"
        elif tool not in {"llvm-objdump", "codesign"}:
            pytest.fail(f"unexpected tool: {cmd}")
        return subprocess.CompletedProcess(cmd, status, output, "nm failed" if status else "")

    monkeypatch.setattr(gates, "run", run)
    gates.gate_macos_linkage(Path("env"), "antb1")
    return gates


@pytest.mark.parametrize(
    "symbol",
    [
        "__ZNSt8bad_castC2Ev",
        "__ZNSt9bad_allocC2Ev",
        "__ZNSt9bad_allocD2Ev",
        "__ZTVN10__cxxabiv119__pointer_type_infoE",
        "__ZTVN10__cxxabiv120__function_type_infoE",
        "__ZnamRKSt9nothrow_t",
        "__ZnwmRKSt9nothrow_t",
        "___cxa_bad_cast",
    ],
)
def test_macos_abi_accepts_symbols_omitted_from_conda_imports(monkeypatch: pytest.MonkeyPatch, symbol: str) -> None:
    gates = check_linkage(monkeypatch, f"(undefined) external {symbol} (from libc++abi)\n")
    assert not gates.FAILURES


def test_macos_abi_rejects_symbols_outside_baseline(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    symbol = "___cxa_future_api_not_in_baseline"
    gates = check_linkage(monkeypatch, f"(undefined) external {symbol} (from libc++abi)\n")
    assert ABI_CHECK in gates.FAILURES
    assert symbol in capsys.readouterr().out


@pytest.mark.parametrize("imports", ["", "(undefined) external ___cxa_throw (from libc++abi)\n"])
def test_macos_abi_fails_when_nm_fails(monkeypatch: pytest.MonkeyPatch, imports: str) -> None:
    gates = check_linkage(monkeypatch, imports, nm_status=1)
    assert gates.FAILURES


def test_macos_abi_fails_without_symbol_output(monkeypatch: pytest.MonkeyPatch) -> None:
    gates = check_linkage(monkeypatch, "")
    assert gates.FAILURES
