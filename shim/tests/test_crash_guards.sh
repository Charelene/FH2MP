#!/usr/bin/env bash
# The speedometer hook guard and the GameplayLog guard (src/hook_guard.inc, src/gameplaylog_guard.inc) against stand-ins for the game's functions, see test_crash_guards.c.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
cleanup() { WINEPREFIX="$tmp/prefix" wineserver -w || true; rm -rf "$tmp"; }
trap cleanup EXIT
. tests/wine_prefix.sh; wine_prefix "$tmp/prefix"
${CC:-x86_64-w64-mingw32-gcc} -O2 -Wall -Wextra -Wno-unused-function -I . tests/test_crash_guards.c -o "$tmp/test.exe"
WINEPREFIX="$tmp/prefix" WINEDEBUG=-all wine "$tmp/test.exe"
WINEPREFIX="$tmp/prefix" wineserver -w
