#!/usr/bin/env python3
"""Minimal Forza Horizon 2 (Xbox One build 1.0.0.64) NetV2 dedicated-server stub.

Run:  python3 -I server/fh2mp_server.py [port] [logfile]

Wire format (recovered from the client binary):
  * UDP, default port 5000.
  * Bit stream is LSB-first (BitStream.cpp): value bits are emitted starting at the low bit.
  * Packet header, 88 bits:  ackId:16 | remoteAck:16 | ackBits:32 | msgCount:8 | size:16
  * Each message:  type:7 | payload
  * String:        varlen(len) | len*8 bits        varlen = cls:2 | value:(cls*8+6)
"""
import selectors
import socket
import sys
import time

VERSION_STRING = "6CC152FD"   # PTR_s_6CC152FD in the game binary
GAME_STRING = "Flagship"      # PTR_s_Flagship
HEADER_BITS = 88

MSG_OPEN_CONNECTION = 0
MSG_OPEN_CONNECTION_RESPONSE = 1
MSG_CLOSE_CONNECTION = 2
MSG_PING = 3
MSG_PONG = 4
MSG_TIME_SYNC = 5
MSG_CONNECTION_SYNC_DONE = 6
FIRST_OBJECT_MSG = 7   # synced-object protocol: 7 create, 8 update, 9 delete, 10..13 ownership
RESPONSE_ACK = 6       # client's third handshake leg
MSG_SERVER_TRAFFIC_MESSAGE = 71   # client -> server (traffic requests)
MSG_CLIENT_TRAFFIC_MESSAGE = 72   # server -> clients (relay of the above, applied by everyone)
MSG_FINALISE_SESSION_ENTRY = 34   # no payload, both directions
MSG_FULL_WEATHER = 54             # atmosphere(3f,4i) + precipitation(3f) + 2 x s32
KEEPALIVE_SECONDS = 1.0
MSG_OBJECT_CREATE = 7
MSG_OBJECT_UPDATE = 8
ROUTER_LOBBY = 6          # ILobbyRouter: SessionHostSynced (type 0), GameOptionsSynced (type 1)
LOBBY_ID_BITS = 2         # bit_length(maxObjects=2)
LOBBY_TYPE_BITS = 2       # bit_length(numTypes=2)
TYPE_SESSION_HOST = 0
TYPE_GAME_OPTIONS = 1
# (idBits, typeBitsOnCreate) per router; derived from SyncedObjectManager config
# (idBits = bit_length(maxObjects), typeBits = bit_length(numTypes) when numTypes > 1).
ROUTER_BITS = {
    1: (7, 0),   # packet-update-only capture pinned idBits=7
    2: (7, 0),   # id+type total = 7
    3: (7, 0),   # IConsoleManager: maxObjects 0x40 -> 7 bits, one type
    6: (2, 2),   # ILobbyRouter: SessionHost / GameOptions
    9: (0, 6),   # id+type total = 6
}
# WeatherSynced (router 10): one object -> 1 id bit, 1 type; max vars 0xb -> 4-bit var ids.
# Flattened member order from the ctors: atmosphere 0..4, precipitation 5..8 (struct ids unverified).
ROUTER_WEATHER = 10
WEATHER_VAR_ID_BITS = 4
ROUTER_BITS[ROUTER_WEATHER] = (1, 0)
WEATHER_VARS = {
    "atm.CurrentIntensity": 0, "atm.CurrentTick": 1, "atm.DesiredIntensity": 2,
    "atm.DesiredTick": 3, "atm.DesiredPresetIndex": 4,
    "pre.CurrentIntensity": 5, "pre.CurrentTick": 6, "pre.DesiredIntensity": 7,
    "pre.DesiredTick": 8,
}
# ConsoleSynced schema (FUN_1417de550): 9 vars -> var id = bit_length(9) = 4 bits
CONSOLE_VAR_ID_BITS = 4
CONSOLE_VAR_SESSION_ENTRY_FINALISED = 1
MSG_REQUEST_RACE = 38               # no payload: the leader asks for an event
MSG_READY_REQUEST = 41              # key (idBits 2) | ready:1
SESSION_HOST_VAR_ID_BITS = 4        # 13 vars
SH_RACE_READY = 4
SH_RACE_PARTICIPANTS_SELECTED = 5
SH_IN_RACE = 6
MSG_STARTING_GRID = 39              # count:4 | key(idBits 7) x count (FUN_141824040)
MSG_START_RACE = 40                 # start time:32 on the server clock (FUN_1418241e0)
MSG_READY_REQUEST_RESPONSE = 42     # response:2 (FUN_141824250)
RACE_COUNTDOWN_MS = 5000
CONSOLE_VAR_PARTICIPATING_IN_RACE = 2
CONSOLE_BOOL_VAR_NAMES = {0: "RenderReady", 1: "SessionEntryFinalised", 2: "ParticipatingInRace",
                          3: "RaceInProgress", 4: "Ready", 5: "HasLoadedCarsForAllParticipants",
                          6: "HasGrid"}
RACE_INVITE_SECONDS = 6.0           # leader requested a race: wait this long for others, then start
MSG_OBJECT_DELETE = 9
MSG_OBJECT_REQUEST_OWNERSHIP = 10   # client -> server: router:4 | typeId | key (A=1,B=0 transfer id)
MSG_OBJECT_GRANT_OWNERSHIP = 11     # server -> client: router:4 | key | seq:8 | size:14 (+size bits)
MSG_CAR_PREDICTION_FULL = 15
CAR_KEY_ID_BITS = 7            # FUN_141808170: object key written with 7 id bits
RELAY_ROUTERS = (1, 2, 3, 6, 9)   # player / console / car objects other clients must see
RELAY_HISTORY_CAP = 2000       # stored messages per client, replayed to late joiners
STALE_SECONDS = 20.0
# Azimuth gives every instance the same Xbox user id (saves/player-1/User_9000000000000).
# The game's player list skips any player whose id equals the local player's, so two instances
# on one profile never see each other in "Players". Relayed copies get a per-connection id.
XUID_PATTERN = 0x0009000000000000
UNRELIABLE_ID = 0xFFFF
NO_ID = 0xFFFE

RESULT_OK, RESULT_VERSION_MISMATCH, RESULT_GAME_MISMATCH, RESULT_BANNED = 0, 1, 2, 3


class BitReader:
    def __init__(self, data: bytes):
        self.value = int.from_bytes(data, "little")
        self.pos = 0
        self.size = len(data) * 8

    def bits(self, n: int) -> int:
        if self.pos + n > self.size:
            raise ValueError("read past end")
        x = (self.value >> self.pos) & ((1 << n) - 1)
        self.pos += n
        return x

    def varlen(self) -> int:
        cls = self.bits(2)
        return self.bits(cls * 8 + 6)

    def string(self) -> str:
        n = self.varlen()
        return bytes(self.bits(8) for _ in range(n)).decode("latin1")


class BitWriter:
    def __init__(self):
        self.value = 0
        self.pos = 0

    def bits(self, value: int, n: int) -> None:
        self.value |= (value & ((1 << n) - 1)) << self.pos
        self.pos += n

    def varlen(self, v: int) -> None:
        cls = 3 if v >= 1 << 22 else 2 if v >= 1 << 14 else 1 if v >= 1 << 6 else 0
        self.bits(cls, 2)
        self.bits(v, cls * 8 + 6)

    def string(self, s: str) -> None:
        raw = s.encode("latin1")
        self.varlen(len(raw))
        for b in raw:
            self.bits(b, 8)

    def tobytes(self) -> bytes:
        return self.value.to_bytes((self.pos + 7) // 8, "little")


def parse_header(r: BitReader):
    return {
        "ack_id": r.bits(16),
        "remote_ack": r.bits(16),
        "ack_bits": r.bits(32),
        "count": r.bits(8),
        "size": r.bits(16),
    }


def build_packet(messages, ack_id=0xFFFF, remote_ack=0, ack_bits=0) -> bytes:
    """messages: list of callables (BitWriter) -> None writing one message each."""
    body = BitWriter()
    for write in messages:
        write(body)
    total_bits = HEADER_BITS + body.pos
    size = (total_bits + 7) // 8
    w = BitWriter()
    w.bits(ack_id, 16)
    w.bits(remote_ack, 16)
    w.bits(ack_bits, 32)
    w.bits(len(messages), 8)
    w.bits(size, 16)
    w.value |= body.value << HEADER_BITS
    w.pos = HEADER_BITS + body.pos
    return w.tobytes()


def open_connection_response(flag2: int, result: int, echo: int, server_counter: int):
    def write(w: BitWriter):
        w.bits(MSG_OPEN_CONNECTION_RESPONSE, 7)
        w.bits(flag2, 2)
        w.bits(result, 3)
        w.bits(echo, 32)
        w.bits(server_counter, 32)
    return write


def pong(echo: int, now: int):
    def write(w: BitWriter):
        w.bits(MSG_PONG, 7)
        w.bits(echo, 32)
        w.bits(now, 32)
    return write


def time_sync(seq: int, now: int):
    def write(w: BitWriter):
        w.bits(MSG_TIME_SYNC, 7)
        w.bits(seq, 16)
        w.bits(now, 32)
    return write


def ping(value: int):
    def write(w: BitWriter):
        w.bits(MSG_PING, 7)
        w.bits(value, 32)
    return write


def client_traffic_message(kind: int, a: int, s14: int, u5: int, b: int, flag: int, seq: int):
    """Relay of a ServerTrafficMessage: kind:4 | u8 | s14 | u5 | u8 | bool | s32 (raw bits)."""
    def write(w: BitWriter):
        w.bits(MSG_CLIENT_TRAFFIC_MESSAGE, 7)
        w.bits(kind, 4)
        w.bits(a, 8)
        w.bits(s14, 14)
        w.bits(u5, 5)
        w.bits(b, 8)
        w.bits(flag, 1)
        w.bits(seq, 32)
    return write


def finalise_session_entry():
    def write(w: BitWriter):
        w.bits(MSG_FINALISE_SESSION_ENTRY, 7)
    return write


def full_weather(atmosphere=(0.0, 0.0, 0.0, 0, 0, 0, 0), precipitation=(0.0, 0.0, 0.0),
                 a: int = 0, b: int = 0):
    import struct

    def f32(x: float) -> int:
        return struct.unpack("<I", struct.pack("<f", x))[0]

    def write(w: BitWriter):
        w.bits(MSG_FULL_WEATHER, 7)
        for v in atmosphere[:3]:
            w.bits(f32(v), 32)
        for v in atmosphere[3:7]:
            w.bits(v, 32)
        for v in precipitation:
            w.bits(f32(v), 32)
        w.bits(a, 32)
        w.bits(b, 32)
    return write


def close_connection(reason: int, a: int = 0, b: int = 0):
    def write(w: BitWriter):
        w.bits(MSG_CLOSE_CONNECTION, 7)
        w.bits(reason, 3)
        w.bits(a, 32)
        w.bits(b, 32)
    return write


def connection_sync_done(value: int):
    def write(w: BitWriter):
        w.bits(MSG_CONNECTION_SYNC_DONE, 7)
        w.bits(value, 32)
    return write


def object_message(msg_type, router, obj_id, type_id, seq, id_bits, type_bits, write_vars=None):
    """Synced-object create (7) / update (8) message, per NerdLib SyncObjectLink/SyncedObject.

    type:7 | router:4 | flagA:1 | flagB:1 | objectId:idBits | [typeId:typeBits (create)] |
    seq:8 | size:14 | vars...      (size = number of bits in vars)
    """
    def write(w: BitWriter):
        w.bits(msg_type, 7)
        w.bits(router, 4)
        w.bits(0, 1)          # flagA: plain id
        w.bits(0, 1)          # flagB
        w.bits(obj_id, id_bits)
        if msg_type == MSG_OBJECT_CREATE and type_bits:
            w.bits(type_id, type_bits)
        w.bits(seq, 8)
        body = BitWriter()
        if write_vars:
            write_vars(body)
        w.bits(body.pos, 14)
        w.value |= body.value << w.pos
        w.pos += body.pos
    return write


def relay_object_message(msg_type, router, obj_id, owner, type_id, seq, id_bits, type_bits,
                         size, payload):
    """Another client's object, as the server relays it to this client.

    Key per FUN_1417b0e40: A=0 | B=1 | id:idBits | owner:8 (relay objects are keyed by the
    owning connection); then type id (create only), seq:8, size:14, vars."""
    def write(w: BitWriter):
        w.bits(msg_type, 7)
        w.bits(router, 4)
        w.bits(0, 1)
        w.bits(1, 1)
        w.bits(obj_id, id_bits)
        w.bits(owner, 8)
        if msg_type == MSG_OBJECT_CREATE and type_bits:
            w.bits(type_id or 0, type_bits)
        w.bits(seq, 8)
        w.bits(size, 14)
        w.value |= payload << w.pos
        w.pos += size
    return write


def grant_ownership(router, obj_id, id_bits, seq=0):
    """FUN_1417ab720 / FUN_1417b1e70: the client resolves its transfer object by key
    (A=1,B=0,id), then applies a sequenced message: seq:8, size:14 and size payload bits."""
    def write(w: BitWriter):
        w.bits(MSG_OBJECT_GRANT_OWNERSHIP, 7)
        w.bits(router, 4)
        w.bits(1, 1)
        w.bits(0, 1)
        w.bits(obj_id, id_bits)
        w.bits(seq, 8)
        w.bits(0, 14)
    return write


def starting_grid(entries):
    """entries: list of (kind, id, owner): kind 'local' = the receiver's own car (A=1,B=0),
    'relay' = another client's car (A=0,B=1,owner follows the id)."""
    def write(w: BitWriter):
        w.bits(MSG_STARTING_GRID, 7)
        w.bits(len(entries), 4)
        for kind, cid, owner in entries:
            if kind == "local":
                w.bits(1, 1)
                w.bits(0, 1)
                w.bits(cid, CAR_KEY_ID_BITS)
            else:
                w.bits(0, 1)
                w.bits(1, 1)
                w.bits(cid, CAR_KEY_ID_BITS)
                w.bits(owner, 8)
    return write


def start_race(start_ms):
    def write(w: BitWriter):
        w.bits(MSG_START_RACE, 7)
        w.bits(start_ms & 0xFFFFFFFF, 32)
    return write


def ready_request_response(code):
    def write(w: BitWriter):
        w.bits(MSG_READY_REQUEST_RESPONSE, 7)
        w.bits(code, 2)
    return write


def session_host_update(seq, write_vars):
    """Server-owned SessionHostSynced (router 6, type 0, id 0) update: A=0,B=0 remote key."""
    return object_message(MSG_OBJECT_UPDATE, ROUTER_LOBBY, 0, 0, seq, LOBBY_ID_BITS, 0, write_vars)


def relay_delete_message(router, obj_id, owner, id_bits, seq):
    """ObjectDelete (FUN_1417ab560): A | B | id | owner:8 (relay) | msgId:8; rejected as 'old'
    unless msgId is not behind the last message applied to that object."""
    def write(w: BitWriter):
        w.bits(MSG_OBJECT_DELETE, 7)
        w.bits(router, 4)
        w.bits(0, 1)
        w.bits(1, 1)
        w.bits(obj_id, id_bits)
        w.bits(owner, 8)
        w.bits(seq, 8)
    return write


def relay_car_prediction(obj_id, owner, rest_value, rest_bits):
    def write(w: BitWriter):
        w.bits(MSG_CAR_PREDICTION_FULL, 7)
        w.bits(0, 1)
        w.bits(1, 1)
        w.bits(obj_id, CAR_KEY_ID_BITS)
        w.bits(owner, 8)
        w.value |= rest_value << w.pos
        w.pos += rest_bits
    return write


def server_update(router: int, obj_id: int, id_bits: int, seq: int, write_vars=None):
    """Update from the authority for an object the client owns: both id flags set, id only."""
    def write(w: BitWriter):
        w.bits(MSG_OBJECT_UPDATE, 7)
        w.bits(router, 4)
        w.bits(1, 1)
        w.bits(1, 1)
        w.bits(obj_id, id_bits)
        w.bits(seq, 8)
        body = BitWriter()
        if write_vars:
            write_vars(body)
        w.bits(body.pos, 14)
        w.value |= body.value << w.pos
        w.pos += body.pos
    return write


def weather_create(seq=1, intensity=0.0, tick=1):
    import struct

    def f32(x: float) -> int:
        return struct.unpack("<I", struct.pack("<f", x))[0]

    def vars_(w: BitWriter):
        for name, kind, value in (
            ("atm.CurrentIntensity", "f", intensity), ("atm.CurrentTick", "u", tick),
            ("atm.DesiredIntensity", "f", intensity), ("atm.DesiredTick", "u", tick),
            ("pre.CurrentIntensity", "f", intensity), ("pre.CurrentTick", "u", tick),
            ("pre.DesiredIntensity", "f", intensity), ("pre.DesiredTick", "u", tick),
        ):
            w.bits(WEATHER_VARS[name], WEATHER_VAR_ID_BITS)
            w.bits(f32(value) if kind == "f" else value, 32)
    return object_message(MSG_OBJECT_CREATE, ROUTER_WEATHER, 0, 0, seq, 1, 0, vars_)


def session_host_create(seq=1, write_vars=None):
    return object_message(MSG_OBJECT_CREATE, ROUTER_LOBBY, 0, TYPE_SESSION_HOST, seq,
                          LOBBY_ID_BITS, LOBBY_TYPE_BITS, write_vars)


def rewrite_xuid(payload: int, size: int, new_value: int):
    """Replace every 64-bit window equal to XUID_PATTERN; returns (payload, replacements)."""
    mask = (1 << 64) - 1
    found = 0
    off = 0
    while off + 64 <= size:
        if (payload >> off) & mask == XUID_PATTERN:
            payload = (payload & ~(mask << off)) | (new_value << off)
            found += 1
            off += 64
        else:
            off += 1
    return payload, found


class Peer:
    """Per-client reliability state: what we have received, to build AckId/AckBits."""

    def __init__(self, addr):
        self.addr = addr
        self.last_id = NO_ID
        self.seen = set()
        self.connected = False
        self.client_counter = 0
        self.sync_sent = False
        self.last_keepalive = time.monotonic()
        self.time_sync_seq = 0
        self.console_acked = False
        self.console_seq = 1
        self.traffic_relayed = 0
        self.next_traffic_id = 0
        self.owner_id = 0          # relay owner id other clients see this client's objects under
        self.entered = False       # sent FinaliseSessionEntry: may receive other clients' objects
        self.objs = {}             # (router, id) -> [relay-ready (type, tid, seq, size, payload)]
        self.host_seq = 2          # next SessionHost update number for this client (create used 1)
        self.relay_next = {}       # (router, id) -> next relay sequence number (server renumbers)
        self.console_vars = {}     # latest bool Console vars reported by this client
        self.console_oid = None    # id of this client's ConsoleSynced object (router 3)
        self.last_rx = time.monotonic()
        self.cars_relayed = 0

    def note_packet_id(self, pid: int) -> None:
        if pid == UNRELIABLE_ID:
            return
        if self.last_id == NO_ID or ((pid - self.last_id) % 0xFFFF) < 0x8000:
            self.last_id = pid
        self.seen.add(pid)
        if len(self.seen) > 64:  # keep it bounded
            keep = {p for p in self.seen if ((self.last_id - p) % 0xFFFF) <= 40}
            self.seen = keep

    def ack_fields(self):
        if self.last_id == NO_ID:
            return NO_ID, 0
        bits = 0
        for pid in self.seen:
            d = (self.last_id - pid) % 0xFFFF
            if 1 <= d <= 32:
                bits |= 1 << (d - 1)
        return self.last_id, bits


class Server:
    def __init__(self, port: int, logpath):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", port))
        self.log_file = open(logpath, "a", buffering=1) if logpath else None
        self.server_counter = 1
        self.start = time.monotonic()
        self.peers = {}
        self.next_owner = 1
        self.pending_race = None   # (deadline, leader Peer)
        self.event = None          # {"people": [Peer], "begun": bool, "start_at": float|None}
        self.log(f"listening on udp/{port} version={VERSION_STRING} game={GAME_STRING}")

    def now_ms(self) -> int:
        return int((time.monotonic() - self.start) * 1000) & 0xFFFFFFFF

    def log(self, msg: str) -> None:
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        if self.log_file:
            self.log_file.write(line + "\n")

    def send(self, peer: Peer, messages) -> None:
        # The client measures RTT in whole milliseconds and discards 0 ms as invalid, which makes
        # it fall back to a 100 ms retransmit timer. Hold replies back ~2 ms so RTT reads >= 1.
        time.sleep(0.002)
        ack, bits = peer.ack_fields()
        pkt = build_packet(messages, ack_id=UNRELIABLE_ID, remote_ack=ack, ack_bits=bits)
        self.sock.sendto(pkt, peer.addr)

    def others(self, peer: Peer):
        return [q for q in self.peers.values() if q is not peer and q.connected and q.entered]

    def remember_and_relay(self, peer: Peer, mtype, router, oid, tid, seq, size, payload) -> None:
        id_bits, type_bits = ROUTER_BITS[router]
        if peer.owner_id:
            payload, n_x = rewrite_xuid(payload, size, XUID_PATTERN + peer.owner_id)
            if n_x:
                self.log(f"{peer.addr[1]} router {router} object {oid}: user id rewritten "
                         f"x{n_x} -> {XUID_PATTERN + peer.owner_id:#x} for other clients")
        # Relay objects are applied strictly in sequence order, and the server also injects
        # its own updates (e.g. Console.SessionEntryFinalised), so the server owns the numbering.
        n = peer.relay_next.get((router, oid), 0)
        peer.relay_next[(router, oid)] = n + 1
        seq = n % 0xFF
        hist = peer.objs.setdefault((router, oid), [])
        if len(hist) < RELAY_HISTORY_CAP:
            hist.append((mtype, tid, seq, size, payload))
        msg = relay_object_message(mtype, router, oid, peer.owner_id, tid, seq, id_bits,
                                   type_bits, size, payload)
        for q in self.others(peer):
            self.send(q, [msg])

    def replay_to(self, newcomer: Peer) -> None:
        """Late joiner: announce everything the other clients already created."""
        for q in self.peers.values():
            if q is newcomer or not q.connected:
                continue
            batch = []
            for (router, oid), hist in q.objs.items():
                id_bits, type_bits = ROUTER_BITS[router]
                for (mtype, tid, seq, size, payload) in hist:
                    batch.append(relay_object_message(mtype, router, oid, q.owner_id, tid, seq,
                                                      id_bits, type_bits, size, payload))
            for i in range(0, len(batch), 4):
                self.send(newcomer, batch[i:i + 4])
            if batch:
                self.log(f"{newcomer.addr[1]} <- replayed {len(batch)} object messages "
                         f"of owner {q.owner_id} ({q.addr[1]})")

    def set_session_host_bool(self, var_id: int, value: int, why: str) -> None:
        for q in self.peers.values():
            if not q.connected:
                continue

            def vars_(w, var_id=var_id, value=value):
                w.bits(var_id, SESSION_HOST_VAR_ID_BITS)
                w.bits(value, 1)
            self.send(q, [session_host_update(q.host_seq, vars_)])
            q.host_seq = (q.host_seq + 1) % 0xFF
        self.log(f"SessionHost var {var_id} = {value} sent to all ({why})")

    def set_console_bool(self, peer: Peer, var_id: int, value: int, why: str) -> None:
        """Server-side write of a Console var: to the owner (server update) and relayed to the others."""
        if peer.console_oid is None:
            return

        def vars_(w):
            w.bits(var_id, CONSOLE_VAR_ID_BITS)
            w.bits(value, 1)
        self.send(peer, [server_update(3, peer.console_oid, ROUTER_BITS[3][0], peer.console_seq, vars_)])
        peer.console_seq = (peer.console_seq + 1) % 0xFF
        vb = BitWriter()
        vars_(vb)
        self.remember_and_relay(peer, MSG_OBJECT_UPDATE, 3, peer.console_oid, None, 0, vb.pos, vb.value)
        self.log(f"{peer.addr[1]} Console.{CONSOLE_BOOL_VAR_NAMES.get(var_id, var_id)} = {value} ({why})")

    def start_event(self, leader: Peer) -> None:
        """Participants = the leader plus every client that reported Console.Ready."""
        people = [leader] + [q for q in self.peers.values()
                             if q is not leader and q.connected and q.console_vars.get(4)]
        self.log(f"event start: {len(people)} participant(s): {[q.owner_id for q in people]}")
        self.event = {"people": people, "begun": False, "start_at": None}
        for q in people:
            self.set_console_bool(q, CONSOLE_VAR_PARTICIPATING_IN_RACE, 1, "participant selected")
        self.set_session_host_bool(SH_RACE_PARTICIPANTS_SELECTED, 1, "participants selected")

    def maybe_begin_race(self) -> None:
        ev = self.event
        if not ev or ev["begun"]:
            return
        if not all(q.connected and q.console_vars.get(5) for q in ev["people"]):
            return
        ev["begun"] = True
        self.log(f"all {len(ev['people'])} participant(s) loaded cars -> InRace=1, start in "
                 f"{RACE_COUNTDOWN_MS} ms")
        # MsgStartingGrid (39) is NOT sent: the client's ILobbyRouter slot for it is a no-op and the
        # dispatch fails ("Dispatch of message 39 failed" -> disconnect due to bad packet), observed
        # in the host log on 2026-10-16. Free Roam events are started with SessionHost flags + 40.
        self.set_session_host_bool(SH_IN_RACE, 1, "race begun")
        go = (self.now_ms() + RACE_COUNTDOWN_MS) & 0xFFFFFFFF
        self.log(f"MsgStartRace start time {go} (server clock now {self.now_ms()})")
        for q in ev["people"]:
            self.send(q, [start_race(go)])

    def drop_peer(self, peer: Peer, why: str) -> None:
        self.peers.pop(peer.addr, None)
        self.log(f"{peer.addr[1]} removed ({why}); owner {peer.owner_id} had {len(peer.objs)} objects")
        for (router, oid) in peer.objs:
            id_bits, _ = ROUTER_BITS[router]
            n = peer.relay_next.get((router, oid), 0)
            msg = relay_delete_message(router, oid, peer.owner_id, id_bits, n % 0xFF)
            for q in self.others(peer):
                self.send(q, [msg])

    def handle(self, data: bytes, addr) -> None:
        is_new = addr not in self.peers
        peer = self.peers.setdefault(addr, Peer(addr))
        r = BitReader(data)
        try:
            hdr = parse_header(r)
        except ValueError:
            self.log(f"{addr} short packet {data.hex()}")
            return
        if is_new and hdr["count"] > 0:
            first = BitReader(data)
            parse_header(first)
            if first.bits(7) != MSG_OPEN_CONNECTION:
                # a client that survived a server restart: tell it the connection is gone
                self.log(f"{addr[1]} stale peer (no handshake); sending CloseConnection")
                self.send(peer, [close_connection(0)])
                self.peers.pop(addr, None)
                return
        peer.note_packet_id(hdr["ack_id"])
        peer.last_rx = time.monotonic()
        replies = []
        types = []
        for _ in range(hdr["count"]):
            try:
                mtype = r.bits(7)
            except ValueError:
                break
            types.append(mtype)
            if mtype == MSG_OPEN_CONNECTION:
                if peer.connected:
                    # same address reconnecting: new connection, ids restart at 0
                    self.log(f"{addr[1]} reconnect: resetting peer state")
                    self.drop_peer(peer, "reconnect")
                    peer = Peer(addr)
                    self.peers[addr] = peer
                version, game = r.string(), r.string()
                flag2 = r.bits(2)
                player_id = r.bits(64)
                a, b, counter = r.bits(32), r.bits(32), r.bits(32)
                result = RESULT_OK
                if version != VERSION_STRING:
                    result = RESULT_VERSION_MISMATCH
                elif game != GAME_STRING:
                    result = RESULT_GAME_MISMATCH
                peer.client_counter = counter
                self.log(f"{addr[1]} OpenConnection version={version!r} game={game!r} "
                         f"id={player_id:#x} counter={counter} -> result {result}")
                replies.append(open_connection_response(flag2, result, counter, self.server_counter))
            elif mtype == MSG_OPEN_CONNECTION_RESPONSE:
                flag2, code = r.bits(2), r.bits(3)
                echo, theirs = r.bits(32), r.bits(32)
                self.log(f"{addr[1]} OpenConnectionResponse code={code} echo={echo} counter={theirs}")
                if code == RESPONSE_ACK and not peer.connected:
                    peer.connected = True
                    peer.owner_id = self.next_owner
                    self.next_owner = self.next_owner % 250 + 1
                    self.log(f"{addr[1]} *** CONNECTED *** (relay owner id {peer.owner_id})")
                    replies.append(time_sync(0, self.now_ms()))
                    replies.append(connection_sync_done(self.server_counter))
                    replies.append(session_host_create(seq=1))
                    replies.append(weather_create(seq=1))
                    self.log(f"{addr[1]} -> TimeSync, ConnectionSyncDone, SessionHost + Weather create")
            elif mtype == MSG_CLOSE_CONNECTION:
                reason, x, y = r.bits(3), r.bits(32), r.bits(32)
                self.log(f"{addr[1]} CloseConnection reason={reason} {x:#x} {y:#x}")
                self.drop_peer(peer, "CloseConnection")
                return
            elif mtype == MSG_PING:
                value = r.bits(32)
                replies.append(pong(value, self.now_ms()))
            elif mtype == MSG_PONG:
                r.bits(32), r.bits(32)
            elif mtype == MSG_SERVER_TRAFFIC_MESSAGE:
                kind = r.bits(4)
                a, s14, u5, b, flag = r.bits(8), r.bits(14), r.bits(5), r.bits(8), r.bits(1)
                seq = r.bits(32)
                peer.traffic_relayed += 1
                if peer.traffic_relayed <= 12 or peer.traffic_relayed % 200 == 0:
                    self.log(f"{addr[1]} traffic msg kind={kind} id={a} s14={s14} u5={u5} "
                             f"u8={b} flag={flag} seq={seq} -> relay #{peer.traffic_relayed} "
                             f"(assigned id {peer.next_traffic_id - 1 if kind == 7 and a == 255 else a})")
                assigned = a
                if kind == 7 and a == 255:
                    # a spawn request: the authority assigns the traffic car id
                    assigned = peer.next_traffic_id
                    peer.next_traffic_id = (peer.next_traffic_id + 1) % 250
                replies.append(client_traffic_message(kind, assigned, s14, u5, b, flag, seq))
            elif mtype == MSG_FINALISE_SESSION_ENTRY:
                self.log(f"{addr[1]} FinaliseSessionEntry -> echo + FullWeather + "
                         f"Console.SessionEntryFinalised=1")
                replies.append(finalise_session_entry())
                replies.append(full_weather())
                if not peer.entered:
                    peer.entered = True
                    self.replay_to(peer)

                def console_vars(w: BitWriter):
                    w.bits(CONSOLE_VAR_SESSION_ENTRY_FINALISED, CONSOLE_VAR_ID_BITS)
                    w.bits(1, 1)          # SyncedBool
                replies.append(server_update(3, 0, ROUTER_BITS[3][0], peer.console_seq, console_vars))
                peer.console_seq = (peer.console_seq + 1) % 0xFF
                if peer.console_oid is not None:
                    # the other clients see this Console as a relay object: give them the same update
                    vb = BitWriter()
                    console_vars(vb)
                    self.remember_and_relay(peer, MSG_OBJECT_UPDATE, 3, peer.console_oid, None, 0,
                                            vb.pos, vb.value)
            elif mtype == MSG_OBJECT_REQUEST_OWNERSHIP:
                router = r.bits(4)
                if router not in ROUTER_BITS:
                    self.log(f"{addr[1]} RequestOwnership on unmapped router {router}; stop parsing")
                    break
                id_bits, type_bits = ROUTER_BITS[router]
                tid = r.bits(type_bits) if type_bits else 0
                a, b = r.bits(1), r.bits(1)
                oid = r.bits(id_bits)
                self.log(f"{addr[1]} RequestOwnership router={router} type={tid} key={a}{b} id={oid} "
                         f"-> grant")
                replies.append(grant_ownership(router, oid, id_bits))
            elif mtype == MSG_REQUEST_RACE:
                self.log(f"{addr[1]} MsgRequestRace (owner {peer.owner_id}) -> RaceReady=1")
                self.set_session_host_bool(SH_RACE_READY, 1, "race requested")
                self.pending_race = (time.monotonic() + RACE_INVITE_SECONDS, peer)
            elif mtype == MSG_READY_REQUEST:
                a, b = r.bits(1), r.bits(1)
                oid = r.bits(2)
                if a == 0 and b == 1:
                    r.bits(8)
                ready = r.bits(1)
                self.log(f"{addr[1]} MsgReadyRequest key={a}{b} id={oid} ready={ready} -> response 0")
                replies.append(ready_request_response(0))
            elif mtype == MSG_TIME_SYNC:
                r.bits(16), r.bits(32)
                self.log(f"{addr[1]} TimeSync")
            elif mtype == MSG_CONNECTION_SYNC_DONE:
                self.log(f"{addr[1]} ConnectionSyncDone {r.bits(32)}")
            elif mtype in (MSG_OBJECT_CREATE, MSG_OBJECT_UPDATE):
                router = r.bits(4)
                if router not in ROUTER_BITS:
                    self.log(f"{addr[1]} object msg {mtype} on unmapped router {router}; stop parsing "
                             f"bits-left={r.size - r.pos} raw={data.hex()}")
                    break
                id_bits, type_bits = ROUTER_BITS[router]
                a, b = r.bits(1), r.bits(1)
                oid = r.bits(id_bits)
                if a == 0 and b == 1:
                    r.bits(8)             # relay owner id follows the id (FUN_1417b0e40)
                tid = r.bits(type_bits) if (mtype == MSG_OBJECT_CREATE and type_bits) else None
                seq = r.bits(8)
                size = r.bits(14)
                payload = r.bits(size) if size else 0
                self.log(f"{addr[1]} obj {'create' if mtype == 7 else 'update'} router={router} "
                         f"flags={a}{b} id={oid} type={tid} seq={seq} varbits={size}")
                if router == ROUTER_LOBBY and a == 1 and b == 0:
                    # the leader's GameOptionsSynced after ownership transfer: others see it as a
                    # relay object owned by the leader (created once, then updated)
                    first = (router, oid) not in peer.objs
                    self.log(f"{addr[1]} GameOptions {'create' if first else 'update'} relayed "
                             f"({size} bits)")
                    self.remember_and_relay(peer, MSG_OBJECT_CREATE if first else MSG_OBJECT_UPDATE,
                                            router, oid, TYPE_GAME_OPTIONS if first else None, seq,
                                            size, payload)
                elif router in RELAY_ROUTERS and a == 0 and b == 0:
                    self.remember_and_relay(peer, mtype, router, oid, tid, seq, size, payload)
                if router == 3 and mtype == MSG_OBJECT_UPDATE and size:
                    pr = BitReader(payload.to_bytes((size + 7) // 8, "little"))
                    seen = []
                    try:
                        while pr.pos + 5 <= size:
                            vid = pr.bits(CONSOLE_VAR_ID_BITS)
                            if vid not in CONSOLE_BOOL_VAR_NAMES:
                                break
                            val = pr.bits(1)
                            peer.console_vars[vid] = val
                            seen.append(f"{CONSOLE_BOOL_VAR_NAMES[vid]}={val}")
                    except ValueError:
                        pass
                    if seen:
                        self.log(f"{addr[1]} Console vars: {' '.join(seen)}")
                        self.maybe_begin_race()
                if mtype == MSG_OBJECT_CREATE and router == 3:
                    peer.console_oid = oid
                if mtype == MSG_OBJECT_CREATE and router == 3 and not peer.console_acked:
                    peer.console_acked = True
                    replies.append(server_update(router, oid, id_bits, 0))
                    self.log(f"{addr[1]} -> ServerUpdate for client Console object id={oid}")
            elif mtype == MSG_CAR_PREDICTION_FULL:
                a, b = r.bits(1), r.bits(1)
                cid = r.bits(CAR_KEY_ID_BITS)
                rest_bits = r.size - r.pos
                rest = r.bits(rest_bits)
                peer.cars_relayed += 1
                if peer.cars_relayed <= 3 or peer.cars_relayed % 500 == 0:
                    self.log(f"{addr[1]} car prediction #{peer.cars_relayed} flags={a}{b} id={cid} "
                             f"rest={rest_bits} bits -> {len(self.others(peer))} peer(s)")
                if a == 0 and b == 0:
                    msg = relay_car_prediction(cid, peer.owner_id, rest, rest_bits)
                    for q in self.others(peer):
                        self.send(q, [msg])
                break
            else:
                # message layouts beyond the transport layer are not decoded yet
                self.log(f"{addr[1]} id={hdr['ack_id']} types={types} first-unknown={mtype} "
                         f"bits-left={r.size - r.pos} raw={data.hex()}")
                break
        if replies or hdr["ack_id"] != UNRELIABLE_ID:
            self.send(peer, replies)

    def tick(self) -> None:
        """Server-initiated traffic: the client times out after 10 s of silence."""
        now = time.monotonic()
        if self.pending_race and now >= self.pending_race[0]:
            _, leader = self.pending_race
            self.pending_race = None
            if leader.connected:
                self.start_event(leader)
        for peer in list(self.peers.values()):
            if peer.connected and now - peer.last_rx > STALE_SECONDS:
                self.drop_peer(peer, f"no packets for {STALE_SECONDS:.0f} s")
                continue
            if peer.connected and now - peer.last_keepalive >= KEEPALIVE_SECONDS:
                peer.last_keepalive = now
                peer.time_sync_seq = (peer.time_sync_seq + 1) & 0xFFFF
                self.send(peer, [ping(self.now_ms()), time_sync(peer.time_sync_seq, self.now_ms())])

    def serve(self) -> None:
        sel = selectors.DefaultSelector()
        sel.register(self.sock, selectors.EVENT_READ)
        while True:
            for _ in sel.select(timeout=0.2):
                data, addr = self.sock.recvfrom(65535)
                try:
                    self.handle(data, addr)
                except Exception as exc:  # keep serving
                    self.log(f"error handling {data.hex()}: {exc!r}")
            self.tick()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 5000
    logfile = sys.argv[2] if len(sys.argv) > 2 else None
    try:
        Server(port, logfile).serve()
    except KeyboardInterrupt:
        pass
