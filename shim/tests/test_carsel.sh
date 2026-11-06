#!/usr/bin/env bash
# Car select hold and group deadline (src/carsel_wire.h, src/carsel_sync.inc): the wire format against server/carsel.py, the hold decision and the deadline
# rules with a faked game, without the game. Runs the test executable directly on Windows, under Wine elsewhere.
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
CC=${CC:-x86_64-w64-mingw32-gcc}
case "$(uname -s)" in
  MINGW*|MSYS*) RUN=""; trap 'rm -rf "$work"' EXIT ;;
  *) . tests/wine_prefix.sh; wine_prefix "$work/prefix"; mkdir -p "$work/prefix"
     trap 'WINEPREFIX="$work/prefix" wineserver -k >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
     RUN="env WINEPREFIX=$work/prefix WINEDEBUG=-all wine" ;;
esac
$CC -O2 -Wall -Wextra -Wno-unused-function -Wno-cast-function-type -I . tests/test_carsel.c -o "$work/test.exe" -lws2_32
$RUN "$work/test.exe"
$RUN "$work/test.exe" --vector > "$work/vector"
PYTHON=${PYTHON:-python3}
{ command -v "$PYTHON" >/dev/null 2>&1 && "$PYTHON" -c 1 >/dev/null 2>&1; } || PYTHON=python
for answer in "1 5000" "0 none" "1 none" "0 0"; do
set -- $answer
tag="$1-$2"
"$PYTHON" -I - "$work/vector" "$work/reply$tag" "$1" "$2" <<'PY'
import sys, pathlib, struct
from types import SimpleNamespace as NS
sys.path.insert(0, str(pathlib.Path.cwd().parent / 'server'))
import carsel
# the request the C code built is accepted by the server, and the server's reply is what the C code accepts
payload = bytearray(carsel.HEADER) + bytes([1]) + struct.pack('<QQ', 0x9000123456789, 7) + bytes(range(16, 32)) + bytes([1, 0]) + bytes(4)
struct.pack_into('<H', payload, 9, carsel.REQUEST_SIZE)
request = bytes.fromhex(pathlib.Path(sys.argv[1]).read_text().strip())
key = bytes(range(32))
assert request == carsel.lighting.signed(bytes(payload), key), 'C request differs from the server wire format'
hold = bool(int(sys.argv[3]))
left = None if sys.argv[4] == 'none' else int(sys.argv[4])
me = NS(addr=('a', 1), xuid=0x9000123456789, owner_id=9, car_id=5, connected=True, entered=True)
world = NS(peers={me.addr: me}, carsel_answer=lambda peer: (hold, left))
sent = []
hub = NS(route={me.addr: world}, revoked={}, bound={me.addr: (me.xuid, 1 << 60, key)}, keys={me.xuid: {key: 1 << 60}},
         ticket_epochs={me.addr: bytes(range(16, 32))}, sock=NS(sendto=lambda d, a: sent.append(d)), log=lambda *_: None)
carsel.handle(hub, request, ('aux', 1))
assert len(sent) == 1, 'the server did not answer'
pathlib.Path(sys.argv[2]).write_text(sent[0].hex() + '\n')
PY
$RUN "$work/test.exe" --reply "$work/reply$tag" "$1" "$2"
done
