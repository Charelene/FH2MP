#!/usr/bin/env bash
# The developer launcher from this checkout: own settings/key (~/.config/fh2mp-dev), dev channel, local shim installs, "sign in only".
#   launcher/dev.sh [--screenshot out.png]      first run creates launcher/.venv
set -euo pipefail
cd "$(dirname "$0")"
if [ ! -x .venv/bin/python ]; then
  python3 -m venv .venv
  .venv/bin/pip install -q "PySide6-Essentials>=6.6" "cryptography>=42" "keyring>=24"
fi
exec .venv/bin/python -m fh2mp_launcher --dev "$@"
