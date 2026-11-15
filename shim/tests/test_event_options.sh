#!/usr/bin/env bash
# The Modded cars event option (src/event_options.inc, src/eventopts_wire.h, src/eventopts_sync.inc): the row with stand-ins for the game's
# functions, the datagrams against the server's frozen vectors, and what is said and followed when. Without the game or the network.
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
$CC -O2 -Wall -Wextra -Wno-unused-function -I . tests/test_event_options.c -o "$work/test.exe"
for v in request reply_0 reply_1 reply_2; do tr -d '\r' < "tests/vectors/eventopts_$v.hex" > "$work/$v"; done
$RUN "$work/test.exe" "$work/request" "$work/reply_0" "$work/reply_1" "$work/reply_2"
