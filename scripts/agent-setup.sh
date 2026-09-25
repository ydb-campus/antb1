#!/usr/bin/env bash
# antb1 sandbox bootstrap: ensure pixi >= 0.81.0, then install the locked environments. Idempotent.
# Linux x86_64/aarch64 only (on macOS install pixi from https://pixi.prefix.dev).
# - An existing pixi >= 0.81.0 (on PATH or in the private dir) is used as is and never overwritten.
# - Otherwise the pinned, sha256-verified pixi 0.81.0 is installed into a PRIVATE dir (default
#   ~/.cache/antb1/pixi-0.81.0), from the GitHub release (static musl binary) or from conda-forge
#   (dynamic binary: needs glibc and OpenSSL 3, which stock Ubuntu 20.04 lacks).
# - The pixi dir is added to PATH for later steps via $CLAUDE_ENV_FILE (Claude Code), $GITHUB_PATH (GitHub Actions)
#   and, with --persist-bashrc, ~/.bashrc and ~/.profile (Codex and other shells).
# Used by: cloud agent sandboxes, CI bootstrap checks and fresh Linux clones (humans too).
#
# Usage: bash scripts/agent-setup.sh [--envs "default lint"] [--pixi-only] [--with-data] [--persist-bashrc]
#   --envs LIST        pixi environments to install (default: "default lint"; `gcc` is linux-64 only)
#   --pixi-only        only make sure pixi is available; install no environment
#   --with-data        also run `pixi run fetch-data` (NETWORK: the pinned ClickBench files, 122 MB; non-fatal)
#   --persist-bashrc   put the pixi dir on PATH in ~/.bashrc and ~/.profile (and /usr/local/bin/pixi as root)
# Env:   ANTB1_PIXI_SOURCE=auto|github|conda (auto: conda first when CLAUDE_CODE_REMOTE=true, else github first);
#        ANTB1_PIXI_DIR (private install dir).
set -euo pipefail

PIXI_VERSION=0.81.0 # R005: must equal requires-pixi in pixi.toml and pixi-version in the workflows

usage() { awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "${BASH_SOURCE[0]}"; }

envs="default lint"
pixi_only=0
with_data=0
persist_bashrc=0
while [ $# -gt 0 ]; do
  case "$1" in
    --envs)
      if [ $# -lt 2 ] || [ -z "$2" ]; then
        echo "agent-setup: --envs needs a value, e.g. --envs \"default lint\"" >&2
        exit 2
      fi
      envs=$2
      shift 2
      ;;
    --pixi-only) pixi_only=1; shift ;;
    --with-data) with_data=1; shift ;;
    --persist-bashrc) persist_bashrc=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "agent-setup: unknown option '$1'" >&2; usage >&2; exit 2 ;;
  esac
done

if [ "$(uname -s)" != Linux ]; then
  echo "agent-setup: Linux only; on macOS install pixi >= $PIXI_VERSION from https://pixi.prefix.dev" >&2
  exit 2
fi
# Pinned release assets; sha256 verified against the GitHub release .sha256 files and anaconda.org metadata.
arch=$(uname -m)
case "$arch" in
  x86_64)
    gh_sha=7aa3ec39aecceff9062fa2ed4d42cbaa0bdc25ddea727d048e061cf188d434f6
    conda_file=linux-64/pixi-0.81.0-hf01adef_0.conda
    conda_sha=691c4f465b27b9ed0aeee0849c5a1f8b234f1ef02d4c7d7572855bcca84d3e10
    ;;
  aarch64)
    gh_sha=9f8d2113fe9dc01788a65f5c2acec34fa56b1193461a5c3e9a775d6d2d621bcb
    conda_file=linux-aarch64/pixi-0.81.0-hc342849_0.conda
    conda_sha=cc81f2c626bd3306badf047bd47806f6dd1dabdf1a37631acb08f0460486313b
    ;;
  *) echo "agent-setup: unsupported architecture $arch (supported: x86_64, aarch64)" >&2; exit 2 ;;
esac

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
pin_dir=${ANTB1_PIXI_DIR:-$HOME/.cache/antb1/pixi-$PIXI_VERSION}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
PIXI=""

version_ok() { # $1 = candidate binary; true if its version is >= PIXI_VERSION
  local v
  v=$("$1" --version 2>/dev/null | awk '{print $2}') || return 1
  [ -n "$v" ] && [ "$(printf '%s\n%s\n' "$PIXI_VERSION" "$v" | sort -V | head -n 1)" = "$PIXI_VERSION" ]
}

find_pixi() {
  local c
  for c in "$(command -v pixi 2>/dev/null || true)" "$pin_dir/pixi"; do
    if [ -n "$c" ] && [ -x "$c" ] && version_ok "$c"; then
      PIXI=$c
      return 0
    fi
  done
  return 1
}

sha_ok() { echo "$1  $2" | sha256sum -c --status -; }

from_github() { # static musl binary; GitHub release assets may be blocked in some cloud sandboxes
  curl -fsSL --retry 3 --connect-timeout 20 -o "$tmp/pixi.tar.gz" \
    "https://github.com/prefix-dev/pixi/releases/download/v${PIXI_VERSION}/pixi-${arch}-unknown-linux-musl.tar.gz" ||
    return 1
  sha_ok "$gh_sha" "$tmp/pixi.tar.gz" || { echo "agent-setup: sha256 mismatch (github)" >&2; return 1; }
  tar -xzf "$tmp/pixi.tar.gz" -C "$tmp" pixi || return 1
  "$tmp/pixi" --version >/dev/null 2>&1 || {
    echo "agent-setup: the github pixi binary does not run here" >&2
    return 1
  }
  install -m 0755 "$tmp/pixi" "$pin_dir/pixi"
}

unzstd() { # stdin .tar.zst -> stdout .tar
  if command -v zstd >/dev/null 2>&1; then
    zstd -dc
  else
    python3 -c '
import sys
try:
    import zstandard
    sys.stdout.buffer.write(zstandard.ZstdDecompressor().stream_reader(sys.stdin.buffer).read())
except ImportError:
    from compression import zstd  # Python >= 3.14
    sys.stdout.buffer.write(zstd.decompress(sys.stdin.buffer.read()))
'
  fi
}

from_conda() { # conda.anaconda.org; dynamic binary (glibc + OpenSSL 3)
  curl -fsSL --retry 3 --connect-timeout 20 -o "$tmp/pixi.conda" "https://conda.anaconda.org/conda-forge/$conda_file" ||
    return 1
  sha_ok "$conda_sha" "$tmp/pixi.conda" || { echo "agent-setup: sha256 mismatch (conda)" >&2; return 1; }
  command -v python3 >/dev/null 2>&1 || {
    echo "agent-setup: python3 is needed to unpack the conda package" >&2
    return 1
  }
  if ! command -v zstd >/dev/null 2>&1 && [ "$(id -u)" = 0 ] && command -v apt-get >/dev/null 2>&1; then
    { timeout 60 apt-get update -qq && timeout 60 apt-get install -y -qq zstd; } >/dev/null 2>&1 || true
  fi
  # A .conda file is a zip holding pkg-*.tar.zst (the payload) and info-*.tar.zst.
  python3 -c '
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
name = next(n for n in z.namelist() if n.startswith("pkg-") and n.endswith(".tar.zst"))
sys.stdout.buffer.write(z.read(name))
' "$tmp/pixi.conda" | unzstd | tar -x -C "$tmp" bin/pixi || return 1
  "$tmp/bin/pixi" --version >/dev/null 2>&1 || {
    echo "agent-setup: the conda-forge pixi binary does not run here (it needs OpenSSL 3)" >&2
    return 1
  }
  install -m 0755 "$tmp/bin/pixi" "$pin_dir/pixi"
}

if ! find_pixi; then
  mkdir -p "$pin_dir"
  sources=${ANTB1_PIXI_SOURCE:-auto}
  if [ "$sources" = auto ]; then
    if [ "${CLAUDE_CODE_REMOTE:-}" = true ]; then sources="conda github"; else sources="github conda"; fi
  fi
  for s in $sources; do
    case "$s" in
      github|conda) ;;
      *) echo "agent-setup: ANTB1_PIXI_SOURCE must be auto, github or conda (got '$s')" >&2; exit 2 ;;
    esac
    echo "agent-setup: installing pixi $PIXI_VERSION from $s into $pin_dir" >&2
    if "from_$s"; then break; fi
    echo "agent-setup: pixi from $s failed" >&2
  done
  find_pixi || { echo "agent-setup: cannot install pixi $PIXI_VERSION" >&2; exit 1; }
fi

bin=$(dirname "$PIXI")
case ":$PATH:" in
  *":$bin:"*) on_path=1 ;;
  *) on_path=0 ;;
esac
export PATH="$bin:$PATH"
line="export PATH=\"$bin:\$PATH\""
if [ -n "${CLAUDE_ENV_FILE:-}" ] && ! grep -qsxF "$line" "$CLAUDE_ENV_FILE"; then
  echo "$line" >>"$CLAUDE_ENV_FILE"
fi
if [ -n "${GITHUB_PATH:-}" ] && ! grep -qsxF "$bin" "$GITHUB_PATH"; then echo "$bin" >>"$GITHUB_PATH"; fi
if [ "$persist_bashrc" = 1 ]; then
  for rc in "$HOME/.bashrc" "$HOME/.profile"; do
    if ! grep -qsxF "$line" "$rc"; then
      # Line 1: before the early `return` of non-interactive shells in Debian/Ubuntu's default ~/.bashrc.
      if [ -s "$rc" ]; then sed -i "1i $line" "$rc"; else echo "$line" >"$rc"; fi
    fi
  done
  if [ "$(id -u)" = 0 ] && [ "$bin" != /usr/local/bin ]; then ln -sf "$PIXI" /usr/local/bin/pixi; fi
fi
if [ "$on_path" = 0 ] && [ "$persist_bashrc" = 0 ]; then
  echo "agent-setup: pixi is not on your PATH yet; run: $line" >&2
fi

cd "$root"
if [ "$pixi_only" = 1 ]; then
  echo "antb1 agent-setup OK: $("$PIXI" --version) at $PIXI (no environments installed)"
  exit 0
fi
if [ ! -f pixi.lock ]; then
  echo "agent-setup: no pixi.lock yet; run 'pixi lock' with pixi $PIXI_VERSION, then re-run this script" >&2
  exit 0
fi

args=()
installed=()
for e in $envs; do
  if [ "$e" = gcc ] && [ "$arch" != x86_64 ]; then
    echo "agent-setup: skipping env 'gcc' (linux-64 only)" >&2
    continue
  fi
  args+=(-e "$e")
  installed+=("$e")
done
if [ ${#args[@]} -gt 0 ]; then
  "$PIXI" install --locked "${args[@]}"
fi

if [ "$with_data" = 1 ]; then
  "$PIXI" run --frozen -e default fetch-data || echo "agent-setup: fetch-data failed (non-fatal)" >&2
fi

echo "antb1 agent-setup OK: $("$PIXI" --version) at $PIXI; envs: ${installed[*]:-(none)}"
