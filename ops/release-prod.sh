#!/usr/bin/env bash
# Rolls a published release out to production in one go: deploys the server code (LSP, then game server), then raises the minimum launcher and
# shim versions to the release so older clients are turned away (server/launcher_api.py: minLauncher at sign-in, minShim on every web-service call).
#   ops/release-prod.sh v1.0.0-rc4 <user@host> <ssh-key> --yes-prod [--skip-game]
# Run it AFTER release/publish.sh and after the GitHub release shows up: raising the minimums before the files exist would lock everybody out,
# so the script refuses unless the release has the signed manifest and the files for this version.
# Restarting the game server drops everyone who is playing (--skip-game deploys the LSP only; the game server then keeps its old code).
# Needs: gh (logged in), ssh access with sudo on the VPS. Config on the VPS: /etc/fh2mp/prod-launcher.json (re-read when it changes, no restart).
set -euo pipefail
cd "$(dirname "$0")/.."
tag=${1:?usage: ops/release-prod.sh v1.0.0-rc4 user@host ssh-key --yes-prod [--skip-game]}; host=${2:?user@host}; key=${3:?ssh key}; shift 3
yes=0; skip_game=0
while [ $# -gt 0 ]; do case $1 in --yes-prod) yes=1; shift;; --skip-game) skip_game=1; shift;; *) echo "unknown option $1"; exit 2;; esac; done
[ $yes = 1 ] || { echo "refusing to touch production without --yes-prod"; exit 2; }
ver=${tag#v}
[[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-rc[0-9]+)?$ ]] || { echo 'invalid release tag'; exit 2; }
cfg=/etc/fh2mp/prod-launcher.json
ssh_() { ssh -i "$key" "$host" "$@"; }

echo "== 1/6 checking the release $tag"
shim=$(tr -d '[:space:]' < shim/VERSION)
app=$(python3 -I -c "import re;print(re.search(r'\"(.+)\"',open('launcher/fh2mp_launcher/version.py').read())[1])")
[ "$ver" = "$shim" ] && [ "$ver" = "$app" ] || { echo "version mismatch: tag $ver, shim $shim, launcher $app"; exit 1; }
git diff --quiet HEAD -- server ops launcher shim release .github third_party || { echo "uncommitted release changes: deploy exactly what was released"; exit 1; }
[ -z "$(git ls-files --others --exclude-standard -- server ops launcher shim release .github third_party)" ] || { echo "untracked release files: deploy exactly what was released"; exit 1; }
git rev-parse -q --verify "refs/tags/$tag" >/dev/null || { echo "no local tag $tag"; exit 1; }
[ "$(git rev-parse "$tag^{commit}")" = "$(git rev-parse HEAD)" ] || { echo "HEAD is not the commit tagged $tag: check out the tag first"; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "gh is not logged in"; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
gh release download "$tag" --pattern manifest.json --pattern manifest.json.sig --dir "$tmp" >/dev/null 2>&1 \
  || { echo "release $tag is not published (or has no manifest yet): wait for the GitHub Actions run to finish"; exit 1; }
gh release view "$tag" --json assets > "$tmp/assets.json"
launcher/.venv/bin/python -I - "$tmp" "$ver" <<'PY'
import json, pathlib, sys
sys.path.insert(0, str(pathlib.Path('launcher').resolve()))
from fh2mp_launcher.updater import verify_manifest
p = pathlib.Path(sys.argv[1]); v = sys.argv[2]
m = verify_manifest((p / 'manifest.json').read_bytes(), (p / 'manifest.json.sig').read_text())
assert m.get('tag') == 'v' + v, 'manifest tag mismatch'
assert m.get("shim", {}).get("version") == v, f"manifest shim version {m.get('shim', {}).get('version')!r}, expected {v}"
assert m.get("launcher", {}).get("version") == v, f"manifest launcher version {m.get('launcher', {}).get('version')!r}, expected {v}"
assets = m["launcher"].get("assets", {})
required = {'windows-x64-installer', 'windows-x64-portable', 'linux-deb', 'linux-appimage'}
assert required <= assets.keys(), 'the manifest is missing launcher packages'
published = {a['name']: a['size'] for a in json.loads((p / 'assets.json').read_text())['assets']}
for asset in [m['shim'], *assets.values()]:
    assert published.get(asset['name']) == asset['size'] > 0, f"missing/incomplete release asset: {asset['name']}"
print("manifest ok:", ", ".join(sorted(assets)))
PY
# Enabling sync must never advertise a missing/corrupt database. The canonical file stays on the server;
# the client backs up car-pack overrides before placement and restores them for offline play.
echo "== checking the canonical car database (enforcement will be enabled)"
ssh_ 'sudo -n -u fh2mp python3 -' <<'PY'
import hashlib, pathlib, sqlite3
p = pathlib.Path('/opt/fh2mp/gamedbRC.slt')
data = p.read_bytes()
assert 0 < len(data) <= 64 * 1024 * 1024 and data.startswith(b'SQLite format 3\0'), 'invalid canonical database'
with sqlite3.connect(p.as_uri() + '?mode=ro', uri=True) as c:
    assert c.execute('PRAGMA quick_check').fetchall() == [('ok',)], 'database quick_check failed'
    count = c.execute('SELECT COUNT(*) FROM Data_Car').fetchone()[0]
    assert count > 0, 'empty car catalog'
print('canonical database:', len(data), 'bytes,', count, 'cars, SHA256', hashlib.sha256(data).hexdigest())
PY
for f in server/*.py; do python3 -I -m py_compile "$f"; done
find server -name __pycache__ -prune -exec rm -rf {} + 2>/dev/null || true

echo "== 2/6 production state before"
ssh_ 'systemctl is-active fh2mp-lsp fh2mp; sudo cat '"$cfg"'' || true
echo
echo "About to deploy $tag to PRODUCTION and set minLauncher/minShim to $ver."
[ $skip_game = 1 ] && echo "(game server left alone)" || echo "(the game server restarts: everyone in a lobby is dropped)"
answer=${FH2MP_CONFIRM_TAG:-}          # non-interactive runs: FH2MP_CONFIRM_TAG=<tag> (the ssh calls above would eat piped input)
[ -n "$answer" ] || read -r -p "Type the tag to continue: " answer
[ "$answer" = "$tag" ] || { echo "cancelled"; exit 1; }

echo "== 3/6 account service (LSP)"
ssh_ 'sudo -n bash -s' "$skip_game" <<'REMOTE'
set -euo pipefail
dir=/etc/systemd/system/fh2mp-lsp.service.d
mkdir -p "$dir"
file=$dir/90-gamedb-sync.conf
if [ -f "$file" ]; then cp -a "$file" "$file.bak-$(date +%Y%m%d-%H%M%S)"; fi
printf '%s\n' '[Service]' 'Environment=FH2MP_SYNC_GAMEDB=/opt/fh2mp/gamedbRC.slt' > "$file"
chmod 644 "$file"
if [ "$1" = 0 ]; then
    file=$dir/91-restricted-tools.conf
    if [ -f "$file" ]; then cp -a "$file" "$file.bak-$(date +%Y%m%d-%H%M%S)"; fi
    printf '%s\n' '[Service]' 'Environment=FH2MP_RESTRICTED_TOOLS=quarantine' > "$file"
    chmod 644 "$file"
    dir=/etc/systemd/system/fh2mp.service.d
    mkdir -p "$dir"
    file=$dir/91-restricted-tools.conf
    if [ -f "$file" ]; then cp -a "$file" "$file.bak-$(date +%Y%m%d-%H%M%S)"; fi
    printf '%s\n' '[Service]' 'Environment=FH2MP_CHEATER_POOL_HOURS=24' > "$file"
    chmod 644 "$file"
fi
systemctl daemon-reload
REMOTE
ops/deploy.sh prod "$host" "$key" --only lsp --yes-prod
ssh_ 'curl -fsS http://127.0.0.1:5080/v1/status' >/dev/null || { echo "the LSP does not answer after the deploy; the backup is /opt/fh2mp/lsp.bak-*"; exit 1; }

echo "== 4/6 game server"
if [ $skip_game = 1 ]; then echo "skipped"; else ops/deploy.sh prod "$host" "$key" --only game --yes-prod; fi

echo "== 5/6 minimum versions and Azimuth compatibility -> $ver"
compatibility=$(launcher/.venv/bin/python -I -c "
import base64, json, runpy
c = runpy.run_path('launcher/fh2mp_launcher/constants.py')
print(base64.b64encode(json.dumps({'latestAzimuth': c['AZIMUTH_VERSION'], 'supportedAzimuth': c['BUILTIN_AZIMUTH_BUILDS']}).encode()).decode())")
ssh_ "sudo bash -s" "$cfg" "$ver" "$compatibility" <<'REMOTE'
set -euo pipefail
cfg=$1; ver=$2; compatibility=$3
[ -f "$cfg" ] || { echo "$cfg does not exist: set FH2MP_LAUNCHER_CONFIG for the LSP first (docs/environments.md)"; exit 1; }
cp -a "$cfg" "$cfg.bak-$(date +%Y%m%d-%H%M%S)"
python3 - "$cfg" "$ver" "$compatibility" <<'PY'
import base64, json, os, sys
p, v, compatibility = sys.argv[1:4]
d = json.load(open(p))
d["minLauncher"] = v
d["minShim"] = v
d.update(json.loads(base64.b64decode(compatibility)))
tmp = p + ".new"
with open(tmp, "w") as f:
    json.dump(d, f, indent=2); f.write("\n")
os.chmod(tmp, os.stat(p).st_mode & 0o777)
os.replace(tmp, p)
PY
ls -t "$cfg".bak-* | tail -n +6 | xargs -r rm -f
REMOTE
# The operator overlay wins over the JSON file. Use the existing local operator API
# too, so a previous website setting cannot keep the old minimum versions active.
# The token stays on the host and goes to curl through stdin, never its argv/output.
ssh_ 'sudo -n bash -s' "$ver" <<'REMOTE'
set -euo pipefail
release_ops_token=$(systemctl show fh2mp-lsp -p Environment --value | tr ' ' '\n' | sed -n 's/^FH2MP_OPS_TOKEN=//p')
[ -n "$release_ops_token" ] || { echo 'operator token unavailable; minimum-version overlay was not updated'; exit 1; }
body=$(python3 -c 'import json,sys; print(json.dumps({"minLauncher":sys.argv[1],"minShim":sys.argv[1]}))' "$1")
printf 'header = "Authorization: Bearer %s"\n' "$release_ops_token" | curl --config - -fsS -o /dev/null -X PUT \
    -H 'Content-Type: application/json' --data "$body" http://127.0.0.1:5080/ops/v1/config
REMOTE
sleep 2
ssh_ 'curl -fsS http://127.0.0.1:5080/v1/launcher/config' | python3 -I -c "
import base64, json, sys
c = json.load(sys.stdin)
expected_compatibility = json.loads(base64.b64decode('$compatibility'))
assert all(c.get(k) == v for k, v in expected_compatibility.items()), 'server reports stale Azimuth compatibility metadata'
got = (c.get('minLauncher'), c.get('minShim'))
print('server reports minLauncher/minShim:', got)
sys.exit(0 if got == ('$ver', '$ver') else 'the server does not report $ver: a setting saved through the operator API (PUT /ops/v1/config) overrides the file; set it there too')"

echo "== 6/6 checks"
old=$(ssh_ "curl -s -o /dev/null -w '%{http_code}' -H 'X-FH2MP-Shim: 1.0.0-rc3' -H 'X-ClassName: x' -H 'X-Method: y' http://127.0.0.1:5080/")
new=$(ssh_ "curl -s -o /dev/null -w '%{http_code}' -H 'X-FH2MP-Shim: $ver' -H 'X-ClassName: x' -H 'X-Method: y' http://127.0.0.1:5080/")
none=$(ssh_ "curl -s -o /dev/null -w '%{http_code}' -H 'X-ClassName: x' -H 'X-Method: y' http://127.0.0.1:5080/")
echo "shim rc3 -> $old (want 426), shim $ver -> $new (want anything but 426), no header -> $none (want 426)"
[ "$old" = 426 ] && [ "$new" != 426 ] && [ "$none" = 426 ] || { echo "CHECK FAILED"; exit 1; }
ssh_ 'systemctl is-active fh2mp-lsp fh2mp'
# game server load (the send queue, issue 35): CPU of the service and the kernel's UDP receive drops; run this again after ~10 minutes of play
ssh_ 'ps -o pcpu=,etime= -p "$(systemctl show -p MainPID --value fh2mp)" | awk "{print \"game server CPU \" \$1 \" % over \" \$2}";
      awk "/^Udp:/{if(!h){split(\$0,k);h=1}else for(i=1;i<=NF;i++)if(k[i]==\"RcvbufErrors\")print \"udp RcvbufErrors since boot: \" \$i}" /proc/net/snmp'
echo "done: production runs $tag and requires it."
echo "Watch: RcvbufErrors should stay flat while lobbies are full. Fallback (one message per datagram): Environment=FH2MP_MAX_PACKET_BYTES=1 for fh2mp."
echo "Rollback of the requirement only: put the old values into $cfg (backup $cfg.bak-*); it is re-read at once."
