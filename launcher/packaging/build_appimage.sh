#!/usr/bin/env bash
# Packs dist/fh2mp-launcher (or $DIST) as dist/FH2MP-Launcher-<ver>-x86_64.AppImage (or $OUT). Needs appimagetool on PATH or $APPIMAGETOOL (CI downloads a pinned one).
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION=$(python3 -I -c "import re;print(re.search(r'\"(.+)\"',open('fh2mp_launcher/version.py').read())[1])")
app=$(mktemp -d)/FH2MP.AppDir
mkdir -p "$app/usr/bin" "$app/usr/lib"
cp -a "${DIST:-dist/fh2mp-launcher}" "$app/usr/lib/fh2mp-launcher"
cp fh2mp_launcher/assets/icon.png "$app/fh2mp-launcher.png"
cat > "$app/fh2mp-launcher.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=FH2MP Launcher
Exec=fh2mp-launcher
Icon=fh2mp-launcher
Categories=Game;
StartupWMClass=fh2mp-launcher
DESK
cat > "$app/AppRun" <<'RUN'
#!/bin/sh
HERE=$(dirname "$(readlink -f "$0")")
exec "$HERE/usr/lib/fh2mp-launcher/fh2mp-launcher" "$@"
RUN
chmod +x "$app/AppRun"
out=${OUT:-dist/FH2MP-Launcher-${VERSION}-x86_64.AppImage}
mkdir -p "$(dirname "$out")"
out=$(cd "$(dirname "$out")" && pwd)/$(basename "$out")
# Update information embedded in the AppImage (GitHub releases, zsync): AppImageUpdate and AppImage managers read it to find new versions. With zsyncmake
# on PATH appimagetool also writes <out>.zsync (into the current directory, hence the cd), which has to be uploaded to the release next to the AppImage.
cd "$(dirname "$out")"
# $APPIMAGE_RUNTIME: a runtime file checked by the caller (CI pins it); without it appimagetool downloads the current one itself.
ARCH=x86_64 "${APPIMAGETOOL:-appimagetool}" --appimage-extract-and-run ${APPIMAGE_RUNTIME:+--runtime-file "$APPIMAGE_RUNTIME"} \
  -u "gh-releases-zsync|Charelene|FH2MP|latest|FH2MP-Launcher-*-x86_64.AppImage.zsync" "$app" "$out"
