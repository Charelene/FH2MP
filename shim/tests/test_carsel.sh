#!/usr/bin/env bash
# Car select hold and group deadline (src/carsel_wire.h, src/carsel_sync.inc): the wire format against server/carsel.py, the hold decision and the deadline
# rules with a faked game, without the game. Runs the test executable directly on Windows, under Wine elsewhere.
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
$CC -O2 -Wall -Wextra -Wno-unused-function -Wno-cast-function-type -I . tests/test_carsel.c -o "$work/test.exe" -lws2_32
$RUN "$work/test.exe"
$RUN "$work/test.exe" --vector > "$work/vector"
# the request the C code builds is the one the server accepts, and the server's four answers (hold / release, with and without a deadline) are what
# the C code accepts: frozen server vectors (tests/vectors/make_vectors.py, server/carsel.py 2026-11-07; the Go server keeps the format in native/internal/game/carsel.go)
[ "$(tr -d '\r\n' < "$work/vector")" = "$(tr -d '\r\n' < tests/vectors/carsel_request.hex)" ] || { echo 'C request differs from the server wire format (tests/vectors/carsel_request.hex)'; exit 1; }
for answer in "1 5000" "0 none" "1 none" "0 0"; do
set -- $answer
tag="$1-$2"
tr -d '\r' < "tests/vectors/carsel_reply_$tag.hex" > "$work/reply$tag"
$RUN "$work/test.exe" --reply "$work/reply$tag" "$1" "$2"
done
