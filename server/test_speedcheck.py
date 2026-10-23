"""The server-side speed and flight checks on relayed car positions (fh2mp_server.Server.track_speed)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S  # noqa: E402


class Clock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t


class SpeedCheck(unittest.TestCase):
    def setUp(self):
        import test_freeroam_tag_grant as T
        self.w, (self.a, self.b) = T.world()
        self.clock = Clock()
        self.real, S.time.monotonic = S.time.monotonic, self.clock
        self.addCleanup(lambda: setattr(S.time, "monotonic", self.real))
        self.a.model_id = 1535                             # Lexus GS 350: stock 286 km/h
        self.pos = [0.0, 100.0, 0.0]

    def drive(self, seconds, dx_kmh=0.0, dy_kmh=0.0, step=1.0):
        for _ in range(int(seconds / step)):
            self.clock.t += step
            self.pos[0] += dx_kmh / 3.6 * step
            self.pos[1] += dy_kmh / 3.6 * step
            self.w.track_speed(self.a, tuple(self.pos))

    def test_a_legal_speed_is_left_alone(self):
        self.drive(10, dx_kmh=300)
        self.assertIsNone(self.a.kick_reason)

    def test_above_the_hard_cap_for_three_seconds_is_removed(self):
        self.drive(2, dx_kmh=500)
        self.assertIsNone(self.a.kick_reason)              # not yet
        self.drive(3, dx_kmh=500)
        self.assertIn("speed", self.a.kick_reason)

    def test_a_respawn_or_fast_travel_is_not_speed(self):
        self.drive(2, dx_kmh=200)
        self.pos[0] += 5000                                # a jump of 5 km in a second
        self.drive(1, dx_kmh=200)
        self.drive(4, dx_kmh=200)
        self.assertIsNone(self.a.kick_reason)

    def test_climbing_for_three_seconds_is_flying(self):
        self.drive(4, dx_kmh=100, dy_kmh=200)              # 200 km/h straight up while driving
        self.assertIn("flying", self.a.kick_reason or "")
        self.assertGreater(self.a.top_climb, 150)

    def test_a_ramp_jump_and_a_steep_hill_are_not_flying(self):
        self.drive(1, dx_kmh=200, dy_kmh=120)              # launched off a ramp: about 120 km/h up for a second
        self.drive(1, dx_kmh=200, dy_kmh=40)
        self.drive(1, dx_kmh=200, dy_kmh=-60)
        self.drive(10, dx_kmh=150, dy_kmh=50)              # a long mountain climb
        self.assertIsNone(self.a.kick_reason)

    def test_log_only_policy_reports_but_keeps_the_player(self):
        old = S.SPEED_POLICY
        S.SPEED_POLICY = "log"
        try:
            self.drive(5, dx_kmh=500)
            self.drive(5, dx_kmh=0, dy_kmh=300)
        finally:
            S.SPEED_POLICY = old
        self.assertIsNone(self.a.kick_reason)
        self.assertIn("hard", self.a.speed_flagged)
        self.assertIn("flight", self.a.speed_flagged)


if __name__ == "__main__":
    unittest.main()
