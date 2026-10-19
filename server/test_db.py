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


class ScoreTests(Base):
    def setUp(self):
        super().setUp()
        self.sc = scoreboard.ScoreStore(self.db, ["A", "B", "C"])

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


if __name__ == "__main__":
    unittest.main()
