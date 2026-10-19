#!/usr/bin/env bash
# Packs dist/fh2mp-launcher (PyInstaller output) as dist/fh2mp-launcher_<ver>_amd64.deb (pure Python: no dpkg needed).
set -euo pipefail
exec python3 -I "$(dirname "$0")/build_deb.py" "$@"
