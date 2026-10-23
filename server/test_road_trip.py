"""Road Trip: team variants of events in the vote, where a drive starts, and how long it may take."""
import os
import random
import sqlite3
import sys
import tempfile
import unittest

os.environ.setdefault("FH2MP_CONTROL_PORT", "0")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S  # noqa: E402


class FakeSock:
    def __getattr__(self, name):
        return lambda *a, **k: 0


def make_db(path):
    db = sqlite3.connect(path)
    db.execute("CREATE TABLE RaceModes (id INTEGER, Name TEXT, Enum TEXT, RaceModeId INTEGER, MinimumPlayers INTEGER)")
    db.execute("CREATE TABLE AnthemOnlineEvents (AnthemOnlineEventId INTEGER, RoadTripLocationId INTEGER, TrackId INTEGER, "
               "NumLaps INTEGER, RaceModeId INTEGER)")
    for mode, minp in ((0, 1), (1, 2), (2, 2), (25, 2), (26, 2), (31, 1)):
        db.execute("INSERT INTO RaceModes VALUES (?, '', '', 1, ?)", (mode, minp))
    ev = 1
    for loc in (3, 4, 5, 6):
        for mode in (0, 0, 0, 0, 31, 31, 1, 1, 2, 25, 26):
            db.execute("INSERT INTO AnthemOnlineEvents VALUES (?, ?, ?, ?, ?)", (ev, loc, 800 + ev, 1 + ev % 3, mode))
            ev += 1
    db.commit()
    db.close()


class Candidates(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        cls.db = os.path.join(cls.dir.name, "roadtrip.sqlite")
        make_db(cls.db)

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def test_a_small_group_gets_no_team_events(self):
        for seed in range(20):
            locs, events, classes, mins = S.pick_road_trip_candidates(self.db, 3, random.Random(seed))
            self.assertFalse(any(S.is_team_event(e) for row in events for e in row))

    def test_a_group_of_four_gets_one_team_variant_per_destination(self):
        for seed in range(20):
            locs, events, classes, mins = S.pick_road_trip_candidates(self.db, 4, random.Random(seed))
            for row, mrow in zip(events, mins):
                teams = [e for e in row if S.is_team_event(e)]
                self.assertEqual(len(teams), 1)
                bases = [S.base_event_id(e) for e in row]
                self.assertEqual(len(set(bases)), 4)                      # never an event and its own team variant
                for e, m in zip(row, mrow):
                    if S.is_team_event(e):
                        self.assertGreaterEqual(m, S.RT_TEAM_MIN_PLAYERS)

    def test_only_race_modes_that_rank_the_field_become_team_events(self):
        for seed in range(30):
            locs, events, classes, mins = S.pick_road_trip_candidates(self.db, 8, random.Random(seed))
            for e in (e for row in events for e in row if S.is_team_event(e)):
                db = sqlite3.connect(self.db)
                mode = db.execute("SELECT RaceModeId FROM AnthemOnlineEvents WHERE AnthemOnlineEventId=?",
                                  (S.base_event_id(e),)).fetchone()[0]
                db.close()
                self.assertIn(mode, S.RT_TEAM_MODES)

    def test_team_only_switch_offers_nothing_but_team_events(self):
        self.addCleanup(setattr, S, "RT_TEAM_ONLY", S.RT_TEAM_ONLY)
        S.RT_TEAM_ONLY = True
        for seed in range(20):
            locs, events, classes, mins = S.pick_road_trip_candidates(self.db, 1, random.Random(seed))
            self.assertTrue(all(S.is_team_event(e) for row in events for e in row))
            self.assertTrue(all(len(row) == 4 for row in events))

    def test_the_server_looks_a_team_event_up_by_its_base_row(self):
        old = os.environ.get("FH2MP_GAMEDB")
        os.environ["FH2MP_GAMEDB"] = self.db
        try:
            w = S.Server(0, None, roadtrip=True, sock=FakeSock(), log_file=None)
        finally:
            if old is None:
                del os.environ["FH2MP_GAMEDB"]
            else:
                os.environ["FH2MP_GAMEDB"] = old
        self.assertEqual(w.rt_numlaps(3 + S.TEAM_EVENT_OFFSET), w.rt_numlaps(3))
        self.assertEqual(w.rt_race_mode(5 + S.TEAM_EVENT_OFFSET), w.rt_race_mode(5))
        self.assertIsNotNone(w.rt_race_mode(5 + S.TEAM_EVENT_OFFSET))


class Teams(unittest.TestCase):
    def test_a_team_event_splits_the_players_and_the_grid_alternates(self):
        w = S.Server(0, None, roadtrip=True, sock=FakeSock(), log_file=None)
        w.rt_candidates = ([3, 4, 5], [[11, 12, 13, 14 + S.TEAM_EVENT_OFFSET]] * 3, [0, 1, 2], None)
        w.rt_winner, w.rt_idx = 0, 3
        teams, grids = {}, {}
        w.set_player_team = lambda q, t: teams.__setitem__(q, t)
        w.set_console_bool = lambda *a, **k: None
        w.set_session_host_bool = lambda *a, **k: None
        w.rt_race_mode = lambda race: 0
        people = []
        for i in range(5):
            q = S.Peer(("10.3.0.%d" % (i + 1), 5000))
            q.owner_id, q.connected = i + 1, True
            q.console_vars = {4: 1, 5: 1}
            w.peers[q.addr] = q
            people.append(q)
        w.start_event(people[0])
        self.assertEqual(sorted(teams.values()), [0, 0, 0, 1, 1])
        w.send = lambda q, msgs: grids.setdefault(q, msgs)
        w.maybe_begin_race()
        order = w.event["order"]
        self.assertEqual([teams[q] for q in order], [0, 1, 0, 1, 0])

    def test_the_team_update_writes_var_90(self):
        w = S.Server(0, None, roadtrip=True, sock=FakeSock(), log_file=None)
        q = S.Peer(("10.3.1.1", 5000))
        q.owner_id, q.connected = 1, True
        w.peers[q.addr] = q
        sent, relayed = [], []
        w.send = lambda peer, msgs: sent.extend(msgs)
        w.remember_and_relay = lambda *a: relayed.append(a)
        w.set_player_team(q, 1)
        bw = S.BitWriter()
        sent[0](bw)
        self.assertEqual(relayed[0][6], 9)                          # 8-bit var id + 1 bit
        self.assertEqual(relayed[0][7], S.PLAYER_VAR_TEAM_ID | (1 << 8))


class PrivateUniverses(unittest.TestCase):
    def test_every_player_gets_an_own_private_universe(self):
        w = S.Server(0, None, roadtrip=True, sock=FakeSock(), log_file=None)
        relayed = []
        w.send = lambda peer, msgs: None
        w.remember_and_relay = lambda *a: relayed.append(a)
        got = []
        for i in range(3):
            q = S.Peer(("10.3.2.%d" % (i + 1), 5000))
            q.owner_id, q.connected = i + 1, True
            w.peers[q.addr] = q
            w.assign_private_universe(q)
            got.append(q.private_universe)
        self.assertEqual(got, [1, 2, 3])
        self.assertEqual(relayed[0][6], 13)                         # 8-bit var id + 5-bit universe
        self.assertEqual(relayed[0][7], S.PLAYER_VAR_PRIVATE_UNIVERSE | (1 << 8))
        w.peers.pop(("10.3.2.1", 5000)).connected = False       # a seat that frees up is handed out again
        q = S.Peer(("10.3.2.9", 5000))
        q.owner_id, q.connected = 9, True
        w.peers[q.addr] = q
        w.assign_private_universe(q)
        self.assertEqual(q.private_universe, 1)


class Legs(unittest.TestCase):
    def road_trip(self, last_race, last_loc, winner_loc=4, idx=0):
        w = S.Server(0, None, roadtrip=True, sock=FakeSock(), log_file=None)
        w.rt_candidates = ([3, winner_loc, 5], [[11, 12, 13, 14], [21, 22, 23, 24], [31, 32, 33, 34]], [0, 1, 2], None)
        w.rt_winner, w.rt_idx = 1, idx
        w.rt_last_race, w.rt_last_loc = last_race, last_loc
        self.calls = []
        w.rt_broadcast = lambda **kw: self.calls.append(kw)
        w.set_session_host_bool = lambda *a, **k: None
        return w

    def test_a_fresh_lobby_has_no_previous_event(self):
        w = self.road_trip(None, None)
        w.rt_start_drive()
        first = self.calls[0]
        self.assertEqual(first["NextRaceId"], 21)
        self.assertEqual(first["EventRaceId"], 21)
        self.assertEqual(first["CompletedEventCount"], 0)

    def test_the_next_drive_starts_where_the_last_event_ended(self):
        w = self.road_trip(23, 4, idx=3)
        w.rt_start_drive()
        first = self.calls[0]
        self.assertEqual(first["EventRaceId"], 23)            # the finish line of the event that was just raced ...
        self.assertEqual(first["NextRaceId"], 24)             # ... to the start of the one coming up
        self.assertEqual(first["CompletedEventCount"], 3)

    def test_a_new_destination_starts_from_the_last_event_of_the_old_one(self):
        w = self.road_trip(24, 3, winner_loc=6)
        w.rt_candidates[0][1] = 6
        w.rt_start_drive()
        self.assertEqual(self.calls[0]["EventRaceId"], 24)
        self.assertEqual(self.calls[0]["NextRaceId"], 21)

    def test_everybody_leaving_between_two_events_leaves_nothing_to_fire(self):
        """Prod #41: the last player left while the next drive was pending; a tick later rt_start_drive ran with no winner."""
        w = self.road_trip(21, 4)
        w.rt_send_configure = lambda: None
        w.rt_next_event()                                          # event 2 of the block: the drive waits for the options
        self.assertTrue(w.rt_pending_drive)
        p = S.Peer(("10.0.0.1", 5001))
        p.connected = True
        w.peers[p.addr] = p
        w.drop_peer(p, "close")
        self.assertIsNone(w.rt_winner)
        self.assertFalse(w.rt_pending_drive)
        w.rt_drive_not_before = w.rt_cfg_deadline = 0.0
        w.road_trip_tick(1e9)                                      # raised TypeError before the fix

    def test_errors_name_their_line(self):
        try:
            [1][None]
        except TypeError as e:
            self.assertRegex(S.where(e), r"^test_errors_name_their_line:\d+$")

    def test_starting_a_team_event_is_noticed(self):
        w = self.road_trip(None, None)
        w.rt_candidates[1][1][0] = 21 + S.TEAM_EVENT_OFFSET
        peers = [S.Peer(("10.0.0.%d" % i, 5000 + i)) for i in (1, 2, 3, 4)]
        for p in peers:
            p.connected = True
            p.console_vars[4] = 1
            w.peers[p.addr] = p
        w.start_event(peers[0])
        self.assertTrue(w.event.get("team"))
        self.assertEqual(w.rt_last_race, 21 + S.TEAM_EVENT_OFFSET)
        self.assertEqual(w.rt_last_loc, 4)


class DriveTime(unittest.TestCase):
    def test_stays_between_the_limits(self):
        for a in (None, 3, 4, 5, 6, 7, 8):
            for b in (3, 4, 5, 6, 7, 8):
                ms = S.road_trip_drive_ms(a, b)
                self.assertGreaterEqual(ms, S.RT_DRIVE_MIN_MS)
                self.assertLessEqual(ms, S.RT_DRIVE_MAX_MS)

    def test_a_far_destination_gets_more_time_than_a_near_one(self):
        self.assertGreater(S.road_trip_drive_ms(4, 6), S.road_trip_drive_ms(7, 8))      # Nice -> Amalfi against Montichello -> San Gimignano
        self.assertGreater(S.road_trip_drive_ms(6, 3), S.road_trip_drive_ms(5, 4))

    def test_more_than_the_old_two_minutes_for_a_long_way(self):
        self.assertGreater(S.road_trip_drive_ms(3, 6), 180 * 1000)

    def test_events_of_one_destination(self):
        ms = S.road_trip_drive_ms(5, 5)
        self.assertEqual(ms, S.road_trip_drive_ms(8, 8))
        self.assertGreaterEqual(ms, S.RT_DRIVE_MIN_MS)

    def test_an_unknown_location_falls_back(self):
        self.assertEqual(S.road_trip_drive_ms(3, 99), S.RT_DRIVE_MS)


if __name__ == "__main__":
    unittest.main()
