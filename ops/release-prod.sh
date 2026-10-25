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
cfg=/etc/fh2mp/prod-launcher.json
ssh_() { ssh -i "$key" "$host" "$@"; }

echo "== 1/6 checking the release $tag"
shim=$(tr -d '[:space:]' < shim/VERSION)
app=$(python3 -I -c "import re;print(re.search(r'\"(.+)\"',open('launcher/fh2mp_launcher/version.py').read())[1])")
[ "$ver" = "$shim" ] && [ "$ver" = "$app" ] || { echo "version mismatch: tag $ver, shim $shim, launcher $app"; exit 1; }
git diff --quiet HEAD -- server ops launcher shim release || { echo "uncommitted changes in server/ops/launcher/shim/release: deploy exactly what was released"; exit 1; }
git rev-parse -q --verify "refs/tags/$tag" >/dev/null || { echo "no local tag $tag"; exit 1; }
[ "$(git rev-parse "$tag^{commit}")" = "$(git rev-parse HEAD)" ] || { echo "HEAD is not the commit tagged $tag: check out the tag first"; exit 1; }
gh auth status >/dev/null 2>&1 || { echo "gh is not logged in"; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
gh release download "$tag" --pattern manifest.json --pattern manifest.json.sig --dir "$tmp" >/dev/null 2>&1 \
  || { echo "release $tag is not published (or has no manifest yet): wait for the GitHub Actions run to finish"; exit 1; }
python3 -I - "$tmp/manifest.json" "$ver" <<'PY'
import json, sys
m = json.load(open(sys.argv[1])); v = sys.argv[2]
assert m.get("shim", {}).get("version") == v, f"manifest shim version {m.get('shim', {}).get('version')!r}, expected {v}"
assert m.get("launcher", {}).get("version") == v, f"manifest launcher version {m.get('launcher', {}).get('version')!r}, expected {v}"
assets = m["launcher"].get("assets", {})
assert assets, "the manifest lists no launcher files"
print("manifest ok:", ", ".join(sorted(assets)))
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
ops/deploy.sh prod "$host" "$key" --only lsp --yes-prod
ssh_ 'curl -fsS http://127.0.0.1:5080/v1/status' >/dev/null || { echo "the LSP does not answer after the deploy; the backup is /opt/fh2mp/lsp.bak-*"; exit 1; }

echo "== 4/6 game server"
if [ $skip_game = 1 ]; then echo "skipped"; else ops/deploy.sh prod "$host" "$key" --only game --yes-prod; fi

echo "== 5/6 minimum versions -> $ver"
ssh_ "sudo bash -s" "$cfg" "$ver" <<'REMOTE'
set -euo pipefail
cfg=$1; ver=$2
[ -f "$cfg" ] || { echo "$cfg does not exist: set FH2MP_LAUNCHER_CONFIG for the LSP first (docs/environments.md)"; exit 1; }
cp -a "$cfg" "$cfg.bak-$(date +%Y%m%d-%H%M%S)"
python3 - "$cfg" "$ver" <<'PY'
import json, os, sys
p, v = sys.argv[1:3]
d = json.load(open(p))
d["minLauncher"] = v
d["minShim"] = v
tmp = p + ".new"
with open(tmp, "w") as f:
    json.dump(d, f, indent=2); f.write("\n")
os.chmod(tmp, os.stat(p).st_mode & 0o777)
os.replace(tmp, p)
PY
ls -t "$cfg".bak-* | tail -n +6 | xargs -r rm -f
REMOTE
sleep 2
ssh_ 'curl -fsS http://127.0.0.1:5080/v1/launcher/config' | python3 -I -c "
import json, sys
c = json.load(sys.stdin)
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
