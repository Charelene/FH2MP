"""The TCP carrier (tunnel.py): framing, limits, and a client that reaches the game server through it."""
import os
import socket
import struct
import sys
import threading
import time
import unittest

os.environ.setdefault("FH2MP_CONTROL_PORT", "0")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S  # noqa: E402
import tunnel as T  # noqa: E402


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def frame(data):
    return struct.pack(">H", len(data)) + data


def open_connection(xuid, lobby=2):
    def write(w):
        w.bits(S.MSG_OPEN_CONNECTION, 7)
        w.string(S.VERSION_STRING)
        w.string(S.GAME_STRING)
        w.bits(0, 2)
        w.bits(xuid, 64)
        w.bits(lobby, 32)
        w.bits(0, 32)
        w.bits(1, 32)
    return S.build_packet([write])


def wait(cond, seconds=3.0):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.01)
    return False


class Framing(unittest.TestCase):
    def setUp(self):
        self.t = T.Tunnel(0)
        self.addCleanup(self.t.close)
        self.c = socket.create_connection(("127.0.0.1", self.t.port))
        self.addCleanup(self.c.close)
        wait(lambda: (self.t.accept(), bool(self.t.conns))[1])
        self.conn = next(iter(self.t.conns.values()))

    def read(self):
        wait(lambda: select_readable(self.conn.sock))
        return self.t.read(self.conn)

    def test_frames_are_split_and_joined(self):
        blob = frame(b"abc") + frame(b"") + frame(b"defgh")
        self.c.sendall(blob[:4])
        self.assertEqual(self.read(), [])                      # "abc" is still missing a byte
        self.c.sendall(blob[4:])
        self.assertEqual(self.read(), [b"abc", b"defgh"])      # the empty frame is only a keep-alive

    def test_an_oversize_frame_closes_the_connection(self):
        self.c.sendall(struct.pack(">H", T.MAX_FRAME + 1))
        self.read()
        self.assertTrue(self.conn.dead)

    def test_replies_come_back_framed(self):
        self.assertTrue(self.t.send(b"hello", self.conn.addr))
        self.c.settimeout(2)
        self.assertEqual(self.c.recv(64), frame(b"hello"))

    def test_a_slow_reader_gets_datagrams_dropped_not_queued(self):
        sent = 0
        for _ in range(4000):
            if not self.t.send(b"x" * 1200, self.conn.addr):
                break
            sent += 1
        self.assertLess(sent, 4000)
        self.assertLessEqual(len(self.conn.wbuf), T.SEND_BACKLOG + 1202)

    def test_at_most_three_connections_per_address(self):
        extra = [socket.create_connection(("127.0.0.1", self.t.port)) for _ in range(4)]
        self.addCleanup(lambda: [e.close() for e in extra])
        time.sleep(0.2)
        self.t.accept()
        self.assertEqual(len(self.t.conns), T.PER_IP)

    def test_silent_connections_are_closed(self):
        gone = self.t.expire(time.monotonic() + T.HELLO_SECONDS + 1, lambda addr: False)
        self.assertEqual(gone, [self.conn.addr])
        self.assertEqual(self.t.conns, {})


def select_readable(sock):
    import select
    return bool(select.select([sock], [], [], 0)[0])


class ThroughTheHub(unittest.TestCase):
    def start(self, auth="off"):
        port = free_port()
        os.environ["FH2MP_TUNNEL_PORT"] = str(port)
        self.addCleanup(os.environ.pop, "FH2MP_TUNNEL_PORT", None)
        self.hub = S.Hub(0, None)
        self.hub.udp_auth = auth
        stop = threading.Event()
        worker = threading.Thread(target=self.hub.serve, args=(stop,), daemon=True)
        def close():
            stop.set()
            worker.join(2)
            self.assertFalse(worker.is_alive(), 'test left a Hub thread running')
            if self.hub.tunnel:
                self.hub.tunnel.close()
            self.hub.sock.close()
            if self.hub.ctl:
                self.hub.ctl.close()
        self.addCleanup(close)
        worker.start()
        return port

    def test_a_player_connects_through_tcp_and_leaves_with_it(self):
        port = self.start()
        c = socket.create_connection(("127.0.0.1", port))
        self.addCleanup(c.close)
        c.sendall(frame(open_connection(0x1234)))
        c.settimeout(3)
        buf = b""
        while len(buf) < 2 or len(buf) < 2 + struct.unpack(">H", buf[:2])[0]:
            buf += c.recv(4096)
        reply = buf[2:2 + struct.unpack(">H", buf[:2])[0]]
        self.assertEqual(reply[11] & 0x7F, S.MSG_OPEN_CONNECTION_RESPONSE)
        self.assertTrue(wait(lambda: any(p.xuid == 0x1234 for w in self.hub.worlds.values() for p in w.peers.values())))
        c.close()
        self.assertTrue(wait(lambda: not any(w.peers for w in self.hub.worlds.values()), 5))

    def test_with_auth_required_only_signed_in_addresses_get_a_connection(self):
        port = self.start(auth="required")
        c = socket.create_connection(("127.0.0.1", port))
        self.addCleanup(c.close)
        c.settimeout(2)
        self.assertEqual(c.recv(16), b"")                      # closed at once: nobody is signed in from 127.0.0.1
        self.hub.authed[0x1234] = ("127.0.0.1", time.monotonic() + 60)
        d = socket.create_connection(("127.0.0.1", port))
        self.addCleanup(d.close)
        d.sendall(frame(open_connection(0x1234)))
        d.settimeout(3)
        self.assertTrue(d.recv(4096))

    def test_a_connection_that_never_opens_a_game_connection_is_closed(self):
        hello = T.HELLO_SECONDS
        T.HELLO_SECONDS = 0.3                                   # the real 10 s wait made this one test 70 % of the server lane
        self.addCleanup(setattr, T, "HELLO_SECONDS", hello)
        port = self.start()
        c = socket.create_connection(("127.0.0.1", port))
        self.addCleanup(c.close)
        c.sendall(frame(b"\x00" * 40))                          # junk
        c.settimeout(T.HELLO_SECONDS + 5)
        self.assertEqual(c.recv(16), b"")


class SessionDocument(unittest.TestCase):
    def test_a_tunnelled_client_is_told_the_server_is_on_its_own_machine(self):
        import xbl
        doc = {"servers": {"gameServerPlatform": {"properties": {"custom": {"hostName": "game.example", "status": "Active"}}}},
               "members": {}}
        xbl.for_tunnel(doc)
        custom = doc["servers"]["gameServerPlatform"]["properties"]["custom"]
        self.assertEqual((custom["hostName"], custom["status"]), ("127.0.0.1", "Active"))

    def test_a_document_without_servers_is_left_alone(self):
        import xbl
        self.assertEqual(xbl.for_tunnel({"members": {}}), {"members": {}})
        self.assertIsNone(xbl.for_tunnel(None))


if __name__ == "__main__":
    unittest.main()
