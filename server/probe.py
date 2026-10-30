"""UDP reachability probe on the game port: the launcher sends one datagram before Play and the server sends the same bytes back. A client that
gets no answer in three tries has a network that drops UDP, and the launcher switches that start to the TCP carrier (tunnel.py) by itself.

Prod 2026-10-29 (#242): "infinite loading when starting a race" - the server never saw the request; blocked or partial UDP. TCP existed but
only as an opt-in checkbox nobody found. The reply is exactly as long as the request, so the probe cannot amplify, and each address gets a few
per minute. Format: the game's 11-byte datagram header (ignored, any bytes) + MAGIC + 8 bytes the client chose."""
import time

MAGIC = b"FH2MPPN1"
LENGTH = 11 + len(MAGIC) + 8
PER_WINDOW = 6            # answers per address per WINDOW seconds
WINDOW = 60.0
MAX_ADDRESSES = 4096      # bounded memory: the table is cleared when it fills up
_seen = {}


def handle(hub, data, addr, now=None):
    """Answers a probe; True when it was one (answered or not), so the caller does not parse it as a game datagram."""
    if len(data) != LENGTH or data[11:19] != MAGIC:
        return False
    tunnel = getattr(hub, "tunnel", None)
    if tunnel is not None and addr in getattr(tunnel, "conns", {}):
        return True          # a TCP client's pseudo-address: nothing to answer over UDP
    now = time.monotonic() if now is None else now
    start, count = _seen.get(addr, (now, 0))
    if now - start >= WINDOW:
        start, count = now, 0
    if count >= PER_WINDOW:
        return True
    if len(_seen) >= MAX_ADDRESSES:
        _seen.clear()
    _seen[addr] = (start, count + 1)
    try:
        hub.sock.sendto(data, addr)
    except OSError:
        pass
    return True
