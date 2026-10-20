"""Tests for the operator API (server/ops_api.py): python3 -I server/test_ops.py (needs psycopg 3 importable, no database server).

The stores are the real ones where that is cheap (ScoreStore on an in-memory SQLite that mimics the PostgreSQL tables the queries touch)
and small fakes elsewhere, so the access rules, the response shapes, the settings validation and the leaderboard order are checked
without a PostgreSQL instance. Statements that only exist in PostgreSQL are covered by the live smoke test in docs/ops.md.
"""
import json
import os
import sqlite3
import sys
import time
import types
import unittest
from contextlib import contextmanager

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ops_api  # noqa: E402
import scoreboard  # noqa: E402
import social  # noqa: E402

SCHEMA = """
CREATE TABLE players (xuid INTEGER PRIMARY KEY, gamertag TEXT NOT NULL DEFAULT '', career_level INTEGER NOT NULL DEFAULT 0,
                      first_seen REAL NOT NULL, last_seen REAL NOT NULL);
CREATE TABLE accounts (xuid INTEGER PRIMARY KEY, gamertag TEXT NOT NULL, created REAL NOT NULL, last_login REAL NOT NULL DEFAULT 0,
                       banned_at REAL, ban_reason TEXT NOT NULL DEFAULT '');
CREATE TABLE score_submissions (id INTEGER PRIMARY KEY AUTOINCREMENT, xuid INTEGER, sb INTEGER, st INTEGER, gsid INTEGER, track INTEGER,
    car INTEGER, cls INTEGER, cpi REAL, score REAL, clean INTEGER, stm INTEGER, tcs INTEGER, abs INTEGER, submitted REAL, score_id BLOB);
CREATE TABLE score_ghosts (submission_id INTEGER PRIMARY KEY, data BLOB, uploaded REAL);
CREATE TABLE player_xp (xuid INTEGER PRIMARY KEY, xp REAL NOT NULL, updated REAL NOT NULL);
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE auth_events (id INTEGER PRIMARY KEY AUTOINCREMENT, at REAL, kind TEXT, ip TEXT, xuid INTEGER, detail TEXT);
CREATE TABLE clubs (id INTEGER PRIMARY KEY);
"""


class FakeDb:
    """The bit of db.Database the code under test uses, on SQLite (`?` placeholders already match)."""

    def __init__(self):
        self.c = sqlite3.connect(":memory:", check_same_thread=False)
        self.c.row_factory = sqlite3.Row
        self.c.create_function("GREATEST", 2, max)
        self.c.executescript(SCHEMA)

    def one(self, sql, args=()):
        return self.c.execute(sql, tuple(args)).fetchone()

    def query(self, sql, args=()):
        return self.c.execute(sql, tuple(args)).fetchall()

    def execute(self, sql, args=()):
        return self.c.execute(sql, tuple(args))

    @contextmanager
    def tx(self):
        yield self.c
        self.c.commit()


class ApiError(Exception):
    def __init__(self, status, msg):
        super().__init__(msg)
        self.status, self.msg = status, msg


class FakeAuth:
    env, mode, registration = "test", "optional", "open"

    def __init__(self):
        self.calls, self.fails = [], 0

    def hit(self, key, limit, window):
        self.fails += 1
        return self.fails <= limit

    def stats(self):
        return {"accounts": 3, "sessions": 2, "registered_24h": 1}

    def set_banned(self, xuid, reason=None):
        self.calls.append(("ban" if reason else "unban", xuid, reason))

    def revoke(self, xuid):
        self.calls.append(("kick", xuid))
        return 2

    def add_invite(self, code, uses=1, note=""):
        self.calls.append(("invite", code, uses, note))


class FakeLauncherConfig:
    """launcher_api.LauncherConfig without the file: the same overlay rule."""
    base = {"motd": "", "maintenance": "", "minLauncher": "0.0.0", "links": {"website": "https://x.example", "patreon": "", "discord": ""}}

    def __init__(self):
        self.overlay = {}

    def get(self):
        o = self.overlay
        return {**self.base, **{k: v for k, v in o.items() if k != "links"}, "links": {**self.base["links"], **o.get("links", {})}}


class FakeRiv:
    def __init__(self, month=9):
        self.month = month
        self.rolled = 0
        self.evs = [{"id": 1000 + i, "name": f"Event {i}", "trackname": f"Track {i}", "laps": 3, "length": 4200.0, "kind": "class",
                     "description": "desc", "cls": 2, "track": i} for i in range(2)]

    def events(self):
        return self.evs

    def serial(self):
        return self.month, 0

    def names(self):
        return {"car": {7: "Test Car"}}

    def rotate(self):
        self.rolled += 1
        return self.evs


def submit(sc, xuid, tag, score, sb=3, st=1, event=1000, car=7):
    sc.submit(xuid, tag, st, {"scoreboardType": sb, "gameScoreboardId": event, "trackId": 0, "carId": car, "carClassId": 2,
                              "carPerformanceIndex": 0.5, "score": scoreboard.dbl_to_u64(score), "isClean": True, "usedSTM": False,
                              "usedTCS": False, "usedABS": True})


def make_lsp():
    database = FakeDb()
    lsp = types.SimpleNamespace(DB=database, AUTH=FakeAuth(), RIV=FakeRiv(), NOTIF=types.SimpleNamespace(count=lambda: 4),
                                UGC=types.SimpleNamespace(count=lambda: 5), xbl=types.SimpleNamespace(count=lambda: 6),
                                TAGS={1: "Alice", 2: "Bob"}, ApiError=ApiError, log=lambda m: None, announce_rotation=lambda: None,
                                online_count=lambda: 1, api=lambda m, p, b: (200, {"echo": [m, p]}))
    lsp.SB = scoreboard.ScoreStore(database, ["Fake"], cars=[(7, 2, 0.5, 80.0)])
    lsp.LAUNCHER = types.SimpleNamespace(config=FakeLauncherConfig())
    return lsp


TOKEN = "s3cret-token"
CFG = ops_api.OpsConfig(TOKEN, ["10.0.0.0/24", "127.0.0.1/32"])


def call(lsp, method, path, body=None, peer="10.0.0.5", token=TOKEN, proxied=False, cfg=CFG):
    headers = {"Authorization": f"Bearer {token}"} if token is not None else {}
    raw = json.dumps(body).encode() if body is not None else b""
    return ops_api.handle(cfg, lsp, method, path, headers, raw, peer, proxied)


class AccessTests(unittest.TestCase):
    def setUp(self):
        self.lsp = make_lsp()

    def test_disabled_without_token(self):
        st, _ = call(self.lsp, "GET", "/ops/v1/overview", cfg=ops_api.OpsConfig(""))
        self.assertEqual(st, 404)

    def test_requires_bearer_token(self):
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/overview", token=None)[0], 401)
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/overview", token="wrong")[0], 401)
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/overview")[0], 200)

    def test_address_allowlist_and_proxy_headers(self):
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/overview", peer="203.0.113.9")[0], 403)
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/overview", peer="::ffff:10.0.0.7")[0], 200)
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/overview", proxied=True)[0], 403)

    def test_wrong_tokens_are_rate_limited(self):
        codes = [call(self.lsp, "GET", "/ops/v1/overview", token="x")[0] for _ in range(12)]
        self.assertEqual(codes[:10], [401] * 10)
        self.assertEqual(codes[10:], [429, 429])

    def test_unknown_endpoint(self):
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/nope")[0], 404)
        self.assertEqual(call(self.lsp, "GET", "/ops/v2/overview")[0], 404)


def festival(sc, xuid, tag, score, track, cls=3):
    sc.submit(xuid, tag, 1, {"scoreboardType": 1, "gameScoreboardId": 0, "trackId": track, "carId": 7, "carClassId": cls,
                             "carPerformanceIndex": 0.5, "score": scoreboard.dbl_to_u64(score), "isClean": True, "usedSTM": False,
                             "usedTCS": False, "usedABS": True})


class RouteTests(unittest.TestCase):
    def setUp(self):
        self.lsp = make_lsp()
        cat = FakeDb()
        cat.c.executescript("""
            CREATE TABLE Tracks (id INTEGER, MediaName TEXT, Active INTEGER);
            CREATE TABLE v_tracks (id INTEGER, name TEXT, length_m REAL, closed INTEGER);
            INSERT INTO Tracks VALUES (856,'Anthem',1), (560,'Anthem',1), (566,'Anthem',1), (891,'Anthem',1), (578,'Anthem',1), (34,'Sebring',1), (913,'Anthem',1);
            INSERT INTO v_tracks VALUES (856,'Sisteron Citadel Circuit',1664,1), (560,'Castelletto D''Orcia Circuit',1710,1),
                (566,'Castelletto D''Orcia Circuit',1710,1), (891,'Sisteron High Speed Sprint',6463,0),
                (578,'Train in Vain Showcase',5598,0), (34,'Nurburgring',9000,1), (913,'Sisteron Abbey Arena Huge',NULL,0);""")
        self.lsp.CARS = types.SimpleNamespace(db=cat)
        now = time.time()
        for x, tag in ((1, "Alice"), (2, "Bob")):
            self.lsp.DB.execute("INSERT INTO players VALUES (?,?,?,?,?)", (x, tag, 5, now, now))
        sc = self.lsp.SB
        festival(sc, 1, "Alice", 70.5, 856)
        festival(sc, 2, "Bob", 66.0, 856)
        festival(sc, 2, "Bob", 80.0, 856)
        festival(sc, 2, "Bob", 60.0, 856, cls=0)
        festival(sc, 1, "Alice", 59.0, 560)
        festival(sc, 2, "Bob", 58.0, 566)                      # the same route under another track id
        festival(sc, 1, "Alice", 10.0, 34)                     # not a Festival Route we list

    def test_routes_grouped_by_destination(self):
        st, o = call(self.lsp, "GET", "/ops/v1/routes")
        self.assertEqual(st, 200)
        self.assertEqual([d["name"] for d in o["destinations"]], ["Castelletto", "Sisteron"])
        sis = o["destinations"][1]["routes"]
        self.assertEqual([(r["name"], r["kind"], r["circuit"]) for r in sis], [("Citadel Circuit", "Circuit", True), ("High Speed Sprint", "Sprint", False)])
        self.assertEqual(sis[0]["entries"], {"D": 1, "A": 2})
        cas = o["destinations"][0]["routes"]
        self.assertEqual((len(cas), cas[0]["id"], cas[0]["tracks"]), (1, 560, [560, 566]))

    def test_route_boards_per_class(self):
        st, o = call(self.lsp, "GET", "/ops/v1/routes/856")
        self.assertEqual((st, o["destination"], o["route"]["name"]), (200, "Sisteron", "Citadel Circuit"))
        self.assertEqual([c["class"] for c in o["classes"]], ["D", "C", "B", "A", "S1", "S2"])
        a = o["classes"][3]["rows"]
        self.assertEqual([(r["position"], r["gamertag"], r["score"]) for r in a], [(1, "Bob", 66.0), (2, "Alice", 70.5)])
        self.assertEqual([r["gamertag"] for r in o["classes"][0]["rows"]], ["Bob"])
        self.assertEqual(o["classes"][1]["rows"], [])

    def test_route_boards_merge_duplicate_track_ids(self):
        _, o = call(self.lsp, "GET", "/ops/v1/routes/560")
        self.assertEqual([(r["gamertag"], r["score"]) for r in o["classes"][3]["rows"]], [("Bob", 58.0), ("Alice", 59.0)])

    def test_unknown_route(self):
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/routes/34")[0], 404)
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/routes/abc")[0], 404)

    def test_recent_runs_newest_first(self):
        submit(self.lsp.SB, 1, "Alice", 77.0)                    # a Monthly event run
        self.lsp.DB.execute("UPDATE score_submissions SET submitted = id")      # a known order
        st, o = call(self.lsp, "GET", "/ops/v1/recent?limit=3")
        self.assertEqual(st, 200)
        runs = o["runs"]
        self.assertEqual((runs[0]["kind"], runs[0]["where"], runs[0]["score"]), ("event", "Event 0", 77.0))
        self.assertEqual((runs[1]["where"], runs[1]["gamertag"], runs[1]["score"]), ("Castelletto D'Orcia Circuit", "Bob", 58.0))
        self.assertEqual(len(runs), 3)                                # the track-34 run is skipped, not counted


class ReadTests(unittest.TestCase):
    def setUp(self):
        self.lsp = make_lsp()
        now = time.time()
        db = self.lsp.DB
        for x, tag in ((1, "Alice"), (2, "Bob"), (3, "Cara")):
            db.execute("INSERT INTO players VALUES (?,?,?,?,?)", (x, tag, 5, now, now - x * 1000))
            db.execute("INSERT INTO accounts (xuid, gamertag, created, last_login) VALUES (?,?,?,?)", (x, tag, now, now - x * 1000))
        db.execute("INSERT INTO clubs VALUES (1)")
        sc = self.lsp.SB
        submit(sc, 1, "Alice", 91.5)
        submit(sc, 2, "Bob", 88.25)
        submit(sc, 2, "Bob", 99.0)                  # a worse second lap must not replace the best
        submit(sc, 3, "Cara", 90.0, event=1001)
        db.execute("INSERT INTO player_xp VALUES (1, 500, 0), (2, 1500, 0)")
        db.execute("UPDATE players SET last_seen = ? - xuid * 1000", (now,))       # submitting a score touched them all just now
        social.LAST_POLL.clear()
        social.touch(1)

    def test_overview(self):
        st, o = call(self.lsp, "GET", "/ops/v1/overview")
        self.assertEqual(st, 200)
        self.assertEqual((o["online"], o["accounts"], o["players"], o["clubs"], o["lobbies"], o["scores"]), (1, 3, 3, 1, 6, 4))
        self.assertEqual(o["registration"], "open")
        self.assertEqual(o["rotation"], {"month": 9, "reroll": 0, "events": 2})

    def test_online_lists_recent_pollers_only(self):
        st, o = call(self.lsp, "GET", "/ops/v1/online")
        self.assertEqual([p["gamertag"] for p in o["players"]], ["Alice"])

    def test_leaderboards_are_real_players_in_order(self):
        st, o = call(self.lsp, "GET", "/ops/v1/leaderboards?limit=10")
        self.assertEqual(st, 200)
        self.assertEqual([(r["gamertag"], r["xp"]) for r in o["xp"]], [("Bob", 1500), ("Alice", 500)])
        e0, e1 = o["events"]
        self.assertEqual([(r["position"], r["gamertag"], r["score"]) for r in e0["rows"]], [(1, "Bob", 88.25), (2, "Alice", 91.5)])
        self.assertEqual([r["gamertag"] for r in e1["rows"]], ["Cara"])
        self.assertEqual(e0["rows"][0]["car"], "Test Car")
        self.assertEqual(e0["rows"][0]["class"], "B")
        self.assertEqual(e0["class"], "B")

    def test_challenges_rotation_window(self):
        st, o = call(self.lsp, "GET", "/ops/v1/challenges?top=1")
        self.assertEqual(o["rotation"]["label"], "October 2026")
        self.assertEqual(o["rotation"]["endsAt"], 1793491200)           # 2026-11-01T00:00:00Z
        self.assertEqual(len(o["events"][0]["rows"]), 1)
        self.lsp.RIV.month = 11                                        # December rolls into the next year
        _, o = call(self.lsp, "GET", "/ops/v1/challenges?top=0")
        self.assertEqual((o["rotation"]["label"], o["rotation"]["endsAt"]), ("December 2026", 1798761600))
        self.assertEqual(o["events"][0]["rows"], [])

    def test_players_search_and_paging(self):
        _, o = call(self.lsp, "GET", "/ops/v1/players")
        self.assertEqual([p["gamertag"] for p in o["players"]], ["Alice", "Bob", "Cara"])      # most recently active first
        self.assertTrue(o["players"][0]["online"])
        _, o = call(self.lsp, "GET", "/ops/v1/players?q=ob")
        self.assertEqual((o["total"], o["players"][0]["gamertag"]), (1, "Bob"))
        _, o = call(self.lsp, "GET", "/ops/v1/players?q=3")                                      # an xuid
        self.assertEqual(o["players"][0]["gamertag"], "Cara")
        _, o = call(self.lsp, "GET", "/ops/v1/players?limit=1&offset=2")
        self.assertEqual(([p["gamertag"] for p in o["players"]], o["total"]), (["Cara"], 3))
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/players?limit=abc")[0], 400)

    def test_auth_events(self):
        self.lsp.DB.execute("INSERT INTO auth_events (at, kind, ip, xuid, detail) VALUES (1, 'login', '1.2.3.4', 1, '')")
        _, o = call(self.lsp, "GET", "/ops/v1/auth-events")
        self.assertEqual(o["events"][0]["kind"], "login")


class ActionTests(unittest.TestCase):
    def setUp(self):
        self.lsp = make_lsp()
        self.lsp.DB.execute("INSERT INTO accounts (xuid, gamertag, created) VALUES (1, 'Alice', 0)")

    def test_ban_unban_kick(self):
        self.assertEqual(call(self.lsp, "POST", "/ops/v1/players/1/ban", {"reason": "cheating"})[1], {"banned": True})
        self.assertEqual(call(self.lsp, "POST", "/ops/v1/players/1/ban", {})[0], 200)
        call(self.lsp, "POST", "/ops/v1/players/0x1/unban", {})
        self.assertEqual(call(self.lsp, "POST", "/ops/v1/players/1/kick", {})[1], {"sessionsDropped": 2})
        self.assertEqual(self.lsp.AUTH.calls, [("ban", 1, "cheating"), ("ban", 1, "banned by an operator"), ("unban", 1, None), ("kick", 1)])
        self.assertEqual(call(self.lsp, "POST", "/ops/v1/players/99/ban", {})[0], 404)
        self.assertEqual(call(self.lsp, "POST", "/ops/v1/players/zz/ban", {})[0], 400)
        self.assertEqual(call(self.lsp, "POST", "/ops/v1/players/1/explode", {})[0], 404)

    def test_notifications_pass_through(self):
        self.assertEqual(call(self.lsp, "POST", "/ops/v1/notifications", {"kind": "announcement", "text": "hi"})[1], {"echo": ["POST", "/api/notifications"]})
        self.assertEqual(call(self.lsp, "DELETE", "/ops/v1/notifications/12")[1], {"echo": ["DELETE", "/api/notifications/12"]})
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/notification-types")[1], {"echo": ["GET", "/api/types"]})

        def boom(m, p, b):
            raise ApiError(400, "unknown kind")
        self.lsp.api = boom
        st, o = call(self.lsp, "POST", "/ops/v1/notifications", {})
        self.assertEqual((st, o["message"]), (400, "unknown kind"))

    def test_rotate(self):
        st, o = call(self.lsp, "POST", "/ops/v1/rivals/rotate")
        self.assertEqual((st, self.lsp.RIV.rolled, len(o["events"])), (200, 1, 2))

    def test_invites(self):
        st, o = call(self.lsp, "POST", "/ops/v1/invites", {"uses": 5, "note": "friends"})
        self.assertEqual(st, 201)
        self.assertRegex(o["code"], r"^FH2-[A-Z2-9]{4}-[A-Z2-9]{4}$")
        self.assertEqual(self.lsp.AUTH.calls[-1], ("invite", o["code"], 5, "friends"))
        self.assertEqual(call(self.lsp, "POST", "/ops/v1/invites", {"code": "my-code"})[1]["code"], "MY-CODE")
        for bad in ({"code": "no spaces"}, {"uses": 0}, {"uses": "5"}, {"uses": True}, {"code": "ab"}):
            self.assertEqual(call(self.lsp, "POST", "/ops/v1/invites", bad)[0], 400, bad)


class ConfigTests(unittest.TestCase):
    def setUp(self):
        self.lsp = make_lsp()

    def test_set_and_reload(self):
        st, o = call(self.lsp, "PUT", "/ops/v1/config", {"motd": "Welcome", "maintenance": "back at 18:00", "registration": "invite",
                                                        "links": {"patreon": "https://patreon.example/x"}})
        self.assertEqual(st, 200)
        self.assertEqual((o["motd"], o["maintenance"], o["registration"]), ("Welcome", "back at 18:00", "invite"))
        self.assertEqual(o["links"], {"website": "https://x.example", "patreon": "https://patreon.example/x", "discord": ""})
        call(self.lsp, "PUT", "/ops/v1/config", {"links": {"discord": "https://discord.example/y"}})        # links merge, not replace
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/config")[1]["links"]["patreon"], "https://patreon.example/x")
        # a restarted LSP gets the saved values back
        fresh = make_lsp()
        fresh.DB = self.lsp.DB
        ops_api.load_saved(fresh)
        self.assertEqual((fresh.AUTH.registration, fresh.LAUNCHER.config.get()["motd"]), ("invite", "Welcome"))
        self.assertEqual(fresh.LAUNCHER.config.get()["links"]["discord"], "https://discord.example/y")
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/overview")[1]["maintenance"], "back at 18:00")

    def test_validation(self):
        for bad in ({"registration": "everyone"}, {"typo": 1}, {"minLauncher": "latest"}, {"motd": "x" * 501}, {"motd": 5},
                    {"links": {"twitter": "https://x"}}, {"links": {"patreon": "javascript:alert(1)"}}, {"links": []},
                    {"maintenance": "bell\x07"}):
            self.assertEqual(call(self.lsp, "PUT", "/ops/v1/config", bad)[0], 400, bad)
        self.assertEqual(self.lsp.AUTH.registration, "open")
        self.assertIsNone(self.lsp.DB.one("SELECT value FROM meta WHERE key='ops_config'"))

    def test_empty_strings_clear(self):
        call(self.lsp, "PUT", "/ops/v1/config", {"maintenance": "down"})
        call(self.lsp, "PUT", "/ops/v1/config", {"maintenance": ""})
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/config")[1]["maintenance"], "")


if __name__ == "__main__":
    unittest.main()
