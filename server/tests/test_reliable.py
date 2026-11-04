"""Lost server messages are sent again (reliable.py): ids, acks and resend timing as the game's own connection does them (n5 1.1, 1.2)."""
import os
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fh2mp_server as S  # noqa: E402
import reliable as R  # noqa: E402
from test_send_queue import FakeSock, car, world  # noqa: E402


def update(owner=2, oid=5, seq=1):
    return S.relay_object_message(S.MSG_OBJECT_UPDATE, 1, oid, owner, None, seq, 7, 0, 8, 0x55)


def create(owner=2, oid=5, seq=0):
    return S.relay_object_message(S.MSG_OBJECT_CREATE, 1, oid, owner, 0, seq, 7, 0, 8, 0x55)


def header(pkt):
    return S.parse_header(S.BitReader(pkt))


class Window(unittest.TestCase):
    def test_ids_count_up_skip_the_unreliable_mark_and_wrap(self):
        w = R.Window()
        m = [S.bake(update())]
        self.assertEqual(w.stamp(m, 0.0), 1)
        self.assertEqual(w.stamp(m, 0.0), 2)
        w.last_id = 0xFFFE
        self.assertEqual(w.stamp(m, 0.0), 0)            # (last + 1) % 0xFFFF, never 0xFFFF
        self.assertEqual(w.stamp(m, 0.0), 1)

    def test_a_datagram_of_predictions_stays_unreliable_and_is_not_kept(self):
        w = R.Window()
        self.assertEqual(w.stamp([S.bake(car(1)), S.bake(S.ping(1))], 0.0), R.UNRELIABLE_ID)
        self.assertEqual(w.pending, {})

    def test_only_the_reliable_messages_of_a_mixed_datagram_are_kept(self):
        w = R.Window()
        u = S.bake(update())
        pid = w.stamp([S.bake(car(1)), u], 0.0)
        self.assertEqual(w.pending[pid][1], [u])

    def test_ack_bits_name_the_ids_behind_the_remote_ack(self):
        w = R.Window()
        for _ in range(4):
            w.stamp([S.bake(update())], 0.0)
        w.ack(4, 0b010)                                 # 4 itself and 4 - 1 - 1 = 2
        self.assertEqual(sorted(w.pending), [1, 3])

    def test_ack_across_the_wrap(self):
        w = R.Window()
        w.last_id = 0xFFFD
        ids = [w.stamp([S.bake(update())], 0.0) for _ in range(3)]
        self.assertEqual(ids, [0xFFFE, 0, 1])
        w.ack(1, 0b10)                                  # 1, and 1 - 1 - 1 wraps to 0xFFFE
        self.assertEqual(sorted(w.pending), [0])

    def test_a_repeated_or_old_ack_is_harmless(self):
        w = R.Window()
        pid = w.stamp([S.bake(update())], 0.0)
        w.ack(pid, 0)
        w.ack(pid, 0)
        w.ack(0xFFFE, 0)
        self.assertEqual(w.pending, {})

    def test_resend_waits_for_the_client_rule(self):
        w = R.Window()
        w.stamp([S.bake(update())], 0.0)
        self.assertEqual(w.due(R.RESEND_SECONDS, 2.0), [])
        self.assertEqual(len(w.due(R.RESEND_SECONDS + 0.1, 2.0)), 1)
        self.assertEqual(w.pending, {})

    def test_nothing_is_resent_to_a_client_that_went_quiet(self):
        w = R.Window()
        w.stamp([S.bake(create())], 0.0)
        self.assertEqual(w.due(10.0, 0.1), [])          # loading: it may hold the original, and a second create recreates the object
        self.assertEqual(len(w.pending), 1)
        self.assertEqual(len(w.due(10.0, 9.9)), 1)      # it is back and still has not acked
        self.assertEqual(w.resent_creates, 1)

    def test_the_backlog_is_bounded_and_counted(self):
        w = R.Window()
        for _ in range(R.MAX_PENDING + 5):
            w.stamp([S.bake(update())], 0.0)
        self.assertEqual(len(w.pending), R.MAX_PENDING)
        self.assertEqual(w.lost, 5)

    def test_relay_owner_reads_the_relay_key(self):
        self.assertEqual(R.relay_owner(S.bake(update(owner=9)), S.ROUTER_BITS), 9)
        self.assertEqual(R.relay_owner(S.bake(create(owner=127)), S.ROUTER_BITS), 127)
        self.assertIsNone(R.relay_owner(S.bake(S.finalise_session_entry()), S.ROUTER_BITS))

    def test_a_leaver_loses_its_waiting_objects_and_nobody_elses(self):
        w = R.Window()
        mine, theirs = S.bake(update(owner=2)), S.bake(update(owner=3))
        one = w.stamp([mine], 0.0)
        two = w.stamp([mine, theirs], 0.0)
        w.forget_owner(2, S.ROUTER_BITS)
        self.assertNotIn(one, w.pending)
        self.assertEqual(w.pending[two][1], [theirs])


class Server(unittest.TestCase):
    def setUp(self):
        self.was = S.RELIABLE
        S.RELIABLE = True
        self.addCleanup(setattr, S, "RELIABLE", self.was)

    def test_off_sends_exactly_the_old_packets(self):
        S.RELIABLE = False
        w, (a, b, c) = world()
        w.send(b, [update(), car(1)])
        w.flush(float("inf"))
        self.assertEqual(w.sock.sent[0][0], S.build_packet([update(), car(1)], 0xFFFF, S.NO_ID, 0))
        self.assertEqual(b.window.pending, {})

    def test_a_lost_datagram_comes_back_with_a_new_id_and_acks_stop_it(self):
        w, (a, b, c) = world()
        w.send(b, [update()])
        w.flush(float("inf"))
        first = header(w.sock.sent[-1][0])["ack_id"]
        self.assertEqual(first, 1)
        at = b.window.pending[first][0]
        b.last_rx = at + 1.0                            # the client is alive, the datagram did not arrive
        b.window.pending[first] = (at - R.RESEND_SECONDS - 1, b.window.pending[first][1])
        b.last_keepalive = time.monotonic()
        w.tick()
        w.flush(float("inf"))
        pkt = w.sock.sent[-1][0]
        self.assertEqual(header(pkt)["ack_id"], 2)
        self.assertEqual(header(pkt)["count"], 1)
        self.assertEqual(b.window.resent, 1)
        b.window.ack(2, 0)
        self.assertEqual(b.window.pending, {})

    def test_every_client_packet_carries_acks(self):
        w, (a, b, c) = world()
        w.send(b, [update()])
        w.flush(float("inf"))
        w.handle(S.build_packet([S.ping(1)], 0xFFFF, 1, 0), b.addr)
        self.assertEqual(b.window.pending, {})

    def test_a_client_not_yet_connected_gets_no_reliable_ids(self):
        w, (a, b, c) = world()
        b.connected = False
        w.send(b, [S.close_connection(0)])
        w.flush(float("inf"))
        self.assertEqual(header(w.sock.sent[-1][0])["ack_id"], 0xFFFF)

    def test_leaving_clears_the_leavers_messages_for_others(self):
        w, (a, b, c) = world()
        w.send(c, [update(owner=b.owner_id)])
        w.flush(float("inf"))
        self.assertTrue(c.window.pending)
        w.drop_peer(b, "test")
        self.assertFalse([m for _, ms in c.window.pending.values() for m in ms if R.relay_owner(m, S.ROUTER_BITS) == b.owner_id and m[0] & 0x7F == S.MSG_OBJECT_UPDATE])

    def test_a_kick_carries_this_connections_ids(self):
        w, (a, b, c) = world()
        b.client_counter, w.server_counter = 77, 88
        w.send(b, [w.goodbye(b)])
        w.flush(float("inf"))
        r = S.BitReader(w.sock.sent[-1][0])
        S.parse_header(r)
        self.assertEqual((r.bits(7), r.bits(3), r.bits(32), r.bits(32)), (S.MSG_CLOSE_CONNECTION, 0, 77, 88))     # 0x1764b10

    def test_client_resends_are_counted_and_still_relayed(self):
        w, (a, b, c) = world()
        msg = S.object_message(S.MSG_OBJECT_UPDATE, 1, 5, None, 3, 7, 0, lambda v: v.bits(1, 8))
        for _ in range(2):
            w.handle(S.build_packet([msg], 0xFFFF, 0, 0), b.addr)
        self.assertEqual(b.dup_objects, 1)


if __name__ == "__main__":
    unittest.main()
