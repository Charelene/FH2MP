#!/usr/bin/env bash
# One command from the develop branch to production: commit and push develop (release/publish.sh: checks, tests; asks once), merge it into
# main through a pull request (as every release so far), tag main, wait for the GitHub Actions build of the tag, roll it out
# (ops/release-prod.sh: LSP + game server, minimum versions), then lift the maintenance message and go back to develop. It never re-rolls
# the Rivals events. Stops at the first failure; nothing is deployed when the build fails.
#   ops/ship.sh v1.0.1 <user@host> <ssh-key>
set -euo pipefail
cd "$(dirname "$0")/.."
tag=${1:?usage: ops/ship.sh v1.0.1 user@host ssh-key}; host=${2:?user@host}; key=${3:?ssh key}
[ "$(git rev-parse --abbrev-ref HEAD)" = develop ] || { echo "run this from the develop branch"; exit 1; }
git rev-parse -q --verify "refs/tags/$tag" >/dev/null && { echo "tag $tag already exists"; exit 1; }

FH2MP_PUBLISH_NO_TAG=1 release/publish.sh "$tag"

echo "== merging develop into main"
pr=$(gh pr list --head develop --base main --state open --json number -q '.[0].number')
if [ -z "$pr" ]; then
  notes="release/notes/$tag.md"
  gh pr create --base main --head develop --title "Release $tag" ${notes:+--body-file "$notes"} >/dev/null
  pr=$(gh pr list --head develop --base main --state open --json number -q '.[0].number')
fi
gh pr merge "$pr" --merge --match-head-commit "$(git rev-parse HEAD)"
git fetch -q origin
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
for _ in $(seq 30); do
  gh release view "$tag" --json assets -q '.assets[].name' 2>/dev/null | grep -qx manifest.json && break
  sleep 10
done

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
echo "done: $tag is live (back on develop)."
