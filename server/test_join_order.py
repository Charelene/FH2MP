"""When a Free Roam joiner is shown the players already there (Server.introduce, FH2MP_INTRODUCE_AT).

The joins follow what real clients do (dev log 2026-10-30 00:22:41): the ACK of the connect comes with the client's own creates on routers 1, 2, 3,
9 and 12, FinaliseSessionEntry about 330 ms later. The player create is a real capture (test_vip). Everything stays in this process: MemorySocket
keeps the server's datagrams and nothing reaches a game. What the game does with the order is for a hand test; this checks what the server sends."""
import os
import struct
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S
import test_race_flow as T
import test_vip as V

CREATE, UPDATE, DELETE = S.MSG_OBJECT_CREATE, S.MSG_OBJECT_UPDATE, S.MSG_OBJECT_DELETE
GAME_OPTIONS_BODY = T.fields({418: 1, 419: 3}, T.OPTIONS, 10)


def wire(data):
    """Every object-layer message of a datagram, as a client reads it: (message id, router, relay owner or None, seq), in order."""
    _ack, _remote, _bits, count, length = struct.unpack('<HHIBH', data[:11])
    r = T.Bits(int.from_bytes(data[11:], 'little'), (len(data) - 11) * 8)
    out = []
    for _ in range(count):
        kind = r.take(7)
        if kind == S.MSG_CONNECTION_SYNC_DONE:
            r.take(32)
            out.append((kind, None, None, None))
        elif kind == 5:                                  # TimeSync
            r.take(16), r.take(32)
            out.append((kind, None, None, None))
        elif kind == S.MSG_FINALISE_SESSION_ENTRY:       # the echo, no payload
            out.append((kind, None, None, None))
        elif kind in (CREATE, UPDATE, DELETE):
            router = r.take(4)
            a, b = r.take(1), r.take(1)
            id_bits, type_bits = {**S.ROUTER_BITS}[router]
            r.take(id_bits)
            owner = r.take(8) if (a, b) == (0, 1) else None
            if kind == CREATE and type_bits:
                r.take(type_bits)
            seq = r.take(8)
            if kind != DELETE:
                r.take(r.take(14))
            out.append((kind, router, owner, seq))
        else:
            break                                        # the rest of this datagram is not object traffic
    return out


class JoinOrder(unittest.TestCase):
    drain, send = T.RaceFlow.drain, T.RaceFlow.send
    join = T.RaceFlow.join

    def setUp(self):
        self.point = "finalise"
        self.start_world()

    def start_world(self, **kind):
        self.now = 1000.0
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(S.time, 'monotonic', side_effect=lambda: self.now).start()
        mock.patch.object(S.socket, 'socket', side_effect=AssertionError('offline tests must not open sockets')).start()
        mock.patch.object(S, 'RESULT_SINK', None).start()
        self.sock = T.MemorySocket()
        with mock.patch.dict(os.environ, {"FH2MP_INTRODUCE_AT": self.point}):
            self.w = S.Server(0, None, sock=self.sock, **kind)
        self.logs = []
        self.w.log = self.logs.append
        self.clients, self.heard = {}, {}

    def use(self, point):
        self.point = point
        self.start_world()
        self.a = self.present(1)

    def collect(self):
        """Flushes the server and sorts its datagrams by client."""
        self.w.flush(float('inf'))
        sent, self.sock.sent = self.sock.sent, []
        for data, addr in sent:
            self.heard.setdefault(addr, []).extend(wire(data))

    def ask_for_connection(self, number, lobby=2):
        client = T.Receiver(('192.0.2.' + str(number), 5000 + number))
        self.clients[client.addr] = client
        hello = T.message(0).text('6CC152FD').text('Flagship').put(0, 2).put(0x9000000010000 + number, 64)
        hello.put(lobby, 32).put(0, 32).put(7, 32)
        self.w.handle(T.packet([hello]), client.addr)
        self.w.flush(float('inf'))
        sent, self.sock.sent = self.sock.sent, []
        for data, _addr in sent:
            client.receive(data)
        self.assertIsNotNone(client.counter)
        return client

    def ack(self, client):
        return T.message(1, (0, 2), (6, 3), (client.counter, 32), (7, 32))

    def console(self):
        return T.obj(3, T.fields({0: 0, 3: 0, 4: 0, 5: 0}, dict.fromkeys(range(7), 1), 4), create=True)

    def own_objects(self, with_console=True):
        """The client's creates that follow the ACK, in the real order (routers 1, 2, 3, 9, 12)."""
        msgs = [T.obj(1, V.real_create(), 0, create=True),
                T.message(7, (2, 4), (0, 1), (0, 1), (0, 7), (0, 8), (37, 14)).put(1500 << 5, 37)]
        if with_console:
            msgs.append(self.console())
        return msgs

    def datagram(self, client, *messages):
        self.w.handle(T.packet(messages), client.addr)
        self.collect()

    def finalise(self, client):
        self.now += 0.33
        self.datagram(client, T.message(34))

    def present(self, number):
        """A player already in the world, with its objects stored on the server."""
        client = self.ask_for_connection(number)
        self.datagram(client, self.ack(client), *self.own_objects())
        self.finalise(client)
        self.heard.pop(client.addr, None)
        return client

    def of(self, client, owner):
        """What the client has been told about that relay owner's objects."""
        return [m for m in self.heard.get(client.addr, []) if m[2] == owner]

    def creates(self, client, owner):
        return [m[1] for m in self.of(client, owner) if m[0] == CREATE]

    def peer(self, client):
        return self.w.peers[client.addr]

    def test_finalise_shows_nobody_before_finalise(self):
        self.use("finalise")
        late = self.ask_for_connection(2)
        self.datagram(late, self.ack(late), *self.own_objects())
        self.assertEqual(self.of(late, 1), [])
        self.assertFalse(self.peer(late).introduced)
        self.finalise(late)
        self.assertEqual(self.creates(late, 1), [1, 2, 3])
        self.assertTrue(self.peer(late).introduced)

    def test_finalise_gives_a_joiner_nothing_live_before_finalise(self):
        self.use("finalise")
        late = self.ask_for_connection(2)
        self.datagram(late, self.ack(late), *self.own_objects())
        self.datagram(self.a, T.obj(1, T.Bits().put(19, 8).put(2, 4), 1))
        self.assertEqual(self.of(late, 1), [])

    def test_the_default_needs_no_switch(self):
        with mock.patch.dict(os.environ):
            os.environ.pop("FH2MP_INTRODUCE_AT", None)
            self.assertEqual(S.Server(0, None, sock=T.MemorySocket()).introduce_at, "handshake")

    def test_handshake_puts_the_players_right_after_the_connect_replies(self):
        self.use("handshake")
        late = self.ask_for_connection(2)
        self.datagram(late, self.ack(late), *self.own_objects())
        told = self.heard[late.addr]
        kinds = [m[0] for m in told]
        first_relayed = next(i for i, m in enumerate(told) if m[2] == 1)
        self.assertIn(S.MSG_CONNECTION_SYNC_DONE, kinds[:first_relayed])
        self.assertIn((CREATE, S.ROUTER_LOBBY), [m[:2] for m in told[:first_relayed]])     # SessionHost
        self.assertIn((CREATE, S.ROUTER_WEATHER), [m[:2] for m in told[:first_relayed]])
        self.assertEqual(self.creates(late, 1), [1, 3])                  # the car waits for its FinaliseSessionEntry
        self.assertFalse(self.peer(late).entered)

    def test_the_cars_follow_at_finalise_after_starting_their_load_flow(self):
        """the car wait (0x7cbf10) must still find a car missing when Started comes, or "Syncing <name>" never shows"""
        self.use("handshake")
        late = self.ask_for_connection(2)
        self.datagram(late, self.ack(late), *self.own_objects())
        self.datagram(self.a, T.obj(2, T.Bits().put(0, 8), 1))           # a live car update in the window: not sent yet
        self.assertEqual([m for m in self.of(late, 1) if m[1] == S.ROUTER_CAR], [])
        self.finalise(late)
        self.assertEqual([m[0] for m in self.of(late, 1) if m[1] == S.ROUTER_CAR][:1], [CREATE])
        numbers = [m[3] for m in self.of(late, 1) if m[1] == S.ROUTER_CAR]
        self.assertEqual(numbers, list(range(len(numbers))), numbers)
        self.datagram(self.a, T.obj(2, T.Bits().put(0, 8), 2))           # and live from then on, in order
        numbers = [m[3] for m in self.of(late, 1) if m[1] == S.ROUTER_CAR]
        self.assertEqual(numbers, list(range(len(numbers))), numbers)
        self.assertTrue([x for x in self.logs if "given the cars" in x and str(late.addr[1]) in x])

    def test_handshake_does_not_repeat_the_players_on_finalise(self):
        self.use("handshake")
        late = self.ask_for_connection(2)
        self.datagram(late, self.ack(late), *self.own_objects())
        told = len(self.of(late, 1))
        self.finalise(late)
        self.assertEqual(len([m for m in self.of(late, 1) if m[1] != S.ROUTER_CAR]), told)
        self.assertEqual(self.creates(late, 1), [1, 3, 2])
        self.assertTrue(self.peer(late).entered)
        self.assertEqual([x for x in self.logs if "introduced (" in x and str(late.addr[1]) in x][-1:],
                         [x for x in self.logs if "introduced (handshake)" in x and str(late.addr[1]) in x])

    def test_console_waits_for_the_clients_console(self):
        self.use("console")
        late = self.ask_for_connection(2)
        self.datagram(late, self.ack(late), *self.own_objects(with_console=False))
        self.assertEqual(self.of(late, 1), [])
        self.now += 0.1
        self.datagram(late, self.console())
        self.assertEqual(self.creates(late, 1), [1, 3])

    def test_console_in_the_ack_datagram_goes_out_after_the_replies(self):
        self.use("console")
        late = self.ask_for_connection(2)
        self.datagram(late, self.ack(late), *self.own_objects())
        told = self.heard[late.addr]
        first_relayed = next(i for i, m in enumerate(told) if m[2] == 1)
        self.assertIn(S.MSG_CONNECTION_SYNC_DONE, [m[0] for m in told[:first_relayed]])

    def test_finalise_is_the_fallback_when_the_console_never_came(self):
        self.use("console")
        late = self.ask_for_connection(2)
        self.datagram(late, self.ack(late), *self.own_objects(with_console=False))
        self.finalise(late)
        self.assertEqual(self.creates(late, 1), [1, 2, 3])
        self.assertTrue([x for x in self.logs if "introduced (finalise)" in x and str(late.addr[1]) in x])

    def test_live_updates_in_the_window_leave_no_gap_for_the_joiner(self):
        for point in ("handshake", "console"):
            with self.subTest(point):
                self.use(point)
                update = lambda i: T.obj(1, T.Bits().put(19, 8).put(i % 5, 4), i)
                for i in range(1, 6):                              # a history, so the replay is renumbered
                    self.datagram(self.a, update(i))
                late = self.ask_for_connection(2)
                self.datagram(late, self.ack(late), *self.own_objects())
                self.datagram(self.a, update(6))                   # before its FinaliseSessionEntry
                self.finalise(late)
                self.datagram(self.a, update(7))                   # after it
                numbers = [m[3] for m in self.of(late, 1) if m[1] == 1]
                self.assertEqual(numbers, list(range(len(numbers))), numbers)

    def test_introduced_is_not_entered(self):
        self.use("handshake")
        late = self.ask_for_connection(2)
        self.datagram(late, self.ack(late), *self.own_objects())
        joiner = self.peer(late)
        self.assertEqual(self.w.others(self.peer(self.a)), [])          # invites, ready checks, load checks, lighting
        self.assertEqual(self.w.audience(self.peer(self.a)), [joiner])

    def test_game_options_and_traffic_still_wait_for_finalise(self):
        self.use("handshake")
        self.w.game_options = (GAME_OPTIONS_BODY.size, GAME_OPTIONS_BODY.value)
        joined = []
        self.w.traffic.join = joined.append
        late = self.ask_for_connection(2)
        self.datagram(late, self.ack(late), *self.own_objects())
        self.assertEqual(joined, [])
        self.assertEqual(self.peer(late).gopts_seq, 0)
        self.finalise(late)
        self.assertEqual(joined, [self.peer(late)])
        self.assertEqual(self.peer(late).gopts_seq, 1)

    def test_a_player_who_leaves_in_the_window_is_deleted_for_the_joiner(self):
        self.use("handshake")
        late = self.ask_for_connection(2)
        self.datagram(late, self.ack(late), *self.own_objects())
        self.w.drop_peer(self.peer(self.a), "CloseConnection")
        self.collect()
        self.assertEqual(sorted(m[1] for m in self.of(late, 1) if m[0] == DELETE), [1, 3])      # never given the car: no delete of it
        self.assertEqual([k for k in self.peer(late).seq_shift if k[0] == 1], [])

    def test_two_joiners_each_get_the_other_once(self):
        self.use("handshake")
        first = self.ask_for_connection(2)
        self.datagram(first, self.ack(first), *self.own_objects())
        second = self.ask_for_connection(3)
        self.datagram(second, self.ack(second), *self.own_objects())
        self.finalise(first)
        self.finalise(second)
        self.assertEqual(self.creates(first, self.peer(second).owner_id), [1, 3, 2])
        self.assertEqual(self.creates(second, self.peer(first).owner_id), [1, 3, 2])
        self.assertEqual(self.creates(second, 1), [1, 3, 2])

    def test_the_other_session_types_ignore_the_switch(self):
        for kind, lobby in (({"roadtrip": True}, 1), ({"carmeet": True}, 3)):
            with self.subTest(kind):
                self.point = "handshake"
                self.start_world(**kind)
                self.a = self.ask_for_connection(1, lobby)
                self.datagram(self.a, self.ack(self.a), *self.own_objects())
                self.finalise(self.a)
                late = self.ask_for_connection(2, lobby)
                self.datagram(late, self.ack(late), *self.own_objects())
                self.assertEqual(self.of(late, 1), [])
                self.finalise(late)
                self.assertEqual(self.creates(late, 1), [1, 2, 3])

    def test_a_bad_value_stops_the_server(self):
        with mock.patch.dict(os.environ, {"FH2MP_INTRODUCE_AT": "soon"}):
            with self.assertRaises(SystemExit):
                S.Server(0, None, sock=T.MemorySocket())


if __name__ == "__main__":
    unittest.main()
