#!/usr/bin/env bash
# One command from the working tree to production: publish the release (release/publish.sh: tests, commit, tag, push; asks once), wait for
# the GitHub Actions build of the tag, roll it out (ops/release-prod.sh: LSP + game server, minimum versions), then lift the maintenance
# message. It never re-rolls the Rivals events. Stops at the first failure; nothing is deployed when the build fails.
#   ops/ship.sh v1.0.0 <user@host> <ssh-key>
set -euo pipefail
cd "$(dirname "$0")/.."
tag=${1:?usage: ops/ship.sh v1.0.0 user@host ssh-key}; host=${2:?user@host}; key=${3:?ssh key}

release/publish.sh "$tag"

echo "== waiting for the release build of $tag"
id=""
for _ in $(seq 60); do
  id=$(gh run list --workflow release.yml --branch "$tag" --limit 1 --json databaseId -q '.[0].databaseId' 2>/dev/null || true)
  [ -n "$id" ] && break
  sleep 5
done
[ -n "$id" ] || { echo "no release build found for $tag: nothing was deployed"; exit 1; }
gh run watch "$id" --exit-status >/dev/null || { echo "the release build failed (gh run view $id): nothing was deployed"; exit 1; }
for _ in $(seq 30); do
  gh release view "$tag" --json assets -q '.assets[].name' 2>/dev/null | grep -qx manifest.json && break
  sleep 10
done

ops/release-prod.sh "$tag" "$host" "$key" --yes-prod

echo "== lifting the maintenance message"
ssh -i "$key" "$host" 'T=$(sudo -n systemctl show fh2mp-lsp -p Environment --value | tr " " "\n" | sed -n "s/^FH2MP_OPS_TOKEN=//p")
  curl -sS -o /dev/null -X PUT -H "Authorization: Bearer $T" -H "Content-Type: application/json" --data "{\"maintenance\": \"\"}" http://127.0.0.1:5080/ops/v1/config
  curl -sS http://127.0.0.1:5080/v1/launcher/config | python3 -c "import json,sys; print(\"maintenance:\", repr(json.load(sys.stdin).get(\"maintenance\")))"'
echo "done: $tag is live."
