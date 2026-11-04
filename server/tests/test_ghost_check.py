"""The rules that judge a run from its ghost, and what the server does with a verdict. Made-up ghosts, server side only."""
import math
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from test_ghost import blob, drive, stream
from test_ops import make_lsp
import ghost_check
import score_reports
import scoreboard


CAR = 1398


def raw_sub(score, sb=1, gsid=0, track=866, cls=2, cpi=0.55, car=CAR):
    return {"scoreboardType": sb, "gameScoreboardId": gsid, "trackId": track, "carId": car, "carClassId": cls, "carPerformanceIndex": cpi,
            "score": scoreboard.dbl_to_u64(score), "isClean": True, "usedSTM": False, "usedTCS": False, "usedABS": True}


def header(score, car=CAR):
    h = bytearray(4800)
    struct.pack_into("<d", h, 2087, score)
    struct.pack_into("<I", h, 3200, car)
    return bytes(h)


def honest(seconds=30.0, speed=45.0, score=None, car=CAR):
    """A steady lap with a gentle launch and a corner: nothing about it is remarkable."""
    pts = []
    x, z, heading = 0.0, 0.0, 0.0
    for i in range(int(seconds * 5) + 1):
        t = i / 5
        v = min(speed, 8.0 * t + 1)
        heading += 0.02
        x += v / 5 * math.cos(heading)
        z += v / 5 * math.sin(heading)
        pts.append((t, x, 40.0 + math.sin(t), z))
    return blob(header(pts[-1][0] if score is None else score, car), stream(pts))


def verdict(data, score=30.0, car=CAR):
    return ghost_check.check(data, score, car)


class Rules(unittest.TestCase):
    def test_an_ordinary_lap_passes(self):
        v, why, m = verdict(honest())
        self.assertEqual((v, why), (ghost_check.PASS, []))
        self.assertAlmostEqual(m["speed"], 45.0, delta=1)
        self.assertLess(m["acceleration"], 10)

    def assert_hard(self, data, word, **kw):
        v, why, _ = verdict(data, **kw)
        self.assertEqual(v, ghost_check.HARD, why)
        self.assertTrue(any(word in w for w in why), why)

    def test_unreadable(self):
        self.assert_hard(b"rubbish", "unreadable")

    def test_a_time_the_ghost_does_not_end_at(self):
        self.assert_hard(honest(score=30.0), "ends at", score=20.0)
        self.assert_hard(blob(header(30.0), stream(drive(30.0))), "ends at", score=60.0)

    def test_a_time_missing_from_the_header(self):
        self.assert_hard(honest(score=12.0), "header", score=30.0)

    def test_a_gap_in_the_recording(self):
        pts = [p for p in drive(30.0) if not 10 < p[0] < 14]
        self.assert_hard(blob(header(30.0), stream(pts)), "missing between frames")

    def test_a_ghost_with_almost_no_driving(self):
        self.assert_hard(blob(header(30.0), stream([(0, 0, 0, 0), (30.0, 1, 0, 0)])), "too little driving")

    def test_a_speed_no_car_reaches(self):
        self.assert_hard(blob(header(30.0), stream(drive(30.0, speed=134.112))), "speed")

    def test_climbing_like_an_aircraft(self):
        pts = [(i / 5, 3.0 * i, 40.0 + 8.0 * i, 0.0) for i in range(151)]
        self.assert_hard(blob(header(30.0), stream(pts)), "climb")

    def test_a_power_no_car_has(self):
        pts, v, x = [], 45.0, 0.0
        for i in range(151):
            pts.append((i / 5, x, 40.0, 0.0))
            v = 45.0 if v > 110 else v + 6.0         # 30 m/s2 at speed, over and over
            x += v / 5
        self.assert_hard(blob(header(30.0), stream(pts)), "power")

    def test_a_run_of_jumps_across_the_map(self):
        pts = [(i / 5, 8.0 * i + (i // 12 % 2) * 900.0, 40.0, 0.0) for i in range(151)]
        self.assert_hard(blob(header(30.0), stream(pts)), "jumps across the map")

    def test_one_jump_is_a_track_reset_not_a_cheat(self):
        pts = [(t, x + (600.0 if t > 15 else 0.0), y, z) for t, x, y, z in drive(30.0)]
        v, why, m = verdict(blob(header(30.0), stream(pts)))
        self.assertEqual(m["teleports"], 1)
        self.assertNotEqual(v, ghost_check.HARD, why)

    def test_a_car_missing_from_the_header_is_only_flagged(self):
        v, why, _ = verdict(honest(car=999))       # a real run of the maintainer's does this
        self.assertEqual(v, ghost_check.FLAG)
        self.assertIn("car", why[0])

    def test_near_the_limit_is_flagged_not_hidden(self):
        v, why, _ = verdict(blob(header(30.0), stream(drive(30.0, speed=123.0))))
        self.assertEqual(v, ghost_check.FLAG, why)


class Flow(unittest.TestCase):
    def setUp(self):
        self.lsp = make_lsp()
        self.addCleanup(self.lsp.DB.c.close)
        self.sc = self.lsp.SB
        self.sc.ghost_policy = "enforce"

    def run_of(self, xuid, tag, score=30.0, **kw):
        s = self.sc.submit(xuid, tag, 1, raw_sub(score, **kw))
        return s

    def board(self, viewer=None, **kw):
        return [r["tag"] for r in self.sc._best_per_player(1, 1, kw.get("track", 866), pivot=kw.get("pivot", -1), viewer=viewer).values()]

    def verdict_of(self, sid):
        return self.sc.db.one("SELECT verdict, reasons, reviewed FROM score_verdicts WHERE submission_id=?", (sid,))

    def test_a_run_waits_for_its_ghost_and_only_its_owner_sees_it(self):
        s = self.run_of(1, "A")
        self.assertEqual(self.board(), [])
        self.assertEqual(self.board(viewer=1), ["A"])
        self.assertIsNotNone(self.sc.best_of(1, 1, 1, 866))
        self.assertIsNone(self.sc.best_of(2, 1, 1, 866))

    def test_a_good_ghost_makes_the_run_public(self):
        s = self.run_of(1, "A")
        self.assertEqual(self.sc.attach_ghost(s["score_id"], honest(), 1), (s["id"], True))
        self.assertEqual(self.board(), ["A"])
        self.assertEqual(self.verdict_of(s["id"])["verdict"], ghost_check.PASS)

    def test_a_hard_verdict_keeps_it_hidden_and_keeps_the_data(self):
        s = self.run_of(1, "A")
        data = honest(score=12.0)
        self.assertEqual(self.sc.attach_ghost(s["score_id"], data, 1), (s["id"], False))
        self.assertEqual(self.board(), [])
        self.assertEqual(self.board(viewer=1), [])
        row = self.sc.db.one("SELECT source, reason FROM hidden_scores WHERE submission_id=?", (s["id"],))
        self.assertEqual(row["source"], "auto")
        self.assertIn("header", row["reason"])
        self.assertIsNotNone(self.sc.db.one("SELECT 1 FROM score_ghosts WHERE submission_id=?", (s["id"],)))

    def test_a_flagged_run_shows_and_reaches_the_review_queue(self):
        s = self.run_of(1, "A")
        self.assertTrue(self.sc.attach_ghost(s["score_id"], honest(car=999), 1)[1])
        self.assertEqual(self.board(), ["A"])
        e = score_reports.queue(self.lsp)["entries"][0]
        self.assertEqual((e["submissionId"], e["verdict"], e["hidden"]), (s["id"], "flag", False))
        score_reports.moderate(self.lsp, s["id"], "dismiss")
        self.assertEqual(score_reports.queue(self.lsp)["entries"], [])

    def test_a_pending_run_is_no_decision_for_the_queue(self):
        self.run_of(1, "A")
        self.assertEqual(score_reports.queue(self.lsp)["entries"], [])

    def test_a_restored_run_stays_shown_through_a_rescan(self):
        s = self.run_of(1, "A")
        self.sc.attach_ghost(s["score_id"], honest(score=12.0), 1)
        score_reports.moderate(self.lsp, s["id"], "unhide")
        self.assertEqual(self.board(), ["A"])
        with self.sc.db.tx() as c:
            self.sc.store_verdict(c, s["id"], ghost_check.HARD, ["again"], {}, 1.0)
        self.assertEqual(self.board(), ["A"])

    def test_hiding_a_pending_run_outlasts_its_ghost(self):
        s = self.run_of(1, "A")
        score_reports.moderate(self.lsp, s["id"], "hide")
        self.sc.attach_ghost(s["score_id"], honest(), 1)
        self.assertEqual(self.board(), [])

    def test_only_the_owner_uploads_and_the_first_upload_stands(self):
        s = self.run_of(1, "A")
        self.assertEqual(self.sc.attach_ghost(s["score_id"], honest(), 2), (None, False))
        data = honest()
        self.assertEqual(self.sc.attach_ghost(s["score_id"], data, 1)[0], s["id"])
        self.assertEqual(self.sc.attach_ghost(s["score_id"], data, 1), (s["id"], False))         # a retry
        self.assertEqual(self.sc.attach_ghost(s["score_id"], honest(seconds=31), 1), (None, False))
        self.assertEqual(self.sc.ghost(s["id"]), data)

    def test_an_oversized_ghost_is_refused(self):
        s = self.run_of(1, "A")
        self.assertEqual(self.sc.attach_ghost(s["score_id"], b"\0" * (1 << 21), 1), (None, False))

    def test_a_hidden_run_serves_no_ghost(self):
        s = self.run_of(1, "A")
        data = honest(score=12.0)
        self.sc.attach_ghost(s["score_id"], data, 1)
        self.assertIsNone(self.sc.public_ghost(s["id"]))
        self.assertEqual(self.sc.ghost(s["id"]), data)           # kept for review

    def test_log_mode_records_a_verdict_and_hides_nothing(self):
        self.sc.ghost_policy = "log"
        s = self.run_of(1, "A")
        self.assertEqual(self.board(), ["A"])
        self.sc.attach_ghost(s["score_id"], honest(score=12.0), 1)
        self.assertEqual(self.board(), ["A"])
        self.assertEqual(self.verdict_of(s["id"])["verdict"], ghost_check.HARD)

    def test_an_empty_board_never_answers_with_an_all_zero_rival(self):
        s = self.run_of(1, "A", track=900)
        self.sc.attach_ghost(s["score_id"], honest(score=12.0), 1)
        self.assertTrue(self.sc.rival_for(2, 1, 1, 900, 0)["score"] > 0)


class AuditExploits(unittest.TestCase):
    """What an honest-looking client could post before (2026-10-30), all accepted and listed then."""

    def setUp(self):
        self.lsp = make_lsp()
        self.addCleanup(self.lsp.DB.c.close)
        self.sc = self.lsp.SB
        self.sc.ghost_policy = "enforce"
        self.sc.route_length = lambda track: 3000.0 if track == 866 else None
        self.sc.event_info = lambda e: {"attrs": {"CarClassId": 5}} if e == 777 else None

    def judge(self, s, data):
        self.sc.attach_ghost(s["score_id"], data, s["xuid"])
        return self.sc.db.one("SELECT verdict, reasons FROM score_verdicts WHERE submission_id=?", (s["id"],))

    def test_a_thousandth_of_a_second_on_a_festival_route(self):
        s = self.sc.submit(1, "A", 1, raw_sub(0.001))
        row = self.judge(s, honest())
        self.assertEqual(row["verdict"], ghost_check.HARD)
        self.assertIn("faster than", row["reasons"])

    def test_a_time_under_the_route_limit_needs_no_ghost_analysis(self):
        s = self.sc.submit(1, "A", 1, raw_sub(20.0))
        self.assertEqual(self.judge(s, honest(score=20.0))["verdict"], ghost_check.HARD)

    def test_half_a_second_with_pi_1_5_on_an_s2_event(self):
        s = self.sc.submit(1, "A", 1, raw_sub(0.5, sb=3, gsid=777, cls=5, cpi=1.5))
        row = self.judge(s, honest())
        self.assertEqual(row["verdict"], ghost_check.HARD)
        self.assertIn("does not belong", row["reasons"])

    def test_every_class_band_accepts_what_real_runs_report(self):
        for cls, pi in ((0, 0.1769), (0, 0.4345), (1, 0.4379), (2, 0.6179), (3, 0.6182), (4, 0.8464), (5, 0.8465), (5, 0.9971), (6, 1.0), (7, 0.9999)):
            r = dict(id=1, xuid=1, sb=6, st=1, gsid=1, track=0, car=CAR, cls=cls, cpi=pi, score=30.0)
            self.assertEqual(self.sc.plausibility(r)[0], [], (cls, pi))

    def test_a_ghost_cannot_be_swapped_after_the_fact(self):
        s = self.sc.submit(1, "A", 1, raw_sub(60.0, track=1))
        first = honest(score=60.0, seconds=60.0)
        self.judge(s, first)
        self.assertEqual(self.sc.attach_ghost(s["score_id"], b"B" * 8, 1), (None, False))
        self.assertEqual(self.sc.ghost(s["id"]), first)

    def test_fifty_top_xp_awards_in_a_row_are_reported(self):
        for _ in range(50):
            self.sc.add_xp(1, 560000)
        reasons = [r["reason"] for r in self.sc.db.query("SELECT reason FROM xp_awards")]
        self.assertEqual(sum("budget" in r for r in reasons), 47)


class Handlers(unittest.TestCase):
    """The game's own calls into the store."""

    def setUp(self):
        import threading
        from unittest import mock
        import fh2mp_lsp as L
        self.L, self.lsp = L, make_lsp()
        self.addCleanup(self.lsp.DB.c.close)
        self.sc = self.lsp.SB
        self.sc.ghost_policy = "enforce"
        self.sent = []
        ctx = threading.local()
        ctx.xuid, ctx.own_cars = 1, False
        for name, value in (("SB", self.sc), ("CTX", ctx), ("AUTH", None), ("TAGS", {1: "A", 2: "B"}), ("log", lambda m, *a: None),
                            ("event_info", lambda e: {"id": e} if e in (7, 8) else None), ("send_beaten", lambda *a: self.sent.append(a))):
            p = mock.patch.object(L, name, value)
            p.start()
            self.addCleanup(p.stop)
        self.ctx = ctx

    def event_run(self, xuid, score, gsid=7, good=True):
        s = self.sc.submit(xuid, "AB"[xuid - 1], 1, raw_sub(score, sb=3, gsid=gsid, track=0))
        self.sc.attach_ghost(s["score_id"], honest(seconds=score) if good else honest(seconds=score, score=score / 2), xuid)
        return s

    def notify(self, s, rival=2, event=7):
        self.L.sb_notify_rival({"myScoreId": s["score_id"], "rivalXuid": rival, "rivalEventId": event})
        return len(self.sent)

    def test_a_real_win_tells_the_rival(self):
        self.event_run(2, 40.0)
        self.assertEqual(self.notify(self.event_run(1, 30.0)), 1)

    def test_no_notice_for_a_run_that_does_not_beat_them(self):
        self.event_run(2, 30.0)
        self.assertEqual(self.notify(self.event_run(1, 40.0)), 0)

    def test_no_notice_on_another_event_or_to_a_player_without_a_time(self):
        self.event_run(2, 40.0)
        mine = self.event_run(1, 30.0)
        self.assertEqual((self.notify(mine, event=8), self.notify(mine, rival=3)), (0, 0))

    def test_no_notice_from_somebody_elses_run_or_a_hidden_one(self):
        self.event_run(2, 40.0)
        theirs = self.event_run(1, 30.0)
        self.ctx.xuid = 3
        self.assertEqual(self.notify(theirs), 0)
        self.ctx.xuid = 1
        self.assertEqual(self.notify(self.event_run(1, 20.0, good=False)), 0)

    def test_the_beaten_notice_waits_for_the_ghost(self):
        beaten = []
        self.L.notify_beaten, keep = lambda *a: beaten.append(a), self.L.notify_beaten
        self.addCleanup(setattr, self.L, "notify_beaten", keep)
        s = self.sc.submit(1, "A", 1, raw_sub(30.0, sb=3, gsid=7, track=0))
        self.L.CTX.xuid = 1
        self.L.sb_upload_file({"scoreId": s["score_id"], "data": honest()})
        self.assertEqual(len(beaten), 1)

    def test_a_hidden_run_downloads_empty(self):
        s = self.event_run(1, 30.0, good=False)
        self.assertEqual(self.L.sb_download_file({"id": s["id"], "maxSize": 1 << 20}), {"data": b""})


if __name__ == "__main__":
    unittest.main()
