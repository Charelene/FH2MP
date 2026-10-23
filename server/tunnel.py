"""TCP carrier for players whose UDP does not reach the game server (experimental, FH2MP_TUNNEL_PORT; 0 = off).

A client behind a network that drops UDP (carrier-grade NAT, hotel and school Wi-Fi, some VPN-less mobile links) runs a small local UDP
proxy in the launcher, and the game talks to that. The proxy carries every datagram to this listener as a frame, [length: 2 bytes big
endian][datagram], and the replies come back the same way. Everything past the framing is the normal NetV2 stream: the hub treats a TCP
connection like one UDP address (the TCP peer's ip and port), so the sign-in check, worlds, relays and kicks work unchanged.

Datagrams are not reliable in the game's own protocol, and TCP must not make them wait behind each other: a connection whose send buffer
holds more than SEND_BACKLOG bytes drops new datagrams (as the network would) instead of queueing them.
"""
import socket
import struct
import time

MAX_FRAME = 2048            # the game's packets are at most MAX_PACKET_BYTES (about 1.3 kB)
RECV_BUFFER = 65536         # an unfinished frame can never be larger than this
SEND_BACKLOG = 48 * 1024    # bytes waiting for a slow connection; beyond it datagrams are dropped
MAX_CONNECTIONS = 512
PER_IP = 3
HELLO_SECONDS = 10.0        # a connection must have opened a game connection (an accepted OpenConnection) by then
IDLE_SECONDS = 60.0         # no frame at all (the proxy sends an empty keep-alive frame every 15 s)


class Conn:
    def __init__(self, sock, addr, now):
        self.sock, self.addr = sock, addr
        self.rbuf = bytearray()
        self.wbuf = bytearray()
        self.born = self.last_rx = now
        self.dead = False

    def fileno(self):
        return self.sock.fileno()


class Tunnel:
    def __init__(self, port, log=lambda m: None, admit=None, on_open=None, on_close=None):
        """`admit(ip)` says whether a connection from that address may be accepted at all (the hub: signed in from there);
        `on_open(conn)` / `on_close(conn)` are called when a connection is taken / just before it is closed (the hub's selector)."""
        self.log = log
        self.admit = admit or (lambda ip: True)
        self.on_open = on_open or (lambda conn: None)
        self.on_close = on_close or (lambda conn: None)
        self.conns = {}                         # (ip, port) -> Conn
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("0.0.0.0", port))
        self.listener.listen(64)
        self.listener.setblocking(False)
        self.port = self.listener.getsockname()[1]

    # -- reading ---------------------------------------------------------------------------------------------------------------
    def accept(self, now=None):
        """Accepts waiting connections; refused ones are closed at once."""
        now = time.monotonic() if now is None else now
        while True:
            try:
                s, addr = self.listener.accept()
            except (BlockingIOError, InterruptedError):
                return
            except OSError:
                return
            ip = addr[0]
            if len(self.conns) >= MAX_CONNECTIONS or sum(1 for a in self.conns if a[0] == ip) >= PER_IP or not self.admit(ip):
                s.close()
                continue
            s.setblocking(False)
            try:
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:
                pass
            self.conns[addr] = conn = Conn(s, addr, now)
            self.on_open(conn)

    def read(self, conn, now=None):
        """Datagrams [(data)] that arrived on `conn`; the connection is marked dead on EOF, error or a bad frame."""
        now = time.monotonic() if now is None else now
        out = []
        try:
            chunk = conn.sock.recv(65536)
        except (BlockingIOError, InterruptedError):
            return out
        except OSError:
            conn.dead = True
            return out
        if not chunk:
            conn.dead = True
            return out
        conn.last_rx = now
        conn.rbuf += chunk
        while len(conn.rbuf) >= 2:
            n = struct.unpack(">H", conn.rbuf[:2])[0]
            if n > MAX_FRAME:
                conn.dead = True
                break
            if len(conn.rbuf) < 2 + n:
                break
            if n:
                out.append(bytes(conn.rbuf[2:2 + n]))
            del conn.rbuf[:2 + n]
        if len(conn.rbuf) > RECV_BUFFER:
            conn.dead = True
        return out

    # -- writing ---------------------------------------------------------------------------------------------------------------
    def send(self, data, addr):
        """Queues one datagram for the connection at `addr`; False when there is none (or the datagram was dropped, backlog full)."""
        conn = self.conns.get(addr)
        if conn is None or conn.dead:
            return False
        if len(data) > MAX_FRAME or len(conn.wbuf) > SEND_BACKLOG:
            return False
        conn.wbuf += struct.pack(">H", len(data)) + data
        self.flush(conn)
        return True

    def flush(self, conn):
        if not conn.wbuf or conn.dead:
            return
        try:
            n = conn.sock.send(conn.wbuf)
        except (BlockingIOError, InterruptedError):
            return
        except OSError:
            conn.dead = True
            return
        del conn.wbuf[:n]

    def flush_all(self):
        for conn in self.conns.values():
            if conn.wbuf:
                self.flush(conn)

    # -- housekeeping ----------------------------------------------------------------------------------------------------------
    def expire(self, now, opened):
        """Marks connections dead that sent nothing for IDLE_SECONDS or never opened a game connection (`opened(addr)` -> bool) within
        HELLO_SECONDS, then closes and returns every dead connection's address."""
        for addr, conn in self.conns.items():
            if now - conn.last_rx > IDLE_SECONDS or (now - conn.born > HELLO_SECONDS and not opened(addr)):
                conn.dead = True
        gone = [a for a, c in self.conns.items() if c.dead]
        for a in gone:
            conn = self.conns.pop(a)
            self.on_close(conn)
            try:
                conn.sock.close()
            except OSError:
                pass
        return gone

    def close(self):
        for c in self.conns.values():
            self.on_close(c)
            try:
                c.sock.close()
            except OSError:
                pass
        self.conns.clear()
        self.listener.close()


class MuxSock:
    """What the worlds send through: a datagram for a tunnelled address goes into its TCP connection, any other over the UDP socket."""

    def __init__(self, udp, tunnel=None):
        self.udp, self.tunnel = udp, tunnel

    def sendto(self, data, addr):
        if self.tunnel is not None and addr in self.tunnel.conns:
            self.tunnel.send(data, addr)
            return len(data)
        return self.udp.sendto(data, addr)
