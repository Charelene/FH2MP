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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import traffic as traffic_mod  # noqa: E402

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
CLOCK_OFFSET_MAX_MS = 86_400_000    # random start of each world's clock = its starting time of day
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
# Car Meet (router 11, ICarMeetRouter: FUN_1417dccb0 configures maxObjects 1 -> 1 id bit and one object type, the server-owned
# CarMeetSynced). Its 18 vars (ctor FUN_141806910) use 5-bit var ids: 0 GameModeInviteType (enum 0..2, 2 bits),
# 1 InviteCountDownTime (u32, default 10000), 2 JoinEndTimeWagerRace (u32), 3 JoinEndTimeGameMode (u32),
# 4..15 JoinTypes[12] (CarMeetJoinTypeWrapperSynced), 16 GameModeSessionName / 17 GameModeSessionTemplateName (strings <= 64).
ROUTER_CARMEET = 11
ROUTER_BITS[ROUTER_CARMEET] = (1, 0)
CARMEET_VAR_ID_BITS = 5
MSG_CARMEET_INVITE_REQUEST = 26        # client -> server: inviteType:2 | value:32          ([MsgCarMeetPlayerInviteRequest])
MSG_CARMEET_JOIN_OR_CANCEL = 27        # client -> server: join:1 | joinType:2 | value:32   ([MsgCarMeetPlayerJoinOrCancelRequest])
MSG_CARMEET_LAUNCH_WAGER_RACE = 28     # server -> client: no payload
MSG_CARMEET_NOTIFY_PENDING = 29        # server -> client: pendingActionType:2
MSG_CARMEET_CONFIGURE_WAGER = 30       # server -> client: no payload
MSG_CARMEET_CONFIGURED_WAGER = 31      # client -> server: u32
CM_VARS = {"GameModeInviteType": (0, 2), "InviteCountDownTime": (1, 32), "JoinEndTimeWagerRace": (2, 32),
           "JoinEndTimeGameMode": (3, 32)}       # then JoinTypes[12] = vars 4..15 (2 bits each), 16/17 session strings
CM_VAR_JOIN0, CM_VAR_SESSION, CM_VAR_TEMPLATE = 4, 16, 17
CM_INVITE_NONE, CM_INVITE_FREE_ROAM, CM_INVITE_ROAD_TRIP = 0, 1, 2    # CarMeetInviteType (assumed order)
CM_JOIN_NONE, CM_JOIN_WAGER, CM_JOIN_GAMEMODE = 0, 1, 2                # CarMeetJoinType (assumed order)
# CarMeetGameModePendingActionType, read from the client handler FUN_1407d6720: 0 clears, 1 = wait for the session and join it,
# 2/3 = create it (they set the creator flags +0x26d)
CM_PENDING_NONE, CM_PENDING_WAIT, CM_PENDING_CREATE_A, CM_PENDING_CREATE_B = 0, 1, 2, 3
CM_JOIN_WINDOW_MS = 20000
MSG_CARMEET_SESSION_READY = 32         # server -> client: string(<=64) session name | string(<=64) template name
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
CAR_PREDICTION_BODY_BITS = 710      # CarPredictionFull body, see the handler
CAR_KEY_ID_BITS = 7            # FUN_141808170: object key written with 7 id bits
# Road Trip (router 9): RoadTripObjectTypes 0 RoadTripSynced (server-owned), 1 RoadTripEventSelection,
# 2 RoadTripPlayerSynced (client-owned). RoadTripSynced vars (ctor FUN_141812ae0, 10 vars -> 4-bit var ids);
# widths other than the state enum are assumed (32-bit ints, 1-bit bool) until the client confirms them.
ROUTER_ROADTRIP = 9
RT_TYPE_SYNCED, RT_TYPE_EVENT_SELECTION, RT_TYPE_PLAYER = 0, 1, 2
RT_VAR_ID_BITS = 4
LOBBY_PLAYER_PLAYER_HOST = 1     # var index in Nerd::LobbyPlayer (FUN_14180bf50): Ready 0, PlayerHost 1, ...
AUTH_TTL_SECONDS = 600.0      # how long a sign-in announced by the LSP lets that address open a connection without further news
RT_INIT_SECONDS = float(os.environ.get("FH2MP_RT_INIT", "3.0"))   # dev knob: hold the state in Initialising         # Initialising -> DestinationVote delay after the connection (the client waits in Initialising
                              # and only sends its session-entry message once the state has changed)
RT_EXCLUDED_MODES = set()        # RaceModes.id never offered (King 25 and Infection 26 are served: tag events, see TAG_MODES)
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
LOBBY_TYPE_CAR_MEET = 3
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
PENDING_SECONDS = 15.0        # a peer that opened a connection but never completed the handshake is forgotten after this long
MAX_PENDING_PEERS = 256       # unconnected peers per world: beyond this new OpenConnections are ignored (spoofed-source flood)
MAX_PENDING_PER_IP = 8
# Azimuth gives every instance the same Xbox user id (saves/player-1/User_9000000000000).
# The game's player list skips any player whose id equals the local player's, so two instances
# on one profile never see each other in "Players". Relayed copies get a per-connection id.
XUID_PATTERN = 0x0009000000000000
UNRELIABLE_ID = 0xFFFF
NO_ID = 0xFFFE

# The Road Trip event tables (AnthemOnlineEvents, RaceModes) of the game's database, cut out by tools/make_roadtrip_db.py; kept
# next to the server so a deployment needs no game install.
ROADTRIP_DB_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "roadtrip.sqlite")

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


def grant_ownership(router, obj_id, id_bits, seq=0, size=0, payload=0):
    """FUN_1417ab720 / FUN_1417b1e70: the client resolves its transfer object by key
    (A=1,B=0,id), then applies a sequenced message: seq:8, size:14 and size payload bits."""
    def write(w: BitWriter):
        w.bits(MSG_OBJECT_GRANT_OWNERSHIP, 7)
        w.bits(router, 4)
        w.bits(1, 1)
        w.bits(0, 1)
        w.bits(obj_id, id_bits)
        w.bits(seq, 8)
        w.bits(size, 14)
        if size:
            w.value |= payload << w.pos
            w.pos += size
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


# Tag modes (King = "keep the It", Infection = "virus"): the client works out a collision's outcome itself from the event's
# TagOptions (FUN_1409a52c0: one car It and the other not -> the It car moves to ItBucketBecomes, the other to NotItBucketBecomes;
# neither It -> TwoNotItsBecome; both It -> nothing) and reports old/new buckets of both cars in MsgTagCollision (36, client ->
# server, FUN_1418048b0). The server owns the buckets: it picks the first It car(s) and answers with MsgTagBuckets (37, server ->
# client, FUN_141815af0): 12 car keys (7-bit ids) then 12 bucket bytes, applied by CTagComponent (FUN_1409a57b0), which raises
# the CarBecomingIt events (crown / patient zero HUD). Evidence: docs/tag-modes.md.
MSG_REPAIR_DAMAGE = 35             # key (client -> server; FUN_141804fd0)
MSG_TAG_COLLISION = 36             # key | key | 4 x u3: bucket A before, A after, B before, B after
MSG_TAG_BUCKETS = 37               # 12 x key | 12 x u8 bucket (unused slots: a key that resolves to no car)
TAG_SLOTS = 12
TAG_BUCKET_NOT_IT, TAG_BUCKET_IT = 0, 1      # TagOptions TagItBucket=1 in every tag preset / career mode (GameOptionPresets.xml)
RACE_MODE_KING, RACE_MODE_INFECTION = 25, 26   # RaceModes.id
# Per mode, from Mount/media/GameTunableSettings/CareerRaceModes.xml (modes 4 King, 5 Infected): the time limit of the event and how
# many players share one It car at the start (NumPlayersPerIt; Infection has a single patient zero).
TAG_MODES = {RACE_MODE_KING: {"name": "King", "seconds": 240.0, "per_it": 6},
             RACE_MODE_INFECTION: {"name": "Infection", "seconds": 240.0, "per_it": 0}}
TAG_ASSIGN_REPEATS = (0.0, 0.4, 1.2, 3.0)      # seconds after the first send: the datagrams are unreliable, so repeat them

# Free roam events are chosen on the map, so the server is never told their mode (MsgRequestRace has no payload). The leader's GameOptions say
# it. Found by lining up payloads whose Road Trip event, and so mode, is known (prod logs 2026-10-19/20): bits 0-7 are the event type (0 street
# race and tag modes, 3 laps, 4 point to point); a tag mode also sets bits 10-12 and has a long (~700 bit) tag section, where a street race
# leaves bits 10-12 clear. Only King has been seen: the Free Roam payloads at 00:17 and 01:56 have the Road Trip King's header. Infection is
# told apart from the first hit (tag_collision): an It car that stays It.
GAME_OPTIONS_TAG_MIN_BITS = 600
GAME_OPTIONS_TAG_BITS = 0b111 << 10


def game_options_tag_mode(game_options):
    """RACE_MODE_KING when the lobby's GameOptions (size, payload) look like a tag event, else None."""
    if not game_options:
        return None
    size, payload = game_options
    if size < GAME_OPTIONS_TAG_MIN_BITS or payload & 0xFF or payload & GAME_OPTIONS_TAG_BITS != GAME_OPTIONS_TAG_BITS:
        return None
    return RACE_MODE_KING


def read_car_key(r):
    """An object key (FUN_1417ba4d0): flagA flagB id:7, plus owner:8 for a relay key. Returns (kind, id, owner)."""
    a, b = r.bits(1), r.bits(1)
    cid = r.bits(CAR_KEY_ID_BITS)
    if a == 0 and b == 1:
        return "relay", cid, r.bits(8)
    return ("own" if (a, b) == (0, 0) else "other"), cid, 0


def write_car_key(w, kind, cid, owner=0):
    if kind == "relay":
        w.bits(0, 1)
        w.bits(1, 1)
        w.bits(cid, CAR_KEY_ID_BITS)
        w.bits(owner, 8)
    else:
        w.bits(0, 2)
        w.bits(cid, CAR_KEY_ID_BITS)


def tag_buckets_message(entries):
    """entries: [(key kind, car id, owner, bucket)], at most 12; unused slots hold a relay key of owner 0 (never assigned)."""
    entries = list(entries)[:TAG_SLOTS]
    entries += [("relay", 127, 0, 0)] * (TAG_SLOTS - len(entries))

    def write(w: BitWriter):
        w.bits(MSG_TAG_BUCKETS, 7)
        for kind, cid, owner, _ in entries:
            write_car_key(w, kind, cid, owner)
        for _, _, _, bucket in entries:
            w.bits(bucket, 8)
    return write


def relay_repair_message(car_id, owner):
    def write(w: BitWriter):
        w.bits(MSG_REPAIR_DAMAGE, 7)
        write_car_key(w, "relay", car_id, owner)
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


# Weather model (recovered from a solo session sampled with FH2MP_WEATHER_DUMP; see docs/weather.md):
#   the atmosphere has 4 severity levels 0, 1/3, 2/3, 1 (clear / mostly sunny / cloudy / rain+storm); the current level ramps toward
#   the target at 1/165 per second (WEATHER_RAMP_MS per unit); every ~4-5 minutes a new target is chosen. Each level has a preset
#   index (observed [0, 1, 3|5, 7]). Precipitation follows the atmosphere: none below level ~0.6, full at level 1.
#   Network form: WeatherSynced (router 10) atm.{Current,Desired}Intensity (float) / {Current,Desired}Tick (net clock ms), the
#   preset index array, and the same four vars for precipitation; the client interpolates between (Current, CurrentTick) and
#   (Desired, DesiredTick) on its network clock [guess: not yet verified in game].
WEATHER_LEVELS = (0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0)
WEATHER_RAMP_MS = 165000            # time for the atmosphere to move one whole unit of intensity
WEATHER_RAIN_START = 0.603          # atmosphere level above which it rains (precipitation ramps 0 -> 1 up to level ~0.92)
WEATHER_WEIGHTS = {1: 0.45, 2: 0.35, 3: 0.20}     # next-level odds (current level excluded, then renormalised)
WEATHER_HOLD_S = (90.0, 180.0)      # steady time after a transition, before the next change


def weather_vars(wx):
    """All nine WeatherSynced variables from the world's weather state dict."""
    import struct

    def f32(x: float) -> int:
        return struct.unpack("<I", struct.pack("<f", x))[0]

    def write(w: BitWriter):
        for name, kind, value in (
            ("atm.CurrentIntensity", "f", wx["a_cur"]), ("atm.CurrentTick", "u", wx["a_cur_tick"]),
            ("atm.DesiredIntensity", "f", wx["a_des"]), ("atm.DesiredTick", "u", wx["a_des_tick"]),
            ("pre.CurrentIntensity", "f", wx["p_cur"]), ("pre.CurrentTick", "u", wx["p_cur_tick"]),
            ("pre.DesiredIntensity", "f", wx["p_des"]), ("pre.DesiredTick", "u", wx["p_des_tick"]),
        ):
            w.bits(WEATHER_VARS[name], WEATHER_VAR_ID_BITS)
            w.bits(f32(value) if kind == "f" else value & 0xFFFFFFFF, 32)
        w.bits(WEATHER_VARS["atm.DesiredPresetIndex"], WEATHER_VAR_ID_BITS)    # SyncedArray<long, 4>: 4 x 32 bits
        for idx in wx["presets"]:
            w.bits(idx & 0xFFFFFFFF, 32)
    return write


def weather_create(seq, wx):
    return object_message(MSG_OBJECT_CREATE, ROUTER_WEATHER, 0, 0, seq, 1, 0, weather_vars(wx))


def weather_update(seq, wx):
    return object_message(MSG_OBJECT_UPDATE, ROUTER_WEATHER, 0, 0, seq, 1, 0, weather_vars(wx))


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
                          % ",".join(str(m) for m in (RT_EXCLUDED_MODES or {-1})), (max(1, players),)).fetchall()
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


def car_meet_create(seq=1, write_vars=None):
    return object_message(MSG_OBJECT_CREATE, ROUTER_CARMEET, 0, 0, seq, ROUTER_BITS[ROUTER_CARMEET][0], 0, write_vars)


def car_meet_update(seq, write_vars):
    return object_message(MSG_OBJECT_UPDATE, ROUTER_CARMEET, 0, 0, seq, ROUTER_BITS[ROUTER_CARMEET][0], 0, write_vars)


def car_meet_vars(invite_type=None, countdown=None, wager_end=None, mode_end=None, joins=None, session=None, template=None):
    """Writer for CarMeetSynced variables (5-bit var ids); joins = {slot: CarMeetJoinType}; strings are 7-bit length + UTF-16."""
    def write(w: BitWriter):
        def put(name, value):
            vid, width = CM_VARS[name]
            w.bits(vid, CARMEET_VAR_ID_BITS)
            w.bits(int(value) & ((1 << width) - 1), width)
        if invite_type is not None:
            put("GameModeInviteType", invite_type)
        if countdown is not None:
            put("InviteCountDownTime", countdown)
        if wager_end is not None:
            put("JoinEndTimeWagerRace", wager_end)
        if mode_end is not None:
            put("JoinEndTimeGameMode", mode_end)
        for slot, jt in sorted((joins or {}).items()):
            w.bits(CM_VAR_JOIN0 + slot, CARMEET_VAR_ID_BITS)
            w.bits(jt, 2)
        for vid, text in ((CM_VAR_SESSION, session), (CM_VAR_TEMPLATE, template)):
            if text is not None:
                w.bits(vid, CARMEET_VAR_ID_BITS)
                w.bits(len(text), 7)
                for ch in text[:64]:
                    w.bits(ord(ch), 16)
    return write


def car_meet_notify_pending(action):
    def write(w: BitWriter):
        w.bits(MSG_CARMEET_NOTIFY_PENDING, 7)
        w.bits(action, 2)
    return write


def car_meet_session_ready(name, template):
    def write(w: BitWriter):
        w.bits(MSG_CARMEET_SESSION_READY, 7)
        w.string(name)
        w.string(template)
    return write


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
        self.wx_seq = 1            # seq of the last message on the server-owned WeatherSynced
        self.wx_ready = False      # the Weather object has been created on this client
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
        self.car_id = None         # id of this client's own car object (from its CarPredictionFull keys)
        self.lobby_type = 0        # OpenConnection field a: Nerd::NetworkLobbyType the client joined (1 RoadTrip, 2 FreeRoam, 3 CarMeet)
        self.xuid = 0              # the client's Xbox user id (OpenConnection playerId)
        self.cm_seq = 1            # Car Meet: seq of the last CarMeetSynced message sent to this client (create = 1)
        self.world = None          # (lobby type, lobby name): clients only see each other inside one world

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
    def __init__(self, port: int, logpath, roadtrip: bool = False, carmeet: bool = False, sock=None, log_file=None,
                 tag: str = ""):
        """One world: the peers of a single lobby, with their own race / Road Trip state. The Hub creates one per lobby
        and shares the UDP socket; run standalone (sock=None) it binds the port itself."""
        if sock is None:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.sock.bind(("0.0.0.0", port))
            self.log_file = open(logpath, "a", buffering=1) if logpath else None
        else:
            self.sock = sock
            self.log_file = log_file
        self.tag = tag
        self.assign = {}
        self.ctl = None
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
        self.rt_pick_players = 0   # players present when the vote candidates were drawn (King / Infection need 2)
        self.gm = {"type": 0, "phase": "idle", "end": 0, "starter": None, "joined": {}, "session": None}  # Car Meet game-mode invite
        self.hub = None
        self.is_idle = False       # the hub's catch-all: answers strangers with CloseConnection and never keeps a peer
        self.carmeet = carmeet     # serve a Car Meet session (SessionHost.LobbyType = 3)
        self.roadtrip = roadtrip   # serve a Road Trip session (router 9 RoadTripSynced) instead of Free Roam
        self.rt_seq = 1
        self.server_counter = 1
        self.start = time.monotonic()
        # Online the client derives the world's time of day from this clock (SetTimeOfDay realtime: FUN_14197af90,
        # tod_seconds = timescale * (network_ms / 1000) % 86400), so a clock that starts at 0 means every world starts at midnight.
        # A random start under 24 h gives each world its own time of day; it stays small so the plain comparisons on now_ms() hold.
        self.clock_offset_ms = int(os.environ.get("FH2MP_CLOCK_OFFSET_MS", random.randrange(CLOCK_OFFSET_MAX_MS)))
        self.traffic = traffic_mod.TrafficWorld(self.send, lambda: [p for p in self.peers.values() if p.connected and p.entered],
                                                self.log)
        self.leftover_logged = 0
        self.big_logged = 0
        self.mt_seen = {}          # message type -> count (debug histogram, logged every 30 s)
        self.mt_next = time.monotonic() + 30
        self.wx = None
        self.wx_next = 0.0         # monotonic time of the next weather change
        self.weather_init()
        self.peers = {}
        self.next_owner = 1
        self.pending_race = None   # (deadline, leader Peer)
        self.game_options = None   # (size, payload) latest GameOptionsSynced written by the leader
        self.event = None          # {"people": [Peer], "begun": bool, "start_at": float|None}
        self.log(f"listening on udp/{port} version={VERSION_STRING} game={GAME_STRING} "
                 f"clock offset {self.clock_offset_ms} ms (time of day at start {(48 * (self.clock_offset_ms // 1000)) % 86400 // 3600:02d}:"
                 f"{(48 * (self.clock_offset_ms // 1000)) % 3600 // 60:02d} if the time scale is 48)")
        if roadtrip:
            gamedb = os.environ.get("FH2MP_GAMEDB") or ROADTRIP_DB_DEFAULT
            if gamedb and os.path.exists(gamedb):
                self.rt_gamedb = gamedb
                self.rt_candidates = pick_road_trip_candidates(gamedb)
                self.log(f"road trip candidates: locations {self.rt_candidates[0]} events {self.rt_candidates[1]} "
                         f"classes {self.rt_candidates[2]}")
            else:
                self.log(f"road trip: no game database ({gamedb or 'FH2MP_GAMEDB unset'}); the vote will be empty. Build "
                         f"{ROADTRIP_DB_DEFAULT} with tools/make_roadtrip_db.py or point FH2MP_GAMEDB at gamedbRC.slt")

    # ------------------------------------------------------------------ weather
    def weather_pick_level(self, current):
        levels = [lv for lv in WEATHER_WEIGHTS if lv != current]
        return random.choices(levels, [WEATHER_WEIGHTS[lv] for lv in levels])[0]

    def weather_presets(self, level):
        """Preset index per severity level (observed [0, 1, 3 or 5, 7]); the cloudy one varies between changes."""
        return [0, 1, random.choice((3, 5)), 7]

    def weather_init(self) -> None:
        env = os.environ.get("FH2MP_WEATHER_START")
        level = int(env) if env else self.weather_pick_level(None)
        now = self.now_ms()
        rain = 1.0 if level == 3 else 0.0
        self.wx = {"level": level, "presets": self.weather_presets(level), "a_cur": WEATHER_LEVELS[level], "a_cur_tick": now,
                   "a_des": WEATHER_LEVELS[level], "a_des_tick": now, "p_cur": rain, "p_cur_tick": now, "p_des": rain,
                   "p_des_tick": now}
        self.wx_next = time.monotonic() + self.weather_hold()
        self.log(f"weather: start level {level} ({WEATHER_LEVELS[level]:.3f}), presets {self.wx['presets']}")

    def weather_hold(self) -> float:
        scale = float(os.environ.get("FH2MP_WEATHER_SCALE", "1.0"))
        return random.uniform(*WEATHER_HOLD_S) * scale

    def weather_tick(self, now: float) -> None:
        if now < self.wx_next or not any(p.connected for p in self.peers.values()):
            return
        wx = self.wx
        t = self.now_ms()
        # where the atmosphere is right now (it may still be ramping)
        span = (wx["a_des_tick"] - wx["a_cur_tick"]) & 0xFFFFFFFF
        done = (t - wx["a_cur_tick"]) & 0xFFFFFFFF
        cur = wx["a_des"] if span == 0 or done >= span else wx["a_cur"] + (wx["a_des"] - wx["a_cur"]) * done / span
        level = self.weather_pick_level(wx["level"])
        target = WEATHER_LEVELS[level]
        ramp_ms = int(abs(target - cur) * WEATHER_RAMP_MS)
        a_end = t + ramp_ms
        # precipitation: only while the atmosphere is above WEATHER_RAIN_START
        p_now = wx["p_des"] if wx["p_des_tick"] == wx["p_cur_tick"] else wx["p_cur"]
        if level == 3:
            p_des = 1.0
            p_start = t + max(0, int((WEATHER_RAIN_START - cur) * WEATHER_RAMP_MS))
        else:
            p_des = 0.0
            p_start = t
        p_end = a_end if level == 3 else t + max(1000, int(max(0.0, cur - WEATHER_RAIN_START) * WEATHER_RAMP_MS))
        if p_des == p_now:
            p_start = p_end = t
        wx.update(level=level, presets=self.weather_presets(level), a_cur=cur, a_cur_tick=t, a_des=target, a_des_tick=a_end,
                  p_cur=p_now, p_cur_tick=p_start, p_des=p_des, p_des_tick=p_end)
        self.wx_next = now + ramp_ms / 1000.0 + self.weather_hold()
        self.log(f"weather: change to level {level} ({target:.3f}) from {cur:.3f}, ramp {ramp_ms / 1000:.0f} s, rain {p_now:.2f} -> {p_des:.2f}, "
                 f"presets {wx['presets']}, next change in {self.wx_next - now:.0f} s")
        for q in self.peers.values():
            if q.connected and q.wx_ready:
                q.wx_seq = (q.wx_seq + 1) % 0xFF
                self.send(q, [weather_update(q.wx_seq, wx)])

    def now_ms(self) -> int:
        return (int((time.monotonic() - self.start) * 1000) + self.clock_offset_ms) & 0xFFFFFFFF

    def log(self, msg: str) -> None:
        line = f"{time.strftime('%H:%M:%S')} {self.tag}{msg}"
        print(line, flush=True)
        if self.log_file:
            self.log_file.write(line + "\n")

    def send(self, peer: Peer, messages, fast: bool = False) -> None:
        # The client measures RTT in whole milliseconds and discards 0 ms as invalid, which makes
        # it fall back to a 100 ms retransmit timer. Hold a reply back until ~2 ms after that client's last
        # packet so RTT reads >= 1. Packets relayed from other clients (fast=True) do not need it and must not
        # stall the loop: the sleep blocks every world on the port (it, not CPU, capped a full server at ~60 players).
        if not fast:
            gap = time.monotonic() - peer.last_rx
            if gap < 0.002:
                time.sleep(0.002 - gap)
        ack, bits = peer.ack_fields()
        pkt = build_packet(messages, ack_id=UNRELIABLE_ID, remote_ack=ack, ack_bits=bits)
        self.sock.sendto(pkt, peer.addr)

    def others(self, peer: Peer):
        return [q for q in self.peers.values()
                if q is not peer and q.connected and q.entered and q.world == peer.world]

    def remember_and_relay(self, peer: Peer, mtype, router, oid, tid, seq, size, payload) -> None:
        id_bits, type_bits = ROUTER_BITS[router]
        if peer.owner_id:
            # Every client's objects carried the same local user id (XUID_PATTERN, Azimuth's default), so they were made unique per
            # client by adding the relay owner id. Clients now have their own xuid; use it, so the id other players resolve names and
            # pictures from (XSAPI profile lookups) is the real one. A client whose xuid IS the pattern needs no rewrite.
            new_id = peer.xuid or XUID_PATTERN + peer.owner_id
            if new_id != XUID_PATTERN:
                payload, n_x = rewrite_xuid(payload, size, new_id)
                if n_x:
                    self.log(f"{peer.addr[1]} router {router} object {oid}: user id rewritten x{n_x} -> {new_id:#x} for other clients")
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
            self.send(q, [msg], fast=True)

    GAME_OPTIONS_TRANSFER_ID = 0   # transfer-object id of the lobby's GameOptions (router 6, type 1)

    def ownership_grant(self, peer: Peer, router: int, tid: int, oid: int, id_bits: int):
        """The reply to a client's RequestOwnership. A client that was already sent the lobby's GameOptions (send_game_options, numbered
        from 0) takes the object over from that state: a grant numbered 0 with no state looks stale to it, and on prod (2026-10-20) a
        second player who asked for ownership of GameOptions that the first leader had written never wrote or requested anything again
        (the old, working cases had no stored GameOptions). That player now gets the current state with the next sequence number, the
        same content as the update it already accepted. Everybody else is answered exactly as before."""
        if router == ROUTER_LOBBY and tid == TYPE_GAME_OPTIONS and self.game_options and peer.gopts_seq > 0:
            size, payload = self.game_options
            seq = peer.gopts_seq
            peer.gopts_seq = (peer.gopts_seq + 1) % 0xFF
            self.log(f"{peer.addr[1]} ownership grant carries the lobby's GameOptions ({size} bits, seq {seq})")
            return grant_ownership(router, oid, id_bits, seq, size, payload)
        return grant_ownership(router, oid, id_bits)

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
            if q is newcomer or not q.connected or q.world != newcomer.world:
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
            if not q.connected or q.lobby_type == LOBBY_TYPE_CAR_MEET:
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

    def rt_race_mode(self, race: int):
        """RaceModes.id of a Road Trip event (AnthemOnlineEvents.RaceModeId), None when unknown."""
        import sqlite3
        try:
            db = sqlite3.connect(f"file:{self.rt_gamedb}?mode=ro&immutable=1", uri=True)
            try:
                row = db.execute("SELECT RaceModeId FROM AnthemOnlineEvents WHERE AnthemOnlineEventId = ?", (race,)).fetchone()
            finally:
                db.close()
            return row[0] if row else None
        except Exception:
            return None

    # ---- tag modes (King, Infection) ----
    def tag_people(self):
        ev = self.event
        return [q for q in ev["people"] if q.connected and q.car_id is not None] if ev else []

    def tag_send(self, why: str) -> None:
        """MsgTagBuckets to every participant: its own car by the plain key, the others by relay keys."""
        tag = self.event["tag"]
        people = self.tag_people()
        for q in people:
            entries = [("own" if p is q else "relay", p.car_id, p.owner_id, tag["buckets"].get(p, TAG_BUCKET_NOT_IT))
                       for p in people]
            self.send(q, [tag_buckets_message(entries)])
        self.log(f"tag: buckets sent ({why}): " + " ".join(f"owner{p.owner_id}={tag['buckets'].get(p, 0)}" for p in people))

    def tag_assign(self) -> None:
        """First It car(s): one per NumPlayersPerIt players (King), a single patient zero (Infection), drawn at random."""
        tag = self.event["tag"]
        people = self.tag_people()
        if not people:
            return
        per_it = tag["mode"]["per_it"]
        n_it = max(1, -(-len(people) // per_it)) if per_it else 1
        tag["buckets"] = {p: TAG_BUCKET_NOT_IT for p in people}
        firsts = random.sample(people, min(n_it, len(people)))
        for p in firsts:
            tag["buckets"][p] = TAG_BUCKET_IT
        tag["assigned"] = True
        self.log(f"tag: {tag['mode']['name']} starts, {len(people)} player(s), It: {[p.owner_id for p in firsts]}")
        self.tag_send("initial assignment")

    def tag_tick(self, now: float) -> None:
        ev = self.event
        tag = ev["tag"]
        if tag.get("assign_at") is None or ev.get("over"):
            return
        if not tag.get("assigned"):
            if now >= tag["assign_at"]:
                self.tag_assign()
                tag["repeats"] = [now + x for x in TAG_ASSIGN_REPEATS[1:]]
            return
        people = self.tag_people()
        its = [p for p in people if tag["buckets"].get(p) == TAG_BUCKET_IT]
        if people and not its:           # the It car left: draw a new one
            p = random.choice(people)
            tag["buckets"][p] = TAG_BUCKET_IT
            self.log(f"tag: no It car left, owner {p.owner_id} is It now")
            self.tag_send("new It")
        elif tag["repeats"] and now >= tag["repeats"][0]:
            tag["repeats"].pop(0)
            self.tag_send("repeat")
        if tag["mode"]["name"] == "Infection" and people and len(its) == len(people) and len(people) > 1:
            self.end_race("everybody is infected")
        elif now >= tag["end_at"]:
            self.end_race("time limit")

    def tag_collision(self, peer, k1, k2, old_a, new_a, old_b, new_b) -> None:
        ev = self.event
        tag = ev.get("tag") if ev else None
        if not tag or not tag.get("assigned") or ev.get("over"):
            return

        def car(key):
            kind, cid, owner = key
            if kind == "own":
                return peer
            if kind == "relay":
                return next((q for q in self.peers.values() if q.connected and q.owner_id == owner), None)
            return None
        a, b = car(k1), car(k2)
        buckets = tag["buckets"]
        if a is None or b is None or a is b or a not in buckets or b not in buckets:
            self.log(f"tag: collision from {peer.addr[1]} names unknown cars {k1} {k2}")
            return
        if (buckets[a], buckets[b]) != (old_a, old_b):
            return           # the other car's client reported the same hit first (or it is stale)
        if (old_a == TAG_BUCKET_IT) == (old_b == TAG_BUCKET_IT) or max(new_a, new_b) > TAG_BUCKET_IT:
            self.log(f"tag: ignored collision {old_a}->{new_a} / {old_b}->{new_b}")
            return
        if tag.get("guessed"):               # a Free Roam event whose mode was only guessed: the first real hit tells King from Infection
            tag["guessed"] = False
            it_after = new_a if old_a == TAG_BUCKET_IT else new_b
            if it_after == TAG_BUCKET_IT:    # the It car stayed It (Infection); in King it hands the crown over
                tag["mode"] = TAG_MODES[RACE_MODE_INFECTION]
                self.log("tag: the It car stayed It: this is Infection, not King")
        buckets[a], buckets[b] = new_a, new_b
        self.log(f"tag: owner {a.owner_id} {old_a}->{new_a}, owner {b.owner_id} {old_b}->{new_b} (reported by {peer.owner_id})")
        self.tag_send("collision")
        tag["repeats"] = [time.monotonic() + x for x in TAG_ASSIGN_REPEATS[1:3]]

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
            self.rt_pick_players = here
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
        mode = self.rt_race_mode(self.rt_race()) if self.roadtrip and self.rt_winner is not None and self.rt_gamedb else None
        guessed = False
        if mode is None and not self.roadtrip and self.game_options:      # free roam: read the mode off the leader's GameOptions
            size, payload = self.game_options
            mode = game_options_tag_mode(self.game_options)
            guessed = mode is not None
            self.log(f"event options: {size} bits, header {payload & 0xFFFFFFFFFF:010x}: "
                     f"{'tag mode (King assumed until the first hit shows otherwise)' if guessed else 'not a tag mode'}")
        if mode in TAG_MODES:
            self.event["tag"] = {"mode": TAG_MODES[mode], "buckets": {}, "assigned": False, "assign_at": None, "repeats": [],
                                 "guessed": guessed}
            self.event["numlaps"] = None
            self.log(f"tag: event {self.rt_race() if not guessed else '(free roam)'} is {TAG_MODES[mode]['name']}")
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

    def note_score_keyframe(self, peer: Peer) -> None:
        """MsgScoreKeyframe is a player's final score: seen once per player at the end of a 3-lap circuit race on prod (the first finisher,
        and again for the player whose race was ended by "race ends in"), never after laps 1 and 2. A circuit event's lap count is not
        decoded (see note_lap_score), so for free roam events this is what tells the server that player is done; without it InRace stayed 1
        and every client sat on the results screen for good. Road Trip events keep their lap-table logic (numlaps)."""
        ev = self.event
        if not ev or not ev.get("racing") or ev.get("numlaps") or ev.get("tag") or peer not in ev["people"] or peer in ev["finished"]:
            return
        ev["finished"][peer] = time.monotonic()
        self.log(f"{peer.addr[1]} final score keyframe: done ({len(ev['finished'])}/{len(ev['people'])} finished)")

    def check_race_end(self, now: float) -> None:
        ev = self.event
        if ev and ev.get("clear_at") and now >= ev["clear_at"]:
            ev["clear_at"] = None      # the "event is about to start" HUD follows RaceReady/ParticipantsSelected
            for var in (SH_RACE_PARTICIPANTS_SELECTED, SH_RACE_READY):
                self.set_session_host_bool(var, 0, "race over: lobby flags cleared")
        if not ev or not ev.get("racing") or ev.get("over"):
            return
        if ev.get("tag"):
            self.tag_tick(now)
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
        if ev.get("tag"):
            ev["tag"]["assign_at"] = time.monotonic() + RACE_COUNTDOWN_MS / 1000.0     # the crown / first infection at "GO"
            ev["tag"]["end_at"] = ev["tag"]["assign_at"] + ev["tag"]["mode"]["seconds"]
        go = (self.now_ms() + RACE_COUNTDOWN_MS) & 0xFFFFFFFF
        self.log(f"MsgStartRace start time {go} (server clock now {self.now_ms()})")
        for q in ev["people"]:
            self.send(q, [start_race(go)])

    def drop_peer(self, peer: Peer, why: str) -> None:
        self.peers.pop(peer.addr, None)
        self.traffic.leave(peer)
        self.rt_votes.pop(peer, None)
        if self.roadtrip and not any(c.connected for c in self.peers.values()):
            self.rt_state, self.rt_end_ms, self.rt_winner = RT_INITIALISING, 0, None   # the next group starts fresh
        self.log(f"{peer.addr[1]} removed ({why}); owner {peer.owner_id} had {len(peer.objs)} objects")
        for (router, oid) in peer.objs:
            id_bits, _ = ROUTER_BITS[router]
            n = peer.relay_next.get((router, oid), 0)
            msg = relay_delete_message(router, oid, peer.owner_id, id_bits, n % 0xFF)
            for q in self.others(peer):
                self.send(q, [msg], fast=True)

    def noise(self, msg: str) -> None:
        """Log about datagrams from strangers at most once a second, so a flood of junk cannot fill the disk."""
        now = time.monotonic()
        if now - getattr(self, "_noise_at", -1e9) >= 1.0:
            self._noise_at = now
            self.log(msg)

    def open_allowed(self, player_id: int, addr) -> bool:
        """Identity check for EVERY OpenConnection message, wherever it sits in a datagram: the hub's sign-in table when
        authentication is required, and never a different player id on a peer that already opened a connection."""
        peer = self.peers.get(addr)
        if peer is not None and peer.xuid and peer.xuid != player_id:
            self.noise(f"{addr[0]}:{addr[1]} refused OpenConnection of {player_id:#x}: this connection belongs to {peer.xuid:#x}")
            return False
        return self.hub is None or self.hub.open_allowed(player_id, addr)

    def pending_room(self, addr) -> bool:
        pending = [a for a, p in self.peers.items() if not p.connected]
        return len(pending) < MAX_PENDING_PEERS and sum(1 for a in pending if a[0] == addr[0]) < MAX_PENDING_PER_IP

    def handle(self, data: bytes, addr) -> None:
        # Nothing is remembered about an address until its datagram parses and starts with an OpenConnection that passes the
        # identity check: junk, empty and stale packets get at most a CloseConnection.
        peer = self.peers.get(addr)
        r = BitReader(data)
        try:
            hdr = parse_header(r)
        except ValueError:
            self.noise(f"{addr} short packet {data[:16].hex()}")
            return
        if peer is None:
            if hdr["count"] < 1:
                return
            first = BitReader(data)
            parse_header(first)
            try:
                first_type = first.bits(7)
            except ValueError:
                return
            if first_type != MSG_OPEN_CONNECTION:
                # a client that survived a server restart: tell it the connection is gone
                self.noise(f"{addr[1]} stale peer (no handshake); sending CloseConnection")
                self.send(Peer(addr), [close_connection(0)])
                return
            if self.is_idle or not self.pending_room(addr):
                self.noise(f"{addr[0]}:{addr[1]} too many half-open connections; ignored")
                return
            peer = self.peers[addr] = Peer(addr)
        peer.note_packet_id(hdr["ack_id"])
        peer.last_rx = time.monotonic()
        replies = []
        types = []
        for _ in range(hdr["count"]):
            try:
                mtype = r.bits(7)
                self.mt_seen[mtype] = self.mt_seen.get(mtype, 0) + 1
            except ValueError:
                break
            types.append(mtype)
            if mtype == MSG_OPEN_CONNECTION:
                version, game = r.string(), r.string()
                flag2 = r.bits(2)
                player_id = r.bits(64)
                a, b, counter = r.bits(32), r.bits(32), r.bits(32)
                if not self.open_allowed(player_id, addr):
                    if not peer.connected:
                        self.peers.pop(addr, None)
                    return                      # the rest of this datagram is not trusted either
                if peer.connected:
                    # same address reconnecting: new connection, ids restart at 0
                    self.log(f"{addr[1]} reconnect: resetting peer state")
                    self.drop_peer(peer, "reconnect")
                    peer = Peer(addr)
                    self.peers[addr] = peer
                result = RESULT_OK
                if version != VERSION_STRING:
                    result = RESULT_VERSION_MISMATCH
                elif game != GAME_STRING:
                    result = RESULT_GAME_MISMATCH
                peer.client_counter = counter
                peer.xuid = player_id
                peer.lobby_type = a if a in (1, 2, 3) else (LOBBY_TYPE_CAR_MEET if self.carmeet else
                                                          LOBBY_TYPE_ROAD_TRIP if self.roadtrip else 2)
                peer.world = ("", )
                self.log(f"{addr[1]} OpenConnection version={version!r} game={game!r} "
                         f"id={player_id:#x} flag={flag2} a={a:#x} b={b:#x} counter={counter} -> result {result} "
                         f"(lobby type {peer.lobby_type})")
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
                    if self.roadtrip or peer.lobby_type == LOBBY_TYPE_CAR_MEET:
                        # SessionHost.LobbyType (var 0, enum NetworkLobbyType: 1 RoadTrip, 2 FreeRoam, 3 CarMeet, ...):
                        # the client loads the session entity group registered for this value (FUN_140804210)
                        lobby = LOBBY_TYPE_ROAD_TRIP if self.roadtrip else LOBBY_TYPE_CAR_MEET

                        def lobby_type(w: BitWriter, lobby=lobby):
                            w.bits(0, SESSION_HOST_VAR_ID_BITS)
                            w.bits(lobby, 3)
                        replies.append(session_host_create(seq=1, write_vars=lobby_type))
                    else:
                        replies.append(session_host_create(seq=1))
                    replies.append(weather_create(peer.wx_seq, self.wx))
                    peer.wx_ready = True
                    if peer.lobby_type == LOBBY_TYPE_CAR_MEET:
                        replies.append(car_meet_create(seq=1))
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
                self.traffic.on_message(peer, kind, a, s14, u5, b, flag, seq)
            elif mtype in traffic_mod.UPDATE_LAYOUT:
                self.traffic.on_update(peer, mtype, r)
            elif mtype == MSG_FINALISE_SESSION_ENTRY:
                self.log(f"{addr[1]} FinaliseSessionEntry -> echo + FullWeather + "
                         f"Console.SessionEntryFinalised=1")
                replies.append(finalise_session_entry())
                replies.append(full_weather())
                if not peer.entered:
                    peer.entered = True
                    self.replay_to(peer)
                    self.send_game_options(peer)
                    self.traffic.join(peer)

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
                replies.append(self.ownership_grant(peer, router, tid, oid, id_bits))
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
                    self.send(q, [msg], fast=True)
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
                    self.send(q, [msg], fast=True)
                if mtype == MSG_LAP_SCORE:
                    self.note_lap_score(peer, lap, blobs)
                elif mtype == MSG_SCORE_KEYFRAME:
                    self.note_score_keyframe(peer)
            elif mtype == MSG_CAR_PREDICTION_FULL:
                a, b = r.bits(1), r.bits(1)
                cid = r.bits(CAR_KEY_ID_BITS)
                # The body is a fixed 710 bits (FUN_141822400, the message's size function: 96 + 326 + 4 x 64 + 32); the
                # client appends its other messages (traffic updates ...) after it, so it must not be read to the end.
                rest_bits = CAR_PREDICTION_BODY_BITS if r.size - r.pos >= CAR_PREDICTION_BODY_BITS else r.size - r.pos
                rest = r.bits(rest_bits)
                peer.cars_relayed += 1
                if peer.cars_relayed <= 3 or peer.cars_relayed % 500 == 0:
                    self.log(f"{addr[1]} car prediction #{peer.cars_relayed} flags={a}{b} id={cid} "
                             f"rest={rest_bits} bits -> {len(self.others(peer))} peer(s)")
                if a == 0 and b == 0:
                    peer.car_id = cid
                    msg = relay_car_prediction(cid, peer.owner_id, rest, rest_bits)
                    for q in self.others(peer):
                        self.send(q, [msg], fast=True)
            elif mtype == MSG_REPAIR_DAMAGE:
                kind, cid, _ = read_car_key(r)
                if kind == "own":
                    msg = relay_repair_message(cid, peer.owner_id)
                    for q in self.others(peer):
                        self.send(q, [msg], fast=True)
            elif mtype == MSG_TAG_COLLISION:
                k1, k2 = read_car_key(r), read_car_key(r)
                olds_news = [r.bits(3) for _ in range(4)]
                self.tag_collision(peer, k1, k2, *olds_news)
            elif mtype == MSG_CARMEET_INVITE_REQUEST:
                invite_type, value = r.bits(2), r.bits(32)
                self.log(f"{addr[1]} CarMeet InviteRequest type={invite_type} value={value:#x} (owner {peer.owner_id})")
                self.cm_invite(peer, invite_type)
            elif mtype == MSG_CARMEET_JOIN_OR_CANCEL:
                join, join_type, value = r.bits(1), r.bits(2), r.bits(32)
                self.log(f"{addr[1]} CarMeet JoinOrCancel join={join} joinType={join_type} value={value:#x} "
                         f"(owner {peer.owner_id})")
                self.cm_join(peer, join, join_type)
            elif mtype == MSG_CARMEET_SESSION_READY:
                at = r.pos
                bits_left = r.size - r.pos
                tail = r.bits(min(bits_left, 600)) if bits_left else 0
                r.pos = at
                self.log(f"{addr[1]} CarMeet SessionReady raw: {bits_left} bits left, "
                         f"{tail.to_bytes((min(bits_left, 600) + 7) // 8, 'little').hex()}")
                name, template = r.string(), r.string()
                self.cm_session_ready(peer, name, template)
            elif mtype == MSG_CARMEET_CONFIGURED_WAGER:
                self.log(f"{addr[1]} CarMeet ConfiguredGameOptionsForWagerRace value={r.bits(32):#x}")
            else:
                # message layouts beyond the transport layer are not decoded yet
                self.log(f"{addr[1]} id={hdr['ack_id']} types={types} first-unknown={mtype} "
                         f"bits-left={r.size - r.pos} raw={data.hex()}")
                break
        else:
            left = r.size - r.pos
            if left >= 16 and self.leftover_logged < 8:
                self.leftover_logged += 1
                self.log(f"{addr[1]} packet of {len(data)} bytes: {left} bits left after {hdr['count']} messages "
                         f"types={types[-6:]} tail={(r.value >> r.pos).to_bytes((left + 7) // 8, 'little')[:60].hex()}")
        if len(data) > 122 and 71 not in types and self.big_logged < 10:
            self.big_logged += 1
            self.log(f"{addr[1]} big packet {len(data)} bytes types={types}: {data.hex()}")
        if replies or hdr["ack_id"] != UNRELIABLE_ID:
            self.send(peer, replies)

    # ---- Car Meet: game-mode invites (Free Roam / Road Trip / Showdown) ----
    def cm_members(self):
        return [q for q in self.peers.values() if q.connected and q.entered and q.lobby_type == LOBBY_TYPE_CAR_MEET]

    def cm_slot(self, peer):
        order = sorted(self.cm_members(), key=lambda c: c.owner_id)
        return order.index(peer) if peer in order else 0

    def cm_broadcast(self, write_vars, only=None) -> None:
        for q in (only if only is not None else self.cm_members()):
            q.cm_seq = (q.cm_seq + 1) % 0xFF
            self.send(q, [car_meet_update(q.cm_seq, write_vars)])

    def cm_invite(self, peer, invite_type: int) -> None:
        g = self.gm
        if invite_type in (CM_INVITE_FREE_ROAM, CM_INVITE_ROAD_TRIP):
            if g["phase"] != "idle":
                self.log(f"{peer.addr[1]} CarMeet invite ignored: a game-mode invite is already {g['phase']}")
                return
            g.update(type=invite_type, phase="joining", starter=peer, end=(self.now_ms() + CM_JOIN_WINDOW_MS) & 0xFFFFFFFF,
                     joined={peer: CM_JOIN_GAMEMODE}, session=None)
            self.cm_broadcast(car_meet_vars(invite_type=invite_type, countdown=CM_JOIN_WINDOW_MS, mode_end=g["end"],
                                            joins={self.cm_slot(peer): CM_JOIN_GAMEMODE}))
            self.log(f"CarMeet: game-mode invite type {invite_type} started by owner {peer.owner_id}, join window "
                     f"{CM_JOIN_WINDOW_MS} ms")
        else:
            self.log(f"{peer.addr[1]} CarMeet showdown (wager race) invite: not served yet")

    def cm_joins(self):
        """Every member's join state, so a lost update cannot leave a client's join list stale."""
        g = self.gm
        return {self.cm_slot(q): (CM_JOIN_GAMEMODE if q in g["joined"] else CM_JOIN_NONE) for q in self.cm_members()}

    def cm_join(self, peer, join: int, join_type: int) -> None:
        g = self.gm
        if join_type == CM_JOIN_WAGER or g["phase"] != "joining":
            return
        # Observed in game (2026-10-17): for the game-mode invite the bit means the opposite of its name. Accepting (Y on "Join")
        # arrives as join=0, while the client's own automatic messages (one around 1-4 s after the invite starts, a second one
        # a few seconds later) arrive as 0 then 1 and put a player who never pressed anything into the lobby.
        join = 0 if join else 1
        if join:
            g["joined"][peer] = CM_JOIN_GAMEMODE
        else:
            g["joined"].pop(peer, None)
        self.cm_broadcast(car_meet_vars(joins=self.cm_joins()))
        self.log(f"CarMeet: owner {peer.owner_id} {'joined' if join else 'left'} the game-mode invite "
                 f"({len(g['joined'])} joined)")

    def cm_create_session(self, mode: int, xuids=()):
        """The game-mode session is created by the LSP (MPSD stand-in) on the server's behalf: the client's own creation path
        does not complete here. Returns (name, template) or (None, None)."""
        import json
        import urllib.request
        base = os.environ.get("FH2MP_LSP", "http://127.0.0.1:5080")
        try:
            with urllib.request.urlopen(f"{base}/admin/gamemode?type={mode}&xuids={','.join(str(x) for x in xuids)}", timeout=3) as resp:
                doc = json.loads(resp.read().decode())
            return doc["name"], doc["template"]
        except Exception as exc:  # noqa: BLE001
            self.log(f"CarMeet: could not create a game-mode session through the LSP: {exc!r}")
            return None, None

    def cm_tick(self, now_ms: int) -> None:
        g = self.gm
        if g["phase"] == "ready" and ((now_ms - g["reset"]) & 0xFFFFFFFF) < 0x80000000:
            g.update(phase="idle", type=0, starter=None, joined={}, session=None)
            self.cm_broadcast(car_meet_vars(invite_type=CM_INVITE_NONE))
            return
        if g["phase"] == "joining" and now_ms - g.get("snap", 0) >= 1000:
            g["snap"] = now_ms
            self.cm_broadcast(car_meet_vars(joins=self.cm_joins()))      # updates are unreliable: repeat the full state
        if g["phase"] == "joining" and ((now_ms - g["end"]) & 0xFFFFFFFF) < 0x80000000:
            starter = g["starter"]
            joined = [q for q in g["joined"] if q.connected]
            if starter is None or not starter.connected or not joined:
                self.log("CarMeet: game-mode invite cancelled (nobody left)")
                g.update(phase="idle", type=0, starter=None, joined={})
                self.cm_broadcast(car_meet_vars(invite_type=CM_INVITE_NONE))
                return
            g["phase"] = "creating"
            # nobody creates the session on the client side: everyone waits for the one the server makes
            for q in joined:
                self.send(q, [car_meet_notify_pending(CM_PENDING_WAIT)])
            name, template = self.cm_create_session(g["type"], [q.xuid for q in joined if q.xuid])
            if not name:
                g.update(phase="idle", type=0, starter=None, joined={})
                self.cm_broadcast(car_meet_vars(invite_type=CM_INVITE_NONE))
                return
            g.update(phase="ready", session=(name, template), reset=(now_ms + 30000) & 0xFFFFFFFF)
            for q in joined:
                if self.hub is not None:
                    self.hub.assign[q.xuid] = name      # they join this lobby's world when they connect
            self.cm_broadcast(car_meet_vars(session=name, template=template))
            for q in joined:
                self.send(q, [car_meet_session_ready(name, template)])
            self.log(f"CarMeet: join window over ({len(joined)} joined): session {name} ({template}) sent to "
                     f"{[q.owner_id for q in joined]}")

    def cm_session_ready(self, peer, name: str, template: str) -> None:
        g = self.gm
        self.log(f"CarMeet: GameModeSessionReady from owner {peer.owner_id}: session {name!r} template {template!r}")
        if g["phase"] != "creating" or peer is not g["starter"]:
            return
        g.update(phase="ready", session=(name, template))
        self.cm_broadcast(car_meet_vars(session=name, template=template))
        for q in g["joined"]:
            if q is not peer and q.connected:
                self.send(q, [car_meet_session_ready(name, template)])

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
                    self.rt_pick_players = here
                    self.rt_votes.clear()
                    self.rt_winner = None
                    self.log(f"road trip candidates: locations {self.rt_candidates[0]} events "
                             f"{self.rt_candidates[1]} classes {self.rt_candidates[2]}")
                self.rt_state = RT_DESTINATION_VOTE
                self.rt_end_ms = self.now_ms() + RT_VOTE_MS
            elif self.rt_state == RT_DESTINATION_VOTE and self.rt_gamedb and not self.rt_votes:
                # The vote was drawn when only the first player was here, which leaves out the events that need two (King,
                # Infection): draw it again when another player arrives, before anybody has voted.
                here = sum(1 for c in self.peers.values() if c.connected)
                if here > self.rt_pick_players:
                    self.rt_candidates = pick_road_trip_candidates(self.rt_gamedb, here)
                    self.rt_pick_players = here
                    self.log(f"road trip: {here} players, vote drawn again: locations {self.rt_candidates[0]} events "
                             f"{self.rt_candidates[1]}")
                    for o in self.peers.values():
                        if o is not q and o.connected and o.rt_started:
                            self.send(o, [road_trip_event_selection_update(o.es_seq, *self.rt_candidates, votes=self.rt_counts())])
                            o.es_seq = (o.es_seq + 1) % 0xFF
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
        if self.carmeet or any(q.lobby_type == LOBBY_TYPE_CAR_MEET for q in self.peers.values()):
            self.cm_tick(self.now_ms())
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
        self.weather_tick(now)
        self.traffic.tick(now)
        if now >= self.mt_next:
            self.mt_next = now + 30
            if self.mt_seen and any(p.connected for p in self.peers.values()):
                self.log("msg types seen (last 30 s): " + " ".join(f"{k}:{v}" for k, v in sorted(self.mt_seen.items(), key=lambda kv: str(kv[0]))))
            self.mt_seen = {}
        for peer in list(self.peers.values()):
            if not peer.connected and now - peer.last_rx > PENDING_SECONDS:
                self.peers.pop(peer.addr, None)          # never finished the handshake
                continue
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


class Hub:
    """One UDP port, many worlds. Each lobby the matchmaking side creates (the LSP announces "assign <xuid> <lobby>" on the
    control port) is its own Server instance with its own peers, races and Road Trip state; a client is routed to the world of
    its lobby when it opens the connection. The connect request's field a is the lobby type (1 Road Trip, 2 Free Roam,
    3 Car Meet); clients without an assignment (direct IP) share one default world per type."""
    IDLE_WORLD_SECONDS = 120.0

    def __init__(self, port: int, logpath, roadtrip: bool = False, carmeet: bool = False):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", port))
        self.log_file = open(logpath, "a", buffering=1) if logpath else None
        self.default_type = LOBBY_TYPE_CAR_MEET if carmeet else LOBBY_TYPE_ROAD_TRIP if roadtrip else 2
        self.assign = {}      # xuid -> lobby name
        # Signed-in players, announced by the LSP over the control port ("auth <xuid hex> <ip>"): xuid -> (ip, valid until). With
        # FH2MP_UDP_AUTH=required an OpenConnection is only accepted from the address its xuid signed in from (the xuid in the packet is
        # otherwise just a claim); "off" keeps the old open behaviour for local rigs.
        self.authed = {}
        self.revoked = {}     # xuid -> until: banned / kicked by the LSP ("revoke <xuid hex>"); refused until the next sign-in announcement
        self.udp_auth = os.environ.get("FH2MP_UDP_AUTH", "off")
        self.auth_log = {}
        self.worlds = {}      # (lobby type, lobby name) -> Server
        self.route = {}       # client address -> Server
        self.empty_since = {}
        self.ctl = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.ctl.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.ctl.bind(("127.0.0.1", int(os.environ.get("FH2MP_CONTROL_PORT", port + 90))))
        except OSError:
            self.ctl = None
        self.idle = Server(port, None, sock=self.sock, log_file=self.log_file, tag="[idle] ")
        self.idle.is_idle = True
        self.idle.log(f"hub listening on udp/{port}, control udp/{self.ctl.getsockname()[1] if self.ctl else '-'}")

    def log(self, msg: str) -> None:
        self.idle.log(msg)

    def world_for(self, lobby_type: int, name: str):
        key = (lobby_type, name)
        w = self.worlds.get(key)
        if w is None:
            tag = f"[{ {1: 'roadtrip', 2: 'freeroam', 3: 'carmeet'}.get(lobby_type, lobby_type) }:{name[:8] or 'default'}] "
            w = Server(0, None, roadtrip=lobby_type == LOBBY_TYPE_ROAD_TRIP, carmeet=lobby_type == LOBBY_TYPE_CAR_MEET,
                       sock=self.sock, log_file=self.log_file, tag=tag)
            w.hub_key = key
            w.hub = self
            self.worlds[key] = w
            self.log(f"new world {key}")
        return w

    def control(self) -> None:
        """Text datagrams from the matchmaking side: "assign <xuid hex> <lobby name>"."""
        while self.ctl:
            try:
                data, _ = self.ctl.recvfrom(1024)
            except (BlockingIOError, OSError):
                return
            parts = data.decode("utf-8", "replace").split()
            if len(parts) == 3 and parts[0] == "auth":
                try:
                    xuid = int(parts[1], 16)
                    if xuid in self.revoked:
                        continue        # a ban or kick holds until the LSP says so again, i.e. until a fresh sign-in
                    self.authed[xuid] = (parts[2], time.monotonic() + AUTH_TTL_SECONDS)
                except ValueError:
                    pass
            elif len(parts) == 2 and parts[0] == "revoke":
                try:
                    self.revoke(int(parts[1], 16))
                except ValueError:
                    pass
            elif len(parts) == 2 and parts[0] == "allow":
                try:
                    self.revoked.pop(int(parts[1], 16), None)
                except ValueError:
                    pass
            elif len(parts) == 3 and parts[0] == "assign":
                try:
                    self.assign[int(parts[1], 16)] = parts[2]
                    self.log(f"control: player {parts[1]} assigned to lobby {parts[2]}")
                except ValueError:
                    pass

    def revoke(self, xuid: int) -> None:
        """A ban or kick: forget the sign-in, the lobby assignment, and end every live connection of this player."""
        self.authed.pop(xuid, None)
        self.assign.pop(xuid, None)
        self.revoked[xuid] = time.monotonic() + 86400.0
        dropped = 0
        for w in list(self.worlds.values()):
            for peer in [q for q in w.peers.values() if q.xuid == xuid]:
                try:
                    w.send(peer, [close_connection(0)], fast=True)
                except OSError:
                    pass
                w.drop_peer(peer, "revoked by the account service")
                dropped += 1
        self.log(f"control: revoked {xuid:#x} ({dropped} connection(s) ended)")

    def open_connection_fields(self, data: bytes):
        """(player id, lobby type) when the datagram starts with an OpenConnection, else None."""
        try:
            r = BitReader(data)
            hdr = parse_header(r)
            if hdr["count"] < 1 or r.bits(7) != MSG_OPEN_CONNECTION:
                return None
            r.string(), r.string()
            r.bits(2)
            player_id = r.bits(64)
            a = r.bits(32)
            return player_id, (a if a in (1, 2, 3) else self.default_type)
        except ValueError:
            return None

    def handle(self, data: bytes, addr) -> None:
        opened = self.open_connection_fields(data)
        cur = self.route.get(addr)
        if opened:
            player_id, ltype = opened
            if not self.open_allowed(player_id, addr):
                return
            held = cur.peers.get(addr) if cur is not None else None
            if held is not None and held.xuid and held.xuid != player_id:
                return                      # this address already belongs to another player's connection
            if held is None and not self.pending_room(addr):
                return                      # before a world is picked or made for it: a flood must not create worlds
            name = self.assign.get(player_id)
            target = self.world_for(ltype, name) if name else self.open_world_for(ltype)
            if cur is not None and cur is not target:
                old = cur.peers.get(addr)
                if old is not None:
                    cur.drop_peer(old, "moved to another lobby")
                    cur.peers.pop(addr, None)
            self.route[addr] = cur = target
        if cur is None:
            cur = self.idle        # a client that survived a server restart: it gets a CloseConnection
        cur.handle(data, addr)

    def pending_room(self, addr) -> bool:
        """Half-open connections (opened, handshake not finished) across all worlds, in total and per source address."""
        total = mine = 0
        for w in self.worlds.values():
            for a, p in w.peers.items():
                if not p.connected:
                    total += 1
                    mine += a[0] == addr[0]
        return total < MAX_PENDING_PEERS and mine < MAX_PENDING_PER_IP

    def open_allowed(self, player_id: int, addr) -> bool:
        """With FH2MP_UDP_AUTH=required an OpenConnection is only accepted from the address its xuid signed in from. Called by the
        worlds for every OpenConnection message, not just the first of a datagram."""
        if player_id in self.revoked:
            return False
        if self.udp_auth != "required" or self.is_authed(player_id, addr[0]):
            return True
        now = time.monotonic()
        if len(self.auth_log) < 5000 and now - self.auth_log.get(addr[0], -1e9) > 10:   # one line per address per 10 s, whatever it sends
            self.auth_log[addr[0]] = now
            self.log(f"auth: refused OpenConnection of {player_id:#x} from {addr[0]} (not signed in from this address)")
        return False

    def is_authed(self, player_id: int, ip: str) -> bool:
        ent = self.authed.get(player_id)
        return ent is not None and ent[1] > time.monotonic() and ent[0] == ip

    WORLD_CAPACITY = 12

    def open_world_for(self, lobby_type: int):
        """A player with no lobby assignment (joined from the menu: the matchmaking side cannot reach this hub's control port from the
        PC) goes to the busiest world of that type that has room, so menu joiners meet each other and whoever is already
        playing; a new world when every one of them is full. Players still connecting count (they are routed before their handshake
        ends), otherwise a burst of arrivals all land in the same world."""
        load = {}
        for w in self.route.values():
            load[id(w)] = load.get(id(w), 0) + 1
        best, best_n = None, 0
        for (ltype, _name), w in self.worlds.items():
            n = load.get(id(w), 0)
            if ltype == lobby_type and 0 < n < self.WORLD_CAPACITY and n > best_n:
                best, best_n = w, n
        if best is not None:
            return best
        shared = self.worlds.get((lobby_type, ""))
        if shared is None or load.get(id(shared), 0) == 0:
            return self.world_for(lobby_type, "")                  # nobody anywhere: the shared default world
        self.auto_worlds = getattr(self, "auto_worlds", 0) + 1
        return self.world_for(lobby_type, f"auto{self.auto_worlds}")

    def sweep(self) -> None:
        now = time.monotonic()
        for xuid, (_ip, until) in list(self.authed.items()):
            if until < now:
                del self.authed[xuid]
        for xuid, until in list(self.revoked.items()):
            if until < now:
                del self.revoked[xuid]
        if len(self.auth_log) > 5000:
            self.auth_log.clear()
        for addr, w in list(self.route.items()):
            if addr not in w.peers:
                del self.route[addr]
        for key, w in list(self.worlds.items()):
            if any(p.connected for p in w.peers.values()):
                self.empty_since.pop(key, None)
            elif now - self.empty_since.setdefault(key, now) > self.IDLE_WORLD_SECONDS:
                self.log(f"world {key} closed (empty)")
                del self.worlds[key]
                self.empty_since.pop(key, None)

    def serve(self) -> None:
        sel = selectors.DefaultSelector()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        self.sock.setblocking(False)
        sel.register(self.sock, selectors.EVENT_READ)
        if self.ctl:
            self.ctl.setblocking(False)
            sel.register(self.ctl, selectors.EVENT_READ)
        last_sweep = 0.0
        while True:
            if sel.select(timeout=0.01):
                self.control()
                while True:     # drain everything queued before the next timer pass
                    try:
                        data, addr = self.sock.recvfrom(65535)
                    except BlockingIOError:
                        break
                    try:
                        self.handle(data, addr)
                    except Exception as exc:  # keep serving
                        self.log(f"error handling {data.hex()}: {exc!r}")
            for w in list(self.worlds.values()):
                try:
                    w.tick()
                except Exception as exc:  # one broken world must not stop the others
                    w.log(f"tick error: {exc!r}")
            now = time.monotonic()
            if now - last_sweep > 1.0:
                last_sweep = now
                self.sweep()


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = int(args[0]) if len(args) > 0 else 5000
    logfile = args[1] if len(args) > 1 else None
    try:
        Hub(port, logfile, roadtrip="--roadtrip" in sys.argv, carmeet="--carmeet" in sys.argv).serve()
    except KeyboardInterrupt:
        pass
