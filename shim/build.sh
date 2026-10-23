#!/usr/bin/env bash
# Builds shim/build/FH2MP.dll (mingw-w64, KERNEL32 only): the FH2MP mod for the FH2 Mod Platform, installed as fh2-mods\FH2MP.dll.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
VERSION=${FH2MP_SHIM_VERSION:-$(tr -d '[:space:]' < VERSION)}
PLATFORM=$(tr -d '[:space:]' < ../third_party/fh2-mod-platform/VERSION)
IFS=. read -r V1 V2 V3 <<< "$(sed -E 's/^([0-9]+)\.([0-9]+)\.([0-9]+).*/\1.\2.\3/' <<< "$VERSION")"
COMMON=(-I ../third_party/fh2-mod-platform -DFH2MOD_PLATFORM_VERSION="\"$PLATFORM\"" -O2 -Wall -Wextra -shared -s
        -nostartfiles -Wl,--entry=DllMain -static-libgcc -lkernel32)
# Release build (default, what ships): no debugger server, no tracer, no probe files, no verbose request logging.
# `shim/build.sh debug` builds the research variant build/FH2MP-debug.dll (live debugger on a localhost port, tracer, probes);
# its version carries "-debug" so the launcher replaces it with the release shim. Never ship that file.
if [ "${1:-}" = debug ]; then
  VERSION="$VERSION-debug"; OUT=build/FH2MP-debug.dll; FLAGS=(-DFH2MP_DEBUG_BUILD)
else
  OUT=build/FH2MP.dll; FLAGS=(-Wno-unused-function)
fi
"$WINDRES" -DFH2MP_V1="$V1" -DFH2MP_V2="$V2" -DFH2MP_V3="$V3" -DFH2MP_VERSION_STR="\\\"$VERSION\\\"" -O coff -o build/version.o src/version.rc
$CC "${FLAGS[@]}" -DFH2MP_SHIM_VERSION="\"$VERSION\"" -o "$OUT" src/fh2mp_mod.c build/version.o "${COMMON[@]}"
rm -f build/version.o
echo "built: $OUT ($VERSION, FH2 Mod Platform $PLATFORM)"
