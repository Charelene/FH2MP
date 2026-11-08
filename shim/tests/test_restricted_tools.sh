#!/usr/bin/env bash
# The tools header names no tool, whatever mods are registered (NitroSystem stopped being reported 2026-11-07), isolated from the user's game/Wine prefix.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
export WINEPREFIX="$tmp/prefix" WINEDEBUG=-all
. tests/wine_prefix.sh; wine_prefix "$WINEPREFIX"
cat > "$tmp/test.c" <<'C'
#include <windows.h>
#include <stdio.h>
#include "restricted_tools.inc"
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); return 1; } } while (0)
int main(void) {
    WCHAR line[64];
    CHECK(LoadLibraryW(L"kernel32.dll") != NULL);
    restricted_tools_header(line);
    CHECK(lstrcmpW(line, L"X-FH2MP-Tools: v1") == 0);
    puts("restricted tools: the header reports no tool");
    return 0;
}
C
${CC:-x86_64-w64-mingw32-gcc} -O2 -Wall -Wextra -Wno-unused-function -I src "$tmp/test.c" -o "$tmp/test.exe"
wine "$tmp/test.exe"
wineserver -w
