"""Free roam circuit races end when every player's final score (MsgScoreKeyframe) has arrived, or 30 s after the first one.

Seen on prod (3-lap circuit): the first finisher's last LapScore came with a ScoreKeyframe, the player who was cut off by "race ends in"
sent one too, and nothing earlier did. Events with a decoded lap count now use LapScore for completion;
the keyframe fallback remains for events without a lap count. These are race-state unit tests;
test_race_flow covers the transport, setup and result transitions together."""
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
    w.event = dict(event, people=list(peers), racing=True, finished={}, over=False, laps={p: 3 for p in peers})
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

    def test_a_player_out_of_the_race_does_not_start_the_end_timer(self):
        """Prod 2026-10-22 22:18:19 lobby c0759261: a participant sat at the finish line in a car the A-class event did not allow; its
        final score 15 s after GO started "30 s after the first finisher" and ended a 3-lap race at 45 s for everybody."""
        w, (a, b) = world({"numlaps": None})
        w.set_session_host_bool = lambda *x: None
        w.event["laps"] = {a: 0}                                   # a never completed a lap
        w.note_score_keyframe(a)
        w.event["finished"][a] -= S.RACE_LAST_FINISHER_SECONDS + 100
        w.check_race_end(time.monotonic())
        self.assertFalse(w.event["over"])
        w.event["laps"][b] = 3                                     # b really finishes: everybody is done
        w.note_score_keyframe(b)
        w.check_race_end(time.monotonic())
        self.assertTrue(w.event["over"])

    def test_lap_scores_count_the_laps(self):
        w, (a, b) = world({"numlaps": None})
        w.event["laps"] = {}
        one = int.from_bytes(S.struct.pack("<d", 60.0), "little")
        w.note_lap_score(a, 2, [int.from_bytes(S.struct.pack("<d", 130.0), "little"), one, one])
        self.assertEqual(w.event["laps"][a], 2)

    def test_south_scramble_elapsed_time_is_not_the_last_lap(self):
        # Prod 2026-10-25 23:31–23:34 UTC, lobby 56614b63. These elapsed
        # times match the player's result screen: 2:34.088 and 2:38.367.
        w, (a, b) = world({"numlaps": 2})
        messages = []
        w.log = messages.append
        def score(peer, lap, times):
            w.note_lap_score(peer, lap, [int.from_bytes(S.struct.pack("<d", t), "little") for t in times])
        score(a, 0, (3.177, 0, 0))
        score(a, 1, (79.331, 76.154, 76.154))
        score(b, 1, (83.367, 81.668, 81.668))
        self.assertEqual(w.event["finished"], {})
        score(a, 2, (154.088, 74.755, 74.755))
        self.assertNotIn(b, w.event["finished"])
        score(b, 2, (158.367, 75, 75))
        self.assertEqual(len(w.event["finished"]), 2)
        self.assertIn("finished in 154.088 s", messages[0])
        self.assertIn("finished in 158.367 s", messages[1])

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


class LeavingTheEvent(unittest.TestCase):
    def test_a_player_who_leaves_while_racing_is_no_longer_a_participant_for_anybody(self):
        w, (a, b) = world({"numlaps": None})
        writes = []
        w.set_console_bool = lambda q, var, value, why: writes.append((q, var, value, why))
        w.set_session_host_bool = lambda *x: None
        w.participant_left(b)
        self.assertEqual(w.event["people"], [a])
        self.assertEqual(writes, [(b, S.CONSOLE_VAR_PARTICIPATING_IN_RACE, 0, "left the event")])

    def test_a_finished_player_leaving_the_results_keeps_the_flag(self):
        w, (a, b) = world({"numlaps": None})
        writes = []
        w.set_console_bool = lambda *x: writes.append(x)
        w.set_session_host_bool = lambda *x: None
        w.event["finished"][b] = time.monotonic()
        w.participant_left(b)
        self.assertEqual(writes, [])
        self.assertEqual(w.event["people"], [a])

    def test_the_last_one_leaving_ends_the_event(self):
        w, (a, b) = world({"numlaps": None})
        w.set_console_bool = lambda *x: None
        w.set_session_host_bool = lambda *x: None
        w.participant_left(a)
        w.participant_left(b)
        self.assertIsNone(w.event)


class InviteHold(unittest.TestCase):
    def test_a_client_starting_its_own_event_does_not_hold_the_invite(self):
        """Prod 2026-10-22 21:54:24: two requests in one second; the server held everybody 90 s for the second requester."""
        w, (leader, other) = world({"numlaps": None})
        w.event = None
        w.set_session_host_bool = lambda *x: None
        started = []
        w.start_event = lambda q: started.append(q)
        now = time.monotonic()
        leader.entered = other.entered = True
        other.console_vars[3] = 1                                    # RaceInProgress, not Ready
        w.race_requested_at, w.race_hard_deadline = now, now + S.RACE_JOIN_GRACE_SECONDS
        w.pending_race = (now - 0.1, leader)
        other.gopts_written_at = now - 0.5                           # wrote GameOptions of its own in the same moment
        w.tick()
        self.assertEqual(started, [leader])

    def test_an_invitee_choosing_a_car_is_still_waited_for(self):
        w, (leader, other) = world({"numlaps": None})
        w.event = None
        w.set_session_host_bool = lambda *x: None
        w.start_event = lambda q: self.fail("started while an invitee was choosing a car")
        now = time.monotonic()
        leader.entered = other.entered = True
        other.console_vars[3] = 1
        w.race_requested_at, w.race_hard_deadline = now, now + S.RACE_JOIN_GRACE_SECONDS
        w.pending_race = (now - 0.1, leader)
        w.tick()
        self.assertIsNotNone(w.pending_race)
        self.assertLessEqual(S.RACE_JOIN_GRACE_SECONDS, 45)


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

    def test_nobody_loaded_drops_one_suspect_not_everyone(self):
        """Prod 2026-10-22 lobby e7255c39: every game waits for the others' cars, so one car nobody could load got the whole group kicked."""
        now = time.monotonic()
        w, (a, b) = world({"numlaps": None, "begun": False, "started": now - S.EVENT_LOAD_TIMEOUT_SECONDS - 1})
        c = S.Peer(("10.0.0.3", 5003))
        c.connected = True
        w.peers[c.addr] = c
        w.event["people"].append(c)
        w.event["racing"] = False
        w.set_session_host_bool = lambda *x: None
        dropped = []
        w.set_console_bool = lambda q, var, value, why: dropped.append((q, value, why))
        # Fresh CI runners can have less than 30 seconds of monotonic uptime.
        # A real failed-check timestamp is positive; zero means no failed check.
        b.car_seen_at = b.car_doubt_at = now - min(30, now / 2)
        c.car_seen_at = now - min(5, now / 4)
        w.tick()
        self.assertEqual(w.event["people"], [a, c])
        self.assertEqual(dropped, [(b, 0, "the others could not load its car")])
        w.tick()                                                     # the next suspect only after a step
        self.assertEqual(len(w.event["people"]), 2)
        for q in (a, c):                                             # with b out, the others finish loading and the event begins
            q.console_vars[S.CONSOLE_VAR_HAS_LOADED_CARS] = 1
        w.maybe_begin_race()
        self.assertTrue(w.event["begun"])

    def test_the_leader_goes_last_and_a_lone_participant_ends_it(self):
        now = time.monotonic()
        w, (a, b) = world({"numlaps": None, "begun": False, "started": now - S.EVENT_LOAD_TIMEOUT_SECONDS - 1})
        w.event["racing"] = False
        w.set_session_host_bool = lambda *x: None
        w.set_console_bool = lambda *x: None
        a.car_seen_at = now                                          # the leader changed car last, still not the first suspect
        w.tick()
        self.assertEqual(w.event["people"], [a])
        w.event["load_next"] = 0.0
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
