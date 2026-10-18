"""Server side of the game's networked civilian traffic (NerdLib "traffic router"), for the FH2 Free Roam server.

How it works in the retail game (recovered from the client binary, build 1.0.0.64; see docs/traffic.md):
  * Every traffic car is simulated by ONE client, its owner. The server never needs the road network: it hands out car ids,
    decides who owns which car, and relays the owner's state updates to everybody else.
  * Client -> server: ServerTrafficMessage (id 71) carrying a TrafficServerMessageType, plus the update messages
    ServerTrafficFullSync 16, RouteProgressUpdate 17, FullRoute 73, Stationary 74.
  * Server -> client: ClientTrafficMessage (id 72) carrying a TrafficClientMessageType and a per-client sequence number that
    the client applies strictly in order (FUN_1403b8b60), plus ClientTraffic FullSync 18, RouteProgressUpdate 19, FullRoute 75,
    Stationary 76.

Confidence: message ids, field layouts, the enum orders and the client's reaction to each server message are [C] (decompiled);
the server-side rules (limits, ownership policy, when to grant) are [guess] and are logged heavily so they can be tuned from
real sessions.
"""
import os
import random
import struct
import time

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

MAX_CARS = 250            # car ids are 8 bit; 0xff is "none"
PER_PLAYER_MAX = 18       # the game's offline limit (FUN_140728a30 sets 0x12)
PER_PLAYER_MIN = 6
WORLD_BUDGET = 36         # total civilian cars the world aims for; each player's limit is WORLD_BUDGET / players
OWNERSHIP_COOLDOWN_S = 2.0  # a car changes hands only if more than 2000 ms passed since its last change (FUN_141955d40) [C]
GRANT_TIMEOUT_S = 15.0    # a granted id that is never spawned is taken back


def load_models(path=None):
    """Traffic car model ids (game database table TrafficCars, 17 rows). Read from a text file of comma separated ids so no
    game data lives in the code: env FH2MP_TRAFFIC_CARS, or data/trafficcars.txt next to this file."""
    path = path or os.environ.get("FH2MP_TRAFFIC_CARS") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "data",
                                                                          "trafficcars.txt")
    try:
        with open(path) as f:
            return [int(x) for x in f.read().replace("\n", ",").split(",") if x.strip()]
    except OSError:
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


class Vehicle:
    __slots__ = ("vid", "owner", "struct", "full_sync", "full_route", "stationary", "route_progress", "born", "model", "moved")

    def __init__(self, vid, owner, struct_):
        self.vid, self.owner, self.struct = vid, owner, struct_     # struct = (a, s14, u5, b, flag) as the owner sent it
        self.full_sync = self.full_route = self.stationary = self.route_progress = None
        self.born = time.monotonic()
        self.moved = self.born       # time of the last ownership change (a new car counts as just changed: FUN_141957ba0)
        self.model = struct_[1] if struct_ else None


class PeerState:
    def __init__(self):
        self.seq = 0                 # next ClientTrafficMessage sequence number for this client
        self.joined = False          # the initial sequence was sent
        self.joined_at = 0.0
        self.burst_n = 0             # CarModelLoaded messages in the current burst
        self.burst_at = None         # time of the last CarModelLoaded (the client announces its loaded models whenever it
                                     # switches (back) to online traffic, e.g. after a fast travel)
        self.owned = set()           # car ids this client simulates
        self.pending = {}            # granted car id -> time granted (not yet spawned)
        self.wanted = 0              # outstanding RequestTraffic count we could not grant yet
        self.can_take = set()        # car ids this client announced it could take over (CanTakeOwnership)
        self.models = set()          # model ids the client reported loaded
        self.limit = PER_PLAYER_MAX
        self.rx = 0                  # server-bound messages seen
        self.last_rx_seq = None


class TrafficWorld:
    """Traffic bookkeeping for one world (lobby). `send(peer, [writer, ...])` delivers messages; `peers()` lists the connected,
    entered clients; `log(text)` logs."""

    def __init__(self, send, peers, log, models=None):
        self.send, self.peers, self.log = send, peers, log
        self.models = models if models is not None else load_models()
        self.vehicles = {}
        self.state = {}
        self.enabled = bool(self.models) and os.environ.get("FH2MP_TRAFFIC", "1") != "0"
        self.counters = {}
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
            self.send(peer, [msg])

    def per_player_limit(self):
        n = max(1, len(self.active()))
        return max(PER_PLAYER_MIN, min(PER_PLAYER_MAX, WORLD_BUDGET // n))

    def free_id(self):
        used = set(self.vehicles) | {i for s in self.state.values() for i in s.pending}
        for _ in range(MAX_CARS):
            vid = random.randrange(MAX_CARS)
            if vid not in used:
                return vid
        return None

    # ------------------------------------------------------------ lifecycle
    def join(self, peer, force=False):
        """The server's initial sequence for a client that entered the session (the client waits for InitialJoinComplete:
        module flags +0xac/+0xad, FUN_140716800 / FUN_140720950)."""
        if not self.enabled:
            return
        s = self.st(peer)
        if s.joined and not force:
            return
        s.joined = True
        s.joined_at = time.monotonic()
        s.limit = self.per_player_limit()
        self._batch = []
        for model in self.models:
            self.push(peer, C_LOAD_CAR_MODEL, s14=model)
        self.push(peer, C_ALL_MODELS_SENT)
        self.push(peer, C_TRAFFIC_LIMIT, a=s.limit)
        n = 0
        for v in sorted(self.vehicles.values(), key=lambda v: v.vid):
            if v.owner is peer:
                continue
            a, s14, u5, b, flag = v.struct
            self.push(peer, C_ADD_TRAFFIC, a=a, s14=s14, u5=u5, b=b, flag=flag)
            for mtype, fields in (("full_sync", v.full_sync), ("full_route", v.full_route), ("stationary", v.stationary)):
                if fields:
                    self.emit(peer, update_message({"full_sync": MSG_SERVER_FULL_SYNC, "full_route": MSG_SERVER_FULL_ROUTE,
                                                    "stationary": MSG_SERVER_STATIONARY}[mtype], fields))
            n += 1
        self.push(peer, C_INITIAL_JOIN_COMPLETE)
        batch, self._batch = self._batch, None
        self.send(peer, batch)
        self.log(f"traffic: join {peer.addr[1]}: {len(self.models)} models, limit {s.limit}, {n} existing cars")
        self.rebalance_limits(exclude=peer)

    def rebalance_limits(self, exclude=None):
        limit = self.per_player_limit()
        for p in self.active():
            s = self.st(p)
            if p is not exclude and s.limit != limit:
                s.limit = limit
                self.push(p, C_TRAFFIC_LIMIT, a=limit)

    def leave(self, peer):
        s = self.state.pop(peer, None)
        if not s:
            return
        gone = sorted(s.owned)
        for vid in gone:
            self.remove_vehicle(vid, notify_owner=False)
        self.log(f"traffic: {peer.addr[1]} left; removed its {len(gone)} cars; {len(self.vehicles)} left")
        self.rebalance_limits()

    def remove_vehicle(self, vid, notify_owner=True):
        v = self.vehicles.pop(vid, None)
        if not v:
            return
        if v.owner in self.state:
            self.state[v.owner].owned.discard(vid)
        for p in self.active():
            if p is v.owner and not notify_owner:
                continue
            self.push(p, C_REMOVE_TRAFFIC, a=vid)

    # ------------------------------------------------------------ client -> server
    def on_message(self, peer, kind, a, s14, u5, b, flag, seq):
        s = self.st(peer)
        s.rx += 1
        name = S_NAMES[kind] if kind < len(S_NAMES) else f"?{kind}"
        if s.rx <= 60 or s.rx % 100 == 0:
            self.log(f"traffic: {peer.addr[1]} -> {name} a={a} s14={signed14(s14)} u5={u5} b={b} flag={flag} seq={seq} "
                     f"(owned {len(s.owned)}, pending {len(s.pending)})")
        if not self.enabled:
            return
        if kind == S_REQUEST_TRAFFIC:
            s.wanted = max(1, a if a != 0xFF else 1)
            self.grant(peer)
        elif kind == S_WITHDRAW_REQUEST:
            s.wanted = 0
            for vid in list(s.pending):
                s.pending.pop(vid)
                self.push(peer, C_SPAWN_CANCELED, a=vid)
            self.push(peer, C_WITHDRAW_ACCEPTED)
        elif kind == S_TRAFFIC_SPAWNED:
            vid = a
            if vid in s.pending:
                s.pending.pop(vid)
            if vid in self.vehicles and self.vehicles[vid].owner is not peer:
                self.log(f"traffic: {peer.addr[1]} spawned id {vid} that belongs to someone else; ignored")
                return
            v = self.vehicles.get(vid) or Vehicle(vid, peer, (a, s14, u5, b, flag))
            v.owner, v.struct = peer, (a, s14, u5, b, flag)
            self.vehicles[vid] = v
            s.owned.add(vid)
            s.can_take.add(vid)      # a spawn marks its owner as able to take the car (back) later: FUN_141957ba0 sets the flag
            for q in self.active():
                if q is not peer:
                    self.push(q, C_ADD_TRAFFIC, a=a, s14=s14, u5=u5, b=b, flag=flag)
            self.push(peer, C_TAKE_OWNERSHIP, a=vid)       # the spawner drives the car; it starts sending updates
            if s.wanted:
                self.grant(peer)
        elif kind == S_TRAFFIC_REMOVED:
            v = self.vehicles.get(a)
            if v and v.owner is peer:
                self.remove_vehicle(a, notify_owner=False)
            elif v and v.owner is None:
                self.vehicles.pop(a, None)
        elif kind == S_REQUEST_DESPAWN:
            v = self.vehicles.get(a)
            if v:
                self.remove_vehicle(a, notify_owner=True)     # the owner removes it and answers TrafficRemoved
        elif kind in (S_CAN_TAKE_OWNERSHIP, S_REQUEST_OWNERSHIP):
            self.ownership(peer, kind, a)
        elif kind == S_CAR_MODEL_LOADED:
            s.models.add(signed14(s14))
            s.burst_at = time.monotonic()
            s.burst_n += 1
        elif kind == S_CAR_MODEL_UNLOADED:
            s.models.discard(signed14(s14))

    def grant(self, peer):
        s = self.st(peer)
        while s.wanted > 0 and len(s.owned) + len(s.pending) < s.limit:
            vid = self.free_id()
            if vid is None:
                break
            s.pending[vid] = time.monotonic()
            s.wanted -= 1
            self.push(peer, C_SPAWN_GRANTED, a=vid)

    def ownership(self, peer, kind, vid):
        """CanTakeOwnership: the client says it could drive car `vid`; the original server only remembers that (FUN_141955d80)
        and answers nothing. RequestOwnership (sent when the client collides with the car): the original checks that flag, tells
        the old owner to drop the car and moves the owner (FUN_141956a20); the new owner is told to take it."""
        v = self.vehicles.get(vid)
        s = self.st(peer)
        if kind == S_CAN_TAKE_OWNERSHIP:
            if v is not None and v.owner is not peer:
                s.can_take.add(vid)
            return
        now = time.monotonic()
        if v is None or v.owner is peer:
            self.push(peer, C_OWNERSHIP_DENIED, a=vid)
            return
        owner_gone = v.owner is None or v.owner not in self.state
        # The original server ignores a request from a client that did not announce CanTakeOwnership first (FUN_141956a20). The shim
        # makes every collision request the car (patch_traffic_request_ownership), so the flag is no longer required here.
        if not owner_gone and now - v.moved < OWNERSHIP_COOLDOWN_S:
            self.push(peer, C_OWNERSHIP_DENIED, a=vid)
            return
        old = v.owner
        if not owner_gone:
            self.state[old].owned.discard(vid)
            self.push(old, C_DROP_OWNERSHIP, a=vid)
        v.owner, v.moved = peer, now
        s.owned.add(vid)
        self.push(peer, C_TAKE_OWNERSHIP, a=vid)
        self.log(f"traffic: car {vid} handed from {old.addr[1] if old is not None else '-'} to {peer.addr[1]} (RequestOwnership)")

    def closer(self, peer, v):
        """True when the requester is clearly nearer to the car than its owner is (positions come from the cars' own updates)."""
        return False        # [guess] ownership stays with the spawner until it leaves; refine from observed sessions

    def on_update(self, peer, mtype, reader):
        """FullSync / RouteProgress / FullRoute / Stationary from the car's owner: copy to everyone else."""
        layout = UPDATE_LAYOUT[mtype]
        fields = [reader.bits(w) for w in layout]
        vid = fields[0]
        n = self.counters.get("rx_" + UPDATE_NAMES[mtype], 0) + 1
        self.counters["rx_" + UPDATE_NAMES[mtype]] = n
        if n <= 6 or n % 500 == 0:
            self.log(f"traffic: {peer.addr[1]} -> {UPDATE_NAMES[mtype]} car {vid} (#{n})")
        v = self.vehicles.get(vid)
        if not self.enabled or v is None or v.owner is not peer:
            return
        if mtype == MSG_SERVER_FULL_SYNC:
            v.full_sync = fields
        elif mtype == MSG_SERVER_FULL_ROUTE:
            v.full_route = fields
        elif mtype == MSG_SERVER_STATIONARY:
            v.stationary = fields
        for q in self.active():
            if q is not peer:
                self.emit(q, update_message(mtype, fields))

    # ------------------------------------------------------------ timers
    def tick(self, now):
        for peer, s in list(self.state.items()):
            # A big burst of CarModelLoaded long after the join means the client just went (back) online, e.g. after a fast
            # travel (FUN_140726230 reports every loaded model at once). While a session starts the models trickle in one or
            # two at a time instead, which must not trigger a resync.
            if s.burst_at is not None and now - s.burst_at > 0.3:
                n, s.burst_n, s.burst_at = s.burst_n, 0, None
                if n >= 4 and now - s.joined_at > 25.0:
                    self.log(f"traffic: {peer.addr[1]} reported {n} loaded models at once; repeating the join sequence")
                    self.join(peer, force=True)
            for vid, t in list(s.pending.items()):
                if now - t > GRANT_TIMEOUT_S:
                    s.pending.pop(vid)
                    self.log(f"traffic: grant of car {vid} to {peer.addr[1]} timed out")
                    self.push(peer, C_SPAWN_CANCELED, a=vid)
            if s.wanted and len(s.owned) + len(s.pending) < s.limit:
                self.grant(peer)
