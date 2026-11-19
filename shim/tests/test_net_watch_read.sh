#!/usr/bin/env bash
# The net watch (src/net_watch.inc) with a bit stream and a connection laid out as FH2 1.0.0.64 lays them out: natively on Windows, under Wine elsewhere.
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
    ${CC:-gcc} -O2 -Wall -Wextra -Wno-unused-function -o "$tmp/test.exe" -I . -I ../third_party/fh2-mod-platform tests/test_net_watch_read.c
    "$tmp/test.exe";;
  *)
    . tests/wine_prefix.sh; wine_prefix "$tmp/prefix"
    mkdir -p "$tmp/prefix"
    x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-function -o "$tmp/test.exe" -I . -I ../third_party/fh2-mod-platform tests/test_net_watch_read.c
    WINEPREFIX="$tmp/prefix" WINEDEBUG=-all wine "$tmp/test.exe";;
esac
