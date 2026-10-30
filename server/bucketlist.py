"""Bucket-list co-op (router 12, IBucketListRouter, messages 61-68).

Recovered from the 1.0.0.64 client (docs/dlc-recon-2026-10-27.md for the message table):

* Router 12 has two object types. Type 0 BucketListPlayerStateSynced is created by every client for itself (id 0); six variables, so 3-bit
  variable ids: 0 Score (float), 1 CurrentSlotId (s32), 2 CurrentChallengeId (s32), 3 RequestingPartner (bool), 4 State (enum 0..4, 3 bits),
  5 ArrivedAtDestination (bool). Type 1 BucketListSlotArraySynced (server-owned, 12 slots) is not created: the client works without it.
  Evidence: constructor FUN_1417f5ae0; a real dev capture of an update (74 bits) carried vars 1, 2, 3 = slot 15, challenge 5, requesting.
* The stateflow bucket_list_activation.xml runs, for an online challenge: SynchronizeSlot, RequestMultiplayerPartner, wait for a partner (a
  RespondBucketListPartner), then NetworkBucketListStarted and the challenge itself. The client sends RequestSynchronize (63: slot, challenge)
  together with the PlayerState update, CancelPartnerRequest / DesynchronizeSlot as 64, and BucketListChallengeEnded (62: bool) at the end.
* Messages 61 and 65 to 68 are applied by the receiving client only when it can find the PlayerState object(s) the key names, so router 12
  objects must be relayed. A key is the object key of that PlayerState as the RECEIVER knows it: its own = plain id, another client's =
  relay key (A=0, B=1, id, owner:8), written with 7 id bits (FUN_1417ba4d0).
* The slot array (router 12, type 1, id 0, server-created; 12 entries of SlotId, ChallengeId, OwnerSyncId, all s32; 61 variables -> 6-bit
  variable ids, the leaves of the entries numbered in order: entry i has ids 3i, 3i+1, 3i+2, like the 12 PlayerChatState groups of the lobby
  player). A slot component finds its entry by SlotId (FUN_1409bcb80); when the entry names another player (OwnerSyncId = the second int of that
  player's object key = its relay owner id; the player list is searched by it, FUN_140325320) the slot offers "Join <name>" with the entry's
  challenge, and the owner's own prompt waits. So a request claims an entry and a pairing, a cancel, an end or a leave frees it (0 = unused).
* 65 sets the partner on the requesting component (only while its request is pending); 61 sets the challenge start time on the network clock
  (FUN_14043d6a0: the challenge may start once start != 0 and start <= clock). The slot component's RequestChallenge (FUN_140442ea0) resets that
  start time to 0 AFTER the partner reply, while the client loads the challenge, so 61 must arrive later: when both partners report State 2
  (set by BucketListLocalPlayerReady in hold_for_all_players_ready). Sent with the partner reply it was wiped, the client waited its 15 s for
  all players and left the challenge (dev 2026-10-29 18:47: both ended with ChallengeEnded ok=0 at +15 s).
* The clients send 64 (cancel the request, desynchronize the slot) right after the partner was found; that must not end the pairing.
  The pairing ends with 62 or when a player leaves.

* The 5-bit value in 65 is the receiving client's own start position (0 or 1): the component stores it (+0x190) and the "place player car at
  start" state passes it to the start-position setter (FUN_1408dd0d0 -> FUN_1407d9290). Sending 0 to both put both cars on the same spot at the start
  and the exit (dev 2026-10-29, "cars start and return to exact same spots"); the player who waited gets 0, the one who joined 1.
* Retail footage (user, 2026-10-29): a waiting player shows "Join <name>" on the challenge intro of the others and the party list says
  "Waiting for player..."; that is BucketListStatusPlayerWaiting (66), so status messages are on by default. The s32 is guessed to be the
  challenge id (the intro screen is the challenge's; the PlayerState already carries the slot); FH2MP_BL_STATUS=slot tries the other.

What is NOT known (each is a module constant or an environment variable, so a dev client can try the alternatives): the meaning of the
string in 65, and of the s32 in 66 to 68 (slot or challenge; FH2MP_BL_STATUS).
"""
import os
import struct

ROUTER = 12
TYPE_PLAYER_STATE, TYPE_SLOT_ARRAY = 0, 1
ID_BITS, TYPE_BITS = 4, 2     # 13 objects, 2 types; bit_length(numTypes) = 2 type bits on a create (proved by the padding of a real create datagram)
VAR_ID_BITS = 3
KEY_ID_BITS = 7

MSG_START = 61            # server -> client: start time u32 (network clock, ms)
MSG_ENDED = 62            # client -> server: bool
MSG_SYNC = 63             # client -> server: s32 slot, s32 challenge
MSG_DESYNC = 64           # client -> server: empty
MSG_PARTNER = 65          # server -> client: key | u5 | bool | string(<= 36)
MSG_WAITING = 66          # server -> clients: key | s32
MSG_STARTED = 67          # server -> clients: key | key | s32
MSG_STATUS_ENDED = 68     # server -> clients: key | key | s32 | bool

WAIT_SECONDS = float(os.environ.get("FH2MP_BL_WAIT_S", "120"))         # a request nobody joined is dropped (the client times out earlier)
START_DELAY_MS = int(os.environ.get("FH2MP_BL_START_DELAY_MS", "3000"))   # after both partners are ready
READY_STATE = int(os.environ.get("FH2MP_BL_READY_STATE", "2"))           # BucketListPlayerStateEnum value a client reports once its challenge is loaded
# Start positions sent in 65 (waiter, joiner). FH2MP_BL_INDEX: own = 0/1 (default), swap = 1/0, both0, both1: a dev experiment to learn whether the value
# reaches the placement of the cars at all (dev 2026-10-30: both players still started and finished on the same spot with 0/1).
INDEX_WAITER, INDEX_JOINER = {"own": (0, 1), "swap": (1, 0), "both0": (0, 0), "both1": (1, 1)}.get(os.environ.get("FH2MP_BL_INDEX", "own"), (0, 1))
STATUS = os.environ.get("FH2MP_BL_STATUS", "challenge")                     # 66-68 to the other players: off, slot or challenge (what the s32 is)

PLAYER_STATE_VARS = {0: ("Score", 32), 1: ("CurrentSlotId", 32), 2: ("CurrentChallengeId", 32),
                     3: ("RequestingPartner", 1), 4: ("State", 3), 5: ("ArrivedAtDestination", 1)}


def s32(value: int) -> int:
    return value - (1 << 32) if value & 0x80000000 else value


def decode_player_state(size: int, payload: int) -> dict:
    """A BucketListPlayerStateSynced update -> {name: value}, in wire order. Raises ValueError for a variable id the object does not have."""
    out, pos = {}, 0
    while pos + VAR_ID_BITS <= size:
        vid = (payload >> pos) & ((1 << VAR_ID_BITS) - 1)
        pos += VAR_ID_BITS
        spec = PLAYER_STATE_VARS.get(vid)
        if spec is None:
            raise ValueError(f"BucketListPlayerStateSynced has no variable {vid}")
        name, width = spec
        if pos + width > size:
            raise ValueError("truncated BucketListPlayerStateSynced update")
        raw = (payload >> pos) & ((1 << width) - 1)
        pos += width
        if name == "Score":
            out[name] = struct.unpack("<f", struct.pack("<I", raw))[0]
        elif name in ("CurrentSlotId", "CurrentChallengeId"):
            out[name] = s32(raw)
        else:
            out[name] = raw
    return out


def write_key(w, kind: str, oid: int, owner: int = 0) -> None:
    """FUN_1417ba4d0: A:1 | B:1 | id:7 | [owner:8 when A=0, B=1]. kind 'own' = the receiver's own object, 'relay' = another client's."""
    if kind == "own":
        w.bits(0, 1)
        w.bits(0, 1)
        w.bits(oid, KEY_ID_BITS)
    else:
        w.bits(0, 1)
        w.bits(1, 1)
        w.bits(oid, KEY_ID_BITS)
        w.bits(owner, 8)


def start_message(start_ms: int):
    def write(w):
        w.bits(MSG_START, 7)
        w.bits(start_ms & 0xFFFFFFFF, 32)
    return write


def partner_message(key, value: int, found: bool, name: str):
    def write(w):
        w.bits(MSG_PARTNER, 7)
        write_key(w, *key)
        w.bits(value, 5)
        w.bits(1 if found else 0, 1)
        w.string(name[:36])
    return write


def waiting_message(key, number: int):
    def write(w):
        w.bits(MSG_WAITING, 7)
        write_key(w, *key)
        w.bits(number & 0xFFFFFFFF, 32)
    return write


def started_message(key_a, key_b, number: int):
    def write(w):
        w.bits(MSG_STARTED, 7)
        write_key(w, *key_a)
        write_key(w, *key_b)
        w.bits(number & 0xFFFFFFFF, 32)
    return write


def status_ended_message(key_a, key_b, number: int, ok: bool):
    def write(w):
        w.bits(MSG_STATUS_ENDED, 7)
        write_key(w, *key_a)
        write_key(w, *key_b)
        w.bits(number & 0xFFFFFFFF, 32)
        w.bits(1 if ok else 0, 1)
    return write


SLOT_COUNT = 12
SLOT_VAR_ID_BITS = 6


class SlotTable:
    """The server's copy of the slot array: [slot, challenge, owner] per entry, [0, 0, 0] = unused."""

    def __init__(self):
        self.entries = [[0, 0, 0] for _ in range(SLOT_COUNT)]

    def claim(self, slot: int, challenge: int, owner: int):
        """An entry for `slot` (an existing one is taken over) -> the changed entry index, or None when all 12 are in use."""
        for i, e in enumerate(self.entries):
            if e[0] == slot:
                break
        else:
            free = [i for i, e in enumerate(self.entries) if e[0] == 0]
            if not free or slot == 0:
                return None
            i = free[0]
        if self.entries[i] == [slot, challenge, owner]:
            return []
        self.entries[i] = [slot, challenge, owner]
        return [i]

    def release(self, owner: int):
        """Frees every entry of this owner -> the changed indexes."""
        changed = [i for i, e in enumerate(self.entries) if e[2] == owner and e[0] != 0]
        for i in changed:
            self.entries[i] = [0, 0, 0]
        return changed

    def used(self):
        return [i for i, e in enumerate(self.entries) if e[0] != 0]

    def writer(self, indexes):
        """Variables of the given entries, in id order (a delta must be ascending)."""
        def write(w):
            for i in sorted(indexes):
                for j, value in enumerate(self.entries[i]):
                    w.bits(3 * i + j, SLOT_VAR_ID_BITS)
                    w.bits(value & 0xFFFFFFFF, 32)
        return write


class Coop:
    """Who waits for a partner and who plays with whom, for one world. `key_of(subject, receiver)` is the key tuple of subject's PlayerState as
    receiver knows it (None while subject has none); `clock_ms()` is the network clock the clients are synchronised to, `seconds()` a monotonic
    clock. Every method returns ([(receiver, writer)], log text) for the server to send and log."""

    def __init__(self, key_of, clock_ms, seconds):
        self.key_of, self.clock_ms, self.seconds = key_of, clock_ms, seconds
        self.waiting = {}      # peer -> (slot, challenge, since)
        self.partner = {}      # peer -> peer, both directions while a challenge runs
        self.running = {}      # peer -> (slot, challenge)
        self.started = set()   # peers that were given their start time
        self.slots = SlotTable()
        self.changed = []      # slot array entries that changed since the server last sent them

    def request(self, peer, slot: int, challenge: int, world, name_of=lambda p: ""):
        """63. `world` = every peer that is in the same world."""
        self.release(peer)
        self.break_pair(peer)
        if self.key_of(peer, peer) is None:
            return [], "has no bucket-list player state yet, ignored"
        at = self.seconds()
        for q in world:
            held = self.waiting.get(q)
            if q is peer or held is None or held[:2] != (slot, challenge) or at - held[2] > WAIT_SECONDS:
                continue
            if self.key_of(q, peer) is None or self.key_of(peer, q) is None:
                continue
            del self.waiting[q]
            self.release(q)
            self.partner[peer], self.partner[q] = q, peer
            self.running[peer] = self.running[q] = (slot, challenge)
            out = []
            for me, other, index in ((q, peer, INDEX_WAITER), (peer, q, INDEX_JOINER)):       # the one who waited starts from position 0
                out.append((me, partner_message(self.key_of(other, me), index, True, name_of(other))))
            out += self.status(world, "started", peer, q, slot, challenge)
            return out, f"paired with owner {getattr(q, 'owner_id', '?')} (slot {slot}, challenge {challenge}); the start time follows when both are ready"
        self.waiting[peer] = (slot, challenge, at)
        claimed = self.slots.claim(slot, challenge, getattr(peer, "owner_id", 0))
        self.changed += claimed or []
        return self.status(world, "waiting", peer, None, slot, challenge), f"waiting for a partner (slot {slot}, challenge {challenge})"

    def status(self, world, kind, a, b, slot, challenge, ok=False):
        """66 to 68 for the other players (FH2MP_BL_STATUS); the s32 they carry is a guess, see the module docstring."""
        if STATUS == "off":
            return []
        number = slot if STATUS == "slot" else challenge
        out = []
        for q in world:
            ka = self.key_of(a, q)
            kb = self.key_of(b, q) if b is not None else None
            if ka is None or (b is not None and kb is None):
                continue
            if kind == "waiting" and q is not a:
                out.append((q, waiting_message(ka, number)))
            elif kind == "started":
                out.append((q, started_message(ka, kb, number)))
            elif kind == "ended":
                out.append((q, status_ended_message(ka, kb, number, ok)))
        return out

    def cancel(self, peer):
        """64: the request (and the slot sync) is cancelled. A pairing that already exists stays: the clients send this right after it."""
        return self.release(peer)

    def release(self, peer):
        """Drops the peer's waiting request and its slot array entries -> whether it was waiting."""
        was = self.waiting.pop(peer, None) is not None
        self.changed += self.slots.release(getattr(peer, "owner_id", 0))
        return was

    def leave(self, peer):
        """The peer left. Returns the partner it was playing with, if any."""
        self.release(peer)
        return self.break_pair(peer)

    def state_changed(self, peer):
        """A PlayerState update of a paired peer arrived: when both partners are ready, both get the start time (once)."""
        other = self.partner.get(peer)
        if other is None or peer in self.started or other in self.started:
            return [], ""
        if peer.bl_state.get("State") != READY_STATE or other.bl_state.get("State") != READY_STATE:
            return [], ""
        start = (self.clock_ms() + START_DELAY_MS) & 0xFFFFFFFF or 1      # 0 would mean "no start time"
        self.started.update((peer, other))
        return [(peer, start_message(start)), (other, start_message(start))], \
            f"both ready with owner {getattr(other, 'owner_id', '?')}: start {start}"

    def break_pair(self, peer):
        other = self.partner.pop(peer, None)
        self.running.pop(peer, None)
        self.started.discard(peer)
        if other is not None and self.partner.get(other) is peer:
            del self.partner[other]
            self.running.pop(other, None)
            self.started.discard(other)
        return other

    def ended(self, peer, ok: bool, world):
        """62: the challenge is over for this client."""
        slot_challenge = self.running.get(peer)
        self.release(peer)
        other = self.break_pair(peer)
        if other is None or slot_challenge is None:
            return [], other
        return self.status(world, "ended", peer, other, slot_challenge[0], slot_challenge[1], ok), other
