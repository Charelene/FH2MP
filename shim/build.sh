#!/usr/bin/env bash
# Builds shim/build/winmm.dll (mingw-w64, KERNEL32 only): the Azimuth 0.9.10+ winmm proxy.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
CC=${CC:-x86_64-w64-mingw32-gcc}
python3 -I gen_winmm.py
VERSION=${FH2MP_SHIM_VERSION:-$(tr -d '[:space:]' < VERSION)}
# Release build (default, what ships): no debugger server, no tracer, no probe files, no verbose request logging.
# `shim/build.sh debug` builds the research variant build/winmm-debug.dll (live debugger on a localhost port, tracer, probes);
# its version carries "-debug" so the launcher replaces it with the release shim. Never ship that file.
if [ "${1:-}" = debug ]; then
  $CC -DFH2MP_DEBUG_BUILD -DFH2MP_SHIM_VERSION="\"$VERSION-debug\"" -O2 -Wall -Wextra -shared -s -o build/winmm-debug.dll src/winmm_proxy.c winmm.def \
      -nostartfiles -Wl,--entry=DllMain -static-libgcc -lkernel32
else
  $CC -DFH2MP_SHIM_VERSION="\"$VERSION\"" -O2 -Wall -Wextra -Wno-unused-function -shared -s -o build/winmm.dll src/winmm_proxy.c winmm.def \
      -nostartfiles -Wl,--entry=DllMain -static-libgcc -lkernel32
fi
echo "built: $(ls -1 build | tr '\n' ' ')"
