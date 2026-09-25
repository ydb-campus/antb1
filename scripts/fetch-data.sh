#!/usr/bin/env bash
# `pixi run fetch-data [--full]` (NETWORK): downloads the pinned ClickBench files of tools/data/clickbench.lock
# (hits_0.parquet, 122 MB, and queries.sql) into $ANTB1_DATA_DIR (default ~/.cache/antb1/clickbench; a relative path
# is relative to the repository root). `pixi run test-data` runs it first. Nothing it downloads is ever committed or
# uploaded (docs/adr/0006-test-strategy-and-data-policy.md).
#
# - A file whose size and sha256 already match the pin is kept, so a second run needs no network.
# - A download goes to <file>.part through conda curl (retries, HTTPS only, never a range request: an interrupted
#   download starts again from scratch), is checked against the pinned size and sha256, then renamed atomically.
# - A data directory inside the repository must be ignored by git (e.g. .cache/clickbench, as in CI).
#
# --full (host only, about 14.7 GB): also downloads the 100 partitions hits_{0..99}.parquet into
#   $ANTB1_DATA_DIR/full/ after a free-space check. hits_0 is verified against the pin; the other partitions are
#   trust-on-first-use: the first download records their sha256 in $ANTB1_DATA_DIR/full/SHA256SUMS (outside the
#   repository, never committed) and every later run verifies them against it. Then run the data tests on them:
#     ANTB1_HITS_FILES="$HOME/.cache/antb1/clickbench/full/hits_*.parquet" pixi run test-data
set -euo pipefail
[ "${1:-}" = "--" ] && shift

usage() { awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "${BASH_SOURCE[0]}"; }

full=0
while [ $# -gt 0 ]; do
  case "$1" in
    --full) full=1 ;;
    -h | --help) usage; exit 0 ;;
    *) echo "fetch-data: unknown option '$1'" >&2; usage >&2; exit 2 ;;
  esac
  shift
done

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
lock="$root/tools/data/clickbench.lock"
data_dir="${ANTB1_DATA_DIR:-$HOME/.cache/antb1/clickbench}"
case "$data_dir" in
  /*) ;;
  *) data_dir="$root/$data_dir" ;; # as in the data tests (cmake/scripts/DataPaths.cmake)
esac
data_dir="${data_dir%/}"

# ClickBench data must never become committable: a data dir inside the work tree must be git-ignored.
case "$data_dir/" in
  "$root"/*)
    rel="${data_dir#"$root"}"
    rel="${rel#/}"
    if ! git -C "$root" check-ignore -q "${rel:+$rel/}queries.sql" 2>/dev/null; then
      echo "fetch-data: ANTB1_DATA_DIR=$data_dir is inside the repository but not ignored by git;" \
        "use a directory under .cache/ or outside the repository" >&2
      exit 2
    fi
    ;;
esac

sha256_of() { cmake -E sha256sum "$1" | awk '{ print $1 }'; }
size_of() { wc -c <"$1" | tr -d ' '; }
# matches <file> <bytes> <sha256>: the file exists with this size and hash.
matches() { [ -f "$1" ] && [ "$(size_of "$1")" = "$2" ] && [ "$(sha256_of "$1")" = "$3" ]; }

# download <url> <dest> [<sha256> [<bytes>]]: downloads to <dest>.part, verifies what is given, renames atomically.
download() {
  local url=$1 dest=$2 want_sha=${3:-} want_bytes=${4:-}
  local part="$dest.part"
  rm -f "$part"
  echo "fetch-data: downloading $url"
  if ! curl -fL --proto '=https' --proto-redir '=https' --retry 5 --retry-all-errors --retry-delay 5 \
    --connect-timeout 30 --no-progress-meter -o "$part" "$url"; then
    rm -f "$part"
    echo "fetch-data: download failed: $url" >&2
    return 1
  fi
  local bytes sha
  bytes=$(size_of "$part")
  if [ -n "$want_bytes" ] && [ "$bytes" != "$want_bytes" ]; then
    rm -f "$part"
    echo "fetch-data: $url: $bytes bytes, the pin says $want_bytes" >&2
    return 1
  fi
  sha=$(sha256_of "$part")
  if [ -n "$want_sha" ] && [ "$sha" != "$want_sha" ]; then
    rm -f "$part"
    echo "fetch-data: $url: sha256 $sha, the pin says $want_sha" >&2
    return 1
  fi
  mv -f "$part" "$dest"
  echo "fetch-data: $(basename "$dest"): downloaded ($bytes bytes, sha256 $sha)"
}

mkdir -p "$data_dir"
status=0
hits0_url="" hits0_sha="" hits0_bytes=""
while read -r name sha bytes url extra; do
  case "$name" in "" | "#"*) continue ;; esac
  if [ -z "${url:-}" ] || [ -n "${extra:-}" ] || [[ ! "$sha" =~ ^[0-9a-f]{64}$ ]] ||
    [[ ! "$bytes" =~ ^[0-9]+$ ]] || [[ "$name" == */* ]]; then
    echo "fetch-data: $lock: malformed line for '$name' (expected: <name> <sha256> <bytes> <url>)" >&2
    exit 2
  fi
  if [ "$name" = hits_0.parquet ]; then
    hits0_url=$url hits0_sha=$sha hits0_bytes=$bytes
  fi
  if matches "$data_dir/$name" "$bytes" "$sha"; then
    echo "fetch-data: $name: ok (cached, size and sha256 match the pin)"
  else
    download "$url" "$data_dir/$name" "$sha" "$bytes" || status=1
  fi
done <"$lock"

if [ "$full" = 1 ]; then
  if [ -z "$hits0_url" ]; then
    echo "fetch-data: --full: no hits_0.parquet in $lock" >&2
    exit 2
  fi
  base=${hits0_url%/hits_0.parquet}
  full_dir="$data_dir/full"
  sums="$full_dir/SHA256SUMS"
  mkdir -p "$full_dir"
  touch "$sums"
  declare -A recorded=()
  while read -r sha file; do
    [ -n "${file:-}" ] && recorded[$file]=$sha
  done <"$sums"

  missing=0
  for ((i = 0; i < 100; i++)); do
    [ -f "$full_dir/hits_$i.parquet" ] || missing=$((missing + 1))
  done
  # About 147 MB per partition: assume 160 MB, plus 1 GiB of headroom.
  need_kb=$((missing * 160 * 1024 + 1024 * 1024))
  avail_kb=$(df -Pk "$full_dir" | awk 'NR == 2 { print $4 }')
  if [ "$missing" -gt 0 ] && [ "$avail_kb" -lt "$need_kb" ]; then
    echo "fetch-data: --full: $missing partitions to download need about $((need_kb / 1048576)) GiB free in" \
      "$full_dir; only $((avail_kb / 1048576)) GiB are available" >&2
    exit 1
  fi

  for ((i = 0; i < 100; i++)); do
    name="hits_$i.parquet"
    dest="$full_dir/$name"
    want=${recorded[$name]:-}
    want_bytes=""
    if [ "$i" = 0 ]; then
      want=$hits0_sha # the pin, never trust-on-first-use
      want_bytes=$hits0_bytes
    fi
    if [ -f "$dest" ]; then
      have=$(sha256_of "$dest")
      if [ -z "$want" ]; then
        # Downloaded and renamed, but the run stopped before recording it.
        echo "$have  $name" >>"$sums"
        echo "fetch-data: full/$name: recorded in SHA256SUMS"
        continue
      fi
      if [ "$have" = "$want" ]; then
        echo "fetch-data: full/$name: ok (cached, sha256 verified)"
        continue
      fi
      echo "fetch-data: full/$name: sha256 $have differs from $want; downloading it again" >&2
    fi
    if [ "$i" = 0 ] && matches "$data_dir/hits_0.parquet" "$hits0_bytes" "$hits0_sha"; then
      cp "$data_dir/hits_0.parquet" "$dest.part"
      mv -f "$dest.part" "$dest"
      echo "fetch-data: full/$name: copied from $data_dir (pinned)"
      continue
    fi
    if ! download "$base/$name" "$dest" "$want" "$want_bytes"; then
      status=1
      continue
    fi
    if [ -z "$want" ]; then
      echo "$(sha256_of "$dest")  $name" >>"$sums"
    fi
  done
fi

if [ "$status" -ne 0 ]; then
  echo "fetch-data: FAIL (see above; re-run \`pixi run fetch-data\` to retry)" >&2
  exit 1
fi
echo "fetch-data: OK: $data_dir"
if [ "$full" = 1 ]; then
  echo "fetch-data: data tests on all partitions: ANTB1_HITS_FILES=\"$data_dir/full/hits_*.parquet\" pixi run test-data"
fi
