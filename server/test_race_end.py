"""Free roam circuit races end when every player's final score (MsgScoreKeyframe) has arrived, or 30 s after the first one.

Seen on prod (3-lap circuit): the first finisher's last LapScore came with a ScoreKeyframe, the player who was cut off by "race ends in"
sent one too, and nothing earlier did. The lap count of a free roam event is not decoded, so the keyframe is the only end signal; before it
the server never set SessionHost.InRace=0 and the clients stayed on the results screen."""
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S  # noqa: E402


class FakeSock:
    def __getattr__(self, name):
        return lambda *a, **k: 0


def world(event):
    w = S.Server(0, None, sock=FakeSock(), log_file=None)
    peers = [S.Peer(("10.0.0.%d" % i, 5000 + i)) for i in (1, 2)]
    for p in peers:
        p.connected = True
        w.peers[p.addr] = p
    w.event = dict(event, people=list(peers), racing=True, finished={}, over=False)
    return w, peers


class FreeRoamRaceEnd(unittest.TestCase):
    def test_first_finisher_alone_does_not_end_the_race(self):
        w, (a, b) = world({"numlaps": None})
        w.note_score_keyframe(a)
        w.check_race_end(time.monotonic())
        self.assertFalse(w.event["over"])
        self.assertIn(a, w.event["finished"])

    def test_everybody_done_ends_it_and_releases_the_clients(self):
        w, (a, b) = world({"numlaps": None})
        sent = []
        w.set_session_host_bool = lambda var, value, why: sent.append((var, value, why))
        w.note_score_keyframe(a)
        w.note_score_keyframe(b)                       # the DNF'd player's final score
        w.check_race_end(time.monotonic())
        self.assertTrue(w.event["over"])
        self.assertIn((S.SH_IN_RACE, 0), [(v, x) for v, x, _ in sent])   # InRace=0: what the clients' results screen waits for

    def test_time_limit_after_first_finisher(self):
        w, (a, b) = world({"numlaps": None})
        w.set_session_host_bool = lambda *x: None
        w.note_score_keyframe(a)
        w.event["finished"][a] -= S.RACE_LAST_FINISHER_SECONDS + 1
        w.check_race_end(time.monotonic())
        self.assertTrue(w.event["over"])

    def test_keyframe_counts_once(self):
        w, (a, b) = world({"numlaps": None})
        w.note_score_keyframe(a)
        first = w.event["finished"][a]
        time.sleep(0.01)
        w.note_score_keyframe(a)
        self.assertEqual(w.event["finished"][a], first)

    def test_road_trip_and_tag_events_keep_their_own_rules(self):
        w, (a, b) = world({"numlaps": 3})              # Road Trip: lap table decides, a keyframe must not end it early
        w.note_score_keyframe(a)
        self.assertEqual(w.event["finished"], {})
        w, (a, b) = world({"numlaps": None, "tag": {"mode": S.TAG_MODES[S.RACE_MODE_KING]}})
        w.note_score_keyframe(a)                       # a keyframe right after GO is not the end of a King / Infection event
        self.assertEqual(w.event["finished"], {})

    def test_not_racing_or_not_a_participant_is_ignored(self):
        w, (a, b) = world({"numlaps": None})
        w.event["racing"] = False
        w.note_score_keyframe(a)
        self.assertEqual(w.event["finished"], {})
        w.event["racing"] = True
        outsider = S.Peer(("10.0.0.9", 5009))
        w.note_score_keyframe(outsider)
        self.assertEqual(w.event["finished"], {})
        w.event = None
        w.note_score_keyframe(a)                       # no event at all: no crash




class LeaderLeavesBeforeStart(unittest.TestCase):
    """Prod 2026-10-21 19:04 (lobby 84dcc3a1): the King leader went silent during the invite window, was removed, and the event still
    started with it as participant 9; the other four sat on "loading" until they quit."""

    def test_pending_event_is_cancelled_and_never_starts(self):
        w, (leader, other) = world({"numlaps": None})
        w.event = None
        sent = []
        w.set_session_host_bool = lambda var, value, why: sent.append((var, value))
        w.pending_race = (time.monotonic() - 1, leader)
        w.drop_peer(leader, "no packets for 20 s")
        self.assertFalse(leader.connected)
        self.assertIsNone(w.pending_race)
        self.assertIsNone(w.event)
        self.assertIn((S.SH_RACE_READY, 0), sent)     # the invitees are released

    def test_other_client_leaving_keeps_the_pending_event(self):
        w, (leader, other) = world({"numlaps": None})
        w.event = None
        w.pending_race = (time.monotonic() + 5, leader)
        w.drop_peer(other, "no packets for 20 s")
        self.assertFalse(other.connected)
        self.assertIs(w.pending_race[1], leader)


class OneEventAtATime(unittest.TestCase):
    """Prod 2026-10-22: 61 multi-player events started over a running one; 50 % of them never began (7 % otherwise)."""

    def test_request_during_a_running_event_is_blocked(self):
        w, (a, b) = world({"numlaps": None})
        outsider = S.Peer(("10.0.0.9", 5009))
        self.assertTrue(w.request_race_blocked(outsider))
        w.event["begun"] = False
        w.event["racing"] = False                     # still loading: blocked as well
        self.assertTrue(w.request_race_blocked(outsider))

    def test_finished_event_does_not_block_and_is_closed(self):
        w, (a, b) = world({"numlaps": None})
        w.set_session_host_bool = lambda *x: None
        w.event["over"] = True
        a.console_vars[3] = 1                         # still on the results screen
        outsider = S.Peer(("10.0.0.9", 5009))
        self.assertFalse(w.request_race_blocked(outsider))
        self.assertEqual(w.close_finished_event(), {a})
        self.assertIsNone(w.event)

    def test_abandoned_event_does_not_lock_the_lobby(self):
        w, (a, b) = world({"numlaps": None})
        a.connected = b.connected = False
        self.assertFalse(w.request_race_blocked(S.Peer(("10.0.0.9", 5009))))
        w, (a, b) = world({"numlaps": None, "started": time.monotonic() - S.EVENT_BLOCK_MAX_SECONDS - 1})
        self.assertFalse(w.request_race_blocked(S.Peer(("10.0.0.9", 5009))))

    def test_second_request_in_the_invite_window_is_blocked_but_not_the_leaders_own(self):
        w, (a, b) = world({"numlaps": None})
        w.event = None
        w.pending_race = (time.monotonic() + 5, a)
        self.assertTrue(w.request_race_blocked(b))
        self.assertFalse(w.request_race_blocked(a))


class LoadTimeout(unittest.TestCase):
    def test_participant_that_never_loads_is_dropped_and_the_rest_begin(self):
        w, (a, b) = world({"numlaps": None, "begun": False, "started": time.monotonic() - S.EVENT_LOAD_TIMEOUT_SECONDS - 1})
        w.event["racing"] = False
        w.set_session_host_bool = lambda *x: None
        w.set_console_bool = lambda *x: None
        a.console_vars[S.CONSOLE_VAR_HAS_LOADED_CARS] = 1          # b hung while loading
        w.tick()
        self.assertEqual(w.event["people"], [a])
        self.assertTrue(w.event["begun"])

    def test_nobody_loaded_ends_the_event(self):
        w, (a, b) = world({"numlaps": None, "begun": False, "started": time.monotonic() - S.EVENT_LOAD_TIMEOUT_SECONDS - 1})
        w.event["racing"] = False
        w.set_session_host_bool = lambda *x: None
        w.set_console_bool = lambda *x: None
        w.tick()
        self.assertIsNone(w.event)

    def test_no_timeout_before_the_limit_or_after_the_grid_started(self):
        w, (a, b) = world({"numlaps": None, "begun": False, "started": time.monotonic()})
        w.event["racing"] = False
        w.tick()
        self.assertEqual(len(w.event["people"]), 2)
        w.event["begun"] = True
        w.event["started"] -= 1000
        w.tick()
        self.assertEqual(len(w.event["people"]), 2)


if __name__ == "__main__":
    unittest.main()
