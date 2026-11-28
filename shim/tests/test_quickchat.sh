#!/usr/bin/env bash
# Quick chat side channel (src/quickchat_wire.h, src/quickchat_sync.inc): the wire format against frozen server vectors, every refusal of a
# reply, and finding QuickChat's bridge through the platform, all without the game.
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'WINEPREFIX="$work/prefix" wineserver -k >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
. tests/wine_prefix.sh; wine_prefix "$work/prefix"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-function -Wno-cast-function-type -I . -I ../paddock/third_party/fh2-mod-platform tests/test_quickchat.c \
    -o "$work/test.exe" -lws2_32
mkdir -p "$work/prefix"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe" --vector > "$work/vector"
# the request the C code builds is the one the server accepts, and the server's reply (one other player's message, with its gamertag) is what
# the C code accepts: frozen server vectors, written and checked by the Go server's own test (native/internal/game/quickchat_test.go,
# TestQuickchatVectors; FH2MP_WRITE_VECTORS=1 writes them)
[ "$(tr -d '\r\n' < "$work/vector")" = "$(tr -d '\r\n' < tests/vectors/quickchat_request.hex)" ] || { echo 'C request differs from the server wire format (tests/vectors/quickchat_request.hex)'; exit 1; }
tr -d '\r' < tests/vectors/quickchat_reply.hex > "$work/reply"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe" --reply "$work/reply"
