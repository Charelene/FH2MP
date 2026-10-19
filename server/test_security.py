"""Regression tests for the input-validation and authorization fixes (UDP identity, MPSD sessions, blob uploads, livery inflate, PNG
dimensions). No database or game files needed: python3 -m unittest test_security (from the server directory)."""
import os
import struct
import sys
import time
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("FH2MP_CONTROL_PORT", "0")
import fh2mp_server as S  # noqa: E402
import json  # noqa: E402
import lzma  # noqa: E402
import pngtool  # noqa: E402
import ugc  # noqa: E402
import xbl  # noqa: E402


class FakeSock:
    def __init__(self):
        self.sent = []

    def sendto(self, data, addr):
        self.sent.append((data, addr))

    def setsockopt(self, *a):
        pass


def open_msg(w, player_id, version=S.VERSION_STRING):
    w.bits(S.MSG_OPEN_CONNECTION, 7)
    w.string(version)
    w.string(S.GAME_STRING)
    w.bits(0, 2)
    w.bits(player_id, 64)
    w.bits(2, 32)
    w.bits(0, 32)
    w.bits(7, 32)


def open_packet(*player_ids):
    return S.build_packet([lambda w, p=p: open_msg(w, p) for p in player_ids])


def other_msg(w):
    w.bits(S.MSG_CLOSE_CONNECTION, 7)
    w.bits(0, 8)


def make_hub(auth="off"):
    os.environ["FH2MP_UDP_AUTH"] = auth
    hub = S.Hub(0, None)
    hub.sock = hub.idle.sock = FakeSock()
    return hub


def peers_total(hub):
    return sum(len(w.peers) for w in list(hub.worlds.values()) + [hub.idle])


class UdpIdentity(unittest.TestCase):
    def setUp(self):
        self.hub = make_hub("required")
        self.addr = ("10.1.1.1", 4000)
        self.hub.authed[0xA] = ("10.1.1.1", time.monotonic() + 100)       # only player A signed in from this address

    def peer(self):
        return self.hub.route[self.addr].peers.get(self.addr)

    def test_second_open_connection_in_a_datagram_is_checked(self):
        self.hub.handle(open_packet(0xA, 0xB), self.addr)                 # A is authorised, B is not
        self.assertEqual(peers_total(self.hub), 0)

    def test_later_open_connection_cannot_change_the_identity(self):
        self.hub.handle(open_packet(0xA), self.addr)
        self.assertEqual(self.peer().xuid, 0xA)
        self.hub.authed[0xB] = ("10.1.1.1", time.monotonic() + 100)       # even a sign-in from the same address does not take over
        self.hub.handle(open_packet(0xB), self.addr)
        self.assertEqual(self.peer().xuid, 0xA)

    def test_open_connection_after_another_message_is_checked(self):
        pkt = S.build_packet([other_msg, lambda w: open_msg(w, 0xB)])
        self.hub.handle(pkt, self.addr)
        self.assertEqual(peers_total(self.hub), 0)

    def test_signed_in_player_connects(self):
        self.hub.handle(open_packet(0xA), self.addr)
        self.assertEqual(self.peer().xuid, 0xA)


class UdpMemory(unittest.TestCase):
    def setUp(self):
        self.hub = make_hub("off")

    def test_junk_creates_no_state(self):
        for i in range(500):
            addr = ("203.0.113.%d" % (i % 250), 1000 + i)
            self.hub.handle(b"\x01", addr)                                # shorter than a header
            self.hub.handle(S.build_packet([]), addr)                     # a header with no messages
            self.hub.handle(S.build_packet([other_msg]), addr)            # does not start with OpenConnection
        self.assertEqual(peers_total(self.hub), 0)
        self.assertEqual(len(self.hub.route), 0)

    def test_half_open_peers_are_capped_and_expire(self):
        for i in range(S.MAX_PENDING_PER_IP + 5):
            self.hub.handle(open_packet(100 + i), ("198.51.100.7", 2000 + i))
        self.assertEqual(peers_total(self.hub), S.MAX_PENDING_PER_IP)
        world = next(iter(self.hub.worlds.values()))
        for p in world.peers.values():
            p.last_rx -= S.PENDING_SECONDS + 1
        world.tick()
        self.assertEqual(len(world.peers), 0)


class Sessions(unittest.TestCase):
    PATH = "/serviceconfigs/s/sessionTemplates/t/sessions/one"

    def setUp(self):
        xbl._SESSIONS.clear()
        st, _ = xbl.put_session(self.PATH, b'{"members":{"me":{}}}', "111")
        self.assertEqual(st, 200)

    def test_cannot_remove_or_write_another_member(self):
        xbl.put_session(self.PATH, b'{"members":{"me":{}}}', "222")
        for body in (b'{"members":{"111":null}}', b'{"members":{"111":{"properties":{"system":{"active":true}}}}}'):
            self.assertEqual(xbl.put_session(self.PATH, body, "222")[0], 403)
        self.assertIn("111", xbl._SESSIONS[tuple(x.lower() for x in xbl.parse_path(self.PATH))]["members"])

    def test_stranger_cannot_change_the_session(self):
        self.assertEqual(xbl.put_session(self.PATH, b'{"properties":{"custom":{"x":1}}}', "333")[0], 403)
        self.assertEqual(xbl.put_session(self.PATH, b'{"members":{"me":{}},"properties":{"custom":{"x":1}}}', "333")[0], 403)

    def test_delete_needs_the_owner(self):
        key = tuple(x.lower() for x in xbl.parse_path(self.PATH))
        self.assertEqual(xbl.delete_session(self.PATH, "333"), 403)
        self.assertIn(key, xbl._SESSIONS)
        xbl.put_session(self.PATH, b'{"members":{"me":{}}}', "222")
        self.assertEqual(xbl.delete_session(self.PATH, "222"), 204)       # a member only leaves
        self.assertIn(key, xbl._SESSIONS)
        self.assertNotIn("222", xbl._SESSIONS[key]["members"])
        self.assertEqual(xbl.delete_session(self.PATH, "111"), 204)       # the owner deletes it
        self.assertNotIn(key, xbl._SESSIONS)

    def test_server_side_calls_are_not_restricted(self):
        self.assertEqual(xbl.put_session(self.PATH, b'{"members":{"111":null}}')[0], 200)


class FakeDb:
    """Just enough of db.Db for UgcStore.blob_*: one table in memory, `?` statements matched by prefix."""

    def __init__(self):
        self.rows = {}
        self.n = 0

    def tx(self):
        db = self

        class Tx:
            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

            def execute(self, sql, p=()):
                return db.run(sql, p)
        return Tx()

    class R:
        def __init__(self, rows=(), rowcount=0, lastrowid=None):
            self.rows, self.rowcount, self.lastrowid = list(rows), rowcount, lastrowid

        def fetchone(self):
            return self.rows[0] if self.rows else None

    def run(self, sql, p):
        if sql.startswith("SELECT COUNT"):
            mine = [r for r in self.rows.values() if r["owner"] == p[0]]
            return self.R([{"n": len(mine), "b": sum(r["total_size"] for r in mine)}])
        if sql.startswith("INSERT"):
            self.n += 1
            self.rows[self.n] = {"owner": p[0], "total_size": p[2], "data": p[4 - 1], "received": 0}
            return self.R(lastrowid=self.n)
        if sql.startswith("SELECT data, total_size"):
            r = self.rows.get(p[0])
            return self.R([r] if r and r["owner"] == p[1] else [])
        if sql.startswith("UPDATE"):
            r = self.rows.get(p[2])
            if r and r["owner"] == p[3]:
                r["data"], r["received"] = p[0], p[1]
                return self.R(rowcount=1)
            return self.R()
        raise AssertionError(sql)


class Blobs(unittest.TestCase):
    def setUp(self):
        self.s = ugc.UgcStore(FakeDb())

    def test_other_account_cannot_write(self):
        bid = self.s.blob_create(1, "x", 100)
        self.assertIsNone(self.s.blob_write(bid, 0, b"evil", owner=2))
        self.assertEqual(self.s.blob_write(bid, 0, b"fine", owner=1), 4)
        self.assertEqual(ugc.unpack(self.s.db.rows[bid]["data"]), b"fine")

    def test_offsets_and_sizes_are_bounded(self):
        bid = self.s.blob_create(1, "x", 100)
        for off, data in ((-1, b"a"), (10 ** 12, b"a"), (99, b"ab"), (0, b"a" * 101)):
            with self.assertRaises(ugc.LimitError):
                self.s.blob_write(bid, off, data, owner=1)
        with self.assertRaises(ugc.LimitError):
            self.s.blob_create(1, "x", ugc.MAX_BLOB_SIZE + 1)
        with self.assertRaises(ugc.LimitError):
            self.s.blob_create(1, "x", -5)

    def test_account_quota(self):
        for _ in range(ugc.MAX_BLOBS_PER_OWNER):
            self.s.blob_create(1, "x", 1)
        with self.assertRaises(ugc.LimitError):
            self.s.blob_create(1, "x", 1)
        self.s.blob_create(2, "x", 1)


class Inflate(unittest.TestCase):
    def test_livery_chunk(self):
        raw = b"vlrc" + b"\0" * 1000
        comp = zlib.compress(raw)
        self.assertEqual(ugc.livery_chunk(struct.pack("<II", len(comp), len(raw)) + comp), raw)
        # a bomb declaring a small size, or a size above the cap, is refused before it inflates
        bomb = zlib.compress(b"\0" * 200_000_000)
        self.assertIsNone(ugc.livery_chunk(struct.pack("<II", len(bomb), 10) + bomb))
        self.assertIsNone(ugc.livery_chunk(struct.pack("<II", len(bomb), 200_000_000) + bomb))
        self.assertIsNone(ugc.livery_chunk(struct.pack("<II", len(comp), len(raw) + 1) + comp))


def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)


def ihdr(w, h):
    return chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))


class Png(unittest.TestCase):
    def test_second_header_with_huge_dimensions_is_rejected(self):
        sig = b"\x89PNG\r\n\x1a\n"
        # first header small (what a size check on bytes 16:24 sees), second header huge
        evil = sig + ihdr(40, 40) + ihdr(60000, 60000) + chunk(b"IDAT", zlib.compress(b"\0" * 1000)) + chunk(b"IEND", b"")
        with self.assertRaises(ValueError):
            pngtool.decode(evil)

    def test_dimension_limit_is_enforced_in_the_decoder(self):
        big = b"\x89PNG\r\n\x1a\n" + ihdr(60000, 60000) + chunk(b"IDAT", zlib.compress(b"\0" * 1000)) + chunk(b"IEND", b"")
        with self.assertRaises(ValueError):
            pngtool.decode(big)
        with self.assertRaises(ValueError):
            pngtool.downscale(big, 192, 108)
        ok = pngtool.encode(600, 40, 4, [bytearray(2400)] * 40)
        self.assertEqual(pngtool.decode(ok)[:2], (600, 40))
        with self.assertRaises(ValueError):
            pngtool.decode(ok, max_dim=512)

    def test_truncated_chunk(self):
        png = pngtool.encode(40, 40, 4, [bytearray(160)] * 40)
        with self.assertRaises(ValueError):
            pngtool.decode(png[:-20])


class Revocation(unittest.TestCase):
    def test_revoke_ends_live_connections_and_blocks_reconnecting(self):
        hub = make_hub("off")
        addr = ("10.2.2.2", 5000)
        hub.handle(open_packet(0xC), addr)
        world = hub.route[addr]
        peer = world.peers[addr]
        peer.connected = True
        before = len(hub.sock.sent)
        hub.revoke(0xC)
        self.assertNotIn(addr, world.peers)
        self.assertGreater(len(hub.sock.sent), before)                    # the client was told with a CloseConnection
        hub.handle(open_packet(0xC), addr)
        self.assertNotIn(addr, hub.route.get(addr, world).peers)
        hub.revoked.pop(0xC)                                              # "allow" after a fresh sign-in
        hub.handle(open_packet(0xC), addr)
        self.assertEqual(hub.route[addr].peers[addr].xuid, 0xC)

    def test_revoked_player_cannot_be_reauthorised_by_a_stale_announcement(self):
        hub = make_hub("required")
        hub.revoke(0xD)
        hub.authed.pop(0xD, None)
        self.assertFalse(hub.open_allowed(0xD, ("10.3.3.3", 1)))


class Storage(unittest.TestCase):
    def test_client_bytes_are_never_taken_for_our_format(self):
        bomb = lzma.compress(b"\0" * (ugc.MAX_UNPACKED + 5), format=lzma.FORMAT_ALONE)
        hostile = ugc.MAGIC + b"\x01" + bomb                              # what an attacker uploads
        stored = ugc.pack(hostile)
        self.assertEqual(ugc.unpack(stored), hostile)                     # wrapped like any bytes, returned verbatim
        with self.assertRaises(ugc.LimitError):                           # and an old row holding such bytes is capped on read
            ugc.unpack(hostile)

    def test_normal_round_trip(self):
        for data in (b"", b"abc", b"\0" * 100000, ugc.MAGIC + b"\x00xyz"):
            self.assertEqual(ugc.unpack(ugc.pack(data)), data)


class Tickets(unittest.TestCase):
    PATH = "/serviceconfigs/s/sessionTemplates/t/sessions/%s"

    def setUp(self):
        xbl._SESSIONS.clear()
        xbl._TICKETS.clear()
        xbl.announce_player = lambda *a: None
        xbl.put_session(self.PATH % "victim", b'{"members":{"me":{}}}', "111")

    def ticket(self, ref_name, who):
        ref = {"scid": "s", "templateName": "t", "name": ref_name}
        return xbl.create_ticket("/serviceconfigs/s/hoppers/h", json.dumps({"ticketSessionRef": ref}).encode(), who)

    def test_cannot_name_a_session_you_are_not_in(self):
        self.assertEqual(self.ticket("victim", "222")[0], 403)
        self.assertEqual(self.ticket("nowhere", "222")[0], 403)
        self.assertEqual(self.ticket("victim", "111")[0], 200)

    def test_ticket_matches_only_its_creator(self):
        xbl.put_session(self.PATH % "victim", b'{"members":{"me":{}}}', "222")       # a joiner of the same session
        st, t = self.ticket("victim", "222")
        self.assertEqual(st, 200)
        st, doc = xbl.get_ticket("/serviceconfigs/s/hoppers/h/tickets/" + t["ticketId"], "222")
        name = doc["targetSessionRef"]["name"]
        members = xbl._SESSIONS[("s", "defaulttemplate", name.lower())]["members"]
        self.assertEqual(sorted(members), ["222"])
        self.assertEqual(xbl.get_ticket("/serviceconfigs/s/hoppers/h/tickets/" + t["ticketId"], "111")[0], 404)

    def test_drop_player(self):
        self.ticket("victim", "111")
        xbl.drop_player(111)
        self.assertEqual(xbl._TICKETS, {})
        self.assertNotIn("111", next(iter(xbl._SESSIONS.values()))["members"])


class MemberIdentity(unittest.TestCase):
    PATH = "/serviceconfigs/s/sessionTemplates/t/sessions/id"

    def setUp(self):
        xbl._SESSIONS.clear()

    def test_explicit_own_entry_cannot_claim_a_foreign_xuid(self):
        body = json.dumps({"members": {"111": {"constants": {"system": {"xuid": "999"}}}}}).encode()
        st, doc = xbl.put_session(self.PATH, body, "111")
        self.assertEqual(st, 200)
        self.assertEqual({m["constants"]["system"]["xuid"] for m in doc["members"].values()}, {"111"})
        st, doc = xbl.put_session(self.PATH, json.dumps({"members": {"me": {"constants": {"system": {"xuid": "999"}}}}}).encode(), "111")
        self.assertEqual({m["constants"]["system"]["xuid"] for m in doc["members"].values()}, {"111"})

    def test_malformed_members_are_refused(self):
        self.assertEqual(xbl.put_session(self.PATH, b'{"members":{"me":"x"}}', "111")[0], 400)
        self.assertEqual(xbl.put_session(self.PATH, b'{"members":{"me":{"constants":[]}}}', "111")[0], 400)


if __name__ == "__main__":
    unittest.main()
