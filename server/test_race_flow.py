"""Offline protocol contracts: handshake -> invite -> selection -> grid -> GO.

No game, network socket, production encoder/decoder, or seeded Peer/event state.
MemorySocket receives the real server's flushed datagrams. The small independent
wire reader below checks the documented 1.0.0.64 layouts (docs/protocol.md), not
Server.game_options / gopts_view. This checks delivery contracts, not the game's UI.
"""
import os
import struct
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S


class Bits:
    def __init__(self, value=0, size=0):
        self.value, self.size, self.pos = value, size, 0

    def put(self, value, width):
        assert 0 <= value < 1 << width
        self.value |= value << self.size
        self.size += width
        return self

    def take(self, width):
        assert self.pos + width <= self.size, 'truncated server output'
        value = self.value >> self.pos & ((1 << width) - 1)
        self.pos += width
        return value

    def text(self, value):
        data = value.encode('ascii')
        assert len(data) < 64
        self.put(0, 2).put(len(data), 6)
        for b in data:
            self.put(b, 8)
        return self


def message(kind, *fields):
    out = Bits().put(kind, 7)
    for value, width in fields:
        out.put(value, width)
    return out


def packet(messages):
    body = Bits()
    for m in messages:
        body.put(m.value, m.size)
    size = 11 + (body.size + 7) // 8
    return struct.pack('<HHIBH', 65535, 0, 0, len(messages), size) + body.value.to_bytes(size - 11, 'little')


# Only the fields used in these scenario fixtures. An unexpected field fails
# the test instead of deriving its width from the production implementation.
OPTIONS = {4: 32, 6: 8, 14: 8, 30: 8, 31: 16, 32: 16, 418: 3, 419: 32}
PLAYER = {27: 32, **dict.fromkeys(range(81, 88), 32), 90: 1, 103: 32}


def fields(values, widths, id_bits):
    out = Bits()
    for vid, value in sorted(values.items()):
        out.put(vid, id_bits).put(value, widths[vid])
    return out


def decode_fields(body, widths, id_bits):
    out, previous = {}, -1
    while body.pos < body.size:
        vid = body.take(id_bits)
        assert vid > previous, 'unordered server variables'
        assert vid in widths, f'unexpected server variable {vid}'
        out[vid] = body.take(widths[vid])
        previous = vid
    return out


def obj(router, body, seq=0, create=False, transfer=False):
    out = message(7 if create else 8, (router, 4), (int(transfer), 1), (0, 1), (0, 2 if router == 6 else 7))
    if create and router == 6:
        out.put(1 if transfer else 0, 2)
    return out.put(seq, 8).put(body.size, 14).put(body.value, body.size)


def float_bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


class MemorySocket:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((data, addr))


class Receiver:
    """Observable wire state; never reads the server's Peer or event objects."""
    def __init__(self, addr):
        self.addr = addr
        self.options, self.console, self.session = {}, {}, {}
        self.last_options_seq = None
        self.options_seq = 0
        self.player_seq = self.console_seq = 0
        self.events = []
        self.responses, self.grids, self.starts = [], [], []
        self.counter = None

    def receive(self, data):
        _ack, _remote, _bits, count, length = struct.unpack('<HHIBH', data[:11])
        assert length == len(data), 'bad datagram length'
        r = Bits(int.from_bytes(data[11:], 'little'), (len(data) - 11) * 8)
        for _ in range(count):
            kind = r.take(7)
            if kind in (7, 8, 11):
                router = r.take(4)
                a, b = r.take(1), r.take(1)
                oid = r.take({1: 7, 3: 7, 6: 2, 10: 1}[router])
                owner = r.take(8) if (a, b) == (0, 1) else None
                if kind == 7 and router == 6:
                    r.take(2)
                seq, size = r.take(8), r.take(14)
                body = Bits(r.take(size), size)
                if router == 6 and (a, b, oid) == (1, 0, 0):
                    if self.last_options_seq is not None:
                        gap = (seq - self.last_options_seq) % 255
                        assert 0 < gap < 128, f'stale GameOptions seq {seq} after {self.last_options_seq}'
                    self.last_options_seq = seq
                    self.options.update(decode_fields(body, OPTIONS, 10))
                    self.events.append(('options', dict(self.options)))
                    if kind == 11:
                        self.options_seq = 0  # observed native writer reset after grant
                elif router == 6 and (a, b, oid) == (0, 0, 0) and kind != 7:
                    self.session.update(decode_fields(body, {4: 1, 5: 1, 6: 1}, 4))
                    self.events.append(('session', dict(self.session)))
                elif router == 3 and (a, b, oid) == (1, 1, 0):
                    self.console.update(decode_fields(body, dict.fromkeys(range(7), 1), 4))
                    self.events.append(('console', dict(self.console)))
                # Relay player/console objects, Weather and initial SessionHost
                # are length-delimited; their contents aren't this contract.
            elif kind == 1:
                r.take(2)
                assert r.take(3) == 0, 'handshake rejected'
                r.take(32)
                self.counter = r.take(32)
            elif kind == 38:
                self.events.append(('invite', dict(self.options)))
            elif kind == 39:
                assert r.take(4) == 0
                grid = []
                for _ in range(r.take(4)):
                    a, b, cid = r.take(1), r.take(1), r.take(7)
                    grid.append((a, b, cid, r.take(8) if (a, b) == (0, 1) else None))
                self.grids.append(grid)
                self.events.append(('grid', grid))
            elif kind == 40:
                assert r.take(4) == 0
                self.starts.append(r.take(32))
                self.events.append(('go', self.starts[-1]))
            elif kind == 42:
                assert r.take(4) == 0
                self.responses.append(r.take(2))
            elif kind == 44:
                a, b = r.take(1), r.take(1)
                r.take(7)
                if (a, b) == (0, 1):
                    r.take(8)
                for _ in range(3):
                    width_class = r.take(2)
                    length = r.take(width_class * 8 + 6)
                    r.take(length * 8)
                r.take(34)
            elif kind in (3, 6):
                r.take(32)
            elif kind == 4:
                r.take(64)
            elif kind == 5:
                r.take(48)
            elif kind == 34:
                pass
            elif kind == 54:
                r.take(384)
            elif kind == 72:
                r.take(4 + 8 + 14 + 5 + 8 + 1 + 32)
            else:
                raise AssertionError(f'unexpected outbound message {kind}')
        assert r.size - r.pos < 8, 'unconsumed server output'
        assert r.take(r.size - r.pos) == 0, 'nonzero datagram padding'


class RaceFlow(unittest.TestCase):
    def setUp(self):
        self.now = 1000.0
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(S.time, 'monotonic', side_effect=lambda: self.now).start()
        mock.patch.object(S.socket, 'socket', side_effect=AssertionError('offline tests must not open sockets')).start()
        mock.patch.object(S, 'RESULT_SINK', None).start()
        self.sock = MemorySocket()
        self.w = S.Server(0, None, sock=self.sock)
        self.logs = []
        self.w.log = self.logs.append
        self.clients = {}
        self.a, self.b = self.join(1), self.join(2)

    def drain(self):
        self.w.flush(float('inf'))
        sent, self.sock.sent = self.sock.sent, []
        for data, addr in sent:
            self.clients[addr].receive(data)

    def send(self, client, *messages):
        self.w.handle(packet(messages), client.addr)
        self.drain()

    def join(self, number):
        client = Receiver(('192.0.2.' + str(number), 5000 + number))
        self.clients[client.addr] = client
        hello = message(0).text('6CC152FD').text('Flagship').put(0, 2).put(0x9000000010000 + number, 64)
        hello.put(2, 32).put(0, 32).put(7, 32)
        self.send(client, hello)
        self.assertIsNotNone(client.counter)
        self.send(client, message(1, (0, 2), (6, 3), (client.counter, 32), (7, 32)))
        self.send(client, obj(3, fields({0: 0, 3: 0, 4: 0, 5: 0}, dict.fromkeys(range(7), 1), 4), create=True),
                  message(34))
        return client

    def advance(self, seconds):
        # Keep connections alive through ordinary pings; drive real tick logic.
        end = self.now + seconds
        while self.now < end:
            self.now = min(end, self.now + 0.5)
            for client in self.clients.values():
                self.send(client, message(3, (int(self.now * 1000), 32)))
            self.w.tick()
            self.drain()

    def grant(self, client):
        self.send(client, message(10, (6, 4), (1, 2), (1, 1), (0, 1), (0, 2)))

    def options(self, client, values):
        body = fields(values, OPTIONS, 10)
        self.send(client, obj(6, body, client.options_seq, transfer=True))
        client.options_seq = (client.options_seq + 1) % 255

    def car(self, client, cls, pi, extra=None, full=True):
        values = {81: cls, 82: float_bits(pi), **(extra or {})}
        if full:
            values.update({v: float_bits(.5) for v in range(83, 88)})
        self.send(client, obj(1, fields(values, PLAYER, 8), client.player_seq))
        client.player_seq = (client.player_seq + 1) % 255

    def console(self, client, values):
        self.send(client, obj(3, fields(values, dict.fromkeys(range(7), 1), 4), client.console_seq))
        client.console_seq = (client.console_seq + 1) % 255

    def ready(self, client):
        self.send(client, message(41, (0, 4), (1, 1)))
        self.assertTrue(client.responses, 'no readiness response delivered')
        return client.responses[-1]

    def request(self, client):
        self.send(client, message(38))

    def select(self, cls=3, pi=.750):
        self.grant(self.a)
        self.options(self.a, {4: 1001, 14: 1, 30: cls, 31: 0, 32: 0, 418: 1, 419: 1})
        self.car(self.a, cls, pi)
        self.car(self.b, cls, pi)
        self.assertEqual(self.ready(self.a), 0)
        self.request(self.a)
        self.assertEqual(self.ready(self.b), 0)
        self.advance(2)
        for client in (self.a, self.b):
            self.assertEqual(client.console.get(2), 1, 'eligible client never selected')

    def go(self):
        grids_before = [len(c.grids) for c in (self.a, self.b)]
        starts_before = [len(c.starts) for c in (self.a, self.b)]
        for client in (self.a, self.b):
            self.console(client, {5: 1})
        for client, count in zip((self.a, self.b), grids_before):
            self.assertEqual(len(client.grids), count + 1, 'starting grid never delivered')
            self.assertEqual(len(client.grids[-1]), 2)
            self.assertEqual(sum(key[:2] == (0, 0) for key in client.grids[-1]), 1)
        for client in (self.a, self.b):
            self.console(client, {0: 1, 6: 1})
        self.assertEqual(len(self.a.starts), len(self.b.starts))
        for client, count in zip((self.a, self.b), starts_before):
            self.assertEqual(len(client.starts), count + 1, 'GO never delivered')
        self.assertEqual(self.a.starts[-1], self.b.starts[-1])

    def finish(self):
        for client in (self.a, self.b):
            score = message(44, (0, 2), (0, 7))
            for seconds in (60., 60., 60.):
                score.put(0, 2).put(8, 6).put(int.from_bytes(struct.pack('<d', seconds), 'little'), 64)
            self.send(client, score.put(1, 32).put(0, 2))
        self.advance(.5)
        for client in (self.a, self.b):
            self.assertEqual(client.session.get(6), 0, 'race never ended')
            self.console(client, {3: 0, 4: 0, 5: 0, 6: 0})
            self.send(client, message(41, (0, 4), (0, 1)))
        self.advance(5)

    def test_alternating_hosts_can_finish_three_different_class_events(self):
        self.select()
        self.go()
        self.finish()
        for host, guest, cls, event in ((self.b, self.a, 4, 1002), (self.a, self.b, 5, 1003)):
            if host is self.b:
                self.grant(host)
            # On the final round a former owner writes with no new grant.
            self.options(host, {4: event, 30: cls})
            pi = .850 if cls == 4 else .950
            self.car(host, cls, pi)
            self.car(guest, cls, pi)
            self.request(host)
            invites = [v for kind, v in guest.events if kind == 'invite']
            self.assertEqual((invites[-1].get(4), invites[-1].get(30)), (event, cls))
            self.assertEqual(self.ready(guest), 0)
            self.advance(2)
            self.go()
            self.finish()
        self.assertEqual((len(self.a.starts), len(self.b.starts)), (3, 3))

    def test_invite_carries_host_class_and_both_clients_reach_go(self):
        self.select()
        invites = [v for kind, v in self.b.events if kind == 'invite']
        self.assertEqual(invites[-1].get(30), 3, 'guest invited with wrong class')
        self.go()

    def test_wrong_class_is_refused_then_a_valid_selection_can_join(self):
        self.grant(self.a)
        self.options(self.a, {4: 1001, 30: 3})
        self.car(self.a, 3, .750)
        self.request(self.a)
        for cls, pi in ((4, .850), (5, .950), (6, .999)):
            self.car(self.b, cls, pi)
            self.assertNotEqual(self.ready(self.b), 0, f'class {cls} admitted to A')
        self.car(self.b, 3, .750)
        self.assertEqual(self.ready(self.b), 0)
        self.advance(2)
        self.go()

    def test_car_choice_in_a_mixed_delta_is_used_for_admission(self):
        self.grant(self.a)
        self.options(self.a, {4: 1001, 30: 3})
        self.car(self.a, 3, .750)
        self.car(self.b, 4, .850)
        self.request(self.a)
        self.assertNotEqual(self.ready(self.b), 0)
        # Native variables are independently dirty: a car pick needn't carry
        # all five ratings. XP + class + PI is a legal typed player delta.
        self.car(self.b, 3, .750, extra={27: float_bits(15000)}, full=False)
        self.assertEqual(self.ready(self.b), 0, 'valid sparse car selection was ignored')
        self.advance(2)
        self.go()

    def test_illegal_mixed_car_change_during_loading_cancels_before_grid(self):
        self.select()
        self.car(self.b, 4, .850, extra={90: 0}, full=False)
        self.console(self.a, {5: 1})
        self.console(self.b, {5: 1})
        self.assertFalse(self.a.grids or self.b.grids, 'wrong-class car reached the grid')
        self.assertFalse(self.a.starts or self.b.starts)
        for client in (self.a, self.b):
            self.assertEqual(client.console.get(2), 0, 'cancel did not clear participation')

    def test_illegal_car_change_on_grid_prevents_go(self):
        self.select()
        for client in (self.a, self.b):
            self.console(client, {5: 1})
        self.assertEqual(len(self.a.grids), 1)
        self.car(self.b, 4, .850, extra={90: 0}, full=False)
        for client in (self.a, self.b):
            self.console(client, {0: 1, 6: 1})
        self.assertFalse(self.a.starts or self.b.starts, 'wrong-class car received GO')
        self.assertEqual(self.b.console.get(2), 0)

    def test_sparse_options_preserve_class_and_pi_on_the_wire(self):
        self.grant(self.a)
        self.options(self.a, {4: 1001, 30: 3, 31: 710, 32: 780})
        self.options(self.a, {6: 2})
        self.request(self.a)
        invite = [v for kind, v in self.b.events if kind == 'invite'][-1]
        self.assertEqual(tuple(invite.get(v) for v in (30, 31, 32)), (3, 710, 780))
        self.car(self.b, 3, .705)
        self.assertNotEqual(self.ready(self.b), 0)
        # PI can also be dirty on its own, alongside a team variable.
        self.send(self.b, obj(1, fields({82: float_bits(.750), 90: 0}, PLAYER, 8), self.b.player_seq))
        self.b.player_seq += 1
        self.assertEqual(self.ready(self.b), 0)

    def test_returning_host_snapshot_is_delivered_before_invite(self):
        self.grant(self.a)
        self.options(self.a, {4: 1001, 30: 3})
        self.grant(self.b)
        self.options(self.b, {4: 1002, 30: 4})
        # Production ordering: former owner writes without a new grant.
        self.options(self.a, {4: 1003, 30: 5})
        self.car(self.a, 5, .950)
        self.car(self.b, 5, .950)
        self.request(self.a)
        invites = [v for kind, v in self.b.events if kind == 'invite']
        self.assertEqual((invites[-1].get(4), invites[-1].get(30)), (1003, 5))
        self.assertEqual(self.ready(self.b), 0)
        self.advance(2)
        self.go()

    def test_returning_host_can_wait_in_setup_before_requesting_race(self):
        self.grant(self.a)
        self.options(self.a, {4: 1001, 30: 3})
        self.grant(self.b)
        self.options(self.b, {4: 1002, 30: 4})
        self.options(self.a, {4: 1003, 30: 5})
        # Selecting settings and pressing Start are separate user actions.
        # Thinking for a few seconds must not silently select the old class.
        self.advance(8)
        self.car(self.a, 5, .950)
        self.car(self.b, 5, .950)
        self.request(self.a)
        invite = [v for kind, v in self.b.events if kind == 'invite'][-1]
        self.assertEqual((invite.get(4), invite.get(30)), (1003, 5))
        self.assertEqual(self.ready(self.b), 0)
        self.advance(2)
        self.go()

    def test_long_lobby_and_late_join_keep_transfer_sequences_and_class(self):
        self.grant(self.a)
        self.options(self.a, {4: 1001, 30: 3})
        for i in range(280):
            self.options(self.a, {6: i % 4})
        c = self.join(3)
        self.assertEqual(c.options.get(30), 3)
        self.assertEqual(c.options.get(4), 1001)
        self.car(c, 4, .850)
        self.assertNotEqual(self.ready(c), 0)
        self.car(c, 3, .750)
        self.assertEqual(self.ready(c), 0)

    def test_late_joiner_gets_class_and_pi_before_accepting(self):
        self.grant(self.a)
        self.options(self.a, {4: 1001, 30: 3, 31: 710, 32: 780})
        c = self.join(3)
        self.assertEqual(tuple(c.options[v] for v in (30, 31, 32)), (3, 710, 780))
        self.car(c, 3, .705)
        self.assertNotEqual(self.ready(c), 0)
        self.car(c, 3, .750)
        self.assertEqual(self.ready(c), 0)


if __name__ == '__main__':
    unittest.main()
