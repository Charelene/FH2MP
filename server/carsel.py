"""Authenticated "keep waiting?" answer for the Free Roam event setup (the same side channel as honk.py / lighting.py, own magic).

A client that has picked its car early waits on the loading screen ("Waiting for Players m:ss") until every participant has left the event car select,
so everybody starts loading together (the shim holds the per-frame update of the state flow's NetworkWaitForCarsLoaded behaviour while the answer says hold).
The shim asks about once a second, every 100 ms while it sits in that wait or in a car select countdown; the answer is state, not an event: a lost
datagram heals on the next one, and no fresh answer means the shim lets the game go on. Besides hold / release it carries the ms left to the group's
car select deadline, which the shims write into the game's countdowns (all players see the same timer, and 'Waiting for Players m:ss' on the loading
screen counts to it). The server decides in Server.carsel_answer. FH2MP_NO_CARSEL=1 turns it off.
"""
import hashlib
import hmac
import itertools
import os
import struct
import time

import diag
import lighting

LOG = diag.get("carsel")
_RATE = diag.RateLimit()
REJECT_LOG_SECONDS = 60.0

MAGIC = b'FH2MPCS2'
HEADER = bytes.fromhex('fffffeff00000000000000') + MAGIC
BASE = 58                  # header 19, type 1, xuid 8, seq 8, epoch 16, state 1 (bit 0: in the cars-loaded wait, bit 1: in a car select countdown), reserved 1,
                           # u32 (request: 0; reply: ms left to the group's deadline, NO_DEADLINE = none)
MAC_SIZE = 32
REQUEST_SIZE = BASE + MAC_SIZE
MIN_INTERVAL = 0.03
NO_DEADLINE = 0xFFFFFFFF


def _enabled():
    return not os.environ.get("FH2MP_NO_CARSEL")


def handle(hub, data, addr):
    """Consume a request. Called before native parsing, never forwarded."""
    if len(data) != REQUEST_SIZE or data[:9] != HEADER[:9] or data[11:19] != MAGIC or not _enabled():
        return
    if int.from_bytes(data[9:11], 'little') != len(data) or data[19] != 1 or data[52] > 3 or data[53] or data[54:58] != bytes(4):
        ok, skipped = _RATE.allow("malformed", REJECT_LOG_SECONDS)
        if ok:
            LOG.warn(f"request with the car select magic is malformed and ignored: length field {int.from_bytes(data[9:11], 'little')} of {len(data)} bytes, "
                     f"type {data[19]} (expected 1), state {data[52]} (max 3), reserved bytes {data[53]} and {data[54:58].hex()} (expected 0)"
                     + (f" (+{skipped} more since the last line)" if skipped else ""))
        return
    xuid, seq = struct.unpack_from('<QQ', data, 20)
    now = time.monotonic()
    if not seq or xuid in hub.revoked:
        return
    # The auxiliary socket has its own NAT mapping: authenticate against the key and epoch of the player's native connection, not its claimed IP.
    routes = getattr(hub, 'carsel_routes', None)
    if routes is None:
        routes = hub.carsel_routes = {}
    last = routes.get(xuid)
    candidates = ((last, hub.route[last]),) if last in hub.route else ()
    found = None
    for main_addr, world in itertools.chain(candidates, hub.route.items()):
        binding = hub.bound.get(main_addr)
        peer = world.peers.get(main_addr)
        if not binding or binding[0] != xuid or not peer or not peer.connected or not peer.entered or peer.xuid != xuid or not 1 <= peer.owner_id <= 254:
            continue
        key = binding[2]
        if (hub.keys.get(xuid, {}).get(key, 0) <= now
                or hub.ticket_epochs.get(main_addr) != data[36:52]
                or not hmac.compare_digest(hmac.new(key, data[:-MAC_SIZE], hashlib.sha256).digest(), data[-MAC_SIZE:])):
            continue
        found = world, peer, key
        routes[xuid] = main_addr
        break
    if found is None:
        routes.pop(xuid, None)
        ok, skipped = _RATE.allow(("unmatched", xuid), REJECT_LOG_SECONDS)
        if ok:
            LOG.debug(f"request from xuid {xuid} ignored: no live game connection of that player matches its address binding, ticket epoch and signature"
                      + (f" (+{skipped} more since the last line)" if skipped else ""))
        return
    world, peer, key = found
    epoch = data[36:52]
    old = getattr(peer, 'carsel', None)
    if old and old[0] == epoch and (seq <= old[1] or now - old[2] < MIN_INTERVAL):
        return
    peer.carsel = (epoch, seq, now)
    reply = bytearray(data[:BASE])
    hold, left = world.carsel_answer(peer)
    reply[19], reply[52] = 2, 1 if hold else 0
    struct.pack_into('<I', reply, 54, NO_DEADLINE if left is None else min(left, NO_DEADLINE - 1))
    hub.sock.sendto(lighting.signed(bytes(reply), key), addr)
