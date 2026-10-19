#!/usr/bin/env bash
# Packages (.deb, AppImage, loose executable) that behave like a released launcher before any GitHub release exists:
# it talks to the given server and installs the shim from a signed release mirror packed inside itself (signature, SHA-256 and the
# whole updater path are the real ones).
#   FH2MP_TEST_API=http://host:5080 packaging/build_testrelease.sh [--key <signing key file>] [--out <file>]
# Linux/macOS only (PyInstaller does not cross-compile); the Windows build is the CI workflow.
set -euo pipefail
cd "$(dirname "$0")/../.."
api=${FH2MP_TEST_API:?set FH2MP_TEST_API to the server address (http://host:port)}
key=${HOME}/.config/fh2mp-release/signing.key
while [ $# -gt 0 ]; do case $1 in --key) key=$2; shift 2;; *) echo "unknown option $1"; exit 2;; esac; done
VERSION=$(tr -d '[:space:]' < shim/VERSION)
release/build_shim_zip.sh >/dev/null
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
repo=Charelene/FH2MP
rel=$work/mirror/$repo/releases
mkdir -p "$rel/download/v$VERSION" "$rel/latest/download"
cp "dist/fh2mp-shim-$VERSION.zip" "$rel/download/v$VERSION/"
launcher/.venv/bin/python -I release/make_manifest.py --tag "v$VERSION" --dist "$rel/download/v$VERSION" --key "$key"
cp "$rel/download/v$VERSION"/manifest.json "$rel/download/v$VERSION"/manifest.json.sig "$rel/download/v$VERSION/fh2mp-shim-$VERSION.zip" "$rel/latest/download/"
case $api in http://*) insecure=true;; *) insecure=false;; esac
printf '{"api_base": "%s", "allow_insecure": %s, "update_base": "bundled"}\n' "$api" "$insecure" > "$work/buildcfg.json"
(cd launcher && FH2MP_BUILDCFG="$work/buildcfg.json" FH2MP_MIRROR="$work/mirror" .venv/bin/pyinstaller --noconfirm --clean \
   --distpath "$work/out" --workpath "$work/pyi" packaging/fh2mp.spec >/dev/null 2>&1)
out=dist/test; mkdir -p "$out"; rm -f "$out"/*
python3 -I launcher/packaging/build_deb.py --dist "$work/out/fh2mp-launcher" --out "$out/fh2mp-launcher_${VERSION}_amd64.deb"
if [ -n "${APPIMAGETOOL:-}" ] || command -v appimagetool >/dev/null; then
  DIST="$work/out/fh2mp-launcher" OUT="$PWD/$out/FH2MP-Launcher-${VERSION}-x86_64.AppImage" launcher/packaging/build_appimage.sh >/dev/null
fi
(cd launcher && FH2MP_ONEFILE=1 FH2MP_BUILDCFG="$work/buildcfg.json" FH2MP_MIRROR="$work/mirror" .venv/bin/pyinstaller --noconfirm --clean \
   --distpath "$work/one" --workpath "$work/pyi1" packaging/fh2mp.spec >/dev/null 2>&1)
cp "$work/one/fh2mp-launcher" "$out/FH2MP-Launcher-$VERSION-linux-x86_64"
echo "built in $out (server $api, shim $VERSION from the bundled signed mirror):"; ls -1sh "$out"
