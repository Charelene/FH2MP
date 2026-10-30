#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d)
cleanup() {
  WINEPREFIX="$tmp/prefix" wineserver -k >/dev/null 2>&1 || true
  rm -rf "$tmp"
}
trap cleanup EXIT
. ./wine_prefix.sh; wine_prefix "$tmp/prefix"
mkdir -p "$tmp/prefix"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra test_xhr_config.c -o "$tmp/test.exe"
WINEPREFIX="$tmp/prefix" WINEDEBUG=-all wine "$tmp/test.exe"
