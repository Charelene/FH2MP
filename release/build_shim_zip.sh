#!/usr/bin/env bash
# Builds the shim and packs it as dist/fh2mp-shim-<version>.zip (version = shim/VERSION). Needs x86_64-w64-mingw32-gcc.
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=$(tr -d '[:space:]' < shim/VERSION)
shim/build.sh
mkdir -p dist
rm -f "dist/fh2mp-shim-$VERSION.zip"
tmp=$(mktemp -d)
cp shim/build/winmm.dll "$tmp/"
cp LICENSE "$tmp/LICENSE.txt"
printf 'FH2MP shim %s\nPut winmm.dll next to azimuth.exe (the launcher does this for you).\n' "$VERSION" > "$tmp/README.txt"
(cd "$tmp" && python3 -I -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1],'w',zipfile.ZIP_DEFLATED); [z.write(f) for f in ('winmm.dll','LICENSE.txt','README.txt')]; z.close()" "$OLDPWD/dist/fh2mp-shim-$VERSION.zip")
rm -rf "$tmp"
echo "dist/fh2mp-shim-$VERSION.zip"
