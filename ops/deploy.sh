#!/usr/bin/env bash
# Deploys the server code (LSP + game server) to the dev or the production stack on the VPS.
#   ops/deploy.sh dev  <user@host> <ssh-key> [--only lsp|game]
#   ops/deploy.sh prod <user@host> <ssh-key> [--only lsp|game] --yes-prod
# dev:  /opt/fh2mp-dev, services fh2mp-lsp-dev + fh2mp-dev, database fh2mp_dev.
# prod: /opt/fh2mp,     services fh2mp-lsp + fh2mp,         database fh2mp.   Restarting the game server drops everyone who is playing.
# Code goes to a root-owned directory (the services run unprivileged and cannot modify it). Nothing in the databases is touched here:
# schema changes are migrations applied by the LSP when it starts.
set -euo pipefail
cd "$(dirname "$0")/.."
env=${1:?dev|prod}; host=${2:?user@host}; key=${3:?ssh key}; shift 3
only=both; yes=0
while [ $# -gt 0 ]; do case $1 in --only) only=$2; shift 2;; --yes-prod) yes=1; shift;; *) echo "unknown option $1"; exit 2;; esac; done
case $env in
  dev)  root=/opt/fh2mp-dev; lsp=fh2mp-lsp-dev; game=fh2mp-dev;;
  prod) root=/opt/fh2mp;     lsp=fh2mp-lsp;     game=fh2mp
        [ $yes = 1 ] || { echo "refusing to touch production without --yes-prod"; exit 2; };;
  *) echo "env must be dev or prod"; exit 2;;
esac
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/lsp" "$tmp/server"
for m in fh2mp_lsp db xls xls_schema cardb drivatar scoreboard score_reports xp_audit rival_records rivals ugc pngtool notifstore xbl clubs social auth launcher_api ops_api site_stats home gamedb_sync rentals; do cp "server/$m.py" "$tmp/lsp/"; done
cp server/fh2mp_server.py server/traffic.py server/car_policy.py server/parts.py server/part_ids.py server/part_tables.py server/gameopts.py server/replay.py server/tunnel.py "$tmp/server/"
ssh -i "$key" "$host" 'rm -rf /tmp/fh2mp-deploy && mkdir /tmp/fh2mp-deploy'
scp -i "$key" -q -r "$tmp/lsp" "$tmp/server" "$host:/tmp/fh2mp-deploy/"
ssh -i "$key" "$host" "set -e
  chmod -R a+rX /tmp/fh2mp-deploy
  stamp=\$(date +%Y%m%d-%H%M%S)
  if [ '$only' != game ]; then
    sudo mkdir -p $root/lsp; sudo cp -a $root/lsp $root/lsp.bak-\$stamp 2>/dev/null || true
    sudo cp /tmp/fh2mp-deploy/lsp/*.py $root/lsp/
  fi
  if [ '$only' != lsp ]; then
    sudo mkdir -p $root/server; sudo cp /tmp/fh2mp-deploy/server/*.py $root/server/
  fi
  [ '$only' = game ] || sudo systemctl restart $lsp
  [ '$only' = lsp ] || sudo systemctl restart $game
  sleep 10
  systemctl is-active $lsp $game
  rm -rf /tmp/fh2mp-deploy
  ls -d $root/lsp.bak-* 2>/dev/null | head -n -3 | xargs -r sudo rm -rf"
echo "deployed to $env"
