"""Tests for the PostgreSQL storage layer: FH2MP_PG_DSN=... python3 -I server/test_db.py (needs psycopg 3).

The DSN must point at a scratch database: every test drops and recreates the `public` schema (the `game` catalog schema is left alone).
No game files needed: the Drivatar templates here are tiny stand-ins.
"""
import math
import os
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import db  # noqa: E402
import drivatar  # noqa: E402
import notifstore  # noqa: E402
import scoreboard  # noqa: E402


def sub(score, sb=4, st=1, track=7, car=1):
    return {"scoreboardType": sb, "gameScoreboardId": 1, "trackId": track, "carId": car, "carClassId": 2,
            "carPerformanceIndex": 0.5, "score": scoreboard.dbl_to_u64(score), "isClean": True, "usedSTM": False,
            "usedTCS": False, "usedABS": True}


def fresh_db():
    import psycopg
    with psycopg.connect(os.environ["FH2MP_PG_DSN"], autocommit=True) as c:
        c.execute("DROP SCHEMA IF EXISTS public CASCADE")
        c.execute("CREATE SCHEMA public")
    return db.open_db()


class Base(unittest.TestCase):
    def setUp(self):
        self.db = fresh_db()

    def tearDown(self):
        self.db.close()


class SchemaTests(Base):
    def test_migrations_are_idempotent(self):
        v = self.db.one("SELECT version FROM schema_version")[0]
        self.assertEqual(v, len(db.MIGRATIONS))
        again = db.Database()
        self.assertEqual(again.one("SELECT version FROM schema_version")[0], v)
        again.close()

    def test_failed_tx_rolls_back(self):
        with self.assertRaises(RuntimeError):
            with self.db.tx() as c:
                c.execute("INSERT INTO players (xuid, first_seen, last_seen) VALUES (1, 0, 0)")
                raise RuntimeError
        self.assertEqual(self.db.one("SELECT COUNT(*) FROM players")[0], 0)


class DisplayLevelTests(unittest.TestCase):
    def test_stock_wristband_thresholds(self):
        for xp, level in ((0, 1), (999, 1), (14999, 1), (15000, 2), (209999, 14), (210000, 15),
                          (2410000, 125), (2909999, 149), (2910000, 150), (2930299, 150),
                          (2930300, 151), (128137499, 998), (128137500, 999), (0xFFFFFFFF, 999)):
            self.assertEqual(db.display_level(xp), level, xp)


class LevelTests(Base):
    """currentCareerLevel is cumulative XP; clamping it to 999 makes the client show level 1."""

    def level(self, reported):
        with self.db.tx() as c:
            db.touch_player(c, 7, "tag", level=reported)
        return self.db.one("SELECT career_level FROM players WHERE xuid=7")[0]

    def test_career_xp_is_preserved_in_u32_range(self):
        self.assertEqual(self.level(41), 41)
        self.assertEqual(self.level(2410000), 2410000)
        self.assertEqual(self.level(3567658931), 3567658931)
        self.assertEqual(self.level(1 << 32), 0xFFFFFFFF)
        self.assertEqual(self.level(-5), 0)
        self.assertEqual(self.level("junk"), 0)

    def test_no_level_leaves_the_stored_one(self):
        self.level(41)
        with self.db.tx() as c:
            db.touch_player(c, 7, "tag")
        self.assertEqual(self.db.one("SELECT career_level FROM players WHERE xuid=7")[0], 41)


class ScoreTests(Base):
    def setUp(self):
        super().setUp()
        self.sc = scoreboard.ScoreStore(self.db, ["A", "B", "C"])

    def test_monthly_class_is_checked_before_best_selection(self):
        event = {"attrs": {"MakeId": 19, "CarClassId": 2}, "cls": 2}
        # Simulate a legacy S2 PB that predates validation and a slower valid B run.
        self.sc.submit(10, "Jaguar", 1, dict(sub(61.410, sb=3), carClassId=5))
        self.sc.submit(10, "Jaguar", 1, sub(70.0, sb=3))
        self.sc.event_info = lambda event_id: event if event_id == 1 else None
        self.assertIsNone(self.sc.submit(11, "S2", 1, dict(sub(60.0, sb=3), carClassId=5)))
        self.assertIsNone(self.sc.submit(11, "D", 1, dict(sub(80.0, sb=3), carClassId=0)))
        self.assertIsNotNone(self.sc.submit(11, "B", 1, sub(68.0, sb=3)))
        self.assertEqual(self.sc.best_of(10, 3, 1, 7, 1)["score"], 70.0)
        self.assertEqual([r["cls"] for r in self.sc.top(3, 1, 7, 1)], [2, 2])
        self.assertEqual(self.sc.rival_for(11, 3, 1, 7, 1, 10)["score"], 70.0)
        # Open events' illustrative card class must not become a restriction.
        event["attrs"] = {}
        self.assertIsNotNone(self.sc.submit(11, "S2", 1, dict(sub(60.0, sb=3), carClassId=5)))

    def test_best_per_player_direction(self):
        # scoreType 1 is ascending (times), 2 descending
        for xuid, t in ((10, 90.0), (10, 80.5), (10, 85.0), (11, 70.0)):
            self.sc.submit(xuid, "p%d" % xuid, 1, sub(t))
        for xuid, v in ((10, 5.0), (10, 9.0), (11, 7.0)):
            self.sc.submit(xuid, "p%d" % xuid, 2, sub(v))
        self.assertEqual(self.sc.best_of(10, 4, 1, 7)["score"], 80.5)
        self.assertEqual(self.sc.best_of(10, 4, 2, 7)["score"], 9.0)
        self.assertIsNone(self.sc.best_of(10, 4, 1, 8))
        self.assertEqual(self.sc.best_of(10, 4, 1, 65535)["score"], 80.5)      # any track
        rows, mine, nreal, _ = self.sc.rows(10, 4, 1, 7, 1, 100)
        self.assertEqual(nreal, 2)
        self.assertEqual(mine["tag"], "p10")
        self.assertEqual([r["score"] for r in rows if r["xuid"] in (10, 11)], [70.0, 80.5])

    def test_speed_trap_boards_are_per_trap(self):
        # board type 9 (speed traps / zones): GetRows pivotId == SubmitScore.gameScoreboardId, trackId is always 65535
        def trap(gsid, v, st=5):
            d = sub(v, sb=9, st=st, track=65535)
            d["gameScoreboardId"] = gsid
            return d
        for xuid, gsid, v in ((10, 27, 80.0), (10, 3, 40.0), (10, 3, 45.0), (11, 3, 50.0), (11, 27, 60.0)):
            self.sc.submit(xuid, "p%d" % xuid, 5, trap(gsid, v))
        self.assertEqual(self.sc.best_of(10, 9, 5, 65535, 3)["score"], 45.0)       # an improvement on trap 3 counts
        self.assertEqual(self.sc.best_of(10, 9, 5, 65535, 27)["score"], 80.0)
        self.assertEqual(self.sc.best_of(11, 9, 5, 65535, 3)["score"], 50.0)
        self.assertIsNone(self.sc.best_of(10, 9, 5, 65535, 4))                       # never played trap 4
        rows, mine, nreal, _ = self.sc.rows(10, 9, 5, 65535, 1, 100, 3)
        self.assertEqual(nreal, 2)
        self.assertEqual(mine["score"], 45.0)
        self.sc.submit(10, "p10", 6, trap(40, 30.0, st=6))                             # a zone board is separate from the traps
        self.assertIsNone(self.sc.best_of(10, 9, 5, 65535, 40))
        self.assertEqual(self.sc.best_of(10, 9, 6, 65535, 40)["score"], 30.0)

    def test_made_up_rivals_never_beat_a_world_record(self):
        names = ["A", "B", "C", "D", "E", "F"]
        wr = {(856, 5): 44.564}                                     # Citadel Circuit, S2
        self.sc.submit(10, "p10", 1, dict(sub(55.0, sb=1, st=1, track=856), carClassId=5))
        free = scoreboard.ScoreStore(self.db, names, records={})
        capped = scoreboard.ScoreStore(self.db, names, records=wr)
        fast = lambda s: min(r["score"] for r in s.rows(10, 1, 1, 856, 1, 100, 5)[0] if r.get("fake"))
        self.addCleanup(setattr, scoreboard, "FAKE_ROWS", scoreboard.FAKE_ROWS)
        scoreboard.FAKE_ROWS = 40                                   # made-up rivals are off by default
        self.assertLess(fast(free), 44.564)                         # without records some made-up rival is faster than the record
        self.assertGreaterEqual(fast(capped), 44.564 * scoreboard.WR_FLOOR - 1e-9)
        other = scoreboard.ScoreStore(self.db, names, records=wr).rows(10, 1, 1, 857, 1, 100, 5)[0]
        self.assertTrue(other)                                      # a track without a record still works

    def test_top_player_races_their_own_ghost(self):
        for xuid, t in ((10, 60.0), (11, 70.0)):
            self.sc.submit(xuid, "p%d" % xuid, 1, sub(t, sb=3))
            self.sc.attach_ghost(self.sc.db.one("SELECT score_id FROM score_submissions WHERE xuid=?", (xuid,))[0], b"g")
        args = (3, 1, 7, 1)
        self.assertEqual(self.sc.rival_for(11, *args)["xuid"], 10)                    # the row just above
        self.assertEqual(self.sc.rival_for(10, *args)["xuid"], 10)                    # #1: nobody above, so their own best
        self.assertTrue(self.sc.rival_for(10, *args)["ghost"])
        self.assertEqual(self.sc.rival_for(11, *args, rival_xuid=11)["xuid"], 11)     # picking yourself
        self.assertEqual(self.sc.rival_for(11, *args, rival_xuid=10)["xuid"], 10)

    def test_picked_rival_is_the_one_raced(self):
        # GitHub #12: picking a row on the board sends its xuid as rivalXuid; the server used to answer the row above you anyway
        for xuid, t in ((10, 50.0), (11, 60.0), (12, 70.0), (13, 80.0)):
            self.sc.submit(xuid, "p%d" % xuid, 1, sub(t, sb=3))
        args = (3, 1, 7, 1)
        self.assertEqual(self.sc.rival_for(13, *args)["xuid"], 12)                    # nothing picked: the row just above
        self.assertEqual(self.sc.rival_for(13, *args, rival_xuid=10)["xuid"], 10)     # the top player, two rows further up
        self.assertEqual(self.sc.rival_for(11, *args, rival_xuid=13)["xuid"], 13)     # someone slower
        self.assertEqual(self.sc.rival_for(10, *args, rival_xuid=12)["xuid"], 12)     # picked by the #1
        self.assertEqual(self.sc.rival_for(13, *args, rival_xuid=99)["xuid"], 12)     # not on this board: the usual choice
        self.assertEqual(self.sc.rival_for(99, *args, rival_xuid=11)["xuid"], 11)     # no time yet, picked a row

    def test_empty_board_races_the_world_record(self):
        # GitHub #12: an empty answer leaves the client spinning; the world record (no ghost) is raced instead and never listed
        wr = {(856, 3): 50.0, (856, 5): 44.564, (900, 1): 70.0}
        events = {2441: {"track": 856, "laps": 1, "cls": 3}, 2442: {"track": 856, "laps": 1, "cls": None},
                  2443: {"track": 857, "laps": 1, "cls": 3}}
        sc = scoreboard.ScoreStore(self.db, ["A"], records=wr, event_info=events.get)
        r = sc.rival_for(10, 1, 1, 856, 5)                                           # Festival Route, S2 board
        self.assertEqual((r["score"], r["tag"], sc.fake_name(r["xuid"]), r["ghost"] if "ghost" in r else 0),
                         (44.564, "World Record", "World Record", 0))                 # never a player's name, no ghost
        self.assertEqual(sc.rival_for(10, 1, 1, 856, 4)["score"], 44.564)            # S1 has no record: the next faster class
        self.assertIsNone(sc.rival_for(10, 1, 1, 900, 3))                            # only slower classes: no record to race
        self.assertEqual(sc.rival_for(10, 3, 1, 856, 2441)["score"], 50.0)           # Monthly: the event's track and class
        self.assertEqual(sc.rival_for(10, 3, 1, 856, 2442)["score"], 44.564)         # open class: the track's fastest record
        self.assertIsNone(sc.rival_for(10, 3, 1, 857, 2443))                         # no record on that track
        self.assertEqual(sc.rival_for(10, 6, 1, 856, 7)["score"], 44.564)            # Showcase: the request's track
        self.assertEqual(sc.rows(10, 1, 1, 856, 1, 100, 5)[0], [])                   # not listed on the board
        sc.submit(10, "p10", 1, dict(sub(50.0, sb=1, track=856), carClassId=5))
        self.assertEqual(sc.rival_for(10, 1, 1, 856, 5)["xuid"], 10)                 # once a time exists: the real board

    def test_speed_boards_are_capped(self):
        def trap(v):
            return dict(sub(v, sb=9, st=5, track=65535), gameScoreboardId=3)
        self.assertIsNone(self.sc.submit(10, "p10", 5, trap(319.477)))                # 715 mph: rejected
        self.assertIsNotNone(self.sc.submit(10, "p10", 5, trap(126.0)))              # 282 mph: a real record
        self.db.execute("INSERT INTO score_submissions (xuid, sb, st, gsid, track, car, cls, cpi, score, clean, stm, tcs, abs, "
                        "submitted) VALUES (11, 9, 5, 3, 65535, 1, 2, 0.5, 200.0, 0, 0, 0, 0, 0)")   # stored before the cap
        rows, _, nreal, _ = self.sc.rows(10, 9, 5, 65535, 1, 100, 3)
        self.assertEqual([r["score"] for r in rows], [126.0])
        self.assertEqual(nreal, 1)
        self.assertIsNotNone(self.sc.submit(10, "p10", 1, sub(200.0, sb=3)))          # the cap is only for speed boards

    def test_shifting_assists_are_kept(self):
        self.sc.submit(10, "p10", 1, dict(sub(50.0, sb=3), usedAutoShifting=False, usedClutch=True, usedAutoBrake=True))
        self.sc.submit(11, "p11", 1, dict(sub(60.0, sb=3), usedAutoShifting=True))
        rows = {r["xuid"]: r for r in self.sc.rows(10, 3, 1, 7, 1, 100, 1)[0]}
        self.assertEqual((rows[10]["autoshift"], rows[10]["clutch"], rows[10]["autobrake"]), (False, True, True))
        self.assertEqual((rows[11]["autoshift"], rows[11]["clutch"]), (True, False))

    def test_non_finite_scores_are_rejected(self):
        for bad in (math.nan, math.inf, -math.inf):
            self.assertIsNone(self.sc.submit(10, "x", 1, sub(bad)))
        self.assertEqual(self.sc.count(), 0)

    def test_xp_increments_are_atomic(self):
        threads = [threading.Thread(target=lambda: [self.sc.add_xp(42, 1.0) for _ in range(25)]) for _ in range(8)]
        [t.start() for t in threads]
        [t.join() for t in threads]
        self.assertEqual(self.sc.xp_of(42), 200.0)

    def test_xp_board_takes_accumulated_xp(self):
        self.sc.add_xp(5, 1234.0)
        self.assertEqual(self.sc.best_of(5, *scoreboard.XP_BOARD, 65535)["score"], 1234.0)

    def test_impossible_xp_awards_are_kept_in_the_ledger_but_not_counted(self):
        self.sc.add_xp(7, 7500.0)
        self.assertEqual(self.sc.add_xp(7, 0.0)[:2], (7500.0, "ok"))      # a zero award adds nothing and is not worth a row
        for bad in (12_500_000.0, 56_001.0, -5.0, math.nan):
            total, verdict, reason = self.sc.add_xp(7, bad)
            self.assertEqual((total, verdict), (7500.0, "rejected"), bad)
            self.assertTrue(reason)
        self.assertEqual(self.sc.add_xp(7, 56_000.0)[:2], (63_500.0, "ok"))
        rows = self.db.query("SELECT verdict, COUNT(*) AS n FROM xp_awards WHERE xuid=7 GROUP BY verdict ORDER BY verdict")
        self.assertEqual([(r["verdict"], r["n"]) for r in rows], [("ok", 2), ("rejected", 4)])

    def test_backfill_takes_logged_cheats_off_the_total_and_the_club_base(self):
        import xp_audit
        with self.db.tx() as c:          # the state the old code left: every award counted
            c.execute("INSERT INTO player_xp (xuid, xp, updated) VALUES (8, 25007550, 0), (9, 300, 0)")
            c.execute("INSERT INTO clubs (id, name, tag, created) VALUES (1, 'c', 't', 0)")
            c.execute("INSERT INTO club_members (xuid, club_id, role, joined, status_changed, xp_base) VALUES (8, 1, 1, 150, 150, 12507500)")
        awards = [(8, 7500.0, 7500.0, 100.0), (8, 12_500_000.0, 12_507_500.0, 120.0), (8, 12_500_000.0, 25_007_500.0, 200.0),
                  (8, 50.0, 25_007_550.0, 210.0), (9, 300.0, 300.0, 100.0)]
        acc = xp_audit.audit(awards)
        self.assertEqual(xp_audit.apply(self.db, acc, write=False), ["0x8: 25007550 -> 7550, club base -12500000"])
        with self.db.tx() as c:          # an honest award after the log was read: it must survive the backfill
            c.execute("UPDATE player_xp SET xp = xp + 200 WHERE xuid=8")
        self.assertEqual(self.sc.xp_of(8), 25_007_750.0)                      # dry run wrote nothing (only the +200 above)
        xp_audit.apply(self.db, acc, write=True)
        self.assertEqual(self.sc.xp_of(8), 7750.0)
        self.assertEqual(self.sc.xp_of(9), 300.0)
        self.assertEqual(self.db.one("SELECT xp_base FROM club_members WHERE xuid=8")[0], 7500.0)   # club gain stays the honest 50
        self.assertEqual(self.db.one("SELECT COUNT(*) FROM xp_awards WHERE xuid=8 AND reason LIKE 'backfill%'")[0], 2)


class NotificationTests(Base):
    def setUp(self):
        super().setUp()
        self.n = notifstore.NotificationStore(self.db)

    def test_broadcast_state_is_per_player(self):
        b = self.n.add("hello", "", 0x200)
        self.n.mark_read(1, b["id"])
        self.assertTrue(self.n.visible(1)[0]["read"])
        self.assertFalse(self.n.visible(2)[0]["read"])
        self.n.clear(1, b["id"])
        self.assertEqual(self.n.visible(1), [])
        self.assertEqual(len(self.n.visible(2)), 1)

    def test_addressed_notification_is_private(self):
        a = self.n.add("secret", "", 0x602, xuid=1)
        self.assertEqual(len(self.n.visible(1)), 1)
        self.assertEqual(self.n.visible(2), [])
        self.n.mark_read(2, a["id"])            # not theirs: ignored
        self.assertFalse(self.n.visible(1)[0]["read"])

    def test_gift_claim_is_exactly_once_under_races(self):
        g = self.n.add("", "", 0x8500, {"Amount": 5}, xuid=1, gift_id=True)
        self.assertEqual(g["params"]["GiftId"], g["id"] & 0x7FFFFFFF)
        wins = []
        ts = [threading.Thread(target=lambda: wins.append(self.n.claim(1, g["id"]))) for _ in range(20)]
        [t.start() for t in ts]
        [t.join() for t in ts]
        self.assertEqual(wins.count(True), 1)
        self.assertTrue(self.n.visible(1)[0]["claimed"])

    def test_broadcast_gift_claims_once_per_player(self):
        g = self.n.add("", "", 0x8500, {"Amount": 5}, gift_id=True)
        self.assertTrue(self.n.claim(1, g["id"]))
        self.assertFalse(self.n.claim(1, g["id"]))
        self.assertTrue(self.n.claim(2, g["id"]))

    def test_ids_grow_and_are_never_reused(self):
        a = self.n.add("a", "", 0x200)
        self.n.delete(a["id"])
        b = self.n.add("b", "", 0x200)
        self.assertGreater(b["id"], a["id"])

    def test_delete_removes_receipts(self):
        a = self.n.add("a", "", 0x200)
        self.n.mark_read(1, a["id"])
        self.n.delete(a["id"])
        self.assertEqual(self.db.one("SELECT COUNT(*) FROM notification_receipts")[0], 0)


def template(skill, wristband, tag):
    return {"GamerTag": tag, "Xuid": 0, "IsFriend": True, "UserSkillLevel": skill, "wristbandLevel": wristband,
            "CarId": 1, "UserInfo": {"base": {}, "sideswipeMean": 0.5}, "Curves": [], "Segments": []}


class DrivatarTests(Base):
    def setUp(self):
        super().setUp()
        self.d = object.__new__(drivatar.DrivatarStore)
        self.d.db, self.d.car_override = self.db, {}
        self.d.templates = [template(s, w, "T%d_%d" % (s, w)) for s in range(5) for w in (100, 200, 300)]

    def test_for_player_creates_once_and_follows_gamertag(self):
        r = self.d.for_player(77, "Alice")
        self.assertEqual((r["Xuid"], r["GamerTag"]), (77, "Alice"))
        self.assertEqual(self.d.count(), 1)
        self.assertEqual(self.d.for_player(77, "Alice2")["GamerTag"], "Alice2")
        self.assertEqual(self.d.count(), 1)

    def test_upload_lock_and_wristband(self):
        self.d.for_player(77, "Alice")
        self.d.save_upload(77, "Alice", {"base": {}, "sideswipeMean": 0.9}, [1], [2])
        self.assertEqual(self.d.for_player(77, "Alice")["UserInfo"]["sideswipeMean"], 0.9)
        self.d.edit(77, {"sideswipeMean": "0.1"}, lock=True)
        self.d.save_upload(77, "Alice", {"base": {}, "sideswipeMean": 0.7}, [1], [2])      # locked: ignored
        r = self.d.for_player(77, "Alice")
        self.assertEqual(r["UserInfo"]["sideswipeMean"], 0.1)
        self.assertTrue(r["_locked"])
        self.d.set_wristband(77, 300)
        self.assertEqual(self.d.for_player(77, "Alice")["wristbandLevel"], 300)

    def test_edit_rolls_back_on_bad_field(self):
        self.d.for_player(77, "Alice")
        with self.assertRaises(KeyError):
            self.d.edit(77, {"sideswipeMean": "0.2", "nonsense": "1"})
        self.assertEqual(self.d.for_player(77, "Alice")["UserInfo"]["sideswipeMean"], 0.5)

    def test_pool_is_bounded_and_excludes(self):
        for x in range(1, 6):
            self.d.add_player(x, "P%d" % x)
        out = self.d.pool(4, exclude_xuid=1, exclude_xuids=[2])
        self.assertEqual(len(out), 4)
        self.assertFalse({1, 2} & {r["Xuid"] for r in out})
        self.assertEqual(len(self.d.pool(100)), 5 + len(self.d.templates))

    def test_player_drivatars_do_not_all_share_one_wristband(self):
        low = min(t["wristbandLevel"] for t in self.d.templates)
        for x in range(1, 41):
            self.d.add_player(x, "P%d" % x, wristband=low)
        self.d.edit(7, {"wristbandLevel": str(low)}, lock=True)
        first = {r["Xuid"]: r["wristbandLevel"] for r in self.d.pool(40)}
        self.assertGreaterEqual(len({v for k, v in first.items() if k <= 40}), 10)
        self.assertEqual(first[7], low)
        self.assertEqual({r["Xuid"]: r["wristbandLevel"] for r in self.d.pool(40)}[3], first[3])


if __name__ == "__main__":
    unittest.main()
