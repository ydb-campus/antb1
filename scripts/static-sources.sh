#!/usr/bin/env bash
# `bash scripts/static-sources.sh <lock> --pins | --fetch <dir> | --variables <dir> | --check-urls`: the pinned
# sources of the static build (docs/adr/0033-standalone-static-binary.md), for scripts/static-arrow.sh, which runs it.
#   --pins             the pins, sorted, one `pin <name> <sha256> <bytes> <variable>` line each (the prefix key's)
#   --fetch <dir>      every pinned file into <dir>, each from the first of its copies that serves the pinned bytes
#   --variables <dir>  `<variable>=<dir>/<name>` per pin (`-` is Arrow's own source)
#   --check-urls       every URL against its pin (`pixi run static-arrow --check-urls`)
# Only the pins decide what static-arrow builds (they enter its prefix key); this file and the URLs only say where
# the same bytes live, so neither enters the key and a change here never forces Arrow to be rebuilt.
#
# Download (static_sources_fetch <dir>): per file, its URLs in order until one serves the pinned size and sha256. A
# file there that already matches its pin is kept. Each URL gets one attempt, and one more $ANTB1_SOURCES_RETRY_DELAY
# seconds later (default 3) after an error that may pass: a refused or broken connection, HTTP 403, 408, 425, 429 or
# 5xx. Any other failure (a dead URL, a timeout, other bytes, a larger file) goes straight to the next URL. An attempt
# is bounded: 10 s to connect, at most the pinned size, at most 30 s plus 1 s per 64 KiB of the pin in all, and a
# transfer under 1 KiB/s for 30 s counts as stalled; HTTPS only, redirects included. A file that no URL serves stops
# the run.
#
# Check (static_sources_check): every URL on its own against its pin, a full download each, except a single-server
# origin (archive.apache.org, zlib.net), which gets a HEAD request only (status 200 and the pinned size), so CI never
# downloads from it every night. A dead URL (HTTP 404 or 410) or other bytes, a larger file included, fail the check
# at once. Any other failure gets one more try after the whole pass, and that try decides: a timeout or an error that
# may pass is only a warning, anything else (a host that does not resolve, a bad certificate, a redirect to HTTP,
# another HTTP error) fails. A file left with fewer than two good URLs fails the check too.

STATIC_SOURCES_ORIGINS=" archive.apache.org zlib.net "
SRC_NAMES=() SRC_SHAS=() SRC_SIZES=() SRC_VARS=() SRC_URLS=()

_src_sha256() { cmake -E sha256sum "$1" | awk '{ print $1 }'; }
_src_size() { wc -c <"$1" | tr -d ' '; }
_src_host() {
  local h=${1#https://}
  echo "${h%%/*}"
}

# static_sources_parse <lock>: fills SRC_NAMES, SRC_SHAS, SRC_SIZES, SRC_VARS and SRC_URLS (each pin's URLs, space
# separated). Exits 2 on a malformed or repeated line, or when no line pins Arrow's own source (variable `-`).
static_sources_parse() {
  local lock=$1 name sha bytes var urls url ok
  local -a list
  SRC_NAMES=() SRC_SHAS=() SRC_SIZES=() SRC_VARS=() SRC_URLS=()
  while read -r name sha bytes var urls || [ -n "${name:-}" ]; do
    case "$name" in "" | "#"*) continue ;; esac
    read -r -a list <<<"${urls:-}"
    ok=1
    [[ "$sha" =~ ^[0-9a-f]{64}$ && "$bytes" =~ ^[0-9]+$ && "$name" != */* && ${#list[@]} -gt 0 ]] || ok=0
    [[ "${var:-}" == - || "${var:-}" =~ ^ARROW_[A-Z0-9_]+_URL$ ]] || ok=0
    for url in "${list[@]}"; do
      [[ "$url" == https://?* ]] || ok=0
    done
    case " ${SRC_NAMES[*]} " in *" $name "*) ok=0 ;; esac
    if [ "$ok" = 0 ]; then
      echo "static-sources: $lock: malformed or repeated line for '$name'" \
        "(expected: <name> <sha256> <bytes> <variable> <https url> [<https url>...])" >&2
      exit 2
    fi
    SRC_NAMES+=("$name") SRC_SHAS+=("$sha") SRC_SIZES+=("$bytes") SRC_VARS+=("$var") SRC_URLS+=("${list[*]}")
  done <"$lock"
  case " ${SRC_VARS[*]} " in
    *" - "*) ;;
    *) echo "static-sources: $lock pins no Arrow source (variable '-')" >&2; exit 2 ;;
  esac
}

# static_sources_pins: the pins, sorted, one `pin <name> <sha256> <bytes> <variable>` line each (the prefix key's).
static_sources_pins() {
  local i
  for i in "${!SRC_NAMES[@]}"; do
    echo "pin ${SRC_NAMES[i]} ${SRC_SHAS[i]} ${SRC_SIZES[i]} ${SRC_VARS[i]}"
  done | LC_ALL=C sort
}

# _src_why <curl exit status> <HTTP status>: what the failure means here (curl's own message is above it).
_src_why() {
  case "$1" in
    22) case "$2" in
          404 | 410) echo "dead (HTTP $2: the URL is gone)" ;;
          403) echo "refused (HTTP 403)" ;;
          429) echo "rate-limited (HTTP 429)" ;;
          5??) echo "server error (HTTP $2, usually passing)" ;;
          *) echo "HTTP error $2" ;;
        esac ;;
    28) echo "timeout (no connection in 10 s, under 1 KiB/s for 30 s, or over the attempt's time limit)" ;;
    6) echo "the host name does not resolve" ;;
    7) echo "the connection was refused" ;;
    1) echo "a redirect to a URL that is not HTTPS" ;;
    16 | 18 | 52 | 55 | 56 | 92) echo "the connection broke during the transfer" ;;
    35 | 60) echo "TLS failed (handshake or certificate)" ;;
    23) echo "the local file could not be written" ;;
    63) echo "larger than the pinned size" ;;
    *) echo "curl exit $1" ;;
  esac
}

# _src_may_pass <curl exit status> <HTTP status>: whether another attempt of the same URL may help.
_src_may_pass() {
  case "$1" in
    22) case "$2" in 403 | 408 | 425 | 429 | 5??) return 0 ;; esac ;;
    7 | 16 | 18 | 35 | 52 | 55 | 56 | 92) return 0 ;;
  esac
  return 1
}

# _src_classify <curl exit status> <HTTP status>: sets SRC_RESULT and SRC_WHY for a failed attempt: dead (HTTP 404 or
# 410), wrong (larger than the pinned size), timeout, passing (an error that may pass) or failed (any other error).
_src_classify() {
  SRC_WHY="curl exit $1, $(_src_why "$1" "$2")"
  if [ "$1" = 22 ] && { [ "$2" = 404 ] || [ "$2" = 410 ]; }; then
    SRC_RESULT=dead
  elif [ "$1" = 63 ]; then
    SRC_RESULT=wrong
  elif [ "$1" = 28 ]; then
    SRC_RESULT=timeout
  elif _src_may_pass "$1" "$2"; then
    SRC_RESULT=passing
  else
    SRC_RESULT=failed
  fi
}

# _src_get <url> <file> <bytes> <sha256>: tries the URL (a second time after an error that may pass). Sets SRC_RESULT
# to ok, wrong (other bytes, or no file) or a result of _src_classify, and SRC_WHY to its reason; returns 0 when
# <file> then holds the pinned bytes, and leaves no file otherwise.
_src_get() {
  local url=$1 file=$2 bytes=$3 sha=$4 attempt rc code got_bytes got_sha
  for attempt in 1 2; do
    rm -f "$file"
    rc=0
    code=$(curl -fsSL --proto '=https' --proto-redir '=https' --connect-timeout 10 --speed-limit 1024 \
      --speed-time 30 --max-time $((30 + bytes / 65536)) --max-filesize "$bytes" -w '%{http_code}' \
      -o "$file" "$url") || rc=$?
    if [ "$rc" = 0 ] && [ ! -f "$file" ]; then
      SRC_RESULT=wrong SRC_WHY="curl reported success but wrote no file"
      return 1
    fi
    if [ "$rc" = 0 ]; then
      got_bytes=$(_src_size "$file")
      got_sha=$(_src_sha256 "$file")
      if [ "$got_bytes" = "$bytes" ] && [ "$got_sha" = "$sha" ]; then
        SRC_RESULT=ok SRC_WHY=""
        return 0
      fi
      rm -f "$file"
      SRC_RESULT=wrong SRC_WHY="$got_bytes bytes, sha256 $got_sha; the pin says $bytes bytes, sha256 $sha"
      return 1
    fi
    rm -f "$file"
    _src_classify "$rc" "${code:-000}"
    [ "$SRC_RESULT" = passing ] || return 1
    [ "$attempt" = 1 ] && sleep "${ANTB1_SOURCES_RETRY_DELAY:-3}"
  done
  return 1
}

# _src_head <url> <bytes>: a HEAD request; SRC_RESULT and SRC_WHY as _src_get (ok when the status is 200 and the
# size is the pinned one).
_src_head() {
  local url=$1 bytes=$2 headers rc=0 code length
  headers=$(mktemp)
  code=$(curl -fsSIL --proto '=https' --proto-redir '=https' --connect-timeout 10 --max-time 30 -D "$headers" \
    -o /dev/null -w '%{http_code}' "$url") || rc=$?
  length=$(tr -d '\r' <"$headers" | awk 'tolower($1) == "content-length:" { n = $2 } END { print n }')
  rm -f "$headers"
  if [ "$rc" != 0 ]; then
    _src_classify "$rc" "${code:-000}"
    return 1
  fi
  if [ "$code" != 200 ] || [ "$length" != "$bytes" ]; then
    SRC_RESULT=wrong SRC_WHY="HEAD: HTTP $code, ${length:-no} Content-Length; the pin says $bytes bytes"
    return 1
  fi
  SRC_RESULT=ok SRC_WHY="HEAD only: a single-server origin"
  return 0
}

# static_sources_fetch <dir>: every pinned file into <dir> (kept when it already matches its pin); exits 1 when no
# URL of a file serves it.
static_sources_fetch() {
  local dir=$1 i name dest n url
  local -a list
  mkdir -p "$dir"
  for i in "${!SRC_NAMES[@]}"; do
    name=${SRC_NAMES[i]}
    dest="$dir/$name"
    if [ -f "$dest" ] && [ "$(_src_size "$dest")" = "${SRC_SIZES[i]}" ] &&
      [ "$(_src_sha256 "$dest")" = "${SRC_SHAS[i]}" ]; then
      echo "static-sources: $name: ok (cached, size and sha256 match the pin)"
      continue
    fi
    rm -f "$dest" "$dest.part"
    read -r -a list <<<"${SRC_URLS[i]}"
    n=0
    for url in "${list[@]}"; do
      n=$((n + 1))
      echo "static-sources: $name: downloading $url (URL $n of ${#list[@]})"
      if _src_get "$url" "$dest.part" "${SRC_SIZES[i]}" "${SRC_SHAS[i]}"; then
        mv -f "$dest.part" "$dest"
        echo "static-sources: $name: downloaded from $url (${SRC_SIZES[i]} bytes, sha256 ${SRC_SHAS[i]})"
        break
      fi
      echo "static-sources: $name: $url: $SRC_RESULT: $SRC_WHY" >&2
    done
    if [ ! -f "$dest" ]; then
      echo "static-sources: $name: none of its ${#list[@]} URLs served the pinned file (reasons above);" \
        "\`pixi run static-arrow --check-urls\` checks every URL" >&2
      exit 1
    fi
  done
}

# _src_check_one <pin index> <url> <scratch file>: checks one URL (HEAD only for a single-server origin).
_src_check_one() {
  local i=$1 url=$2 part=$3 rc=0
  case "$STATIC_SOURCES_ORIGINS" in
    *" $(_src_host "$url") "*) _src_head "$url" "${SRC_SIZES[i]}" || rc=$? ;;
    *) _src_get "$url" "$part" "${SRC_SIZES[i]}" "${SRC_SHAS[i]}" || rc=$? ;;
  esac
  rm -f "$part"
  return "$rc"
}

# static_sources_check <scratch dir>: every URL against its pin; returns 1 when the check fails. A dead URL or other
# bytes count at once; any other result is tried again after the pass, and only a timeout or an error that may pass
# on that try is a warning.
static_sources_check() {
  local scratch=$1 i url total=0 bad=0 thin=0 good
  local -a list retry=()
  for i in "${!SRC_NAMES[@]}"; do
    read -r -a list <<<"${SRC_URLS[i]}"
    good=0
    for url in "${list[@]}"; do
      total=$((total + 1))
      _src_check_one "$i" "$url" "$scratch/part" || true
      case "$SRC_RESULT" in
        ok) good=$((good + 1)) ;;
        dead | wrong) bad=$((bad + 1)) ;;
        *) retry+=("$i $url") ;;
      esac
      echo "static-sources: ${SRC_NAMES[i]}: $url: $SRC_RESULT${SRC_WHY:+ ($SRC_WHY)}"
    done
    SRC_GOOD[i]=$good
  done
  local entry warned=0
  for entry in "${retry[@]}"; do
    i=${entry%% *} url=${entry#* }
    _src_check_one "$i" "$url" "$scratch/part" || true
    case "$SRC_RESULT" in
      ok)
        SRC_GOOD[i]=$((SRC_GOOD[i] + 1))
        echo "static-sources: ${SRC_NAMES[i]}: $url: ok on the second pass"
        ;;
      timeout | passing)
        warned=$((warned + 1))
        echo "static-sources: warning: ${SRC_NAMES[i]}: $url: $SRC_RESULT on the second pass${SRC_WHY:+ ($SRC_WHY)}" >&2
        [ -n "${GITHUB_ACTIONS:-}" ] && echo "::warning::${SRC_NAMES[i]}: $url: $SRC_RESULT on the second pass"
        ;;
      *)
        bad=$((bad + 1))
        echo "static-sources: ${SRC_NAMES[i]}: $url: $SRC_RESULT on the second pass${SRC_WHY:+ ($SRC_WHY)}" >&2
        ;;
    esac
  done
  for i in "${!SRC_NAMES[@]}"; do
    if [ "${SRC_GOOD[i]}" -lt 2 ]; then
      thin=$((thin + 1))
      echo "static-sources: ${SRC_NAMES[i]}: only ${SRC_GOOD[i]} good URL(s); every file needs two" >&2
    fi
  done
  if [ "$bad" != 0 ] || [ "$thin" != 0 ]; then
    echo "static-sources: FAIL: $bad of $total URLs are dead, serve other bytes or fail, $thin file(s) have fewer" \
      "than two good URLs; fix the lines in cmake/static/arrow-sources.lock" >&2
    return 1
  fi
  local note=""
  [ "$warned" = 0 ] || note="; $warned warned: a timeout or an error that may pass on the second pass"
  echo "static-sources: OK: $((total - warned)) of $total URLs serve their pinned file$note"
}

SRC_GOOD=()

set -euo pipefail
if [ $# -lt 2 ]; then
  echo "usage: static-sources.sh <lock> --pins | --fetch <dir> | --variables <dir> | --check-urls" >&2
  exit 2
fi
static_sources_parse "$1"
case "$2" in
  --pins) static_sources_pins ;;
  --fetch) static_sources_fetch "${3:?--fetch needs a directory}" ;;
  --variables)
    for i in "${!SRC_NAMES[@]}"; do
      echo "${SRC_VARS[i]}=${3:?--variables needs a directory}/${SRC_NAMES[i]}"
    done
    ;;
  --check-urls)
    scratch=$(mktemp -d)
    trap 'rm -rf "$scratch"' EXIT
    static_sources_check "$scratch"
    ;;
  *) echo "static-sources: unknown option '$2'" >&2; exit 2 ;;
esac
