"""The car select hold and the group deadline (server/carsel.py + Server.carsel_answer): the signed answer a client's cars-loaded wait and car select
countdown ask for, and the decision behind it."""
import hashlib
import hmac
import os
import struct
import sys
import unittest
from types import SimpleNamespace as NS
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import carsel as C
import fh2mp_server as S
import test_race_flow as T

CAR_SELECT, PRE_RACE = 1, 3


class Wire(unittest.TestCase):
    def setUp(self):
        self.now, self.hold, self.left, self.sent, self.logged = 100.0, False, None, [], []
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(C.time, 'monotonic', side_effect=lambda: self.now).start()
        self.a = NS(addr=('unit-a', 1), xuid=1, owner_id=10, connected=True, entered=True)
        self.world = NS(peers={self.a.addr: self.a}, carsel_answer=lambda peer: (self.hold, self.left))
        self.key, self.epoch = bytes(range(32)), b'a' * 16
        self.hub = NS(route={self.a.addr: self.world}, revoked={}, bound={self.a.addr: (1, 200, self.key)}, keys={1: {self.key: 200}},
                      ticket_epochs={self.a.addr: self.epoch}, sock=NS(sendto=lambda data, addr: self.sent.append((data, addr))), log=self.logged.append)

    def request(self, seq=1, state=1, key=None, epoch=None, reserved=0, tail=b'\0\0\0\0', xuid=1):
        out = bytearray(C.HEADER)
        out += bytes([1]) + struct.pack('<QQ', xuid, seq) + (epoch or self.epoch) + bytes([state, reserved]) + tail
        struct.pack_into('<H', out, 9, len(out) + C.MAC_SIZE)
        return C.lighting.signed(bytes(out), key or self.key)

    def call(self, **kw):
        C.handle(self.hub, self.request(**kw), ('aux', 1))

    def answer(self):
        data = self.sent[-1][0]
        self.assertEqual(len(data), C.REQUEST_SIZE)
        self.assertEqual(int.from_bytes(data[9:11], 'little'), len(data))
        self.assertTrue(hmac.compare_digest(data[-32:], hmac.new(self.key, data[:-32], hashlib.sha256).digest()))
        self.assertEqual((data[19], data[53]), (2, 0))
        return data[52], struct.unpack_from('<I', data, 54)[0]

    def test_layout_matches_the_spec(self):
        self.assertEqual((C.BASE, C.REQUEST_SIZE, len(C.HEADER), C.MAGIC), (58, 90, 19, b'FH2MPCS2'))
        self.assertEqual(len(self.request()), 90)

    def test_the_answer_says_hold_or_release_and_how_long_is_left(self):
        self.hold, self.left = True, 12345
        self.call(seq=1)
        self.assertEqual(self.answer(), (1, 12345))
        self.assertEqual(self.sent[-1][1], ('aux', 1))
        self.hold, self.left = False, 0
        self.now += .1
        self.call(seq=2, state=2)
        self.assertEqual(self.answer(), (0, 0))

    def test_no_deadline_is_all_ones(self):
        self.call()
        self.assertEqual(self.answer(), (0, C.NO_DEADLINE))

    def test_the_answer_echoes_the_request_so_the_shim_can_match_it(self):
        self.call(seq=7)
        data, req = self.sent[-1][0], self.request(seq=7)
        self.assertEqual(data[:19] + data[20:52], req[:19] + req[20:52])      # header, xuid, seq, epoch; type and the answer bytes differ
        self.assertEqual(struct.unpack_from('<Q', data, 28)[0], 7)

    def test_refused_requests_get_no_answer(self):
        for kw in ({'key': b'x' * 32}, {'epoch': b'z' * 16}, {'seq': 0}, {'reserved': 1}, {'state': 4}, {'tail': b'\0\0\0\1'}, {'xuid': 2}):
            self.call(**kw)
        self.assertEqual(self.sent, [])
        self.hub.revoked[1] = True
        self.call()
        self.assertEqual(self.sent, [])

    def test_a_repeated_or_old_sequence_and_a_burst_are_ignored(self):
        self.call(seq=5)
        self.call(seq=5)
        self.now += .001
        self.call(seq=6)
        self.assertEqual(len(self.sent), 1)
        self.now += .1
        self.call(seq=4)
        self.assertEqual(len(self.sent), 1)
        self.call(seq=8)
        self.assertEqual(len(self.sent), 2)

    def test_it_can_be_switched_off(self):
        with mock.patch.dict(os.environ, {'FH2MP_NO_CARSEL': '1'}):
            self.call()
        self.assertEqual(self.sent, [])


class Answer(unittest.TestCase):
    drain, send, advance, setUp, join = T.RaceFlow.drain, T.RaceFlow.send, T.RaceFlow.advance, T.RaceFlow.setUp, T.RaceFlow.join
    select, grant, options, car, ready, request = (T.RaceFlow.select, T.RaceFlow.grant, T.RaceFlow.options, T.RaceFlow.car, T.RaceFlow.ready,
                                                   T.RaceFlow.request)

    def status(self, client, value):
        update = T.Bits().put(S.PLAYER_VAR_STATUS, 8).put(value, 4)
        self.send(client, T.obj(1, update, client.player_seq))
        client.player_seq = (client.player_seq + 1) % 255

    def answer(self, client):
        return self.w.carsel_answer(self.w.peers[client.addr])

    def test_no_answer_without_an_event(self):
        self.assertEqual(self.answer(self.a), (False, None))

    def test_the_deadline_is_the_first_entry_plus_20_s_for_everybody(self):
        self.select()
        self.advance(1)
        self.status(self.a, CAR_SELECT)
        self.advance(3)
        self.status(self.b, CAR_SELECT)                       # 3 s later: gets the same deadline, not its own 20 s
        left_a, left_b = self.answer(self.a)[1], self.answer(self.b)[1]
        self.assertEqual(left_a, left_b)
        self.assertAlmostEqual(left_a / 1000, S.CARSEL_SELECT_SECONDS - 3.0, delta=0.6)
        self.assertEqual(len([line for line in self.logs if 'is the first in it' in line]), 1)

    def test_without_an_entry_the_deadline_comes_from_the_fallback(self):
        self.select()
        left = self.answer(self.a)[1] / 1000
        started = self.w.event['started']
        self.assertAlmostEqual(left, started + S.CARSEL_ENTRY_FALLBACK_SECONDS + S.CARSEL_SELECT_SECONDS - self.now, delta=0.01)

    def test_an_early_picker_is_held_until_the_last_participant_is_done(self):
        self.select()
        self.status(self.a, CAR_SELECT)
        self.status(self.b, CAR_SELECT)
        self.assertTrue(self.answer(self.a)[0])
        self.status(self.a, PRE_RACE)
        self.assertTrue(self.answer(self.a)[0])
        self.assertTrue(any('carsel: owner' in line and 'waits for' in line for line in self.logs))
        self.status(self.b, PRE_RACE)
        self.assertEqual(self.answer(self.a)[0], False)
        self.assertEqual(self.answer(self.b)[0], False)

    def test_the_hold_ends_a_grace_after_the_deadline_whoever_is_still_choosing(self):
        self.select()
        self.status(self.a, CAR_SELECT)
        self.status(self.b, CAR_SELECT)
        self.status(self.a, PRE_RACE)
        self.assertTrue(self.answer(self.a)[0])
        self.advance(S.CARSEL_SELECT_SECONDS + S.CARSEL_HOLD_GRACE_SECONDS - 1)
        if self.w.event:
            self.assertTrue(self.answer(self.a)[0])
            self.advance(2)
        if self.w.event:
            hold, left = self.answer(self.a)
            self.assertEqual((hold, left), (False, 0))
            self.assertTrue(any('hold ends' in line for line in self.logs))

    def test_a_participant_that_left_does_not_hold_the_others(self):
        self.select()
        self.status(self.a, CAR_SELECT)
        self.status(self.b, CAR_SELECT)
        self.status(self.a, PRE_RACE)
        self.assertTrue(self.answer(self.a)[0])
        self.w.peers[self.b.addr].connected = False
        self.assertFalse(self.answer(self.a)[0])

    def test_a_road_trip_is_never_held_and_has_no_deadline(self):
        self.select()
        self.status(self.a, CAR_SELECT)
        self.w.roadtrip = True
        self.assertEqual(self.answer(self.a), (False, None))


if __name__ == "__main__":
    unittest.main()
