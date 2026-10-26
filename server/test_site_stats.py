"""Tests for the website statistics (server/site_stats.py) through the operator API: python3 -I server/test_site_stats.py
(needs psycopg 3 importable like test_ops.py, no database server; the stores are test_ops.py's fakes on SQLite)."""
import json
import os
import sys
import time
import types
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import site_stats  # noqa: E402
import ugc  # noqa: E402
from test_ops import FakeDb, call, festival, make_lsp, submit  # noqa: E402

TABLES = """
CREATE TABLE rival_events (id INTEGER PRIMARY KEY, month INTEGER, reroll INTEGER, idx INTEGER, track INTEGER, laps INTEGER, length REAL,
    kind TEXT, attrs TEXT, cls INTEGER, name TEXT, trackname TEXT, description TEXT, created REAL, tod INTEGER, weather INTEGER,
    wetness REAL, precip REAL);
CREATE TABLE ugc_files (id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT, type INTEGER DEFAULT 0, owner INTEGER, title TEXT DEFAULT '',
    description TEXT DEFAULT '', created REAL, payload_size INTEGER DEFAULT 0, preview BLOB, versioned_id INTEGER, blob_id INTEGER,
    car_id INTEGER, make_id INTEGER, model_id INTEGER, class_id INTEGER, keyword1 INTEGER DEFAULT 0, keyword2 INTEGER DEFAULT 0, env INTEGER,
    track INTEGER, pi INTEGER, downloads INTEGER DEFAULT 0, likes INTEGER DEFAULT 0, uses INTEGER DEFAULT 0, views INTEGER DEFAULT 0,
    popularity INTEGER DEFAULT 0, removed INTEGER DEFAULT 0);
CREATE TABLE stats_samples (at REAL PRIMARY KEY, online INTEGER NOT NULL, lobbies INTEGER NOT NULL);
CREATE TABLE mode_results (id INTEGER PRIMARY KEY AUTOINCREMENT, at REAL NOT NULL, mode TEXT NOT NULL, detail TEXT NOT NULL);
"""
CATALOG = """
CREATE TABLE Tracks (id INTEGER, MediaName TEXT, Active INTEGER);
CREATE TABLE v_tracks (id INTEGER, name TEXT, length_m REAL, closed INTEGER);
CREATE TABLE v_cars (id INTEGER, name TEXT, make TEXT, year INTEGER, class_id INTEGER, country TEXT, drive TEXT, body TEXT, top_speed_ms REAL);
INSERT INTO Tracks VALUES (856,'Anthem',1), (891,'Anthem',1);
INSERT INTO v_tracks VALUES (856,'Sisteron Citadel Circuit',1664,1), (891,'Sisteron High Speed Sprint',6463,0);
INSERT INTO v_cars VALUES (7,'Test Car','Testmake',2014,3,'Italy','RWD','None',90.0), (8,'Other Car','Othermake',1969,0,'Japan','AWD','Rally',60.0);
"""
PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 24
JPEG = b"\xff\xd8\xff\xe0" + b"\x00" * 24


def speed(sc, xuid, tag, mps, trap, st=5, car=7):
    sc.submit(xuid, tag, st, {"scoreboardType": 9, "gameScoreboardId": trap, "trackId": 0, "carId": car, "carClassId": 3,
                              "carPerformanceIndex": 0.5, "score": site_stats.scoreboard.dbl_to_u64(mps), "isClean": True, "usedSTM": False,
                              "usedTCS": False, "usedABS": False})


class Base(unittest.TestCase):
    def setUp(self):
        self.lsp = lsp = make_lsp()
        lsp.DB.c.executescript(TABLES)
        cat = FakeDb()
        cat.c.executescript(CATALOG)
        lsp.CARS = types.SimpleNamespace(db=cat)
        lsp.RIV.names = lambda: {"car": {7: "Test Car", 8: "Other Car"}}
        now = time.time()
        for x, tag in ((1, "Alice"), (2, "Bob"), (3, "Cara")):
            lsp.DB.execute("INSERT INTO players VALUES (?,?,?,?,?)", (x, tag, 5, now, now))
        for i, e in enumerate(lsp.RIV.evs):
            lsp.DB.execute("INSERT INTO rival_events (id, month, reroll, idx, track, laps, length, kind, attrs, cls, name, trackname, description, "
                           "created) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)", (e["id"], 9, 0, i, e["track"], 3, 4200.0, "class", "{}", 2,
                                                                             e["name"], e["trackname"], "", now))
        lsp.DB.execute("INSERT INTO rival_events (id, month, reroll, idx, track, laps, length, kind, attrs, cls, name, trackname, description, "
                       "created) VALUES (900, 8, 0, 0, 1, 2, 3000.0, 'class', '{}', 1, 'Old Event', 'Old Track', '', 0)")

    def get(self, path):
        st, o = call(self.lsp, "GET", path)
        self.assertEqual(st, 200, o)
        return o

    def ordered(self):
        self.lsp.DB.execute("UPDATE score_submissions SET submitted = id * 100")      # a known order in time


class RecordTests(Base):
    def setUp(self):
        super().setUp()
        sc = self.lsp.SB
        festival(sc, 1, "Alice", 70.0, 856)          # 1: first time on the A board
        festival(sc, 2, "Bob", 69.5, 856)            # 2: Bob takes it by 0.5
        festival(sc, 1, "Alice", 69.6, 856)          # 3: Alice improves but stays second (0.1 behind)
        submit(sc, 3, "Cara", 90.0)                  # 4: Monthly event 1000
        submit(sc, 3, "Cara", 91.0)                  # 5: slower: no record
        speed(sc, 2, "Bob", 80.0, 3)                 # 6: trap 3
        speed(sc, 1, "Alice", 85.0, 3)               # 7: faster is better on a trap
        speed(sc, 1, "Alice", 200.0, 3)              # 8: above the speed cap: never a record
        submit(sc, 1, "Alice", 50.0, event=900)      # 9: an event of an earlier month
        self.ordered()

    def test_feed_newest_first_with_previous_holder_and_margin(self):
        o = self.get("/ops/v1/records")
        feed = o["feed"]
        self.assertEqual([(r["board"]["kind"], r["gamertag"]) for r in feed],
                         [("event", "Alice"), ("trap", "Alice"), ("trap", "Bob"), ("event", "Cara"), ("route", "Bob"), ("route", "Alice")])
        bob = feed[4]
        self.assertEqual((bob["board"]["name"], bob["board"]["class"], bob["previous"]["gamertag"], round(bob["margin"], 3)),
                         ("Sisteron Citadel Circuit", "A", "Alice", 0.5))
        self.assertIsNone(bob["until"])
        self.assertEqual(feed[5]["until"], 200)                       # Alice's first record ended when Bob beat it
        self.assertIsNone(feed[3]["previous"])
        self.assertEqual(feed[1]["board"]["name"], "Speed Trap 3")
        self.assertEqual((feed[0]["board"]["name"], feed[0]["board"]["month"]), ("Old Event", 8))
        self.assertEqual(o["totals"], {"records": 6, "boards": 4, "holders": 3})

    def test_holders_longest_and_closest(self):
        o = self.get("/ops/v1/records?limit=0")
        self.assertEqual(o["feed"], [])
        self.assertEqual([(h["gamertag"], h["current"], h["total"]) for h in o["holders"]], [("Alice", 2, 3), ("Bob", 1, 2), ("Cara", 1, 1)])
        self.assertEqual([r["board"]["kind"] for r in o["longest"]], ["route", "event", "trap", "event"])
        close = o["closest"]
        self.assertEqual(len(close), 1)                               # only the route has two drivers (traps are not photo finishes)
        self.assertEqual((close[0]["first"]["gamertag"], close[0]["second"]["gamertag"], round(close[0]["gap"], 3)), ("Bob", "Alice", 0.1))

    def test_history_is_cached_until_a_run_is_stored(self):
        site_stats.record_history(self.lsp)
        first = self.lsp._site_history["data"]
        self.assertIs(site_stats.record_history(self.lsp), first)
        festival(self.lsp.SB, 3, "Cara", 60.0, 856)
        self.lsp._site_history["checked"] -= site_stats.HISTORY_RECHECK + 1
        self.assertIsNot(site_stats.record_history(self.lsp), first)
        self.assertEqual(self.get("/ops/v1/records?limit=1")["feed"][0]["gamertag"], "Cara")


class HallTrapTests(Base):
    def test_hall_of_fame_months_podiums_and_champion(self):
        sc = self.lsp.SB
        submit(sc, 1, "Alice", 80.0)
        submit(sc, 2, "Bob", 82.0)
        submit(sc, 2, "Bob", 70.0, event=1001)
        submit(sc, 1, "Alice", 75.0, event=1001)
        submit(sc, 3, "Cara", 60.0, event=900)
        o = self.get("/ops/v1/hall-of-fame")
        months = o["months"]
        self.assertEqual([(m["label"], m["current"]) for m in months], [("October 2026", True), ("September 2026", False)])
        oct_ = months[0]
        self.assertEqual([[r["gamertag"] for r in e["podium"]] for e in oct_["events"]], [["Alice", "Bob"], ["Bob", "Alice"]])
        self.assertEqual(oct_["champion"]["gamertag"], "Alice")      # one win each, two podiums each: second places tie, name decides
        self.assertEqual((months[1]["champion"]["gamertag"], months[1]["events"][0]["podium"][0]["carId"]), ("Cara", 7))

    def test_current_month_shows_even_without_times(self):
        o = self.get("/ops/v1/hall-of-fame")
        self.assertEqual([len(m["events"]) for m in o["months"]], [2])
        self.assertIsNone(o["months"][0]["champion"])

    def test_traps_and_zones(self):
        sc = self.lsp.SB
        speed(sc, 1, "Alice", 70.0, 3)
        speed(sc, 2, "Bob", 75.0, 3)
        speed(sc, 2, "Bob", 300.0, 4)                                 # impossible: hidden, so trap 4 has no board
        speed(sc, 3, "Cara", 50.0, 40, st=6)
        o = self.get("/ops/v1/traps")
        self.assertEqual([(t["name"], [r["gamertag"] for r in t["rows"]]) for t in o["traps"]], [("Speed Trap 3", ["Bob", "Alice"])])
        self.assertEqual([t["name"] for t in o["zones"]], ["Speed Zone 40"])


class CarTests(Base):
    def setUp(self):
        super().setUp()
        sc = self.lsp.SB
        festival(sc, 1, "Alice", 70.0, 856)
        festival(sc, 1, "Alice", 71.0, 856)
        festival(sc, 2, "Bob", 69.0, 856)
        sc.submit(3, "Cara", 1, {"scoreboardType": 1, "gameScoreboardId": 0, "trackId": 891, "carId": 8, "carClassId": 0,
                                 "carPerformanceIndex": 0.3, "score": site_stats.scoreboard.dbl_to_u64(200.0), "isClean": True,
                                 "usedSTM": False, "usedTCS": False, "usedABS": False})
        sc.submit(3, "Cara", 1, {"scoreboardType": 1, "gameScoreboardId": 0, "trackId": 856, "carId": 99, "carClassId": 3,
                                 "carPerformanceIndex": 0.3, "score": site_stats.scoreboard.dbl_to_u64(60.0), "isClean": True,
                                 "usedSTM": False, "usedTCS": False, "usedABS": False})          # a car the catalog does not know
        self.ordered()

    def test_most_raced_cars_makes_and_classes(self):
        o = self.get("/ops/v1/cars")
        self.assertEqual([(c["name"], c["runs"], c["drivers"], c["records"]) for c in o["cars"]],
                         [("Testmake Test Car", 3, 2, 0), ("Othermake Other Car", 1, 1, 1)])
        self.assertEqual(o["total"], 4)
        self.assertEqual([(m["make"], m["country"], m["runs"], m["records"]) for m in o["makes"]],
                         [("Testmake", "Italy", 3, 0), ("Othermake", "Japan", 1, 1)])
        self.assertEqual({c["class"]: c["runs"] for c in o["classes"]}["A"], 4)          # the unknown car counts in the class split
        self.assertEqual(o["cars"][1]["body"], "Rally")
        self.assertEqual(o["cars"][0]["body"], "")

    def test_class_and_day_filters(self):
        self.assertEqual([c["name"] for c in self.get("/ops/v1/cars?class=d")["cars"]], ["Othermake Other Car"])
        self.lsp.DB.execute("UPDATE score_submissions SET submitted = ?", (time.time() - 30 * 86400,))
        self.assertEqual(self.get("/ops/v1/cars?days=7")["cars"], [])
        self.assertEqual(len(self.get("/ops/v1/cars?days=60")["cars"]), 2)

    def test_one_car(self):
        o = self.get("/ops/v1/cars/7")
        self.assertEqual((o["car"]["name"], o["car"]["year"], o["runs"], o["drivers"]), ("Testmake Test Car", 2014, 3, 2))
        self.assertEqual([(b["board"]["name"], b["gamertag"], b["score"], b["record"]) for b in o["best"]],
                         [("Sisteron Citadel Circuit", "Bob", 69.0, False)])            # the unknown car's 60.0 holds that record
        self.assertEqual([(d["gamertag"], d["runs"]) for d in o["topDrivers"]], [("Alice", 2), ("Bob", 1)])
        self.assertEqual(len(o["weeks"]), site_stats.WEEKS)
        other = self.get("/ops/v1/cars/8")
        self.assertEqual([r["board"]["name"] for r in other["records"]], ["Sisteron High Speed Sprint"])
        self.assertTrue(other["best"][0]["record"])

    def test_unknown_car(self):
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/cars/12345")[0], 404)
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/cars/abc")[0], 404)
        self.assertEqual(self.get("/ops/v1/cars/99")["car"]["name"], "")             # raced but not in the catalog: still answers


class GarageTests(Base):
    def setUp(self):
        super().setUp()
        db = self.lsp.DB
        rows = [("livery", 1, "Red Arrow", 8, 3, 10, 5, ugc.pack(PNG), 0), ("livery", 2, "Blue Moon", 7, 1, 2, 9, b"", 0),
                ("tune", 2, "Grip", 7, 3, 40, 1, b"", 0), ("livery", 3, "Gone", 7, 1, 99, 99, ugc.pack(PNG), 1)]
        for i, (kind, owner, title, car, cls, dl, likes, prev, removed) in enumerate(rows):
            db.execute("INSERT INTO ugc_files (kind, owner, title, created, car_id, class_id, downloads, likes, preview, removed) "
                       "VALUES (?,?,?,?,?,?,?,?,?,?)", (kind, owner, title, 1000 + i, car, cls, dl, likes, prev, removed))

    def test_listing_sorts_and_counts(self):
        o = self.get("/ops/v1/garage")
        self.assertEqual([i["title"] for i in o["items"]], ["Blue Moon", "Red Arrow"])              # likes; the removed one is hidden
        self.assertEqual((o["items"][1]["car"], o["items"][1]["class"], o["items"][1]["preview"], o["items"][0]["preview"]),
                         ("Othermake Other Car", "A", True, False))
        self.assertEqual(o["counts"], {"livery": 2, "tune": 1, "photo": 0})
        self.assertEqual([i["title"] for i in self.get("/ops/v1/garage?sort=downloads")["items"]], ["Red Arrow", "Blue Moon"])
        self.assertEqual([i["title"] for i in self.get("/ops/v1/garage?kind=tune")["items"]], ["Grip"])
        self.assertEqual([(c["gamertag"], c["designs"], c["downloads"]) for c in o["creators"]], [("Bob", 2, 42), ("Alice", 1, 10)])

    def test_bad_arguments(self):
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/garage?kind=car")[0], 400)
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/garage?sort=random")[0], 400)

    def test_preview(self):
        st, png = call(self.lsp, "GET", "/ops/v1/garage/1/preview.png")
        self.assertEqual((st, png), (200, PNG))
        for fid in (2, 4, 77, "x"):                                    # no picture, removed, unknown, not a number
            self.assertEqual(call(self.lsp, "GET", f"/ops/v1/garage/{fid}/preview.png")[0], 404)

    def test_jpeg_preview(self):                                       # photos are stored as the game's JPEG
        self.lsp.DB.execute("UPDATE ugc_files SET preview = ? WHERE id = 1", (ugc.pack(JPEG),))
        self.assertEqual(call(self.lsp, "GET", "/ops/v1/garage/1/preview.png"), (200, JPEG))


class PopulationTests(Base):
    def test_samples_peak_and_history(self):
        lsp = self.lsp
        now = time.time()
        base = (now - 3600) // 600 * 600 + 10                             # three samples inside one 10-minute bucket
        for i, n in enumerate((3, 5, 4)):
            lsp.online_count = lambda n=n: n
            site_stats.sample(lsp, base + i * 60)
        lsp.online_count = lambda: 0
        site_stats.sample(lsp, now - 60)
        o = self.get("/ops/v1/history?range=24h")
        self.assertEqual(sum(1 for p in o["points"] if p[1]), 1)        # one 10-minute bucket holds the three busy samples (max kept)
        self.assertEqual((o["peak"]["online"], o["allTimePeak"]["online"]), (5, 5))
        self.assertEqual(o["points"][-1][2], 6)                          # lobbies
        self.assertEqual(len(o["days"]), site_stats.DAYS)
        self.assertEqual(call(lsp, "GET", "/ops/v1/history?range=1y")[0], 400)

    def test_days_count_signups_runs_and_racers(self):
        db = self.lsp.DB
        db.execute("INSERT INTO accounts (xuid, gamertag, created) VALUES (1, 'Alice', ?), (2, 'Bob', ?)", (time.time(), time.time() - 40 * 86400))
        festival(self.lsp.SB, 1, "Alice", 70.0, 856)
        festival(self.lsp.SB, 1, "Alice", 69.0, 856)
        today = self.get("/ops/v1/history")["days"][-1]
        self.assertEqual((today["signups"], today["runs"], today["racers"]), (1, 2, 1))

    def test_old_samples_are_pruned(self):
        site_stats.sample(self.lsp, 1000.0)
        site_stats.sample(self.lsp, 3600.0 * 10000)                      # on the hour: prunes everything older than KEEP_SAMPLES
        self.assertEqual(self.lsp.DB.one("SELECT COUNT(*) FROM stats_samples")[0], 1)


class ModeTests(Base):
    def post(self, mode, **detail):
        return site_stats.add_result(self.lsp, json.dumps({"mode": mode, "detail": detail}).encode())

    def test_results_and_totals(self):
        self.post("race", winner=1, players=[{"xuid": 1, "car": 7, "finish": 1}, {"xuid": 2, "car": 8, "rental": True, "finish": 2}])
        self.post("team", winner=2, players=[{"xuid": 1, "team": 0}, {"xuid": 2, "team": 1, "car": 8, "rental": True}])
        self.post("king", players=[{"xuid": 1}, {"xuid": 2}], hits=[[1, 3], [2, 1]], held=[[1, 50.5], [2, 80.0]], winner=2)
        self.post("infection", players=[{"xuid": 1}, {"xuid": 2}], hits=[[2, 1]], everyone=True, seconds=42.5, survivors=[])
        self.post("infection", players=[{"xuid": 1}, {"xuid": 2}], hits=[], everyone=False, seconds=90, survivors=[1])
        festival(self.lsp.SB, 1, "Alice", 3600.0, 856)
        speed(self.lsp.SB, 2, "Bob", 88.0, 1)
        o = self.get("/ops/v1/totals")
        m = o["modes"]
        self.assertEqual((m["races"]["count"], m["races"]["avgPlayers"], [w["gamertag"] for w in m["races"]["winners"]]), (2, 2.0, ["Alice", "Bob"]))
        self.assertEqual((m["teams"]["red"], m["teams"]["blue"]), (0, 1))
        self.assertEqual([(s["gamertag"], s["count"]) for s in m["king"]["stealers"]], [("Alice", 3), ("Bob", 1)])
        self.assertEqual(([w["gamertag"] for w in m["king"]["winners"]], m["king"]["longestCrown"]["gamertag"]), (["Bob"], "Bob"))
        self.assertEqual([(s["gamertag"], s["count"]) for s in m["infection"]["survivors"]], [("Alice", 1)])
        self.assertEqual((m["infection"]["count"], m["infection"]["everyone"], m["infection"]["fastest"]), (2, 1, 42.5))
        self.assertEqual(m["rentals"], [{"carId": 8, "car": "Othermake Other Car", "count": 2}])
        self.assertEqual((o["km"], o["hours"], o["topSpeed"]["gamertag"], o["runs"]), (2, 1.0, "Bob", 2))

    def test_bad_results_are_refused(self):
        for body in (b"[]", b'{"mode": "drift", "detail": {}}', b'{"mode": "race"}', b"x" * (site_stats.RESULT_MAX + 1)):
            with self.assertRaises(ValueError):
                site_stats.add_result(self.lsp, body)


class ModerationTests(Base):
    def test_warning_and_hide_apply_to_all_run_surfaces(self):
        import score_reports
        import hashlib
        submit(self.lsp.SB, 1, 'Alice', 80)
        submit(self.lsp.SB, 2, 'Bob', 90)
        sid = self.lsp.DB.one('SELECT MIN(id) FROM score_submissions')[0]
        for i in range(10):
            value = hashlib.sha256(str(i).encode()).hexdigest()
            score_reports.report(self.lsp, sid, {'browser': value, 'ip': value})
        for path, pick in [('/ops/v1/recent', lambda d: next(r for r in d['runs'] if r['submissionId'] == sid)),
                           ('/ops/v1/records', lambda d: d['feed'][0]),
                           ('/ops/v1/hall-of-fame', lambda d: d['months'][0]['events'][0]['podium'][0]),
                           ('/ops/v1/cars/7', lambda d: d['best'][0])]:
            self.assertTrue(pick(self.get(path))['suspicious'], path)
        score_reports.moderate(self.lsp, sid, 'hide')
        self.assertNotIn(sid, [r['submissionId'] for r in self.get('/ops/v1/recent')['runs']])
        self.assertEqual(self.get('/ops/v1/records')['feed'][0]['gamertag'], 'Bob')
        self.assertEqual(self.get('/ops/v1/hall-of-fame')['months'][0]['events'][0]['podium'][0]['gamertag'], 'Bob')
        self.assertEqual(self.get('/ops/v1/cars/7')['best'][0]['gamertag'], 'Bob')


if __name__ == "__main__":
    unittest.main()
