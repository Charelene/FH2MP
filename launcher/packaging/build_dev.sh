#!/usr/bin/env bash
# Developer build with a signed, bundled shim mirror and an isolated dev profile.
# Run from any directory. Optional: FH2MP_DEV_API, FH2MP_DEV_OUT, FH2MP_SIGNING_KEY.
set -euo pipefail
cd "$(dirname "$0")/../.."
root=$PWD
version=$(tr -d '[:space:]' < shim/VERSION)
azimuth_version=$(launcher/.venv/bin/python -I -c "import runpy; print(runpy.run_path('launcher/fh2mp_launcher/constants.py')['AZIMUTH_VERSION'])")
out=${FH2MP_DEV_OUT:-$root/dist/dev/$version}
mkdir -p "$out"
out=$(cd "$out" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work" "${image_stage:-}"' EXIT
release/build_shim_zip.sh
rel=$work/mirror/Charelene/FH2MP/releases
mkdir -p "$rel/download/v$version" "$rel/latest/download"
cp "dist/fh2mp-shim-$version.zip" "$rel/download/v$version/"
launcher/.venv/bin/python -I release/make_manifest.py --tag "v$version" --min-launcher "$version" \
    --dist "$rel/download/v$version" --key "${FH2MP_SIGNING_KEY:-$HOME/.config/fh2mp-release/signing.key}"
cp "$rel/download/v$version/"* "$rel/latest/download/"
FH2MP_DEV_API=${FH2MP_DEV_API:-https://dev-api.forza.scuffed.af} launcher/.venv/bin/python -I - "$work/buildcfg.json" <<'PY'
import json, os, sys
with open(sys.argv[1], 'w') as f:
    json.dump({'api_base': os.environ['FH2MP_DEV_API'], 'update_base': 'bundled', 'migrate_legacy_dev_api': True}, f)
PY
(cd launcher && FH2MP_DEV_BUILD=1 FH2MP_BUILDCFG="$work/buildcfg.json" FH2MP_MIRROR="$work/mirror" \
    .venv/bin/pyinstaller --noconfirm --clean --distpath "$out" --workpath "$root/launcher/build/dev-$version" packaging/fh2mp.spec)
cp "dist/fh2mp-shim-$version.zip" "$out/"
if [ -n "${APPIMAGETOOL:-}" ] || command -v appimagetool >/dev/null; then
    image_stage=$(mktemp -d "$out/.appimage-XXXXXX")
    image_name="FH2MP-Dev-$version-Azimuth-$azimuth_version-x86_64.AppImage"
    DIST="$out/fh2mp-launcher" OUT="$image_stage/$image_name" launcher/packaging/build_appimage.sh
    mv -f "$image_stage/$image_name" "$out/$image_name"
    if [ -f "$image_stage/$image_name.zsync" ]; then
        mv -f "$image_stage/$image_name.zsync" "$out/$image_name.zsync"
    fi
fi
echo "Developer launcher built in $out"
