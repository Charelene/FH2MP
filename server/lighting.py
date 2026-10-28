"""Authenticated, opt-in visual lighting state alongside unchanged NerdLib packets.

Only a live, ticket-bound game connection can publish. No game objects are created;
records name the relay owner/car id already observed in native predictions.
"""
import hashlib
import hmac
import struct
import time

MAGIC = b'FH2MPLT1'
HEADER = bytes.fromhex('fffffeff00000000000000') + MAGIC
BASE = 55
MAC_SIZE = 32
REQUEST_SIZE = BASE + MAC_SIZE
MAX_RECORDS = 12
LEASE_SECONDS = 2.0
MIN_INTERVAL = 0.08


def signed(payload, key):
    return payload + hmac.new(key, payload, hashlib.sha256).digest()


def handle(hub, data, addr):
    """Consume a lighting request. Called before native parsing, never forwarded."""
    if len(data) != REQUEST_SIZE or data[:9] != HEADER[:9] or data[11:19] != MAGIC:
        return
    if int.from_bytes(data[9:11], 'little') != len(data) or data[19] != 1 or data[54] or data[52] > 127 or data[53] > 7:
        return
    xuid, seq = struct.unpack_from('<QQ', data, 20)
    now = time.monotonic()
    if not seq or xuid in hub.revoked:
        return
    # The auxiliary socket has its own NAT mapping. Authenticate against the key
    # and epoch of the player's existing native connection, not its claimed IP.
    found = None
    for main_addr, world in hub.route.items():
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
        break
    if found is None:
        return
    world, peer, key = found
    old = getattr(peer, 'lighting', None)
    epoch = data[36:52]
    if old and old['epoch'] == epoch and (seq <= old['seq'] or now - old['at'] < MIN_INTERVAL):
        return
    peer.lighting = {'epoch': epoch, 'seq': seq, 'at': now, 'flags': data[53], 'car': peer.car_id}
    if not old or old['flags'] != data[53] or old['car'] != peer.car_id:
        getattr(hub, 'log', lambda *_: None)(f"lighting: owner {peer.owner_id} car {peer.car_id} headlights={data[53] & 1} signal={data[53] >> 1}")
    records = []
    for other in world.others(peer):
        state = getattr(other, 'lighting', None)
        binding = hub.bound.get(other.addr)
        if (not state or now - state['at'] > LEASE_SECONDS or state['car'] != other.car_id
                or not other.connected or not other.entered or not 1 <= other.owner_id <= 254
                or not binding or binding[0] != other.xuid or other.xuid in hub.revoked
                or hub.keys.get(other.xuid, {}).get(binding[2], 0) <= now
                or hub.ticket_epochs.get(other.addr) != state['epoch']):
            continue
        records.append(bytes((other.owner_id, state['car'], state['flags'], 0)))
        if len(records) == MAX_RECORDS:
            break
    reply = bytearray(data[:BASE])
    reply[19], reply[54] = 2, len(records)
    reply.extend(b''.join(records))
    struct.pack_into('<H', reply, 9, len(reply) + MAC_SIZE)
    hub.sock.sendto(signed(reply, key), addr)
