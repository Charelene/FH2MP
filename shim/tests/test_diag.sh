#!/usr/bin/env bash
# Traffic diagnostics (src/diag_trace.inc) against a fake game image and stand-in natives, see tests/test_diag.c.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
cleanup() { WINEPREFIX="$tmp/prefix" wineserver -w || true; rm -rf "$tmp"; }
trap cleanup EXIT
. tests/wine_prefix.sh; wine_prefix "$tmp/prefix"
${CC:-x86_64-w64-mingw32-gcc} -O2 -Wall -Wextra -Wno-unused-function -I . tests/test_diag.c -o "$tmp/test.exe"
WINEPREFIX="$tmp/prefix" WINEDEBUG=-all wine "$tmp/test.exe"
# The lines the hooks wrote must be the lines tools/analysis/diag_report.py reads: every kind, and the count it was told was dropped. tools/ is not in
# every checkout, so without the reader this part says so and is skipped.
if [ -f ../tools/analysis/diag_report.py ]; then
    WINEPREFIX="$tmp/prefix" WINEDEBUG=-all wine "$tmp/test.exe" --lines | tr -d '\r' > "$tmp/lines"
    python3 -I - "$tmp/lines" ../tools/analysis <<'PY'
import sys, collections
sys.path.insert(0, sys.argv[2])
import diag_report
events, lost = diag_report.parse([sys.argv[1]])
kinds = collections.Counter(k if k != 'server' else 'server ' + f[0] for _, k, f in events)
want = {'own': 1, 'req': 1, 'warp': 1, 'server take': 1, 'server drop': 1, 'server denied': 1}
assert all(kinds[k] >= n for k, n in want.items()), f'the parser missed a kind of line: {dict(kinds)}'
assert lost == 60, f'the parser read {lost} dropped lines, the shim wrote 60'
PY
else
    echo "tools/analysis/diag_report.py is not in this checkout: the line-format check against it is skipped"
fi
WINEPREFIX="$tmp/prefix" wineserver -w
