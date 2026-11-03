"""Diagnostic lines of the service modules (xbl, scoreboard, ugc, gamedb_sync, reliable, tunnel, lighting, probe, traffic, db helpers):
the reason and the values behind a refusal reach the log, and a repeating failure is rate limited."""
import json
import os
import socket
import sqlite3
import struct
import sys
import tempfile
import unittest
from types import SimpleNamespace as NS

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import db  # noqa: E402
import diag  # noqa: E402
import gamedb_sync  # noqa: E402
import lighting  # noqa: E402
import probe  # noqa: E402
import reliable  # noqa: E402
import scoreboard  # noqa: E402
import traffic  # noqa: E402
import traffic_manager  # noqa: E402
import tunnel  # noqa: E402
import ugc  # noqa: E402
import xbl  # noqa: E402


class Captured(unittest.TestCase):
    def setUp(self):
        self.lines = []
        old = diag._sink
        diag.set_sink(lambda level, sub, msg: self.lines.append((level, msg)))
        self.addCleanup(lambda: diag.set_sink(old))
        for mod in (xbl, scoreboard, ugc, reliable, tunnel, lighting, probe, traffic, traffic_manager, db):
            mod._RATE = diag.RateLimit()

    def text(self, level=None):
        return [m for lv, m in self.lines if level is None or lv == level]


class Scores(Captured):
    def board(self):
        return scoreboard.ScoreStore(None, ["Rival"], event_info=lambda event: {"attrs": {"CarClassId": 2}} if event == 77 else None)

    def d(self, **kw):
        base = {"score": scoreboard.dbl_to_u64(55.0), "scoreboardType": 1, "gameScoreboardId": 0, "trackId": 8001, "carId": 100,
                "carClassId": 3, "carPerformanceIndex": 0.62, "isClean": True, "usedSTM": False, "usedTCS": False, "usedABS": False}
        base.update(kw)
        return base

    def test_each_refusal_names_the_value_and_the_limit(self):
        s = self.board()
        cases = [
            (self.d(score=scoreboard.dbl_to_u64(float("nan"))), 1, "not a finite number"),
            (self.d(score=scoreboard.dbl_to_u64(0.0)), 1, "not a possible time"),
            (self.d(scoreboardType=9, score=scoreboard.dbl_to_u64(130.0)), 5, "127.4 m/s cap"),
            (self.d(scoreboardType=3, carClassId=7), 1, "X/R"),
            (self.d(scoreboardType=3, gameScoreboardId=77, carClassId=4), 1, "event's class 2"),
            (self.d(scoreboardType=4, score=scoreboard.dbl_to_u64(151.0)), 9, "above the 150"),
            (self.d(scoreboardType=4, score=scoreboard.dbl_to_u64(12.5)), 9, "not a whole number"),
        ]
        for data, score_type, expect in cases:
            with self.subTest(expect=expect):
                self.assertIn(expect, s.refusal(score_type, data, scoreboard.u64_to_dbl(data["score"])))
        self.assertIsNone(s.refusal(1, self.d(), 55.0))

    def test_a_refused_submission_is_logged_with_who_and_what(self):
        s = self.board()
        self.assertIsNone(s.submit(0x900000000000B, "T", 5, self.d(scoreboardType=9, score=scoreboard.dbl_to_u64(140.0))))
        (line,) = self.text("WARN")
        self.assertIn("SubmitScore refused: speed 140.000 m/s is above the 127.4 m/s cap", line)
        self.assertIn("xuid=0x900000000000b", line)
        self.assertIn("board=(9,5)", line)


class Reliable(Captured):
    def test_giving_up_a_packet_is_reported_once_per_window_of_time(self):
        w = reliable.Window("owner 7")
        msg = [(7,) + (0,) * 3]                   # message id 7 (a create) is kept for resending
        for i in range(reliable.MAX_PENDING + 50):
            w.stamp(msg, float(i))
        self.assertEqual(w.lost, 50)
        (line,) = self.text("WARN")
        self.assertIn("owner 7: 1024 reliable packets are waiting for an ack; giving up packet id 1", line)


class Mpsd(Captured):
    PATH = "/serviceconfigs/s/sessiontemplates/t/sessions/abcdef123456"

    def setUp(self):
        super().setUp()
        xbl._SESSIONS.clear()
        self.addCleanup(xbl._SESSIONS.clear)

    def test_a_refused_write_says_why_and_repeats_are_counted(self):
        body = json.dumps({"constants": {"custom": {"gameServerPlatform": {"maxAllowedPlayers": 500}}}}).encode()
        now = [0.0]
        xbl._RATE = diag.RateLimit(clock=lambda: now[0])
        for _ in range(3):
            self.assertEqual(xbl.put_session(self.PATH, body, "111")[0], 400)
        (line,) = self.text("WARN")
        self.assertIn("PUT refused 400: maxAllowedPlayers 500 is not a number from 1 to 100", line)
        self.assertIn("xuid=111", line)
        now[0] = 1000.0                                         # a later window: the next line says how many it replaced
        xbl.put_session(self.PATH, body, "111")
        self.assertIn("(+2 like it", self.text("WARN")[-1])

    def test_writing_another_players_member_entry_is_403_with_the_entries(self):
        body = json.dumps({"members": {"222": {"constants": {"system": {}}}}}).encode()
        self.assertEqual(xbl.put_session(self.PATH, body, "111")[0], 403)
        self.assertIn("writes the member entry of another player", self.text("WARN")[0])
        self.assertIn("entries=222", self.text("WARN")[0])

    def test_a_ticket_match_logs_the_decision(self):
        old = xbl.hub_populations
        xbl.hub_populations = lambda: {"!flagged": []}
        self.addCleanup(lambda: setattr(xbl, "hub_populations", old))
        xbl._LOBBIES.clear()
        xbl._RESERVED.clear()
        name = xbl._lobby_for("s", "FreeRoam_qos", {"gsiSetId": "00000000-0000-0000-0000-000000000102", "Location": "9"}, None, "300")
        (line,) = self.text("INFO")
        self.assertIn(f"ticket matched xuid=300 lobby={name[:8]} how='new lobby' hopper=FreeRoam_qos location=9", line)
        self.assertIn("occupancy_source='game server'", line)

    def test_a_silent_game_server_is_one_warning_then_one_recovery_line(self):
        old = os.environ.get("FH2MP_SERVER_CONTROL")
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        os.environ["FH2MP_SERVER_CONTROL"] = f"127.0.0.1:{port}"          # bound but never answers: the 0.3 s timeout
        xbl._POP_DOWN_UNTIL, xbl._hub_down_since = 0.0, None

        def restore():
            s.close()
            xbl._POP_DOWN_UNTIL, xbl._hub_down_since = 0.0, None
            if old is None:
                os.environ.pop("FH2MP_SERVER_CONTROL", None)
            else:
                os.environ["FH2MP_SERVER_CONTROL"] = old
        self.addCleanup(restore)
        self.assertIsNone(xbl.hub_populations())
        xbl._POP_DOWN_UNTIL = 0.0
        self.assertIsNone(xbl.hub_populations())
        (line,) = self.text("WARN")
        self.assertIn("did not answer the population query within 0.3 s", line)
        self.assertIn("counting MPSD members", line)


class Gamedb(Captured):
    def test_a_file_that_is_not_sqlite_is_reported_once_and_a_good_one_is_announced(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "gamedbRC.slt")
            with open(path, "wb") as f:
                f.write(b"<html>upload failed</html>")
            g = gamedb_sync.GameDb(path)
            self.assertIsNone(g.manifest())
            self.assertIsNone(g.manifest())
            (line,) = self.text("WARN")
            self.assertIn("does not start with the SQLite header", line)
            self.assertIn("launchers get 404", line)
            os.unlink(path)
            c = sqlite3.connect(path)
            c.execute("CREATE TABLE t (a)")
            c.commit()
            c.close()
            self.assertIsNotNone(g.manifest())
            self.assertTrue(any("car database now served: sha256" in m for m in self.text("INFO")))


class Ugc(Captured):
    def test_a_broken_livery_blob_says_which_number_is_wrong(self):
        name = b"design"
        blob = struct.pack("<II", 999, 100) + b"x" * 10          # header says 999 compressed bytes, 10 are there
        body = struct.pack(">I", len(name)) + name + struct.pack(">IH", 7, 3) + b"\x00" + b"\x00" * 8 + struct.pack(">I", len(blob)) + blob
        self.assertIsNone(ugc.parse_livery_upload(body))
        (line,) = self.text("WARN")
        self.assertIn("header says 999 compressed bytes (blob has 10)", line)
        self.assertIn("car 7", line)

    def test_a_quota_refusal_carries_the_counts(self):
        class Rows:
            def execute(self, *_):
                return self

            def fetchone(self):
                return {"n": ugc.MAX_BLOBS_PER_OWNER, "b": 5}

        class Tx:
            def __enter__(self):
                return Rows()

            def __exit__(self, *_):
                return False
        store = ugc.UgcStore(NS(tx=Tx))
        with self.assertRaises(ugc.LimitError) as raised:
            store.blob_create(1, "x", 100)
        self.assertIn(f"{ugc.MAX_BLOBS_PER_OWNER} of {ugc.MAX_BLOBS_PER_OWNER} blobs", str(raised.exception))


class Tunnel(Captured):
    def test_a_closed_connection_says_why_and_how_long_it_lived(self):
        t = tunnel.Tunnel(0)
        self.addCleanup(t.close)
        client = socket.create_connection(("127.0.0.1", t.port))
        self.addCleanup(client.close)
        for _ in range(100):
            t.accept(now=1000.0)
            if t.conns:
                break
        (conn,) = t.conns.values()
        client.sendall(struct.pack(">H", tunnel.MAX_FRAME + 1))
        for _ in range(100):
            if t.read(conn, now=1005.0) or conn.dead:
                break
        t.expire(1006.0, lambda addr: True)
        (line,) = [m for m in self.text("INFO") if "closed" in m]
        self.assertIn(f"frame of {tunnel.MAX_FRAME + 1} bytes announced, limit {tunnel.MAX_FRAME}", line)
        self.assertIn("lived_s=6", line)

    def test_a_connection_that_never_opened_a_game_connection_is_reported(self):
        t = tunnel.Tunnel(0)
        self.addCleanup(t.close)
        client = socket.create_connection(("127.0.0.1", t.port))
        self.addCleanup(client.close)
        for _ in range(100):
            t.accept(now=0.0)
            if t.conns:
                break
        t.expire(tunnel.HELLO_SECONDS + 1, lambda addr: False)
        self.assertTrue(any("no game connection opened within 10 s" in m for m in self.text("INFO")))


class Lighting(Captured):
    def test_a_request_with_the_magic_but_a_bad_field_is_reported_with_the_fields(self):
        data = bytearray(lighting.REQUEST_SIZE)
        data[:len(lighting.HEADER)] = lighting.HEADER
        struct.pack_into("<H", data, 9, len(data))
        data[19], data[52], data[53] = 1, 200, 1
        lighting.handle(NS(), bytes(data), ("a", 1))
        (line,) = self.text("WARN")
        self.assertIn("malformed", line)
        self.assertIn("car 200 (max 127)", line)

    def test_an_unbound_player_is_a_warning_with_the_reason(self):
        data = bytearray(lighting.REQUEST_SIZE)
        data[:len(lighting.HEADER)] = lighting.HEADER
        struct.pack_into("<H", data, 9, len(data))
        data[19] = 1
        struct.pack_into("<QQ", data, 20, 55, 1)
        hub = NS(revoked={}, route={}, bound={}, keys={}, ticket_epochs={})
        lighting.handle(hub, bytes(data), ("a", 1))
        (line,) = self.text("WARN")
        self.assertIn("xuid 55 ignored: no game connection of this player is bound to a ticket", line)


class Probe(Captured):
    def test_the_over_limit_warning_comes_once(self):
        probe._seen.clear()
        data = b"\0" * 11 + probe.MAGIC + b"\1" * 8
        hub = NS(sock=NS(sendto=lambda *_: None))
        for _ in range(probe.PER_WINDOW + 5):
            self.assertTrue(probe.handle(hub, data, ("p", 1), now=10.0))
        (line,) = self.text("WARN")
        self.assertIn(f"over the limit of {probe.PER_WINDOW} answers", line)


class Traffic(Captured):
    def test_an_ownership_denial_keeps_its_reason(self):
        class Out(traffic_manager.Out):
            pass
        m = traffic_manager.TrafficManager(Out())
        m.join("a")
        m.join("b")
        self.assertFalse(m.request_ownership("a", 3, 0.0))
        self.assertEqual(m.refusal, "no such car")
        self.assertFalse(m.spawned("a", 5, 100, None, 0.0))
        self.assertEqual(m.refusal, "no grant is outstanding for that id")


class Database(unittest.TestCase):
    def test_a_slow_query_line_shows_the_statement_collapsed_and_cut_without_its_arguments(self):
        self.assertEqual(db._sql_head("SELECT  *\n  FROM players WHERE xuid=%s", 20), "SELECT * FROM player")


if __name__ == "__main__":
    unittest.main()
