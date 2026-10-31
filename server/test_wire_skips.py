"""Client messages the server has no use for must not cost it the rest of their datagram: the packet is acked, so the client never sends the
tail again. On prod 1,127 datagrams lost their tail in 16 h to messages 24, 52 and 60."""
import math
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S  # noqa: E402


class FakeSock:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((data, addr))


def world():
    w = S.Server(0, None, sock=FakeSock(), log_file=None)
    w.lines = []
    w.log = w.lines.append
    peers = []
    for i in range(2):
        p = S.Peer(("10.0.0.%d" % (i + 1), 5000 + i))
        p.connected = p.entered = True
        p.owner_id = i + 1
        p.world = ("",)
        p.xuid = 0x9000000000001 + i
        w.peers[p.addr] = p
        peers.append(p)
    return w, peers


def key(w, id_bits, oid=1, owner=None):
    w.bits(0, 1)
    w.bits(0 if owner is None else 1, 1)
    w.bits(oid, id_bits)
    if owner is not None:
        w.bits(owner, 8)


def fixed(mtype, id_bits, tail):
    def write(w):
        w.bits(mtype, 7)
        if id_bits is not None:
            key(w, id_bits)
        for _ in range(tail // 32):
            w.bits(0x5A5A5A5A, 32)
        w.bits(0, tail % 32)
    return write


def delete(w):
    w.bits(S.MSG_OBJECT_DELETE, 7)
    w.bits(2, 4)
    key(w, S.ROUTER_BITS[2][0])
    w.bits(3, 8)


def detach(count):
    def write(w):
        w.bits(S.MSG_COLLOBJ_DETACH_PART, 7)
        w.bits(1, 4)
        w.bits(count, 11)
        for _ in range(count):
            w.bits(0xAB, 8)
    return write


def chat(count):
    def write(w):
        w.bits(S.MSG_CHAT_MANAGEMENT, 7)
        w.bits(1, 32)
        w.bits(2, 32)
        w.bits(count, 7)
        for _ in range(count):
            w.bits(0xCD, 8)
    return write


def prediction(w):
    w.bits(S.MSG_CAR_PREDICTION_FULL, 7)
    key(w, S.CAR_KEY_ID_BITS)
    w.bits(int.from_bytes(struct.pack("<fff", 100.0, 5.0, 200.0), "little"), 96)
    w.bits(0, S.CAR_PREDICTION_BODY_BITS - 128)
    w.bits(1000, 32)


class SkippedMessages(unittest.TestCase):
    def relayed_after(self, *messages):
        w, (a, b) = world()
        w.relay_prediction = lambda *args, **kw: setattr(w, "relayed", True)
        w.relayed = False
        w.handle(S.build_packet(list(messages) + [prediction]), a.addr)
        return w

    def test_a_prediction_behind_each_unused_message_is_still_read(self):
        unused = [fixed(m, *layout) for m, layout in S.UNUSED_MESSAGE_LAYOUT.items()] + [delete, detach(40), chat(3)]
        self.assertEqual(len(unused), len(S.UNUSED_MESSAGES))
        for write in unused:
            w = self.relayed_after(write)
            self.assertTrue(w.relayed)
            self.assertFalse([l for l in w.lines if "first-unknown" in l])

    def test_several_in_one_datagram(self):
        self.assertTrue(self.relayed_after(fixed(60, None, 0), detach(0), fixed(52, 2, 32), detach(1024)).relayed)

    def test_nothing_is_relayed_for_them(self):
        w, (a, b) = world()
        w.handle(S.build_packet([detach(40), chat(70), fixed(60, None, 0)]), a.addr)
        self.assertEqual(b.outbox, [])

    def test_a_count_the_client_would_overflow_on_stops_the_datagram(self):
        w = self.relayed_after(detach(1025))
        self.assertFalse(w.relayed)
        self.assertTrue([l for l in w.lines if "first-unknown=24" in l])

    def test_an_id_with_no_known_width_stops_the_datagram_and_logs_once(self):
        def needs_updates(w):
            w.bits(13, 7)
        w, (a, b) = world()
        w.relay_prediction = lambda *args, **kw: self.fail("the tail of this datagram cannot be located")
        for _ in range(3):
            w.handle(S.build_packet([needs_updates, prediction]), a.addr)
        self.assertEqual(len([l for l in w.lines if "first-unknown=13" in l]), 1)

    def test_a_delete_on_an_unmapped_router_stops_the_datagram(self):
        def bad(w):
            w.bits(S.MSG_OBJECT_DELETE, 7)
            w.bits(15, 4)
        self.assertFalse(self.relayed_after(bad).relayed)


class UnusablePositions(unittest.TestCase):
    def relayed(self, x, y=10.0, z=20.0):
        w, (a, b) = world()
        position = int.from_bytes(struct.pack("<fff", x, y, z), "little")

        def msg(wr):
            wr.bits(S.MSG_CAR_PREDICTION_FULL, 7)
            key(wr, S.CAR_KEY_ID_BITS)
            wr.bits(position, 96)
            wr.bits(0, S.CAR_PREDICTION_BODY_BITS - 128)
            wr.bits(1000, 32)
        w.handle(S.build_packet([msg]), a.addr)
        return bool(b.outbox), a

    def test_a_normal_car_is_relayed(self):
        sent, a = self.relayed(1200.0)
        self.assertTrue(sent)
        self.assertEqual(a.pos[0], 1200.0)

    def test_a_position_far_outside_the_world_box_but_plausible_is_only_logged(self):
        self.assertTrue(self.relayed(60000.0)[0])

    def test_nan_infinity_and_absurd_positions_are_withheld(self):
        for x in (math.nan, math.inf, -math.inf, 2e6):
            sent, a = self.relayed(x)
            self.assertFalse(sent, x)
            self.assertIsNone(a.pos)


if __name__ == "__main__":
    unittest.main()
