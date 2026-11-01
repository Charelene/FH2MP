"""Server side of the game's networked civilian traffic (NerdLib "traffic router"), for the FH2 Free Roam server.

How it works in the retail game (recovered from the client binary, build 1.0.0.64; see docs/traffic.md):
  * Every traffic car is simulated by ONE client, its owner. The server never needs the road network: it hands out car ids,
    decides who owns which car, and relays the owner's state updates to everybody else.
  * Client -> server: ServerTrafficMessage (id 71) carrying a TrafficServerMessageType, plus the update messages
    ServerTrafficFullSync 16, RouteProgressUpdate 17, FullRoute 73, Stationary 74.
  * Server -> client: ClientTrafficMessage (id 72) carrying a TrafficClientMessageType and a per-client sequence number that
    the client applies strictly in order (FUN_1403b8b60), plus ClientTraffic FullSync 18, RouteProgressUpdate 19, FullRoute 75,
    Stationary 76.

Message ids, field layouts, the enum orders and the client's reaction to each server message are [C] (decompiled). Who holds and owns what is
traffic_manager.py, a port of the game's own manager; this file is the wire and the join sequence.
"""
import os
import struct
import time

import diag
import traffic_manager

LOG = diag.get("traffic")
_RATE = diag.RateLimit()
IGNORED_LOG_SECONDS = 30.0

# message ids
MSG_SERVER_FULL_SYNC = 16
MSG_SERVER_ROUTE_PROGRESS = 17
MSG_SERVER_MESSAGE = 71
MSG_SERVER_FULL_ROUTE = 73
MSG_SERVER_STATIONARY = 74
MSG_CLIENT_FULL_SYNC = 18
MSG_CLIENT_ROUTE_PROGRESS = 19
MSG_CLIENT_MESSAGE = 72
MSG_CLIENT_FULL_ROUTE = 75
MSG_CLIENT_STATIONARY = 76

# TrafficServerMessageType (client -> server), in enum order
S_REQUEST_TRAFFIC, S_WITHDRAW_REQUEST, S_REQUEST_DESPAWN, S_TRAFFIC_SPAWNED, S_TRAFFIC_REMOVED, S_CAN_TAKE_OWNERSHIP, \
    S_REQUEST_OWNERSHIP, S_CAR_MODEL_LOADED, S_CAR_MODEL_UNLOADED = range(9)
S_NAMES = ["RequestTraffic", "WithdrawTrafficRequest", "RequestDespawn", "TrafficSpawned", "TrafficRemoved", "CanTakeOwnership",
           "RequestOwnership", "CarModelLoaded", "CarModelUnloaded"]

# TrafficClientMessageType (server -> client), in enum order (the client's switch in FUN_1403b8b60 confirms the order)
C_TRAFFIC_LIMIT, C_SPAWN_GRANTED, C_SPAWN_CANCELED, C_WITHDRAW_ACCEPTED, C_ADD_TRAFFIC, C_REMOVE_TRAFFIC, C_TAKE_OWNERSHIP, \
    C_DROP_OWNERSHIP, C_OWNERSHIP_DENIED, C_LOAD_CAR_MODEL, C_REMOVE_CAR_MODEL, C_UNLOAD_CAR_MODEL, C_ALL_MODELS_SENT, \
    C_INITIAL_JOIN_COMPLETE = range(14)
C_NAMES = ["TrafficLimit", "SpawnRequestGranted", "SpawnRequestCanceled", "WithdrawRequestAccepted", "AddTraffic",
           "RemoveTraffic", "TakeOwnership", "DropOwnership", "OwnershipRequestDenied", "LoadCarModel", "RemoveCarModel",
           "UnloadCarModel", "AllModelIdsSent", "InitialJoinComplete"]

# Field widths of the update messages, in wire order (FUN_141821550, 1418211a0, 1418212b0, 141820020, 1418213e0).
# S3 = three 32-bit words (floats). Every message starts with the 8-bit car id.
UPDATE_LAYOUT = {
    MSG_SERVER_FULL_SYNC: [8, 32, 32, 32, 32] + [16] * 9 + [32, 1, 1],
    # RouteProgress / FullRoute carry one more 3 x 32 block when sent to the server than the client reads back (the car's position:
    # FUN_1417f6c00 / FUN_1417f6cd0 write it, FUN_14181fce0 / FUN_14181fe30 do not read it); the server keeps it and forwards the rest.
    MSG_SERVER_ROUTE_PROGRESS: [8, 32, 32, 32, 32, 32, 32],
    MSG_SERVER_FULL_ROUTE: [8] + [16] * 8 + [2, 32, 32, 32, 32, 32, 32],
    MSG_SERVER_STATIONARY: [8, 32, 32, 32, 16, 16, 16, 32, 1],
}
# What is sent on to the other clients (the same list, minus the trailing position block of 17 and 73)
RELAY_LAYOUT = {
    MSG_SERVER_FULL_SYNC: UPDATE_LAYOUT[MSG_SERVER_FULL_SYNC],
    MSG_SERVER_ROUTE_PROGRESS: UPDATE_LAYOUT[MSG_SERVER_ROUTE_PROGRESS][:4],
    MSG_SERVER_FULL_ROUTE: UPDATE_LAYOUT[MSG_SERVER_FULL_ROUTE][:-3],
    MSG_SERVER_STATIONARY: UPDATE_LAYOUT[MSG_SERVER_STATIONARY],
}
RELAY_ID = {MSG_SERVER_FULL_SYNC: MSG_CLIENT_FULL_SYNC, MSG_SERVER_ROUTE_PROGRESS: MSG_CLIENT_ROUTE_PROGRESS,
            MSG_SERVER_FULL_ROUTE: MSG_CLIENT_FULL_ROUTE, MSG_SERVER_STATIONARY: MSG_CLIENT_STATIONARY}
UPDATE_NAMES = {MSG_SERVER_FULL_SYNC: "FullSync", MSG_SERVER_ROUTE_PROGRESS: "RouteProgress", MSG_SERVER_FULL_ROUTE: "FullRoute",
                MSG_SERVER_STATIONARY: "Stationary"}

TRAFFIC_LIMIT = traffic_manager.HARD_CAP
GRANT_TIMEOUT_S = 15.0    # a granted id that is never spawned is taken back
TICK_S = 0.2              # holders and ownership change with distance, at driving speed; retail did this every frame
MAX_MESSAGES_PER_S = 400  # per client, requests and updates together; far above what a client sends
RADIUS_KINDS = frozenset((0, 1, 2, 4, 5, 6))   # these carry the client's radius (callers of SetPlayerTrafficBudget); in 3, 7 and 8 the field is the car model


def load_models(path=None):
    """Traffic car model ids (game database table TrafficCars, 17 rows). Read from a text file of comma separated ids so no
    game data lives in the code: env FH2MP_TRAFFIC_CARS, or data/trafficcars.txt next to this file."""
    path = path or os.environ.get("FH2MP_TRAFFIC_CARS") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "data",
                                                                          "trafficcars.txt")
    try:
        with open(path) as f:
            return [int(x) for x in f.read().replace("\n", ",").split(",") if x.strip()]
    except OSError as e:
        LOG.warn(f"car model list {path} cannot be read, so there is no online traffic (TrafficWorld stays disabled): {diag.where(e)}")
        return []


def f32(bits):
    return struct.unpack("<f", struct.pack("<I", bits & 0xFFFFFFFF))[0]


def signed14(v):
    return v - (1 << 14) if v & (1 << 13) else v


def client_message(kind, seq, a=0xFF, s14=-1, u5=31, b=0xFF, flag=0):
    """ClientTrafficMessage: kind:4 | u8 | s14 | u5 | u8 | bool | seq:32 (FUN_14181fd70 / FUN_141820950)."""
    def write(w):
        w.bits(C_MSG_ID, 7)
        w.bits(kind, 4)
        w.bits(a & 0xFF, 8)
        w.bits(s14 & 0x3FFF, 14)
        w.bits(u5 & 0x1F, 5)
        w.bits(b & 0xFF, 8)
        w.bits(flag & 1, 1)
        w.bits(seq & 0xFFFFFFFF, 32)
    return write


C_MSG_ID = MSG_CLIENT_MESSAGE


def update_message(mtype, fields):
    layout = RELAY_LAYOUT[mtype]

    def write(w):
        w.bits(RELAY_ID[mtype], 7)
        for width, v in zip(layout, fields):
            w.bits(v, width)
    return write


class PeerState:
    def __init__(self):
        self.seq = 0                 # next ClientTrafficMessage sequence number for this client
        self.joined = False          # the initial sequence was sent
        self.joined_at = 0.0
        self.burst_n = 0             # CarModelLoaded messages in the current burst
        self.burst_at = None         # time of the last CarModelLoaded (the client announces its loaded models whenever it
                                     # switches (back) to online traffic, e.g. after a fast travel)
        self.granted_at = {}         # car id -> time granted
        self.rx = 0                  # server-bound messages seen
        self.window_at = 0.0         # start of the current rate window
        self.window_n = 0
        self.flooded = False


class CarData:
    """What traffic_manager calls a car's info: its AddTraffic struct and the last state of each kind, for whoever gets it next."""
    __slots__ = ("struct", "full_sync", "full_route", "stationary")

    def __init__(self, struct_):
        self.struct = struct_        # (a, s14, u5, b, flag) as the owner sent it
        self.full_sync = self.full_route = self.stationary = None


def update_position(mtype, fields):
    """The car's position in an update from its owner: FullSync and Stationary carry it first, RouteProgress and FullRoute as the block only the server reads."""
    if mtype == MSG_SERVER_FULL_SYNC:
        words = fields[2:5]
    elif mtype == MSG_SERVER_STATIONARY:
        words = fields[1:4]
    else:
        words = fields[-3:]
    return tuple(f32(w) for w in words)


class TrafficWorld:
    """Traffic for one world (lobby). `send(peer, [writer, ...], fast=)` delivers messages; `peers()` lists the connected, entered clients; `log(text)` logs;
    `universe(peer)` says which side of an event boundary a client is on; `bake` freezes a writer so one update reaches many clients cheaply."""

    def __init__(self, send, peers, log, models=None, universe=None, bake=None):
        self.send, self.peers, self.log = send, peers, log
        self.universe = universe or (lambda peer: 0)
        self.bake = bake or (lambda m: m)
        self.models = models if models is not None else load_models()
        self.state = {}
        self.enabled = bool(self.models) and os.environ.get("FH2MP_TRAFFIC", "1") != "0"
        self.counters = {}
        self.manager = traffic_manager.TrafficManager(self)
        self.next_tick = 0.0
        self._batch = None
        if not self.enabled:
            self.log("traffic: disabled (no model list or FH2MP_TRAFFIC=0)")
        else:
            self.log(f"traffic: {len(self.models)} car models")

    # ------------------------------------------------------------ helpers
    def st(self, peer):
        return self.state.setdefault(peer, PeerState())

    def active(self):
        return [p for p in self.peers() if p in self.state and self.state[p].joined]

    def push(self, peer, kind, **fields):
        s = self.st(peer)
        msg = client_message(kind, s.seq, **fields)
        s.seq = (s.seq + 1) & 0xFFFFFFFF
        self.counters[C_NAMES[kind]] = self.counters.get(C_NAMES[kind], 0) + 1
        if self._batch is not None:
            self._batch.append(msg)
        else:
            self.send(peer, [msg])

    def emit(self, peer, msg):
        """A non-sequenced message (an update relay), kept in order with the batch when one is open."""
        if self._batch is not None:
            self._batch.append(msg)
        else:
            self.send(peer, [msg], fast=True)

    # ------------------------------------------------------------ traffic_manager.Out
    def add(self, peer, car):
        d = car.info
        a, s14, u5, b, flag = d.struct
        self.push(peer, C_ADD_TRAFFIC, a=a, s14=s14, u5=u5, b=b, flag=flag)
        for mtype, fields in ((MSG_SERVER_FULL_SYNC, d.full_sync), (MSG_SERVER_FULL_ROUTE, d.full_route), (MSG_SERVER_STATIONARY, d.stationary)):
            if fields:
                self.emit(peer, update_message(mtype, fields))

    def remove(self, peer, vid):
        if peer in self.state:
            self.push(peer, C_REMOVE_TRAFFIC, a=vid)

    def granted(self, peer, vid):
        self.st(peer).granted_at[vid] = time.monotonic()
        self.push(peer, C_SPAWN_GRANTED, a=vid)

    def canceled(self, peer, vid):
        self.st(peer).granted_at.pop(vid, None)
        self.push(peer, C_SPAWN_CANCELED, a=vid)

    def withdrawn(self, peer):
        self.push(peer, C_WITHDRAW_ACCEPTED)

    def take(self, peer, vid):
        self.push(peer, C_TAKE_OWNERSHIP, a=vid)

    def drop(self, peer, vid):
        if peer in self.state:
            self.push(peer, C_DROP_OWNERSHIP, a=vid)

    def denied(self, peer, vid):
        self.push(peer, C_OWNERSHIP_DENIED, a=vid)
        ok, skipped = _RATE.allow(("denied", peer.addr), IGNORED_LOG_SECONDS)
        if ok:
            LOG.debug(f"{peer.addr[1]} ownership of car {vid} denied: {self.manager.refusal}" + (f" (+{skipped} more since the last line)" if skipped else ""))

    # ------------------------------------------------------------ lifecycle
    def join(self, peer, force=False):
        """The server's initial sequence for a client that entered the session (the client waits for InitialJoinComplete:
        module flags +0xac/+0xad, FUN_140716800 / FUN_140720950). Cars follow from the tick, once its position and loaded models are known."""
        if not self.enabled:
            return
        s = self.st(peer)
        if s.joined and not force:
            return
        if s.joined:
            self.manager.reset(peer)
        s.joined = True
        s.joined_at = time.monotonic()
        self.manager.join(peer)
        self._batch = []
        for model in self.models:
            self.push(peer, C_LOAD_CAR_MODEL, s14=model)
        self.push(peer, C_ALL_MODELS_SENT)
        self.push(peer, C_TRAFFIC_LIMIT, a=TRAFFIC_LIMIT)
        self.push(peer, C_INITIAL_JOIN_COMPLETE)
        batch, self._batch = self._batch, None
        self.send(peer, batch)
        self.log(f"traffic: join {peer.addr[1]}: {len(self.models)} models, limit {TRAFFIC_LIMIT}")

    def leave(self, peer):
        if self.state.pop(peer, None) is not None:
            self.manager.leave(peer)
            self.log(f"traffic: {peer.addr[1]} left; {len(self.manager.cars)} cars remain")

    # ------------------------------------------------------------ client -> server
    def flooding(self, peer, s):
        now = time.monotonic()
        if now - s.window_at >= 1.0:
            s.window_at, s.window_n = now, 0
        s.window_n += 1
        if s.window_n <= MAX_MESSAGES_PER_S:
            return False
        if not s.flooded:
            s.flooded = True
            self.log(f"traffic: {peer.addr[1]} over {MAX_MESSAGES_PER_S} messages/s; the excess is ignored")
        return True

    def on_message(self, peer, kind, a, s14, u5, b, flag, seq):
        s = self.st(peer)
        s.rx += 1
        name = S_NAMES[kind] if kind < len(S_NAMES) else f"?{kind}"
        if s.rx <= 60 or s.rx % 100 == 0:
            self.log(f"traffic: {peer.addr[1]} -> {name} a={a} s14={signed14(s14)} u5={u5} b={b} flag={flag} seq={seq} "
                     f"(held {len(self.manager.players[peer].held) if peer in self.manager.players else 0})")
        if not self.enabled or not s.joined or self.flooding(peer, s):
            return
        m, now = self.manager, time.monotonic() * 1000.0
        if kind in RADIUS_KINDS:
            m.set_radius(peer, signed14(s14))
        if kind == S_REQUEST_TRAFFIC:
            m.set_universe(peer, self.universe(peer))
            m.request(peer, max(1, a if a != 0xFF else 1))
        elif kind == S_WITHDRAW_REQUEST:
            m.withdraw(peer)
        elif kind == S_TRAFFIC_SPAWNED:
            s.granted_at.pop(a, None)
            if m.spawned(peer, a, signed14(s14), CarData((a, s14, u5, b, flag)), now):
                self.push(peer, C_TAKE_OWNERSHIP, a=a)       # the spawner drives the car; without this it despawns it after ~7 s
            else:
                self.log(f"traffic: {peer.addr[1]} spawned car {a} it was not granted; ignored ({m.refusal})")
        elif kind == S_TRAFFIC_REMOVED:
            m.removed(peer, a)
        elif kind == S_REQUEST_DESPAWN:
            m.despawn(peer, a, now)
        elif kind == S_CAN_TAKE_OWNERSHIP:
            m.can_take(peer, a)
        elif kind == S_REQUEST_OWNERSHIP:
            if m.request_ownership(peer, a, now):
                self.log(f"traffic: car {a} handed to {peer.addr[1]} (RequestOwnership)")
        elif kind == S_CAR_MODEL_LOADED:
            m.model_loaded(peer, signed14(s14))
            s.burst_at = time.monotonic()
            s.burst_n += 1
        elif kind == S_CAR_MODEL_UNLOADED:
            m.model_unloaded(peer, signed14(s14))

    def on_update(self, peer, mtype, reader):
        """FullSync / RouteProgress / FullRoute / Stationary from the car's owner: copy to the others who hold it."""
        fields = [reader.bits(w) for w in UPDATE_LAYOUT[mtype]]        # read first: the rest of the datagram follows it
        vid = fields[0]
        n = self.counters.get("rx_" + UPDATE_NAMES[mtype], 0) + 1
        self.counters["rx_" + UPDATE_NAMES[mtype]] = n
        if n <= 6 or n % 500 == 0:
            self.log(f"traffic: {peer.addr[1]} -> {UPDATE_NAMES[mtype]} car {vid} (#{n})")
        m = self.manager
        car = m.cars.get(vid)
        if not self.enabled or car is None or car.owner is not peer or self.flooding(peer, self.st(peer)):
            if self.enabled and (car is None or car.owner is not peer):
                ok, skipped = _RATE.allow(("stale", peer.addr), IGNORED_LOG_SECONDS)
                if ok:
                    LOG.debug(f"{peer.addr[1]} {UPDATE_NAMES[mtype]} for car {vid} ignored: "
                              + ("no such car" if car is None else f"owned by {getattr(car.owner, 'addr', ('', car.owner))[1]}")
                              + (f" (+{skipped} more since the last line)" if skipped else ""))
            return
        if mtype == MSG_SERVER_FULL_SYNC:
            car.info.full_sync = fields
        elif mtype == MSG_SERVER_FULL_ROUTE:
            car.info.full_route = fields
        elif mtype == MSG_SERVER_STATIONARY:
            car.info.stationary = fields
        m.car_position(peer, vid, update_position(mtype, fields))
        msg = None
        for q in car.holders:
            if q is not peer:
                msg = msg or self.bake(update_message(mtype, fields))
                self.emit(q, msg)

    # ------------------------------------------------------------ timers
    def tick(self, now):
        if not self.enabled or not self.state or now < self.next_tick:
            return
        self.next_tick = now + TICK_S
        m = self.manager
        for peer, s in list(self.state.items()):
            # A big burst of CarModelLoaded long after the join means the client just went (back) online, e.g. after a fast
            # travel (FUN_140726230 reports every loaded model at once). While a session starts the models trickle in one or
            # two at a time instead, which must not trigger a resync.
            if s.burst_at is not None and now - s.burst_at > 0.3:
                n, s.burst_n, s.burst_at = s.burst_n, 0, None
                if n >= 4 and now - s.joined_at > 25.0:
                    self.log(f"traffic: {peer.addr[1]} reported {n} loaded models at once; repeating the join sequence")
                    self.join(peer, force=True)
            if peer in m.players:
                pos = getattr(peer, "pos", None)
                if pos is not None:
                    m.set_position(peer, pos)
                m.set_universe(peer, self.universe(peer))
            for vid, t in list(s.granted_at.items()):
                if now - t > GRANT_TIMEOUT_S:
                    s.granted_at.pop(vid)
                    self.log(f"traffic: grant of car {vid} to {peer.addr[1]} timed out")
                    m.withdraw_one(peer, vid)
                    self.push(peer, C_SPAWN_CANCELED, a=vid)
        m.tick(now * 1000.0)
