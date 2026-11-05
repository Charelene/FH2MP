"""Pure unit checks: no sockets, game clients or game traffic."""
import hashlib
import hmac
import importlib.util
import os
import struct
import sys
import unittest
from types import SimpleNamespace as NS
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import quickchat as Q


class QuickChat(unittest.TestCase):
    def setUp(self):
        self.sent, self.logged = [], []
        self.now = 100.0
        self.clock = mock.patch.object(Q.time, 'monotonic', side_effect=lambda: self.now)
        self.clock.start(); self.addCleanup(self.clock.stop)
        self.a = NS(addr=('unit-a', 1), xuid=1, owner_id=10, connected=True, entered=True)
        self.b = NS(addr=('unit-b', 2), xuid=2, owner_id=11, connected=True, entered=True)
        self.c = NS(addr=('unit-c', 3), xuid=3, owner_id=12, connected=True, entered=True)
        self.world = NS(peers={p.addr: p for p in (self.a, self.b, self.c)})
        self.world.others = lambda p: [q for q in self.world.peers.values() if q is not p]
        self.keys = {1: bytes(range(32)), 2: b'b' * 32, 3: b'c' * 32}
        self.epochs = {1: b'a' * 16, 2: b'b' * 16, 3: b'c' * 16}
        self.seq = {1: 0, 2: 0, 3: 0}
        peers = (self.a, self.b, self.c)
        self.hub = NS(route={p.addr: self.world for p in peers}, revoked={},
                      bound={p.addr: (p.xuid, 200, self.keys[p.xuid]) for p in peers},
                      keys={x: {key: 200} for x, key in self.keys.items()},
                      ticket_epochs={p.addr: self.epochs[p.xuid] for p in peers},
                      gamertags={1: "Trinculo54", 2: "Dusk Carton", 3: "Ärger"},
                      sock=NS(sendto=lambda data, addr: self.sent.append((data, addr))), log=self.logged.append)

    def request(self, peer, number=0, phrase=0, seq=None, key=None, epoch=None, reserved=0):
        if seq is None:
            self.seq[peer.xuid] += 1
            seq = self.seq[peer.xuid]
        out = bytearray(Q.HEADER)
        out += bytes([1]) + struct.pack('<QQ', peer.xuid, seq) + (epoch or self.epochs[peer.xuid])
        out += struct.pack('<IBB', number, phrase, reserved)
        struct.pack_into('<H', out, 9, len(out) + Q.MAC_SIZE)
        return Q.lighting.signed(out, key or self.keys[peer.xuid])

    def call(self, peer, **kw):
        self.sent.clear()
        Q.handle(self.hub, self.request(peer, **kw), ('aux', peer.xuid))

    def reply(self):
        """(taken number, taken phrase, [(owner, phrase, number, age ms, name)]) of the last reply, checked as the client checks it."""
        self.assertEqual(len(self.sent), 1)
        data = self.sent[-1][0]
        key = self.keys[struct.unpack_from('<Q', data, 20)[0]]
        self.assertTrue(hmac.compare_digest(data[-32:], hmac.new(key, data[:-32], hashlib.sha256).digest()))
        self.assertEqual(len(data), int.from_bytes(data[9:11], 'little'))
        self.assertEqual(data[19], 2)
        count = data[57]
        self.assertEqual(len(data), Q.BASE + count * Q.RECORD_SIZE + Q.MAC_SIZE)
        recs = []
        for i in range(count):
            at = Q.BASE + i * Q.RECORD_SIZE
            owner, phrase, nlen, res, number, age, res2 = struct.unpack_from('<BBBBIHH', data, at)
            self.assertEqual((res, res2), (0, 0))
            name = data[at + 12:at + 12 + nlen].decode('utf-8')
            recs.append((owner, phrase, number, age, name))
        number, phrase = struct.unpack_from('<IB', data, 52)
        return number, phrase, recs

    def test_a_message_reaches_the_other_players_with_the_senders_gamertag(self):
        self.call(self.a, number=1, phrase=2)
        self.assertEqual(self.reply(), (1, 2, []))                       # taken; nobody else has a message
        self.now += .25
        self.call(self.b)
        self.assertEqual(self.reply()[2], [(10, 2, 1, 250, "Trinculo54")])
        self.call(self.c)
        self.assertEqual(self.reply()[2], [(10, 2, 1, 250, "Trinculo54")])
        self.assertEqual(self.sent[-1][1], ('aux', 3))
        self.assertIn("quickchat: owner 10 message 1 phrase 2", self.logged)

    def test_a_gamertag_with_spaces_or_letters_beyond_ascii_comes_through(self):
        self.call(self.b, number=1, phrase=3)
        self.call(self.c, number=1, phrase=4)
        self.call(self.a)
        self.assertEqual(sorted(r[4] for r in self.reply()[2]), ["Dusk Carton", "Ärger"])

    def test_no_gamertag_announced_gives_an_empty_name(self):
        del self.hub.gamertags[1]
        self.call(self.a, number=1, phrase=0)
        self.call(self.b)
        self.assertEqual(self.reply()[2][0][4], "")

    def test_a_message_is_passed_on_for_ten_seconds_then_dropped(self):
        self.call(self.a, number=1, phrase=0)
        for _ in range(9):
            self.now += 1.0
            self.call(self.a, number=1, phrase=0)             # the client keeps sending its newest
            self.call(self.b)
            self.assertEqual(len(self.reply()[2]), 1)
        self.now += 1.1
        self.call(self.a, number=1, phrase=0)
        self.call(self.b)
        self.assertEqual(self.reply()[2], [])

    def test_a_client_that_stopped_asking_has_no_live_message(self):
        self.call(self.a, number=1, phrase=0)
        self.now += Q.LEASE_SECONDS + .1
        self.call(self.b)
        self.assertEqual(self.reply()[2], [])

    def test_a_second_message_within_a_second_waits_until_it_may_be_taken(self):
        self.call(self.a, number=1, phrase=0)
        self.now += .5
        self.call(self.a, number=2, phrase=5)
        self.assertEqual(self.reply()[:2], (1, 0))                       # still message 1: too soon
        self.now += .5
        self.call(self.a, number=2, phrase=5)
        self.assertEqual(self.reply()[:2], (2, 5))
        self.call(self.b)
        self.assertEqual([r[:3] for r in self.reply()[2]], [(10, 5, 2)])

    def test_a_restarted_client_starts_again_at_one(self):
        self.call(self.a, number=7, phrase=1)
        self.epochs[1] = b'n' * 16
        self.hub.ticket_epochs[self.a.addr] = self.epochs[1]
        self.now += .1
        self.call(self.a, number=1, phrase=3, seq=1)                      # a new session: its state starts fresh
        self.assertEqual(self.reply()[:2], (1, 3))

    def test_only_the_same_event_sees_a_message_when_the_world_has_events(self):
        self.world.same_event = lambda p, q: {p.xuid, q.xuid} <= {1, 2}
        self.call(self.a, number=1, phrase=0)
        self.call(self.b)
        self.assertEqual(len(self.reply()[2]), 1)
        self.call(self.c)
        self.assertEqual(self.reply()[2], [])

    def test_bad_requests_are_ignored(self):
        self.call(self.a, number=1, phrase=0, key=b'x' * 32)              # wrong key
        self.assertEqual(self.sent, [])
        self.call(self.a, number=1, phrase=Q.PHRASES)                    # no such phrase
        self.assertEqual(self.sent, [])
        self.call(self.a, number=1, phrase=0, reserved=1)
        self.assertEqual(self.sent, [])
        self.call(self.a, number=1, phrase=0, epoch=b'z' * 16)            # not this session's epoch
        self.assertEqual(self.sent, [])
        self.hub.revoked[1] = 1
        self.call(self.a, number=1, phrase=0)
        self.assertEqual(self.sent, [])

    def test_an_old_or_repeated_sequence_number_is_ignored(self):
        self.call(self.a, number=1, phrase=0, seq=5)
        self.now += .1
        self.call(self.a, number=2, phrase=1, seq=5)
        self.assertEqual(self.sent, [])
        self.call(self.a, number=2, phrase=1, seq=4)
        self.assertEqual(self.sent, [])

    def test_a_revoked_sender_is_left_out_of_the_replies(self):
        self.call(self.a, number=1, phrase=0)
        self.hub.revoked[1] = 1
        self.call(self.b)
        self.assertEqual(self.reply()[2], [])

    def test_switched_off_with_the_environment(self):
        with mock.patch.dict(os.environ, {"FH2MP_NO_QUICKCHAT": "1"}):
            self.call(self.a, number=1, phrase=0)
        self.assertEqual(self.sent, [])

    def test_a_long_gamertag_is_cut_on_a_character_boundary(self):
        self.hub.gamertags[1] = "é" * 40                                  # 80 bytes of UTF-8
        self.call(self.a, number=1, phrase=0)
        self.call(self.b)
        name = self.reply()[2][0][4]
        self.assertEqual(name, "é" * 24)


class ControlName(unittest.TestCase):
    """The account service's "name" line on the control port, through a real Hub (test_security.make_hub)."""
    def test_the_control_port_takes_a_gamertag_in_hex(self):
        from test_security import make_hub
        hub = make_hub()
        hub.ctl_text("name %x %s" % (0x9000000000001, "Dusk Carton".encode().hex()))
        self.assertEqual(hub.gamertags, {0x9000000000001: "Dusk Carton"})
        hub.ctl_text("name %x %s" % (0x9000000000001, "Renamed".encode().hex()))
        self.assertEqual(hub.gamertags[0x9000000000001], "Renamed")

    def test_an_unreadable_name_line_is_ignored(self):
        from test_security import make_hub
        hub = make_hub()
        hub.ctl_text("name 12 zz")
        hub.ctl_text("name 12 ff")                                       # not UTF-8
        self.assertEqual(hub.gamertags, {})

    @unittest.skipUnless(importlib.util.find_spec("psycopg"), "the account service needs psycopg (release/requirements-test.txt)")
    def test_the_account_service_sends_the_name_with_auth(self):
        import fh2mp_lsp as L
        sent = []
        L._AUTH_ANNOUNCED.clear()
        with mock.patch.object(L.xbl, "_control", sent.append):
            L.announce_auth(0x12, "1.2.3.4", "", "Dusk Carton")
        self.assertEqual(sent, ["auth 12 1.2.3.4 %s" % L.auth.udp_ticket_key("") if L.auth.udp_ticket_key("") else "auth 12 1.2.3.4",
                                "name 12 %s" % "Dusk Carton".encode().hex()])


if __name__ == "__main__":
    unittest.main()
