"""Made the frozen side-channel vectors in this folder from the Python server modules (server/lighting.py, honk.py, carsel.py) on 2026-11-07,
right before the Python servers were removed. The C tests (test_lighting.sh, test_honk.sh, test_carsel.sh) compare the request the shim builds
with *_request.hex and feed *_reply*.hex to the C parser. The Go server (native/internal/game/side.go, carsel.go) keeps the same wire format;
when it changes, regenerate these from the Go code (a `go run` helper) and say so here. This script no longer runs: it needs server/*.py.

    python -I shim/tests/vectors/make_vectors.py <repo root with server/>
"""
import pathlib
import struct
import sys
import time
from types import SimpleNamespace as NS

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
sys.path.insert(0, str(root / "server"))
import carsel  # noqa: E402
import honk  # noqa: E402
import lighting  # noqa: E402

out = pathlib.Path(__file__).resolve().parent
key = bytes(range(32))
xuid = 0x9000123456789
epoch = bytes(range(16, 32))

# lighting: the request test_lighting.c builds with --vector (identity xuid, seq 7, epoch 16..31, car 5, flags 7)
payload = bytearray(lighting.HEADER) + bytes([1]) + struct.pack("<QQ", xuid, 7) + epoch + bytes([5, 7, 0])
struct.pack_into("<H", payload, 9, lighting.REQUEST_SIZE)
(out / "lighting_request.hex").write_text(lighting.signed(payload, key).hex() + "\n")

# honk: the request test_honk.c builds, and the server's reply to it with one other car honking (owner 10, car 6, horn 7, press 300, started 200 ms ago,
# at (10, 20, -30)); time pinned so the age field is exactly 200 ms
T = 1000.0
time.monotonic = lambda: T
payload = bytearray(honk.HEADER) + bytes([1]) + struct.pack("<QQ", xuid, 7) + epoch + bytes([5, honk.HORN_OFF, 0, 0, 0])
struct.pack_into("<H", payload, 9, honk.REQUEST_SIZE)
request = honk.lighting.signed(payload, key)
(out / "honk_request.hex").write_text(request.hex() + "\n")
me = NS(addr=("a", 1), xuid=xuid, owner_id=9, car_id=5, connected=True, entered=True, pos=(0.0, 0.0, 0.0))
other = NS(addr=("b", 2), xuid=2, owner_id=10, car_id=6, connected=True, entered=True, pos=(10.0, 20.0, -30.0),
           honk={"epoch": b"b" * 16, "seq": 1, "at": T, "car": 6, "horn": 7, "horn_asked": 7, "press": 300,
                 "started": T - 0.2, "blocked_until": 0.0, "capped": None, "cap_hit": False})
world = NS(peers={me.addr: me, other.addr: other})
world.others = lambda p: [q for q in world.peers.values() if q is not p]
sent = []
hub = NS(route={me.addr: world, other.addr: world}, revoked={}, bound={me.addr: (xuid, 1 << 60, key), other.addr: (2, 1 << 60, b"k" * 32)},
         keys={xuid: {key: 1 << 60}, 2: {b"k" * 32: 1 << 60}}, ticket_epochs={me.addr: epoch, other.addr: b"b" * 16},
         sock=NS(sendto=lambda d, a: sent.append(d)), log=lambda *_: None)
honk.handle(hub, request, ("aux", 1))
assert len(sent) == 1, "the server did not answer the honk request"
(out / "honk_reply.hex").write_text(sent[0].hex() + "\n")

# carsel: the request test_carsel.c builds (state 1: in the cars-loaded wait), and the four answers the test feeds back
payload = bytearray(carsel.HEADER) + bytes([1]) + struct.pack("<QQ", xuid, 7) + epoch + bytes([1, 0]) + bytes(4)
struct.pack_into("<H", payload, 9, carsel.REQUEST_SIZE)
request = carsel.lighting.signed(bytes(payload), key)
(out / "carsel_request.hex").write_text(request.hex() + "\n")
for hold, left in ((1, 5000), (0, None), (1, None), (0, 0)):
    me = NS(addr=("a", 1), xuid=xuid, owner_id=9, car_id=5, connected=True, entered=True)
    world = NS(peers={me.addr: me}, carsel_answer=lambda peer, h=hold, l=left: (bool(h), l))
    sent = []
    hub = NS(route={me.addr: world}, revoked={}, bound={me.addr: (xuid, 1 << 60, key)}, keys={xuid: {key: 1 << 60}},
             ticket_epochs={me.addr: epoch}, sock=NS(sendto=lambda d, a: sent.append(d)), log=lambda *_: None)
    carsel.handle(hub, request, ("aux", 1))
    assert len(sent) == 1, "the server did not answer the car select request"
    tag = f"{hold}-{'none' if left is None else left}"
    (out / f"carsel_reply_{tag}.hex").write_text(sent[0].hex() + "\n")
print("wrote", sorted(p.name for p in out.glob("*.hex")))
