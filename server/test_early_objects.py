"""Objects a client sends before its handshake ends are relayed under its relay owner id, never under owner 0.

A client that reconnects inside one game process sends its creates (routers 1, 2, 3, 9, 12) right behind its OpenConnection, before it acks
the server's reply (prod 2026-11-01 23:07:06, lobby acd40e38, port 54806: five creates logged before CONNECTED; about a dozen joins a day do
this). The relay owner id is handed out with the ack, so the server relayed those creates under owner 0 and the leave's deletes under the real
id: every player already there kept that player object for the rest of the session. The HUD player list (HudPartyMembersComponent,
count at +0x1ac, 12 slots at +0x130) then read "Players 13" with 12 players connected, and every client with the list open crashed in it when
the 12th real player joined (23:09:06, game+0x144661D called from 0x78BEB5 / 0x782F59, seven clients dropped within 35 s).

Everything stays in this process (MemorySocket); nothing reaches a game."""
import os
import struct
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S  # noqa: E402
import test_race_flow as T  # noqa: E402
import test_vip as V  # noqa: E402

CREATE, UPDATE, DELETE = S.MSG_OBJECT_CREATE, S.MSG_OBJECT_UPDATE, S.MSG_OBJECT_DELETE


def wire(data):
    """The object-layer messages of a datagram as a client reads them: (message id, router, relay owner or None, seq)."""
    _ack, _remote, _bits, count, _length = struct.unpack('<HHIBH', data[:11])
    r = T.Bits(int.from_bytes(data[11:], 'little'), (len(data) - 11) * 8)
    out = []
    for _ in range(count):
        kind = r.take(7)
        if kind == S.MSG_CONNECTION_SYNC_DONE:
            r.take(32)
        elif kind == 5:                                  # TimeSync
            r.take(16), r.take(32)
        elif kind == S.MSG_FINALISE_SESSION_ENTRY:
            pass
        elif kind in (CREATE, UPDATE, DELETE):
            router = r.take(4)
            a, b = r.take(1), r.take(1)
            id_bits, type_bits = S.ROUTER_BITS[router]
            r.take(id_bits)
            owner = r.take(8) if (a, b) == (0, 1) else None
            if kind == CREATE and type_bits:
                r.take(type_bits)
            seq = r.take(8)
            if kind != DELETE:
                r.take(r.take(14))
            out.append((kind, router, owner, seq))
            continue
        else:
            break                                        # the rest is not object traffic
    return out


class EarlyObjects(unittest.TestCase):
    def setUp(self):
        self.now = 1000.0
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(S.time, 'monotonic', side_effect=lambda: self.now).start()
        mock.patch.object(S.socket, 'socket', side_effect=AssertionError('offline tests must not open sockets')).start()
        mock.patch.object(S, 'RESULT_SINK', None).start()
        self.sock = T.MemorySocket()
        self.w = S.Server(0, None, sock=self.sock)
        self.w.log = lambda *a, **k: None
        self.heard = {}

    def collect(self):
        self.w.flush(float('inf'))
        sent, self.sock.sent = self.sock.sent, []
        for data, addr in sent:
            self.heard.setdefault(addr, []).extend(wire(data))

    def hello(self, number):
        client = T.Receiver(('192.0.2.' + str(number), 5000 + number))
        msg = T.message(0).text('6CC152FD').text('Flagship').put(0, 2).put(0x9000000010000 + number, 64)
        return client, msg.put(2, 32).put(0, 32).put(7, 32)

    def own_objects(self):
        """The client's own creates in the real order of a join (routers 1, 2, 3)."""
        return [T.obj(1, V.real_create(), 0, create=True),
                T.message(7, (2, 4), (0, 1), (0, 1), (0, 7), (0, 8), (37, 14)).put(1500 << 5, 37),
                T.obj(3, T.fields({0: 0, 3: 0, 4: 0, 5: 0}, dict.fromkeys(range(7), 1), 4), create=True)]

    def send(self, client, *messages):
        self.w.handle(T.packet(messages), client.addr)
        self.w.flush(float('inf'))
        sent, self.sock.sent = self.sock.sent, []
        for data, addr in sent:
            if addr == client.addr and client.counter is None:
                client.receive(data)                     # only the server's counter from its OpenConnectionResponse is needed
            self.heard.setdefault(addr, []).extend(wire(data))

    def ack(self, client):
        return T.message(1, (0, 2), (6, 3), (client.counter, 32), (7, 32))

    def join(self, number, early=False):
        """A client joins; with early=True its creates come right behind the OpenConnection, before its ack (a reconnect in one process)."""
        client, hello = self.hello(number)
        if early:
            self.send(client, hello, *self.own_objects())
            self.send(client, self.ack(client))
        else:
            self.send(client, hello)
            self.send(client, self.ack(client), *self.own_objects())
        self.now += 0.33
        self.send(client, T.message(34))
        return client

    def leave(self, client):
        self.send(client, T.message(S.MSG_CLOSE_CONNECTION, (3, 3), (1, 32), (2, 32)))

    def live_objects(self, client):
        """What the client still holds: relayed (owner, router) objects created and not deleted since."""
        held = set()
        for kind, router, owner, _seq in self.heard.get(client.addr, []):
            if owner is None:
                continue
            if kind == CREATE:
                held.add((owner, router))
            elif kind == DELETE:
                held.discard((owner, router))
        return held

    def test_early_creates_are_relayed_under_the_owner_id(self):
        a = self.join(1)
        self.heard.clear()
        b = self.join(2, early=True)
        owner = self.w.peers[b.addr].owner_id
        self.assertGreater(owner, 0)
        got = self.heard.get(a.addr, [])
        self.assertFalse([m for m in got if m[2] == 0], "objects relayed under relay owner 0")
        self.assertEqual(sorted(m[1] for m in got if m[0] == CREATE and m[2] == owner), [1, 2, 3])
        # the stream of each object starts at 0 for the player already there, without a gap
        for router in (1, 2, 3):
            seqs = [m[3] for m in got if m[1] == router and m[2] == owner]
            self.assertEqual(seqs, list(range(len(seqs))), f"router {router}")

    def test_an_early_joiner_who_leaves_leaves_nothing_behind(self):
        a = self.join(1)
        b = self.join(2, early=True)
        self.leave(b)
        self.assertEqual(self.live_objects(a), set())

    def test_nothing_is_relayed_before_the_owner_id_exists(self):
        a = self.join(1)
        self.heard.clear()
        b, hello = self.hello(2)
        self.send(b, hello, *self.own_objects())
        self.assertEqual([m for m in self.heard.get(a.addr, []) if m[0] in (CREATE, UPDATE)], [])
        self.assertFalse(self.w.peers[b.addr].connected)

    def test_a_handshake_that_never_ends_sends_no_deletes(self):
        a = self.join(1)
        self.heard.clear()
        b, hello = self.hello(2)
        self.send(b, hello, *self.own_objects())
        self.w.drop_peer(self.w.peers[b.addr], "test: handshake never finished")
        self.collect()
        self.assertEqual(self.heard.get(a.addr, []), [])

    def test_twelve_players_after_early_joins_are_twelve_on_every_client(self):
        clients = [self.join(1)]
        for i in range(2, 13):
            clients.append(self.join(i, early=i % 3 == 0))
        for i in (3, 6):                                    # two early joiners leave, two others take their seats
            self.leave(clients[i - 1])
        clients = [c for i, c in enumerate(clients, 1) if i not in (3, 6)]
        self.now += S.OWNER_QUARANTINE_S + 1
        clients += [self.join(20, early=True), self.join(21)]
        self.assertEqual(sum(1 for p in self.w.peers.values() if p.connected), 12)
        for c in clients:
            players = {o for o, r in self.live_objects(c) if r == 1}
            self.assertEqual(len(players), 11, f"{c.addr}: player objects of {sorted(players)}")


if __name__ == "__main__":
    unittest.main()
