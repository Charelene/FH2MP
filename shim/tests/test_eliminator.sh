#!/usr/bin/env bash
# Eliminator side channel (src/eliminator_wire.h): the wire format against frozen server vectors, without the game. Runs the test executable directly
# on Windows, under Wine elsewhere.
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
$CC -O2 -Wall -Wextra -Wno-unused-function -I . tests/test_eliminator.c -o "$work/test.exe"
$RUN "$work/test.exe" --vector > "$work/vector"
# the request the C code builds is the one the server accepts, and the server's reply is what the C code accepts: both files are also read by the
# server's test (native/internal/game/eliminator_test.go, TestEliminatorWireVectors), which fails when the Go side stops building them
[ "$(tr -d '\r\n' < "$work/vector")" = "$(tr -d '\r\n' < tests/vectors/eliminator_request.hex)" ] || { echo 'C request differs from the server wire format (tests/vectors/eliminator_request.hex)'; exit 1; }
tr -d '\r' < tests/vectors/eliminator_reply.hex > "$work/reply"
$RUN "$work/test.exe" --reply "$work/reply"
