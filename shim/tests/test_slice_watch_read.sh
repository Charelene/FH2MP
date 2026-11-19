#!/usr/bin/env bash
# The slice watch (src/slice_watch.inc) reading buffers laid out as azimuth.exe 1.0.4 lays them out: natively on Windows, under Wine elsewhere.
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
    ${CC:-gcc} -O2 -Wall -Wextra -Wno-unused-function -o "$tmp/test.exe" -I . -I ../third_party/fh2-mod-platform tests/test_slice_watch_read.c
    "$tmp/test.exe";;
  *)
    . tests/wine_prefix.sh; wine_prefix "$tmp/prefix"
    mkdir -p "$tmp/prefix"
    x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-function -o "$tmp/test.exe" -I . -I ../third_party/fh2-mod-platform tests/test_slice_watch_read.c
    WINEPREFIX="$tmp/prefix" WINEDEBUG=-all wine "$tmp/test.exe";;
esac
