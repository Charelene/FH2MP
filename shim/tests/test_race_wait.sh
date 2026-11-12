#!/usr/bin/env bash
# Race load wait (src/race_wait.inc): the report of who the game still waits for and the opt-in way through, against a faked game in this process.
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
$CC -O2 -Wall -Wextra -Wno-unused-function -Wno-cast-function-type -I . tests/test_race_wait.c -o "$work/test.exe"
$RUN "$work/test.exe"
