"""The online event results the game server reports for the website (fh2mp_server.py report_event -> the LSP's /admin/result):
finish order and winner, teams, rentals, and the King / Infection crowns, infections and crown times."""
import os
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S  # noqa: E402
from test_freeroam_tag_grant import options, world  # noqa: E402


class Reports(unittest.TestCase):
    def setUp(self):
        self.got = []
        self.old, S.RESULT_SINK = S.RESULT_SINK, lambda mode, detail, log: self.got.append((mode, detail))
        self.w, (self.a, self.b) = world()
        for i, q in enumerate((self.a, self.b)):
            q.xuid, q.model_id = 0x900000000001 + i, 7 + i
        self.w.set_session_host_bool = lambda *x: None
        self.w.tag_send = lambda why: None

    def tearDown(self):
        S.RESULT_SINK = self.old

    def results(self):
        for t in threading.enumerate():
            if t.name == "event-result":
                t.join(2)
        return self.got

    def race(self, **extra):
        self.w.event = dict(people=[self.a, self.b], racing=True, finished={}, over=False, numlaps=None, laps={self.a: 3, self.b: 3},
                            racing_since=time.monotonic() - 100, **extra)

    def test_race_winner_is_the_first_finisher(self):
        self.race()
        self.b.rental_car = 8
        self.w.note_score_keyframe(self.b)
        self.w.note_score_keyframe(self.a)
        self.w.check_race_end(time.monotonic())
        [(mode, d)] = self.results()
        self.assertEqual((mode, d["winner"], d["end"]), ("race", self.b.xuid, "everybody finished"))
        self.assertEqual([(p["xuid"], p["car"], p["rental"], p["finish"]) for p in d["players"]],
                         [(self.a.xuid, 7, False, 2), (self.b.xuid, 8, True, 1)])
        self.assertAlmostEqual(d["seconds"], 100, delta=2)

    def test_team_event_names_the_teams(self):
        self.race(team={self.a: 0, self.b: 1})
        self.w.note_score_keyframe(self.a)
        self.w.note_score_keyframe(self.b)
        self.w.check_race_end(time.monotonic())
        [(mode, d)] = self.results()
        self.assertEqual((mode, d["winner"], [p["team"] for p in d["players"]]), ("team", self.a.xuid, [0, 1]))

    def test_solo_events_are_not_reported(self):
        self.a.xuid = 0                                    # a client that never said who it is
        self.race()
        self.w.end_race("time limit")
        self.assertEqual(self.results(), [])
        self.assertTrue(self.w.event["over"])

    def test_a_failing_report_never_blocks_the_end(self):
        self.race()
        self.w.report_event = lambda why: 1 / 0
        self.w.end_race("time limit")
        self.assertTrue(self.w.event["over"])

    def tag_round(self, name):
        w, a, b = self.w, self.a, self.b
        w.game_options = options("FR_KING")
        w.start_event(a)
        tag = w.event["tag"]
        if name == "Infection":
            tag["mode"], tag["guessed"] = S.TAG_MODES[S.RACE_MODE_INFECTION], False
        now = time.monotonic()
        w.event.update(racing=True, racing_since=now - 100, finished={})
        tag["assigned"], tag["first"], tag["assign_at"] = True, [a.xuid], now - 100
        tag["buckets"] = {a: S.TAG_BUCKET_IT, b: S.TAG_BUCKET_NOT_IT}
        tag["go_at"], tag["marked"] = now - 100, now - 30                # a has held the crown for the last 30 s
        return tag

    def test_king_crowns_taken_and_crown_time(self):
        tag = self.tag_round("King")
        self.w.tag_collision(self.a, ("own", 0, 0), ("relay", 1, 2), 1, 0, 0, 1)      # b takes the crown
        tag["marked"] -= 50                                                            # and keeps it 50 s
        self.w.end_race("time limit")
        [(mode, d)] = self.results()
        self.assertEqual((mode, d["hits"]), ("king", [[self.b.xuid, 1]]))
        held = dict(d["held"])
        self.assertAlmostEqual(held[self.a.xuid], 30, delta=1)
        self.assertAlmostEqual(held[self.b.xuid], 50, delta=1)
        self.assertEqual((d["winner"], d["first"], d["survivors"]), (self.b.xuid, [self.a.xuid], []))

    def test_infection_infections_and_survivors(self):
        self.tag_round("Infection")
        self.w.end_race("time limit")
        [(mode, d)] = self.results()
        self.assertEqual((mode, d["everyone"], d["survivors"], d["hits"]), ("infection", False, [self.b.xuid], []))
        self.got.clear()
        self.tag_round("Infection")
        self.w.tag_collision(self.a, ("own", 0, 0), ("relay", 1, 2), 1, 1, 0, 1)      # a infects b: everybody is It
        self.w.check_race_end(time.monotonic())
        [(mode, d)] = self.results()
        self.assertEqual((d["everyone"], d["survivors"], d["hits"]), (True, [], [[self.a.xuid, 1]]))


if __name__ == "__main__":
    unittest.main()
