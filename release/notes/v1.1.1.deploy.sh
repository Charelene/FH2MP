#!/usr/bin/env bash
# Plan:    release/notes/v1.1.1.deploy.sh user@host ssh-key
# Publish: release/notes/v1.1.1.deploy.sh user@host ssh-key --execute
# Uses the existing full release workflow. Execution requires a clean committed
# develop tree and the workflow's explicit tag confirmation.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
usage() {
  cat <<EOF
Usage: $0 [user@host ssh-key] [--execute]

Without --execute, show the release plan.
With --execute, publish and deploy after the release checks and tag confirmation.

The host and key may be saved in release/operator.local.env as
FH2MP_DEPLOY_HOST and FH2MP_DEPLOY_KEY, or passed as environment variables.
EOF
}
case "${1:-}" in -h|--help) usage; exit 0;; esac
host=${FH2MP_DEPLOY_HOST:-}
key=${FH2MP_DEPLOY_KEY:-}
if [ -f "$root/release/operator.local.env" ]; then
  while IFS='=' read -r name value; do
    case "$name" in
      FH2MP_DEPLOY_HOST) host=${host:-$value};;
      FH2MP_DEPLOY_KEY) key=${key:-$value};;
    esac
  done < "$root/release/operator.local.env"
fi
if [ "$#" -gt 0 ] && [ "$1" != --execute ]; then
  if [ "$#" -lt 2 ]; then usage >&2; exit 2; fi
  host=$1; key=$2; shift 2
fi
if [ -z "$host" ] || [ -z "$key" ]; then
  usage >&2
  echo 'Set the deployment host and SSH key before running this release.' >&2
  exit 2
fi
exec "$root/ops/ship.sh" v1.1.1 "$host" "$key" "$@"
