"""A world never holds more than 12 players, whatever lobby matchmaking or an invite assigned: the HUD player list has 12 slots and a
13th player crashed every client in the world (prod 2026-10-22 15:34:45, all 12 clients of lobby 53629321 at game+0x144661d)."""
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("FH2MP_CONTROL_PORT", "0")
import fh2mp_server as S  # noqa: E402
import test_security  # noqa: E402
from test_security import make_hub, open_packet  # noqa: E402


class WorldCapacity(unittest.TestCase):
    def setUp(self):
        self.hub = make_hub()

    def join(self, i, lobby="lobby-a"):
        xuid = 0x9000000000000 + i
        addr = ("10.2.%d.%d" % (i // 200, i % 200 + 1), 5000)
        self.hub.assign[xuid] = lobby
        self.hub.handle(open_packet(xuid), addr)
        return addr

    def test_thirteenth_assigned_player_goes_to_another_world(self):
        addrs = [self.join(i) for i in range(13)]
        full = self.hub.route[addrs[0]]
        self.assertEqual(self.hub.world_load(full), 12)
        self.assertIsNot(self.hub.route[addrs[12]], full)
        self.assertNotIn(addrs[12], full.peers)

    def test_reconnecting_player_keeps_their_seat(self):
        for i in range(12):
            self.join(i)
        xuid = 0x9000000000000 + 3                       # the same player again from a new port (stale connection not timed out)
        addr = ("10.9.9.9", 6000)
        self.hub.handle(open_packet(xuid), addr)
        self.assertIs(self.hub.route[addr], self.hub.worlds[(2, "lobby-a")])

    def test_resent_open_connection_of_a_seated_player_stays(self):
        addrs = [self.join(i) for i in range(12)]
        self.hub.handle(open_packet(0x9000000000000 + 11), addrs[11])
        self.assertIs(self.hub.route[addrs[11]], self.hub.worlds[(2, "lobby-a")])


class RelayOwnerRange(unittest.TestCase):
    """The game holds 128 relay-owner tables (game+17A0140); an object relayed under owner 128 or more made every client fault at
    game+17AA83B (reproduced in game 2026-10-26: owner 127 + 128 crashed the existing client, 128 + 129 crashed both, and so did an ordinary
    leave and rejoin once the world had counted that far). A world gives each connection the lowest free id in 1..127; a leaver's id rests
    for OWNER_QUARANTINE_S, then comes back; a world with no id left takes no more players."""

    def setUp(self):
        self.hub = make_hub()
        self.echo = test_security.HandshakeEcho()
        self.n = 0

    def connect(self, lobby=None, addr=None, xuid=None):
        self.n += 1
        xuid = xuid or 0x9000000100000 + self.n
        addr = addr or ("10.3.%d.%d" % (self.n // 200, self.n % 200 + 1), 5000)
        if lobby:
            self.hub.assign[xuid] = lobby
        self.hub.handle(open_packet(xuid), addr)
        world = self.hub.route.get(addr)
        if world is not None and addr in world.peers:
            self.hub.handle(self.echo.ack(world.server_counter), addr)
        return addr

    def leave(self, addr):
        world = self.hub.route[addr]
        self.hub.handle(S.build_packet([lambda w: (w.bits(S.MSG_CLOSE_CONNECTION, 7), w.bits(0, 3), w.bits(0, 32), w.bits(0, 32))]), addr)
        self.hub.sweep()
        self.assertNotIn(addr, world.peers)

    def owner(self, addr):
        peer = self.hub.route[addr].peers[addr]
        return peer.owner_id if peer.connected else None

    def rest_all_but(self, world, *free):
        """Every id except `free` is held back for an hour: the world is nearly out of ids."""
        until = time.monotonic() + 3600
        world.owner_quarantine = {o: until for o in range(1, S.RELAY_OWNER_LAST + 1) if o not in free}

    def test_ids_are_the_lowest_free_ones(self):
        addrs = [self.connect() for _ in range(5)]
        self.assertEqual([self.owner(a) for a in addrs], [1, 2, 3, 4, 5])

    def test_a_leavers_id_rests_and_then_comes_back(self):
        a, b = self.connect(), self.connect()
        self.assertEqual((self.owner(a), self.owner(b)), (1, 2))
        world = self.hub.route[a]
        self.leave(b)
        c = self.connect()
        self.assertEqual(self.owner(c), 3)                      # 2 is resting
        world.owner_quarantine[2] = time.monotonic() - 1        # its rest is over
        d = self.connect()
        self.assertEqual(self.owner(d), 2)
        self.assertIs(self.hub.route[d], world)

    def test_leaving_clears_the_receivers_replay_counters_of_that_owner(self):
        a, b = self.connect(), self.connect()
        world = self.hub.route[a]
        world.peers[a].seq_shift[(2, 1, 0)] = 3                 # what a replay of owner 2 left in the other client
        world.peers[a].seq_shift[(7, 1, 0)] = 5
        self.leave(b)
        self.assertNotIn((2, 1, 0), world.peers[a].seq_shift)
        self.assertIn((7, 1, 0), world.peers[a].seq_shift)

    def test_churn_never_hands_out_an_owner_past_the_table_and_the_world_never_retires(self):
        anchor = self.connect()
        first = self.hub.route[anchor]
        for i in range(600):                                    # one world sees every join; each player leaves before the next arrives
            addr = self.connect()
            peer = self.hub.route[addr].peers[addr]
            self.assertTrue(peer.connected, f"join {i}")
            self.assertLessEqual(peer.owner_id, S.RELAY_OWNER_LAST, f"join {i}")
            self.assertIs(self.hub.route[addr], first, f"join {i}")
            owners = [p.owner_id for p in first.peers.values() if p.connected]
            self.assertEqual(len(owners), len(set(owners)), f"join {i}")
            self.leave(addr)
            for o in list(first.owner_quarantine):              # the rest period passes between two joins
                first.owner_quarantine[o] = time.monotonic() - 1
        self.assertEqual(len(self.hub.worlds), 1)

    def test_a_burst_of_joins_and_leaves_inside_the_rest_period_stays_below_128(self):
        anchor = self.connect()
        first = self.hub.route[anchor]
        seen = set()
        for i in range(400):                                    # no rest period passes: ids only come back when the world is full
            addr = self.connect()
            peer = self.hub.route[addr].peers.get(addr)
            if peer is not None and peer.connected:
                seen.add(peer.owner_id)
                self.leave(addr)
        self.assertLessEqual(max(seen), S.RELAY_OWNER_LAST)
        self.assertGreater(len(self.hub.worlds), 1)             # the world ran out of free ids; newcomers went to another one
        self.assertIn(anchor, self.hub.route)
        self.assertIs(self.hub.route[anchor], first)

    def test_a_world_with_no_free_id_sends_the_next_player_elsewhere(self):
        a = self.connect()
        world = self.hub.route[a]
        self.rest_all_but(world, 126, 127)
        b, c = self.connect(), self.connect()
        self.assertEqual((self.owner(b), self.owner(c)), (126, 127))
        self.assertIs(self.hub.route[b], self.hub.route[c])
        d = self.connect()
        self.assertIsNot(self.hub.route[d], world)
        self.assertLessEqual(self.owner(d), S.RELAY_OWNER_LAST)

    def test_assigned_lobby_with_no_free_id_sends_the_player_elsewhere(self):
        a = self.connect("lobby-x")
        world = self.hub.route[a]
        self.rest_all_but(world, 127)
        b = self.connect("lobby-x")
        self.assertEqual(self.owner(b), 127)
        c = self.connect("lobby-x")
        self.assertIsNot(self.hub.route[c], world)
        self.assertLessEqual(self.owner(c), S.RELAY_OWNER_LAST)

    def test_same_address_reconnect_takes_a_new_id_or_moves_when_none_is_left(self):
        a = self.connect()
        world = self.hub.route[a]
        b = self.connect(addr=("10.4.4.4", 7000), xuid=0x9000000200001)
        self.assertEqual(self.owner(b), 2)
        self.hub.handle(open_packet(0x9000000200001), b)        # the game reconnects from the same port
        self.hub.handle(self.echo.ack(self.hub.route[b].server_counter), b)
        self.assertIs(self.hub.route[b], world)
        self.assertEqual(self.owner(b), 3)                      # its old id 2 is resting
        self.rest_all_but(world)                                # nothing free at all
        self.hub.handle(open_packet(0x9000000200001), b)
        self.assertIsNot(self.hub.route[b], world)
        self.assertNotIn(b, world.peers)

    def test_connections_still_handshaking_count_against_the_free_ids(self):
        a = self.connect()
        world = self.hub.route[a]
        self.rest_all_but(world, 126, 127)
        pending = []
        for i in range(3):                                      # three opened, none acknowledged yet
            self.n += 1
            addr = ("10.5.5.%d" % (i + 1), 5000)
            self.hub.handle(open_packet(0x9000000300000 + i), addr)
            pending.append(addr)
        in_world = [x for x in pending if self.hub.route[x] is world]
        self.assertEqual(len(in_world), 2)                      # ids 126 and 127 only
        for addr in pending:
            self.hub.handle(self.echo.ack(self.hub.route[addr].server_counter), addr)
        for w in self.hub.worlds.values():
            self.assertLessEqual(max([p.owner_id for p in w.peers.values()] or [0]), S.RELAY_OWNER_LAST)
        self.assertEqual(sum(1 for x in pending if self.hub.route[x].peers[x].connected), 3)

    def test_standalone_world_refuses_the_handshake_with_no_free_id(self):
        sock = type("S", (), {"sent": [], "sendto": lambda self, d, a: None, "setsockopt": lambda *x: None})()
        world = S.Server(0, None, sock=sock, log_file=None)
        world.log = lambda msg: None
        self.rest_all_but(world)
        addr = ("10.7.7.7", 5000)
        world.handle(open_packet(0x9000000400001), addr)
        world.handle(self.echo.ack(world.server_counter), addr)
        self.assertNotIn(addr, world.peers)


if __name__ == "__main__":
    unittest.main()
