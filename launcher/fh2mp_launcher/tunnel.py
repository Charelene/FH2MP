"""UDP-over-TCP proxy for the experimental TCP fallback (Settings: "Connect over TCP").

The game talks UDP to 127.0.0.1:<local port>; each datagram goes to the game server's TCP carrier (server/tunnel.py) as a frame,
[length: 2 bytes big endian][datagram], and the frames that come back are sent to the game as datagrams. An empty frame is a keep-alive.
One TCP connection per source address the game sends from. Like the network, the proxy drops datagrams for a connection that cannot keep
up (SEND_BACKLOG) instead of queueing them: the game's own protocol copes with loss, not with delay.
"""
import selectors
import socket
import struct
import threading
import time

MAX_FRAME = 2048
SEND_BACKLOG = 48 * 1024
KEEPALIVE_SECONDS = 15.0
MAX_CONNECTIONS = 3                 # the server accepts three per address
CONNECT_TIMEOUT = 6.0
RETRY_SECONDS = 3.0                 # after a failed connect, datagrams are dropped for this long before the next attempt


class _Link:
    def __init__(self, sock):
        self.sock = sock
        self.rbuf = bytearray()
        self.wbuf = bytearray()
        self.last_tx = time.monotonic()


class UdpOverTcp:
    def __init__(self, host, tcp_port, local_port=0, log=lambda m: None):
        self.host, self.tcp_port, self.log = host, tcp_port, log
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(("127.0.0.1", local_port))
        self.udp.setblocking(False)
        self.port = self.udp.getsockname()[1]
        self.links = {}                 # game source address -> _Link
        self.retry_at = 0.0
        self.sel = selectors.DefaultSelector()
        self.sel.register(self.udp, selectors.EVENT_READ, None)
        self.stopping = False
        self.thread = None
        self.stats = {"sent": 0, "received": 0, "dropped": 0, "connects": 0, "failed": 0}

    def start(self):
        self.thread = threading.Thread(target=self.run, name="udp-over-tcp", daemon=True)
        self.thread.start()
        return self

    def stop(self):
        self.stopping = True
        if self.thread is not None:
            self.thread.join(2.0)
        for addr in list(self.links):
            self._close(addr)
        try:
            self.sel.unregister(self.udp)
        except (KeyError, ValueError):
            pass
        self.udp.close()
        self.sel.close()

    # ------------------------------------------------------------------------------------------------------------------------
    def run(self):
        while not self.stopping:
            try:
                for key, _mask in self.sel.select(timeout=0.02):
                    if key.data is None:
                        self._from_game()
                    else:
                        self._from_server(key.data)
                self._flush_and_keepalive()
            except OSError as e:
                self.log(f"tunnel: {e!r}")

    def _from_game(self):
        for _ in range(256):
            try:
                data, addr = self.udp.recvfrom(65535)
            except (BlockingIOError, InterruptedError):
                return
            except OSError:
                return                  # e.g. ICMP "port unreachable" echoed back on Windows
            link = self.links.get(addr)
            if link is None:
                link = self._connect(addr)
            if link is None or len(data) > MAX_FRAME or len(link.wbuf) > SEND_BACKLOG:
                self.stats["dropped"] += 1
                continue
            link.wbuf += struct.pack(">H", len(data)) + data
            self.stats["sent"] += 1
            self._write(addr, link)

    def _connect(self, addr):
        now = time.monotonic()
        if len(self.links) >= MAX_CONNECTIONS or now < self.retry_at:
            return None
        try:
            s = socket.create_connection((self.host, self.tcp_port), timeout=CONNECT_TIMEOUT)
        except OSError as e:
            self.stats["failed"] += 1
            self.retry_at = now + RETRY_SECONDS
            self.log(f"tunnel: cannot reach {self.host}:{self.tcp_port}: {e!r}")
            return None
        s.setblocking(False)
        try:
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
        link = self.links[addr] = _Link(s)
        self.sel.register(s, selectors.EVENT_READ, addr)
        self.stats["connects"] += 1
        self.log(f"tunnel: connected to {self.host}:{self.tcp_port} for the game's port {addr[1]}")
        return link

    def _write(self, addr, link):
        if not link.wbuf:
            return
        try:
            n = link.sock.send(link.wbuf)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            self._close(addr)
            return
        del link.wbuf[:n]
        link.last_tx = time.monotonic()

    def _from_server(self, addr):
        link = self.links.get(addr)
        if link is None:
            return
        try:
            chunk = link.sock.recv(65536)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            chunk = b""
        if not chunk:
            self.log(f"tunnel: the server closed the connection for port {addr[1]}")
            self._close(addr)
            return
        link.rbuf += chunk
        while len(link.rbuf) >= 2:
            n = struct.unpack(">H", link.rbuf[:2])[0]
            if n > MAX_FRAME:
                self._close(addr)
                return
            if len(link.rbuf) < 2 + n:
                break
            if n:
                try:
                    self.udp.sendto(bytes(link.rbuf[2:2 + n]), addr)
                    self.stats["received"] += 1
                except OSError:
                    pass
            del link.rbuf[:2 + n]

    def _flush_and_keepalive(self):
        now = time.monotonic()
        for addr, link in list(self.links.items()):
            if link.wbuf:
                self._write(addr, link)
            elif now - link.last_tx > KEEPALIVE_SECONDS:
                link.wbuf += b"\x00\x00"
                self._write(addr, link)

    def _close(self, addr):
        link = self.links.pop(addr, None)
        if link is None:
            return
        try:
            self.sel.unregister(link.sock)
        except (KeyError, ValueError):
            pass
        try:
            link.sock.close()
        except OSError:
            pass


def free_udp_port(candidates=range(5200, 5300)):
    """A free 4-digit local UDP port (the shim's FH2MP_SERVER_PORT takes exactly four digits)."""
    for p in candidates:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind(("127.0.0.1", p))
            return p
        except OSError:
            continue
        finally:
            s.close()
    return 0


PROBE_MAGIC = b"FH2MPPN1"


def probe_udp(host, port, tries=3, wait=1.2):
    """Whether the game server's UDP port answers: one datagram (the game's 11-byte header, the probe magic, 8 random bytes) that the server
    sends back unchanged (server/probe.py). False after `tries` unanswered tries: a network that drops UDP, or a server that does not
    answer probes (the caller only asks one that says it does). Never raises."""
    import os
    for _ in range(tries):
        nonce = os.urandom(8)
        data = bytes(11) + PROBE_MAGIC + nonce
        s = None
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.settimeout(wait)
            s.sendto(data, (host, port))
            reply, _addr = s.recvfrom(256)
            if reply == data:
                return True
        except (OSError, UnicodeError):
            pass
        finally:
            if s is not None:
                s.close()
    return False
