#!/usr/bin/env bash
# Builds the shim and packs it as dist/fh2mp-shim-<version>.zip (version = shim/VERSION). Needs x86_64-w64-mingw32-gcc and Go (the zip is
# packed by native/cmd/fh2mp-release, release/reltool.sh; it was a python3 one-liner until 1.5.0).
# The zip is laid out like the Azimuth folder: fh2-mods/FH2MP.dll, fh2mp/LICENSE.txt, fh2mp/README.txt. The release's Windows job signs
# FH2MP.dll and packs the zip again (.github/workflows/release.yml shim-sign). The Guide (FH2MPGuide.dll) is not in it since 1.5.0: Encore
# builds, signs and installs its own. Neither are the FH2 Mod Platform and FH2 Extra Options: Encore downloads their authors' pinned releases.
set -euo pipefail
cd "$(dirname "$0")/.."
. release/reltool.sh
VERSION=$(tr -d '[:space:]' < shim/VERSION)
PLATFORM=$(tr -d '[:space:]' < paddock/third_party/fh2-mod-platform/VERSION)
shim/build.sh
mkdir -p dist
rm -f "dist/fh2mp-shim-$VERSION.zip"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/fh2-mods" "$tmp/fh2mp"
cp shim/build/FH2MP.dll "$tmp/fh2-mods/"
cp LICENSE "$tmp/fh2mp/LICENSE.txt"
cat > "$tmp/fh2mp/README.txt" <<TXT
FH2MP $VERSION (Forza Horizon 2 online)
https://github.com/Charelene/FH2MP

Encore, the launcher for FH2MP, installs and updates everything for you. By hand, extract this zip into your Azimuth folder (the one
with azimuth.exe), so FH2MP.dll ends up in fh2-mods. It requires Azimuth 1.0.4, FH2 Mod Platform $PLATFORM (Trinculo54,
https://github.com/Trinculo54/fh2-mod-platform), and FH2 Extra Options (RokkuDayo, https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions)
at the builds the README of the FH2MP release names. On Linux start Azimuth with WINEDLLOVERRIDES="winmm,dwrite,d3dcompiler_47=n,b" so
Wine uses both mods' DLLs instead of its own.

Logs: data\bin\fh2mp.log (FH2MP) and fh2-mods\platform.log (the platform and every mod). FH2MP is GPL-3.0 (fh2mp\LICENSE.txt).
TXT
reltool zip "dist/fh2mp-shim-$VERSION.zip" "$tmp" fh2-mods/FH2MP.dll fh2mp/LICENSE.txt fh2mp/README.txt
