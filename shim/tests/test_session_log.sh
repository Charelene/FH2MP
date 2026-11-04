#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
cleanup() {
  WINEPREFIX="$tmp/prefix" wineserver -k >/dev/null 2>&1 || true
  rm -rf "$tmp"
}
trap cleanup EXIT
. tests/wine_prefix.sh; wine_prefix "$tmp/prefix"
mkdir -p "$tmp/prefix"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-cast-function-type -municode -I ../third_party/fh2-mod-platform -I . tests/test_session_log.c -o "$tmp/test.exe"
x86_64-w64-mingw32-gcc -O2 -shared -nostartfiles -Wl,--entry=DllMain -static-libgcc \
  -DFH2MP_SHIM_VERSION='"session-log-test"' -DFH2MOD_PLATFORM_VERSION='"2.8.0"' \
  -I ../third_party/fh2-mod-platform src/fh2mp_mod.c -o "$tmp/FH2MP.dll" -lkernel32
WINEPREFIX="$tmp/prefix" WINEDEBUG=-all wine "$tmp/test.exe" "Z:${tmp//\//\\}\\FH2MP.dll"
