#!/usr/bin/env bash
# Plan:    release/notes/v1.1.1.deploy.sh user@host ssh-key
# Publish: release/notes/v1.1.1.deploy.sh user@host ssh-key --execute
# Uses the existing full release workflow. Execution requires a clean committed
# develop tree and the workflow's explicit tag confirmation.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
exec "$root/ops/ship.sh" v1.1.1 "$@"
