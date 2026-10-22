"""Outgoing messages are queued per client and leave together (Server.send / relay / flush): fewer datagrams, no sleep on the loop.

Prod 2026-10-21, 72 players: one datagram per relayed car update per recipient (~12k/s) put half the server's time into sendto, the receive
queue overflowed and races rubberbanded."""
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S  # noqa: E402


class FakeSock:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((data, addr))


def world(n=3):
    w = S.Server(0, None, sock=FakeSock(), log_file=None)
    w.log = lambda msg: None
    peers = []
    for i in range(n):
        p = S.Peer(("10.0.0.%d" % (i + 1), 5000 + i))
        p.connected = p.entered = True
        p.owner_id = i + 1
        p.world = ("",)
        p.last_rx = 0.0
        w.peers[p.addr] = p
        peers.append(p)
    return w, peers


def car(cid, owner=1):
    return S.relay_car_prediction(cid, owner, (1 << S.CAR_PREDICTION_BODY_BITS) - 1 - cid, S.CAR_PREDICTION_BODY_BITS)


def messages(pkt):
    r = S.BitReader(pkt)
    hdr = S.parse_header(r)
    assert hdr["size"] == len(pkt)
    return hdr["count"]


class SendQueue(unittest.TestCase):
    def test_packed_bytes_equal_the_old_builder(self):
        for msgs in ([], [S.pong(7, 9)], [car(3), S.ping(5), car(4)]):
            self.assertEqual(S.pack_messages([S.bake(m) for m in msgs], 0xFFFF, 12, 0x5), S.build_packet(msgs, 0xFFFF, 12, 0x5))

    def test_relayed_updates_share_one_datagram_per_recipient(self):
        w, (a, b, c) = world()
        for cid in range(5):
            w.relay(a, car(cid))
        self.assertEqual(w.sock.sent, [])                       # queued, not sent
        w.flush(float("inf"))
        self.assertEqual(sorted(addr for _, addr in w.sock.sent), sorted([b.addr, c.addr]))
        self.assertTrue(all(messages(pkt) == 5 for pkt, _ in w.sock.sent))

    def test_relay_waits_at_most_the_flush_interval(self):
        w, (a, b, c) = world()
        w.relay(a, car(1))
        self.assertEqual(w.flush(b.due - 0.001), b.due)
        self.assertEqual(w.sock.sent, [])
        w.flush(b.due)
        self.assertEqual(len(w.sock.sent), 2)

    def test_reply_is_held_2ms_after_the_clients_packet_without_sleeping(self):
        w, (a, b, c) = world()
        a.last_rx = t = time.monotonic()
        w.send(a, [S.pong(1, 2)])
        w.flush(t + 0.001)
        self.assertEqual(w.sock.sent, [])
        w.flush(t + 0.0021)
        self.assertEqual(len(w.sock.sent), 1)

    def test_empty_reply_still_sends_an_ack(self):
        w, (a, b, c) = world()
        w.send(a, [])
        w.flush(float("inf"))
        self.assertEqual([messages(p) for p, _ in w.sock.sent], [0])

    def test_large_queues_are_split_under_the_packet_limit(self):
        w, (a, b, c) = world()
        for cid in range(40):
            w.relay(a, car(cid % 100))
        w.flush(float("inf"))
        to_b = [p for p, addr in w.sock.sent if addr == b.addr]
        self.assertGreater(len(to_b), 1)
        self.assertTrue(all(len(p) <= S.MAX_PACKET_BYTES for p in to_b))
        self.assertEqual(sum(messages(p) for p in to_b), 40)

    def test_kick_close_leaves_before_the_peer_is_forgotten(self):
        w, (a, b, c) = world()
        w.send(a, [S.close_connection(0)], fast=True)
        w.drop_peer(a, "kick")
        self.assertIn(a.addr, [addr for _, addr in w.sock.sent])

    def test_stranger_is_answered_at_once(self):
        w, _ = world()
        w.send(S.Peer(("10.9.9.9", 1)), [S.close_connection(0)])
        self.assertEqual(len(w.sock.sent), 1)


if __name__ == "__main__":
    unittest.main()
