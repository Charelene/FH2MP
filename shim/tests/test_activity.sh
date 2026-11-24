#!/usr/bin/env bash
# The encore-activity.json text of src/activity_report.inc, see tests/test_activity.c.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
cleanup() { WINEPREFIX="$tmp/prefix" wineserver -w || true; rm -rf "$tmp"; }
trap cleanup EXIT
. tests/wine_prefix.sh; wine_prefix "$tmp/prefix"
${CC:-x86_64-w64-mingw32-gcc} -O2 -Wall -Wextra -Wno-unused-function -I . tests/test_activity.c -o "$tmp/test.exe"
WINEPREFIX="$tmp/prefix" WINEDEBUG=-all wine "$tmp/test.exe"
WINEPREFIX="$tmp/prefix" wineserver -w
