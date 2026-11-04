"""Matchmaking puts a player into the fullest lobby that has room: 25 players make 12 + 12 + 1, and lobbies that lost players are refilled
before a new one is opened (prod 2026-10-22: Free Roam lobbies of 8, 10, 6 and 1 players, because only the newest lobby ever got joiners)."""
import os
import sys
import unittest

os.environ.setdefault("FH2MP_CONTROL_PORT", "0")
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fh2mp_server as S  # noqa: E402
import xbl  # noqa: E402


GUID = "11111111-2222-3333-4444-555555555555"


class LobbyDocument(unittest.TestCase):
    """The first ticket session seeds the lobby every later joiner parses (0x183bb50), so only checked keys may pass."""
    def test_a_normal_ticket_session_keeps_what_the_game_reads(self):
        custom = xbl._lobby_custom({"hopperId": 7, "EnableFullArbitration": True,
                                    "gameServerPlatform": {"gameVariantId": GUID, "gsiSetId": GUID, "location": "east"}}, {})
        self.assertEqual(custom, {"hopperId": 7, "EnableFullArbitration": True,
                                  "gameServerPlatform": {"gameVariantId": GUID, "gsiSetId": GUID, "location": "east",
                                                         "maxAllowedPlayers": xbl.LOBBY_CAPACITY}})

    def test_hostile_values_become_the_defaults(self):
        for bad in (None, 5, "x", [], {"gameServerPlatform": "x"}, {"hopperId": "1", "EnableFullArbitration": 1, "extra": {"a": 1},
                    "gameServerPlatform": {"gameVariantId": 5, "gsiSetId": "' OR", "location": {"a": 1}, "maxAllowedPlayers": 999, "x": 1}}):
            custom = xbl._lobby_custom(bad, {"hopperId": 3})
            self.assertEqual(set(custom), {"hopperId", "gameServerPlatform"}, bad)
            self.assertEqual(custom["hopperId"], 3)
            self.assertEqual(set(custom["gameServerPlatform"]), {"gameVariantId", "gsiSetId", "maxAllowedPlayers"}, bad)
            self.assertEqual(custom["gameServerPlatform"]["maxAllowedPlayers"], xbl.LOBBY_CAPACITY)

    def test_the_gsi_set_falls_back_to_the_ticket_attributes(self):
        self.assertEqual(xbl._lobby_custom(None, {"gsiSetId": GUID})["gameServerPlatform"]["gsiSetId"], GUID)

    def test_a_hostile_seed_does_not_reach_the_lobby(self):
        xbl._SESSIONS.clear()
        xbl._SESSIONS[("s", "t", "n")] = {"constants": {"custom": {"gameServerPlatform": {"gsiSetId": 5}, "x": 1}}}
        name = xbl._new_lobby("s", {}, {"scid": "s", "templateName": "t", "name": "n"})
        rec = xbl._SESSIONS[("s", xbl.TARGET_TEMPLATE.lower(), name.lower())]
        self.assertNotIn("x", rec["constants"]["custom"])
        self.assertTrue(xbl._GUID.fullmatch(rec["constants"]["custom"]["gameServerPlatform"]["gsiSetId"]))
        xbl._SESSIONS.clear()


class TicketsDoNotQueue(unittest.TestCase):
    def test_the_lobby_lock_is_free_while_the_game_server_is_asked(self):
        orig, held = xbl.hub_populations, []
        xbl.hub_populations = lambda: held.append(xbl._LOBBY_LOCK.locked()) or {}
        self.addCleanup(setattr, xbl, "hub_populations", orig)
        xbl._lobby_for("s", "h", {}, None, "1000")
        self.assertEqual(held, [False])


class Packing(unittest.TestCase):
    def setUp(self):
        xbl._SESSIONS.clear()
        xbl._LOBBIES.clear()
        xbl._RESERVED.clear()
        self.present = {}          # lobby name -> set of xuids the (fake) game server has connected
        self.hub_up = True
        self.orig = xbl.hub_populations
        xbl.hub_populations = lambda: ({n: ["%x" % x for x in xs] for n, xs in self.present.items()} if self.hub_up else None)
        self.next_xuid = 1000

    def tearDown(self):
        xbl.hub_populations = self.orig

    def join(self, connect=True):
        self.next_xuid += 1
        name = xbl._lobby_for("s", "h", {}, None, str(self.next_xuid))
        if connect:
            self.present.setdefault(name, set()).add(self.next_xuid)
        return name

    def age_reservations(self):
        for res in xbl._RESERVED.values():
            for x in res:
                res[x] -= xbl.RESERVE_SECONDS + 1

    def sizes(self):
        return sorted((len(v) for v in self.present.values() if v), reverse=True)

    def test_twenty_five_players_make_12_12_1(self):
        for _ in range(25):
            self.join()
        self.assertEqual(self.sizes(), [12, 12, 1])

    def test_players_matched_but_not_connected_yet_hold_their_seat(self):
        names = [self.join(connect=False) for _ in range(13)]
        counts = sorted((names.count(n) for n in set(names)), reverse=True)
        self.assertEqual(counts, [12, 1])

    def test_a_reservation_runs_out(self):
        name = self.join(connect=False)
        self.age_reservations()
        self.assertEqual(xbl.lobby_occupancy(name, xbl._SESSIONS[("s", "defaulttemplate", name)], xbl.hub_populations()), 0)

    def test_lobbies_that_lost_players_are_refilled_fullest_first(self):
        for _ in range(36):
            self.join()
        a, b, c = list(self.present)
        for lobby, keep in ((a, 8), (b, 10), (c, 6)):
            for x in sorted(self.present[lobby])[keep:]:
                self.present[lobby].discard(x)
        self.assertEqual(self.sizes(), [10, 8, 6])
        self.age_reservations()                                 # the last player of a lobby may have left before the next match ran
        placed = [self.join() for _ in range(2 + 4 + 6)]
        self.assertEqual([placed.count(n) for n in (b, a, c)], [2, 4, 6])
        self.assertEqual(placed[:2], [b, b])                    # the 10 before the 8, the 8 before the 6
        self.assertEqual(self.sizes(), [12, 12, 12])
        self.assertEqual(len(xbl._LOBBIES[xbl.lobby_key("s", "h", {})]), 3)    # and no fourth lobby was opened

    def test_a_new_lobby_only_when_every_one_is_full(self):
        for _ in range(24):
            self.join()
        self.assertEqual(len(self.present), 2)
        self.join()
        self.assertEqual(len(self.present), 3)

    def test_without_the_game_server_the_session_members_are_counted(self):
        self.hub_up = False
        first = self.join(connect=False)
        rec = xbl._SESSIONS[("s", "defaulttemplate", first)]
        for i in range(11):
            rec["members"][str(5000 + i)] = {}
        self.assertEqual(self.join(connect=False), first)       # 11 members: one seat left
        rec["members"]["9999"] = {}
        self.assertNotEqual(self.join(connect=False), first)    # 12: full

    def test_other_hoppers_have_their_own_lobbies(self):
        a = xbl._lobby_for("s", "h", {}, None, "1")
        b = xbl._lobby_for("s", "other", {}, None, "2")
        self.assertNotEqual(a, b)

    def test_a_new_match_replaces_the_seat_held_before(self):
        a = self.join(connect=False)
        xbl._RESERVED[a]["1001"] = xbl._RESERVED[a]["1001"]      # the first player of the test
        again = xbl._lobby_for("s", "h", {}, None, "1001")
        self.assertEqual(sum(1 for r in xbl._RESERVED.values() if "1001" in r), 1)
        self.assertEqual(again, a)


class NeverBackIntoTheLobbyBeingLeft(unittest.TestCase):
    """A search started inside a lobby, or right after leaving one, is never matched back into it (dev 2026-11-01: "Unable to join
    session" at 22:31:03, a 111 s stall at 21:34:40), while a player whose game dropped still goes back to its friends."""
    def setUp(self):
        xbl._SESSIONS.clear()
        xbl._LOBBIES.clear()
        xbl._RESERVED.clear()
        xbl._LEFT.clear()
        self.present = {}
        self.orig = xbl.hub_populations
        xbl.hub_populations = lambda: {n: ["%x" % x for x in xs] for n, xs in self.present.items()}
        self.addCleanup(setattr, xbl, "hub_populations", self.orig)
        self.addCleanup(xbl._LEFT.clear)

    def match(self, xuid, connect=True):
        name = xbl._lobby_for("s", "h", {}, None, str(xuid))
        if connect:
            for xs in self.present.values():
                xs.discard(xuid)
            self.present.setdefault(name, set()).add(xuid)
        return name

    def leave(self, xuid, name):
        path = "/serviceconfigs/s/sessionTemplates/%s/sessions/%s" % (xbl.TARGET_TEMPLATE, name)
        status, _doc = xbl.put_session(path, b'{"members": {"me": null}}', xuid=str(xuid))
        self.assertEqual(status, 200)

    def join_mpsd(self, xuid, name):
        path = "/serviceconfigs/s/sessionTemplates/%s/sessions/%s" % (xbl.TARGET_TEMPLATE, name)
        body = b'{"members": {"me": {"properties": {"system": {"active": true}}}}}'
        self.assertEqual(xbl.put_session(path, body, xuid=str(xuid))[0], 200)

    def test_a_search_from_inside_a_lobby_gets_another_one(self):
        a = self.match(1001)
        self.assertEqual(self.match(1002), a)                   # the second player joins the first
        self.join_mpsd(1002, a)
        again = xbl._lobby_for("s", "h", {}, None, "1002")      # 1002 searches again while still connected to a
        self.assertNotEqual(again, a)
        self.assertNotIn("1002", xbl._SESSIONS[("s", "defaulttemplate", a.lower())]["members"], "its seat in a is released at once")

    def test_a_search_right_after_leaving_gets_another_one(self):
        a = self.match(1001)
        self.match(1002)
        self.join_mpsd(1002, a)
        self.leave(1002, a)
        self.present[a].discard(1002)                           # its connection is gone too: only the MPSD leave tells
        self.assertNotEqual(xbl._lobby_for("s", "h", {}, None, "1002"), a)
        self.assertEqual(xbl._lobby_for("s", "h", {}, None, "1003"), a, "other players still fill it")

    def test_the_rule_runs_out(self):
        a = self.match(1001)
        self.match(1002)
        self.join_mpsd(1002, a)
        self.leave(1002, a)
        self.present[a].discard(1002)
        lobby, t = xbl._LEFT["1002"]
        xbl._LEFT["1002"] = (lobby, t - xbl.LEFT_AVOID_SECONDS - 1)
        self.assertEqual(xbl._lobby_for("s", "h", {}, None, "1002"), a)
        self.assertNotIn("1002", xbl._LEFT)

    def test_a_dropped_player_goes_back_to_its_friends(self):
        a = self.match(1001)
        self.match(1002)
        self.join_mpsd(1002, a)                                 # stays an MPSD member: the game crashed, no leave was written
        self.present[a].discard(1002)
        self.assertEqual(xbl._lobby_for("s", "h", {}, None, "1002"), a)

    def test_leaving_a_qos_ticket_session_is_not_leaving_a_lobby(self):
        path = "/serviceconfigs/s/sessionTemplates/defaultTemplate_qos/sessions/%s" % GUID
        self.assertEqual(xbl.put_session(path, b'{"members": {"me": {}}}', xuid="1002")[0], 200)
        self.assertEqual(xbl.put_session(path, b'{"members": {"me": null}}', xuid="1002")[0], 200)
        self.assertNotIn("1002", xbl._LEFT)


class HubReport(unittest.TestCase):
    def test_populations_name_the_players_of_each_lobby(self):
        hub = S.Hub(0, None)

        class W:
            pass

        def world(key, players):
            w = W()
            w.hub_key = key
            w.peers = {}
            w.bots = []
            for i, xuid in enumerate(players):
                addr = ("10.0.0.%d" % (len(hub.route) + 1), 5000)
                p = S.Peer(addr)
                p.xuid = xuid
                w.peers[addr] = p
                hub.route[addr] = w
            return w

        world((2, "lobby-a"), [0x10, 0x20])
        world((1, "lobby-b"), [0x30])
        world((2, ""), [0x40])                                  # the shared default world has no name to report
        got = hub.populations()
        self.assertEqual({k: sorted(v) for k, v in got.items()}, {"lobby-a": ["10", "20"], "lobby-b": ["30"]})
        self.assertTrue(hub.populations_text().startswith("pop "))


class CheaterPool(unittest.TestCase):
    """A player the cheat checks kicked is matched only with other kicked players for a day, and nobody else is matched with them."""

    def setUp(self):
        xbl._SESSIONS.clear()
        xbl._LOBBIES.clear()
        xbl._RESERVED.clear()
        self.present = {}
        self.flagged = set()
        self.orig = xbl.hub_populations

        def pops():
            out = {n: ["%x" % x for x in xs] for n, xs in self.present.items()}
            out["!flagged"] = ["%x" % x for x in self.flagged]
            return out
        xbl.hub_populations = pops

    def tearDown(self):
        xbl.hub_populations = self.orig

    def join(self, xuid):
        name = xbl._lobby_for("s", "h", {}, None, str(xuid))
        self.present.setdefault(name, set()).add(xuid)
        return name

    def test_clean_and_flagged_players_never_share_a_lobby(self):
        self.flagged = {900, 901}
        clean = [self.join(x) for x in (1, 2, 3)]
        bad = [self.join(x) for x in (900, 901)]
        self.assertEqual(len(set(clean)), 1)
        self.assertEqual(len(set(bad)), 1)
        self.assertNotEqual(clean[0], bad[0])
        self.assertEqual(self.join(4), clean[0])           # later clean players still fill the clean lobby
        self.flagged.add(902)
        self.assertEqual(self.join(902), bad[0])

    def test_a_player_in_a_lobby_when_flagged_is_not_matched_back_into_it(self):
        lobby = self.join(1)
        self.join(2)
        self.present[lobby].discard(2)                      # kicked, and flagged by the server
        self.flagged = {2}
        self.assertNotEqual(self.join(2), lobby)

    def test_a_reserved_flagged_player_keeps_clean_players_out(self):
        self.flagged = {900}
        name = xbl._lobby_for("s", "h", {}, None, "900")    # matched, not connected yet
        self.assertNotEqual(xbl._lobby_for("s", "h", {}, None, "5"), name)

    def test_without_the_game_server_report_nothing_is_filtered(self):
        xbl.hub_populations = lambda: None
        a = xbl._lobby_for("s", "h", {}, None, "1")
        self.assertEqual(xbl._lobby_for("s", "h", {}, None, "2"), a)

    def test_the_hub_flags_a_kicked_player_and_reports_it(self):
        import tempfile
        d = tempfile.mkdtemp()
        hub = S.Hub(0, os.path.join(d, "server.log"))
        hub.flag(0xABC, "speed 500 km/h")
        self.assertIn("!flagged=abc", hub.populations_text())
        again = S.Hub(0, os.path.join(d, "server.log"))     # a restart keeps the flag
        self.assertEqual(again.flagged_now(), [0xABC])
        again.flagged[0xABC] = (0.0, "old")                  # a day later
        self.assertEqual(again.flagged_now(), [])
        self.assertNotIn("!flagged", again.populations_text())
        S.CHEATER_POOL_HOURS, old = 0, S.CHEATER_POOL_HOURS
        try:
            again.flag(0xDEF, "x")
        finally:
            S.CHEATER_POOL_HOURS = old
        self.assertEqual(again.flagged_now(), [])


if __name__ == "__main__":
    unittest.main()
