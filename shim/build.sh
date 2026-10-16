#!/usr/bin/env bash
# Builds shim/build/azimuth-tls.dll (mingw-w64, KERNEL32 only) and the contract test harness.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p build
CC=${CC:-x86_64-w64-mingw32-gcc}
$CC -O2 -Wall -Wextra -shared -s -o build/azimuth-tls.dll src/azimuth_tls_proxy.c azimuth-tls.def \
    -nostartfiles -Wl,--entry=DllMain -static-libgcc -lkernel32
$CC -O2 -Wall -municode -o build/tls_contract_test.exe test/tls_contract_test.c
echo "built: $(ls -1 build | tr '\n' ' ')"
