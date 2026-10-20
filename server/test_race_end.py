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
        w.note_score_keyframe(a)
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


if __name__ == "__main__":
    unittest.main()
