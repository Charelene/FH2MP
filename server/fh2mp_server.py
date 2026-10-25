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
import collections
import json
import math
import os
import random
import selectors
import socket
import sys
import struct
import threading
import time
import traceback
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import traffic as traffic_mod  # noqa: E402
import gameopts  # noqa: E402
import replay  # noqa: E402
import tunnel as tunnel_mod  # noqa: E402
try:
    from car_policy import BLOCKED_CARS, KNOWN_CARS, TOP_SPEED_KMH  # noqa: E402     # tools/make_car_policy.py
except ImportError:
    BLOCKED_CARS, KNOWN_CARS, TOP_SPEED_KMH = {}, frozenset(), {}
try:
    from car_policy import CAR_CHOICES, RENTAL_CARS  # noqa: E402
except ImportError:
    CAR_CHOICES, RENTAL_CARS = {}, frozenset()
try:
    import parts as parts_check  # noqa: E402     # part ids of the game data: tools/make_part_ids.py -> part_ids.py
except ImportError:
    parts_check = None


def _dlc_cars():
    """Car ids that exist in the game but not in the base catalog (DLC): one id per line or separated by spaces in data/dlc_cars.txt (env FH2MP_DLC_CARS)."""
    path = os.environ.get("FH2MP_DLC_CARS") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "dlc_cars.txt")
    try:
        with open(path) as f:
            return frozenset(int(x) for line in f for x in line.split("#")[0].split() if x.lstrip("-").isdigit())
    except OSError:
        return frozenset()


DLC_CARS = _dlc_cars()

SYNC_GAMEDB = os.environ.get("FH2MP_SYNC_GAMEDB", "")    # the car database every launcher syncs (gamedb_sync.py): its cars and part rows count as known
_sync = {"key": None, "checked": -1e9}


def load_sync_catalog(log=print, every=30.0):
    """Adds the cars and part rows of FH2MP_SYNC_GAMEDB (car packs on top of the stock database) to the known ones, again whenever the
    file changes (looked at once per `every` seconds). Rows are only ever added. -> True when it (re)loaded."""
    global KNOWN_CARS, RENTAL_CARS
    now = time.monotonic()
    if not SYNC_GAMEDB or now - _sync["checked"] < every:
        return False
    _sync["checked"] = now
    try:
        st = os.stat(SYNC_GAMEDB)
    except OSError:
        return False
    key = (st.st_mtime_ns, st.st_size)
    if key == _sync["key"]:
        return False
    _sync["key"] = key
    import sqlite3
    import part_tables
    try:
        c = part_tables.open_ro(SYNC_GAMEDB)
        try:
            cars = part_tables.read_cars(c)
            choices = part_tables.read_car_choices(c)
            rented = part_tables.read_rentals(c)
            tables = part_tables.read_parts(c) if parts_check is not None else None
        finally:
            c.close()
    except sqlite3.Error as e:
        log(f"car database {SYNC_GAMEDB}: not read ({e}); the known cars stay as they were")
        return False
    new = sorted(set(cars) - KNOWN_CARS)
    KNOWN_CARS = KNOWN_CARS | frozenset(cars)
    for cid, (_name, kmh) in cars.items():
        if kmh:
            TOP_SPEED_KMH.setdefault(cid, kmh)
    for cid, cp in choices.items():
        CAR_CHOICES.setdefault(cid, cp)
    RENTAL_CARS = frozenset(rented) | RENTAL_CARS
    added = parts_check.extend(tables) if tables else 0
    log(f"car database {SYNC_GAMEDB}: {len(cars)} cars ({len(new)} new: {new[:20]}), {added} new part ids")
    return True

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
CONSOLE_VAR_HAS_LOADED_CARS = 5     # HasLoadedCarsForAllParticipants (the grid waits for it, see maybe_begin_race)
CONSOLE_VAR_HAS_GRID = 6
CONSOLE_VAR_RENDER_READY = 0
CONSOLE_BOOL_VAR_NAMES = {0: "RenderReady", 1: "SessionEntryFinalised", 2: "ParticipatingInRace",
                          3: "RaceInProgress", 4: "Ready", 5: "HasLoadedCarsForAllParticipants",
                          6: "HasGrid"}
RACE_INVITE_SECONDS = 20.0          # leader requested a race: wait this long for others, then start
RACE_JOIN_GRACE_SECONDS = 45.0      # ...but keep waiting this long for clients that accepted and are picking a car (90 held a lobby for nothing)
RACE_OWN_REQUEST_SECONDS = 3.0      # a client that wrote its own GameOptions this close to the request was starting its own event
LOAD_SUSPECT_STEP_SECONDS = float(os.environ.get("FH2MP_LOAD_SUSPECT_STEP", "10"))   # nobody loaded: one suspect dropped per step
EVENT_BLOCK_MAX_SECONDS = 1200.0    # a running event stops blocking new race requests after this long
EVENT_LOAD_TIMEOUT_SECONDS = float(os.environ.get("FH2MP_EVENT_LOAD_TIMEOUT", "60"))   # a participant that has not reported HasLoadedCars this long after the event start is dropped from it
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
CAR_POLICY = os.environ.get("FH2MP_CAR_POLICY", "kick")   # a car in BLOCKED_CARS (traffic vehicles): off = ignore, log = report it, kick = withhold it and end the account's connections
PART_POLICY = os.environ.get("FH2MP_PART_POLICY", "fix")     # a car whose part list holds an id that is not in the game data (it crashes the other clients that build it): fix = relay it with stock parts / log / kick / off
UNKNOWN_CAR_POLICY = os.environ.get("FH2MP_UNKNOWN_CAR_POLICY", "standin")   # a car that is in neither the game data, the synced car database nor data/dlc_cars.txt (a car pack the server does not have): standin = the others see STANDIN_CAR / off / log / kick
TEST_CAR = int(os.environ.get("FH2MP_TEST_CAR", "0") or 0)    # dev test only: every relayed car is shown to the others as this car (stock parts)
RENTAL_CLASS_ID = 7                  # CarClasses 7 = R: what a rental's PlayerChoices carry (the grid showed "R 999"), never a real car's class
STANDIN_CAR = int(os.environ.get("FH2MP_STANDIN_CAR", "1215"))    # NUL_Car_00: the game's own placeholder car (Data_Car 1215, media/cars/nul_car_00.zip)
SPEED_POLICY = os.environ.get("FH2MP_SPEED_POLICY", "kick")   # speed check: off / log / kick. kick (default since 2026-10-22): a car held above HARD_CAP_KMH for SPEED_SECONDS is removed; the soft per-car limit stays log only
CAR_CAPTURE = bool(os.environ.get("FH2MP_CAR_CAPTURE"))   # dev only: log every car object message and every speed measurement (ground truth for the checks)
ROUTER_CAR = 2                      # ICarRouter: the client's ForzaCarNetworked (CarInitSynced, CarSetupSynced, CarColorSynced, ...)
# Speed check on what a car actually does (a client can lie about its car, parts and tune, not about where it ends up). Speeds come from
# the car positions the server relays; all limits are guesses to be calibrated from the logs (the fastest stock car, the Venom GT, does 439 km/h).
HARD_CAP_KMH = 451.0                # no car does this for long, however it was modified (user decision 2026-10-21)
SOFT_FACTOR, SOFT_EXTRA_KMH = 1.5, 140.0   # soft limit = min(hard cap, max(stock x 1.5, stock + 140 km/h)); log only
TELEPORT_KMH = 1000.0               # an apparent speed above this is a respawn / fast travel, not driving
CHEATER_POOL_HOURS = float(os.environ.get("FH2MP_CHEATER_POOL_HOURS", "24"))   # how long a kicked player is matched only with other kicked players
SPEED_SECONDS = 3.0                 # how long a speed must hold before it is reported
FLIGHT_CLIMB_KMH = 150.0            # vertical speed a car cannot hold: a ramp jump decays under gravity within a second (v0 would have to be 250 km/h straight up)
FLIGHT_SECONDS = 3.0                # climbing faster than that for this long = flying (reported 2026-10-22: F2GR, "literally flying")
POSITION_LIMIT = 50000.0            # metres: a car further from the origin than this (or a NaN) is logged as out of range
WIPE_MIN_DROPS = 3                  # this many clients removed within WIPE_WINDOW_S seconds is reported as a wipe
WIPE_WINDOW_S = 3.0
WIPE_CONTEXT_S = 14.0               # a wipe report shows what happened this long before the clients went silent


def where(exc, depth=4):
    """The last `depth` frames of an exception as "function:line < caller:line ...", for one-line error logs."""
    tb = traceback.extract_tb(exc.__traceback__)[-depth:]
    return " < ".join(f"{f.name}:{f.lineno}" for f in reversed(tb)) or "?"


def car_model_id(size, payload):
    """ModelId (the car) of a car object update, or None. CarInitSynced.ModelId is the first variable, a 32-bit integer (ctor FUN_1417e3db0);
    the 37-bit update a client sends right after it creates its car is read as a 5-bit variable id 0 followed by that value [inferred; check the
    logged payload against a known car on a real client before using FH2MP_CAR_POLICY=kick]."""
    if size == 37 and (payload & 31) == 0:
        v = (payload >> 5) & 0xFFFFFFFF
        return v - (1 << 32) if v >= 1 << 31 else v
    return None


def car_position(rest):
    """(x, y, z) of a CarPredictionFull body, or None. The first 96 bits of CarPredictionState are three 32-bit values (FUN_14181ffe0,
    the same vec3 the traffic messages use for a position), read here as floats [inferred, check on a real client]."""
    try:
        return struct.unpack('<3f', (rest & ((1 << 96) - 1)).to_bytes(12, 'little'))
    except (struct.error, OverflowError):
        return None
CAR_KEY_ID_BITS = 7            # FUN_141808170: object key written with 7 id bits
# Road Trip (router 9): RoadTripObjectTypes 0 RoadTripSynced (server-owned), 1 RoadTripEventSelection,
# 2 RoadTripPlayerSynced (client-owned). RoadTripSynced vars (ctor FUN_141812ae0, 10 vars -> 4-bit var ids);
# widths other than the state enum are assumed (32-bit ints, 1-bit bool) until the client confirms them.
ROUTER_ROADTRIP = 9
RT_TYPE_SYNCED, RT_TYPE_EVENT_SELECTION, RT_TYPE_PLAYER = 0, 1, 2
RT_VAR_ID_BITS = 4
LOBBY_PLAYER_PLAYER_HOST = 1     # var index in Nerd::LobbyPlayer (FUN_14180bf50): Ready 0, PlayerHost 1, ...
# The player object's PlayerChoices (Nerd::PlayerChoices, ctor FUN_1417e7bb0) starts at var 81: CarClass 81, PerfIndex 82, the five ratings 83-87,
# AIRank 88, AIIsFriend 89, TeamId 90 (1 bit), PlayerBucketId 91, PittingProfile 92, LiveryId 93 [C: a client's car-select update carries
# 82-87 and 93 with these widths]. Race setup (FUN_140930540) copies every participant's TeamId into its car (race data +0x2a9), which the grid,
# HUD and team scoring read. No client sets it, so the server assigns the teams (dev 2026-10-22: without this everybody was on red).
PLAYER_VAR_TEAM_ID = 90
# Nerd::LobbyPlayer's vars start at 12 in the player object (network order FUN_14180bb10: Ready 12, PlayerHost 13, ... PlayerStatus 19,
# ... PrivateUniverseId 40) [C: Ready (1 bit) arrives as var 12 on arrival, PlayerStatus (4 bits, 1..4 through car select / drive /
# arrival) as var 19]. PrivateUniverseId (SyncedEnum<UniverseIds>, 5 bits: 0 OfflineDefault, 1-17 OnlinePrivate0-16, 18-23 BucketList1-6,
# 24 OnlineFreeroam) is the universe NetworkForceLocalCarToPrivateUniverse moves the car into (garage, autoshow, Road Trip arrival / XP
# reward screen). Left at 0 every player's "private" universe was the same one, so on the Road Trip XP reward screen both cars stood on the
# same spot (dev 2026-10-22). The server gives every player in a world its own OnlinePrivateN.
PLAYER_VAR_PRIVATE_UNIVERSE, PRIVATE_UNIVERSE_BITS = 40, 5
PRIVATE_UNIVERSE_FIRST, PRIVATE_UNIVERSE_COUNT = 1, 17
AUTH_TTL_SECONDS = 600.0      # how long a sign-in announced by the LSP lets that address open a connection without further news
RT_INIT_SECONDS = float(os.environ.get("FH2MP_RT_INIT", "3.0"))   # dev knob: hold the state in Initialising         # Initialising -> DestinationVote delay after the connection (the client waits in Initialising
                              # and only sends its session-entry message once the state has changed)
RT_EXCLUDED_MODES = set()        # RaceModes.id never offered (King 25 and Infection 26 are served: tag events, see TAG_MODES)
RT_CAR_CLASSES = 6               # CarClasses 0..5 = D C B A S1 S2 (6 = X and 7 = R are rental/event-specific)
RT_EXCLUDED_LOCATIONS = {9}      # Storm Island: expansion content, not offered for now
# Team variants. The client lists every online event twice, as id and id + 10000, the second marked as a team event (FUN_1409ee710 builds its
# OnlineEventDatabase that way for every mode except King / Infection) and the map offers them as FreeForAllOnlineEventId / TeamOnlineEventId.
# Teams are two (ScoringOptions.NumTeams). The client does not derive them from the grid (dev 2026-10-22: everybody was red): the server
# writes each player's PlayerChoices.TeamId (PLAYER_VAR_TEAM_ID) at event start and sends a grid that alternates red and blue.
TEAM_EVENT_OFFSET = 10000
RT_TEAM_MODES = {0, 1, 2, 4, 5, 31}   # laps, street, cross country, scramble, trail, point to point: every mode that ranks the field
RT_TEAM_MIN_PLAYERS = int(os.environ.get("FH2MP_TEAM_MIN_PLAYERS", "4"))   # fewer players than this are not offered a team event
RT_TEAM_EVENTS_PER_LOCATION = int(os.environ.get("FH2MP_TEAM_EVENTS", "1"))  # how many of a destination's 4 events are team events
RT_TEAM_ONLY = os.environ.get("FH2MP_RT_TEAM_ONLY") == "1"
# EventRaceId of a fresh lobby's first drive. After car select the client always fast-travels to the grid at the finish of EventRaceId
# (network_roadtrip_states.xml free_roam_ui -> fast_travel_to_destination_drive_grid); sending the upcoming race put everyone next to the
# destination instead of at the festival (dev 2026-10-22). Unset: the old behaviour. Being tested: which value means "the festival".
RT_FIRST_RACE = os.environ.get("FH2MP_RT_FIRST_RACE")   # test switch: the vote offers only team events, whatever the group size
RT_VOTE_MS = 60 * 1000          # length of the destination vote
RT_ALL_VOTED_MS = 10 * 1000      # everybody voted: close the vote after this long
RT_ALL_PICKED_MS = 3 * 1000      # everybody picked a car: end car select after this long
RT_CAR_SELECT_MS = 30 * 1000     # car select phase length
RT_DRIVE_MS = 180 * 1000         # time allowed to drive to the event when the leg is unknown (see road_trip_drive_ms)
RT_DRIVE_MIN_MS = int(os.environ.get("FH2MP_RT_DRIVE_MIN", "150")) * 1000
RT_DRIVE_MAX_MS = int(os.environ.get("FH2MP_RT_DRIVE_MAX", "600")) * 1000
RT_DRIVE_SPEED_MS = 20.0         # m/s a group is assumed to average on the way (traffic, junctions, a mixed field of cars)
RT_DRIVE_DETOUR = 1.5            # road length over straight-line distance
RT_DRIVE_SLACK_S = 45.0          # on top of the driving: loading, the countdown, whoever starts late
RT_REGION_LEG_M = 3500.0         # straight-line distance between two events of the same destination (not in the game data; a guess)
# Where the destination hubs are (x, z in metres; entities destination_hubs.xml, the activation_position of each hub, by RoadTripLocationId).
# The festival (location 0, where a new lobby spawns) is not in that file; the middle of the map is a guess and only sizes the first drive.
RT_HUB_XZ = {3: (-3532.0, -2308.0), 4: (-821.0, -6061.0), 5: (2241.0, -3100.0), 6: (9941.0, -7896.0), 7: (8831.0, -3573.0),
             8: (5111.0, -3528.0)}
RT_FESTIVAL_XZ = (0.0, 0.0)
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
RELAY_HISTORY_CAP = 8000       # stored messages per object, replayed to late joiners (compacted, see replay.py); a memory guard
# Outgoing messages are queued per client and sent together: one datagram per client every few ms instead of one per relayed car update.
# On prod (2026-10-21, 72 players) half the server's time went into sendto (~12k datagrams/s), the receive queue overflowed and races rubberbanded.
RELAY_FLUSH_SECONDS = float(os.environ.get("FH2MP_RELAY_FLUSH_MS", "10")) / 1000.0   # longest a relayed message waits for company
MAX_PACKET_BYTES = int(os.environ.get("FH2MP_MAX_PACKET_BYTES", "1000"))          # late-join replays already send ~1 KB datagrams
REPLY_HOLD_SECONDS = 0.002     # replies leave >= 2 ms after the client's packet (see Server.send)
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


def bake(write):
    """A message written once, as (bits, bit count), so it can be queued for many clients without writing it again."""
    w = BitWriter()
    write(w)
    return (w.value, w.pos)


PACKET_HEADER = struct.Struct("<HHIBH")    # ackId, remoteAckId, ackBits, count, size: the 88 header bits are byte aligned


def pack_messages(baked, ack_id=0xFFFF, remote_ack=0, ack_bits=0) -> bytes:
    """build_packet for messages already baked; same bytes."""
    body = pos = 0
    for value, n in baked:
        body |= value << pos
        pos += n
    nbytes = (pos + 7) // 8
    return PACKET_HEADER.pack(ack_id, remote_ack, ack_bits, len(baked), HEADER_BITS // 8 + nbytes) + body.to_bytes(nbytes, "little")


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
TAG_KEYFRAME_MIN_SECONDS = 20.0               # score keyframes earlier than this after GO are not taken as "the event is over"
TAG_GRID_ASSIGN_DELAY = 1.5                   # seconds after the starting grid is sent: the first It car is drawn while the grid screen is up
TAG_ASSIGN_REPEATS = (0.0, 0.4, 1.2, 3.0)      # seconds after the first send: the datagrams are unreliable, so repeat them
LSP_URL = os.environ.get("FH2MP_LSP", "http://127.0.0.1:5080")    # this stack's LSP (loopback /admin endpoints)


def post_result(mode, detail, log):
    """An online event's result to the LSP's /admin/result (the website's statistics, server/site_stats.py). Runs on its own thread."""
    body = json.dumps({"mode": mode, "detail": detail}).encode()
    req = urllib.request.Request(f"{LSP_URL}/admin/result", data=body, method="POST", headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            resp.read()
    except Exception as exc:  # noqa: BLE001 - statistics only: never more than a log line
        log(f"event result not stored ({mode}): {exc!r}")


RESULT_SINK = post_result      # tests replace it

# Free roam events are chosen on the map, so the server is never told their mode (MsgRequestRace has no payload). The leader's GameOptions say
# it. Found by lining up payloads whose Road Trip event, and so mode, is known (prod logs 2026-10-19/20): bits 0-7 are the event type (0 street
# race and tag modes, 3 laps, 4 point to point); a tag mode also sets bits 10-12 and has a long (~700 bit) tag section, where a street race
# leaves bits 10-12 clear. Only King has been seen: the Free Roam payloads at 00:17 and 01:56 have the Road Trip King's header. Infection is
# told apart from the first hit (tag_collision): an It car that stays It.
GAME_OPTIONS_TAG_MIN_BITS = 400
GAME_OPTIONS_TAG_BITS = 0b111 << 10
# Infection in Free Roam (seen on dev 2026-10-20, the player had picked Infection: 667 bits, header 04600c2000): bits 10-12 are NOT set there,
# so the King test above misses it. The byte above bit 31 changes between events (04600c2000, later 1b600c2000; King: 04700c1c00, 1b700c1c00), so only the low 32 bits are compared. The same header (731 and 826 bits) was logged twice on prod as "not a tag mode" on the day players
# reported Infection hanging. Matched on the low 40 bits and a long tag section.
GAME_OPTIONS_INFECTION_HEADER = 0x600C2000          # low 32 bits; the byte above them varies (04 at the first event of a session, 1b later, as with King)


def game_options_tag_mode(game_options):
    """RACE_MODE_KING or RACE_MODE_INFECTION when the lobby's GameOptions (size, payload) look like a tag event, else None."""
    if not game_options:
        return None
    size, payload = game_options
    if size >= GAME_OPTIONS_TAG_MIN_BITS and payload & 0xFFFFFFFF == GAME_OPTIONS_INFECTION_HEADER:
        return RACE_MODE_INFECTION
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


def is_team_event(event_id: int) -> bool:
    return event_id >= TEAM_EVENT_OFFSET


def base_event_id(event_id: int) -> int:
    """The AnthemOnlineEvents row of an event id; the team variant of an event (id + 10000) is the same row."""
    return event_id - TEAM_EVENT_OFFSET if is_team_event(event_id) else event_id


def road_trip_drive_ms(from_loc, to_loc) -> int:
    """How long a group gets for a drive. The legs: from the festival (from_loc None: a fresh lobby) or from where the last event ended to the
    next event. Inside one destination the events are some kilometres apart; between destinations the distance of the two hubs counts.
    A destination far from the last one gets the most time, a short hop the least; players that arrive early end the wait for everybody."""
    if from_loc == to_loc and from_loc is not None:
        straight = RT_REGION_LEG_M
    else:
        a = RT_FESTIVAL_XZ if from_loc is None else RT_HUB_XZ.get(from_loc)
        b = RT_HUB_XZ.get(to_loc)
        if a is None or b is None:
            return RT_DRIVE_MS
        straight = max(math.hypot(a[0] - b[0], a[1] - b[1]), RT_REGION_LEG_M)
    seconds = straight * RT_DRIVE_DETOUR / RT_DRIVE_SPEED_MS + RT_DRIVE_SLACK_S
    return int(min(max(seconds * 1000, RT_DRIVE_MIN_MS), RT_DRIVE_MAX_MS))


def pick_road_trip_candidates(gamedb_path, players=1, rng=None):
    """3 random road-trip locations, 4 events each from the game's own event table, mixing race types where the
    location has them, a different car class per location. Only race modes that can run with `players` players
    are offered (RaceModes.MinimumPlayers: King and Infection need 2, a P2P or lap race 1). With RT_TEAM_MIN_PLAYERS players or
    more, one event per destination is offered as its team variant (id + 10000, see TEAM_EVENT_OFFSET)."""
    import random
    import sqlite3
    rng = rng or random.Random()
    db = sqlite3.connect(f"file:{gamedb_path}?mode=ro&immutable=1", uri=True)
    try:
        rows = db.execute("SELECT e.AnthemOnlineEventId, e.RoadTripLocationId, e.RaceModeId, m.MinimumPlayers "
                          "FROM AnthemOnlineEvents e JOIN RaceModes m ON m.id = e.RaceModeId "
                          "WHERE e.RoadTripLocationId > 0 AND m.MinimumPlayers <= ? AND e.RaceModeId NOT IN (%s)%s"
                          % (",".join(str(m) for m in (RT_EXCLUDED_MODES or {-1})),
                             " AND e.RaceModeId IN (%s)" % ",".join(map(str, sorted(RT_TEAM_MODES))) if RT_TEAM_ONLY else ""),
                          (max(1, players),)).fetchall()
    finally:
        db.close()
    by_loc = {}
    min_players = {}
    event_mode = {}
    for ev, loc, mode, minp in rows:
        by_loc.setdefault(loc, {}).setdefault(mode, []).append(ev)
        min_players[ev] = minp
        event_mode[ev] = mode
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
        if players >= RT_TEAM_MIN_PLAYERS or RT_TEAM_ONLY:
            # a team variant (id + 10000) of one or more of the destination's events, never both versions of the same event
            teamable = [i for i, e in enumerate(picked) if event_mode[e] in RT_TEAM_MODES]
            for i in rng.sample(teamable, len(teamable) if RT_TEAM_ONLY else min(RT_TEAM_EVENTS_PER_LOCATION, len(teamable))):
                team = picked[i] + TEAM_EVENT_OFFSET
                min_players[team] = max(min_players[picked[i]], RT_TEAM_MIN_PLAYERS)
                picked[i] = team
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
        self.rental_car = None     # car id when the player's current car is a rental (RENTAL_CARS), else None
        self.choices = None        # (class, PI) the player's game last sent in its PlayerChoices (vars 81/82)
        self.choices_notes = 0
        self.car_seen_at = 0.0     # last LobbyPlayer object that carried the car (join, car change)
        self.car_doubt_at = 0.0    # last time that car failed a check (unreadable part list, unknown car, bad parts): a load-timeout suspect
        self.gopts_written_at = 0.0   # last GameOptions this client wrote itself (it was setting up an event of its own)
        self.es_seq = 0           # server updates to the client's RoadTripEventSelection transfer object
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
        self.seq_shift = {}        # (owner id, router, id) -> how far the server's count of that object is ahead of what this client was sent (replay.py)
        self.compact_at = {}       # (router, id) -> history length at which it is compacted next
        self.console_vars = {}     # latest bool Console vars reported by this client
        self.gopts_seq = 0         # numbering of the server-owned GameOptions object sent to this client
        self.gopts_own_seq = None  # sequence number of the last GameOptions write this client made itself (it applied that to its own copy)
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
        self.pos = None            # last (x, y, z) of this client's car (CarPredictionFull), see car_position
        self.pos_flagged = False   # an out-of-range position was already logged for this client
        self.model_id = None       # the car this client uses (CarInitSynced.ModelId), see car_model_id
        self.model_logged = None
        self.max_y = -math.inf     # highest car position seen (y is up): 'flying' shows here
        self.climb_s = 0.0
        self.top_climb = 0.0       # fastest climb (km/h of vertical movement) over a second: a car does not do this on the ground
        self.car_payload_logs = 0
        self.kick_reason = None    # set by the car policy; acted on in tick()
        self.parts = None          # the car's 43 part ids as sent at join (parts.find_parts)
        self.part_notes = 0        # part check log lines written for this client
        self.part_logged = None    # (part list, policy) last reported, so one car is reported once and not on every update
        self.speed_ref = None      # (monotonic, position) the last speed was measured from
        self.top_kmh = 0.0         # highest measured speed (km/h)
        self.hard_s = 0.0          # seconds the speed has held above the hard cap
        self.soft_s = 0.0          # same for the soft limit
        self.speed_flagged = set() # which reports were already made for this client
        self.max_abs = 0.0         # largest |coordinate| seen
        self.outbox = []           # baked messages waiting for the next datagram to this client (Server.send / flush_peer)
        self.due = math.inf        # when the queued messages must leave
        self.hold = 0.0            # not before this (a reply: 2 ms after the client's packet)
        self.ack_cache = None

    def note_packet_id(self, pid: int) -> None:
        if pid == UNRELIABLE_ID:
            return
        self.ack_cache = None
        if self.last_id == NO_ID or ((pid - self.last_id) % 0xFFFF) < 0x8000:
            self.last_id = pid
        self.seen.add(pid)
        if len(self.seen) > 64:  # keep it bounded
            keep = {p for p in self.seen if ((self.last_id - p) % 0xFFFF) <= 40}
            self.seen = keep

    def ack_fields(self):
        if self.ack_cache is not None:
            return self.ack_cache
        if self.last_id == NO_ID:
            return NO_ID, 0
        bits = 0
        for pid in self.seen:
            d = (self.last_id - pid) % 0xFFFF
            if 1 <= d <= 32:
                bits |= 1 << (d - 1)
        self.ack_cache = (self.last_id, bits)
        return self.ack_cache


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
        self.rt_last_race = None          # the event the group raced last (None in a fresh lobby): the next drive starts at its finish line
        self.rt_last_loc = None           # and the destination it belongs to
        self.rt_drive_ms = RT_DRIVE_MS    # length of the current drive
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
        self.recent = collections.deque(maxlen=2500)   # (monotonic, kind, owner, detail): what a wipe report replays
        self.drops = collections.deque(maxlen=64)      # (monotonic, owner, xuid, last_rx, pos, car id, why) of removed clients
        self.wipe_at = 0.0                             # when a pending wipe report is due
        self.wipe_last = 0.0
        self.wx = None
        self.wx_next = 0.0         # monotonic time of the next weather change
        self.weather_init()
        self.peers = {}
        self.dirty = set()         # peers with queued messages (send -> flush)
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

    def car_model_seen(self, peer: Peer, mid: int) -> bool:
        """The client told us its car. Returns True when the car object must not be relayed to the others (policy kick)."""
        peer.model_id = mid
        name = BLOCKED_CARS.get(mid)
        if peer.model_logged != mid:
            peer.model_logged = mid
            self.log(f"{peer.addr[1]} car model {mid}" + (f" ({name}: not obtainable in the game)" if name else ""))
            self.note("car", peer.owner_id, f"model {mid}" + (" BLOCKED" if name else ""))
        unknown = (not name and KNOWN_CARS and mid not in KNOWN_CARS and mid not in DLC_CARS)
        if unknown and UNKNOWN_CAR_POLICY != "off" and "unknown" not in peer.speed_flagged:
            peer.speed_flagged.add("unknown")
            self.log(f"CAR POLICY{'' if UNKNOWN_CAR_POLICY == 'kick' else ' (log only)'}: owner {peer.owner_id} xuid {peer.xuid:#x} uses car id {mid}, which is not in the game data"
                     f" (DLC cars are listed in data/dlc_cars.txt)" + (": ending the connection" if UNKNOWN_CAR_POLICY == "kick" else ""))
            if UNKNOWN_CAR_POLICY == "kick" and peer.kick_reason is None:
                peer.kick_reason = f"car id {mid} is not in the game"
        if unknown and UNKNOWN_CAR_POLICY == "kick":
            return True
        if not name or CAR_POLICY == "off":
            return False
        if CAR_POLICY == "kick":
            if peer.kick_reason is None:
                self.log(f"CAR POLICY: owner {peer.owner_id} xuid {peer.xuid:#x} uses car {mid} ({name}), which players cannot get: ending the connection")
                peer.kick_reason = f"car {mid} ({name})"
            return True
        self.log(f"CAR POLICY (log only): owner {peer.owner_id} xuid {peer.xuid:#x} uses car {mid} ({name}), which players cannot get")
        return False

    def track_speed(self, peer: Peer, pos) -> None:
        """Measure the car's speed from its relayed positions (about once a second, 3D distance over arrival time)."""
        now = time.monotonic()
        ref = peer.speed_ref
        if ref is None or now - ref[0] > 5.0:
            peer.speed_ref, peer.hard_s, peer.soft_s = (now, pos), 0.0, 0.0
            return
        dt = now - ref[0]
        if dt < 1.0:
            return
        peer.speed_ref = (now, pos)
        d = math.dist(pos, ref[1])
        kmh = 3.6 * d / dt
        if not math.isfinite(kmh) or kmh > TELEPORT_KMH:
            peer.hard_s = peer.soft_s = 0.0
            return
        peer.top_kmh = max(peer.top_kmh, kmh)
        climb = 3.6 * (pos[1] - ref[1][1]) / dt
        peer.top_climb = max(peer.top_climb, climb)
        peer.climb_s = peer.climb_s + dt if climb > FLIGHT_CLIMB_KMH else 0.0
        if peer.climb_s >= FLIGHT_SECONDS and "flight" not in peer.speed_flagged:
            peer.speed_flagged.add("flight")
            self.log(f"FLIGHT: owner {peer.owner_id} xuid {peer.xuid:#x} car {peer.model_id} climbing at {climb:.0f} km/h for {peer.climb_s:.0f} s, "
                     f"now at height {pos[1]:.0f}" + ("" if SPEED_POLICY != "kick" else ": ending the connection"))
            if SPEED_POLICY == "kick" and peer.kick_reason is None:
                peer.kick_reason = f"flying ({climb:.0f} km/h climb)"
        if CAR_CAPTURE:
            self.log(f"{peer.addr[1]} CAPTURE speed {kmh:.1f} km/h over {dt:.2f} s, moved {d:.1f} units, car {peer.model_id}, pos ({pos[0]:.1f},{pos[1]:.1f},{pos[2]:.1f})")
        stock = TOP_SPEED_KMH.get(peer.model_id) if peer.model_id is not None else None
        soft = min(HARD_CAP_KMH, max(stock * SOFT_FACTOR, stock + SOFT_EXTRA_KMH)) if stock else None
        peer.hard_s = peer.hard_s + dt if kmh > HARD_CAP_KMH else 0.0
        peer.soft_s = peer.soft_s + dt if soft and kmh > soft else 0.0
        if peer.hard_s >= SPEED_SECONDS and "hard" not in peer.speed_flagged:
            peer.speed_flagged.add("hard")
            self.log(f"SPEED: owner {peer.owner_id} xuid {peer.xuid:#x} car {peer.model_id} drove {kmh:.0f} km/h for {peer.hard_s:.0f} s (hard cap {HARD_CAP_KMH:.0f})"
                     + ("" if SPEED_POLICY != "kick" else ": ending the connection"))
            if SPEED_POLICY == "kick" and peer.kick_reason is None:
                peer.kick_reason = f"speed {kmh:.0f} km/h"
        if peer.soft_s >= SPEED_SECONDS and "soft" not in peer.speed_flagged:
            peer.speed_flagged.add("soft")
            self.log(f"SPEED (log only): owner {peer.owner_id} xuid {peer.xuid:#x} car {peer.model_id} (stock {stock} km/h) drove {kmh:.0f} km/h for {peer.soft_s:.0f} s (soft limit {soft:.0f})")

    def check_parts(self, peer: Peer, size: int, payload: int):
        """The car's installed part ids (in the LobbyPlayer object, see parts.py) against the game data. An id the game does not have makes
        the other clients crash when they build that car (prod 2026-10-21/22: lobby wipes in the same second a car with a made-up engine
        row appeared). PART_POLICY fix (default) relays the object with those ids replaced by the car's stock parts, so the others build a
        car they have and the owner keeps playing with its own (its game never gets its own object back); log only reports, kick removes
        the account. Returns (something was wrong, withhold the object, the payload to relay)."""
        if parts_check is None or PART_POLICY == "off":
            return False, False, payload
        load_sync_catalog(self.log)
        peer.car_seen_at = time.monotonic()
        found = parts_check.find_parts(size, payload)
        if found is None or found[1][2] not in parts_check.P.TABLE_IDS["CarBody"]:
            alien = parts_check.find_unknown_car(size, payload)
            if alien is not None:
                peer.car_doubt_at = peer.car_seen_at
                return self.unknown_car(peer, payload, *alien)
        if found is not None and TEST_CAR and (test := parts_check.stock_list(TEST_CAR)) is not None:
            self.log(f"{peer.addr[1]} TEST CAR: owner {peer.owner_id}'s car {found[1][2] // 1000} relayed as car {TEST_CAR}")
            return False, False, parts_check.write_parts(payload, found[0], test)
        if found is None:
            peer.car_doubt_at = peer.car_seen_at
            if peer.part_notes < 3:
                peer.part_notes += 1
                self.log(f"{peer.addr[1]} part check: no part list found in the {size}-bit player object (owner {peer.owner_id})")
            return False, False, payload
        offset, words = found
        peer.parts = words
        # The car itself: slot 2 (body) holds the CarBody row of the car, id = car id x 1000 (the 354 rows of the catalog table). The car's
        # object (router 2) only says things like "100.0" (a float), so this is where the car id is actually readable. Same rules as before:
        # a traffic-only model (BLOCKED_CARS) or an id that is not in the game is withheld / its owner removed by the car policy.
        hide = False
        body = words[2]
        if body != parts_check.NONE and body in parts_check.P.TABLE_IDS["CarBody"]:
            hide = self.car_model_seen(peer, body // 1000)
        fixed, changes, odd = parts_check.neutralize(words)
        car = body // 1000 if body in parts_check.P.TABLE_IDS["CarBody"] else None
        name = lambda s: parts_check.P.SLOT_TABLE.get(s, "?")          # noqa: E731
        key = (tuple(words), PART_POLICY)
        if (changes or odd) and peer.part_logged != key and peer.part_notes < 6:   # once per car, and at most 6 cars per client
            peer.part_logged = key
            peer.part_notes += 1
            what = {"fix": "replaced with the stock part for the others", "kick": "ending the connection", "log": "log only"}.get(PART_POLICY, PART_POLICY)
            if changes:
                self.log(f"PART CHECK ({what}): owner {peer.owner_id} xuid {peer.xuid:#x} car {car}: "
                         + "; ".join(f"slot {s} {name(s)} {old:#x} ({old}, {why}) -> {new:#x}" for s, old, new, why in changes))
                self.note("parts", peer.owner_id, f"car {car}: " + ", ".join(f"slot {s} {old} -> {new}" for s, old, new, _ in changes))
            if odd:
                self.log(f"PART CHECK (log only, row of another owner): owner {peer.owner_id} xuid {peer.xuid:#x} car {car}: "
                         + "; ".join(f"slot {s} {w} ({text})" for s, w, text in odd[:8]))
        if changes or odd:
            peer.car_doubt_at = peer.car_seen_at
        if changes and PART_POLICY == "kick" and peer.kick_reason is None:
            peer.kick_reason = f"part id {changes[0][1]} in slot {changes[0][0]}"
        if changes and PART_POLICY == "fix":
            payload = parts_check.write_parts(payload, offset, fixed)
        payload = self.rental_choices(peer, car, size, payload)
        return bool(changes), hide, payload

    def note_choices(self, peer: Peer, size: int, payload: int) -> int:
        """The class and performance index a player's game sends with its car (Nerd::PlayerChoices vars 81/82; the other clients show
        them on the starting grid and the results). Remembered; and a rental that reports class R (7) gets its real class and PI back
        before the others see it: the grid showed "R 999" for every rental (prod 2026-10-22, D'Orcia Circuit)."""
        found = parts_check.find_choices(size, payload)
        if not found:
            return payload
        payload, bad = parts_check.sanitize_choices(payload, found)
        if bad and peer.choices_notes < 3:
            peer.choices_notes += 1
            self.log(f"{peer.addr[1]} CHOICES: owner {peer.owner_id} xuid {peer.xuid:#x} sent values the other clients cannot show: "
                     + "; ".join(bad) + " (replaced)")
        cls, pi = parts_check.read_choices(payload, found)
        if cls is None and peer.choices:
            cls = peer.choices[0]
        peer.choices = (cls, pi)
        want = CAR_CHOICES.get(peer.rental_car) if peer.rental_car is not None else None
        if want and cls == RENTAL_CLASS_ID and (cls, round(pi, 3)) != (want[0], round(want[1], 3)):
            self.log(f"{peer.addr[1]} RENTAL: owner {peer.owner_id} car {peer.rental_car} reports class {cls} PI {pi:.3f}, shown to the others as "
                     f"class {want[0]} PI {want[1]:.3f}")
            payload = parts_check.write_choices(payload, found, want[0], want[1])
            peer.choices = want
        return payload

    def short_rental_choices(self, peer: Peer, size: int, payload: int) -> int:
        """Right after a rental's part list its game sends a short [81 class][82 PI] update with class R and PI 1.0: the others' standings
        and grid showed "R 999" (dev 2026-10-25, Road Trip, an S2 Aventador). For a rental the catalog's class and PI go out instead."""
        found = parts_check.find_short_choices(size, payload)
        if not found:
            return payload
        cls, pi = parts_check.read_short_choices(payload, found)
        want = CAR_CHOICES.get(peer.rental_car) if peer.rental_car is not None else None
        if want and (cls == RENTAL_CLASS_ID or (cls is None and pi is not None and pi >= 0.999)):
            self.log(f"{peer.addr[1]} RENTAL: owner {peer.owner_id} car {peer.rental_car} sent class {cls} PI {pi}, shown as class {want[0]} "
                     f"PI {want[1]:.3f}")
            payload = parts_check.write_short_choices(payload, found, want[0], want[1])
            cls, pi = want
        prev = peer.choices or (None, 0.0)
        peer.choices = (cls if cls is not None else prev[0], pi if pi is not None else prev[1])
        return payload

    def rental_choices(self, peer: Peer, car, size: int, payload: int) -> int:
        """The player's car is known (part list): a rental shows its real class and PI to the others. Same object carries the PlayerChoices
        (join): fixed in place; a car change sends them just before the parts, so a corrected update follows (applied in order)."""
        peer.rental_car = car if car in RENTAL_CARS and car in CAR_CHOICES else None
        if peer.rental_car is None:
            return payload
        want = CAR_CHOICES[car]
        found = parts_check.find_choices(size, payload)
        if found:
            cls, pi = parts_check.read_choices(payload, found)
            if (cls if cls is not None else want[0]) != want[0] or abs(pi - want[1]) > 0.002:
                self.log(f"{peer.addr[1]} RENTAL: owner {peer.owner_id} car {car} sent class {cls} PI {pi:.3f}, shown as class {want[0]} PI {want[1]:.3f}")
                payload = parts_check.write_choices(payload, found, want[0], want[1])
            peer.choices = want
            return payload
        sent = peer.choices
        if sent is None or sent[0] != want[0] or abs(sent[1] - want[1]) > 0.002:
            self.log(f"{peer.addr[1]} RENTAL: owner {peer.owner_id} car {car}: its game sent {sent}, the others are told class {want[0]} PI {want[1]:.3f}")
            bits, value = parts_check.choices_update(*want)
            self.remember_and_relay(peer, MSG_OBJECT_UPDATE, 1, 0, None, 0, bits, value)
            peer.choices = want
        return payload

    def unknown_car(self, peer: Peer, payload: int, offset: int, words: list):
        """A car the game data does not have (a car pack the server does not ship): the other clients would crash building it. With
        UNKNOWN_CAR_POLICY standin they get the stand-in car's stock part list instead (the owner keeps driving its own car); kick removes
        the account; log only reports. Returns check_parts' (something was wrong, withhold the object, the payload to relay)."""
        car = words[2] // 1000
        peer.model_id = car
        what = {"standin": f"the others see car {STANDIN_CAR}", "kick": "ending the connection"}.get(UNKNOWN_CAR_POLICY, "log only")
        if peer.model_logged != car:
            peer.model_logged = car
            self.log(f"CAR POLICY ({what}): owner {peer.owner_id} xuid {peer.xuid:#x} uses car {car} (body {words[2]}), which is not in the game data")
            self.note("car", peer.owner_id, f"unknown model {car}")
        if UNKNOWN_CAR_POLICY == "kick":
            if peer.kick_reason is None:
                peer.kick_reason = f"car {car} is not in the game"
            return True, True, payload
        if UNKNOWN_CAR_POLICY == "standin":
            stand = parts_check.stock_list(STANDIN_CAR)
            if stand is None:
                return True, True, payload                 # no stand-in in the catalog: withhold the car rather than crash the others
            return True, False, parts_check.write_parts(payload, offset, stand)
        return UNKNOWN_CAR_POLICY != "off", False, payload

    def kick_for_cars(self) -> None:
        for q in [q for q in self.peers.values() if q.kick_reason]:
            reason, q.kick_reason = q.kick_reason, None
            if self.hub is not None and q.xuid:
                self.hub.flag(q.xuid, reason)   # matchmaking keeps a kicked player among other kicked players for a day
                self.hub.revoke(q.xuid)         # ends every connection of the account and keeps it refused until its next sign-in
            else:
                try:
                    self.send(q, [close_connection(0)], fast=True)
                except OSError:
                    pass
                self.drop_peer(q, f"car policy: {reason}")

    def note(self, kind: str, owner: int, detail: str) -> None:
        self.recent.append((time.monotonic(), kind, owner, detail))

    def report_wipe(self, now: float) -> None:
        """Several clients vanished together (issue: players dropped when someone joins). Log what we know about them and what the lobby
        saw in the seconds before they went silent. Logging only: nothing is changed."""
        self.wipe_at, self.wipe_last = 0.0, now
        gone = [d for d in self.drops if now - d[0] <= WIPE_WINDOW_S + 2.0]
        if len(gone) < WIPE_MIN_DROPS:
            return
        t0 = min(d[3] for d in gone)                    # when the first of them was last heard
        self.log(f"WIPE: {len(gone)} client(s) removed together, {len(self.peers)} left in this lobby; they went silent about {now - t0:.0f} s ago")
        for (_, owner, xuid, last_rx, pos, car, why) in gone:
            p = 'unknown' if pos is None else f'({pos[0]:.0f},{pos[1]:.0f},{pos[2]:.0f})'
            self.log(f"WIPE:   owner {owner} xuid {xuid:#x} last heard {t0 - last_rx:+.1f} s, last car position {p}, car object {car}, {why}")
        shown = objs = 0
        for (t, kind, owner, detail) in list(self.recent):
            if t0 - WIPE_CONTEXT_S <= t <= t0 + 3.0:
                if kind == "obj":
                    if objs >= 12:
                        continue
                    objs += 1
                elif shown >= 90:
                    continue
                else:
                    shown += 1
                self.log(f"WIPE:   {t - t0:+6.1f} s {kind:5s} owner {owner:3d} {detail}")

    def log(self, msg: str) -> None:
        line = f"{time.strftime('%H:%M:%S')} {self.tag}{msg}"
        print(line, flush=True)
        if self.log_file:
            self.log_file.write(line + "\n")

    def send(self, peer: Peer, messages, fast: bool = False) -> None:
        """Queues messages (writers or baked) for one client; flush() sends them. An empty list still sends a datagram (an ack).
        The client measures RTT in whole milliseconds and discards 0 ms as invalid, which makes it fall back to a 100 ms retransmit
        timer, so a reply is held until 2 ms after that client's last packet. That used to be a sleep, which stalled every world
        on the port; now the reply waits in the queue. Relayed messages (fast=True) may wait up to RELAY_FLUSH_SECONDS for others."""
        now = time.monotonic()
        peer.outbox.extend(m if type(m) is tuple else bake(m) for m in messages)
        if fast:
            peer.due = min(peer.due, now + RELAY_FLUSH_SECONDS)
        else:
            peer.due = min(peer.due, now)
            peer.hold = max(peer.hold, peer.last_rx + REPLY_HOLD_SECONDS)
        if self.peers.get(peer.addr) is peer:
            self.dirty.add(peer)
        else:
            self.flush_peer(peer)       # a stranger or a removed client (CloseConnection): nothing will flush it later

    def relay(self, sender: Peer, msg) -> None:
        """send(fast=True) of one message to everybody else in the sender's world: the hot path (every car update)."""
        baked = bake(msg)
        due = time.monotonic() + RELAY_FLUSH_SECONDS
        for q in self.others(sender):
            q.outbox.append(baked)
            if due < q.due:
                q.due = due
            self.dirty.add(q)

    def flush_peer(self, peer: Peer) -> None:
        out, peer.outbox = peer.outbox, []
        peer.due, peer.hold = math.inf, 0.0
        self.dirty.discard(peer)
        ack, bits = peer.ack_fields()
        limit = MAX_PACKET_BYTES * 8 - HEADER_BITS
        i = 0
        while True:
            j, size = i, 0
            while j < len(out) and j - i < 255 and (j == i or size + out[j][1] <= limit):
                size += out[j][1]
                j += 1
            try:
                self.sock.sendto(pack_messages(out[i:j], UNRELIABLE_ID, ack, bits), peer.addr)
            except OSError as exc:
                self.noise(f"{peer.addr[1]} send failed: {exc!r}")
            i = j
            if i >= len(out):
                return

    def flush(self, now: float) -> float:
        """Sends every queue that is due; returns when the next one will be."""
        nxt = math.inf
        for peer in list(self.dirty):
            at = max(peer.due, peer.hold)
            if at <= now:
                self.flush_peer(peer)
            else:
                nxt = min(nxt, at)
        return nxt

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
        hist.append((mtype, tid, seq, size, payload))
        if router == 1 and size >= 200:
            # what the other clients were just given (joins, car changes): the WIPE report prints it, so a crash can be matched to the bytes
            self.note("obj", peer.owner_id, f"{'create' if mtype == MSG_OBJECT_CREATE else 'update'} router 1 {size} bits hex="
                      + payload.to_bytes((size + 7) // 8, "little").hex())
        if len(hist) >= peer.compact_at.get((router, oid), 256):
            hist[:] = replay.compact(router, hist, MSG_OBJECT_UPDATE)          # a replay is renumbered, so the stored numbers do not matter
            peer.compact_at[(router, oid)] = max(256, 2 * len(hist))
            if len(hist) > RELAY_HISTORY_CAP:
                del hist[1:len(hist) - RELAY_HISTORY_CAP + 1]                  # keep the create; the oldest updates go
        self.relay_numbered(peer, (router, oid), n,
                            lambda sq: relay_object_message(mtype, router, oid, peer.owner_id, tid, sq, id_bits, type_bits, size, payload))

    def relay_numbered(self, sender: Peer, key, n: int, build) -> None:
        """relay() for a message numbered by the server (its n-th of that object): `build(seq)` writes it. A client that joined late was
        replayed a shorter history and is numbered from where it stands (Peer.seq_shift)."""
        baked, alt = bake(build(n % 0xFF)), {}
        due = time.monotonic() + RELAY_FLUSH_SECONDS
        skey = (sender.owner_id,) + tuple(key)
        for q in self.others(sender):
            b = baked
            shift = q.seq_shift.get(skey) if q.seq_shift else 0
            if shift:
                sq = replay.wire_seq(n, shift)
                b = alt.get(sq)
                if b is None:
                    b = alt[sq] = bake(build(sq))
            q.outbox.append(b)
            if due < q.due:
                q.due = due
            self.dirty.add(q)

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
        # A client that led an earlier event holds the GameOptions it wrote itself, numbered by its own writes, and applies updates
        # strictly in sequence order. The forward to it used to restart at 0, which looks stale: the Infection leader kept its own
        # Infection options when the other player then started King (dev 2026-10-20: one client showed Infection, the other King,
        # and the server ran the mode of one of them). Number the forward after its last own write.
        if q.gopts_own_seq is not None and (q.gopts_seq - q.gopts_own_seq - 1) % 0xFF >= 0x80:
            q.gopts_seq = (q.gopts_own_seq + 1) % 0xFF
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
            batch, stored = [], 0
            for (router, oid), hist in q.objs.items():
                id_bits, type_bits = ROUTER_BITS[router]
                entries = replay.renumber(replay.compact(router, hist, MSG_OBJECT_UPDATE))
                stored += len(hist)
                for (mtype, tid, seq, size, payload) in entries:
                    batch.append(relay_object_message(mtype, router, oid, q.owner_id, tid, seq,
                                                      id_bits, type_bits, size, payload))
                # the joiner now expects message number len(entries); the server's next one is relay_next
                shift = q.relay_next.get((router, oid), len(hist)) - len(entries)
                if shift > 0:
                    newcomer.seq_shift[(q.owner_id, router, oid)] = shift
                else:
                    newcomer.seq_shift.pop((q.owner_id, router, oid), None)
            for i in range(0, len(batch), 4):
                self.send(newcomer, batch[i:i + 4])
            if batch:
                self.log(f"{newcomer.addr[1]} <- replayed {len(batch)} object messages "
                         f"of owner {q.owner_id} ({q.addr[1]})" + (f", {stored} stored" if stored != len(batch) else ""))

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

    def assign_private_universe(self, peer: Peer) -> None:
        """LobbyPlayer.PrivateUniverseId: an OnlinePrivateN no other player in this world has (to the owner and relayed to the others)."""
        used = {getattr(q, "private_universe", None) for q in self.peers.values() if q is not peer and q.connected}
        free = [u for u in range(PRIVATE_UNIVERSE_FIRST, PRIVATE_UNIVERSE_FIRST + PRIVATE_UNIVERSE_COUNT) if u not in used]
        if not free:
            return
        peer.private_universe = free[0]

        def vars_(w):
            w.bits(PLAYER_VAR_PRIVATE_UNIVERSE, 8)
            w.bits(peer.private_universe, PRIVATE_UNIVERSE_BITS)
        self.send(peer, [server_update(1, 0, ROUTER_BITS[1][0], peer.player_seq, vars_)])
        peer.player_seq = (peer.player_seq + 1) % 0xFF
        vb = BitWriter()
        vars_(vb)
        self.remember_and_relay(peer, MSG_OBJECT_UPDATE, 1, 0, None, 0, vb.pos, vb.value)
        self.log(f"{peer.addr[1]} LobbyPlayer.PrivateUniverseId = {peer.private_universe} (OnlinePrivate{peer.private_universe - 1})")

    def set_player_team(self, peer: Peer, team: int) -> None:
        """PlayerChoices.TeamId of a player: to the owner (server update of its player object) and relayed to the others."""
        peer.team_id = team & 1

        def vars_(w):
            w.bits(PLAYER_VAR_TEAM_ID, 8)
            w.bits(team & 1, 1)
        self.send(peer, [server_update(1, 0, ROUTER_BITS[1][0], peer.player_seq, vars_)])
        peer.player_seq = (peer.player_seq + 1) % 0xFF
        vb = BitWriter()
        vars_(vb)
        self.remember_and_relay(peer, MSG_OBJECT_UPDATE, 1, 0, None, 0, vb.pos, vb.value)

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
                                 (base_event_id(race),)).fetchone()
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
                row = db.execute("SELECT RaceModeId FROM AnthemOnlineEvents WHERE AnthemOnlineEventId = ?",
                                 (base_event_id(race),)).fetchone()
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
        self.tag_account(tag, time.monotonic())
        tag["buckets"] = {p: TAG_BUCKET_NOT_IT for p in people}
        firsts = random.sample(people, min(n_it, len(people)))
        tag["first"] = [p.xuid for p in firsts]
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
            self.tag_account(tag, now)
            tag["buckets"][p] = TAG_BUCKET_IT
            self.log(f"tag: no It car left, owner {p.owner_id} is It now")
            self.tag_send("new It")
        elif tag["repeats"] and now >= tag["repeats"][0]:
            tag["repeats"].pop(0)
            self.tag_send("repeat")
        if tag["mode"]["name"] == "Infection" and people and len(its) == len(people) and len(people) > 1:
            self.end_race("everybody is infected")
        elif tag.get("end_at") is not None and now >= tag["end_at"]:        # end_at is set at "GO"; the It car is drawn earlier, on the grid
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
        self.tag_account(tag, time.monotonic())
        buckets[a], buckets[b] = new_a, new_b
        it_was, other = (a, b) if old_a == TAG_BUCKET_IT else (b, a)
        scorer = other if tag["mode"]["name"] == "King" else it_was        # King: the crown is taken; Infection: the It car infects
        if scorer.xuid:
            tag.setdefault("hits", {})[scorer.xuid] = tag.get("hits", {}).get(scorer.xuid, 0) + 1
        self.log(f"tag: owner {a.owner_id} {old_a}->{new_a}, owner {b.owner_id} {old_b}->{new_b} (reported by {peer.owner_id})")
        self.tag_send("collision")
        tag["repeats"] = [time.monotonic() + x for x in TAG_ASSIGN_REPEATS[1:3]]

    def tag_account(self, tag, now: float) -> None:
        """Adds the time since the last bucket change to every It car's crown time (only from GO on). Call before buckets change."""
        start, last = tag.get("go_at"), tag.get("marked")
        if start is not None and last is not None and now > max(start, last):
            held = tag.setdefault("held", {})
            for p, bucket in tag["buckets"].items():
                if bucket == TAG_BUCKET_IT and p.xuid:
                    held[p.xuid] = held.get(p.xuid, 0.0) + now - max(start, last)
        tag["marked"] = now

    def report_event(self, why: str) -> None:
        """The result of an online event (two or more players) for the website: finish order (first finisher wins a race; in a team event
        that also names the winning team), the cars and rentals raced, and for King / Infection the crowns taken or infections made, crown
        times (the King winner is the longest holder) and who was never infected. Posted to the LSP off the tick (RESULT_SINK)."""
        ev = self.event
        people = [q for q in ev["people"] if q.xuid]
        if len(people) < 2:
            return
        now = time.monotonic()
        fin, out = ev.get("finished", {}), ev.get("quit", set())
        order = sorted((q for q in people if q in fin and q not in out), key=lambda q: fin[q])
        team = ev.get("team", {})
        players = [{"xuid": q.xuid, "car": q.model_id, "rental": q.rental_car is not None, "team": team.get(q),
                    "finish": order.index(q) + 1 if q in order else None} for q in people]
        detail = {"players": players, "end": why, "roadtrip": bool(self.roadtrip),
                  "seconds": round(now - ev["racing_since"], 1) if ev.get("racing_since") else None}
        tag = ev.get("tag")
        if tag:
            self.tag_account(tag, now)
            mode = "infection" if tag["mode"]["name"] == "Infection" else "king"
            held = tag.get("held", {})
            detail.update(hits=[[x, n] for x, n in tag.get("hits", {}).items()], first=tag.get("first", []),
                          held=[[x, round(t, 1)] for x, t in held.items()], everyone=why == "everybody is infected",
                          survivors=[q.xuid for q in people if tag["buckets"].get(q) != TAG_BUCKET_IT] if mode == "infection" else [],
                          winner=max(held, key=held.get) if mode == "king" and held else 0)
        else:
            mode = "team" if team else "race"
            detail["winner"] = order[0].xuid if order else 0
        threading.Thread(target=RESULT_SINK, args=(mode, detail, self.log), name="event-result", daemon=True).start()

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

    def request_race_blocked(self, peer: Peer) -> bool:
        """One event at a time per Free Roam lobby (prod 2026-10-21/22: 61 of 285 multi-player events were started while the lobby's
        previous event was still loading or racing, and 50 % of those never began against 7 % of the others; the new request replaced
        the old event and invited its participants again). A second request during somebody else's invite window is ignored as well.
        The client greys the event tile out while SessionHost.ParticipantsSelected / InRace is set, so a real client sends this only
        in the gaps [inferred]."""
        if self.roadtrip:
            return False
        ev = self.event
        if ev and not ev.get("over") and any(q.connected for q in ev["people"]) \
                and time.monotonic() - ev.get("started", time.monotonic()) < EVENT_BLOCK_MAX_SECONDS:
            return True       # (an event whose participants all vanished, or that never ends, must not lock the lobby)
        pr = self.pending_race
        return bool(pr and pr[1] is not peer and pr[1].connected)

    def close_finished_event(self) -> set:
        """A new request while the previous event only sits on its results screen (InRace=0, participants still leaving): end the old
        event first so the new one starts clean. Returns the peers that are still on its results screen."""
        ev = self.event
        if not ev or self.roadtrip:
            return set()
        busy = {q for q in ev["people"] if q.connected and q.console_vars.get(3)}
        self.end_event("a new race was requested")
        return busy

    def participant_left(self, peer: Peer) -> None:
        """A participant's game reported RaceInProgress=0: it left the event (quit from the pause menu, left a rental, was sent back to
        free roam). Out of the event, and, when it had not finished, its Console.ParticipatingInRace goes to 0 for everybody: the server
        had set it to 1 at the start and never cleared it, so the others' games kept treating the car standing on the track as a racer
        (prod 2026-10-22 22:18, D'Orcia Circuit: a player's Lancer stood at the finish line through the whole race). A finished player who
        only leaves the results screen keeps its flag (it is still part of the result)."""
        ev = self.event
        left_early = peer not in ev.get("finished", {})
        ev["people"].remove(peer)
        if left_early and peer.connected:
            self.set_console_bool(peer, CONSOLE_VAR_PARTICIPATING_IN_RACE, 0, "left the event")
        if not ev["people"]:
            self.end_event("every participant left the event")

    def drop_unloaded(self, now: float) -> None:
        """Participants that never reported HasLoadedCarsForAllParticipants (a client that hung or crashed while building the grid, but
        still answers the keep-alive) held everybody else on the loading screen for good; only 20 s of silence ever removed them."""
        ev = self.event
        stuck = [q for q in ev["people"] if not (q.connected and q.console_vars.get(CONSOLE_VAR_HAS_LOADED_CARS))]
        if not stuck:
            ev["load_checked"] = True
            return
        if len(stuck) < len(ev["people"]) or len(stuck) == 1:
            # somebody finished: the others hung on their own
            ev["load_checked"] = True
            drop, why = stuck, "did not load in time"
            self.log(f"event load timeout: dropping {[q.owner_id for q in stuck]} from the event, "
                     f"{len(ev['people']) - len(stuck)} participant(s) remain")
        else:
            # Nobody finished. A game reports HasLoadedCars only once the cars of all the OTHER participants are loaded
            # (NetworkWaitForCarsLoaded, FUN_1408e0990, counts remote participants only), so one car nobody can load holds every game,
            # and dropping them all kicked a whole group for one player (prod 2026-10-22 lobby e7255c39, 4 then 2 players). Take out the
            # likeliest one; the others' wait ends as soon as it is no longer a participant. Again after the next step if still nobody.
            leader = ev["people"][0]
            suspect = max(stuck, key=lambda q: (not q.connected, q is not leader, q.car_doubt_at, q.car_seen_at, q.connected_at))
            drop, why = [suspect], "the others could not load its car"
            ev["load_next"] = now + LOAD_SUSPECT_STEP_SECONDS
            self.log(f"event load timeout: nobody loaded; dropping owner {suspect.owner_id} as the likeliest blocker (car checked "
                     f"{now - suspect.car_seen_at:.0f} s ago{', failed a check' if suspect.car_doubt_at else ''}), "
                     f"{len(ev['people']) - 1} participant(s) remain")
        for q in drop:
            ev["people"].remove(q)
            if q.connected:
                self.set_console_bool(q, CONSOLE_VAR_PARTICIPATING_IN_RACE, 0, why)
        if not any(q.connected for q in ev["people"]):
            self.end_event("nobody loaded the event in time")
        else:
            self.maybe_begin_race()
            self.maybe_go_race()

    def start_event(self, leader: Peer) -> None:
        """Participants = the leader plus every client that reported Console.Ready."""
        people = [leader] + [q for q in self.peers.values()
                             if q is not leader and q.connected and q.console_vars.get(4)]
        self.log(f"event start: {len(people)} participant(s): {[q.owner_id for q in people]}")
        for q in self.peers.values():
            if q is not leader:
                self.log(f"  other client owner {q.owner_id}: connected={q.connected} Console vars={q.console_vars}")
        self.event = {"people": people, "begun": False, "start_at": None, "started": time.monotonic(),
                      "numlaps": self.rt_numlaps(self.rt_race()) if self.roadtrip and self.rt_winner is not None
                      and self.rt_gamedb else None}
        mode = self.rt_race_mode(self.rt_race()) if self.roadtrip and self.rt_winner is not None and self.rt_gamedb else None
        if self.roadtrip and self.rt_winner is not None and self.rt_candidates:
            race = self.rt_race()
            self.rt_last_race = race                    # where the next drive starts: the finish line of this event
            self.rt_last_loc = self.rt_candidates[0][self.rt_winner]
            if is_team_event(race):
                self.assign_teams(people, f"{race} (event {base_event_id(race)} as a team race)")
        guessed = False
        if mode is None and not self.roadtrip and self.game_options:      # free roam: read the mode off the leader's GameOptions
            size, payload = self.game_options
            mode = game_options_tag_mode(self.game_options)
            guessed = mode == RACE_MODE_KING                 # King is assumed from the header until the first hit; Infection has its own header
            self.log(f"event options: {size} bits, header {payload & 0xFFFFFFFFFF:010x}: "
                     + ("not a tag mode" if mode is None else "Infection (header ..600c2000)" if mode == RACE_MODE_INFECTION
                        else "tag mode (King assumed until the first hit shows otherwise)"))
        if not self.roadtrip and mode not in TAG_MODES and gameopts.teams_on(self.game_options):
            # Free Roam: the leader chose Teams: On in the event options (ScoringOptions.NumTeams = 2). No client sets PlayerChoices.TeamId, the
            # server did in retail: without this everybody stayed on the red team (prod 2026-10-22, reported).
            self.assign_teams(people, "free roam, Teams: On")
        if "team" not in self.event:
            for q in people:                      # an event without teams: nobody keeps the team of an earlier one
                if getattr(q, "team_id", 0):
                    self.set_player_team(q, 0)
        if mode in TAG_MODES:
            self.event["tag"] = {"mode": TAG_MODES[mode], "buckets": {}, "assigned": False, "assign_at": None, "repeats": [],
                                 "guessed": guessed}
            self.event["numlaps"] = None
            self.log(f"tag: event {self.rt_race() if self.roadtrip else '(free roam)'} is {TAG_MODES[mode]['name']}")
        for q in people:
            # Readiness reported for the PREVIOUS event is still stored (a client only resets HasLoadedCarsForAllParticipants / HasGrid /
            # RenderReady when it starts loading again). Without this the grid started as soon as the first client had loaded, before a
            # leader that was still loading (dev 2026-10-20: second event in a row, the leader sat on "Setting Up Event").
            for stale in (CONSOLE_VAR_HAS_LOADED_CARS, CONSOLE_VAR_HAS_GRID, CONSOLE_VAR_RENDER_READY):
                q.console_vars.pop(stale, None)
            self.set_console_bool(q, CONSOLE_VAR_PARTICIPATING_IN_RACE, 1, "participant selected")
        self.set_session_host_bool(SH_RACE_PARTICIPANTS_SELECTED, 1, "participants selected")

    def assign_teams(self, people, what: str) -> None:
        """Two teams, red (0) and blue (1), alternating over a shuffled order; the starting grid keeps that order (odd slots red)."""
        order = list(people)
        random.shuffle(order)
        self.event["team"] = {q: i % 2 for i, q in enumerate(order)}
        self.event["order"] = order
        for q in order:
            self.set_player_team(q, self.event["team"][q])
        self.log(f"team event: {what}, {len(people)} player(s): "
                 + " ".join(f"owner{q.owner_id}={'blue' if t else 'red'}" for q, t in self.event["team"].items()))

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
        order = [q for q in ev.get("order", ()) if q in ev["people"]]
        if len(order) != len(ev["people"]):
            order = list(ev["people"])
            random.shuffle(order)               # grid slots are drawn, not by join order; a team event keeps its red/blue alternation
        for q in ev["people"]:
            entries = [("local", 0, 0) if p is q else ("relay", 0, p.owner_id) for p in order]
            self.send(q, [starting_grid(entries)])
        for q in ev["people"]:
            q.console_vars.pop(0, None)     # RenderReady is re-reported by each client once its grid is up
        if ev.get("tag"):
            # The starting-grid screen already says who is infected / who holds the crown, so the first It car is drawn while the grid
            # is up (dev 2026-10-20: it was drawn at "GO" and everybody read "You're a survivor" on the grid).
            ev["tag"]["assign_at"] = time.monotonic() + TAG_GRID_ASSIGN_DELAY
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
        if ev and ev.get("racing") and peer in ev["people"]:
            done = ev.setdefault("laps", {})
            done[peer] = max(done.get(peer, 0), lap)
        if not ev or not ev.get("racing") or peer not in ev["people"] or peer in ev["finished"]:
            return
        lap_t, _, total_t =(struct.unpack("<d", x.to_bytes(8, "little"))[0] for x in blobs)
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
        if not ev or not ev.get("racing") or ev.get("numlaps") or peer not in ev["people"] or peer in ev["finished"]:
            return
        if ev.get("tag") and time.monotonic() - ev.get("racing_since", time.monotonic()) < TAG_KEYFRAME_MIN_SECONDS:
            return        # King / Infection: the clients end the event on their own time limit and send their final keyframes then
        ev["finished"][peer] = time.monotonic()
        if not ev.get("tag") and ev.get("laps", {}).get(peer, 0) < 1:
            # A final score from a player who never completed a lap: out of the race (sat at the finish line in a car the event does not
            # allow, quit, crashed out), not a finisher. Prod 2026-10-22 22:18:19 lobby c0759261: such a keyframe 15 s after GO started
            # the "30 s after the first finisher" limit and ended a 3-lap race at 45 s; nobody else's finish counted.
            ev.setdefault("quit", set()).add(peer)
            self.log(f"{peer.addr[1]} final score keyframe without a completed lap: out of the race, not a finisher "
                     f"({len(ev['finished'])}/{len(ev['people'])} done)")
            return
        self.log(f"{peer.addr[1]} final score keyframe: done ({len(ev['finished'])}/{len(ev['people'])} finished)")

    def check_race_end(self, now: float) -> None:
        ev = self.event
        if ev and ev.get("clear_at") and now >= ev["clear_at"]:
            ev["clear_at"] = None      # the "event is about to start" HUD follows RaceReady/ParticipantsSelected
            for var in (SH_RACE_PARTICIPANTS_SELECTED, SH_RACE_READY):
                self.set_session_host_bool(var, 0, "race over: lobby flags cleared")
        if ev and ev.get("tag") and not ev.get("racing") and not ev.get("over") and ev["tag"].get("assign_at") is not None:
            self.tag_tick(now)          # the first It car is drawn while the starting grid is up, before GO
            return
        if not ev or not ev.get("racing") or ev.get("over"):
            return
        if ev.get("tag"):
            # The clients end King / Infection on their own time limit (60 s in the arena, not the 240 s of CareerRaceModes) and then wait on
            # the results screen for InRace=0 (dev 2026-10-20 21:21: both sat on "KING / Aerodrome Arena Small" for good). Their final
            # score keyframes (one burst per client) say the event is over; the server's own limit stays as a fallback.
            fin = ev["finished"]
            people = [q for q in ev["people"] if q.connected]
            if fin and people and all(q in fin for q in people):
                self.end_race("everybody finished")
                return
            self.tag_tick(now)
            return
        fin = ev["finished"]
        if not fin:
            return
        real = [t for q, t in fin.items() if q not in ev.get("quit", ())]
        if all(q in fin for q in ev["people"] if q.connected):
            self.end_race("everybody finished")
        elif real and now >= min(real) + RACE_LAST_FINISHER_SECONDS:
            self.end_race("time limit after the first finisher")

    def end_race(self, why: str) -> None:
        """SessionHost.InRace=0 is what the clients' NetworkWaitForRaceEnd waits for (FUN_1408eeb20)."""
        try:
            self.report_event(why)
        except Exception as exc:  # noqa: BLE001 - statistics must never keep an event from ending
            self.log(f"event result failed: {exc!r}")
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
        ev["racing_since"] = time.monotonic()
        self.set_session_host_bool(SH_IN_RACE, 1, "race begun")
        if ev.get("tag"):
            go_at = time.monotonic() + RACE_COUNTDOWN_MS / 1000.0
            if not ev["tag"].get("assigned") and ev["tag"].get("assign_at") is None:
                ev["tag"]["assign_at"] = go_at            # no grid assignment was scheduled: the crown / first infection at "GO"
            ev["tag"]["end_at"] = go_at + ev["tag"]["mode"]["seconds"]
            ev["tag"]["go_at"] = go_at
            ev["tag"]["repeats"] = [time.monotonic() + x for x in (0.5, 2.0)]     # the buckets again at GO
        go = (self.now_ms() + RACE_COUNTDOWN_MS) & 0xFFFFFFFF
        self.log(f"MsgStartRace start time {go} (server clock now {self.now_ms()})")
        for q in ev["people"]:
            self.send(q, [start_race(go)])

    def drop_peer(self, peer: Peer, why: str) -> None:
        self.peers.pop(peer.addr, None)
        if peer in self.dirty:
            self.flush_peer(peer)   # what was queued for it, e.g. the CloseConnection of a kick
        peer.connected = False      # other references (pending_race, event people) must not see a removed peer as present
        self.traffic.leave(peer)
        self.rt_votes.pop(peer, None)
        if self.roadtrip and not any(c.connected for c in self.peers.values()):
            self.rt_state, self.rt_end_ms, self.rt_winner = RT_INITIALISING, 0, None   # the next group starts fresh
            self.rt_last_race = self.rt_last_loc = None
            # and nothing of the old trip may fire later: a drive pending between two events called rt_start_drive with no winner
            # (prod "tick error: TypeError('list indices must be integers or slices, not NoneType')", 1 s after the last CloseConnection)
            self.rt_pending_drive = self.rt_drive_wait = self.rt_configured = False
            self.rt_idx = 0
        self.log(f"{peer.addr[1]} removed ({why}); owner {peer.owner_id} had {len(peer.objs)} objects")
        if peer.cars_relayed:
            self.log(f"{peer.addr[1]} session summary: xuid {peer.xuid:#x} car {peer.model_id} (stock {TOP_SPEED_KMH.get(peer.model_id)} km/h), "
                     f"top measured speed {peer.top_kmh:.0f} km/h, farthest position {peer.max_abs:.0f}, highest point {peer.max_y:.0f}, fastest climb {peer.top_climb:.0f} km/h")
        now_m = time.monotonic()
        self.drops.append((now_m, peer.owner_id, peer.xuid, peer.last_rx, peer.pos, peer.car_id, why))
        if (not self.wipe_at and now_m - self.wipe_last > 30.0
                and sum(1 for d in self.drops if now_m - d[0] <= WIPE_WINDOW_S) >= WIPE_MIN_DROPS):
            self.wipe_at = now_m + 1.0          # the rest of the group is removed in the same sweep: report once they are all in
        if self.pending_race and self.pending_race[1] is peer:
            # the leader left during the invite window (prod 2026-10-21 19:04, lobby 84dcc3a1): the event used to start around it anyway
            # and everybody else waited on "loading" for a client that no longer existed
            self.end_event("the event leader left before it started")
        ev = self.event
        if ev and peer in ev["people"] and not ev.get("racing") and not ev.get("over"):
            # a participant vanished while the event was being set up (King/Infection stuck on "setting up event" for the others, 2026-10-20):
            # the start waits for every listed participant to be connected and loaded, so drop it from the list instead of waiting forever
            ev["people"].remove(peer)
            self.log(f"event setup: participant {peer.owner_id} is gone, {len(ev['people'])} participant(s) remain")
            if not any(q.connected for q in ev["people"]):
                self.end_event("every participant left during setup")
            else:
                self.maybe_begin_race()
                self.maybe_go_race()
        for (router, oid) in peer.objs:
            id_bits, _ = ROUTER_BITS[router]
            n = peer.relay_next.get((router, oid), 0)
            self.relay_numbered(peer, (router, oid), n, lambda sq, router=router, oid=oid, id_bits=id_bits:
                                relay_delete_message(router, oid, peer.owner_id, id_bits, sq))

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
                    self.note("join", peer.owner_id, f"xuid {peer.xuid:#x} lobby type {peer.lobby_type}")
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
                    self.assign_private_universe(peer)

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
            elif mtype == MSG_REQUEST_RACE and self.request_race_blocked(peer):
                self.log(f"{addr[1]} MsgRequestRace (owner {peer.owner_id}) ignored: an event is already being set up or running in this lobby")
            elif mtype == MSG_REQUEST_RACE:
                busy = self.close_finished_event()      # players still on the previous event's results screen cannot take an invite
                self.log(f"{addr[1]} MsgRequestRace (owner {peer.owner_id}) -> RaceReady=1")
                self.set_session_host_bool(SH_RACE_READY, 1, "race requested")
                self.pending_race = (time.monotonic() + RACE_INVITE_SECONDS, peer)
                self.race_requested_at = time.monotonic()
                self.race_hard_deadline = time.monotonic() + RACE_JOIN_GRACE_SECONDS
                invited = [q for q in self.others(peer) if q.connected and q.entered and q not in busy]
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
                if mtype == MSG_OBJECT_CREATE or size >= 500:
                    self.note("obj", peer.owner_id, f"{'create' if mtype == 7 else 'update'} router={router} id={oid} bits={size}")
                suppress = False
                if CAR_CAPTURE and router == 1 and size:        # the LobbyPlayer object: gamertag, car, setup, livery ids (dev only)
                    self.log(f"{addr[1]} CAPTURE player {'create' if mtype == 7 else 'update'} id={oid} seq={seq} bits={size} "
                             f"xuid={peer.xuid:#x} hex={payload.to_bytes((size + 7) // 8, 'little').hex()}")
                if router == 1 and parts_check is not None and size >= 240:
                    payload = self.note_choices(peer, size, payload)
                elif router == 1 and parts_check is not None and size in (40, 80):
                    payload = self.short_rental_choices(peer, size, payload)
                if router == 1 and parts_check is not None and size >= parts_check.MIN_BITS:   # the join-time object, and later updates that carry the whole car
                    bad_parts, hide_car, payload = self.check_parts(peer, size, payload)
                    if hide_car or (bad_parts and PART_POLICY == "kick"):
                        suppress = True
                if CAR_CAPTURE and router == ROUTER_CAR and size:
                    self.log(f"{addr[1]} CAPTURE car {'create' if mtype == 7 else 'update'} id={oid} seq={seq} bits={size} "
                             f"hex={payload.to_bytes((size + 7) // 8, 'little').hex()}")
                if router == ROUTER_CAR and 0 < size <= 80:
                    if peer.car_payload_logs < 8:
                        peer.car_payload_logs += 1
                        self.log(f"{addr[1]} car object {'create' if mtype == 7 else 'update'} payload {size} bits = {payload:#x}")
                    mid = car_model_id(size, payload)
                    if mid is not None:
                        suppress = self.car_model_seen(peer, mid)
                        if (not suppress and UNKNOWN_CAR_POLICY == "standin" and KNOWN_CARS and mid not in KNOWN_CARS and mid not in DLC_CARS
                                and mid not in BLOCKED_CARS):
                            payload = (payload & ~(0xFFFFFFFF << 5)) | (STANDIN_CAR << 5)     # same ModelId field car_model_id reads
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
                    peer.gopts_own_seq = seq
                    peer.gopts_written_at = time.monotonic()
                    self.log(f"{addr[1]} GameOptions {'create' if first else 'update'} relayed "
                             f"({size} bits, own seq {seq}, mtype {mtype})")
                    # debug: raw bits (LSB first) and where small ids (e.g. the event id 1412 = 0x584) appear
                    bitstr = "".join(str((payload >> i) & 1) for i in range(size))
                    self.log(f"  GameOptions payload hex={payload.to_bytes((size + 7) // 8, 'little').hex()}")
                    for width in range(8, 17):
                        for off in range(0, max(0, size - width + 1)):
                            if (payload >> off) & ((1 << width) - 1) == 1412:
                                self.log(f"  GameOptions: value 1412 at bit offset {off} width {width}")
                    unchanged = self.game_options == (size, payload)
                    self.game_options = (size, payload)
                    if unchanged:
                        # A client that has accepted the forwarded options writes the same object back, which was forwarded again and
                        # written back again: with the sequence numbers right (see send_game_options) that became an endless ping-pong
                        # between the clients (dev 2026-10-20 21:11, ~100 writes per second). Identical content is not forwarded again.
                        self.gopts_echo_dropped = getattr(self, "gopts_echo_dropped", 0) + 1
                    else:
                        for q in self.others(peer):
                            self.send_game_options(q)
                elif router in RELAY_ROUTERS and a == 0 and b == 0 and not suppress and not peer.kick_reason:
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
                            self.participant_left(peer)
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
                self.relay(peer, msg)
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
                self.relay(peer, msg)
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
                pos = car_position(rest) if rest_bits >= 96 else None
                far = max(abs(pos[0]), abs(pos[1]), abs(pos[2])) if pos is not None else math.nan
                if pos is not None and a == 0 and b == 0:
                    peer.pos = pos
                    if math.isfinite(far):
                        peer.max_abs = max(peer.max_abs, far)
                        peer.max_y = max(peer.max_y, pos[1])
                    if not peer.pos_flagged and (not all(math.isfinite(c) for c in pos) or far > POSITION_LIMIT):
                        peer.pos_flagged = True
                        self.log(f"{addr[1]} car position out of range: ({pos[0]:.1f},{pos[1]:.1f},{pos[2]:.1f}) owner {peer.owner_id} xuid {peer.xuid:#x}")
                if peer.cars_relayed <= 3 or peer.cars_relayed % 500 == 0:
                    ps = '' if pos is None else f' pos=({pos[0]:.0f},{pos[1]:.0f},{pos[2]:.0f})'
                    self.log(f"{addr[1]} car prediction #{peer.cars_relayed} flags={a}{b} id={cid} "
                             f"rest={rest_bits} bits -> {len(self.others(peer))} peer(s){ps}")
                if pos is not None and a == 0 and b == 0 and SPEED_POLICY != "off" and math.isfinite(far):
                    self.track_speed(peer, pos)
                if peer.cars_relayed % 20 == 1 and pos is not None:
                    self.note("car", peer.owner_id, f"pos=({pos[0]:.0f},{pos[1]:.0f},{pos[2]:.0f}) id={cid}")
                if a == 0 and b == 0 and not peer.kick_reason:
                    peer.car_id = cid
                    msg = relay_car_prediction(cid, peer.owner_id, rest, rest_bits)
                    self.relay(peer, msg)
            elif mtype == MSG_REPAIR_DAMAGE:
                kind, cid, _ = read_car_key(r)
                if kind == "own":
                    msg = relay_repair_message(cid, peer.owner_id)
                    self.relay(peer, msg)
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
        try:
            with urllib.request.urlopen(f"{LSP_URL}/admin/gamemode?type={mode}&xuids={','.join(str(x) for x in xuids)}", timeout=3) as resp:
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
        # The client builds the drive from RoadTripSynced (BuildRouteForDrive, FUN_140805340): it starts at the finish line of EventRaceId and
        # ends at the start of the destination's event number CompletedEventCount; after an event the clients are fast-travelled to that
        # start (the entry-point state, FUN_1408d4140: DriveToEvent after an InEvent / FinaliseEvent goes to "fast travel to the drive grid").
        # So EventRaceId is the event raced last, not the one coming up; the first drive of a fresh lobby has none and starts where the
        # clients spawned (the festival), where this only names the route.
        prev = self.rt_last_race if self.rt_last_race is not None else (int(RT_FIRST_RACE, 0) if RT_FIRST_RACE else race)
        self.rt_drive_ms = road_trip_drive_ms(self.rt_last_loc if self.rt_last_race is not None else None, loc)
        self.rt_end_ms = self.now_ms() + self.rt_drive_ms
        self.log(f"car select over: State -> DriveToEvent (location {loc}, event {race}, from the end of event "
                 f"{prev if self.rt_last_race is not None else 'none: the festival'}, {self.rt_drive_ms // 1000} s)")
        # the clients cancel their drive-to-event wait while SessionHost.RaceReady is off (NetworkIsServerRaceReady)
        self.set_session_host_bool(SH_RACE_READY, 1, "road trip: the next event is chosen")
        # The client raises NetworkStartDriveToDestination (route + drive HUD) from RoadTripSynced's variable-changed
        # handler (FUN_140a43a20) only when, in state DriveToEvent, StateEndTime goes from 0 to non-zero. So the
        # state change carries StateEndTime=0 and the countdown follows as its own update.
        self.rt_broadcast(CurrentLocationId=loc, CompletedEventCount=self.rt_idx, EventRaceId=prev, NextRaceId=race,
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
                # Infection): draw it again when another player arrives, before anybody has voted. Not once a client shows the vote:
                # its open vote screen ignores the update, so it would show other destinations than everybody else (dev 2026-10-22).
                here = sum(1 for c in self.peers.values() if c.connected)
                shown = any(o is not q and o.connected and o.rt_started for o in self.peers.values())
                if here > self.rt_pick_players and not shown:
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
                self.rt_end_ms = self.now_ms() + self.rt_drive_ms
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
        if self.wipe_at and now >= self.wipe_at:
            self.report_wipe(now)
        self.kick_for_cars()
        ev = self.event
        if ev and not ev["begun"] and not ev.get("load_checked") and not self.roadtrip \
                and now - ev.get("started", now) > EVENT_LOAD_TIMEOUT_SECONDS and now >= ev.get("load_next", 0.0):
            self.drop_unloaded(now)
        if self.event and self.event.get("go_at") and now >= self.event["go_at"]:
            self.go_race()
        self.check_race_end(now)
        if self.carmeet or any(q.lobby_type == LOBBY_TYPE_CAR_MEET for q in self.peers.values()):
            self.cm_tick(self.now_ms())
        if self.roadtrip:
            self.road_trip_tick(now)
        if self.pending_race and now >= self.pending_race[0]:
            _, leader = self.pending_race
            # accepted the invite (RaceInProgress) but not Ready yet: still choosing a car. A client that wrote GameOptions of its own around
            # the request was setting up its own event, not accepting this one (prod 2026-10-22 lobby e7255c39: two requests in one second,
            # everybody held 90 s for the second requester, which never got Ready)
            asked = getattr(self, "race_requested_at", 0.0)
            joining = [q for q in self.others(leader)
                       if q.connected and q.entered and q.console_vars.get(3) and not q.console_vars.get(4)
                       and abs(q.gopts_written_at - asked) > RACE_OWN_REQUEST_SECONDS]
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
        nxt = math.inf
        while True:
            if sel.select(timeout=min(0.01, max(0.0, nxt - time.monotonic()))):
                while True:     # drain everything queued before the next timer pass
                    try:
                        data, addr = self.sock.recvfrom(65535)
                    except BlockingIOError:
                        break
                    try:
                        self.handle(data, addr)
                    except Exception as exc:  # keep serving
                        self.log(f"error handling {data.hex()}: {exc!r} at {where(exc)}")
            self.tick()
            nxt = self.flush(time.monotonic())


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
        # Players the car / speed / flight checks kicked: xuid -> (flagged until [wall clock], reason). Matchmaking (xbl._lobby_for) puts
        # them only into lobbies with other flagged players, and everyone else never into those (FH2MP_CHEATER_POOL_HOURS, 0 = off).
        # Kept in a file next to the log so a restart does not let them out.
        self.flagged = {}
        self.flag_file = os.path.join(os.path.dirname(os.path.abspath(logpath)), "cheaters.json") if logpath else None
        self.load_flags()
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
        # Experimental TCP carrier for players whose UDP does not arrive (tunnel.py): FH2MP_TUNNEL_PORT, 0 = off. Worlds send through
        # `mux`, which puts a datagram for a tunnelled address into its TCP connection.
        self.tunnel = None
        self.sel = None
        tport = int(os.environ.get("FH2MP_TUNNEL_PORT", "0") or 0)
        if tport:
            self.tunnel = tunnel_mod.Tunnel(tport, log=self.log, admit=self.tunnel_admit, on_open=self.tunnel_open, on_close=self.tunnel_close)
        self.mux = tunnel_mod.MuxSock(self.sock, self.tunnel)
        self.idle = Server(port, None, sock=self.mux, log_file=self.log_file, tag="[idle] ")
        self.idle.is_idle = True
        self.idle.log(f"hub listening on udp/{port}, control udp/{self.ctl.getsockname()[1] if self.ctl else '-'}"
                      + (f", tunnel tcp/{self.tunnel.port}" if self.tunnel else ""))

    def log(self, msg: str) -> None:
        self.idle.log(msg)

    def world_for(self, lobby_type: int, name: str):
        key = (lobby_type, name)
        w = self.worlds.get(key)
        if w is None:
            tag = f"[{ {1: 'roadtrip', 2: 'freeroam', 3: 'carmeet'}.get(lobby_type, lobby_type) }:{name[:8] or 'default'}] "
            w = Server(0, None, roadtrip=lobby_type == LOBBY_TYPE_ROAD_TRIP, carmeet=lobby_type == LOBBY_TYPE_CAR_MEET,
                       sock=self.mux, log_file=self.log_file, tag=tag)
            w.hub_key = key
            w.hub = self
            self.worlds[key] = w
            self.log(f"new world {key}")
        return w

    def control(self) -> None:
        """Text datagrams from the matchmaking side: "assign <xuid hex> <lobby name>"."""
        while self.ctl:
            try:
                data, reply_to = self.ctl.recvfrom(1024)
            except (BlockingIOError, OSError):
                return
            parts = data.decode("utf-8", "replace").split()
            if len(parts) == 1 and parts[0] == "pop":
                try:
                    self.ctl.sendto(self.populations_text().encode(), reply_to)
                except OSError:
                    pass
            elif parts == ["lobbies"]:
                try:
                    self.ctl.sendto(json.dumps(self.lobby_status()).encode(), reply_to)
                except OSError:
                    pass
            elif len(parts) == 3 and parts[0] == "auth":
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
            elif len(parts) == 2 and parts[0] == "unflag":
                try:
                    if self.flagged.pop(int(parts[1], 16), None) is not None:
                        self.save_flags()
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

    def lobby_status(self) -> dict:
        """Public lobby summary; produced on the game loop so world state is consistent."""
        stages = {0: "Starting", 1: "Voting for a destination", 2: "Choosing cars", 3: "Getting ready",
                  4: "Driving to the event", 5: "Arrived at destination", 6: "Preparing the event",
                  7: "Racing", 8: "Event results"}
        out = []
        for (kind, name), world in self.worlds.items():
            if kind not in (2, LOBBY_TYPE_ROAD_TRIP):
                continue
            peers = [p for p in world.peers.values() if p.connected]
            if not peers:
                continue
            row = {"id": name or "default", "kind": "roadtrip" if kind == LOBBY_TYPE_ROAD_TRIP else "freeroam",
                   "players": len(peers), "capacity": self.WORLD_CAPACITY,
                   "memberXuids": [f"{p.xuid:x}" for p in peers if p.xuid]}
            if kind == LOBBY_TYPE_ROAD_TRIP:
                row.update(status=stages.get(world.rt_state, "Getting ready"), event=world.rt_idx + 1, events=4)
            else:
                row["status"] = "Free roaming"
            out.append(row)
        return {"lobbies": sorted(out, key=lambda r: (r["kind"], -r["players"], r["id"]))}

    def populations(self) -> dict:
        """Lobby name -> hex xuids of the players in it right now (connected or still connecting; 0 when the id is not known yet). The
        matchmaking side asks for this to put a new player into the fullest lobby that has room."""
        out = {}
        for addr, w in self.route.items():
            key = getattr(w, "hub_key", None)
            if key is None or not key[1]:
                continue
            peer = w.peers.get(addr)
            out.setdefault(key[1], []).append("%x" % (peer.xuid if peer is not None and peer.xuid else 0))
        return out

    def populations_text(self) -> str:
        text = "pop" + "".join(" %s=%s" % (name, ",".join(xs)) for name, xs in self.populations().items())
        flagged = self.flagged_now()
        return text + (" !flagged=" + ",".join("%x" % x for x in flagged) if flagged else "")

    def load_flags(self) -> None:
        try:
            with open(self.flag_file) as f:
                self.flagged = {int(k): (float(v[0]), str(v[1])) for k, v in json.load(f).items()}
        except (OSError, ValueError, TypeError, IndexError, AttributeError):
            self.flagged = {}

    def save_flags(self) -> None:
        if not self.flag_file:
            return
        try:
            tmp = self.flag_file + ".tmp"
            with open(tmp, "w") as f:
                json.dump({str(k): list(v) for k, v in self.flagged.items()}, f)
            os.replace(tmp, self.flag_file)
        except OSError:
            pass

    def flag(self, xuid: int, reason: str) -> None:
        """A kicked player goes into the cheater pool for CHEATER_POOL_HOURS (renewed by every kick)."""
        if CHEATER_POOL_HOURS <= 0 or not xuid:
            return
        self.flagged[xuid] = (time.time() + CHEATER_POOL_HOURS * 3600.0, reason)
        self.save_flags()
        self.log(f"control: {xuid:#x} flagged for {CHEATER_POOL_HOURS:g} h ({reason}): matched only with other flagged players")

    def flagged_now(self) -> list:
        now = time.time()
        gone = [x for x, (until, _r) in self.flagged.items() if until < now]
        for x in gone:
            del self.flagged[x]
        if gone:
            self.save_flags()
        return list(self.flagged)

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
        if len(data) < 12 or data[8] == 0 or data[11] & 0x7F != MSG_OPEN_CONNECTION:
            return None         # message count (byte 8) and first type (low 7 bits of byte 11): no parse for the other ~99.9 %
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
            if target is not cur and self.world_load(target, addr, player_id) >= self.WORLD_CAPACITY:
                # The HUD player list has exactly 12 slots (HudPartyMembersComponent ctor FUN_1407726c0); a 13th player makes every
                # client in the world read past them and call through a CUserProfile (game+0x144661d, "exception at 0x9") [C].
                self.log(f"{player_id:#x} assigned to full lobby {name or 'default'}: sent to another world")
                target = self.open_world_for(ltype)
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

    def tunnel_admit(self, ip: str) -> bool:
        """A TCP connection is only taken from an address a player is signed in from (with UDP auth required)."""
        if self.udp_auth != "required":
            return True
        now = time.monotonic()
        return any(ent[0] == ip and ent[1] > now for ent in self.authed.values())

    def tunnel_open(self, conn) -> None:
        if self.sel is not None:
            self.sel.register(conn.sock, selectors.EVENT_READ, conn)

    def tunnel_close(self, conn) -> None:
        if self.sel is not None:
            try:
                self.sel.unregister(conn.sock)
            except (KeyError, ValueError, OSError):
                pass

    def tunnel_events(self, key) -> None:
        """One readable socket of the selector: the TCP listener or a tunnelled client."""
        if key.fileobj is self.tunnel.listener:
            self.tunnel.accept()
            return
        conn = key.data
        for data in self.tunnel.read(conn):
            try:
                self.handle(data, conn.addr)
            except Exception as exc:
                self.log(f"error handling tunnelled {data.hex()}: {exc!r} at {where(exc)}")

    def tunnel_sweep(self, now: float) -> None:
        """Closes tunnel connections that died, went silent or never opened a game connection, and drops their players."""
        def opened(addr):
            w = self.route.get(addr)
            return w is not None and addr in w.peers
        for addr in self.tunnel.expire(now, opened):
            w = self.route.pop(addr, None)
            peer = w.peers.get(addr) if w is not None else None
            if peer is not None:
                w.drop_peer(peer, "tunnel connection closed")

    def is_authed(self, player_id: int, ip: str) -> bool:
        ent = self.authed.get(player_id)
        return ent is not None and ent[1] > time.monotonic() and ent[0] == ip

    WORLD_CAPACITY = 12

    def world_load(self, w, addr=None, xuid=0) -> int:
        """Players routed to world `w` (connected or still connecting), not counting `addr` or an older connection of `xuid`."""
        n = 0
        for a, rw in self.route.items():
            if rw is w and a != addr:
                peer = w.peers.get(a)
                n += not (xuid and peer is not None and peer.xuid == xuid)
        return n

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
        sel = self.sel = selectors.DefaultSelector()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        self.sock.setblocking(False)
        sel.register(self.sock, selectors.EVENT_READ)
        if self.ctl:
            self.ctl.setblocking(False)
            sel.register(self.ctl, selectors.EVENT_READ)
        if self.tunnel:
            sel.register(self.tunnel.listener, selectors.EVENT_READ)
        last_sweep = 0.0
        nxt = math.inf
        while True:
            events = sel.select(timeout=min(0.01, max(0.0, nxt - time.monotonic())))
            if events:
                self.control()
                for key, _mask in events:
                    if self.tunnel and key.fileobj is not self.sock and key.fileobj is not self.ctl:
                        self.tunnel_events(key)
                while True:     # drain everything queued before the next timer pass
                    try:
                        data, addr = self.sock.recvfrom(65535)
                    except BlockingIOError:
                        break
                    try:
                        self.handle(data, addr)
                    except Exception as exc:  # keep serving
                        self.log(f"error handling {data.hex()}: {exc!r} at {where(exc)}")
            for w in list(self.worlds.values()):
                try:
                    w.tick()
                except Exception as exc:  # one broken world must not stop the others
                    w.log(f"tick error: {exc!r} at {where(exc)}")
            now = time.monotonic()
            nxt = self.idle.flush(now)
            for w in self.worlds.values():
                nxt = min(nxt, w.flush(now))
            if self.tunnel:
                self.tunnel.flush_all()
            if now - last_sweep > 1.0:
                last_sweep = now
                self.sweep()
                if self.tunnel:
                    self.tunnel_sweep(now)


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = int(args[0]) if len(args) > 0 else 5000
    logfile = args[1] if len(args) > 1 else None
    try:
        Hub(port, logfile, roadtrip="--roadtrip" in sys.argv, carmeet="--carmeet" in sys.argv).serve()
    except KeyboardInterrupt:
        pass
