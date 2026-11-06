#!/usr/bin/env bash
# Free Roam car picker PI (src/picker_pi.inc): the decision on unknown part ids / unusable PI and the hook around QueryCarAttributes with a faked game and
# database, without the game. Runs the test executable directly on Windows, under Wine elsewhere.
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
$CC -O2 -Wall -Wextra -Wno-unused-function -I . tests/test_picker_pi.c -o "$work/test.exe" -lm
$RUN "$work/test.exe"
