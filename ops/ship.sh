#!/usr/bin/env bash
# One command from a reviewed develop commit to production: push develop (release/publish.sh: checks and tests), merge it into
# main through a pull request (as every release so far), tag main, wait for the GitHub Actions build of the tag, roll it out
# (ops/release-prod.sh: LSP + game server, minimum versions), then lift the maintenance message and go back to develop. It never re-rolls
# the Rivals events. Stops at the first failure; nothing is deployed when the build fails.
#   ops/ship.sh v1.1.0 <user@host> <ssh-key>             # local plan only
#   ops/ship.sh v1.1.0 <user@host> <ssh-key> --execute   # asks for the tag once
# Commit reviewed files before execution. This script never stages or commits.
set -euo pipefail
cd "$(dirname "$0")/.."
tag=${1:?usage: ops/ship.sh v1.1.0 user@host ssh-key [--execute]}; host=${2:?user@host}; key=${3:?ssh key}; shift 3
execute=0
while [ $# -gt 0 ]; do
  case $1 in --execute) execute=1;; *) echo "unknown option: $1"; exit 2;; esac
  shift
done
[[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo 'Use a stable release tag such as v1.1.0.'; exit 2; }
[[ "$host" =~ ^[A-Za-z0-9_.-]+@[A-Za-z0-9_.-]+$ ]] || { echo 'Expected user@host.'; exit 2; }
ver=${tag#v}
shim=$(tr -d '[:space:]' < shim/VERSION)
app=$(python3 -I -c "import ast; print(ast.literal_eval(open('launcher/fh2mp_launcher/version.py').read().split('=',1)[1].strip()))")
[ "$ver" = "$shim" ] && [ "$ver" = "$app" ] || { echo "version mismatch: tag=$ver shim=$shim launcher=$app"; exit 1; }
[ -s "release/notes/$tag.md" ] || { echo "Missing release/notes/$tag.md"; exit 1; }
echo "Full production release: $tag -> $host"
echo '1. Run launcher, server and shim checks against a clean, committed develop tree.'
echo '2. Push that commit; merge develop into main through a matching-head PR.'
echo '3. Tag main; wait for the exact tag/commit release workflow to succeed.'
echo '4. Verify the signed manifest and Windows/Linux launcher + shim assets.'
echo '5. Back up and deploy LSP + game server; apply migrations and restart both.'
echo "6. Require launcher/shim $ver, verify health and version gates, clear maintenance."
echo 'The game-server restart disconnects active players.'
if [ "$execute" = 0 ]; then
  echo 'Plan only: no Git, GitHub or server changes were made.'
  echo 'Review and commit the release files, then rerun with --execute.'
  exit 0
fi
[ -z "$(git status --porcelain)" ] || { echo 'Commit reviewed release files first; nothing was staged.'; exit 1; }
[ -f "$key" ] || { echo 'SSH key file not found.'; exit 1; }
[ "$(git rev-parse --abbrev-ref HEAD)" = develop ] || { echo "run this from the develop branch"; exit 1; }
git rev-parse -q --verify "refs/tags/$tag" >/dev/null && { echo "tag $tag already exists"; exit 1; }
release_develop_head=$(git rev-parse HEAD)
answer=${FH2MP_CONFIRM_TAG:-}
[ -n "$answer" ] || read -r -p "Type $tag to publish all clients and deploy production: " answer
[ "$answer" = "$tag" ] || { echo cancelled; exit 1; }
export FH2MP_CONFIRM_TAG="$tag"

FH2MP_PUBLISH_NO_TAG=1 release/publish.sh "$tag"

echo "== merging develop into main"
pr=$(gh pr list --head develop --base main --state open --json number -q '.[0].number')
if [ -z "$pr" ]; then
  notes="release/notes/$tag.md"
  gh pr create --base main --head develop --title "Release $tag" --body-file "$notes" >/dev/null
  pr=$(gh pr list --head develop --base main --state open --json number -q '.[0].number')
fi
gh pr merge "$pr" --merge --match-head-commit "$release_develop_head"
git fetch -q origin
git merge-base --is-ancestor "$release_develop_head" origin/main || { echo 'The reviewed develop commit is absent from origin/main; not tagging.'; exit 1; }
git checkout -q main
git merge -q --ff-only origin/main
git tag -a "$tag" -m "FH2MP $tag"
git push origin "$tag"

echo "== waiting for the release build of $tag"
id=""
for _ in $(seq 60); do
  id=$(gh run list --workflow release.yml --branch "$tag" --event push --commit "$(git rev-parse HEAD)" --limit 1 --json databaseId -q '.[0].databaseId' 2>/dev/null || true)
  [ -n "$id" ] && break
  sleep 5
done
[ -n "$id" ] || { echo "no release build found for $tag: nothing was deployed"; exit 1; }
gh run watch "$id" --exit-status >/dev/null || { echo "the release build failed (gh run view $id): nothing was deployed"; exit 1; }
ready=0
for _ in $(seq 30); do
  if gh release view "$tag" --json assets -q '.assets[].name' 2>/dev/null | grep -qx manifest.json; then ready=1; break; fi
  sleep 10
done
[ "$ready" = 1 ] || { echo 'No published manifest found; nothing was deployed. Resume with ops/release-prod.sh after checking the release.'; exit 1; }

FH2MP_CONFIRM_TAG="$tag" ops/release-prod.sh "$tag" "$host" "$key" --yes-prod

echo "== lifting the maintenance message"
ssh -i "$key" "$host" 'sudo -n bash -s' <<'REMOTE'
set -euo pipefail
release_ops_token=$(systemctl show fh2mp-lsp -p Environment --value | tr ' ' '\n' | sed -n 's/^FH2MP_OPS_TOKEN=//p')
[ -n "$release_ops_token" ] || { echo 'operator token unavailable; maintenance was not cleared'; exit 1; }
printf 'header = "Authorization: Bearer %s"\n' "$release_ops_token" | curl --config - -fsS -o /dev/null -X PUT \
  -H 'Content-Type: application/json' --data '{"maintenance":""}' http://127.0.0.1:5080/ops/v1/config
curl -fsS http://127.0.0.1:5080/v1/launcher/config | python3 -c 'import json,sys; c=json.load(sys.stdin); assert c.get("maintenance") == "", "maintenance is still active"; print("maintenance cleared")'
REMOTE
git checkout -q develop
echo "done: $tag clients are published and both production services deployed (back on develop)."
echo 'Code backups: /opt/fh2mp/{lsp,server}.bak-*; config backups: /etc/fh2mp/prod-launcher.json.bak-*.'
echo 'Rollback requires restoring both code directories and the minimum-version config/ops overlay.'
echo 'Database migrations need a separate verified database backup; code rollback does not undo them.'
