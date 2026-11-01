"""Server side of the game's reliable packets (Nerd::Connection), so one lost datagram no longer freezes an object.

The client applies object messages strictly in sequence and never skips a gap (PendingObjectMessages::ProcessInOrder 0x17b1e70), so a relayed
create or update that is lost stops that object on that client for the rest of the session. The game's own sender rules:
  a reliable packet carries id (last + 1) % 0xFFFF, 0xFFFF means unreliable (0x17672b0 drops an id equal to the last one received or more than
  0x3ffe away, and the rest of that frame's packets with it, so an id is never repeated);
  every incoming packet acks id == remoteAck, and remoteAck - 1 - k for each bit k of ackBits (0x1766400, n5 1.2);
  a packet older than rtt + 2000 ms is dissolved and its reliable messages go out again in a new packet with a new id (0x1769e30).
A resent update the client already has is dropped as old by its sequence check. A resent create with no newer update recreates the object (0x17ab160),
which is why nothing is resent to a client that has gone quiet: it may be loading and still hold the original."""

import diag

LOG = diag.get("reliable")
_RATE = diag.RateLimit()
GIVE_UP_LOG_SECONDS = 10.0

ID_WRAP = 0xFFFF
UNRELIABLE_ID = 0xFFFF
RESEND_SECONDS = 2.2         # the client's rtt + 2000 ms; there is no RTT here yet
ACK_GRACE_SECONDS = 0.5      # the client must have spoken this long after the packet left before silence about it counts as a loss
MAX_PENDING = 1024           # packets per client; beyond this the oldest are given up and counted

# Never stored: the handshake resends itself (0, 1, 2), the client's categories 4 and 5 are never resent (3, 4, 15, 16, 17, 20, 21, 24), and TimeSync (5), 18 and 19
# are superseded by the next one.
UNRELIABLE_MESSAGES = frozenset({0, 1, 2, 3, 4, 5, 15, 16, 17, 18, 19, 20, 21, 24})
_KEEP = tuple(i not in UNRELIABLE_MESSAGES for i in range(128))
OBJECT_MESSAGES = (7, 8)
CREATE = 7


def relay_owner(baked, router_bits):
    """The relay owner named by a baked create or update (A=0, B=1: id, then owner:8), or None for anything else."""
    value = baked[0]
    if value & 0x7F not in OBJECT_MESSAGES or (value >> 11) & 3 != 2:
        return None
    bits = router_bits.get((value >> 7) & 0xF)
    return None if bits is None else (value >> (13 + bits[0])) & 0xFF


class Window:
    """Reliable packets sent to one client and not acked yet. `pending` is in send order: id -> (time sent, baked messages)."""
    __slots__ = ("last_id", "pending", "sent", "resent", "resent_creates", "peak", "lost", "label")

    def __init__(self, label=""):
        self.label = label          # who this window is for, shown in its log lines (optional: the caller sets it, e.g. "owner 7")
        self.last_id = 0
        self.pending = {}
        self.sent = self.resent = self.resent_creates = self.peak = self.lost = 0

    def stamp(self, messages, now):
        """The id for a datagram of these baked messages. Only its reliable messages are kept; a datagram with none stays unreliable."""
        keep = [m for m in messages if _KEEP[m[0] & 0x7F]]
        if not keep:
            return UNRELIABLE_ID
        self.last_id = (self.last_id + 1) % ID_WRAP
        self.pending[self.last_id] = (now, keep)
        self.sent += 1
        if len(self.pending) > self.peak:
            self.peak = len(self.pending)
        while len(self.pending) > MAX_PENDING:
            oldest = next(iter(self.pending))
            at, dropped = self.pending.pop(oldest)
            self.lost += 1
            ok, skipped = _RATE.allow(("overflow", self.label), GIVE_UP_LOG_SECONDS)
            if ok:
                LOG.warn(f"{self.label + ': ' if self.label else ''}{MAX_PENDING} reliable packets are waiting for an ack; giving up packet id {oldest} "
                         f"({len(dropped)} message(s), sent {now - at:.1f} s ago), {self.lost} given up so far" + (f" (+{skipped} more since the last line)" if skipped else ""))
        return self.last_id

    def ack(self, remote_ack, ack_bits):
        if not self.pending:
            return
        for pid in list(self.pending):
            d = (remote_ack - pid) % ID_WRAP
            if d == 0 or (d <= 32 and ack_bits >> (d - 1) & 1):
                del self.pending[pid]

    def due(self, now, last_rx):
        """Takes the packets that count as lost and returns their messages, oldest first."""
        out = []
        for pid, (at, messages) in list(self.pending.items()):
            if now - at <= RESEND_SECONDS:
                break
            if last_rx <= at + ACK_GRACE_SECONDS:
                continue
            del self.pending[pid]
            out.extend(messages)
        self.resent += len(out)
        creates = sum(1 for m in out if m[0] & 0x7F == CREATE)
        self.resent_creates += creates
        if out:
            LOG.debug(f"{self.label + ': ' if self.label else ''}{len(out)} message(s) ({creates} creates) resent, no ack after {RESEND_SECONDS:g} s; {len(self.pending)} packet(s) still waiting")
        return out

    def forget_owner(self, owner, router_bits):
        """That client left: its objects are being deleted, and a create resent after the delete would bring one back for good."""
        for pid, (at, messages) in list(self.pending.items()):
            kept = [m for m in messages if relay_owner(m, router_bits) != owner]
            if not kept:
                del self.pending[pid]
            elif len(kept) != len(messages):
                self.pending[pid] = (at, kept)
