#!/usr/bin/env bash
# Stall tracer (src/stall_trace.inc) against a fake game image, see tests/test_stall.c. Built plain and with the car select hold (-DTEST_CARSEL) so both
# shapes of the snapshot are checked. Runs the executables directly on Windows, under Wine elsewhere.
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
CC=${CC:-x86_64-w64-mingw32-gcc}
case "$(uname -s)" in
  MINGW*|MSYS*) RUN=""; trap 'rm -rf "$work"' EXIT ;;
  *) . tests/wine_prefix.sh; wine_prefix "$work/prefix"; mkdir -p "$work/prefix"
     trap 'WINEPREFIX="$work/prefix" wineserver -w || true; rm -rf "$work"' EXIT
     RUN="env WINEPREFIX=$work/prefix WINEDEBUG=-all wine" ;;
esac
for variant in plain carsel; do
  flags=(); [ "$variant" = carsel ] && flags=(-DTEST_CARSEL)
  $CC -O2 -Wall -Wextra -Wno-unused-function "${flags[@]}" -I . tests/test_stall.c -o "$work/test_$variant.exe"
  $RUN "$work/test_$variant.exe"
done
