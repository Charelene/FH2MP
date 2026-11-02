"""Authenticated horn state alongside unchanged NerdLib packets (the same side channel as lighting.py, own magic).

A client says what its horn is doing now ("horn 7 held, press 12"), never an event: a lost datagram heals on the next one and a lost
"stop" costs one lease. The reply lists the horns held by the other players of the world, with the car's last position. Only an index
into the HonkHonk bank crosses the network (every client has the pinned bank); nothing is loaded from it. FH2MP_NO_HONK=1 turns it off.
"""
import hashlib
import hmac
import itertools
import math
import os
import struct
import time

import diag
import lighting

LOG = diag.get("honk")
_RATE = diag.RateLimit()
REJECT_LOG_SECONDS = 60.0

MAGIC = b'FH2MPHN1'
HEADER = bytes.fromhex('fffffeff00000000000000') + MAGIC
BASE = 57                  # header 19, type 1, xuid 8, seq 8, epoch 16, car 1, horn 1, reserved 1, press id 2
MAC_SIZE = 32
REQUEST_SIZE = BASE + MAC_SIZE
RECORD_SIZE = 20           # owner, car, horn, flags (bit0 position valid), press id u16, age ms u16, x y z f32
MAX_RECORDS = 12
HORN_OFF = 0xFF
HORNS = 31                 # sounds in HonkHonk 0.3.4's bank; an index beyond it is refused
LEASE_SECONDS = 0.5        # a horn that was not refreshed for this long is off
CAP_SECONDS = 10.0         # longest press; the receiver applies the same cap
COOLDOWN_SECONDS = 2.0     # after a capped press, a new one is ignored for this long
MIN_INTERVAL = 0.03
MAX_DISTANCE = 400.0       # beyond the engine's rolloff (150 m) with margin; skipped when either position is unknown
LOG_EVERY = 10.0


def _enabled():
    return not os.environ.get("FH2MP_NO_HONK")


def _state_live(hub, other, now):
    state = getattr(other, 'honk', None)
    binding = hub.bound.get(other.addr)
    return (state is not None and state['horn'] is not None and now - state['at'] <= LEASE_SECONDS and state['car'] == other.car_id
            and other.connected and other.entered and 1 <= other.owner_id <= 254
            and binding is not None and binding[0] == other.xuid and other.xuid not in hub.revoked
            and hub.keys.get(other.xuid, {}).get(binding[2], 0) > now
            and hub.ticket_epochs.get(other.addr) == state['epoch'])


def _step(old, epoch, seq, now, horn, press, car):
    """The new state of a player's horn from one request (old = the previous state or None). `horn` of the result is what the others may hear."""
    fresh = bool(old) and old['epoch'] == epoch
    blocked_until = old['blocked_until'] if fresh else 0.0
    started = capped = None
    cap_hit = False
    allowed = None
    if horn != HORN_OFF:
        if fresh and old['horn_asked'] != HORN_OFF and old['press'] == press:      # the same press, still held
            started, capped = old['started'], old['capped']
        else:                                                                       # a new press, ignored during the cooldown after a capped one
            started, capped = now, (press if now < blocked_until else None)
        if capped != press and now - started > CAP_SECONDS:
            capped, blocked_until, cap_hit = press, now + COOLDOWN_SECONDS, True
        if capped != press:
            allowed = horn
    return {'epoch': epoch, 'seq': seq, 'at': now, 'car': car, 'horn': allowed, 'horn_asked': horn, 'press': press,
            'started': started, 'blocked_until': blocked_until, 'capped': capped, 'cap_hit': cap_hit}


def handle(hub, data, addr):
    """Consume a horn request. Called before native parsing, never forwarded."""
    if len(data) != REQUEST_SIZE or data[:9] != HEADER[:9] or data[11:19] != MAGIC or not _enabled():
        return
    if (int.from_bytes(data[9:11], 'little') != len(data) or data[19] != 1 or data[54] or data[52] > 127
            or (data[53] != HORN_OFF and data[53] >= HORNS)):
        ok, skipped = _RATE.allow("malformed", REJECT_LOG_SECONDS)
        if ok:
            LOG.warn(f"request with the horn magic is malformed and ignored: length field {int.from_bytes(data[9:11], 'little')} of {len(data)} bytes, "
                     f"type {data[19]} (expected 1), reserved byte {data[54]} (expected 0), car {data[52]} (max 127), horn {data[53]} (max {HORNS - 1} or {HORN_OFF})"
                     + (f" (+{skipped} more since the last line)" if skipped else ""))
        return
    xuid, seq = struct.unpack_from('<QQ', data, 20)
    now = time.monotonic()
    if not seq or xuid in hub.revoked:
        return
    # The auxiliary socket has its own NAT mapping: authenticate against the key and epoch of the player's native connection, not its claimed IP.
    routes = getattr(hub, 'honk_routes', None)
    if routes is None:
        routes = hub.honk_routes = {}
    last = routes.get(xuid)
    candidates = ((last, hub.route[last]),) if last in hub.route else ()
    found = None
    for main_addr, world in itertools.chain(candidates, hub.route.items()):
        binding = hub.bound.get(main_addr)
        peer = world.peers.get(main_addr)
        if (not binding or binding[0] != xuid or not peer or not peer.connected or not peer.entered
                or peer.xuid != xuid or peer.car_id != data[52] or not 1 <= peer.owner_id <= 254):
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
            why, expected = lighting._why_unmatched(hub, xuid, data, now)
            (LOG.debug if expected else LOG.warn)(f"request from xuid {xuid} ignored: {why}" + (f" (+{skipped} more since the last line)" if skipped else ""))
        return
    world, peer, key = found
    epoch = data[36:52]
    old = getattr(peer, 'honk', None)
    if old and old['epoch'] == epoch and (seq <= old['seq'] or now - old['at'] < MIN_INTERVAL):
        return
    press = struct.unpack_from('<H', data, 55)[0]
    peer.honk = new = _step(old, epoch, seq, now, data[53], press, peer.car_id)
    started_now = new['horn'] is not None and not (old and old['epoch'] == epoch and old['horn'] is not None and old['press'] == press)
    if started_now and now - getattr(peer, 'honk_logged_at', -LOG_EVERY) >= LOG_EVERY:
        peer.honk_logged_at = now
        getattr(hub, 'log', lambda *_: None)(f"honk: owner {peer.owner_id} car {peer.car_id} horn {new['horn']} press {press}")
    if new['cap_hit']:
        getattr(hub, 'log', lambda *_: None)(f"honk: owner {peer.owner_id} held press {press} for {CAP_SECONDS:g} s, silenced; next one allowed {COOLDOWN_SECONDS:g} s after")
    same_event = getattr(world, 'same_event', None)
    records = []
    for other in world.others(peer):
        if not _state_live(hub, other, now) or (same_event is not None and not same_event(peer, other)):
            continue
        state = other.honk
        pos, mine = getattr(other, 'pos', None), getattr(peer, 'pos', None)
        known = (pos is not None and all(math.isfinite(c) for c in pos))
        if known and mine is not None and all(math.isfinite(c) for c in mine) and math.dist(pos, mine) > MAX_DISTANCE:
            continue
        age = min(0xFFFF, int((now - state['started']) * 1000)) if state['started'] is not None else 0
        records.append(struct.pack('<BBBBHH3f', other.owner_id, state['car'], state['horn'], 1 if known else 0, state['press'], age,
                                   *(pos if known else (0.0, 0.0, 0.0))))
        if len(records) == MAX_RECORDS:
            break
    reply = bytearray(data[:BASE])
    reply[19], reply[54] = 2, len(records)
    reply.extend(b''.join(records))
    struct.pack_into('<H', reply, 9, len(reply) + MAC_SIZE)
    hub.sock.sendto(lighting.signed(reply, key), addr)
