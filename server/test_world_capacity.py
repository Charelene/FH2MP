"""A world never holds more than 12 players, whatever lobby matchmaking or an invite assigned: the HUD player list has 12 slots and a
13th player crashed every client in the world (prod 2026-10-22 15:34:45, all 12 clients of lobby 53629321 at game+0x144661d)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("FH2MP_CONTROL_PORT", "0")
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


if __name__ == "__main__":
    unittest.main()
