"""Quick chat: authenticated quick-chat state alongside unchanged NerdLib packets (the same side channel as honk.py, own magic).

A client says what the newest message its player sent is ("message 4, phrase 2"), never an event: it repeats that until the reply shows it
was taken, so a lost datagram heals on the next one. The reply lists the live messages of the other players of the world (of the same event,
when in one), each with its sender's gamertag, so every client shows each (player, message) once. Only a phrase number crosses the network:
every client has the same fixed list (the QuickChat mod), so there is no free text to check. FH2MP_NO_QUICKCHAT=1 turns it off.
"""
import hashlib
import hmac
import itertools
import os
import struct
import time

import diag
import lighting

LOG = diag.get("quickchat")
_RATE = diag.RateLimit()
REJECT_LOG_SECONDS = 60.0

MAGIC = b'FH2MPQC1'
HEADER = bytes.fromhex('fffffeff00000000000000') + MAGIC
BASE = 58                  # header 19, type 1, xuid 8, seq 8, epoch 16, message number u32, phrase 1, reserved / reply count 1
MAC_SIZE = 32
REQUEST_SIZE = BASE + MAC_SIZE
NAME_SIZE = 48             # a gamertag in UTF-8, zero-padded
RECORD_SIZE = 12 + NAME_SIZE   # owner, phrase, name length, reserved, message number u32, age ms u16, reserved u16, name
MAX_RECORDS = 12
PHRASES = 64               # phrase numbers 0-63 are accepted: QuickChat has 16, and new ones only ever go at the end
LEASE_SECONDS = 3.0        # a player whose client stopped asking for this long has no live message
MESSAGE_SECONDS = 10.0     # a message is passed on for this long after it arrived
MIN_GAP_SECONDS = 1.0      # a new message sooner than this after the last one is ignored until it may be taken
MIN_INTERVAL = 0.03
LOG_EVERY = 10.0


def _enabled():
    return not os.environ.get("FH2MP_NO_QUICKCHAT")


def _live(hub, other, now):
    """The other player's newest message, if it may be passed on: (message number, phrase, age s) or None."""
    state = getattr(other, 'quickchat', None)
    binding = hub.bound.get(other.addr)
    if (state is None or not state['number'] or now - state['at'] > LEASE_SECONDS or now - state['taken_at'] > MESSAGE_SECONDS
            or not other.connected or not other.entered or not 1 <= other.owner_id <= 254
            or binding is None or binding[0] != other.xuid or other.xuid in hub.revoked
            or hub.keys.get(other.xuid, {}).get(binding[2], 0) <= now
            or hub.ticket_epochs.get(other.addr) != state['epoch']):
        return None
    return state['number'], state['phrase'], now - state['taken_at']


def _step(old, epoch, seq, now, number, phrase):
    """The player's state after one request (old = the previous state or None): `number` and `phrase` are the newest message taken, which may
    lag what the client asks for while it is too soon after the last one."""
    fresh = bool(old) and old['epoch'] == epoch
    state = {'epoch': epoch, 'seq': seq, 'at': now,
             'number': old['number'] if fresh else 0, 'phrase': old['phrase'] if fresh else 0, 'taken_at': old['taken_at'] if fresh else -1e18,
             'new': False}
    if number and number != state['number'] and now - state['taken_at'] >= MIN_GAP_SECONDS:
        state.update(number=number, phrase=phrase, taken_at=now, new=True)
    return state


def _name(hub, xuid):
    """The gamertag the account service announced for `xuid` ("name" on the control port), as at most NAME_SIZE bytes of UTF-8."""
    tag = (getattr(hub, 'gamertags', None) or {}).get(xuid, "")
    raw = tag.encode('utf-8')[:NAME_SIZE]
    while raw:                                  # never half a character
        try:
            raw.decode('utf-8')
            break
        except UnicodeDecodeError:
            raw = raw[:-1]
    return raw


def _why_unmatched(hub, xuid, data, now):
    """As lighting._why_unmatched, without its car check (a quick-chat request names no car): (why, expected around a join)."""
    for main_addr, binding in hub.bound.items():
        if binding[0] != xuid:
            continue
        world = hub.route.get(main_addr)
        peer = world.peers.get(main_addr) if world is not None else None
        if peer is None:
            return "its game connection is in no world", True
        if not peer.connected or not peer.entered:
            return "its game connection has not entered the world yet" if peer.connected else "its game connection is gone", True
        if peer.xuid != xuid or not 1 <= peer.owner_id <= 254:
            return f"the connection's xuid is {peer.xuid} and its owner id {peer.owner_id}", False
        if hub.keys.get(xuid, {}).get(binding[2], 0) <= now:
            return "the ticket key has expired", False
        if hub.ticket_epochs.get(main_addr) != data[36:52]:
            return "the ticket epoch differs from the one of the game connection (the client holds an older or newer ticket)", False
        return "the signature does not match the ticket key", False
    return "no game connection of this player is bound to a ticket", False


def handle(hub, data, addr):
    """Consume a quick-chat request. Called before native parsing, never forwarded."""
    if len(data) != REQUEST_SIZE or data[:9] != HEADER[:9] or data[11:19] != MAGIC or not _enabled():
        return
    if int.from_bytes(data[9:11], 'little') != len(data) or data[19] != 1 or data[57] or data[56] >= PHRASES:
        ok, skipped = _RATE.allow("malformed", REJECT_LOG_SECONDS)
        if ok:
            LOG.warn(f"request with the quick-chat magic is malformed and ignored: length field {int.from_bytes(data[9:11], 'little')} of "
                     f"{len(data)} bytes, type {data[19]} (expected 1), reserved byte {data[57]} (expected 0), phrase {data[56]} (max {PHRASES - 1})"
                     + (f" (+{skipped} more since the last line)" if skipped else ""))
        return
    xuid, seq = struct.unpack_from('<QQ', data, 20)
    now = time.monotonic()
    if not seq or xuid in hub.revoked:
        return
    # The auxiliary socket has its own NAT mapping: authenticate against the key and epoch of the player's native connection, not its claimed IP.
    routes = getattr(hub, 'quickchat_routes', None)
    if routes is None:
        routes = hub.quickchat_routes = {}
    last = routes.get(xuid)
    candidates = ((last, hub.route[last]),) if last in hub.route else ()
    found = None
    for main_addr, world in itertools.chain(candidates, hub.route.items()):
        binding = hub.bound.get(main_addr)
        peer = world.peers.get(main_addr)
        if (not binding or binding[0] != xuid or not peer or not peer.connected or not peer.entered
                or peer.xuid != xuid or not 1 <= peer.owner_id <= 254):
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
            why, expected = _why_unmatched(hub, xuid, data, now)
            (LOG.debug if expected else LOG.warn)(f"request from xuid {xuid} ignored: {why}" + (f" (+{skipped} more since the last line)" if skipped else ""))
        return
    world, peer, key = found
    epoch = data[36:52]
    old = getattr(peer, 'quickchat', None)
    if old and old['epoch'] == epoch and (seq <= old['seq'] or now - old['at'] < MIN_INTERVAL):
        return
    number = struct.unpack_from('<I', data, 52)[0]
    peer.quickchat = new = _step(old, epoch, seq, now, number, data[56])
    if new['new'] and now - getattr(peer, 'quickchat_logged_at', -LOG_EVERY) >= LOG_EVERY:
        peer.quickchat_logged_at = now
        getattr(hub, 'log', lambda *_: None)(f"quickchat: owner {peer.owner_id} message {new['number']} phrase {new['phrase']}")
    same_event = getattr(world, 'same_event', None)
    records = []
    for other in world.others(peer):
        live = _live(hub, other, now)
        if live is None or (same_event is not None and not same_event(peer, other)):
            continue
        msg, phrase, age = live
        name = _name(hub, other.xuid)
        records.append(struct.pack('<BBBBIHH', other.owner_id, phrase, len(name), 0, msg, min(0xFFFF, int(age * 1000)), 0)
                       + name.ljust(NAME_SIZE, b'\0'))
        if len(records) == MAX_RECORDS:
            break
    reply = bytearray(data[:BASE])
    reply[19] = 2
    struct.pack_into('<IB', reply, 52, new['number'], new['phrase'])   # what was taken: the client stops waiting for its message once it's here
    reply[57] = len(records)
    reply.extend(b''.join(records))
    struct.pack_into('<H', reply, 9, len(reply) + MAC_SIZE)
    hub.sock.sendto(lighting.signed(reply, key), addr)
