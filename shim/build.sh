#!/usr/bin/env bash
# Builds shim/build/winmm.dll (mingw-w64, KERNEL32 only): the Azimuth 0.9.10+ winmm proxy.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
CC=${CC:-x86_64-w64-mingw32-gcc}
python3 -I gen_winmm.py
$CC -O2 -Wall -Wextra -shared -s -o build/winmm.dll src/winmm_proxy.c winmm.def \
    -nostartfiles -Wl,--entry=DllMain -static-libgcc -lkernel32
echo "built: $(ls -1 build | tr '\n' ' ')"
