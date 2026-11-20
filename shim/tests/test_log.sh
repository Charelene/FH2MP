#!/usr/bin/env bash
# The shim's log (src/log.inc): the line lock is released even when fh2mp.log cannot be opened, lines reach the file otherwise. Runs the test
# executable directly on Windows, under Wine elsewhere (see tests/test_log.c).
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
CC=${CC:-x86_64-w64-mingw32-gcc}
case "$(uname -s)" in
  MINGW*|MSYS*) RUN=""; trap 'rm -rf "$work"' EXIT ;;
  *) . tests/wine_prefix.sh; wine_prefix "$work/prefix"; mkdir -p "$work/prefix"
     trap 'WINEPREFIX="$work/prefix" wineserver -k >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
     RUN="env WINEPREFIX=$work/prefix WINEDEBUG=-all wine" ;;
esac
$CC -O2 -Wall -Wextra -Wno-unused-function -I . tests/test_log.c -o "$work/test.exe"
$RUN "$work/test.exe"
