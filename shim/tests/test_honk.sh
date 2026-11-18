#!/usr/bin/env bash
# Horn side channel (src/honk_wire.h, src/honk_sync.inc): the wire format against frozen server vectors, the bank reader, the remote voices with a
# recording FMOD fake and the left->right stick change of HonkHonk's button, all without the game.
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'WINEPREFIX="$work/prefix" wineserver -k >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
. tests/wine_prefix.sh; wine_prefix "$work/prefix"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-function -Wno-cast-function-type -I . tests/test_honk.c -o "$work/test.exe" -lws2_32
mkdir -p "$work/prefix"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe" --vector > "$work/vector"
# the request the C code builds is the one the server accepts, and the server's reply (one other car honking) is what the C code accepts:
# frozen server vectors (tests/vectors/make_vectors.py, server/honk.py 2026-11-07; the Go server keeps the format in native/internal/game/side.go)
[ "$(tr -d '\r\n' < "$work/vector")" = "$(tr -d '\r\n' < tests/vectors/honk_request.hex)" ] || { echo 'C request differs from the server wire format (tests/vectors/honk_request.hex)'; exit 1; }
tr -d '\r' < tests/vectors/honk_reply.hex > "$work/reply"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe" --reply "$work/reply"
# horn v2 (HonkHonk 1.0.0 numbering): vectors written by the Go server's test (native/internal/game/honk_test.go TestHonkV2Vectors)
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe" --vector2 > "$work/vector2"
[ "$(tr -d '\r\n' < "$work/vector2")" = "$(tr -d '\r\n' < tests/vectors/honk2_request.hex)" ] || { echo 'C v2 request differs from the server wire format (tests/vectors/honk2_request.hex)'; exit 1; }
tr -d '\r' < tests/vectors/honk2_reply.hex > "$work/reply2"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe" --reply2 "$work/reply2"
