"""events.jsonl (event_log.py): the structured record behind tools/prod_report.py."""
import json
import os
import sys
import tempfile
import unittest

os.environ.setdefault("FH2MP_CONTROL_PORT", "0")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import event_log  # noqa: E402
import fh2mp_server as S  # noqa: E402


class Writer(unittest.TestCase):
    def test_lines_are_json_and_bad_ones_are_skipped_on_read(self):
        with tempfile.TemporaryDirectory() as d:
            log = event_log.EventLog(os.path.join(d, "events.jsonl"))
            log.emit("session_end", xuid="0x1", why="CloseConnection", seconds=12.5)
            log.emit("event_end", why="race over", finished=2)
            with open(log.path, "a") as f:
                f.write("not json\n[1, 2]\n{\"kind\": \"no time\"}\n")
            got = list(event_log.read(open(log.path)))
        self.assertEqual([e["kind"] for e in got], ["session_end", "event_end"])
        self.assertEqual((got[0]["xuid"], got[0]["seconds"]), ("0x1", 12.5))

    def test_a_log_that_cannot_be_opened_never_raises(self):
        log = event_log.EventLog("/proc/definitely/not/writable/events.jsonl")
        log.emit("session_end", why="x")                       # the game server must not care
        event_log.EventLog(None).emit("session_end")

    def test_since_keeps_the_newer_events(self):
        lines = [json.dumps({"t": 100, "kind": "a"}), json.dumps({"t": 200, "kind": "b"})]
        self.assertEqual([e["kind"] for e in event_log.read(lines, since=150)], ["b"])


class Summary(unittest.TestCase):
    def test_counters(self):
        events = [
            {"t": 1, "kind": "session_end", "world_kind": "freeroam", "why": "CloseConnection", "seconds": 10, "xuid": "0x1", "entered": True},
            {"t": 2, "kind": "session_end", "world_kind": "freeroam", "why": "signed out", "seconds": 4000, "xuid": "0x2", "entered": True},
            {"t": 3, "kind": "session_end", "world_kind": "carmeet", "why": "no packets for 20 s", "seconds": 50, "xuid": "0x1", "entered": False},
            {"t": 4, "kind": "event_end", "why": "race over: everybody finished", "begun": True, "finished": 2},
            {"t": 5, "kind": "event_end", "why": "the leader's car does not meet the event restrictions"},
            {"t": 6, "kind": "not_eligible", "rule_class": 4, "class": 2, "rentable": True},
            {"t": 7, "kind": "parts_repair", "car": 2017},
            {"t": 8, "kind": "parts_repair", "car": 2017},
        ]
        s = event_log.summarize(events)
        self.assertEqual(s["players"]["distinct"], 2)
        self.assertEqual(s["session length"], {"<30 s": 1, "<2 min": 1, ">=1 h": 1})
        self.assertEqual(s["sessions that never entered"], {"carmeet": 1})
        self.assertEqual(s["events by outcome"]["finished: race over: everybody finished"], 1)
        self.assertEqual(s["events by outcome"]["never began: the leader's car does not meet the event restrictions"], 1)
        self.assertEqual(s["class refusals"]["rule class 4 vs car class 2 (rentable model)"], 1)
        self.assertEqual(s["part repairs by car"], {"2017": 2})


class FromTheServer(unittest.TestCase):
    def test_a_session_end_and_an_event_end_are_recorded(self):
        import test_freeroam_tag_grant as T
        w, (a, _b) = T.world()
        with tempfile.TemporaryDirectory() as d:
            w.events = event_log.EventLog(os.path.join(d, "events.jsonl"))
            w.tag = "[freeroam:abc12345] "
            w.event = {"people": [a], "begun": True, "started": S.time.monotonic() - 30, "finished": {a: 1}}
            w.end_event("race over: everybody finished")
            w.drop_peer(a, "CloseConnection")
            got = list(event_log.read(open(w.events.path)))
        kinds = [e["kind"] for e in got]
        self.assertIn("event_end", kinds)
        self.assertIn("session_end", kinds)
        end = next(e for e in got if e["kind"] == "event_end")
        self.assertEqual((end["world"], end["world_kind"], end["finished"], end["begun"]), ("freeroam:abc12345", "freeroam", 1, True))
        session = next(e for e in got if e["kind"] == "session_end")
        self.assertEqual((session["why"], session["world_kind"]), ("CloseConnection", "freeroam"))
        self.assertNotIn("addr", json.dumps(session))                  # no network addresses in the record


class Visibility(unittest.TestCase):
    def test_a_player_sending_predictions_without_a_car_is_reported_once(self):
        import test_freeroam_tag_grant as T
        w, (a, b) = T.world()
        logs = []
        w.log = logs.append
        with tempfile.TemporaryDirectory() as d:
            w.events = event_log.EventLog(os.path.join(d, "events.jsonl"))
            a.connected = a.entered = b.connected = b.entered = True
            a.model_id, a.cars_relayed = None, S.VISIBILITY_MIN_PREDICTIONS - 1
            b.model_id, b.cars_relayed = 1496, 5000                       # a car, plenty of predictions: fine
            w.audit_visibility(100.0)
            self.assertEqual(logs, [])                                     # not enough predictions yet
            a.cars_relayed = S.VISIBILITY_MIN_PREDICTIONS
            w.audit_visibility(100.5)
            self.assertEqual(logs, [])                                     # checked too recently
            w.audit_visibility(100.0 + S.VISIBILITY_CHECK_SECONDS)
            self.assertEqual(len([m for m in logs if "INVISIBLE?" in m]), 1)
            w.audit_visibility(100.0 + 3 * S.VISIBILITY_CHECK_SECONDS)
            self.assertEqual(len([m for m in logs if "INVISIBLE?" in m]), 1)    # once per session
            a.model_id = 1496
            got = list(event_log.read(open(w.events.path)))
        self.assertEqual([e["kind"] for e in got], ["no_car_model"])
        self.assertEqual(got[0]["owner"], a.owner_id)


if __name__ == "__main__":
    unittest.main()
