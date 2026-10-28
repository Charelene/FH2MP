#!/usr/bin/env bash
# Builds the shim and packs it as dist/fh2mp-shim-<version>.zip (version = shim/VERSION). Needs x86_64-w64-mingw32-gcc.
# The zip is laid out like the Azimuth folder (fh2-mods/FH2MP.dll, fh2mp/LICENSE.txt, fh2mp/README.txt). The FH2 Mod Platform and FH2 Extra
# Options are not in it: the launcher downloads their authors' pinned releases (launcher/fh2mp_launcher/upstream.py).
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=$(tr -d '[:space:]' < shim/VERSION)
PLATFORM=$(tr -d '[:space:]' < third_party/fh2-mod-platform/VERSION)
shim/build.sh
mkdir -p dist
rm -f "dist/fh2mp-shim-$VERSION.zip"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/fh2-mods" "$tmp/fh2mp"
cp shim/build/FH2MP.dll "$tmp/fh2-mods/"
cp LICENSE "$tmp/fh2mp/LICENSE.txt"
cat > "$tmp/fh2mp/README.txt" <<EOF
FH2MP $VERSION (Forza Horizon 2 online)
https://github.com/Charelene/FH2MP

The FH2MP launcher installs and updates everything for you. By hand, extract this zip into your Azimuth folder (the one with
azimuth.exe), so FH2MP.dll ends up in fh2-mods. It requires Azimuth 1.0.3, FH2 Mod Platform $PLATFORM (Trinculo54,
https://github.com/Trinculo54/fh2-mod-platform), and FH2 Extra Options 0.3.9 (RokkuDayo,
https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions). On Linux start Azimuth with
WINEDLLOVERRIDES="winmm,dwrite,d3dcompiler_47=n,b" so Wine uses both mods' DLLs instead of its own.

Logs: data\\bin\\fh2mp.log (FH2MP) and fh2-mods\\platform.log (the platform and every mod). FH2MP is GPL-3.0 (fh2mp\\LICENSE.txt).
EOF
(cd "$tmp" && python3 -I -c "
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED)
for f in ('fh2-mods/FH2MP.dll', 'fh2mp/LICENSE.txt', 'fh2mp/README.txt'):
    z.write(f)
z.close()" "$OLDPWD/dist/fh2mp-shim-$VERSION.zip")
echo "dist/fh2mp-shim-$VERSION.zip"
