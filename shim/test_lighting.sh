#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
work=$(mktemp -d)
trap 'WINEPREFIX="$work/prefix" wineserver -k >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-function test_lighting.c -o "$work/test.exe"
mkdir -p "$work/prefix"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe" --vector > "$work/vector"
python3 -I - "$work/vector" <<'PY'
import sys, pathlib,struct
sys.path.insert(0,str(pathlib.Path.cwd().parent/'server'))
import lighting
payload=bytearray(lighting.HEADER)+bytes([1])+struct.pack('<QQ',0x9000123456789,7)+bytes(range(16,32))+bytes([5,7,0])
struct.pack_into('<H',payload,9,lighting.REQUEST_SIZE)
assert pathlib.Path(sys.argv[1]).read_text().strip()==lighting.signed(payload,bytes(range(32))).hex()
print('C/Python authenticated wire format: PASS')
PY
