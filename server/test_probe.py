"""The UDP reachability probe (probe.py): the launcher's way to find out, before Play, that a network drops UDP."""
import os
import sys
import unittest

os.environ.setdefault("FH2MP_CONTROL_PORT", "0")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import probe  # noqa: E402


class Hub:
    def __init__(self, tunnel=None):
        self.sent = []
        self.tunnel = tunnel
        self.sock = self

    def sendto(self, data, addr):
        self.sent.append((data, addr))


def request(nonce=b"12345678"):
    return bytes(11) + probe.MAGIC + nonce


class Probe(unittest.TestCase):
    def setUp(self):
        probe._seen.clear()

    def test_the_same_bytes_come_back(self):
        hub, data = Hub(), request()
        self.assertTrue(probe.handle(hub, data, ("192.0.2.1", 5000), now=100.0))
        self.assertEqual(hub.sent, [(data, ("192.0.2.1", 5000))])
        self.assertEqual(len(hub.sent[0][0]), len(data))               # never more than was asked: no amplification

    def test_game_datagrams_and_other_lengths_are_not_probes(self):
        hub = Hub()
        for data in (bytes(11) + b"\x07\x00" * 20, request() + b"x", request()[:-1], bytes(11) + b"FH2MPPN2" + bytes(8)):
            self.assertFalse(probe.handle(hub, data, ("192.0.2.1", 5000), now=100.0))
        self.assertEqual(hub.sent, [])

    def test_an_address_gets_a_few_answers_a_minute(self):
        hub, addr = Hub(), ("192.0.2.1", 5000)
        for i in range(probe.PER_WINDOW + 4):
            self.assertTrue(probe.handle(hub, request(), addr, now=100.0 + i))
        self.assertEqual(len(hub.sent), probe.PER_WINDOW)
        probe.handle(hub, request(), addr, now=100.0 + probe.WINDOW + 1)           # the next window
        self.assertEqual(len(hub.sent), probe.PER_WINDOW + 1)
        probe.handle(hub, request(), ("192.0.2.2", 5000), now=101.0)               # another address is not affected
        self.assertEqual(len(hub.sent), probe.PER_WINDOW + 2)

    def test_a_tcp_clients_pseudo_address_is_not_answered_over_udp(self):
        class Tunnel:
            conns = {("192.0.2.9", 40000): object()}
        hub = Hub(Tunnel())
        self.assertTrue(probe.handle(hub, request(), ("192.0.2.9", 40000), now=100.0))
        self.assertEqual(hub.sent, [])

    def test_the_table_stays_bounded(self):
        hub = Hub()
        for i in range(probe.MAX_ADDRESSES + 10):
            probe.handle(hub, request(), ("198.51.100.1", i), now=100.0)
        self.assertLessEqual(len(probe._seen), probe.MAX_ADDRESSES)


if __name__ == "__main__":
    unittest.main()
