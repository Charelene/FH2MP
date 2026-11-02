"""The game's Traffic::TrafficManager, ported: which player holds which civilian car, and who owns it. Pure logic, no sockets; traffic.py does the wire.

Holding is separate from owning. A player is sent a car (AddTraffic) when it comes inside the radius the player reports, and loses it (RemoveTraffic)
when pushed out or when that player culls it; the car lives on for the others. The owner is whichever holder simulates it.
Addresses are build 1.0.0.64. Read in the exe except where marked [I]: the exe only holds the in-process offline server, so its network sends are vtable slots
and the handover below is inferred from OnRequestOwnership, which does Drop and Take itself."""

import diag

LOG = diag.get("traffic")
_RATE = diag.RateLimit()
EXHAUSTED_LOG_SECONDS = 30.0


def _name(key):
    """How a player is named in a log line: the port traffic.py uses for peers, the key itself otherwise."""
    addr = getattr(key, "addr", None)
    return addr[1] if addr else key


MAX_CARS = 216               # the loops in 0x1957e00 and 0x19580c0 stop at 0xd8
HARD_CAP = 18                # AssignVehicleToPlayer 0x1955080
BASE_CAPACITY = 10           # helper_1956690: 10 + car models in use - cars held
EVICT_MARGIN = 100.0         # squared metres a new car must beat the farthest held one by (0x1957e00)
OWNER_BONUS = 10             # Rebalance 0x1955210: score = distance, -10 for the owner ...
RECENT_MS = 10000.0          # ... and up to -100 more for a change in the last 10 s
RECENT_FACTOR = 0.01
CHANGE_GAP_MS = 2000.0       # IsVehicleOwnershipStale 0x1955e10, CanChangeOwner 0x1955d40
ROUND_BUDGET, CHANGE_COST = 10, 5    # Rebalance hands each player at most 2 cars a round
DEFAULT_RADIUS = 250         # Densities spawn-out; the client sends its own with most messages (helper_71ef00)


def dist2(a, b):
    dx, dy, dz = a[0] - b[0], a[1] - b[1], a[2] - b[2]
    return dx * dx + dy * dy + dz * dz


class Player:
    __slots__ = ("key", "universe", "pos", "radius", "models", "held", "can_take", "removing", "granted", "canceled")

    def __init__(self, key):
        self.key, self.universe, self.pos, self.radius = key, -1, None, DEFAULT_RADIUS
        self.models = {}             # loaded model -> cars of it held (player+0x90)
        self.held = set()            # matrix +0x8f50
        self.can_take = set()        # matrix +0x9970: AddTraffic done on the client, it could simulate the car
        self.removing = set()        # told RemoveTraffic, no TrafficRemoved yet (player+0xa0)
        self.granted = set()         # ids granted, not spawned yet (player+0x48)
        self.canceled = set()        # ids canceled while the client may be spawning them (player+0x60)

    def capacity(self):
        return BASE_CAPACITY + sum(1 for n in self.models.values() if n > 0) - len(self.held)

    def settled(self):
        """Every held car is ready on this client; only then may it spawn or take cars."""
        return self.held <= self.can_take


class Car:
    __slots__ = ("vid", "model", "universe", "info", "owner", "pos", "holders", "changed", "reserved")

    def __init__(self, vid, model, universe, info, owner, now):
        self.vid, self.model, self.universe, self.info, self.owner = vid, model, universe, info, owner
        self.pos = None              # unknown until the owner's first update (all zero at +0x8a0)
        self.holders = set()         # count at +0x888
        self.changed = now           # +0x880 and +0x884
        self.reserved = False


class Out:
    """The vtable slots of TrafficManager: what goes to a client. traffic.py implements these."""
    def add(self, key, car): pass
    def remove(self, key, vid): pass
    def granted(self, key, vid): pass
    def canceled(self, key, vid): pass
    def withdrawn(self, key): pass
    def take(self, key, vid): pass
    def drop(self, key, vid): pass
    def denied(self, key, vid): pass


class TrafficManager:
    def __init__(self, out):
        self.out = out
        self.players = {}
        self.cars = {}
        self.reserved = {}           # vid -> key: granted, not spawned yet (+0x88c)
        self.requests = {}           # universe -> [[key, count], ...] in arrival order (queue +0x8)
        self.placing = {}            # vid -> key: spawned, no position yet (queue +0x38), blocks spawns nearby
        self.refusal = ""            # why the last spawned() / request_ownership() said no, for the caller's log line

    def join(self, key):
        return self.players.setdefault(key, Player(key))

    def leave(self, key):
        p = self.players.get(key)
        if p is not None:
            self._release(p, silent=True)
            del self.players[key]

    def reset(self, key):
        """The client threw its traffic away (back online after a fast travel): nothing to tell it, the tick adds everything again."""
        p = self.players.get(key)
        if p is not None:
            self._release(p, silent=True)
            p.removing.clear()

    def set_position(self, key, pos):
        self.players[key].pos = pos

    def set_radius(self, key, radius):
        if radius > 0:
            self.players[key].radius = radius

    def set_universe(self, key, universe):
        """SetPlayerUniverse 0x1954e60: a player leaving a universe loses everything there."""
        p = self.players[key]
        if p.universe != universe:
            if p.universe != -1:
                self._release(p, silent=False)
            p.universe = universe

    def model_loaded(self, key, model):
        self.players[key].models.setdefault(model, 0)

    def model_unloaded(self, key, model):
        self.players[key].models.pop(model, None)

    def request(self, key, count):
        """OnRequestTraffic 0x19578a0: ignored while earlier grants are unanswered."""
        p = self.players[key]
        if p.granted or p.universe == -1:
            LOG.debug(f"request from {_name(key)} for {count} car(s) ignored: " + (f"{len(p.granted)} grant(s) not answered yet" if p.granted else "it is in no universe yet"))
            return
        p.canceled.clear()
        self.requests.setdefault(p.universe, []).append([key, count])

    def withdraw(self, key):
        """OnWithdrawTrafficRequest 0x1958470. The ids go on the canceled list: the client may already be spawning them."""
        p = self.players[key]
        for vid in p.granted:
            self.reserved.pop(vid, None)
        p.canceled |= p.granted
        p.granted.clear()
        self._unqueue(key)
        self.out.withdrawn(key)

    def withdraw_one(self, key, vid):
        """A grant the client never answered."""
        p = self.players[key]
        p.granted.discard(vid)
        p.canceled.add(vid)
        self.reserved.pop(vid, None)

    def spawned(self, key, vid, model, info, now):
        """helper_19579a0, the handler SendTrafficSpawned reaches."""
        p = self.players[key]
        if vid in p.canceled:
            p.canceled.discard(vid)
            self.refusal = "the grant was cancelled before the spawn arrived"
            return False
        if vid not in p.granted:
            self.refusal = "no grant is outstanding for that id" + (" (a car with that id exists already)" if vid in self.cars else "")
            return False
        p.granted.discard(vid)
        self.reserved.pop(vid, None)
        car = self.cars[vid] = Car(vid, model, p.universe, info, key, now)
        self._hold(p, car)
        p.can_take.add(vid)
        if len(p.held) > HARD_CAP:
            far, _ = self._farthest(p)
            if far is not None:
                self._drop_car(p, self.cars[far], False, now)
        self.placing[vid] = key
        return True

    def removed(self, key, vid):
        """OnTrafficRemoved 0x1957680: only clears that player's pending entry."""
        self.players[key].removing.discard(vid)

    def despawn(self, key, vid, now):
        """OnRequestDespawn 0x1956390: the car leaves this player only."""
        p, car = self.players[key], self.cars.get(vid)
        if car is not None and vid in p.held:
            self._drop_car(p, car, False, now)

    def can_take(self, key, vid):
        """OnCanTakeOwnership 0x1955d80."""
        p = self.players[key]
        if vid in self.cars and vid in p.held:
            p.can_take.add(vid)

    def request_ownership(self, key, vid, now):
        """OnRequestOwnership 0x1956a20, except that the retail flag check is gone: the shim requests a car on every collision, and that
        is the only thing that sends this."""
        p, car = self.players[key], self.cars.get(vid)
        if car is None or car.owner == key or vid not in p.held:
            self.refusal = "no such car" if car is None else "the player owns it already" if car.owner == key else "the player does not hold it"
            self.out.denied(key, vid)
            return False
        if now - car.changed <= CHANGE_GAP_MS:
            self.refusal = f"its owner changed {now - car.changed:.0f} ms ago, minimum {CHANGE_GAP_MS:.0f}"
            self.out.denied(key, vid)
            return False
        self._hand_over(car, key, now)
        return True

    def car_position(self, key, vid, pos):
        car = self.cars.get(vid)
        if car is not None and car.owner == key and any(pos):
            car.pos = pos

    def holders(self, vid):
        car = self.cars.get(vid)
        return car.holders if car is not None else ()

    def tick(self, now):
        """Update 0x1958340, per universe: holders, ownership, then spawns."""
        universes = {p.universe for p in self.players.values() if p.universe != -1}
        for universe in universes:
            players = [p for p in self.players.values() if p.universe == universe]
            cars = [c for c in self.cars.values() if c.universe == universe]
            self._update_holders(players, cars, now)
            self._rebalance(players, cars, now)
            self._grant(universe)

    def _update_holders(self, players, cars, now):
        """UpdatePerFrame 0x1957e00."""
        for p in players:
            if p.pos is None:
                continue
            reach = float(p.radius) * p.radius
            for car in cars:
                vid = car.vid
                if car.pos is None or vid in p.held or vid in p.removing or car.model not in p.models or vid not in self.cars:
                    continue                # the last check: an eviction just above can end a car that is still in this list
                d = dist2(car.pos, p.pos)
                if d >= reach:
                    continue
                if p.capacity() < 1 and p.models[car.model]:
                    far, far_d = self._farthest(p)
                    if -EVICT_MARGIN + far_d <= d:
                        continue
                    self._drop_car(p, self.cars[far], False, now)
                self._hold(p, car)
                self.out.add(p.key, car)

    def _rebalance(self, players, cars, now):
        """Rebalance 0x1955210: cars with 2+ holders go to the best placed one (lowest score [I]), at most 2 per player per round."""
        by_key = {p.key: p for p in players}
        scores = []
        for car in cars:
            if len(car.holders) < 2 or now - car.changed <= CHANGE_GAP_MS:
                continue
            for key in car.holders:
                p = by_key.get(key)
                if p is None or p.pos is None or car.pos is None:
                    continue
                score = int(dist2(car.pos, p.pos) ** 0.5)
                if car.owner == key:
                    score -= OWNER_BONUS
                    age = now - car.changed
                    if age < RECENT_MS:
                        score -= int((RECENT_MS - age) * RECENT_FACTOR)
                scores.append((score, car.vid, key))
        # Score, then car; never the player: keys are Peer objects, and two holders of one car at the same score made tick() raise TypeError
        # every round on dev (2026-11-01), so no spawn was granted while it lasted. Retail's sort (0x1953d40) leaves such ties in any order.
        scores.sort(key=lambda entry: entry[:2])
        done = set()
        while scores:
            budget = dict.fromkeys(by_key, ROUND_BUDGET)
            rest = []
            for entry in scores:
                _, vid, key = entry
                if budget[key] < CHANGE_COST:
                    rest.append(entry)
                elif vid not in done:
                    car = self.cars[vid]
                    if car.owner != key:
                        if not by_key[key].settled():
                            continue
                        self._hand_over(car, key, now)
                    done.add(vid)
                    budget[key] -= CHANGE_COST
            scores = rest

    def _grant(self, universe):
        """helper_19580c0."""
        for vid in [v for v in self.placing if (c := self.cars.get(v)) is None or c.pos is not None]:
            del self.placing[vid]
        queue = self.requests.get(universe)
        if not queue:
            return
        i = 0
        while i < len(queue):
            entry = queue[i]
            p = self.players.get(entry[0])
            if p is None or not self._may_spawn(p):
                i += 1
                continue
            vid = next((v for v in range(MAX_CARS) if v not in self.cars and v not in self.reserved), None)
            if vid is None:
                ok, skipped = _RATE.allow("exhausted", EXHAUSTED_LOG_SECONDS)
                if ok:
                    LOG.warn(f"no free car id: all {MAX_CARS} are in use ({len(self.cars)} cars, {len(self.reserved)} granted and not spawned), "
                             f"{sum(len(q) for q in self.requests.values())} request(s) wait" + (f" (+{skipped} more since the last line)" if skipped else ""))
                return
            entry[1] -= 1
            if entry[1] <= 0:
                del queue[i]
            self.reserved[vid] = p.key
            p.granted.add(vid)
            self.out.granted(p.key, vid)

    def _may_spawn(self, p):
        """helper_1955b30: settled, and nobody else mid-spawn close enough that the two would see each other's cars appear."""
        if not p.settled() or p.pos is None:
            return False
        others = {key for key in self.placing.values()} | {key for key in self.reserved.values()}
        for key in others:
            q = self.players.get(key)
            if q is None or q is p or q.pos is None:
                continue
            if dist2(q.pos, p.pos) <= float(q.radius) * q.radius + float(p.radius) * p.radius:
                return False
        return True

    def _farthest(self, p):
        """RebalanceByDistance 0x1956720: the farthest held car, sparing a player's last car of a model."""
        best, best_d = None, -1.0
        for vid in p.held:
            car = self.cars[vid]
            if car.pos is not None and p.models.get(car.model, 0) > 1:
                d = dist2(car.pos, p.pos)
                if best_d < d:
                    best, best_d = vid, d
        return best, best_d

    def _hold(self, p, car):
        p.held.add(car.vid)
        p.models[car.model] = p.models.get(car.model, 0) + 1
        car.holders.add(p.key)

    def _drop_car(self, p, car, silent, now):
        """RemoveVehicleFromPlayer 0x1957130: the owner leaving passes the car on (0x1956430)."""
        p.held.discard(car.vid)
        p.can_take.discard(car.vid)
        car.holders.discard(p.key)
        if car.model in p.models:
            p.models[car.model] -= 1
        if not silent:
            p.removing.add(car.vid)
            self.out.remove(p.key, car.vid)
        if car.owner == p.key:
            car.owner = None
            next_owner = self._next_owner(car)
            if next_owner is not None:
                car.owner = next_owner
                car.changed = now
                self.out.take(next_owner, car.vid)
        if not car.holders:
            del self.cars[car.vid]
            self.placing.pop(car.vid, None)

    def _next_owner(self, car):
        """0x1956430: a holder with every car ready first, then any holder with this one ready."""
        holders = [q for q in self.players.values() if q.key in car.holders]        # in slot order, as retail walks them
        for q in holders:
            if q.settled():
                return q.key
        for q in holders:
            if car.vid in q.can_take:
                return q.key
        return None

    def _hand_over(self, car, key, now):
        if car.owner is not None:
            self.out.drop(car.owner, car.vid)
        car.owner, car.changed = key, now
        self.out.take(key, car.vid)

    def _release(self, p, silent):
        """ReleasePlayerState 0x1956db0."""
        for vid in p.granted:
            self.reserved.pop(vid, None)
            if not silent:
                self.out.canceled(p.key, vid)
        p.granted.clear()
        for vid in list(p.held):
            car = self.cars[vid]
            self._drop_car(p, car, silent, car.changed)
        self._unqueue(p.key)
        for vid in [v for v, k in self.placing.items() if k == p.key]:
            del self.placing[vid]

    def _unqueue(self, key):
        for queue in self.requests.values():
            queue[:] = [e for e in queue if e[0] != key]
