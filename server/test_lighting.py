"""Pure unit checks: no sockets, game clients or game traffic."""
import hashlib
import hmac
import os
import struct
import sys
import unittest
from types import SimpleNamespace as NS
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lighting as L


class Lighting(unittest.TestCase):
    def setUp(self):
        self.reset_state()
        self.clock = mock.patch.object(L.time, 'monotonic', side_effect=lambda: self.now)
        self.clock.start(); self.addCleanup(self.clock.stop)

    def reset_state(self):
        self.sent = []
        self.now = 100.0
        self.a = NS(addr=('unit-a', 1), xuid=1, owner_id=10, car_id=5, connected=True, entered=True)
        self.b = NS(addr=('unit-b', 2), xuid=2, owner_id=11, car_id=6, connected=True, entered=True)
        self.world = NS(peers={p.addr: p for p in (self.a, self.b)})
        self.world.others = lambda p: [q for q in self.world.peers.values() if q is not p]
        self.keys = {1: bytes(range(32)), 2: b'b'*32}
        self.epochs = {1: b'a'*16, 2: b'b'*16}
        self.hub = NS(route={p.addr: self.world for p in (self.a,self.b)}, revoked={},
                      bound={p.addr: (p.xuid, 200, self.keys[p.xuid]) for p in (self.a,self.b)},
                      keys={x: {key: 200} for x,key in self.keys.items()},
                      ticket_epochs={p.addr: self.epochs[p.xuid] for p in (self.a,self.b)},
                      sock=NS(sendto=lambda data, addr: self.sent.append((data,addr))))

    def request(self, peer, seq=1, flags=0, key=None, epoch=None, car=None):
        out = bytearray(L.HEADER)
        out += bytes([1]) + struct.pack('<QQ',peer.xuid,seq) + (epoch or self.epochs[peer.xuid])
        out += bytes([peer.car_id if car is None else car,flags,0])
        struct.pack_into('<H',out,9,len(out)+L.MAC_SIZE)
        return L.signed(out, key or self.keys[peer.xuid])

    def call(self, peer, **kw):
        L.handle(self.hub,self.request(peer,**kw),('aux',peer.xuid))

    def records(self):
        data = self.sent[-1][0]
        key = self.keys[struct.unpack_from('<Q', data,20)[0]]
        self.assertTrue(hmac.compare_digest(data[-32:],hmac.new(key,data[:-32],hashlib.sha256).digest()))
        self.assertEqual(len(data),int.from_bytes(data[9:11],'little'))
        return [tuple(data[i:i+4]) for i in range(L.BASE,len(data)-32,4)]

    def test_owner_and_car_come_from_live_connection_and_late_join_gets_current_state(self):
        self.call(self.a,flags=7)
        self.assertEqual(self.records(),[])
        self.call(self.b,flags=2)
        self.assertEqual(self.records(),[(10,5,7,0)])
        self.now += .1
        self.call(self.a,seq=2,flags=0)
        self.assertEqual(self.records(),[(11,6,2,0)])
        self.now += .1
        self.call(self.b,seq=2)
        self.assertEqual(self.records(),[(10,5,0,0)])

    def test_forged_identity_wrong_key_car_and_epoch_do_not_publish_or_reply(self):
        for kw in ({'key':b'x'*32},{'car':7},{'epoch':b'z'*16},{'flags':8}):
            self.call(self.a,**kw)
        self.assertEqual(self.sent,[])
        self.assertFalse(hasattr(self.a,'lighting'))

    def test_only_ticket_bound_live_non_revoked_sessions_can_publish(self):
        for mutation in ('connected','entered','key','bound','revoked','epoch'):
            with self.subTest(mutation=mutation):
                self.reset_state()
                if mutation in ('connected','entered'):setattr(self.a,mutation,False)
                elif mutation=='key':self.hub.keys[1][self.keys[1]]=99
                elif mutation=='bound':del self.hub.bound[self.a.addr]
                elif mutation=='revoked':self.hub.revoked[1]=200
                else:del self.hub.ticket_epochs[self.a.addr]
                self.call(self.a)
                self.assertEqual(self.sent,[])

    def test_replay_ordering_and_rate_limit(self):
        self.call(self.a,seq=4,flags=7)
        self.call(self.a,seq=5,flags=0)  # too fast, no response/state change
        self.now += .1
        self.call(self.a,seq=3)
        self.call(self.a,seq=4)
        self.assertEqual(len(self.sent),1)
        self.assertEqual(self.a.lighting['flags'],7)
        self.call(self.a,seq=5)
        self.assertEqual(len(self.sent),2)
        self.assertEqual(self.a.lighting['flags'],0)

    def test_car_change_disconnect_expiry_revocation_and_new_ticket_drop_old_state(self):
        for mutation in ('car','connected','entered','expiry','revoked','epoch','key'):
            with self.subTest(mutation=mutation):
                self.reset_state(); self.call(self.a,flags=7)
                if mutation=='car':self.a.car_id=12
                elif mutation in ('connected','entered'):setattr(self.a,mutation,False)
                elif mutation=='expiry':self.now+=2.1
                elif mutation=='revoked':self.hub.revoked[1]=200
                elif mutation=='epoch':self.hub.ticket_epochs[self.a.addr]=b'c'*16
                else:self.hub.keys[1].clear()
                self.call(self.b)
                self.assertEqual(self.records(),[])

    def test_cross_world_state_is_not_exposed(self):
        self.call(self.a,flags=7)
        self.world.others=lambda p: []
        self.call(self.b)
        self.assertEqual(self.records(),[])

    def test_malformed_messages_fail_closed(self):
        good = self.request(self.a)
        for data in (good[:-1], good+b'x', good[:19]+b'\x02'+good[20:], good[:54]+b'\x01'+good[55:],
                     good[:9]+b'\x00\x00'+good[11:], good[:-1]+bytes([good[-1]^1])):
            L.handle(self.hub,data,('aux',1))
        self.assertEqual(self.sent,[])
