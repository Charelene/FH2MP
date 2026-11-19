#!/usr/bin/env bash
# Mod state side channel (src/mods_wire.h, src/mods_sync.inc): the wire format against the server's frozen vectors, Underglow's status text, the
# effects, NitroSystem's online rule with a fake NitroSystem and Underglow's status through a fake platform command pipe, all without the game.
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'WINEPREFIX="$work/prefix" wineserver -k >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
. tests/wine_prefix.sh; wine_prefix "$work/prefix"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-function -Wno-cast-function-type -I . -I ../third_party/fh2-mod-platform tests/test_mods.c -o "$work/test.exe" -lws2_32
mkdir -p "$work/prefix"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe" --vector > "$work/vector"
# the request the C code builds is the one the server accepts, and the server's reply is what the C code accepts: vectors written by the Go
# tests (native/internal/game/modsync_test.go, FH2MP_WRITE_VECTORS=1)
[ "$(tr -d '\r\n' < "$work/vector")" = "$(tr -d '\r\n' < tests/vectors/mods_request.hex)" ] || { echo 'C request differs from the server wire format (tests/vectors/mods_request.hex)'; exit 1; }
tr -d '\r' < tests/vectors/mods_reply.hex > "$work/reply"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe" --reply "$work/reply"
