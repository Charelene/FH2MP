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
import os
import random
import selectors
import socket
import sys
import struct
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
TIME_SYNC_SECONDS = 30.0
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
    9: (4, 2),   # IRoadTripRouter (FUN_1418120c0 registers it as router 9; FUN_141811f90: maxObjects 13 -> 4 id bits,
                 # 3 object types -> 2 type bits). The client owns a RoadTripPlayerSynced here in every session type.
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
RACE_GRID_TIMEOUT_SECONDS = 45.0     # starting grid sent: wait this long for every Console.RenderReady, then go anyway
CONSOLE_VAR_PARTICIPATING_IN_RACE = 2
CONSOLE_BOOL_VAR_NAMES = {0: "RenderReady", 1: "SessionEntryFinalised", 2: "ParticipatingInRace",
                          3: "RaceInProgress", 4: "Ready", 5: "HasLoadedCarsForAllParticipants",
                          6: "HasGrid"}
RACE_INVITE_SECONDS = 20.0          # leader requested a race: wait this long for others, then start
RACE_JOIN_GRACE_SECONDS = 90.0      # ...but keep waiting this long for clients that accepted and are picking a car
MSG_OBJECT_DELETE = 9
MSG_OBJECT_REQUEST_OWNERSHIP = 10   # client -> server: router:4 | typeId | key (A=1,B=0 transfer id)
MSG_OBJECT_GRANT_OWNERSHIP = 11     # server -> client: router:4 | key | seq:8 | size:14 (+size bits)
MSG_CAR_PREDICTION_FULL = 15
COLLISION_TO_RECEIVER_CAR = False   # experiment: apply a reported hit to the receiver's own car (True) or to
                                    # its copy of the sender's car (False, the first version: "better")
MSG_CAR_COLLISION = 21             # key | 6 x u32 (contact point, 3 values)  (FUN_141804e20)
MSG_IN_PROGRESS_SCORE_UPDATE = 43   # key | u32 | bool                       (FUN_1418041b0)
MSG_LAP_SCORE = 44                  # key | 3 x blob | u32 lap | bool | bool  (FUN_141804380)
MSG_SCORE_KEYFRAME = 45             # key | 5 x blob | u32 | 3 x u8 | 3 x bool (FUN_141804660)
RACE_FLAGS_CLEAR_SECONDS = 4.0       # after InRace=0, drop RaceReady/ParticipantsSelected as well
RACE_LAST_FINISHER_SECONDS = 30.0   # after the first finisher the others get this long (client UI: RACE ENDS IN)
CAR_KEY_ID_BITS = 7            # FUN_141808170: object key written with 7 id bits
# Road Trip (router 9): RoadTripObjectTypes 0 RoadTripSynced (server-owned), 1 RoadTripEventSelection,
# 2 RoadTripPlayerSynced (client-owned). RoadTripSynced vars (ctor FUN_141812ae0, 10 vars -> 4-bit var ids);
# widths other than the state enum are assumed (32-bit ints, 1-bit bool) until the client confirms them.
ROUTER_ROADTRIP = 9
RT_TYPE_SYNCED, RT_TYPE_EVENT_SELECTION, RT_TYPE_PLAYER = 0, 1, 2
RT_VAR_ID_BITS = 4
LOBBY_PLAYER_PLAYER_HOST = 1     # var index in Nerd::LobbyPlayer (FUN_14180bf50): Ready 0, PlayerHost 1, ...
RT_INIT_SECONDS = float(os.environ.get("FH2MP_RT_INIT", "3.0"))   # dev knob: hold the state in Initialising         # Initialising -> DestinationVote delay after the connection (the client waits in Initialising
                              # and only sends its session-entry message once the state has changed)
RT_EXCLUDED_MODES = {25, 26}     # King, Infection: party modes, not offered for now (RaceModes.id)
RT_CAR_CLASSES = 6               # CarClasses 0..5 = D C B A S1 S2 (6 = X and 7 = R are rental/event-specific)
RT_EXCLUDED_LOCATIONS = {9}      # Storm Island: expansion content, not offered for now
RT_VOTE_MS = 60 * 1000          # length of the destination vote
RT_ALL_VOTED_MS = 10 * 1000      # everybody voted: close the vote after this long
RT_ALL_PICKED_MS = 3 * 1000      # everybody picked a car: end car select after this long
RT_CAR_SELECT_MS = 30 * 1000     # car select phase length
RT_DRIVE_MS = 180 * 1000         # time allowed to drive to the event; the route starts wherever the car is (the free-roam
                                 # airport the first time, then the last hub), ~5-6 km for a far destination
RT_PREPARE_MS = 2 * 60 * 1000    # prepare-for-event phase length
RT_FINALISE_MS = 240 * 1000      # fallback only: the next drive starts once every participant has left the event
RT_FIRST_ARRIVED_MS = 30 * 1000  # once one player has arrived, the others get this long to follow
MSG_RT_CONFIGURE_GAME_OPTIONS = 50   # server -> leader: u32 | u32          ([ConfigureGameOptionsForRoadTripEvent])
MSG_RT_SELECTED_NEXT_EVENT = 49      # leader -> server: bool                ([SelectedNextRoadTripEvent])
MSG_RT_CONFIGURED_GAME_OPTIONS = 51  # leader -> server: u32 | u16           ([ConfiguredGameOptionsForRoadTripEvent])
MSG_VOTE_FOR_TRACK = 47          # lobby-event key (idBits 2) | location index:32     (FUN_141811f20)
LOBBY_TYPE_ROAD_TRIP = 1      # Nerd::NetworkLobbyType (FUN_1401123b0); width bit_length(7) = 3, assumed
RT_VARS = {"CurrentLocationId": (0, 32), "CompletedEventCount": (1, 3), "EventRaceId": (2, 32),
           "NextRaceId": (3, 32), "MinimumDriveToDestinationReward": (4, 14),
           "MaximumDriveToDestinationReward": (5, 14), "MaximumDriveToDestinationRewardEarntBefore": (6, 32),
           "DecreaseXPEnabled": (7, 1), "State": (8, 4), "StateEndTime": (9, 32)}
# RoadTripServerState (FUN_140113da0)
RT_INITIALISING, RT_DESTINATION_VOTE, RT_CAR_SELECT, RT_STATE_3, RT_DRIVE_TO_EVENT = 0, 1, 2, 3, 4
RT_ARRIVED_AT_DESTINATION, RT_PREPARE_FOR_EVENT, RT_IN_EVENT, RT_FINALISE_EVENT = 5, 6, 7, 8
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


def ping_live(clock):
    """Ping stamped when the packet is built, i.e. after send() has finished any deliberate delay."""
    def write(w: BitWriter):
        w.bits(MSG_PING, 7)
        w.bits(clock(), 32)
    return write


def time_sync_live(seq: int, clock):
    def write(w: BitWriter):
        w.bits(MSG_TIME_SYNC, 7)
        w.bits(seq, 16)
        w.bits(clock(), 32)
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


def request_race_relay():
    """Server -> other clients: the leader requested a race. The client's MsgRequestRace Receive handler
    (FUN_1417f6fb0) calls CLobby slot 9, which fires NetworkRaceRequestingParticipants and pops the
    "event invite" prompt; accepting it sets Console.Ready (network.ready_for_race)."""
    def write(w: BitWriter):
        w.bits(MSG_REQUEST_RACE, 7)
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


def transfer_update_message(router, obj_id, id_bits, seq, size, payload):
    """Update of a *transfer object* (key A=1, B=0): an object that exists on every client from the start
    (e.g. the lobby's GameOptions) and whose authority moves between clients. The leader writes it with
    this key and the server forwards the same key to everybody else."""
    def write(w: BitWriter):
        w.bits(MSG_OBJECT_UPDATE, 7)
        w.bits(router, 4)
        w.bits(1, 1)
        w.bits(0, 1)
        w.bits(obj_id, id_bits)
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
        w.bits(0, LOBBY_EVENT_KEY_BITS)
        w.bits(len(entries), 4)
        for kind, cid, owner in entries:
            if kind == "local":
                w.bits(0, 1)   # no flags: the receiver's own car is the plain id (the key FUN_140a31140 gets
                w.bits(0, 1)   # from the local car, probed at runtime) and matches by position -> grid slot
                w.bits(cid, CAR_KEY_ID_BITS)
            else:
                w.bits(0, 1)
                w.bits(1, 1)
                w.bits(cid, CAR_KEY_ID_BITS)
                w.bits(owner, 8)
    return write


LOBBY_EVENT_KEY_BITS = 4   # RouterEventMessageHandler<ILobbyRouter,ILobbyEvents> prefixes an object key:
                           # flagA:1 | flagB:1 | id:2 (FUN_1418091c0/1418092b0); all zero = server-owned id 0,
                           # the same key the client puts in its own MsgReadyRequest


def start_race(start_ms):
    def write(w: BitWriter):
        w.bits(MSG_START_RACE, 7)
        w.bits(0, LOBBY_EVENT_KEY_BITS)
        w.bits(start_ms & 0xFFFFFFFF, 32)
    return write


def ready_request_response(code):
    def write(w: BitWriter):
        w.bits(MSG_READY_REQUEST_RESPONSE, 7)
        w.bits(0, LOBBY_EVENT_KEY_BITS)
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


def relay_score_message(msg_type, car_id, owner, rest_value, rest_bits):
    """Score messages carry the sender's car key (own object: flags 00) followed by the payload; others see
    that car as a relay object (A=0,B=1,id,owner)."""
    def write(w: BitWriter):
        w.bits(msg_type, 7)
        w.bits(0, 1)
        w.bits(1, 1)
        w.bits(car_id, CAR_KEY_ID_BITS)
        w.bits(owner, 8)
        w.value |= rest_value << w.pos
        w.pos += rest_bits
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


def road_trip_vars(**values):
    """Writer for RoadTripSynced variables, by name (see RT_VARS)."""
    def write(w: BitWriter):
        for name, value in values.items():
            vid, width = RT_VARS[name]
            w.bits(vid, RT_VAR_ID_BITS)
            w.bits(int(value), width)
    return write


# RoadTripEventSelectionSynced (ctor FUN_141813f80; layout read from a live client): 61 vars, 7-bit var ids.
#   var 0            LocationIds   SyncedArray<long,3>             3 x 32 bits
#   vars 1..30       3 x block of 10 (the vote), vars 31..60 another 3 blocks (not used by the vote screen)
#   block (10 vars)  4 x [event id: 32 bits, SyncedInt<uchar 0..12>: 4 bits], CarClass: 3 bits, VoteCount: 5 bits
RT_ES_VAR_ID_BITS = 7
RT_ES_OBJECT_ID = 1            # server-owned id on router 9 (RoadTripSynced is id 0)


def road_trip_event_selection_vars(locations, events, classes=(0, 0, 0), mins=None, votes=(0, 0, 0)):
    """locations: 3 RoadTripLocation ids; events: 3 lists of 4 AnthemOnlineEvent ids; votes: tally per location."""
    def write(w: BitWriter):
        w.bits(0, RT_ES_VAR_ID_BITS)
        for loc in locations:
            w.bits(loc & 0xFFFFFFFF, 32)
        for first_var in (1, 31):
            for b in range(3):
                base = first_var + 10 * b
                for k in range(4):
                    w.bits(base + 2 * k, RT_ES_VAR_ID_BITS)
                    w.bits(events[b][k] & 0xFFFFFFFF, 32)
                    w.bits(base + 2 * k + 1, RT_ES_VAR_ID_BITS)
                    w.bits(mins[b][k] if mins else 0, 4)       # RoadTripEvent.MinimumPlayers
                w.bits(base + 8, RT_ES_VAR_ID_BITS)
                w.bits(classes[b], 3)   # CarClass: SyncedEnum<CarClassSynced,0,3,0> is 3 bits wide
                w.bits(base + 9, RT_ES_VAR_ID_BITS)
                w.bits(votes[b], 5)     # VoteCount
    return write


def road_trip_event_selection_update(seq, locations, events, classes=(0, 0, 0), mins=None, votes=(0, 0, 0)):
    """Fill the client's RoadTripEventSelection *transfer object* (key A=1,B=0, id 0): the vote screen reads that
    instance (probed live: key 0x80000000), not a server-owned one. Same mechanism as the lobby's GameOptions."""
    body = BitWriter()
    road_trip_event_selection_vars(locations, events, classes, mins, votes)(body)
    return transfer_update_message(ROUTER_ROADTRIP, 0, 4, seq, body.pos, body.value)


def road_trip_vote_counts_update(seq, votes):
    """Only the three VoteCount variables (var 10/20/30 of the vote blocks, and their twins 40/50/60)."""
    body = BitWriter()
    for first_var in (1, 31):
        for b in range(3):
            body.bits(first_var + 10 * b + 9, RT_ES_VAR_ID_BITS)
            body.bits(votes[b], 5)
    return transfer_update_message(ROUTER_ROADTRIP, 0, 4, seq, body.pos, body.value)


def pick_road_trip_candidates(gamedb_path, players=1, rng=None):
    """3 random road-trip locations, 4 events each from the game's own event table, mixing race types where the
    location has them, a different car class per location. Only race modes that can run with `players` players
    are offered (RaceModes.MinimumPlayers: King and Infection need 2, a P2P or lap race 1)."""
    import random
    import sqlite3
    rng = rng or random.Random()
    db = sqlite3.connect(f"file:{gamedb_path}?mode=ro&immutable=1", uri=True)
    try:
        rows = db.execute("SELECT e.AnthemOnlineEventId, e.RoadTripLocationId, e.RaceModeId, m.MinimumPlayers "
                          "FROM AnthemOnlineEvents e JOIN RaceModes m ON m.id = e.RaceModeId "
                          "WHERE e.RoadTripLocationId > 0 AND m.MinimumPlayers <= ? AND e.RaceModeId NOT IN (%s)"
                          % ",".join(str(m) for m in RT_EXCLUDED_MODES), (max(1, players),)).fetchall()
    finally:
        db.close()
    by_loc = {}
    min_players = {}
    for ev, loc, mode, minp in rows:
        by_loc.setdefault(loc, {}).setdefault(mode, []).append(ev)
        min_players[ev] = minp
    eligible = [l for l, modes in by_loc.items()
                if l not in RT_EXCLUDED_LOCATIONS and sum(len(v) for v in modes.values()) >= 4]
    locs = rng.sample(eligible, 3)
    events = []
    for l in locs:
        pools = {m: rng.sample(v, len(v)) for m, v in by_loc[l].items()}
        picked = []
        while len(picked) < 4:                      # round-robin over the race types, shuffled
            for m in rng.sample(list(pools), len(pools)):
                if pools[m] and len(picked) < 4:
                    picked.append(pools[m].pop())
        events.append(picked)
    classes = rng.sample(range(RT_CAR_CLASSES), 3)   # a different car class per location
    mins = [[min_players[e] for e in row] for row in events]
    return locs, events, classes, mins


def road_trip_configure_game_options(a: int, b: int):
    def write(w: BitWriter):
        w.bits(MSG_RT_CONFIGURE_GAME_OPTIONS, 7)
        w.bits(a & 0xFFFFFFFF, 32)
        w.bits(b & 0xFFFFFFFF, 32)
    return write


def road_trip_create(seq=1, **values):
    """The server-owned RoadTripSynced (router 9, type 0, id 0)."""
    return object_message(MSG_OBJECT_CREATE, ROUTER_ROADTRIP, 0, RT_TYPE_SYNCED, seq, 4, 2,
                          road_trip_vars(**values))


def road_trip_update(seq, **values):
    return object_message(MSG_OBJECT_UPDATE, ROUTER_ROADTRIP, 0, 0, seq, 4, 0, road_trip_vars(**values))


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
        self.rt_seq = 1            # Road Trip: seq of the last message on the server-owned RoadTripSynced
        self.rt_started = False    # Road Trip: Initialising -> DestinationVote already sent
        self.connected_at = 0.0
        self.es_seq = 0            # server updates to the client's RoadTripEventSelection transfer object
        self.player_seq = 0        # server updates to this client's LobbyPlayer object (router 1)
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
        self.gopts_seq = 0         # numbering of the server-owned GameOptions object sent to this client
        self.gopts_created = False
        self.console_oid = None    # id of this client's ConsoleSynced object (router 3)
        self.last_rx = time.monotonic()
        self.last_time_sync = time.monotonic()    # the connect handshake sends the first TimeSync
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
    def __init__(self, port: int, logpath, roadtrip: bool = False):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", port))
        self.log_file = open(logpath, "a", buffering=1) if logpath else None
        self.rt_state = RT_INITIALISING   # shared Road Trip session state
        self.rt_end_ms = 0
        self.rt_votes = {}                # Peer -> location index
        self.rt_winner = None
        self.rt_cfg_deadline = 0.0
        self.rt_drive_not_before = 0.0
        self.rt_drive_wait = False        # DriveToEvent sent, countdown (StateEndTime) held until the clients wait for it
        self.rt_drive_deadline = 0.0
        self.rt_drive_ready_at = 0.0
        self.rt_idx = 0                   # which of the destination's 4 events is next
        self.rt_pending_drive = False     # next event of the block: waiting for the leader's game options
        self.rt_picked = set()            # peers whose car pick was seen during CarSelect
        self.rt_configured = False        # the leader answered ConfigureGameOptionsForRoadTripEvent for this round
        self.rt_gamedb = None
        self.rt_candidates = None  # (location ids, events per location) for the Road Trip vote
        self.roadtrip = roadtrip   # serve a Road Trip session (router 9 RoadTripSynced) instead of Free Roam
        self.rt_seq = 1
        self.server_counter = 1
        self.start = time.monotonic()
        self.peers = {}
        self.next_owner = 1
        self.pending_race = None   # (deadline, leader Peer)
        self.game_options = None   # (size, payload) latest GameOptionsSynced written by the leader
        self.event = None          # {"people": [Peer], "begun": bool, "start_at": float|None}
        self.log(f"listening on udp/{port} version={VERSION_STRING} game={GAME_STRING}")
        if roadtrip:
            gamedb = os.environ.get("FH2MP_GAMEDB")
            if gamedb:
                self.rt_gamedb = gamedb
                self.rt_candidates = pick_road_trip_candidates(gamedb)
                self.log(f"road trip candidates: locations {self.rt_candidates[0]} events {self.rt_candidates[1]} "
                         f"classes {self.rt_candidates[2]}")
            else:
                self.log("road trip: FH2MP_GAMEDB (path to the game's gamedbRC.slt) not set; the vote will be empty")

    def now_ms(self) -> int:
        return int((time.monotonic() - self.start) * 1000) & 0xFFFFFFFF

    def log(self, msg: str) -> None:
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        if self.log_file:
            self.log_file.write(line + "\n")

    def send(self, peer: Peer, messages) -> None:
        # The client measures RTT in whole milliseconds and discards 0 ms as invalid, which makes
        # it fall back to a 100 ms retransmit timer. Hold a reply back until ~2 ms after that client's last
        # packet so RTT reads >= 1. Packets relayed from other clients do not need it and must not stall
        # the loop: a fixed sleep per relayed car update added latency and jitter to every remote car.
        gap = time.monotonic() - peer.last_rx
        if gap < 0.002:
            time.sleep(0.002 - gap)
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

    GAME_OPTIONS_TRANSFER_ID = 0   # transfer-object id of the lobby's GameOptions (router 6, type 1)

    def send_game_options(self, q: Peer) -> None:
        """Forwards the leader's GameOptions to a client under the same transfer-object key the leader wrote
        it with (A=1,B=0,id 0). That object already exists in every client's lobby (CGameOptions); the update
        fills in its synced variables, including the selected event id the fast-travel state needs."""
        if not self.game_options:
            return
        size, payload = self.game_options
        msg = transfer_update_message(ROUTER_LOBBY, self.GAME_OPTIONS_TRANSFER_ID, LOBBY_ID_BITS,
                                      q.gopts_seq, size, payload)
        q.gopts_seq = (q.gopts_seq + 1) % 0xFF
        self.send(q, [msg])
        self.log(f"{q.addr[1]} <- GameOptions forwarded as transfer-object update ({size} bits, seq "
                 f"{(q.gopts_seq - 1) % 0xFF})")

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

    def end_event(self, why: str) -> None:
        """Back to the idle lobby: clear the SessionHost race flags so the next event starts clean."""
        self.event = None
        self.pending_race = None
        for var in (SH_IN_RACE, SH_RACE_PARTICIPANTS_SELECTED, SH_RACE_READY):
            self.set_session_host_bool(var, 0, f"event over: {why}")
        for q in self.peers.values():
            q.console_vars.pop(4, None)
        if self.roadtrip:
            self.rt_next_event()

    def rt_numlaps(self, race: int) -> int:
        """Laps of a Road Trip event (AnthemOnlineEvents.NumLaps in the game's own database)."""
        import sqlite3
        try:
            db = sqlite3.connect(f"file:{self.rt_gamedb}?mode=ro&immutable=1", uri=True)
            try:
                row = db.execute("SELECT NumLaps FROM AnthemOnlineEvents WHERE AnthemOnlineEventId = ?",
                                 (race,)).fetchone()
            finally:
                db.close()
            return max(1, row[0]) if row else 1
        except Exception:
            return 1

    def rt_race(self) -> int:
        return self.rt_candidates[1][self.rt_winner][self.rt_idx]

    def rt_next_event(self) -> None:
        """A destination is a block of four events: vote -> drive -> event 1 -> drive -> event 2 ... then vote again."""
        if self.rt_winner is None or self.rt_idx + 1 >= len(self.rt_candidates[1][self.rt_winner]):
            self.rt_new_round()
            return
        self.rt_idx += 1
        race = self.rt_race()
        self.log(f"road trip: event {self.rt_idx + 1}/{len(self.rt_candidates[1][self.rt_winner])} of the destination "
                 f"is {race}")
        self.rt_pending_drive = True
        self.rt_end_ms = 0
        self.rt_send_configure()
        self.rt_cfg_deadline = time.monotonic() + 12.0
        self.rt_drive_not_before = time.monotonic() + 6.0      # let the clients finish returning to free roam
        self.rt_broadcast(CompletedEventCount=self.rt_idx, NextRaceId=race)

    def rt_new_round(self) -> None:
        """The event is over (or abandoned): a fresh destination vote, like the game's own loop."""
        if self.rt_gamedb:
            here = sum(1 for c in self.peers.values() if c.connected)
            self.rt_candidates = pick_road_trip_candidates(self.rt_gamedb, here)
        self.rt_votes.clear()
        self.rt_winner = None
        self.rt_idx = 0
        self.rt_pending_drive = False
        self.rt_state = RT_DESTINATION_VOTE
        self.rt_end_ms = self.now_ms() + RT_VOTE_MS
        self.log(f"road trip: new round, candidates {self.rt_candidates[0] if self.rt_candidates else None}")
        for q in self.peers.values():
            if not (q.connected and q.rt_started):
                continue
            msgs = []
            if self.rt_candidates:
                msgs.append(road_trip_event_selection_update(q.es_seq, *self.rt_candidates, votes=self.rt_counts()))
                q.es_seq = (q.es_seq + 1) % 0xFF
            q.rt_seq = (q.rt_seq + 1) % 0xFF
            msgs.append(road_trip_update(q.rt_seq, CompletedEventCount=0, State=self.rt_state, StateEndTime=self.rt_end_ms))
            self.send(q, msgs)

    def check_invitees_ready(self) -> None:
        others_waiting = [q for q in self.others(self.pending_race[1])
                          if q.connected and q.entered and not q.console_vars.get(4)]
        if not others_waiting:   # everybody invited has accepted: no need to wait out the window
            self.pending_race = (min(self.pending_race[0], time.monotonic() + 1.5), self.pending_race[1])
            self.log("all invited clients ready -> starting the event now")

    def start_event(self, leader: Peer) -> None:
        """Participants = the leader plus every client that reported Console.Ready."""
        people = [leader] + [q for q in self.peers.values()
                             if q is not leader and q.connected and q.console_vars.get(4)]
        self.log(f"event start: {len(people)} participant(s): {[q.owner_id for q in people]}")
        for q in self.peers.values():
            if q is not leader:
                self.log(f"  other client owner {q.owner_id}: connected={q.connected} Console vars={q.console_vars}")
        self.event = {"people": people, "begun": False, "start_at": None,
                      "numlaps": self.rt_numlaps(self.rt_race()) if self.roadtrip and self.rt_winner is not None
                      and self.rt_gamedb else None}
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
        self.log(f"all {len(ev['people'])} participant(s) loaded cars -> starting grid")
        # MsgStartingGrid (39) moves the participants onto the event route (its ILobbyEvents handler,
        # FUN_140a2e130, raises NetworkSwitchRoute). It must arrive BEFORE SessionHost.InRace: a client
        # still waiting in wait_for_event_to_start sees InRace=1 and returns to free roam instead.
        order = list(ev["people"])
        random.shuffle(order)                   # grid slots are drawn, not by join order
        for q in ev["people"]:
            entries = [("local", 0, 0) if p is q else ("relay", 0, p.owner_id) for p in order]
            self.send(q, [starting_grid(entries)])
        for q in ev["people"]:
            q.console_vars.pop(0, None)     # RenderReady is re-reported by each client once its grid is up
        ev["go_at"] = time.monotonic() + RACE_GRID_TIMEOUT_SECONDS

    def maybe_go_race(self) -> None:
        """Everyone's starting-grid screen is up (Console.RenderReady): release InRace + MsgStartRace."""
        ev = self.event
        if ev and ev.get("go_at") and all(q.connected and q.console_vars.get(0) for q in ev["people"]):
            self.log("all participants render-ready")
            self.go_race()

    def note_lap_score(self, peer: Peer, lap: int, blobs) -> None:
        """MsgLapScore = (lap time, best lap, total time, lap number). The lap count is in GameOptions (not
        decoded), so only a lap whose time equals the total time counts as the finish: a one-lap event. Circuit
        events (a lap-0 start segment precedes lap 1) are ended by the players leaving instead."""
        ev = self.event
        if not ev or not ev.get("racing") or peer not in ev["people"] or peer in ev["finished"]:
            return
        lap_t, _, total_t = (struct.unpack("<d", x.to_bytes(8, "little"))[0] for x in blobs)
        laps = ev.get("numlaps")
        if laps:
            if lap < laps:       # Road Trip event: the lap count comes from the event table
                return
        elif lap < 1 or abs(lap_t - total_t) > 1e-6:
            return
        ev["finished"][peer] = time.monotonic()
        self.log(f"{peer.addr[1]} finished in {total_t:.3f} s ({len(ev['finished'])}/{len(ev['people'])} finished)")

    def check_race_end(self, now: float) -> None:
        ev = self.event
        if ev and ev.get("clear_at") and now >= ev["clear_at"]:
            ev["clear_at"] = None      # the "event is about to start" HUD follows RaceReady/ParticipantsSelected
            for var in (SH_RACE_PARTICIPANTS_SELECTED, SH_RACE_READY):
                self.set_session_host_bool(var, 0, "race over: lobby flags cleared")
        if not ev or not ev.get("racing") or ev.get("over"):
            return
        fin = ev["finished"]
        if not fin:
            return
        if all(q in fin for q in ev["people"] if q.connected):
            self.end_race("everybody finished")
        elif now >= min(fin.values()) + RACE_LAST_FINISHER_SECONDS:
            self.end_race("time limit after the first finisher")

    def end_race(self, why: str) -> None:
        """SessionHost.InRace=0 is what the clients' NetworkWaitForRaceEnd waits for (FUN_1408eeb20)."""
        self.event["over"] = True
        self.event["racing"] = False
        self.event["clear_at"] = time.monotonic() + RACE_FLAGS_CLEAR_SECONDS
        self.set_session_host_bool(SH_IN_RACE, 0, f"race over: {why}")
        if self.roadtrip:
            self.rt_state = RT_FINALISE_EVENT
            self.rt_end_ms = self.now_ms() + RT_FINALISE_MS
            # the results screens show the number of COMPLETED events ("EVENT n/4"), so it counts up here
            self.rt_broadcast(CompletedEventCount=min(self.rt_idx + 1, 4), State=RT_FINALISE_EVENT,
                              StateEndTime=self.rt_end_ms)

    def go_race(self) -> None:
        ev = self.event
        ev["go_at"] = None
        ev["racing"] = True
        if self.roadtrip and self.rt_state != RT_IN_EVENT:
            self.rt_state = RT_IN_EVENT
            self.rt_broadcast(State=RT_IN_EVENT, StateEndTime=0)
        ev["finished"] = {}
        self.set_session_host_bool(SH_IN_RACE, 1, "race begun")
        go = (self.now_ms() + RACE_COUNTDOWN_MS) & 0xFFFFFFFF
        self.log(f"MsgStartRace start time {go} (server clock now {self.now_ms()})")
        for q in ev["people"]:
            self.send(q, [start_race(go)])

    def drop_peer(self, peer: Peer, why: str) -> None:
        self.peers.pop(peer.addr, None)
        self.rt_votes.pop(peer, None)
        if self.roadtrip and not any(c.connected for c in self.peers.values()):
            self.rt_state, self.rt_end_ms, self.rt_winner = RT_INITIALISING, 0, None   # the next group starts fresh
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
                    peer.connected_at = time.monotonic()
                    peer.owner_id = self.next_owner
                    self.next_owner = self.next_owner % 250 + 1
                    self.log(f"{addr[1]} *** CONNECTED *** (relay owner id {peer.owner_id})")
                    replies.append(time_sync(0, self.now_ms()))
                    replies.append(connection_sync_done(self.server_counter))
                    if self.roadtrip:
                        # SessionHost.LobbyType (var 0, enum NetworkLobbyType: 1 RoadTrip, 2 FreeRoam, 3 CarMeet, ...):
                        # the client loads the session entity group registered for this value (FUN_140804210)
                        def lobby_type(w: BitWriter):
                            w.bits(0, SESSION_HOST_VAR_ID_BITS)
                            w.bits(LOBBY_TYPE_ROAD_TRIP, 3)
                        replies.append(session_host_create(seq=1, write_vars=lobby_type))
                    else:
                        replies.append(session_host_create(seq=1))
                    replies.append(weather_create(seq=1))
                    self.log(f"{addr[1]} -> TimeSync, ConnectionSyncDone, SessionHost + Weather create")
                    if self.roadtrip:
                        # Created in Initialising; the switch to DestinationVote follows from tick() once the client has
                        # entered. The leader's client picks the vote candidates when it sees that state *change*.
                        replies.append(road_trip_create(seq=peer.rt_seq, State=RT_INITIALISING))
                        self.log(f"{addr[1]} -> RoadTripSynced create (State=Initialising)")
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
                    self.send_game_options(peer)

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
                self.race_hard_deadline = time.monotonic() + RACE_JOIN_GRACE_SECONDS
                invited = [q for q in self.others(peer) if q.connected and q.entered]
                for q in invited:
                    self.send(q, [request_race_relay()])
                self.log(f"MsgRequestRace relayed to {len(invited)} other client(s): "
                         f"{[q.owner_id for q in invited]} (invite window {RACE_INVITE_SECONDS:.0f} s)")
            elif mtype == MSG_READY_REQUEST:
                a, b = r.bits(1), r.bits(1)
                oid = r.bits(2)
                if a == 0 and b == 1:
                    r.bits(8)
                ready = r.bits(1)
                self.log(f"{addr[1]} MsgReadyRequest key={a}{b} id={oid} ready={ready} -> response 0")
                replies.append(ready_request_response(0))
                if ready and self.roadtrip and self.rt_state in (RT_DRIVE_TO_EVENT, RT_PREPARE_FOR_EVENT) and not self.event:
                    # a player that arrives first reports ready while the others are still driving
                    self.set_console_bool(peer, 4, 1, "ready request accepted")
                    peer.console_vars[4] = 1
                    if self.rt_state == RT_PREPARE_FOR_EVENT:
                        self.rt_check_ready()
                if ready and self.pending_race and peer is not self.pending_race[1]:
                    # the client's accept is the MsgReadyRequest; the authority records it as Console.Ready
                    self.set_console_bool(peer, 4, 1, "ready request accepted")
                    peer.console_vars[4] = 1
                    self.check_invitees_ready()
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
                if router == 1 and mtype == MSG_OBJECT_UPDATE and size >= 1000 and self.roadtrip:
                    self.rt_car_picked(peer)
                if router == ROUTER_ROADTRIP and mtype == MSG_OBJECT_UPDATE and a == 0 and b == 0 and self.roadtrip:
                    pr = BitReader(payload.to_bytes((size + 7) // 8, "little"))
                    seen_vars = []
                    try:
                        while pr.pos + 3 <= size:
                            vid = pr.bits(3)
                            seen_vars.append(("State", pr.bits(4)) if vid == 0 else
                                             (("CompletedEventCount" if vid == 1 else "XPOnDestinationJoin"), pr.bits(32)))
                    except ValueError:
                        pass
                    self.log(f"{addr[1]} RoadTripPlayerSynced update seq={seq}: {seen_vars} ({size} bits)")
                    for name, val in seen_vars:
                        if name == "State":
                            peer.rt_pstate = val
                    if self.rt_state == RT_DRIVE_TO_EVENT:
                        voters = [q for q in self.peers.values() if q.connected and q.rt_started]
                        arrived = [q for q in voters if getattr(q, "rt_pstate", 0) >= RT_ARRIVED_AT_DESTINATION]
                        if voters and len(arrived) == len(voters):
                            self.rt_end_ms = self.now_ms()      # everybody arrived: the drive is over now
                            self.log("everybody arrived: ending the drive")
                        elif arrived and self.rt_end_ms > self.now_ms() + RT_FIRST_ARRIVED_MS:
                            self.rt_end_ms = self.now_ms() + RT_FIRST_ARRIVED_MS
                            self.rt_broadcast(StateEndTime=self.rt_end_ms)
                            self.log(f"first arrival: the others get {RT_FIRST_ARRIVED_MS // 1000} s")
                if router == ROUTER_LOBBY and a == 1 and b == 0:
                    # the leader's GameOptionsSynced after ownership transfer: others see it as a
                    # relay object owned by the leader (created once, then updated)
                    first = (router, oid) not in peer.objs
                    self.log(f"{addr[1]} GameOptions {'create' if first else 'update'} relayed "
                             f"({size} bits)")
                    # debug: raw bits (LSB first) and where small ids (e.g. the event id 1412 = 0x584) appear
                    bitstr = "".join(str((payload >> i) & 1) for i in range(size))
                    self.log(f"  GameOptions payload hex={payload.to_bytes((size + 7) // 8, 'little').hex()}")
                    for width in range(8, 17):
                        for off in range(0, max(0, size - width + 1)):
                            if (payload >> off) & ((1 << width) - 1) == 1412:
                                self.log(f"  GameOptions: value 1412 at bit offset {off} width {width}")
                    self.game_options = (size, payload)
                    for q in self.others(peer):
                        self.send_game_options(q)
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
                        if self.event and peer in self.event["people"] and peer.console_vars.get(3) == 0 \
                                and "RaceInProgress=0" in seen:
                            self.event["people"].remove(peer)
                            if not self.event["people"]:
                                self.end_event("every participant left the event")
                        self.maybe_begin_race()
                        self.maybe_go_race()
                        if self.pending_race and peer is not self.pending_race[1] and peer.console_vars.get(4):
                            self.check_invitees_ready()
                if mtype == MSG_OBJECT_CREATE and router == 3:
                    peer.console_oid = oid
                if mtype == MSG_OBJECT_CREATE and router == 3 and not peer.console_acked:
                    peer.console_acked = True
                    replies.append(server_update(router, oid, id_bits, 0))
                    self.log(f"{addr[1]} -> ServerUpdate for client Console object id={oid}")
            elif mtype == MSG_RT_SELECTED_NEXT_EVENT:
                self.log(f"{addr[1]} SelectedNextRoadTripEvent ok={r.bits(1)}")
            elif mtype == MSG_RT_CONFIGURED_GAME_OPTIONS:
                self.log(f"{addr[1]} ConfiguredGameOptionsForRoadTripEvent a={r.bits(32)} b={r.bits(16)}")
                self.rt_configured = True
            elif mtype == MSG_VOTE_FOR_TRACK:
                a, b = r.bits(1), r.bits(1)
                r.bits(2)
                if a == 0 and b == 1:
                    r.bits(8)
                self.rt_vote(peer, r.bits(32))
            elif mtype == MSG_CAR_COLLISION:
                # The sender reports a hit on its own car (key flags 00); the other clients apply it to
                # their copy of that car, so a pushed car actually moves instead of being a fixed obstacle.
                a, b = r.bits(1), r.bits(1)
                cid = r.bits(CAR_KEY_ID_BITS)
                if a == 0 and b == 1:
                    r.bits(8)
                rest = r.bits(192)
                if COLLISION_TO_RECEIVER_CAR:
                    def msg(w, cid=cid, rest=rest):     # key left as the plain id: the receiver's own car
                        w.bits(MSG_CAR_COLLISION, 7)
                        w.bits(0, 2)
                        w.bits(cid, CAR_KEY_ID_BITS)
                        w.value |= rest << w.pos
                        w.pos += 192
                else:
                    msg = relay_score_message(mtype, cid, peer.owner_id, rest, 192)
                for q in self.others(peer):
                    self.send(q, [msg])
                peer.collisions_relayed = getattr(peer, "collisions_relayed", 0) + 1
                if peer.collisions_relayed <= 3 or peer.collisions_relayed % 200 == 0:
                    self.log(f"{addr[1]} car collision #{peer.collisions_relayed} relayed")
            elif mtype in (MSG_IN_PROGRESS_SCORE_UPDATE, MSG_LAP_SCORE, MSG_SCORE_KEYFRAME):
                a, b = r.bits(1), r.bits(1)
                cid = r.bits(CAR_KEY_ID_BITS)
                if a == 0 and b == 1:
                    r.bits(8)
                start = r.pos
                blobs = []

                def blob():
                    n = r.varlen()
                    blobs.append(r.bits(n * 8))
                if mtype == MSG_IN_PROGRESS_SCORE_UPDATE:
                    r.bits(32), r.bits(1)
                elif mtype == MSG_LAP_SCORE:
                    blob(), blob(), blob()
                    lap = r.bits(32)
                    r.bits(2)
                    self.log(f"{addr[1]} LapScore lap={lap} times="
                             f"{[round(struct.unpack('<d', x.to_bytes(8, 'little'))[0], 3) for x in blobs]}")
                else:
                    for _ in range(5):
                        blob()
                    r.bits(32 + 24 + 3)
                end = r.pos
                r.pos = start
                rest = r.bits(end - start)
                msg = relay_score_message(mtype, cid, peer.owner_id, rest, end - start)
                for q in self.others(peer):
                    self.send(q, [msg])
                if mtype == MSG_LAP_SCORE:
                    self.note_lap_score(peer, lap, blobs)
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

    # ---- Road Trip session (router 9): the server owns RoadTripSynced and tallies the votes ----
    def rt_broadcast(self, **values) -> None:
        for q in self.peers.values():
            if q.connected and q.rt_started:
                q.rt_seq = (q.rt_seq + 1) % 0xFF
                self.send(q, [road_trip_update(q.rt_seq, **values)])

    def rt_counts(self):
        counts = [0, 0, 0]
        for idx in self.rt_votes.values():
            if 0 <= idx < 3:
                counts[idx] += 1
        return counts

    def rt_vote(self, peer: Peer, idx: int) -> None:
        if self.rt_state != RT_DESTINATION_VOTE or not 0 <= idx < 3:
            return
        self.rt_votes[peer] = idx
        counts = self.rt_counts()
        self.log(f"{peer.addr[1]} voted for location index {idx} ({self.rt_candidates[0][idx]}); tally {counts}")
        for q in self.peers.values():
            if q.connected and q.rt_started:
                self.send(q, [road_trip_vote_counts_update(q.es_seq, counts)])
                q.es_seq = (q.es_seq + 1) % 0xFF
        voters = [q for q in self.peers.values() if q.connected and q.rt_started]
        if voters and all(q in self.rt_votes for q in voters):
            self.rt_end_ms = min(self.rt_end_ms, self.now_ms() + RT_ALL_VOTED_MS)
            self.rt_broadcast(StateEndTime=self.rt_end_ms)
            self.log("everybody voted: closing the vote shortly")

    def rt_car_picked(self, peer: Peer) -> None:
        """A client's car pick during CarSelect shows up as a large update of its player object (router 1)."""
        if self.rt_state != RT_CAR_SELECT:
            return
        self.rt_picked.add(peer)
        voters = [q for q in self.peers.values() if q.connected and q.rt_started]
        if voters and all(q in self.rt_picked for q in voters):
            self.rt_end_ms = min(self.rt_end_ms, self.now_ms() + RT_ALL_PICKED_MS)
            self.rt_broadcast(StateEndTime=self.rt_end_ms)
            self.log("everybody picked a car: ending car select shortly")

    def rt_finish_vote(self) -> None:
        counts = self.rt_counts()
        best = max(counts)
        winner = counts.index(best)             # ties go to the first of the tied locations
        self.rt_winner = winner
        self.rt_state = RT_CAR_SELECT
        self.rt_end_ms = self.now_ms() + RT_CAR_SELECT_MS
        loc = self.rt_candidates[0][winner]
        self.log(f"vote closed {counts}: location {loc} wins; State -> CarSelect")
        # NextRaceId (RoadTripSynced var 3, read by the client's StartDrive handler FUN_140819de0 through the getter
        # at vtable+0xd0 -> [obj+0x100]) is the event the group drives to next; 0 makes that handler do nothing.
        race = self.rt_candidates[1][winner][0]
        # CurrentLocationId must be one of the 3 voted locations: the client's BuildRouteForDrive (FUN_140805340) finds its
        # index in the EventSelection (FUN_140a40a70) and indexes with -1 (crash) otherwise.
        self.rt_picked.clear()
        self.rt_idx = 0
        self.rt_broadcast(CurrentLocationId=loc, NextRaceId=race, State=RT_CAR_SELECT, StateEndTime=self.rt_end_ms)
        self.rt_send_configure()

    def rt_send_configure(self) -> None:
        """Ask the leader's client to build the GameOptions of the round's event. The client's StartDrive handler
        (FUN_140819de0) looks the event up by the id in those options, so it must exist before DriveToEvent."""
        race = self.rt_race()
        self.rt_configured = False
        self.rt_cfg_deadline = time.monotonic() + RT_CAR_SELECT_MS / 1000 + 5.0
        host = min((c for c in self.peers.values() if c.connected and c.rt_started), key=lambda c: c.owner_id,
                   default=None)
        if host:
            self.send(host, [road_trip_configure_game_options(race, 1)])
            self.log(f"{host.addr[1]} -> ConfigureGameOptionsForRoadTripEvent({race}, 1)")

    def rt_check_ready(self, force: bool = False) -> None:
        """PrepareForEvent: once every player is ready (or the phase ran out) the event starts, using the same
        participant selection and race flow as a Free Roam event."""
        peers = [q for q in self.peers.values() if q.connected and q.rt_started]
        ready = [q for q in peers if q.console_vars.get(4)]
        if not ready or not (force or len(ready) == len(peers)):
            return
        leader = min(ready, key=lambda c: c.owner_id)
        # The server stays in PrepareForEvent until the race begins (go_race): a client that sees InEvent while it is
        # still waiting for NetworkSwitchRoute takes the "event started without us" exit back to free roam.
        self.rt_end_ms = 0
        self.log(f"road trip: {len(ready)}/{len(peers)} players ready: event starts (State stays PrepareForEvent)")
        self.rt_broadcast(StateEndTime=0)
        self.start_event(leader)

    def rt_start_drive(self) -> None:
        """Car select is over: everybody drives to the first event of the winning location."""
        loc = self.rt_candidates[0][self.rt_winner]
        race = self.rt_race()
        self.rt_state = RT_DRIVE_TO_EVENT
        for q in self.peers.values():
            q.rt_pstate = 0
        self.rt_end_ms = self.now_ms() + RT_DRIVE_MS
        self.log(f"car select over: State -> DriveToEvent (location {loc}, event {race})")
        # the clients cancel their drive-to-event wait while SessionHost.RaceReady is off (NetworkIsServerRaceReady)
        self.set_session_host_bool(SH_RACE_READY, 1, "road trip: the next event is chosen")
        # The client raises NetworkStartDriveToDestination (route + drive HUD) from RoadTripSynced's variable-changed
        # handler (FUN_140a43a20) only when, in state DriveToEvent, StateEndTime goes from 0 to non-zero. So the
        # state change carries StateEndTime=0 and the countdown follows as its own update.
        self.rt_broadcast(CurrentLocationId=loc, CompletedEventCount=self.rt_idx, EventRaceId=race, NextRaceId=race,
                          State=RT_DRIVE_TO_EVENT, StateEndTime=0)
        if self.rt_idx == 0:
            self.rt_broadcast(StateEndTime=self.rt_end_ms)
        else:
            # Between events the clients are still on the results screen: the state change makes them leave it, and
            # a StartDrive raised before they are back in free roam is ignored. Hold the countdown until they
            # report player State DriveToEvent.
            self.rt_end_ms = 0
            self.rt_drive_wait = True
            self.rt_drive_ready_at = 0.0
            self.rt_drive_deadline = time.monotonic() + 180.0   # a DNF'd car can take minutes to get back to free roam

    def road_trip_tick(self, now: float) -> None:
        host = min((c.owner_id for c in self.peers.values() if c.connected), default=None)
        for q in list(self.peers.values()):
            if not q.connected or q.rt_started or now - q.connected_at < RT_INIT_SECONDS:
                continue
            q.rt_started = True
            if self.rt_state == RT_INITIALISING:       # first client: the session leaves Initialising
                if self.rt_gamedb:
                    here = sum(1 for c in self.peers.values() if c.connected)
                    self.rt_candidates = pick_road_trip_candidates(self.rt_gamedb, here)   # fresh random vote
                    self.rt_votes.clear()
                    self.rt_winner = None
                    self.log(f"road trip candidates: locations {self.rt_candidates[0]} events "
                             f"{self.rt_candidates[1]} classes {self.rt_candidates[2]}")
                self.rt_state = RT_DESTINATION_VOTE
                self.rt_end_ms = self.now_ms() + RT_VOTE_MS
            msgs = []
            if q.owner_id == host:
                # the game's "local player is the leader" test reads LobbyPlayer.PlayerHost (var 1 of 162, 8-bit var
                # ids, SyncedBool)
                def host_flag(w: BitWriter):
                    w.bits(LOBBY_PLAYER_PLAYER_HOST, 8)
                    w.bits(1, 1)
                msgs.append(server_update(1, 0, ROUTER_BITS[1][0], q.player_seq, host_flag))
                q.player_seq = (q.player_seq + 1) % 0xFF
            if self.rt_candidates:
                msgs.append(road_trip_event_selection_update(q.es_seq, *self.rt_candidates, votes=self.rt_counts()))
                q.es_seq = (q.es_seq + 1) % 0xFF
            q.rt_seq = (q.rt_seq + 1) % 0xFF
            values = {"State": self.rt_state, "StateEndTime": self.rt_end_ms}
            if self.rt_winner is not None:
                values["CurrentLocationId"] = self.rt_candidates[0][self.rt_winner]
                values["NextRaceId"] = self.rt_race()
            msgs.append(road_trip_update(q.rt_seq, **values))
            self.send(q, msgs)
            self.log(f"{q.addr[1]} Road Trip session state sent: State={self.rt_state}")
        if self.rt_drive_wait and self.rt_state == RT_DRIVE_TO_EVENT:
            voters = [q for q in self.peers.values() if q.connected and q.rt_started]
            if self.rt_drive_ready_at == 0.0 and voters and all(getattr(q, "rt_pstate", 0) == RT_DRIVE_TO_EVENT
                                                               for q in voters):
                self.rt_drive_ready_at = now + 1.5
            if (self.rt_drive_ready_at and now >= self.rt_drive_ready_at) or now >= self.rt_drive_deadline:
                self.rt_drive_wait = False
                self.rt_end_ms = self.now_ms() + RT_DRIVE_MS
                self.rt_broadcast(StateEndTime=self.rt_end_ms)
                self.log("clients are waiting: drive countdown started")
        if self.rt_pending_drive and now >= self.rt_drive_not_before and \
                (self.rt_configured or now >= self.rt_cfg_deadline):
            self.rt_pending_drive = False
            self.rt_start_drive()
        if self.rt_state == RT_FINALISE_EVENT and self.event and not self.rt_pending_drive and \
                all(getattr(q, "rt_pstate", 0) >= RT_FINALISE_EVENT for q in self.event["people"] if q.connected):
            self.end_event("event finished")      # the clients sit on the results until the server state moves
        if self.rt_end_ms and self.now_ms() >= self.rt_end_ms:
            if self.rt_state == RT_DESTINATION_VOTE:
                self.rt_finish_vote()
            elif self.rt_state == RT_CAR_SELECT:
                if self.rt_configured or now >= self.rt_cfg_deadline:   # wait (bounded) for the leader's options
                    self.rt_start_drive()
            elif self.rt_state == RT_PREPARE_FOR_EVENT:
                self.rt_check_ready(force=True)
            elif self.rt_state == RT_FINALISE_EVENT:
                self.end_event("event finished")
            elif self.rt_state == RT_DRIVE_TO_EVENT:
                self.rt_state = RT_PREPARE_FOR_EVENT
                self.rt_end_ms = self.now_ms() + RT_PREPARE_MS
                self.log("drive over: State -> PrepareForEvent")
                self.rt_broadcast(State=RT_PREPARE_FOR_EVENT, StateEndTime=self.rt_end_ms)
                self.rt_check_ready()       # players that arrived earlier are already ready

    def tick(self) -> None:
        """Server-initiated traffic: the client times out after 10 s of silence."""
        now = time.monotonic()
        if self.event and self.event.get("go_at") and now >= self.event["go_at"]:
            self.go_race()
        self.check_race_end(now)
        if self.roadtrip:
            self.road_trip_tick(now)
        if self.pending_race and now >= self.pending_race[0]:
            _, leader = self.pending_race
            joining = [q for q in self.others(leader)
                       if q.connected and q.entered and q.console_vars.get(3) and not q.console_vars.get(4)]
            if joining and leader.connected and now < getattr(self, "race_hard_deadline", 0):
                self.pending_race = (now + 2.0, leader)   # accepted the invite, still choosing a car
            else:
                self.pending_race = None
                if leader.connected:
                    self.start_event(leader)
        for peer in list(self.peers.values()):
            if peer.connected and now - peer.last_rx > STALE_SECONDS:
                self.drop_peer(peer, f"no packets for {STALE_SECONDS:.0f} s")
                continue
            if peer.connected and now - peer.last_keepalive >= KEEPALIVE_SECONDS:
                peer.last_keepalive = now
                msgs = [ping_live(self.now_ms)]
                # A TimeSync re-bases the client's network clock, which the remote cars' prediction is timed
                # against: sent every second it showed up as a bump at that rhythm. The clock is set at
                # connect; refresh it only rarely.
                if now - peer.last_time_sync >= TIME_SYNC_SECONDS:
                    peer.last_time_sync = now
                    peer.time_sync_seq = (peer.time_sync_seq + 1) & 0xFFFF
                    msgs.append(time_sync_live(peer.time_sync_seq, self.now_ms))
                self.send(peer, msgs)

    def serve(self) -> None:
        sel = selectors.DefaultSelector()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        self.sock.setblocking(False)
        sel.register(self.sock, selectors.EVENT_READ)
        while True:
            if sel.select(timeout=0.01):
                while True:     # drain everything queued before the next timer pass
                    try:
                        data, addr = self.sock.recvfrom(65535)
                    except BlockingIOError:
                        break
                    try:
                        self.handle(data, addr)
                    except Exception as exc:  # keep serving
                        self.log(f"error handling {data.hex()}: {exc!r}")
            self.tick()


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = int(args[0]) if len(args) > 0 else 5000
    logfile = args[1] if len(args) > 1 else None
    try:
        Server(port, logfile, roadtrip="--roadtrip" in sys.argv).serve()
    except KeyboardInterrupt:
        pass
