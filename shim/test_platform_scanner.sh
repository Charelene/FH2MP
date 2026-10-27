#!/usr/bin/env bash
# Isolated diagnostic against the pinned original Mod Platform 2.7.2 DLL.
# No game process, FH2MP, gameplay input or network traffic.
set -euo pipefail
cd "$(dirname "$0")/.."
platform=$(realpath "${1:?usage: shim/test_platform_scanner.sh /path/to/d3dcompiler_47.dll}")
expected=65b59f88fcc96cb65c8b1b7c185e9490d1a6580e39d5947f7f1f436328e9ec67
[[ $(sha256sum "$platform" | cut -d' ' -f1) == "$expected" ]] || {
    echo 'Refusing an unexpected platform build' >&2; exit 2;
}
tmp=$(mktemp -d)
cleanup() { WINEPREFIX="$tmp/prefix" wineserver -w || true; rm -rf "$tmp"; }
trap cleanup EXIT
cp "$platform" "$tmp/platform.dll"
${CC:-x86_64-w64-mingw32-gcc} -O2 -Wall -Wextra shim/test_platform_scanner.c -o "$tmp/scanner_probe.exe"
export WINEPREFIX="$tmp/prefix" WINEDEBUG=-all
wine "$tmp/scanner_probe.exe" "Z:${tmp//\//\\}\\platform.dll" control > "$tmp/control.log" 2>&1
set +e
wine "$tmp/scanner_probe.exe" "Z:${tmp//\//\\}\\platform.dll" revoke > "$tmp/revoke.log" 2>&1
result=$?
set -e
cat "$tmp/control.log" "$tmp/revoke.log"
[[ $result == 42 ]]
grep -q 'original platform scanned committed candidate without a fault' "$tmp/control.log"
grep -q 'original platform+0x18fa reads revoked candidate' "$tmp/revoke.log"
[[ $(sha256sum "$tmp/platform.dll" | cut -d' ' -f1) == "$expected" ]]
[[ $(sha256sum "$platform" | cut -d' ' -f1) == "$expected" ]]
echo 'PASS: paired original-platform probe; independent scanner defect reproduced; input DLL unchanged'
