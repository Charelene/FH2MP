#!/usr/bin/env bash
# The watcher's heap census (src/health.inc) on a real heap: natively on Windows, under Wine elsewhere (where there is no NT heap to find).
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
cleanup() {
  [ -d "$tmp/prefix" ] && { WINEPREFIX="$tmp/prefix" wineserver -k >/dev/null 2>&1 || true; }
  rm -rf "$tmp"
}
trap cleanup EXIT
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    ${CC:-gcc} -O2 -Wall -Wextra -Wno-unused-function -o "$tmp/test.exe" -I . tests/test_heap_census.c
    "$tmp/test.exe";;
  *)
    . tests/wine_prefix.sh; wine_prefix "$tmp/prefix"
    mkdir -p "$tmp/prefix"
    x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-function -o "$tmp/test.exe" -I . tests/test_heap_census.c
    WINEPREFIX="$tmp/prefix" WINEDEBUG=-all wine "$tmp/test.exe";;
esac
