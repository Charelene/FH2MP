#!/usr/bin/env bash
# Isolated Windows receive-boundary checks. Does not send any network traffic.
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d)
cleanup() { WINEPREFIX="$tmp/prefix" wineserver -w || true; rm -rf "$tmp"; }
trap cleanup EXIT
. ./wine_prefix.sh; wine_prefix "$tmp/prefix"
${CC:-x86_64-w64-mingw32-gcc} -O2 -Wall -Wextra -I . test_net_strings.c -o "$tmp/test.exe"
WINEPREFIX="$tmp/prefix" WINEDEBUG=-all wine "$tmp/test.exe"
WINEPREFIX="$tmp/prefix" wineserver -w
