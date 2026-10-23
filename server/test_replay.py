"""The history replay for late joiners (replay.py, Server.replay_to / relay_numbered): compact, gap-free for the joiner, live numbering continues."""
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S  # noqa: E402
import replay  # noqa: E402

UPD, CRE = S.MSG_OBJECT_UPDATE, S.MSG_OBJECT_CREATE


def var(vid, bits, value):
    return 8 + bits, vid | (value << 8)


def decode(baked):
    """(mtype, router, owner, seq) of a baked relay_object_message / relay_delete_message."""
    value, size = baked
    r = S.BitReader(value.to_bytes((size + 7) // 8 + 1, "little"))
    mtype = r.bits(7)
    router = r.bits(4)
    r.bits(2)
    id_bits, type_bits = S.ROUTER_BITS[router]
    r.bits(id_bits)
    owner = r.bits(8)
    if mtype == CRE and type_bits:
        r.bits(type_bits)
    return mtype, router, owner, r.bits(8), r.bits(14) if mtype != S.MSG_OBJECT_DELETE else 0


class Compact(unittest.TestCase):
    def hist(self, n_xp=300):
        h = [(CRE, 0, 0, 2500, 0xABC)]
        for i in range(n_xp):
            h.append((UPD, None, len(h) % 255, *var(27, 32, i)))
            if i % 50 == 0:
                h.append((UPD, None, len(h) % 255, *var(19, 4, i // 50 % 5)))
            if i % 97 == 0:
                h.append((UPD, None, len(h) % 255, 1502, 0x1234 + i))      # a car change: not a single variable, always kept
        return h

    def test_only_the_last_value_of_a_single_variable_survives(self):
        h = self.hist()
        c = replay.compact(1, h, UPD)
        xp = [e for e in c if replay.single_var(1, e[3], e[4]) == 27]
        self.assertEqual(len(xp), 1)
        self.assertEqual(xp[0][4] >> 8, 299)                                  # the last XP value
        self.assertEqual(sum(1 for e in c if e[3] == 1502), sum(1 for e in h if e[3] == 1502))
        self.assertEqual(c[0], h[0])                                          # the create stays first
        self.assertLess(len(c), 20)

    def test_other_routers_and_unknown_variables_are_untouched(self):
        h = [(CRE, 0, 0, 40, 1)] + [(UPD, None, i, *var(5, 32, i)) for i in range(1, 9)]
        self.assertEqual(replay.compact(1, h, UPD), h)                         # variable 5 is not whitelisted
        self.assertEqual(replay.compact(3, h, UPD), h)
        self.assertEqual(replay.single_var(1, 40, 27), 27 | 0)                  # a 40-bit message of variable 27
        self.assertIsNone(replay.single_var(1, 41, 27))                        # not exactly one variable: two packed together

    def test_renumber_and_wire_seq(self):
        e = replay.renumber([(CRE, 0, 99, 10, 1)] + [(UPD, None, 99, 10, 1)] * 300)
        self.assertEqual([x[2] for x in e[:3]] + [e[254][2], e[255][2]], [0, 1, 2, 254 % 255, 255 % 255])
        self.assertEqual(replay.wire_seq(1000, 990), 10)
        self.assertEqual(replay.wire_seq(300, 0), 300 % 255)


class Receiver:
    """A game: applies the messages of one object strictly in sequence order; remembers the last value of every variable."""

    def __init__(self):
        self.expect = {}
        self.state = {}
        self.errors = []

    def take(self, baked):
        mtype, router, owner, seq, size = decode(baked)
        key = (owner, router)
        want = self.expect.get(key, 0)
        if seq != want:
            self.errors.append((key, seq, want))
        self.expect[key] = (seq + 1) % 255
        return mtype, router, owner


class Joiners(unittest.TestCase):
    def world(self):
        import test_freeroam_tag_grant as T
        w = S.Server(0, None, sock=T.FakeSock(), log_file=None)
        peers = []
        for i in (1, 2, 3):
            p = S.Peer(("10.1.0.%d" % i, 5000 + i))
            p.connected, p.entered, p.owner_id = True, i != 3, i
            w.peers[p.addr] = p
            peers.append(p)
        return w, peers

    def drain(self, q):
        out, q.outbox = q.outbox, []
        return out

    def test_a_late_joiner_gets_a_short_gap_free_replay_and_live_messages_that_continue(self):
        w, (a, b, c) = self.world()
        old, new = Receiver(), Receiver()
        w.remember_and_relay(a, CRE, 1, 0, 0, 0, 2500, 0xABC)
        for i in range(1, 2200):                                               # a long session: XP updates, status changes, car changes
            if i % 400 == 0:
                w.remember_and_relay(a, UPD, 1, 0, None, 0, 1502, 0x1234 + i)
            else:
                w.remember_and_relay(a, UPD, 1, 0, None, 0, *var(27 if i % 5 else 19, 32 if i % 5 else 4, i & 15))
            for baked in self.drain(b):                                         # b has been there all the time
                old.take(baked)
        self.assertEqual(old.errors, [])
        stored = len(a.objs[(1, 0)])
        self.assertLess(stored, 300)                                            # the stored history is compacted while it grows
        sent = []
        w.send = lambda q, msgs, **kw: sent.extend(bake for bake in (S.bake(m) for m in msgs))
        c.entered = True
        w.replay_to(c)
        for baked in sent:
            new.take(baked)
        self.assertEqual(new.errors, [])
        self.assertLess(len(sent), 20)
        for i in range(2200, 2700):                                             # the next 500 live messages, across the 255 wrap
            w.remember_and_relay(a, UPD, 1, 0, None, 0, *var(27, 32, i))
            for baked in self.drain(b):
                old.take(baked)
            for baked in self.drain(c):
                new.take(baked)
        self.assertEqual(old.errors, [])
        self.assertEqual(new.errors, [])                                        # the joiner's numbers continue without a gap
        self.assertEqual(a.relay_next[(1, 0)], 2700)
        self.assertGreater(c.seq_shift[(1, 1, 0)], 2000)

    def test_the_delete_when_the_owner_leaves_is_numbered_for_each_recipient(self):
        w, (a, b, c) = self.world()
        old, new = Receiver(), Receiver()
        w.remember_and_relay(a, CRE, 1, 0, 0, 0, 2500, 0xABC)
        for i in range(1, 700):
            w.remember_and_relay(a, UPD, 1, 0, None, 0, *var(27, 32, i))
            for baked in self.drain(b):
                old.take(baked)
        sent = []
        w.send = lambda q, msgs, **kw: sent.extend(S.bake(m) for m in msgs)
        c.entered = True
        w.replay_to(c)
        for baked in sent:
            new.take(baked)
        got = {}
        w.drop_peer(a, "test")
        for name, q, rec in (("b", b, old), ("c", c, new)):
            for baked in self.drain(q):
                got[name] = decode(baked)
                rec.take(baked)
        self.assertEqual((old.errors, new.errors), ([], []))                     # both saw the delete with the number they expected
        self.assertEqual(got["b"][0], got["c"][0])
        self.assertNotEqual(got["b"][3], got["c"][3])

    def test_a_joiner_to_a_young_object_is_not_shifted(self):
        w, (a, b, c) = self.world()
        w.remember_and_relay(a, CRE, 1, 0, 0, 0, 2500, 0xABC)
        for i in range(5):
            w.remember_and_relay(a, UPD, 1, 0, None, 0, 1502, 0x40 + i)
        sent = []
        w.send = lambda q, msgs, **kw: sent.extend(S.bake(m) for m in msgs)
        c.entered = True
        w.replay_to(c)
        self.assertEqual(len(sent), 6)
        self.assertEqual(c.seq_shift, {})


class WipeReport(unittest.TestCase):
    def test_the_report_carries_the_bytes_of_the_big_objects_before_it(self):
        import time
        import test_freeroam_tag_grant as T
        w = S.Server(0, None, sock=T.FakeSock(), log_file=None)
        a = S.Peer(("10.2.0.1", 5001))
        a.connected, a.entered, a.owner_id = True, True, 7
        w.peers[a.addr] = a
        lines = []
        w.log = lines.append
        now = time.monotonic()
        w.remember_and_relay(a, UPD, 1, 0, None, 0, 1502, 0xDEADBEEF)
        w.remember_and_relay(a, UPD, 1, 0, None, 0, 40, 27 | (5 << 8))          # a small update is not worth a line
        w.drops.extend((now + 20, 20 + i, 0x9000000000000 + i, now + 1, None, 0, "no packets for 20 s") for i in range(3))   # last heard 1 s after the object
        w.report_wipe(now + 21)
        objs = [ln for ln in lines if " obj " in ln]
        self.assertEqual(len(objs), 1)
        self.assertIn("update router 1 1502 bits hex=", objs[0])
        self.assertIn("efbeadde", objs[0])                                        # 0xDEADBEEF, little endian


if __name__ == "__main__":
    unittest.main()
