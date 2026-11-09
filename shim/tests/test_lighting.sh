#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'WINEPREFIX="$work/prefix" wineserver -k >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
. tests/wine_prefix.sh; wine_prefix "$work/prefix"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-function -I . tests/test_lighting.c -o "$work/test.exe"
mkdir -p "$work/prefix"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe" --vector > "$work/vector"
# the request the shim builds is the one the server accepts: the frozen server vector (tests/vectors/make_vectors.py, server/lighting.py 2026-11-07;
# the Go server keeps the format in native/internal/game/side.go)
[ "$(tr -d '
' < "$work/vector")" = "$(tr -d '
' < tests/vectors/lighting_request.hex)" ] || { echo 'the request differs from the server wire format (tests/vectors/lighting_request.hex)'; exit 1; }
echo 'C/server authenticated wire format: PASS'
