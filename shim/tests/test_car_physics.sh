#!/usr/bin/env bash
# The physics gate (src/car_physics_guard.inc): every caller of the game's car physics builder gets a car the database can build. Offline; never
# launches the game or sends traffic.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
cleanup() { WINEPREFIX="$tmp/prefix" wineserver -w || true; rm -rf "$tmp"; }
trap cleanup EXIT
. tests/wine_prefix.sh; wine_prefix "$tmp/prefix"
${CC:-x86_64-w64-mingw32-gcc} -O2 -Wall -Wextra -Wno-unused-function -I . tests/test_car_physics.c -o "$tmp/test.exe"
WINEPREFIX="$tmp/prefix" WINEDEBUG=-all wine "$tmp/test.exe"
