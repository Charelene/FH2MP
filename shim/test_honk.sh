#!/usr/bin/env bash
# Horn side channel (src/honk_wire.h, src/honk_sync.inc): the wire format against server/honk.py, the bank reader, the remote voices with a
# recording FMOD fake and the left->right stick change of HonkHonk's button, all without the game.
set -euo pipefail
cd "$(dirname "$0")"
work=$(mktemp -d)
trap 'WINEPREFIX="$work/prefix" wineserver -k >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
. ./wine_prefix.sh; wine_prefix "$work/prefix"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-function -Wno-cast-function-type test_honk.c -o "$work/test.exe" -lws2_32
mkdir -p "$work/prefix"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe" --vector > "$work/vector"
python3 -I - "$work/vector" "$work/reply" <<'PY'
import sys, pathlib, struct, time
from types import SimpleNamespace as NS
sys.path.insert(0, str(pathlib.Path.cwd().parent / 'server'))
import honk
# the request the C code built is accepted by the server, and the server's reply is what the C code accepts
payload = bytearray(honk.HEADER) + bytes([1]) + struct.pack('<QQ', 0x9000123456789, 7) + bytes(range(16, 32)) + bytes([5, honk.HORN_OFF, 0, 0, 0])
struct.pack_into('<H', payload, 9, honk.REQUEST_SIZE)
request = bytes.fromhex(pathlib.Path(sys.argv[1]).read_text().strip())
key = bytes(range(32))
assert request == honk.lighting.signed(payload, key), 'C request differs from the server wire format'
me = NS(addr=('a', 1), xuid=0x9000123456789, owner_id=9, car_id=5, connected=True, entered=True, pos=(0.0, 0.0, 0.0))
other = NS(addr=('b', 2), xuid=2, owner_id=10, car_id=6, connected=True, entered=True, pos=(10.0, 20.0, -30.0),
           honk={'epoch': b'b' * 16, 'seq': 1, 'at': time.monotonic(), 'car': 6, 'horn': 7, 'horn_asked': 7, 'press': 300,
                 'started': time.monotonic() - 0.2, 'blocked_until': 0.0, 'capped': None, 'cap_hit': False})
world = NS(peers={me.addr: me, other.addr: other})
world.others = lambda p: [q for q in world.peers.values() if q is not p]
sent = []
hub = NS(route={me.addr: world, other.addr: world}, revoked={}, bound={me.addr: (me.xuid, 1 << 60, key), other.addr: (2, 1 << 60, b'k' * 32)},
         keys={me.xuid: {key: 1 << 60}, 2: {b'k' * 32: 1 << 60}}, ticket_epochs={me.addr: bytes(range(16, 32)), other.addr: b'b' * 16},
         sock=NS(sendto=lambda d, a: sent.append(d)), log=lambda *_: None)
honk.handle(hub, request, ('aux', 1))
assert len(sent) == 1
pathlib.Path(sys.argv[2]).write_text(sent[0].hex() + '\n')
PY
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe" --reply "$work/reply"
