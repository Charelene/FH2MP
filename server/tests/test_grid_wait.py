"""The starting grid waits for every connected participant's Console.RenderReady, and the log says who is slow, how slow, and who was left behind.

Prod 2026-11-01 lobby 42557c65: one client reported RenderReady 83 s after the grid, the server had started the race at 45 s, and that client
stayed on the Starting Grid screen. Nothing in the log said how long each client took or who the start left out."""
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fh2mp_server as S  # noqa: E402
from test_freeroam_tag_grant import options, world  # noqa: E402


def grid():
    w, (a, b) = world()
    lines = []
    w.log = lines.append
    w.game_options = options("FR_KING")
    w.start_event(a)
    for q in (a, b):
        q.xuid = 0x9000000000000 + q.owner_id
        q.last_rx = time.monotonic()
        q.console_vars[S.CONSOLE_VAR_HAS_LOADED_CARS] = 1
    w.maybe_begin_race()
    assert w.event["begun"]
    return w, a, b, lines


def ready(w, peer, after):
    w.event["grid_at"] = time.monotonic() - after            # the client reports `after` seconds into the grid
    peer.console_vars[S.CONSOLE_VAR_RENDER_READY] = 1
    w.note_render_ready(peer, 1)
    w.maybe_go_race()


class GridWait(unittest.TestCase):
    def test_a_slow_client_is_waited_for_past_the_old_45_second_limit(self):
        w, a, b, lines = grid()
        ready(w, a, 25.0)
        self.assertFalse(w.event.get("racing"))
        self.assertGreater(w.event["go_at"] - time.monotonic(), 200)          # the backstop, not 45 s
        w.event["grid_at"] -= 83
        w.event["grid_report_at"] = 0
        a.last_rx = b.last_rx = time.monotonic() - 0.4
        w.grid_wait_report(time.monotonic())
        report = [x for x in lines if "waiting for" in x and "after the grid" in x][-1]
        self.assertIn("owner2 xuid 0x9000000000002 last packet 0.4 s ago", report)
        self.assertNotIn("owner1", report.split("waiting for")[1])
        ready(w, b, 83.0)
        self.assertTrue(w.event["racing"])
        go = [x for x in lines if "race starts" in x][-1]
        self.assertIn("(all ready)", go)
        self.assertIn("owner1+25.0s owner2+83.0s", go)
        self.assertNotIn("LEFT BEHIND", go)

    def test_each_ready_report_says_how_long_after_the_grid_and_who_is_missing(self):
        w, a, b, lines = grid()
        ready(w, a, 24.8)
        self.assertIn("grid: owner1 RenderReady 24.8 s after the grid, 1/2 ready, waiting for owner2", lines[-1])

    def test_the_backstop_names_the_client_it_leaves_behind_and_its_late_arrival_is_logged(self):
        w, a, b, lines = grid()
        ready(w, a, 25.0)
        b.last_rx = time.monotonic() - 3.0
        w.go_race("backstop after 240 s")
        go = [x for x in lines if "race starts" in x][-1]
        self.assertIn("LEFT BEHIND owner2 xuid 0x9000000000002 (last packet 3.0 s ago)", go)
        w.event["go_time"] -= 38
        b.console_vars[S.CONSOLE_VAR_RENDER_READY] = 1
        w.note_render_ready(b, 1)
        self.assertIn("38.0 s AFTER the race started without it", lines[-1])
        w.note_render_ready(b, 1)
        self.assertEqual(sum("AFTER the race started" in x for x in lines), 1)                      # said once

    def test_a_client_that_restarts_its_pre_race_flow_is_noticed(self):
        w, a, b, lines = grid()
        ready(w, a, 20.0)
        a.console_vars[S.CONSOLE_VAR_RENDER_READY] = 0
        w.note_render_ready(a, 0)
        self.assertIn("owner1 cleared RenderReady", lines[-1])
        self.assertIn("it was ready after 20.0 s", lines[-1])

    def test_reports_are_rate_limited(self):
        w, a, b, lines = grid()
        w.event["grid_report_at"] = 0
        w.grid_wait_report(time.monotonic())
        n = len(lines)
        for _ in range(20):
            w.grid_wait_report(time.monotonic())
        self.assertEqual(len(lines), n)


if __name__ == "__main__":
    unittest.main()
