"""World sizes (capacity.py): only Free Roam grows past 12, and only when FH2MP_WORLD_CAPACITY says so. An event, Road Trip and Car Meet stay at the
client's 12 (the starting grid's key buffer, MaxPlayers 1..12). The game server and the LSP read the same setting."""
import os
import sys
import unittest
from unittest import mock

os.environ.setdefault("FH2MP_CONTROL_PORT", "0")
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import capacity  # noqa: E402
import fh2mp_server as S  # noqa: E402
import test_security  # noqa: E402
import xbl  # noqa: E402
from test_freeroam_tag_grant import options, world  # noqa: E402
from test_security import make_hub  # noqa: E402


def env(value):
    return mock.patch.dict(os.environ, {capacity.ENV: value})


class Setting(unittest.TestCase):
    def test_twelve_unless_it_is_set(self):
        with mock.patch.dict(os.environ):
            os.environ.pop(capacity.ENV, None)
            self.assertEqual(capacity.free_roam(), 12)

    def test_a_number_from_12_to_75_is_taken_and_anything_else_is_not(self):
        for value, want in (("24", 24), ("75", 75), ("72", 72), ("12", 12), ("11", 12), ("0", 12), ("-5", 12), ("500", 75), ("abc", 12), ("", 12), (" 30 ", 30)):
            with env(value):
                self.assertEqual(capacity.free_roam(), want, repr(value))

    def test_only_free_roam_follows_it(self):
        with env("36"):
            self.assertEqual([capacity.for_type(t) for t in (1, 2, 3)], [12, 36, 12])
            self.assertEqual([capacity.for_kind(k) for k in ("freeroam", "roadtrip", None)], [36, 12, 12])
            self.assertEqual([capacity.for_hopper(h) for h in ("FreeRoam_qos", "freeroam_qos", "RoadTrip_qos", "CarMeet_qos", None, "h")], [36, 36, 12, 12, 12, 12])


class LobbyMatching(unittest.TestCase):
    """xbl._lobby_for fills a Free Roam lobby up to its size before it opens another; Road Trip keeps 12."""
    def setUp(self):
        xbl._SESSIONS.clear()
        xbl._LOBBIES.clear()
        xbl._RESERVED.clear()
        self.present = {}
        self.orig = xbl.hub_populations
        xbl.hub_populations = lambda: {n: ["%x" % x for x in xs] for n, xs in self.present.items()}
        self.next_xuid = 1000
        self.addCleanup(setattr, xbl, "hub_populations", self.orig)
        self.addCleanup(xbl._SESSIONS.clear)

    def join(self, hopper):
        self.next_xuid += 1
        name = xbl._lobby_for("s", hopper, {}, None, str(self.next_xuid))
        self.present.setdefault(name, set()).add(self.next_xuid)
        return name

    def sizes(self):
        return sorted((len(v) for v in self.present.values() if v), reverse=True)

    def test_free_roam_lobbies_hold_the_set_size(self):
        with env("24"):
            for _ in range(25):
                self.join("FreeRoam_qos")
            self.assertEqual(self.sizes(), [24, 1])

    def test_road_trip_lobbies_stay_at_12(self):
        with env("24"):
            for _ in range(25):
                self.join("RoadTrip_qos")
            self.assertEqual(self.sizes(), [12, 12, 1])

    def test_the_lobby_document_carries_its_own_size(self):
        with env("24"):
            for hopper, want in (("FreeRoam_qos", 24), ("RoadTrip_qos", 12)):
                name = self.join(hopper)
                rec = xbl._SESSIONS[("s", xbl.TARGET_TEMPLATE.lower(), name.lower())]
                self.assertEqual(rec["constants"]["custom"]["gameServerPlatform"]["maxAllowedPlayers"], want, hopper)

    def test_a_record_without_a_size_falls_back_to_the_size_of_its_kind(self):
        """a session stored before the size was validated has none: the default is the hopper's, not Free Roam's"""
        with env("24"):
            first = self.join("RoadTrip_qos")
            rec = xbl._SESSIONS[("s", xbl.TARGET_TEMPLATE.lower(), first.lower())]
            rec["constants"]["custom"]["gameServerPlatform"].pop("maxAllowedPlayers")
            for _ in range(12):
                self.join("RoadTrip_qos")
            self.assertEqual(self.sizes(), [12, 1])

    def test_by_default_nothing_changes(self):
        with mock.patch.dict(os.environ):
            os.environ.pop(capacity.ENV, None)
            for _ in range(25):
                self.join("FreeRoam_qos")
            self.assertEqual(self.sizes(), [12, 12, 1])


class HubWorlds(unittest.TestCase):
    def setUp(self):
        self.hub = make_hub()
        self.n = 0

    def join(self, lobby, lobby_type=2):
        self.n += 1
        xuid = 0x9000000200000 + self.n
        addr = ("10.4.%d.%d" % (self.n // 200, self.n % 200 + 1), 5000)
        if lobby:
            self.hub.assign[xuid] = lobby

        def open_msg(w):
            w.bits(S.MSG_OPEN_CONNECTION, 7)
            w.string(S.VERSION_STRING)
            w.string(S.GAME_STRING)
            w.bits(0, 2)
            w.bits(xuid, 64)
            w.bits(lobby_type, 32)
            w.bits(0, 32)
            w.bits(7, 32)
        self.hub.handle(S.build_packet([open_msg]), addr)
        return addr

    def test_a_free_roam_world_takes_the_set_size_and_the_next_player_goes_elsewhere(self):
        with env("24"):
            addrs = [self.join("roam") for _ in range(25)]
            world = self.hub.route[addrs[0]]
            self.assertEqual(self.hub.world_load(world), 24)
            self.assertTrue(all(self.hub.route[a] is world for a in addrs[:24]))
            self.assertIsNot(self.hub.route[addrs[24]], world)

    def test_players_with_no_lobby_assignment_fill_one_world_to_the_set_size(self):
        """joined from the menu: open_world_for picks the busiest world of the type that has room"""
        with env("24"):
            addrs = [self.join(None) for _ in range(25)]
            world = self.hub.route[addrs[0]]
            self.assertEqual(self.hub.world_load(world), 24)
            self.assertIsNot(self.hub.route[addrs[24]], world)
        with env("12"):
            self.hub = make_hub()
            addrs = [self.join(None) for _ in range(13)]
            self.assertIsNot(self.hub.route[addrs[12]], self.hub.route[addrs[0]])

    def test_road_trip_and_car_meet_worlds_stay_at_12(self):
        with env("24"):
            for lobby_type in (S.LOBBY_TYPE_ROAD_TRIP, 3):
                addrs = [self.join("w%d" % lobby_type, lobby_type) for _ in range(13)]
                full = self.hub.route[addrs[0]]
                self.assertEqual(self.hub.world_load(full), 12, lobby_type)
                self.assertIsNot(self.hub.route[addrs[12]], full, lobby_type)

    def test_the_status_row_reports_the_capacity_of_its_kind(self):
        with env("24"):
            self.join("roam")
            self.join("trip", S.LOBBY_TYPE_ROAD_TRIP)
            for w in self.hub.worlds.values():
                for p in w.peers.values():
                    p.connected = True
            rows = {r["kind"]: r["capacity"] for r in self.hub.lobby_status()["lobbies"]}
        self.assertEqual(rows, {"freeroam": 24, "roadtrip": 12})


class EventSize(unittest.TestCase):
    def crowd(self, n):
        w, peers = world()
        w.log = lambda text: None
        for i in range(3, n + 1):
            p = S.Peer(("10.0.0.%d" % i, 5000 + i))
            p.connected, p.owner_id, p.car_id, p.choices = True, i, i, (3, 0.750)
            p.console_vars[4] = 1
            w.peers[p.addr] = p
            peers.append(p)
        w.game_options = options("FR_CIRCUIT")
        return w, peers

    def test_an_event_holds_12_of_a_bigger_world(self):
        w, peers = self.crowd(16)
        w.start_event(peers[0])
        people = w.event["people"]
        self.assertEqual(len(people), 12)
        self.assertIs(people[0], peers[0])
        left_out = [q for q in peers if q not in people]
        self.assertEqual(len(left_out), 4)
        for q in left_out:                                    # told they are not in: Ready and ParticipatingInRace are cleared
            self.assertEqual(q.console_vars[4], 0)
            self.assertEqual(q.console_vars[S.CONSOLE_VAR_PARTICIPATING_IN_RACE], 0)

    def test_a_small_world_is_unchanged(self):
        w, peers = self.crowd(5)
        w.start_event(peers[0])
        self.assertEqual(len(w.event["people"]), 5)

class EventSeats(unittest.TestCase):
    """A Free Roam world above 12 players: only the leader and 11 invitees get a seat, the 13th Ready is refused when it is asked, not when the
    event starts (the starting grid has 12 keys; a 13th participant is what made clients crash in the event)."""

    def invited(self, n, ready):
        """A world of n clients, owner 1 asked for the race; `ready` of the invitees (in order) have said Ready."""
        w, peers = world()
        w.log = lambda text: None
        for i in range(3, n + 1):
            p = S.Peer(("10.0.0.%d" % i, 5000 + i))
            p.connected, p.owner_id, p.car_id, p.choices = True, i, i, (3, 0.750)
            w.peers[p.addr] = p
            peers.append(p)
        for p in peers:
            p.entered = True
            p.console_vars[4] = 0
        w.pending_race = (S.time.monotonic() + 20.0, peers[0])
        w.race_requested_at = S.time.monotonic()
        for p in peers[1:1 + ready]:
            p.console_vars[4] = 1
        return w, peers

    def test_the_seats_fill_at_the_leader_and_11_invitees(self):
        w, peers = self.invited(16, 10)
        self.assertFalse(w.event_seats_full(ignoring=peers[11]))      # leader + 10 ready: the 12th seat is free
        w, peers = self.invited(16, 11)
        self.assertTrue(w.event_seats_full(ignoring=peers[12]))       # leader + 11 ready: a 13th is turned away
        self.assertFalse(w.event_seats_full(ignoring=peers[11]))      # ... but one that is already seated asking again is not
        self.assertFalse(w.event_seats_full(ignoring=peers[1]))

    def test_a_small_world_never_fills(self):
        w, peers = self.invited(8, 7)
        self.assertFalse(w.event_seats_full(ignoring=peers[7]))

    def test_the_refused_ready_stays_off(self):
        w, peers = self.invited(16, 11)
        late = peers[13]
        late.console_vars[4] = 1
        w.refuse_event_ready(late, "test")
        self.assertEqual(late.console_vars[4], 0)
        self.assertEqual(sum(1 for q in peers[1:] if q.console_vars[4]), 11)

    def test_a_full_event_starts_without_waiting_out_the_window(self):
        w, peers = self.invited(16, 11)
        w.pending_race = (S.time.monotonic() + 20.0, peers[0])
        w.check_invitees_ready()
        self.assertLess(w.pending_race[0] - S.time.monotonic(), 2.0)

    def test_a_not_yet_full_event_keeps_waiting(self):
        w, peers = self.invited(16, 6)
        w.pending_race = (S.time.monotonic() + 20.0, peers[0])
        w.check_invitees_ready()
        self.assertGreater(w.pending_race[0] - S.time.monotonic(), 15.0)

    def test_a_refused_ready_retried_after_a_car_change_does_not_take_a_full_seat(self):
        w, peers = self.invited(16, 11)
        late = peers[13]
        late.ready_retry = w.race_requested_at
        w.game_options = options("FR_CIRCUIT")
        w.retry_refused_ready(late)
        self.assertEqual(late.console_vars[4], 0)
        self.assertIsNone(late.ready_retry)
        free = peers[12]
        peers[1].console_vars[4] = 0                                  # a seated invitee left: one seat is free again
        w.pending_race = (S.time.monotonic() + 20.0, peers[0])
        free.ready_retry = w.race_requested_at
        w.retry_refused_ready(free)                                   # one seat is free again: this one gets it
        self.assertEqual(free.console_vars[4], 1)

    def test_the_start_still_cuts_to_12_as_a_last_resort(self):
        w, peers = self.invited(16, 15)                               # everybody got a Ready through (an older server, a lost refusal)
        w.start_event(peers[0])
        self.assertEqual(len(w.event["people"]), 12)


class EventSizeGrid(unittest.TestCase):
    def test_the_starting_grid_never_lists_more_than_12(self):
        for count, want in ((12, 12), (15, 12), (5, 5)):
            w = S.BitWriter()
            S.starting_grid([("relay", i, i + 1) for i in range(count)])(w)
            r = S.BitReader(w.value.to_bytes((w.pos + 7) // 8, "little"))
            r.bits(7)
            r.bits(S.LOBBY_EVENT_KEY_BITS)
            self.assertEqual(r.bits(4), want, count)


if __name__ == "__main__":
    unittest.main()
