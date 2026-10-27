import socket
import struct
import threading
import time
import unittest

from fh2mp_launcher import tunnel


class FakeServer:
    """The server's TCP carrier, reduced to what the proxy sees: frames in, frames out."""

    def __init__(self):
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(4)
        self.srv.settimeout(.1)
        self.port = self.srv.getsockname()[1]
        self.got = []
        self.conn = None
        self.lock = threading.Lock()
        self.closed = threading.Event()
        self.connections = []
        self.workers = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.closed.is_set():
            try:
                c, _ = self.srv.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            self.conn = c
            self.connections.append(c)
            worker = threading.Thread(target=self.read, args=(c,), daemon=True)
            self.workers.append(worker)
            worker.start()

    def read(self, c):
        buf = b""
        while True:
            try:
                d = c.recv(4096)
            except OSError:
                return
            if not d:
                return
            buf += d
            while len(buf) >= 2 and len(buf) >= 2 + struct.unpack(">H", buf[:2])[0]:
                n = struct.unpack(">H", buf[:2])[0]
                with self.lock:
                    self.got.append(buf[2:2 + n])
                buf = buf[2 + n:]

    def send(self, data):
        self.conn.sendall(struct.pack(">H", len(data)) + data)

    def close(self):
        self.closed.set()
        self.srv.close()
        self.thread.join(1)
        for conn in self.connections:
            try:
                conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            conn.close()
        for worker in self.workers:
            worker.join(1)


def wait(cond, seconds=3.0):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.01)
    return False


class ProxyTests(unittest.TestCase):
    def setUp(self):
        self.server = FakeServer()
        self.addCleanup(self.server.close)
        self.proxy = tunnel.UdpOverTcp("127.0.0.1", self.server.port).start()
        self.addCleanup(self.proxy.stop)
        self.game = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.game.bind(("127.0.0.1", 0))
        self.game.settimeout(2)
        self.addCleanup(self.game.close)

    def test_datagrams_go_out_as_frames_and_replies_come_back(self):
        self.game.sendto(b"open", ("127.0.0.1", self.proxy.port))
        self.assertTrue(wait(lambda: self.server.got == [b"open"]))
        self.server.send(b"welcome")
        self.assertEqual(self.game.recvfrom(4096)[0], b"welcome")

    def test_order_is_kept(self):
        for i in range(50):
            self.game.sendto(b"m%d" % i, ("127.0.0.1", self.proxy.port))
        self.assertTrue(wait(lambda: len(self.server.got) == 50))
        self.assertEqual(self.server.got, [b"m%d" % i for i in range(50)])

    def test_an_unreachable_server_drops_datagrams_and_tries_again_later(self):
        dead = socket.socket()
        dead.bind(("127.0.0.1", 0))
        port = dead.getsockname()[1]
        dead.close()
        proxy = tunnel.UdpOverTcp("127.0.0.1", port).start()
        self.addCleanup(proxy.stop)
        self.game.sendto(b"x", ("127.0.0.1", proxy.port))
        self.assertTrue(wait(lambda: proxy.stats["failed"] == 1))
        self.game.sendto(b"y", ("127.0.0.1", proxy.port))
        self.assertTrue(wait(lambda: proxy.stats["dropped"] >= 2))
        self.assertEqual(proxy.stats["connects"], 0)

    def test_a_closed_connection_is_reopened_by_the_next_datagram(self):
        self.game.sendto(b"one", ("127.0.0.1", self.proxy.port))
        self.assertTrue(wait(lambda: self.server.got == [b"one"]))
        self.server.conn.shutdown(socket.SHUT_RDWR)
        self.assertTrue(wait(lambda: not self.proxy.links))
        self.game.sendto(b"two", ("127.0.0.1", self.proxy.port))
        self.assertTrue(wait(lambda: b"two" in self.server.got))
        self.assertEqual(self.proxy.stats["connects"], 2)

    def test_a_free_port_has_four_digits(self):
        p = tunnel.free_udp_port()
        self.assertTrue(1000 <= p <= 9999)


if __name__ == "__main__":
    unittest.main()
