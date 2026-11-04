"""Pure unit checks: no sockets, game clients or game traffic."""
import hashlib
import hmac
import os
import struct
import sys
import unittest
from types import SimpleNamespace as NS
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import honk as H


class Honk(unittest.TestCase):
    def setUp(self):
        self.reset_state()
        self.clock = mock.patch.object(H.time, 'monotonic', side_effect=lambda: self.now)
        self.clock.start(); self.addCleanup(self.clock.stop)

    def reset_state(self):
        self.sent, self.logged = [], []
        self.now = 100.0
        self.a = NS(addr=('unit-a', 1), xuid=1, owner_id=10, car_id=5, connected=True, entered=True, pos=(10.0, 0.0, 20.0))
        self.b = NS(addr=('unit-b', 2), xuid=2, owner_id=11, car_id=6, connected=True, entered=True, pos=(30.0, 1.0, 20.0))
        self.world = NS(peers={p.addr: p for p in (self.a, self.b)})
        self.world.others = lambda p: [q for q in self.world.peers.values() if q is not p]
        self.keys = {1: bytes(range(32)), 2: b'b' * 32}
        self.epochs = {1: b'a' * 16, 2: b'b' * 16}
        self.hub = NS(route={p.addr: self.world for p in (self.a, self.b)}, revoked={},
                      bound={p.addr: (p.xuid, 200, self.keys[p.xuid]) for p in (self.a, self.b)},
                      keys={x: {key: 200} for x, key in self.keys.items()},
                      ticket_epochs={p.addr: self.epochs[p.xuid] for p in (self.a, self.b)},
                      sock=NS(sendto=lambda data, addr: self.sent.append((data, addr))), log=self.logged.append)

    def request(self, peer, seq=1, horn=H.HORN_OFF, press=0, key=None, epoch=None, car=None, reserved=0):
        out = bytearray(H.HEADER)
        out += bytes([1]) + struct.pack('<QQ', peer.xuid, seq) + (epoch or self.epochs[peer.xuid])
        out += bytes([peer.car_id if car is None else car, horn, reserved]) + struct.pack('<H', press)
        struct.pack_into('<H', out, 9, len(out) + H.MAC_SIZE)
        return H.lighting.signed(out, key or self.keys[peer.xuid])

    def call(self, peer, **kw):
        H.handle(self.hub, self.request(peer, **kw), ('aux', peer.xuid))

    def records(self):
        data = self.sent[-1][0]
        key = self.keys[struct.unpack_from('<Q', data, 20)[0]]
        self.assertTrue(hmac.compare_digest(data[-32:], hmac.new(key, data[:-32], hashlib.sha256).digest()))
        self.assertEqual(len(data), int.from_bytes(data[9:11], 'little'))
        self.assertEqual(data[54], (len(data) - H.BASE - H.MAC_SIZE) // H.RECORD_SIZE)
        return [struct.unpack_from('<BBBBHH3f', data, i) for i in range(H.BASE, len(data) - 32, H.RECORD_SIZE)]

    def test_a_held_horn_reaches_the_other_player_with_the_cars_position(self):
        self.call(self.a, horn=7, press=3)
        self.assertEqual(self.records(), [])
        self.now += .25
        self.call(self.b, seq=1)
        self.assertEqual(self.records(), [(10, 5, 7, 1, 3, 250, 10.0, 0.0, 20.0)])
        self.assertEqual(self.sent[-1][1], ('aux', 2))

    def test_releasing_the_horn_removes_it_at_once(self):
        self.call(self.a, horn=7, press=3)
        self.now += .1
        self.call(self.a, seq=2, horn=H.HORN_OFF, press=3)
        self.call(self.b)
        self.assertEqual(self.records(), [])

    def test_a_horn_that_is_not_refreshed_stops_after_the_lease(self):
        self.call(self.a, horn=7, press=3)
        self.now += H.LEASE_SECONDS - .05
        self.call(self.b)
        self.assertEqual(len(self.records()), 1)
        self.now += .1
        self.call(self.b, seq=2)
        self.assertEqual(self.records(), [])

    def test_a_press_is_capped_and_a_new_one_waits_for_the_cooldown(self):
        seq = 0
        def hold(press, seconds, step=.25):
            nonlocal seq
            end = self.now + seconds
            while self.now < end:
                seq += 1
                self.call(self.a, seq=seq, horn=7, press=press)
                self.now += step
        hold(1, H.CAP_SECONDS + 1)
        seq += 1
        self.call(self.b, seq=seq)
        self.assertEqual(self.records(), [])                       # silenced although the key is still down
        self.assertTrue(any('silenced' in line for line in self.logged))
        seq += 1
        self.call(self.a, seq=seq, horn=H.HORN_OFF, press=1)
        self.now += .1
        hold(2, .3)                                                # a new press inside the cooldown
        self.call(self.b, seq=seq + 1)
        self.assertEqual(self.records(), [])
        self.now += H.COOLDOWN_SECONDS
        hold(3, .3)
        self.call(self.b, seq=seq + 2)
        self.assertEqual([r[4] for r in self.records()], [3])

    def test_two_quick_presses_are_two_presses(self):
        self.call(self.a, horn=7, press=1)
        self.now += .05
        self.call(self.a, seq=2, horn=H.HORN_OFF, press=1)
        self.now += .05
        self.call(self.a, seq=3, horn=7, press=2)
        self.call(self.b)
        self.assertEqual([(r[4], r[5]) for r in self.records()], [(2, 0)])

    def test_forged_identity_wrong_key_car_epoch_and_bad_fields_do_not_publish_or_reply(self):
        for kw in ({'key': b'x' * 32}, {'car': 7}, {'epoch': b'z' * 16}, {'horn': 31}, {'horn': 200}, {'reserved': 1}):
            self.call(self.a, **{'horn': 7, **kw})
        self.assertEqual(self.sent, [])
        self.assertFalse(hasattr(self.a, 'honk'))

    def test_only_ticket_bound_live_non_revoked_sessions_can_publish(self):
        for mutation in ('connected', 'entered', 'key', 'bound', 'revoked', 'epoch'):
            with self.subTest(mutation=mutation):
                self.reset_state()
                if mutation in ('connected', 'entered'): setattr(self.a, mutation, False)
                elif mutation == 'key': self.hub.keys[1][self.keys[1]] = 99
                elif mutation == 'bound': del self.hub.bound[self.a.addr]
                elif mutation == 'revoked': self.hub.revoked[1] = 200
                else: del self.hub.ticket_epochs[self.a.addr]
                self.call(self.a, horn=7)
                self.assertEqual(self.sent, [])

    def test_replay_ordering_and_rate_limit(self):
        self.call(self.a, seq=4, horn=7, press=1)
        self.call(self.a, seq=5, horn=H.HORN_OFF, press=1)         # too fast: no reply, no state change
        self.now += .1
        self.call(self.a, seq=3, horn=H.HORN_OFF, press=1)
        self.call(self.a, seq=4, horn=H.HORN_OFF, press=1)
        self.assertEqual(len(self.sent), 1)
        self.assertEqual(self.a.honk['horn'], 7)
        self.call(self.a, seq=5, horn=H.HORN_OFF, press=1)
        self.assertEqual(len(self.sent), 2)
        self.assertIsNone(self.a.honk['horn'])

    def test_car_change_disconnect_revocation_and_new_ticket_drop_the_horn(self):
        for mutation in ('car', 'connected', 'entered', 'revoked', 'epoch', 'key'):
            with self.subTest(mutation=mutation):
                self.reset_state(); self.call(self.a, horn=7, press=1)
                if mutation == 'car': self.a.car_id = 12
                elif mutation in ('connected', 'entered'): setattr(self.a, mutation, False)
                elif mutation == 'revoked': self.hub.revoked[1] = 200
                elif mutation == 'epoch': self.hub.ticket_epochs[self.a.addr] = b'c' * 16
                else: self.hub.keys[1].clear()
                self.call(self.b)
                self.assertEqual(self.records(), [])

    def test_other_worlds_and_other_events_are_not_heard(self):
        self.call(self.a, horn=7, press=1)
        self.world.same_event = lambda x, y: False                 # one is in a race, the other in free roam
        self.call(self.b)
        self.assertEqual(self.records(), [])
        self.world.same_event = lambda x, y: True
        self.now += .1
        self.call(self.b, seq=2)
        self.assertEqual(len(self.records()), 1)
        self.world.others = lambda p: []
        self.now += .1
        self.call(self.b, seq=3)
        self.assertEqual(self.records(), [])

    def test_far_horns_are_left_out_and_an_unknown_position_is_flagged(self):
        self.call(self.a, horn=7, press=1)
        self.b.pos = (10.0 + H.MAX_DISTANCE + 1, 0.0, 20.0)
        self.call(self.b)
        self.assertEqual(self.records(), [])
        self.a.pos = None
        self.now += .1
        self.call(self.a, seq=2, horn=7, press=1)
        self.call(self.b, seq=2)
        self.assertEqual([r[3] for r in self.records()], [0])

    def test_the_whole_feature_can_be_switched_off(self):
        with mock.patch.dict(os.environ, {'FH2MP_NO_HONK': '1'}):
            self.call(self.a, horn=7)
        self.assertEqual(self.sent, [])

    def test_at_most_twelve_records(self):
        peers = [NS(addr=('p', i), xuid=100 + i, owner_id=20 + i, car_id=1, connected=True, entered=True, pos=(0.0, 0.0, 0.0)) for i in range(14)]
        for p in peers:
            self.world.peers[p.addr] = p
            self.hub.route[p.addr] = self.world
            self.keys[p.xuid], self.epochs[p.xuid] = bytes(32), bytes(16)
            self.hub.bound[p.addr] = (p.xuid, 200, bytes(32))
            self.hub.keys[p.xuid] = {bytes(32): 200}
            self.hub.ticket_epochs[p.addr] = bytes(16)
            self.call(p, horn=1, press=1)
        self.call(self.b)
        self.assertEqual(len(self.records()), H.MAX_RECORDS)

    def test_malformed_messages_fail_closed(self):
        good = self.request(self.a, horn=7)
        for data in (good[:-1], good + b'x', good[:19] + b'\x02' + good[20:], good[:9] + b'\x00\x00' + good[11:], good[:-1] + bytes([good[-1] ^ 1])):
            H.handle(self.hub, data, ('aux', 1))
        self.assertEqual(self.sent, [])


if __name__ == '__main__':
    unittest.main()
