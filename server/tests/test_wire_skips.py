"""Client messages the server has no use for must not cost it the rest of their datagram: the packet is acked, so the client never sends the
tail again. On prod 1,127 datagrams lost their tail in 16 h to messages 24, 52 and 60."""
import math
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
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


class RelayValidation(unittest.TestCase):
    """What a receiver would crash on or lose its place on must stop here (0x17a2040, ReadStringVarLen 0x1767150)."""
    def sent_to_other(self, *messages):
        w, (a, b) = world()
        w.handle(S.build_packet(list(messages)), a.addr)
        return w, b

    @staticmethod
    def create(router, oid, tid=None):
        id_bits, type_bits = S.ROUTER_BITS[router]

        def write(w):
            w.bits(S.MSG_OBJECT_CREATE, 7)
            w.bits(router, 4)
            key(w, id_bits, oid)
            if type_bits:
                w.bits(tid, type_bits)
            w.bits(0, 8)
            w.bits(8, 14)
            w.bits(0x5A, 8)
        return write

    @staticmethod
    def score(mtype, sizes):
        def write(w):
            w.bits(mtype, 7)
            key(w, S.CAR_KEY_ID_BITS)
            for n in sizes:
                w.varlen(n)
                for _ in range(n):
                    w.bits(0, 8)
            w.bits(1, 32)
            w.bits(0, 2 if mtype == S.MSG_LAP_SCORE else 27)
        return write

    def test_objects_inside_the_limits_are_relayed(self):
        for message in (self.create(2, 63), self.create(9, 12, 2), self.create(12, 12, 1)):
            self.assertTrue(self.sent_to_other(message)[1].outbox)

    def test_an_id_or_type_past_the_game_table_stops_the_datagram(self):
        for message in (self.create(2, 64), self.create(1, 127), self.create(9, 13, 0), self.create(9, 0, 3), self.create(12, 0, 2)):
            w, b = self.sent_to_other(message, prediction)
            self.assertEqual(b.outbox, [])
            self.assertEqual(len([l for l in w.lines if "malformed" in l]), 1)

    def test_a_client_owned_object_on_the_lobby_router_is_not_relayed(self):
        self.assertEqual(self.sent_to_other(self.create(6, 0, 1))[1].outbox, [])

    def test_scores_of_the_game_size_are_relayed(self):
        self.assertTrue(self.sent_to_other(self.score(S.MSG_LAP_SCORE, [8] * 3))[1].outbox)
        self.assertTrue(self.sent_to_other(self.score(S.MSG_SCORE_KEYFRAME, [8] * 5))[1].outbox)

    def test_a_score_field_of_another_size_stops_the_datagram(self):
        for mtype, sizes in ((S.MSG_LAP_SCORE, [8, 9, 8]), (S.MSG_LAP_SCORE, [7, 8, 8]), (S.MSG_SCORE_KEYFRAME, [8, 8, 8, 8, 0])):
            w, b = self.sent_to_other(self.score(mtype, sizes), prediction)
            self.assertEqual(b.outbox, [])
            self.assertEqual(len([l for l in w.lines if "malformed" in l]), 1)

    def test_malformed_is_logged_once_per_client(self):
        w, (a, b) = world()
        for _ in range(3):
            w.handle(S.build_packet([self.create(2, 64)]), a.addr)
        self.assertEqual(len([l for l in w.lines if "malformed" in l]), 1)


if __name__ == "__main__":
    unittest.main()
