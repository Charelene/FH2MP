#!/usr/bin/env bash
# Quick chat side channel (src/quickchat_wire.h, src/quickchat_sync.inc): the wire format against server/quickchat.py, every refusal of a
# reply, and finding QuickChat's bridge through the platform, all without the game.
set -euo pipefail
cd "$(dirname "$0")"
work=$(mktemp -d)
trap 'WINEPREFIX="$work/prefix" wineserver -k >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
. ./wine_prefix.sh; wine_prefix "$work/prefix"
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -Wno-unused-function -Wno-cast-function-type -I ../third_party/fh2-mod-platform test_quickchat.c \
    -o "$work/test.exe" -lws2_32
mkdir -p "$work/prefix"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe"
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe" --vector > "$work/vector"
python3 -I - "$work/vector" "$work/reply" <<'PY'
import sys, pathlib, struct, time
from types import SimpleNamespace as NS
sys.path.insert(0, str(pathlib.Path.cwd().parent / 'server'))
import quickchat
# the request the C code built is accepted by the server, and the server's reply is what the C code accepts
payload = bytearray(quickchat.HEADER) + bytes([1]) + struct.pack('<QQ', 0x9000123456789, 7) + bytes(range(16, 32)) + struct.pack('<IBB', 3, 2, 0)
struct.pack_into('<H', payload, 9, quickchat.REQUEST_SIZE)
request = bytes.fromhex(pathlib.Path(sys.argv[1]).read_text().strip())
key = bytes(range(32))
assert request == quickchat.lighting.signed(payload, key), 'C request differs from the server wire format'
now = time.monotonic()
me = NS(addr=('a', 1), xuid=0x9000123456789, owner_id=9, connected=True, entered=True)
other = NS(addr=('b', 2), xuid=2, owner_id=10, connected=True, entered=True,
           quickchat={'epoch': b'b' * 16, 'seq': 1, 'at': now, 'number': 4, 'phrase': 5, 'taken_at': now - 0.25, 'new': False})
world = NS(peers={me.addr: me, other.addr: other})
world.others = lambda p: [q for q in world.peers.values() if q is not p]
sent = []
hub = NS(route={me.addr: world, other.addr: world}, revoked={}, bound={me.addr: (me.xuid, 1 << 60, key), other.addr: (2, 1 << 60, b'k' * 32)},
         keys={me.xuid: {key: 1 << 60}, 2: {b'k' * 32: 1 << 60}}, ticket_epochs={me.addr: bytes(range(16, 32)), other.addr: b'b' * 16},
         gamertags={2: "Dusk Carton"}, sock=NS(sendto=lambda d, a: sent.append(d)), log=lambda *_: None)
quickchat.handle(hub, request, ('aux', 1))
assert len(sent) == 1
pathlib.Path(sys.argv[2]).write_text(sent[0].hex() + '\n')
PY
WINEPREFIX="$work/prefix" WINEDEBUG=-all wine "$work/test.exe" --reply "$work/reply"
