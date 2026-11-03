#!/usr/bin/env bash
# Remote car slots (src/car_pool.inc) against stand-ins for the game's functions, see test_car_pool.c.
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d)
cleanup() { WINEPREFIX="$tmp/prefix" wineserver -w || true; rm -rf "$tmp"; }
trap cleanup EXIT
. ./wine_prefix.sh; wine_prefix "$tmp/prefix"
${CC:-x86_64-w64-mingw32-gcc} -O2 -Wall -Wextra -Wno-unused-function -I . test_car_pool.c -o "$tmp/test.exe"
WINEPREFIX="$tmp/prefix" WINEDEBUG=-all wine "$tmp/test.exe"
WINEPREFIX="$tmp/prefix" wineserver -w
