"""Scoreboards (Rivals / leaderboards) for the LSP stand-in.

Stores the scores players submit (Scoreboard.SubmitScore / SubmitXpScore) and serves them back through
GetBestScore / GetRival / GetRowsAnthem. Boards are keyed (scoreboardType, scoreType, trackId); `score`
is a big-endian IEEE double carried as a u64 on the wire. Direction (lower is better for times, higher
for everything else) is a guess by scoreType, see ASCENDING. Boards can be topped up with deterministic
fake rivals (drawn from the Drivatar names) when FH2MP_FAKE_ROWS > 0 (default 0: real players only); they are marked in the log only.
Persistence: score_submissions / player_xp tables (db.py, PostgreSQL); best-per-player is a query, not a cached table.
"""
import db
import hashlib
import os
import math
import struct
import time
import xp_audit

ASCENDING = {1}          # scoreTypes where a smaller score wins (lap/race times) [guess]
XP_BOARD = (4, 9)        # (scoreboardType, scoreType) of the XP ladder [guess from SubmitXpScore traffic]
FAKE_ROWS = int(os.environ.get("FH2MP_FAKE_ROWS", "0"))   # synthesized rivals topping up a board; 0 = real players only
CLASS_AVG_KMH = (88, 103, 119, 134, 150, 166)   # km/h; first race data: A-class Huracan averaged ~144 km/h on route 8001
EVENT_BOARDS = {3, 6}   # scoreboardType whose pivotId is the rival event id (Monthly = 3, Showcase = 6) and SubmitScore.gameScoreboardId matches it [C]
MAXED_CLASSES = {6, 7}  # X, R: classes above S2 the client reports for maxed-out cars; never allowed on a Rivals board (user decision)
CLASS_PIVOT_BOARDS = {1}   # scoreboardType whose pivotId is the car class (Festival Routes rivals: pivot 0..5 = D..S2) [inferred from GetRival calls]
SPEED_BOARDS = {9}       # scoreboardType of the speed trap / speed zone boards: score = m/s (32.565 shows as 117.23 km/h)
GSID_BOARDS = {9}        # scoreboardType with one board per trap/zone: GetRows pivotId == SubmitScore.gameScoreboardId (scoreType 5 traps ids 1-30,
                         # scoreType 6 zones ids 36-50 in the live data) [C]. Without this every trap and zone shared one board per player.
SPEED_CAP = 127.4        # m/s (285 mph): highest speed a trap can really show (Ultima GTR with bounce wheelies; Sisteron Highway record 283 mph,
                         # GitHub #12). Faster speed-board scores are rejected on submit and hidden on read (older ones are still stored).


def dbl_to_u64(x):
    return struct.unpack(">Q", struct.pack(">d", x))[0]


def u64_to_dbl(u):
    return struct.unpack(">d", struct.pack(">Q", u))[0]


def iso(t):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))


try:
    from rival_records import RECORDS as WORLD_RECORDS    # (track id, class id) -> record seconds, from tools/make_rival_records.py
except ImportError:
    WORLD_RECORDS = {}
WR_FLOOR = 1.005         # made-up rival times stay at least this far above a world record
WR_SPREAD = 0.12         # a made-up rival that would have beaten the record lands 0-12% above it instead (not all on the same time)


class ScoreStore:
    def __init__(self, db, names, cars=None, route_length=None, event_info=None, event_cars=None, records=None):
        self.db = db
        self.records = WORLD_RECORDS if records is None else records
        self.route_length = route_length or (lambda track: None)   # track id -> metres or None
        self.event_info = event_info or (lambda event: None)       # rival event id -> {"track","laps","cls"} or None
        self.event_cars = event_cars or (lambda event: None)       # rival event id -> [(car id, class, PI, top m/s)] the event allows
        self.names = list(dict.fromkeys(n for n in names if n)) or ["Rival"]   # unique, order kept
        self.cars = sorted(cars or [], key=lambda c: c[2])   # (carId, class, pi, top m/s) from the game database, by PI

    def count(self):
        return self.db.one("SELECT COUNT(*) FROM score_submissions")[0]

    # ---- writes
    def submit(self, xuid, tag, score_type, d):
        """Stores one submission and returns it as a row dict, or None if the score is not a finite number
        (a NaN/inf time must never reach a public board) or the car is class X/R on a Rivals board (user
        decision: a maxed-out car must never appear on a Rivals leaderboard, not even disguised as S2), or a speed above SPEED_CAP."""
        score = u64_to_dbl(d["score"])
        if not math.isfinite(score):
            return None
        if d["scoreboardType"] in SPEED_BOARDS and score > SPEED_CAP:
            return None
        if d["scoreboardType"] in EVENT_BOARDS and d["carClassId"] in MAXED_CLASSES:
            return None
        now = time.time()
        score_id = os.urandom(16)
        with self.db.tx() as c:
            db.touch_player(c, xuid, tag, now)
            cur = c.execute("INSERT INTO score_submissions (xuid, sb, st, gsid, track, car, cls, cpi, score, clean, stm, tcs, "
                            "abs, autobrake, autoshift, clutch, submitted, score_id) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                            (xuid, d["scoreboardType"], score_type, d["gameScoreboardId"], d["trackId"], d["carId"],
                             d["carClassId"], d["carPerformanceIndex"], score, int(d["isClean"]), int(d["usedSTM"]),
                             int(d["usedTCS"]), int(d["usedABS"]), int(d.get("usedAutoBrake", False)),
                             int(d.get("usedAutoShifting", False)), int(d.get("usedClutch", False)), now, score_id))
        return {"id": cur.lastrowid, "score_id": score_id, "xuid": xuid, "tag": tag, "sb": d["scoreboardType"],
                "st": score_type, "score": score}

    def attach_ghost(self, score_id, data):
        """Stores the ghost the client uploads for a SubmitScore (matched by the scoreId we issued). Returns the
        submission id, or None if the id is unknown."""
        with self.db.tx() as c:
            r = c.execute("SELECT id FROM score_submissions WHERE score_id=?", (bytes(score_id),)).fetchone()
            if r is None:
                return None
            c.execute("INSERT INTO score_ghosts (submission_id, data, uploaded) VALUES (?,?,?) ON CONFLICT (submission_id) "
                      "DO UPDATE SET data=excluded.data, uploaded=excluded.uploaded",
                      (r["id"], bytes(data), time.time()))
            return r["id"]

    def by_score_id(self, score_id):
        """The stored submission a client refers to by the scoreId we issued (None if unknown)."""
        r = self.db.one("SELECT s.*, COALESCE(p.gamertag, '') AS tag, 0 AS ghost FROM score_submissions s "
                        "LEFT JOIN players p ON p.xuid = s.xuid WHERE s.score_id=?", (bytes(score_id),))
        return _row(r) | {"id": r["id"]} if r else None

    def ghost(self, ghost_id):
        r = self.db.one("SELECT data FROM score_ghosts WHERE submission_id=?", (ghost_id,))
        return bytes(r["data"]) if r else None

    def add_xp(self, xuid, xp):
        """Records the award in the XP ledger and, when xp_audit accepts it, adds it to the total (atomic increment, so concurrent
        submissions cannot lose an update). Returns (total, verdict, reason)."""
        verdict, reason = xp_audit.judge(xp)
        if verdict == xp_audit.OK and xp == 0:
            return self.xp_of(xuid), verdict, reason        # nothing to add, nothing worth a ledger row
        now = time.time()
        with self.db.tx() as c:
            db.touch_player(c, xuid, "", now)
            c.execute("INSERT INTO xp_awards (xuid, xp, at, verdict, reason) VALUES (?,?,?,?,?)",
                      (xuid, xp if math.isfinite(xp) else 0.0, now, verdict, reason))
            if verdict != xp_audit.OK:
                r = c.execute("SELECT xp FROM player_xp WHERE xuid=?", (xuid,)).fetchone()
                return (r[0] if r else 0.0), verdict, reason
            total = c.execute("INSERT INTO player_xp (xuid, xp, updated) VALUES (?,?,?) ON CONFLICT(xuid) "
                              "DO UPDATE SET xp = player_xp.xp + excluded.xp, updated = excluded.updated RETURNING xp",
                              (xuid, xp, now)).fetchone()[0]
            return total, verdict, reason

    def xp_of(self, xuid):
        r = self.db.one("SELECT xp FROM player_xp WHERE xuid=?", (xuid,))
        return r[0] if r else 0.0

    # ---- reads
    def better(self, st, a, b):
        return a < b if st in ASCENDING else a > b

    def _best_per_player(self, sb, st, track, xuid=None, pivot=-1):
        """Each player's best submission on a board (any track when `track` is -1/65535), as row dicts keyed by
        xuid. The XP ladder also takes the accumulated XP when that beats a submitted score."""
        asc = st in ASCENDING
        where, args = "s.sb=? AND s.st=?", [sb, st]
        if sb in EVENT_BOARDS:    # class X/R must never surface on a Rivals board, read or write (user decision); belt-and-suspenders
            where += f" AND s.cls NOT IN ({','.join(str(c) for c in MAXED_CLASSES)})"
        if sb in SPEED_BOARDS:    # impossible speeds submitted before the cap existed stay stored but never show
            where += f" AND s.score <= {SPEED_CAP}"
        if (sb in EVENT_BOARDS or sb in GSID_BOARDS) and pivot > 0:   # one board per event / per trap or zone (the client's track field is 0, the track or 65535)
            where += " AND s.gsid=?"
            args.append(pivot)
        elif track not in (-1, 65535):
            where += " AND s.track=?"
            args.append(track)
        if xuid is not None:
            where += " AND s.xuid=?"
            args.append(xuid)
        if sb in CLASS_PIVOT_BOARDS and 0 <= pivot <= 5:     # one board per car class (the submission's carClassId)
            where += " AND s.cls=?"
            args.append(pivot)
        # ties go to the earlier submission (lowest id), as the in-memory scan did
        sql = ("SELECT * FROM (SELECT s.*, COALESCE(p.gamertag, '') AS tag, "
               "CASE WHEN g.submission_id IS NULL THEN 0 ELSE s.id END AS ghost, ROW_NUMBER() OVER "
               f"(PARTITION BY s.xuid ORDER BY s.score {'ASC' if asc else 'DESC'}, s.id) AS rn "
               "FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid "
               f"LEFT JOIN score_ghosts g ON g.submission_id = s.id WHERE {where}) ranked WHERE rn = 1")
        best = {r["xuid"]: _row(r) for r in self.db.query(sql, args)}
        if (sb, st) == XP_BOARD:
            q = "SELECT x.xuid, x.xp, COALESCE(p.gamertag, '') AS tag FROM player_xp x LEFT JOIN players p ON p.xuid = x.xuid"
            for r in self.db.query(q + (" WHERE x.xuid=?" if xuid is not None else ""), (xuid,) if xuid is not None else ()):
                cur = best.get(r["xuid"])
                if cur is None or r["xp"] > cur["score"]:
                    best[r["xuid"]] = {"xuid": r["xuid"], "tag": r["tag"], "sb": sb, "st": st, "track": track, "car": 0,
                                       "cls": 0, "cpi": 0.0, "score": r["xp"], "clean": True, "stm": False,
                                       "tcs": False, "abs": False, "t": time.time()}
        return best

    def best_of(self, xuid, sb, st, track, pivot=-1):
        return self._best_per_player(sb, st, track, xuid, pivot).get(xuid)

    def top(self, sb, st, track=-1, pivot=-1, limit=25):
        """Real players only (no synthesized rivals), best first, each row with its `position`. For the website's public boards."""
        rows = sorted(self._best_per_player(sb, st, track, pivot=pivot).values(),
                      key=lambda r: (r["score"] if st in ASCENDING else -r["score"], r["t"]))
        for i, r in enumerate(rows):
            r["position"] = i + 1
        return rows[:max(0, limit)]

    FAKE_BASE = 0x9100000000000
    TARGET_XUID = FAKE_BASE + 0xFFFFFF     # the empty board's world record to beat (rival_for); outside every fake_xuid value
    TARGET_TAG = "World Record"            # not a person: the community record time, never a player's name

    @classmethod
    def fake_xuid(cls, name):
        return cls.FAKE_BASE + (hash_int(name) % 0xFFFFFF)

    def fake_name(self, xuid):
        """The racing-driver name behind a fake rival's id ('' if the id is not one): the profile service needs it, the game shows what it says."""
        if xuid < self.FAKE_BASE or xuid > self.FAKE_BASE + 0xFFFFFF:
            return ""
        if xuid == self.TARGET_XUID:
            return self.TARGET_TAG
        if not hasattr(self, "_fake_names"):
            self._fake_names = {self.fake_xuid(n): n[:15] for n in self.names}
        return self._fake_names.get(xuid, "")

    def _fake(self, sb, st, track, anchor, n, pivot=-1):
        """Deterministic fake rivals around `anchor` (the player's best, or a default scale)."""
        out = []
        ev_cars = self.event_cars(pivot) if sb in EVENT_BOARDS and pivot > 0 else None   # rivals drive cars the event allows
        # Festival Route class boards (pivot = class D..S2): no made-up time may beat the world record for that track and class
        wr = self.records.get((track, pivot)) if (st in ASCENDING and sb in CLASS_PIVOT_BOARDS and 0 <= pivot <= 5) else None
        for i in range(n):
            nm = self.names[(i * 7 + (hash_int(sb, st, track, pivot) % len(self.names))) % len(self.names)]
            r = hash_frac(sb, st, track, pivot, nm, i)
            if st in ASCENDING:
                length = None if track in (-1, 0, 65535) else self.route_length(track)
                ev = self.event_info(pivot) if sb in EVENT_BOARDS and pivot > 0 else None
                if ev:
                    length = (self.route_length(ev["track"]) or 0) * ev["laps"] or length
                if anchor:
                    score = anchor * (0.6 + 0.9 * r)            # 60% .. 150% of the player's own time
                elif length:
                    # average speed over a lap by car class (D..S2), km/h, incl. corners [guess]; the route is
                    # a polyline of waypoints, so add 10% for the road between them
                    cls_ = (ev["cls"] if ev["cls"] is not None else 4) if ev else (pivot if (sb in CLASS_PIVOT_BOARDS and 0 <= pivot <= 5) else 4)   # open events: assume S1-ish
                    score = length * 1.1 / (CLASS_AVG_KMH[cls_] / 3.6) * (0.75 + 0.6 * r)
                else:
                    score = 100.0 * (0.6 + 0.9 * r)
                if wr and score < wr * WR_FLOOR:
                    score = wr * WR_FLOOR * (1.0 + WR_SPREAD * hash_frac(sb, st, track, pivot, nm, i, "wr"))
            elif sb in SPEED_BOARDS:
                base = anchor if anchor else 50.0               # about 180 km/h
                score = min(102.0, max(20.0, base * (0.7 + 0.8 * r) if anchor else 35.0 + 45.0 * r))   # m/s, capped at ~367 km/h
            else:
                base = anchor if anchor else 20000.0
                score = base * (0.05 + 3.0 * r * r)             # long tail of lower scores, a few above
            car, cls, cpi = self._fake_car(nm, score if sb in SPEED_BOARDS else None, ev_cars)
            out.append({"xuid": self.fake_xuid(nm), "tag": nm[:15], "sb": sb, "st": st,
                        "track": track, "car": car, "cls": cls, "cpi": cpi, "score": round(score, 3), "clean": r > 0.3,
                        "stm": False, "tcs": r < 0.5, "abs": True, "autoshift": r < 0.6, "t": time.time() - 86400 * (1 + 20 * r), "fake": True})
        return out

    def bounty_for(self, track, position=1):
        """Credits for beating a rival on `track` (the "REWARDS" box): scales with the route length and with how high
        the rival ranks [guess; the real formula is Turn 10's]."""
        length = None if track in (-1, 0, 65535) else self.route_length(track)
        km = (length or 3000.0) / 1000.0
        return int(round(km * 400 * (1.0 + 1.0 / max(1, position)), -1))

    def _fake_car(self, nm, speed=None, pool=None):
        """A real car for a fake row (so the page shows its name, not "DLC Car"). For speed boards the car is
        chosen by performance index so the speed is plausible for its class: roughly
        km/h = 40 + 320 * PI^1.5 (D ~150-190, A ~230-270, S2 ~300+). The database's own top-speed column is
        not usable on its own (a '63 Corvette Forza Edition is listed at 362 km/h) [inferred from spot checks]."""
        cars = pool or self.cars
        if not cars:
            return 100 + hash_int(nm, "car") % 500, hash_int(nm, "c") % 6, 0.3 + 0.6 * hash_frac(nm, "cpi")
        if speed is None:
            c = cars[hash_int(nm, "car") % len(cars)]
            return c[0], c[1], c[2]
        kmh = speed * 3.6
        target = min(1.0, max(0.0, (kmh - 40.0) / 320.0)) ** (2.0 / 3.0)
        # also cap by the car's listed top speed: those are unlimited-gearing values, ~10% above real stock
        # figures (Veyron SS 431 = real, GT86 253 vs ~225), so a trap reading stays under 85% of it [inferred]
        ok = [c for c in cars if c[3] * 0.85 >= speed] or cars[-8:]
        near = sorted(ok, key=lambda c: abs(c[2] - target))[:8]
        c = near[hash_int(nm, "car", round(speed)) % len(near)]
        return c[0], c[1], c[2]

    def rows(self, xuid, sb, st, track, start_at, max_results, pivot=-1):
        """Sorted best-per-player rows (real + fake) and the requesting player's row."""
        best = self._best_per_player(sb, st, track, pivot=pivot)
        me = best.get(xuid)
        real = list(best.values())
        fake = self._fake(sb, st, track, me["score"] if me else None, max(0, FAKE_ROWS - len(real)), pivot)
        allrows = real + fake
        allrows.sort(key=lambda s: s["score"], reverse=(st not in ASCENDING))
        for i, s in enumerate(allrows):
            s["position"] = i + 1
        mine = next((s for s in allrows if s["xuid"] == xuid), None)
        page = allrows[max(0, start_at - 1):max(0, start_at - 1) + max_results]
        return page, mine, len(real), len(fake)

    def rival_for(self, xuid, sb, st, track, pivot=-1, rival_xuid=0):
        """The rival to chase: the row of `rival_xuid` when the player picked one on the board (GitHub #12), else the row just above
        the player's best; with no best yet, one near the middle of the board. A real player's row (it has a ghost to race) is
        preferred over a made-up one: for a newcomer any real ghost on the board, otherwise one within 4 rows above the player's
        best. The player's own row is the rival when they are #1 (nobody is above them, so they race their own best ghost) or when
        the client asks for their own xuid (picking yourself on the board); the caller pays no bounty for it. On an empty board it
        is the world record ("World Record", no ghost, never listed on the board; see record_target), or None when there is no
        record: GetRival has no "not found" flag, and the client spins forever on an all-zero answer (GitHub #12)."""
        rows, mine, _, _ = self.rows(xuid, sb, st, track, 1, 1000, pivot)
        if not rows:
            return self._record_row(sb, st, track, pivot)
        picked = next((r for r in rows if rival_xuid and r["xuid"] == rival_xuid), None)
        if picked is not None:
            return picked
        if mine is None:
            mid = len(rows) // 2
            return self._near_real(rows, mid, xuid, len(rows)) or rows[mid]      # any real ghost on the board beats a made-up rival
        above = [r for r in rows if r["position"] < mine["position"]]
        if not above:
            return mine
        return self._near_real(rows, above[-1]["position"] - 1, xuid, 4, only_above=mine["position"]) or above[-1]

    def record_target(self, sb, st, track, pivot=-1):
        """(seconds, class) of the world record an empty time board is raced against, or None. Festival Routes: the record of
        the board's class. Monthly (always 1 lap, rivals.py): the event's track and class. Showcase / open events: the track's fastest
        class. A class without a record on that track takes the next faster class that has one (never a slower class's time)."""
        if st not in ASCENDING:
            return None
        cls = None
        if sb in CLASS_PIVOT_BOARDS and 0 <= pivot <= 5:
            cls = pivot
        elif sb in EVENT_BOARDS and pivot > 0:
            ev = self.event_info(pivot)       # Monthly only; Showcase pivots are the game's own event ids (request track, any class)
            if ev:
                track, cls = ev["track"], ev["cls"]
        on_track = {c: t for (tr, c), t in self.records.items() if tr == track}
        usable = [c for c in on_track if cls is None or c >= cls]
        if not usable:
            return None
        c = max(usable) if cls is None else min(usable)
        return on_track[c], c

    def _record_row(self, sb, st, track, pivot):
        target = self.record_target(sb, st, track, pivot)
        if target is None:
            return None
        secs, cls = target
        pool = (self.event_cars(pivot) if sb in EVENT_BOARDS and pivot > 0 else None) or self.cars
        car, ccls, cpi = self._fake_car(self.TARGET_TAG, None, [c for c in pool if c[1] == cls] or pool)   # a car of the record's class
        return {"xuid": self.TARGET_XUID, "tag": self.TARGET_TAG, "sb": sb, "st": st, "track": track, "car": car, "cls": ccls,
                "cpi": cpi, "score": secs, "clean": True, "stm": False, "tcs": False, "abs": False, "t": time.time(),
                "position": 1, "fake": True}

    @staticmethod
    def _near_real(rows, idx, xuid, radius, only_above=None):
        """Closest real (non-fake) row with a ghost within `radius` rows of index `idx`, never the player's own row."""
        best = None
        for j in range(max(0, idx - radius), min(len(rows), idx + radius + 1)):
            r = rows[j]
            if r.get("fake") or r["xuid"] == xuid or not r.get("ghost"):
                continue
            if only_above is not None and r["position"] >= only_above:
                continue
            if best is None or abs(j - idx) < abs(best[0] - idx):
                best = (j, r)
        return best[1] if best else None


def hash_int(*parts):
    return int.from_bytes(hashlib.sha1("|".join(str(p) for p in parts).encode()).digest()[:6], "big")


def hash_frac(*parts):
    return hash_int(*parts) / float(1 << 48)


def _row(r):
    """score_submissions row (+ joined tag) -> the dict the rest of the server uses."""
    return {"xuid": r["xuid"], "tag": r["tag"], "sb": r["sb"], "st": r["st"], "gsid": r["gsid"], "track": r["track"],
            "car": r["car"], "cls": r["cls"], "cpi": r["cpi"], "score": r["score"], "clean": bool(r["clean"]),
            "stm": bool(r["stm"]), "tcs": bool(r["tcs"]), "abs": bool(r["abs"]), "autobrake": bool(r["autobrake"]),
            "autoshift": bool(r["autoshift"]), "clutch": bool(r["clutch"]), "t": r["submitted"],
            "ghost": r["ghost"] if "ghost" in r.keys() else 0}
