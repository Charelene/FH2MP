"""LSP handlers: FH2MP_ENV=test FH2MP_TEST_PG_DSN=... python3 -I server/test_lsp.py (needs psycopg 3 and the `game`
catalog schema in the scratch database; public is reset). The default-parameter smoke test excludes NotifyRival,
which needs a specific notification payload; the main covered flows then run end to end."""
import json
import io
import os
import struct
import sys
import unittest
from unittest import mock
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ.setdefault("FH2MP_AUTH", "optional")      # these tests call as legacy clients (X-UserId); the LSP default is required
import fh2mp_lsp as L  # noqa: E402
import test_db  # noqa: E402
import ugc  # noqa: E402
import xls  # noqa: E402

CLS = {"Forza.WebServices.User": "Forza.WebServices", "Xls.WebServices.Content": "Xls.WebServices"}


def clubs_role(name):
    """The wire role number: 1 leader, 2 co-leader, 3 member."""
    return {"leader": 1, "co": 2, "member": 3}[name]


def merge(dst, src):
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            merge(dst[k], v)
        else:
            dst[k] = v


def call(cls, method, params=None):
    """Runs a handler through handle_xls with an encoded request; returns (result code, decoded results or None)."""
    asm = CLS.get(cls, "Forza.WebServices" if cls.startswith("Forza") else "Xls.WebServices")
    pname, pf = L.find_struct(cls, method, "_Parameters")
    body = b""
    if pf is not None:
        p = xls.default_struct(L.SC, pf)
        merge(p, params or {})
        body = xls.encode(L.SC, pf, p)
    out = L.handle_xls(f"{cls}, {asm}", method, body)
    result, error, n = struct.unpack(">III", out[:12])
    rname, rf = L.find_struct(cls, method, "_Results")
    if result == 0 and rf is not None and len(out) > 12 + n:
        return result, xls.decode(L.SC, rf, out[12 + n:])[0]
    return result, None


class StringValidationTests(unittest.TestCase):
    def request(self, body):
        handler = object.__new__(L.Handler)
        handler.path, handler.command = "/system/strings/validate", "POST"
        handler.headers = {"Content-Length": str(len(body)), "X-FH2MP-Host": "client-strings.xboxlive.com"}
        handler.client_address = ("127.0.0.1", 1234)
        handler.rfile, handler.wfile = io.BytesIO(body), io.BytesIO()
        handler.send_response = mock.Mock()
        handler.send_header = mock.Mock()
        handler.end_headers = mock.Mock()
        with mock.patch.object(handler, "_identify", return_value=True), mock.patch.object(L.social, "touch"), \
                mock.patch.object(L.CTX, "xuid", 1, create=True), mock.patch.object(L, "log"):
            handler._handle()
        return handler.send_response.call_args.args[0], json.loads(handler.wfile.getvalue())

    def test_photo_title_and_description_each_get_a_result(self):
        # Actual XSAPI wire spelling, unlike the old handler's stringsToVerify.
        status, response = self.request(json.dumps({"stringstoVerify": ["Sunset", "A drive by the sea"]}).encode())
        self.assertEqual(status, 200)
        self.assertEqual(response, {"verifyStringResult": [
            {"resultCode": 0, "offendingString": ""}, {"resultCode": 0, "offendingString": ""}]})

    def test_legacy_spelling_and_empty_lists_preserve_count(self):
        for key in ("stringstoVerify", "stringsToVerify"):
            for strings in ([], ["one"], ["one", "two", "three"]):
                status, response = self.request(json.dumps({key: strings}).encode())
                self.assertEqual((status, len(response["verifyStringResult"])), (200, len(strings)))

    def test_malformed_input_does_not_fabricate_a_success(self):
        for body in (b"{", b"null", b"{}", b"[]", b'{"stringstoVerify":"title"}', b'{"stringstoVerify":[1]}'):
            status, _ = self.request(body)
            self.assertEqual(status, 400, body)


class CarTokenTests(unittest.TestCase):
    """The game server's notices carry `{car:ID}`; the LSP names the car."""

    def expand(self, text, rows):
        db = mock.Mock()
        db.one.side_effect = lambda sql, args: rows.get(args[0])
        with mock.patch.object(L, "DB", db):
            return L.expand_car_tokens(text)

    def test_a_known_car_is_named_with_its_id(self):
        self.assertEqual(self.expand("on your car {car:2101} are", {2101: {"make": "Porsche", "name": "911 GT3"}}), "on your car Porsche 911 GT3 (2101) are")

    def test_a_car_the_catalog_lacks_stays_its_id(self):
        self.assertEqual(self.expand("on your car {car:9999} are", {}), "on your car 9999 are")

    def test_text_without_a_token_is_untouched(self):
        self.assertEqual(self.expand("hello {car:x} {cars}", {}), "hello {car:x} {cars}")


class GiftingFailsSafelyTests(unittest.TestCase):
    def test_sending_a_car_is_refused_so_the_game_puts_it_back(self):
        for cls, method in (("Forza.WebServices.Gifting, Forza.WebServices", "SendCarGift"),
                            ("Forza.WebServices.Auction, Forza.WebServices", "UploadAuctionTextures"),
                            ("Forza.WebServices.GiftLivery, Forza.WebServices", "Create")):
            self.assertEqual(struct.unpack(">I", L.handle_xls(cls, method, b"")[:4])[0], 2, method)


class AuthShareTests(unittest.TestCase):
    def test_one_line_an_hour_with_requests_and_players_on_each_side(self):
        now, lines = [0.0], []
        share = L.AuthShare(clock=lambda: now[0], sink=lines.append)
        for xuid, authed in ((1, True), (1, True), (2, True), (3, False), (3, False), (4, False), (5, False)):
            share.note(xuid, authed)
        self.assertEqual(lines, [])
        now[0] = 3600.0
        share.note(1, True)
        self.assertEqual(lines, ["auth share: last hour 3 requests with a token from 2 players, 4 without from 3"])
        now[0] = 7200.0
        share.note(9, False)
        self.assertEqual(lines[1], "auth share: last hour 1 requests with a token from 1 players, 0 without from 0")


class HandlerDecodeTests(unittest.TestCase):
    def test_a_method_without_a_handler_is_not_decoded(self):
        key = ("Forza.WebServices.OnlineProfile", "UpdateAllNoHandler")
        self.assertNotIn(key, L.HANDLERS)
        with mock.patch.object(L, "SC", mock.Mock()), mock.patch.object(L, "find_struct", return_value=("x", [])), \
                mock.patch.object(L.xls, "decode", side_effect=AssertionError("decoded")), \
                mock.patch.object(L.xls, "default_struct", return_value={}), mock.patch.object(L.xls, "encode", return_value=b""):
            out = L.handle_xls(key[0], key[1], b"\x00" * 64)
        self.assertEqual(struct.unpack(">III", out[:12]), (0, 0, 0))


class NotificationSessionTests(unittest.TestCase):
    def test_the_fallback_session_never_asks_for_an_immediate_poll(self):
        handler = L.HANDLERS[("Xls.WebServices.Notifications", "StartNewSession")][1]
        self.assertGreater(handler({})["nextRetrieveSeconds"], 0)


class LayerCountTests(unittest.TestCase):
    def test_operator_preview_content_type_matches_the_stored_image(self):
        handler = object.__new__(L.Handler)
        handler.command, handler.path, handler.headers = "GET", "/ops/v1/garage/1/preview.png", {}
        handler.client_address = ("127.0.0.1", 1234)
        handler._reply_json = mock.Mock()
        for data, content_type in ((b"\xff\xd8\xffphoto", "image/jpeg"), (b"\x89PNG\r\n\x1a\nimage", "image/png")):
            with mock.patch.object(L.ops_api, "handle", return_value=(200, data)):
                handler._ops(b"", False)
            self.assertEqual(handler._reply_json.call_args.args[2], content_type)

    def test_layer_count_always_fits_the_wire_field(self):
        self.assertEqual([L._layer_count(n) for n in (-1, 0, 7, 65535, 70000, None)], [0, 0, 7, 65535, 65535, 0])


    def test_livery_layers_prefers_the_stored_count_and_never_goes_negative(self):
        class NoBlobs:
            def blob_by_name(self, owner, name):
                return None
        old, L.UGC = L.UGC, NoBlobs()
        try:
            self.assertEqual(L._livery_layers({"layers": 18, "creator": 1, "name": "x"}), 18)
            self.assertEqual(L._livery_layers({"layers": -1, "creator": 1, "name": "x"}), 0)        # unknown and no upload left
            self.assertEqual(L._livery_layers({"layers": -1, "creator": 1, "name": ""}), 0)
        finally:
            L.UGC = old


    def test_shim_version_decides_who_may_see_layered_designs(self):
        f = L.shim_has_placement_fix
        self.assertEqual([f(v) for v in ("", "junk", "1.0.0-rc1", "1.0.0-rc2")], [False] * 4)
        self.assertEqual([f(v) for v in ("1.0.0-rc3", "1.0.0-rc10", "1.0.0", "1.0.1", "1.1.0-rc1", "1.0.0-rc2-debug")], [True] * 6)

    def test_an_outdated_clients_logon_is_refused_like_every_other_call(self):
        """Prod 2026-10-30: answering an outdated client's Logon with the empty result crashed every such game 10-30 s after it started
        (game+0x9c898f, a null CGameplayLog). Logon gets the 426 again; no ticket is announced."""
        cfg = L.launcher_api.LauncherConfig()
        cfg.overlay = {"minShim": "1.1.2"}
        old = (L.LAUNCHER, L.AUTH)
        announced, replies = [], []
        L.LAUNCHER, L.AUTH = L.launcher_api.Ctx(None, None, cfg, lambda: 0), None
        frame = L.frame()                                        # a real, empty result frame: only the routing is under test here

        class Req:
            headers = {}
            _remove_old_client = lambda self: None          # noqa: E731
            client_ip = lambda self: "127.0.0.1"            # noqa: E731
            _reply_json = lambda self, status, obj, ctype="application/json", *a, **k: replies.append((status, ctype, obj))   # noqa: E731
        try:
            with mock.patch.object(L, "announce_auth", lambda *a: announced.append(a)), \
                    mock.patch.object(L, "SC", object()):
                def ask(cls, method, shim="1.1.1"):
                    replies.clear()
                    r = Req()
                    r.headers = {"X-FH2MP-Shim": shim, "X-ClassName": cls, "X-Method": method}
                    return L.Handler._identify(r), list(replies)
                ok, got = ask("Forza.WebServices.User, Forza.WebServices", "Logon")
                self.assertFalse(ok)                                     # the request is answered here and goes no further
                self.assertEqual(got[0][0], 426)
                self.assertEqual(announced, [])
                ok, got = ask("Forza.WebServices.Scoreboard, Forza.WebServices", "GetRowsAnthem")
                self.assertEqual((ok, got[0][0]), (False, 426))          # everything else is still refused
                self.assertEqual(ask("Forza.WebServices.User, Forza.WebServices", "Logon", "1.1.2")[0], True)   # a current client is untouched
        finally:
            L.LAUNCHER, L.AUTH = old

    def test_min_shim_turns_old_clients_away(self):
        cfg = L.launcher_api.LauncherConfig()
        self.assertEqual(cfg.get()["minShim"], "0.0.0")
        old_launcher, old_auth, old_revoke = L.LAUNCHER, L.AUTH, L.on_revoke
        revoked = []

        class FakeAuth:
            mode = "optional"

            def authenticate(self, token, ip):
                if token != "fh2.good":
                    raise L.auth.AuthError(401, "invalid_token")
                return type("A", (), {"xuid": 0x900000000abc, "gamertag": "Old Timer"})()
        L.LAUNCHER, L.AUTH, L.on_revoke = L.launcher_api.Ctx(None, None, cfg, lambda: 0), FakeAuth(), revoked.append
        L._OLD_REMOVED.clear()
        ctx_before = {k: getattr(L.CTX, k, None) for k in ("xuid", "tag", "authed", "shim")}          # _identify writes the request context
        replies = []

        class Req:
            headers = {}
            _remove_old_client = L.Handler._remove_old_client
            _gamedb_ok = L.Handler._gamedb_ok
            client_ip = lambda self: "127.0.0.1"          # noqa: E731
            _reply_json = lambda self, status, obj, *a, **k: replies.append((status, obj["error"]))     # noqa: E731
        try:
            def ask(shim, token=""):
                replies.clear()
                r = Req()
                r.headers = {**({"X-FH2MP-Shim": shim} if shim else {}), **({"X-FH2MP-Token": token} if token else {})}
                return L.Handler._identify(r), list(replies)
            self.assertEqual(ask(""), (True, []))                          # off by default
            cfg.overlay = {"minShim": "1.0.0-rc4"}
            self.assertEqual(ask(""), (False, [(426, "shim_too_old")]))
            self.assertEqual(ask("1.0.0-rc3"), (False, [(426, "shim_too_old")]))
            self.assertEqual(ask("1.0.0-rc4"), (True, []))
            self.assertEqual(ask("1.0.0"), (True, []))
            self.assertEqual(revoked, [])                                   # nobody was identified so far: nobody removed
            self.assertEqual(ask("1.0.0-rc3", "fh2.bad")[0], False)
            self.assertEqual(revoked, [])                                   # a token that does not check out proves nothing
            self.assertEqual(ask("1.0.0-rc3", "fh2.good")[0], False)
            self.assertEqual(revoked, [0x900000000abc])                     # a verified old client is taken out of the game ...
            ask("1.0.0-rc3", "fh2.good")
            self.assertEqual(len(revoked), 1)                               # ... once a minute, not on every request
            self.assertEqual(ask("1.0.0-rc4", "fh2.good")[0], True)
            self.assertEqual(len(revoked), 1)                               # an up-to-date client is left alone
            r = Req()                                                       # its Message Center poll without a schema: the plain refusal
            r.headers = {"X-FH2MP-Shim": "1.0.0-rc3", "X-ClassName": "Xls.WebServices.Notifications, Xls.WebServices",
                         "X-Method": "RetrieveForUser"}
            replies.clear()
            self.assertEqual((L.Handler._identify(r), replies), (False, [(426, "shim_too_old")]))
        finally:
            L.LAUNCHER, L.AUTH, L.on_revoke = old_launcher, old_auth, old_revoke
            for k, v in ctx_before.items():
                setattr(L.CTX, k, v)

    def test_another_car_database_keeps_the_services_but_not_multiplayer(self):
        import hashlib
        import tempfile
        path = os.path.join(tempfile.mkdtemp(), "gamedbRC.slt")
        with open(path, "wb") as f:
            f.write(b"SQLite format 3\x00" + b"cars" * 100)
        with open(path, "rb") as f:
            good = hashlib.sha256(f.read()).hexdigest()
        old_launcher, old_auth, old_revoke = L.LAUNCHER, L.AUTH, L.on_revoke
        revoked, replies = [], []

        class FakeAuth:
            mode = "optional"

            def authenticate(self, token, ip):
                return type("A", (), {"xuid": 0x900000000def, "gamertag": "Panzer Fan"})()
        L.LAUNCHER = L.launcher_api.Ctx(None, None, L.launcher_api.LauncherConfig(), lambda: 0, gamedb=L.gamedb_sync.GameDb(""))
        L.AUTH, L.on_revoke = FakeAuth(), revoked.append
        L._OWN_CARS.clear()
        ctx_before = {k: getattr(L.CTX, k, None) for k in ("xuid", "tag", "authed", "shim", "own_cars")}

        class Req:
            headers = {}
            _gamedb_ok = L.Handler._gamedb_ok
            client_ip = lambda self: "127.0.0.1"          # noqa: E731
            _reply_json = lambda self, status, obj, *a, **k: replies.append((status, obj["error"]))     # noqa: E731
        try:
            def ask(db=None):
                replies.clear()
                r = Req()
                r.headers = {"X-FH2MP-Token": "fh2.x", **({"X-FH2MP-GameDb": db} if db is not None else {})}
                return L.Handler._identify(r), list(replies)
            self.assertEqual(ask("missing"), (True, []))                   # the server sets no database: nothing to compare
            self.assertFalse(L.CTX.own_cars)
            L.LAUNCHER.gamedb = L.gamedb_sync.GameDb(path)
            for match in (good, good.upper()):
                self.assertEqual(ask(match), (True, []))
                self.assertFalse(L.CTX.own_cars)
            self.assertEqual(revoked, [])
            for other in (None, "", "0" * 64, "missing"):                  # omitting the hash cannot bypass the multiplayer gate
                self.assertEqual(ask(other), (True, []))                   # every service stays open ...
                self.assertTrue(L.CTX.own_cars)                            # ... but the request is marked
            self.assertEqual(revoked, [0x900000000def])                    # taken out of the game once, not on every request
            self.assertEqual(ask(good), (True, []))                        # back to the server's database: unmarked, and a later change counts again
            self.assertFalse(L.CTX.own_cars)
            ask("0" * 64)
            self.assertEqual(revoked, [0x900000000def] * 2)
            L.LAUNCHER.gamedb = L.gamedb_sync.GameDb(path + ".missing")
            self.assertEqual(ask(good), (False, [(503, "gamedb_unavailable")]))
        finally:
            L.LAUNCHER, L.AUTH, L.on_revoke = old_launcher, old_auth, old_revoke
            for k, v in ctx_before.items():
                setattr(L.CTX, k, v)

    def test_indented_log_lines_carry_their_requests_number(self):
        import contextlib
        out = io.StringIO()
        before = getattr(L.CTX, "seq", None)
        try:
            with contextlib.redirect_stdout(out):
                L.CTX.seq = 41
                L.log("#41 POST host/x Forza.WebServices.Scoreboard SubmitScore body=10 user=0x1")      # the request line already has it
                L.log("  -> error result: no such livery 5")
                L.log("  #41 already numbered")
                L.log("stats sampler: nothing to attribute")                                            # not a request's line
                L.CTX.seq = None
                L.log("  ops: banned 0x1")                                                              # outside a request
        finally:
            L.CTX.seq = before
        lines = [l.split(" ", 1)[1] for l in out.getvalue().splitlines()]
        self.assertEqual(lines, ["#41 POST host/x Forza.WebServices.Scoreboard SubmitScore body=10 user=0x1",
                                 "  #41 -> error result: no such livery 5", "  #41 already numbered",
                                 "stats sampler: nothing to attribute", "  ops: banned 0x1"])

    def test_files_of_cars_the_server_does_not_have_are_listed_only_to_players_with_their_own_database(self):
        class Cars:
            def row(self, cid):
                return {"Id": cid} if cid in (325, 1496) else None            # the server's catalog: two cars of many
        files = [{"id": 1, "car_id": 325}, {"id": 2, "car_id": 99999}, {"id": 3, "car_id": 0}, {"id": 4, "car_id": None}, {"id": 5, "car_id": 1496}]
        old_cars, before = L.CARS, getattr(L.CTX, "own_cars", False)
        L.CARS = Cars()
        try:
            L.CTX.own_cars = False
            self.assertEqual([f["id"] for f in L._shown(files)], [1, 3, 4, 5])      # the pack car's file is not shown; car-less ones are
            L.CTX.own_cars = True
            self.assertEqual([f["id"] for f in L._shown(files)], [1, 2, 3, 4, 5])   # the player with packs sees everything
            L.CTX.own_cars = False
            L.CARS = None
            self.assertEqual(len(L._shown(files)), 5)                                  # no catalog loaded: nothing can be judged
        finally:
            L.CARS, L.CTX.own_cars = old_cars, before

    def test_a_marked_request_may_not_reach_multiplayer_or_the_boards(self):
        """The player's own car packs: tunes, liveries and photos keep working; matchmaking, session documents and the game servers do not."""
        replies, calls = [], []

        class Req:
            command, path = "POST", "/x"
            headers = {}
            rfile = io.BytesIO(b"")
            wfile = io.BytesIO()
            client_address = ("127.0.0.1", 1)
            close_connection = False

            def _identify(self):
                L.CTX.xuid, L.CTX.own_cars = 0x900000000def, True
                return True
            _reply_json = lambda self, status, obj, *a, **k: replies.append((status, obj["error"]))     # noqa: E731
            send_response = send_header = end_headers = lambda self, *a: None                             # noqa: E731
        ctx_before = {k: getattr(L.CTX, k, None) for k in ("xuid", "tag", "authed", "shim", "own_cars")}
        try:
            with mock.patch.object(L, "handle_xls", lambda *a: calls.append(a) or b""), mock.patch.object(L.social, "touch", lambda *a: None):
                for host in ("momatch.xboxlive.com", "sessiondirectory.xboxlive.com", "gameserverds.xboxlive.com",
                             "gameserverms.xboxlive.com"):                     # written out: not read from the code under test
                    replies.clear()
                    r = Req()
                    r.headers = {"Content-Length": "0", "X-FH2MP-Host": host}
                    L.Handler._handle(r)
                    self.assertEqual(replies, [(409, "gamedb_mismatch")], host)
                self.assertEqual(calls, [])
                r = Req()
                r.headers = {"Content-Length": "0", "X-FH2MP-Host": "gameservices.FH2.forzamotorsport.net",
                             "X-ClassName": "Forza.WebServices.Storefront, Forza.WebServices", "X-Method": "GetCuratedContent"}
                replies.clear()
                L.Handler._handle(r)
                self.assertEqual((replies, len(calls)), ([], 1))               # the tune and livery storefront is served as usual
        finally:
            for k, v in ctx_before.items():
                setattr(L.CTX, k, v)

    def test_layered_designs_are_recognised_even_when_the_stored_count_is_wrong(self):
        self.assertFalse(L.design_has_layers(0, b""))
        self.assertTrue(L.design_has_layers(12, b""))
        self.assertTrue(L.design_has_layers(-1, b"xyz"))              # unreadable: hidden, the safe side

    def test_placeholder_thumbnails_have_the_sizes_the_game_writes(self):
        big, small = L._placeholder_thumbs()
        self.assertEqual(L.pngtool.decode(big)[:2], L.BIG_THUMB)
        self.assertEqual(L.pngtool.decode(small)[:2], L.SMALL_THUMB)


class TuneBuildabilityTests(unittest.TestCase):
    """Opening a tune makes the client build its car for the stats page; an engine slot without a catalog row crashed it (2026-10-31)."""
    BAD = [1332002, 1332000, 1332000, -1, 1332003, 1332003, 1332003, 1332003, 1332009, 1332001, 1332000, 1332000, 90002000, 90002000, 90002000,
           90002000, 90002000, 90002000, 90002000, 90002000, 90002000, -1, -1, 90002000, -1, 90002000, -1, -1, -1, 90002000, 736001, 736000,
           736003, 736003, 1332001, 1332000, 1332001, 1332000, 1332000, 1332002, 1332003, 1332000, -1]
    SWAP = [1332001, 1332001, 1332000, -1, 1332003, 1332003, 1332003, 1332003, 1332003, 1332000, 1332001, 1332000, 2144003, 2144003, 2144003,
            2144003, 2144003, 2144003, 2144003, 2144003, 2144003, -1, -1, 2144000, -1, 2144003, -1, -1, -1, 2144002, 2102003, 2102003, 2102003,
            2102003, 1332000, 1332000, 1332001, 1332000, 1332003, 1332006, 1332003, 1332001, -1]

    def test_unbuildable_parts_become_the_cars_stock_parts(self):
        import parts
        out = L._buildable_tune_parts(1332, {"base": {"m_WheelStyle": 706}, "m_CarParts": self.BAD}, 5447)
        self.assertEqual(out["base"], {"m_WheelStyle": 706})
        self.assertEqual(parts.integrity_errors([w & 0xFFFFFFFF for w in out["m_CarParts"]]), [])
        self.assertEqual(out["m_CarParts"][12], 792000)          # the stock engine's camshaft, which holds the torque curve

    def test_uploading_an_unbuildable_tune_is_refused(self):
        req = {"carId": 1332, "installedParts": {"m_CarParts": self.BAD}, "tuning": {}}
        with mock.patch.object(L, "_me", return_value=(7, "x")), mock.patch.object(L, "UGC") as ugc_store:
            with self.assertRaises(L.XlsError):
                L.cv_create_tune_structs(req)
            ugc_store.versioned_create.assert_not_called()
            ugc_store.versioned_create.return_value = 5
            L.cv_create_tune_structs(dict(req, installedParts={"m_CarParts": self.SWAP}))
            ugc_store.versioned_create.assert_called_once()

    def test_valid_swap_and_unknown_cars_are_served_as_stored(self):
        swap = {"m_CarParts": self.SWAP}
        self.assertIs(L._buildable_tune_parts(1332, swap, 15394), swap)
        bad = {"m_CarParts": self.BAD}
        self.assertIs(L._buildable_tune_parts(999999, bad, 1), bad)
        short = {"m_CarParts": [1, 2]}
        self.assertIs(L._buildable_tune_parts(1332, short, 2), short)


class LspTests(unittest.TestCase):
    def setUp(self):
        # Handlers mutate scores, gamertags and UGC. Sharing one public schema
        # made unrelated tests inherit earlier runs and fail in different orders.
        test_db.fresh_db().close()
        L.TAGS.clear()
        L.LEVELS.clear()
        L.setup()
        self.addCleanup(L.DB.close)
        L.CTX.xuid = 0x0009000000000042
        L.TAGS[L.CTX.xuid] = "Tester"

    def test_bots_show_their_names_not_player_numbers(self):
        for nm in L.SB.names[:20]:
            tag = L.SOCIAL.profile(L.SB.fake_xuid(nm))["Gamertag"]
            self.assertEqual(tag, nm[:15])
            self.assertFalse(tag.startswith("Player"), tag)
        self.assertTrue(L.SOCIAL.profile(0x9000000ABCDE)["Gamertag"].startswith("Player"))          # an unknown id still gets a fallback

    def test_an_outdated_client_reads_how_to_update_in_its_message_center(self):
        out = L.outdated_notice("1.1.2")
        result, _error, n = struct.unpack(">III", out[:12])
        self.assertEqual(result, 0)
        _name, rf = L.find_struct("Xls.WebServices.Notifications", "RetrieveForUser", "_Results")
        res, rest = xls.decode(L.SC, rf, out[12 + n:])
        self.assertEqual(rest, 0)
        self.assertEqual([r["notificationType"] for r in res["results"]], [0x200])
        params, _ = xls.decode(L.SC, L.notif_fields(0x200), bytes(res["parameterBlock"]))
        self.assertIn("open the FH2MP launcher and press Play", params["Message"])
        self.assertIn("1.1.2", params["Message"])
        self.assertEqual(L.NOTIF.visible(0), L.NOTIF.visible(0))           # nothing was stored for anybody

    def test_a_banned_player_reads_the_end_date_and_reason_in_the_messages_tab(self):
        out = L.ban_notice(L.auth.ban_message("Cheating in Monthly Rivals", 1793646495))
        _result, _error, n = struct.unpack(">III", out[:12])
        _name, rf = L.find_struct("Xls.WebServices.Notifications", "RetrieveForUser", "_Results")
        res, rest = xls.decode(L.SC, rf, out[12 + n:])
        self.assertEqual(rest, 0)
        self.assertEqual([r["notificationType"] for r in res["results"]], [0x602])
        params, _ = xls.decode(L.SC, L.notif_fields(0x602), bytes(res["parameterBlock"]))
        self.assertIn("banned until 2026-11-02", params["Message"])
        self.assertIn("Cheating in Monthly Rivals", params["Message"])

    def retrieve(self, **limits):
        for i in range(limits.pop("rows")):
            L.NOTIF.add(f"news {i}", "", 0x200)
        out = L.notif_retrieve(limits)
        _name, rf = L.find_struct("Xls.WebServices.Notifications", "RetrieveForUser", "_Results")
        res, rest = xls.decode(L.SC, rf, xls.encode(L.SC, rf, out))
        self.assertEqual(rest, 0)
        self.assertEqual(sum(r["cbParameters"] for r in res["results"]), len(res["parameterBlock"]))
        return res, [r["id"] for r in res["results"]]

    def test_the_message_center_reply_keeps_to_the_count_the_client_asked_for(self):
        res, ids = self.retrieve(rows=8, maxResults=5, cbParameterBlockSize=L.NOTIF_MAX_BLOCK)
        self.assertEqual(len(ids), 5)
        self.assertEqual(ids, sorted(n["id"] for n in L.NOTIF.visible(L.CTX.xuid))[-5:])         # the newest, in id order
        self.assertEqual(list(res["activeNotificationIds"]), ids)

    def test_the_message_center_reply_keeps_to_the_bytes_the_client_asked_for(self):
        whole, ids = self.retrieve(rows=6, maxResults=L.NOTIF_MAX_RESULTS, cbParameterBlockSize=L.NOTIF_MAX_BLOCK)
        self.assertEqual(len(ids), 6)
        each = len(whole["parameterBlock"]) // 6
        res, kept = self.retrieve(rows=0, maxResults=L.NOTIF_MAX_RESULTS, cbParameterBlockSize=each * 2 + each // 2)
        self.assertEqual(kept, ids[-2:])
        self.assertLessEqual(len(res["parameterBlock"]), each * 2 + each // 2)
        self.assertEqual(list(res["activeNotificationIds"]), kept)
        fields = L.notif_fields(0x200)
        first, _ = xls.decode(L.SC, fields, bytes(res["parameterBlock"]))        # blocks stay in the order of their results
        self.assertEqual(first["Message"], "news 4")

    def test_a_notification_reply_is_reused_until_something_changes(self):
        L.NOTIF.add("first", "", 0x200)
        built = []
        real = L._notif_reply
        with mock.patch.object(L, "_notif_reply", lambda *a: built.append(a) or real(*a)):
            first = L.notif_retrieve({"maxResults": 512, "cbParameterBlockSize": 51200})
            self.assertEqual(L.notif_retrieve({"maxResults": 512, "cbParameterBlockSize": 51200})["activeNotificationIds"], first["activeNotificationIds"])
            self.assertEqual(len(built), 1)
            L.NOTIF.add("second", "", 0x200)
            after = L.notif_retrieve({"maxResults": 512, "cbParameterBlockSize": 51200})
        self.assertEqual(len(built), 2)
        self.assertEqual(len(after["activeNotificationIds"]), len(first["activeNotificationIds"]) + 1)
        self.assertEqual(after["nextRetrieveSeconds"], L.POLL_SECONDS)

    def test_the_poll_slows_once_nothing_has_changed_for_a_while(self):
        L.NOTIF.add("first", "", 0x200)
        L.notif_retrieve({})
        L._NOTIF_CHANGE[1] -= L.POLL_BUSY_WINDOW + 1
        self.assertEqual(L.notif_retrieve({})["nextRetrieveSeconds"], L.POLL_IDLE_SECONDS)

    def test_best_score_on_a_class_board_is_the_best_in_that_class(self):
        x = L.CTX.xuid

        def run(cls, seconds):
            return L.SB.submit(x, "Tester", 1, {"scoreboardType": 1, "gameScoreboardId": -1, "trackId": 300, "carId": 283, "carClassId": cls,
                                                "carPerformanceIndex": 0.5, "score": L.scoreboard.dbl_to_u64(seconds), "isClean": True,
                                                "usedSTM": False, "usedTCS": False, "usedABS": False})
        self.assertIsNotNone(run(4, 60.0))
        self.assertIsNotNone(run(1, 90.0))

        def best(cls):
            return L.sb_best({"scoreboardType": 1, "scoreType": 1, "gameScoreboardId": -1, "trackId": 300, "carId": 283, "carClassId": cls})
        self.assertEqual(L.scoreboard.u64_to_dbl(best(4)["bestScore"]["score"]), 60.0)
        self.assertEqual(L.scoreboard.u64_to_dbl(best(1)["bestScore"]["score"]), 90.0)
        self.assertFalse(best(2)["scoreFound"])                                  # S2 best no longer stops a first D-class run

    def test_handlers_accept_default_parameters_except_rival_notifications(self):
        failures = []
        for (cls, method) in sorted(L.HANDLERS):
            if method == "NotifyRival":          # needs real rival parameters (it queues a notification with typed fields)
                continue
            try:
                call(cls, method)
            except Exception as e:  # noqa: BLE001
                failures.append(f"{cls}.{method}: {type(e).__name__}: {e}")
        self.assertEqual(failures, [])

    def test_logon_creates_the_player(self):
        r, res = call("Forza.WebServices.User", "Logon", {"loginData": {"xuid": L.CTX.xuid, "gamerTag": "Tester",
                                                                       "currentCareerLevel": 7}})
        self.assertEqual(r, 0)
        row = L.DB.one("SELECT gamertag, career_level FROM players WHERE xuid=?", (L.CTX.xuid,))
        self.assertEqual((row["gamertag"], row["career_level"]), ("Tester", 7))

    def test_livery_share_and_download(self):
        # UGC search resolves creators through the durable player record. A real
        # client logs on before sharing; an in-memory TAGS entry isn't that flow.
        code, _ = call('Forza.WebServices.User', 'Logon', {'loginData': {
            'xuid': L.CTX.xuid, 'gamerTag': 'Tester', 'currentCareerLevel': 7}})
        self.assertEqual(code, 0)
        container = b"vlrc" + struct.pack("<II", 1, 325) + bytes(range(200))
        comp = zlib.compress(container)
        blob = struct.pack("<II", len(comp), len(container)) + comp
        name = b"Livery_0325_test"
        body = (struct.pack(">I", len(name)) + name + struct.pack(">IH", 325, 1) + b"\x00" + struct.pack(">Q", L.CTX.xuid)
                + struct.pack(">I", len(blob)) + blob)
        up = ugc.parse_livery_upload(body)
        self.assertEqual(up["payload"], blob)
        self.assertEqual(up["container"], container)
        L.profile_update_livery(body)
        vid = L.cv_create_livery({"name": name.decode(), "carId": 325, "LiveryThumbnail": b""})["versionedLiveryId"]
        v = L.UGC.versioned_get(vid)
        self.assertEqual(v["data"], blob)
        self.assertEqual(v["layers"], 1)
        r, res = call("Forza.WebServices.ContentVersioning", "RetrieveVersionedLiveryById", {"versionedLiveryId": vid})
        self.assertEqual(r, 0)
        self.assertEqual(bytes(res["versionedLiveryData"]["data"]), blob)
        fid = L.UGC.file_create("livery", L.CTX.xuid, "Stripe", "", 2, vid, 0, b"\x89PNG", None, 325, 21, 325, 3, 1, 11)
        self.assertEqual([f["id"] for f in L.UGC.search("livery", text="strip")], [fid])        # ILIKE
        self.assertIn(fid, [f["id"] for f in L.UGC.search("livery", gamertag="tester")])           # lower() match on the gamertag
        for method, params, key in (("GetFeaturedLiveriesForRandomCars", {"numCars": 5}, "liveryData"),
                                    ("GetFileHeavyWeightData", {"fileCount": 1, "fileIds": [fid]}, "fileData")):
            r, res = call("Forza.WebServices.Storefront", method, params)
            self.assertEqual([x["Ordinal"] for x in res[key] if x["Id"] == fid], [325], method)    # the car id: "DLC Car" when 0
            previews = [bytes(x["PreviewPayload"]) for x in res[key] if x["Id"] == fid]
            self.assertEqual(previews, [b"\x89PNG"], method)      # served unpacked: a packed blob made every tile blank
        L.UGC.bump(fid, "likes")
        self.assertEqual(L.UGC.file_get(fid)["likes"], 1)
        L.UGC.versioned_ref(vid, -5)
        self.assertEqual(L.UGC.versioned_get(vid)["refs"], 0)                                     # GREATEST(0, ...)

    def test_tune_share_search_and_download(self):
        S = "Forza.WebServices.Storefront"
        r, res = call("Forza.WebServices.ContentVersioning", "CreateVersionedTuneFromStructs", {"carId": 325})
        vid = res["versionedTuneId"]
        r, res = call("Forza.WebServices.StorefrontTuning", "Create", {
            "fileHeader": {"Title": "Grip tune", "Type": 0, "PayloadSize": 10, "KeywordOne": 1, "KeywordTwo": 3},
            "tuningDescription": {"base": {"VersionedTuneId": vid, "MakeId": 21, "ModelId": 325, "ClassId": 3, "PerformanceIndex": 700}},
            "fileName": "tune", "previewData": b"\x89PNG"})
        fid = res["contentFileId"]
        r, res = call(S, "SearchTuningFiles", {"searchParameters": {"ClassId": -1, "KeywordOneId": 0}, "maxFiles": 10})
        found = [x for x in res["searchResults"] if x["Id"] == fid]
        self.assertEqual([x["Ordinal"] for x in found], [325])                      # the car id, taken from the versioned tune
        r, res = call(S, "SearchTuningFiles", {"searchParameters": {"ClassId": 0}, "maxFiles": 10})
        self.assertEqual([x for x in res["searchResults"] if x["Id"] == fid], [])    # wrong class filtered out
        r, res = call(S, "GetFileHeavyWeightData", {"fileCount": 1, "fileIds": [fid]})
        self.assertEqual(res["fileData"][0]["Title"], "Grip tune")
        r, res = call("Forza.WebServices.ContentVersioning", "RetrieveVersionedTuneById", {"versionedTuneId": vid})
        self.assertEqual((r, res["versionedTuneData"]["CarId"]), (0, 325))
        r, res = call(S, "GetFileKeywords", {"fileType": 0, "maxKeywordResults": 50})
        self.assertIn("Drift", [k["Keyword"] for k in res["keywords"]])

    def test_keyword_lists_follow_fh4_in_two_pickers(self):
        S = "Forza.WebServices.Storefront"
        want = {0: (7, 11, "Top Speed", "Wide-Ratio Gearbox"), 1: (10, 11, "Logo", "Wide"), 2: (9, 10, "Racing", "Drift"),
                4: (5, 4, "Racing", "Untouched")}
        for ftype, (n1, n2, first, last) in want.items():
            r, res = call(S, "GetFileKeywords", {"fileType": ftype, "maxKeywordResults": 128})
            kws = res["keywords"]
            self.assertEqual([k["KeywordId"] for k in kws], list(range(1, n1 + n2 + 1)), ftype)
            self.assertEqual([k["Bucket"] for k in kws], [0] * n1 + [1] * n2, ftype)
            self.assertEqual((kws[0]["Keyword"], kws[-1]["Keyword"]), (first, last), ftype)

    def test_downloads_and_likes_count_once_per_other_player(self):
        S = "Forza.WebServices.Storefront"
        owner, other, third = L.CTX.xuid, 0x0009000000000043, 0x0009000000000044
        r, res = call("Forza.WebServices.ContentVersioning", "CreateVersionedTuneFromStructs", {"carId": 325})
        vid = res["versionedTuneId"]
        r, res = call("Forza.WebServices.StorefrontTuning", "Create", {
            "fileHeader": {"Title": "Grip tune", "Type": 0, "PayloadSize": 10, "KeywordOne": 3, "KeywordTwo": 13},
            "tuningDescription": {"base": {"VersionedTuneId": vid, "MakeId": 21, "ModelId": 325, "ClassId": 3, "PerformanceIndex": 700}},
            "fileName": "tune", "previewData": b"\x89PNG"})
        fid = res["contentFileId"]

        def counts():
            f = L.UGC.file_get(fid)
            return f["downloads"], f["likes"]
        call(S, "GetFileHeavyWeightData", {"fileCount": 1, "fileIds": [fid]})
        call(S, "SearchTuningFiles", {"searchParameters": {"ClassId": -1}, "maxFiles": 10})
        self.assertEqual(counts(), (0, 0))                                            # browsing is not downloading
        retrieve = lambda: call("Forza.WebServices.ContentVersioning", "RetrieveVersionedTuneById", {"versionedTuneId": vid})  # noqa: E731
        retrieve()
        call(S, "IncrementLikes", {"fileId": fid})
        self.assertEqual(counts(), (0, 0))                                            # the creator's own fetch and like do not count
        L.CTX.xuid = other
        retrieve()
        retrieve()
        call(S, "IncrementLikes", {"fileId": fid})
        call(S, "IncrementLikes", {"fileId": fid})
        self.assertEqual(counts(), (1, 1))                                            # once per player
        L.CTX.xuid = third
        retrieve()
        call(S, "IncrementLikes", {"fileId": fid})
        self.assertEqual(counts(), (2, 2))
        L.CTX.xuid = owner
        r, res = call(S, "GetMyFileList", {"fileType": 0, "maxFiles": 10})
        mine = [x for x in res["files"] if x["Id"] == fid][0]
        self.assertEqual((mine["DownloadCount"], mine["LikeCount"], mine["PopularityRating"]), (2, 2, ugc.popularity(L.UGC.file_get(fid))))
        self.assertEqual(mine["PopularityRating"], 2)                                 # score 2*3 + 2 = 8 -> two stars

    def test_a_design_seen_in_a_lobby_is_not_a_download(self):
        owner, other = L.CTX.xuid, 0x0009000000000043
        vid = L.UGC.versioned_create("livery", owner, "Stripe", 325, b"", layers=0)
        fid = L.UGC.file_create("livery", owner, "Stripe", "", 2, vid, 0, b"\x89PNG", None, 325)
        L.CTX.xuid = other
        with mock.patch.dict(os.environ, {"FH2MP_LIVERY_GATE_OFF": "1"}):
            call("Forza.WebServices.ContentVersioning", "RetrieveVersionedLiveryById", {"versionedLiveryId": vid, "returnThumbnail": True})
            self.assertEqual(L.UGC.file_get(fid)["downloads"], 0)
            call("Forza.WebServices.ContentVersioning", "RetrieveVersionedLiveryById", {"versionedLiveryId": vid, "returnThumbnail": False})
        self.assertEqual(L.UGC.file_get(fid)["downloads"], 1)

    def test_popular_files_come_first(self):
        owner = L.CTX.xuid
        quiet = L.UGC.file_create("livery", owner, "Quiet", "", 2, None, 0, b"x")
        loved = L.UGC.file_create("livery", owner, "Loved", "", 2, None, 0, b"x")
        for _ in range(3):
            L.UGC.bump(loved, "likes")
        self.assertEqual([f["id"] for f in L.UGC.search("livery")][:2], [loved, quiet])
        self.assertEqual([ugc.popularity({"likes": n, "uses": 0, "downloads": 0}) for n in (0, 1, 2, 7, 20, 50)], [0, 1, 2, 3, 4, 5])

    def test_vinyl_group_share_search_and_download(self):
        S = "Forza.WebServices.Storefront"
        owner, other = L.CTX.xuid, 0x0009000000000043
        name, blob = b"Flames", os.urandom(300)
        body = (struct.pack(">I", len(name)) + name + struct.pack(">H", 4) + b"\x00" + struct.pack(">Q", owner)
                + struct.pack(">I", len(blob)) + blob)
        self.assertEqual(ugc.parse_layergroup_upload(body), {"name": "Flames", "layers": 4, "payload": blob})
        self.assertIsNone(ugc.parse_layergroup_upload(body[:-1]))                     # shorter than it declares
        L.profile_update_layergroup(body)
        vid = L.cv_create_layergroup({"name": "Flames", "thumbnail": b"\x89PNG"})["versionedLayerGroupId"]
        v = L.UGC.versioned_get(vid)
        self.assertEqual((v["data"], v["layers"]), (blob, 4))
        r, res = call("Forza.WebServices.StorefrontLayerGroup", "Create", {
            "fileHeader": {"Title": "Flames", "Type": 1, "PayloadSize": len(blob), "KeywordOne": 2, "KeywordTwo": 13},
            "layerGroupDescription": {"base": {"VersionedLayerGroupId": vid, "NumberOfLayers": 4}},
            "fileName": "Flames", "previewData": b"\x89PNG"})
        self.assertEqual(r, 0)
        fid = res["contentFileId"]
        L.CTX.xuid = other
        for kw, found in ((0, True), (2, True), (3, False)):
            r, res = call(S, "SearchLayerGroupFiles", {"searchParameters": {"ClassId": -1, "KeywordOneId": kw}, "maxFiles": 10})
            self.assertEqual(fid in [x["Id"] for x in res["searchResults"]], found, kw)
        r, res = call("Forza.WebServices.ContentVersioning", "RetrieveVersionedLayerGroupById", {"versionedLayerGroupId": vid})
        d = res["versionedLayerGroupData"]
        self.assertEqual((bytes(d["data"]), d["numberOfLayers"], d["creatorXuid"]), (blob, 4, owner))
        self.assertEqual(L.UGC.file_get(fid)["downloads"], 1)

    def test_photo_share_upload_list_and_download(self):
        S = "Forza.WebServices.Storefront"
        X = "Xls.WebServices.Content"
        pic = os.urandom(600_000)
        r, res = call("Forza.WebServices.StorefrontPhoto", "Create", {
            "fileHeader": {"Title": "Sunset", "Description": "Nice", "Type": 1, "PayloadSize": len(pic), "KeywordOne": 2},
            "photoDescription": {"base": {"MakeId": 21, "ModelId": 325, "EnvironmentId": 1, "TrackId": 7}},
            "payloadSize": len(pic), "previewData": b"\x89PNGpreview"})
        self.assertEqual(r, 0)
        fid = res["contentFileId"]
        self.assertNotEqual(fid, 0)
        r, res = call(S, "GetPopularPhotos", {"maxFiles": 50})
        self.assertNotIn(fid, [x["Id"] for x in res["PopularPhotos"]])          # hidden until the upload is complete
        for off in range(0, len(pic), 262144):                                  # the client's chunk size, contentId = the file id
            r, res = call(X, "UploadContent", {"contentId": fid, "payloadOffset": off, "payloadTotalSize": len(pic),
                                                "uploadData": pic[off:off + 262144]})
            self.assertEqual((r, res["payloadUploadedSize"]), (0, min(off + 262144, len(pic))))
        L.CTX.xuid = 0x0009000000000043                                          # another player browses and downloads it
        for method, key in (("GetPopularPhotos", "PopularPhotos"), ("GetFeaturedPhotos", "FeaturedPhotos")):
            r, res = call(S, method, {"maxFiles": 50})
            mine = [x for x in res[key] if x["Id"] == fid]
            self.assertEqual([(x["Title"], bytes(x["PreviewPayload"]), x["PayloadSize"]) for x in mine],
                             [("Sunset", b"\x89PNGpreview", len(pic))], method)
        r, res = call(S, "SearchPhotoFiles", {"searchParameters": {"MakeId": 21, "ClassId": -1}, "maxFiles": 10})
        self.assertIn(fid, [x["Id"] for x in res["searchResults"]])
        r, res = call(S, "SearchPhotoFiles", {"searchParameters": {"MakeId": 22, "ClassId": -1}, "maxFiles": 10})
        self.assertNotIn(fid, [x["Id"] for x in res["searchResults"]])
        got, off = b"", 0
        while True:
            r, res = call(X, "DownloadContent", {"contentId": fid, "payloadOffset": off, "requestedSize": 262144})
            got += bytes(res["downloadData"])
            off = len(got)
            if res["isLastBuffer"]:
                break
        self.assertEqual((got, res["totalSize"]), (pic, len(pic)))
        r, _ = call(X, "UploadContent", {"contentId": fid, "payloadOffset": 0, "payloadTotalSize": len(pic), "uploadData": b"x"})
        self.assertNotEqual(r, 0)                                                # somebody else's photo cannot be overwritten

    def test_car_clubs(self):
        C = "Forza.WebServices.Club"
        K = "Forza.WebServices.CarClub"
        A, B, D, E = (0x0009000000001000 + i for i in range(4))
        with L.DB.tx() as c:
            for i, x in enumerate((A, B, D, E)):
                L.db.touch_player(c, x, f"Racer{i}", level=10 + i)
                L.TAGS[x] = f"Racer{i}"
        asx = lambda x: setattr(L.CTX, "xuid", x)
        notes = lambda x: [(n["type"], n["params"]) for n in L.NOTIF.visible(x)]

        asx(A)
        r, res = call(C, "CreateClub", {"clubTag": "AB", "clubName": "x", "privacy": 1})
        self.assertEqual((r, 2), (2, 2))                                        # tag shorter than 3: error result
        r, res = call(C, "CreateClub", {"clubTag": "ZOOM", "clubName": "Zoomers", "clubDescription": "fast", "privacy": 2,
                                        "regionKeywordId": 4, "attitudeKeywordId": 9, "interestKeywordId": 14})
        self.assertEqual(r, 0)
        cid = res["clubId"]
        r, _ = call(C, "CreateClub", {"clubTag": "zoom", "clubName": "Other"})
        self.assertEqual(r, 2)                                                 # tag exists (case-insensitive)
        r, res = call("Xls.WebServices.User", "GetUserData")                        # the client's own club tag / role after a change
        self.assertEqual((res["userData"]["clubTag"], res["userData"]["roleInClub"], res["userData"]["clubId"]), ("ZOOM", clubs_role("leader"), cid))
        r, res = call("Forza.WebServices.Scoreboard", "GetLadderRows", {"clubId": cid, "startAt": 1, "maxResults": 10, "maxTiers": 5})
        self.assertEqual((r, [x["user"]["qwXuid"] for x in res["rows"]], res["userRow"]["rank"], res["available"], len(res["ladderTiers"])),
                         (0, [A], 1, 1, 5))                                      # the club's members, ranked by XP earned in it
        self.assertEqual(res["rows"][0]["user"]["clubTag"], "ZOOM")
        th = [t["xpThreshold"] for t in res["ladderTiers"]]
        self.assertEqual((th, [t["tierNumber"] for t in res["ladderTiers"]]), (sorted(th, reverse=True), [1, 2, 3, 4, 5]))   # Tier 1 = highest
        r, res = call("Forza.WebServices.Scoreboard", "GetRows", {"scoreboardType": 8, "scoreType": 4, "view": 0, "isClubBoard": True,
                                                                  "startAt": 1, "maxResults": 20})
        self.assertEqual([(x["rank"], x["clubData"]["clubTag"], x["clubData"]["memberCount"]) for x in res["rows"]], [(1, "ZOOM", 1)])
        self.assertEqual((res["userRow"]["rank"], res["available"]), (1, 1))
        L.SB.submit(A, "Racer0", 1, {"scoreboardType": 1, "gameScoreboardId": -1, "trackId": 300, "carId": 283, "carClassId": 3,
                                     "carPerformanceIndex": 0.5, "score": L.scoreboard.dbl_to_u64(61.0), "isClean": True,
                                     "usedSTM": False, "usedTCS": False, "usedABS": False})
        r, res = call("Forza.WebServices.Scoreboard", "GetRows", {"scoreboardType": 1, "scoreType": 1, "trackId": 300, "pivotId": 3,
                                                                  "view": 0, "startAt": 1, "maxResults": 20})
        mine = [x["user"] for x in res["rows"] if x["user"]["qwXuid"] == A]
        self.assertEqual([(u["clubTag"], u["clubId"], u["roleInClub"]) for u in mine], [("ZOOM", cid, clubs_role("leader"))])   # the player boards show [TAG] too
        board = {"scoreboardType": 1, "scoreType": 1, "trackId": 300, "pivotId": 3, "view": 0, "startAt": 1, "maxResults": 20}
        r, res = call("Forza.WebServices.Scoreboard", "GetRowsAnthem", board)               # Rivals pages: the same rows, no handler error
        self.assertEqual((r, res["userRow"]["user"]["clubTag"]), (0, "ZOOM"))
        r, res = call("Forza.WebServices.Scoreboard", "GetRival", dict(board, rivalXuid=A))
        self.assertEqual((r, res["rival"]["scoreData"]["user"]["clubTag"]), (0, "ZOOM"))
        r, res = call(K, "SearchForClubByTag", {"clubTag": "zoom"})
        self.assertEqual(res["clubDetails"]["clubName"], "Zoomers")
        r, res = call(K, "SearchForClubsByName", {"clubName": "zoo", "maxResults": 5})
        self.assertEqual([x["clubTag"] for x in res["clubDetailsList"]], ["ZOOM"])

        asx(B)                                                                 # invite-only: a request, the leader approves
        r, res = call(C, "RequestJoiningClub", {"clubId": cid})
        self.assertEqual((r, res["requestApproved"]), (0, False))
        self.assertIn(0x700, [t for t, _ in notes(A)])
        asx(A)
        r, _ = call(C, "ApproveRequestToJoin", {"approvedUser": B})
        self.assertEqual(r, 0)
        self.assertIn(0x705, [t for t, _ in notes(B)])
        r, res = call(C, "GetClubMembers", {"clubId": cid, "maxMembers": 10})
        self.assertEqual((res["totalMembers"], [m["user"]["qwXuid"] for m in res["clubMembers"]]), (2, [A, B]))
        self.assertEqual(res["clubMembers"][1]["user"]["clubTag"], "ZOOM")

        r, _ = call(C, "InviteToClub", {"userToInvite": D})                      # invitation, accepted from the notification
        self.assertEqual(r, 0)
        inv = [n for n in L.NOTIF.visible(D) if n["type"] == 0x701]
        self.assertEqual(inv[0]["params"]["clubId"], cid)
        r, _ = call(C, "InviteToClub", {"userToInvite": D})
        self.assertEqual(r, 2)                                                 # already invited
        asx(D)
        r, _ = call(C, "AcceptClubInvite", {"clubId": cid, "invitationId": inv[0]["id"]})
        self.assertEqual(r, 0)
        self.assertEqual([n for n in L.NOTIF.visible(D) if n["type"] == 0x701], [])
        asx(B)
        r, _ = call(C, "InviteToClub", {"userToInvite": E})
        self.assertEqual(r, 2)                                                 # a plain member may not invite

        asx(A)                                                                 # roles
        r, _ = call(C, "ChangeMemberRole", {"userToChange": B, "newRole": clubs_role("co")})
        self.assertEqual(r, 0)
        self.assertIn(0x708, [t for t, _ in notes(B)])
        r, _ = call(C, "ChangeMemberRole", {"userToChange": B, "newRole": clubs_role("member")})
        self.assertIn(0x704, [t for t, _ in notes(B)])
        r, res = call(C, "CreateNoticeBoardNotification", {"noticeBoardMessage": "race at 9"})
        self.assertEqual(r, 0)
        self.assertIn(0x709, [t for t, _ in notes(B)])
        r, _ = call(C, "DeleteNoticeBoardNotification", {"notificationId": res["notificationId"]})
        self.assertNotIn(0x709, [t for t, _ in notes(B)])

        r, _ = call(C, "RemoveFromClub", {"userToRemove": D})                    # kick
        self.assertEqual(r, 0)
        self.assertIn(0x703, [t for t, _ in notes(D)])
        asx(B)
        r, _ = call(C, "LeaveClub")                                              # quit (the Leave Club button; RemoveFromClub on oneself does the same)
        self.assertEqual(r, 0)
        self.assertEqual(L.CLUBS.member_info(B), (0, "", 0))
        asx(A)
        r, _ = call(C, "LeaveClub")                                              # the game's Leave Club button: a leader leaving disbands the club
        self.assertEqual((r, L.CLUBS.club(cid)), (0, None))
        r, res = call(K, "GetRecommendedClubs", {"maxResults": 5})
        self.assertEqual(res["clubDetailsList"], [])

    def test_friends_profiles_and_gamerpics(self):
        A, B, C = (0x0009000000002000 + i for i in range(3))
        with L.DB.tx() as c:
            for i, x in enumerate((A, B, C)):
                L.db.touch_player(c, x, f"Friend{i}", level=5)
        hdr = {"Authorization": f"XBL3.0 x=fh2mp;{A}"}
        self.assertEqual(L.social_api("POST", "/api/friends", json.dumps({"a": hex(A), "b": B}).encode())[0], 201)
        self.assertEqual(L.social_api("POST", "/api/friends", json.dumps({"a": B, "b": A}).encode())[0], 201)        # idempotent, either order
        L.SOCIAL.add_friend(A, C)
        self.assertEqual(L.SOCIAL.friends(A), [B, C])
        self.assertEqual(L.social_api("POST", "/api/friends", json.dumps({"a": A, "b": A}).encode())[0], 400)
        st, obj = L.xbl_social("social.xboxlive.com", "GET", "/users/me/people", b"", hdr)
        self.assertEqual((st, [p["xuid"] for p in obj["people"]], obj["totalCount"]), (200, [str(B), str(C)], 2))
        st, obj = L.xbl_social("profile.xboxlive.com", "POST", "/users/batch/profile/settings",
                               json.dumps({"userIds": [str(B)], "settings": ["Gamertag", "GameDisplayPicRaw", "Gamerscore"]}).encode(), hdr)
        vals = {x["id"]: x["value"] for x in obj["profileUsers"][0]["settings"]}
        self.assertEqual((vals["Gamertag"], vals["Gamerscore"]), ("Friend1", "0"))
        self.assertIn(f"/gamerpic/{B}.png", vals["GameDisplayPicRaw"])
        st, obj = L.xbl_social("profile.xboxlive.com", "GET", "/users/me/profile/settings/people/people?settings=Gamertag,Gamerscore", b"", hdr)
        self.assertEqual([u["id"] for u in obj["profileUsers"]], [str(B), str(C)])
        png = L.SOCIAL.gamerpic(B)                                                  # no upload yet: a generated picture
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        up = L.pngtool.encode(2, 2, 3, [bytearray(b"\xff\x00\x00" * 2)] * 2)
        self.assertEqual(L.social_api("PUT", f"/api/players/{B}/gamerpic", up)[0], 200)
        self.assertEqual(L.social_api("GET", f"/api/players/{B}/gamerpic", b"")[2], up)
        self.assertEqual(L.social_api("PUT", f"/api/players/{B}/gamerpic", b"not a png")[0], 400)
        social_online = L.social.online
        L.social.touch(B)
        st, obj = L.xbl_social("userpresence.xboxlive.com", "POST", "/users/batch", json.dumps({"users": [str(B), str(C)]}).encode(), hdr)
        self.assertEqual([(p["xuid"], p["state"]) for p in obj], [(str(B), "Online"), (str(C), "Offline")])
        self.assertTrue(social_online(B))
        self.assertTrue(L.social_api("DELETE", "/api/friends", json.dumps({"a": A, "b": C}).encode())[1]["removed"])

    def test_scores_and_rivals(self):
        x = L.CTX.xuid
        for score in (95.5, 90.25):
            L.SB.submit(x, "Tester", 1, {"scoreboardType": 3, "gameScoreboardId": 1000, "trackId": 7, "carId": 325, "carClassId": 3,
                                         "carPerformanceIndex": 0.7, "score": L.scoreboard.dbl_to_u64(score), "isClean": True,
                                         "usedSTM": False, "usedTCS": False, "usedABS": True})
        self.assertEqual(L.SB.best_of(x, 3, 1, 7, 1000)["score"], 90.25)
        rows, mine, real, fake = L.SB.rows(x, 3, 1, 7, 1, 20, 1000)
        self.assertEqual(real, 1)
        self.assertEqual(mine["position"] > 0, True)
        self.assertAlmostEqual(L.SB.add_xp(x, 10.0)[0] - L.SB.add_xp(x, 0.0)[0], 0.0)

    def test_leaderboard_career_xp_survives_the_wire_and_restart(self):
        S = "Forza.WebServices.Scoreboard"
        xuid = L.CTX.xuid
        xp = 2410000  # stock curve: level 125, bronze wristband
        code, _ = call("Forza.WebServices.User", "Logon", {
            "loginData": {"xuid": xuid, "gamerTag": "Level125", "currentCareerLevel": xp}})
        self.assertEqual(code, 0)
        self.assertEqual(L.DB.one("SELECT career_level FROM players WHERE xuid=?", (xuid,))[0], xp)
        # The startup path reloads this persisted value into LEVELS.
        L.LEVELS.pop(xuid)
        L.LEVELS[xuid] = L.DB.one("SELECT career_level FROM players WHERE xuid=?", (xuid,))[0]
        for sb, st, pivot in ((9, 5, 27), (1, 1, 2), (3, 1, 1000)):
            d = dict(test_db.sub(50.0, sb=sb), gameScoreboardId=pivot)
            self.assertIsNotNone(L.SB.submit(xuid, "Level125", st, d))
            req = {"scoreboardType": sb, "scoreType": st, "trackId": 7, "pivotId": pivot,
                   "maxResults": 100, "startAt": 1}
            for method in ("GetRows", "GetRowsAnthem"):
                code, res = call(S, method, req)
                self.assertEqual(code, 0)
                mine = next(r for r in res["rows"] if r["user"]["qwXuid"] == xuid)
                self.assertEqual(mine["user"]["currentCareerLevel"], xp)
                self.assertEqual(res["userRow"]["user"]["currentCareerLevel"], xp)
            code, res = call(S, "GetRival", dict(req, rivalXuid=xuid))
            self.assertEqual(code, 0)
            self.assertEqual(res["rival"]["scoreData"]["user"]["currentCareerLevel"], xp)

    def test_stored_events_get_the_class_they_show_enforced(self):
        with L.DB.tx() as c:
            c.execute("DELETE FROM rival_events WHERE id=999001")
            c.execute("INSERT INTO rival_events (id, month, reroll, idx, track, laps, length, kind, attrs, cls, name, trackname, description, created) "
                      "VALUES (999001, 999, 0, 0, 849, 1, 2000.0, 'country', ?, 3, 'Made in Italy Cup', 'Nice West Circuit', '', 0)",
                      (json.dumps({"CountryId": 4}),))
        try:
            self.assertEqual(L.RIV.enforce_stored_classes(), 1)
            row = L.DB.one("SELECT attrs FROM rival_events WHERE id=999001")
            self.assertEqual(json.loads(row["attrs"]), {"CountryId": 4, "CarClassId": 3})
            self.assertEqual(L.RIV.enforce_stored_classes(), 0)            # nothing left to do
        finally:
            with L.DB.tx() as c:
                c.execute("DELETE FROM rival_events WHERE id=999001")

    def test_rival_themes_enforce_the_class_they_show(self):
        # GitHub #2: "Made in Italy" showed an A class card but allowed every Italian car
        used = [t for t in L.RIV.themes() if t["kind"] in L.rivals.CLASS_ENFORCED and not t.get("avoid")]
        self.assertTrue(used)
        for t in used:
            self.assertEqual(t["attrs"].get("CarClassId"), t["cls"], t["title"])
        for e in L.RIV._generate(5, 0):
            if e["kind"] in L.rivals.CLASS_ENFORCED:
                self.assertEqual(e["attrs"].get("CarClassId"), e["cls"], e["name"])

    def test_legacy_jaguar_class_is_enforced_with_only_one_stock_b_car(self):
        with L.DB.tx() as c:
            c.execute("INSERT INTO rival_events (id, month, reroll, idx, track, laps, length, kind, attrs, cls, name, trackname, description, created) "
                      "VALUES (999002, 999, 0, 1, 849, 1, 2000.0, 'make', ?, 2, 'Jaguar Showdown', 'Old Town Circuit', '', 0)",
                      (json.dumps({"MakeId": 19}),))
        try:
            self.assertEqual(L.RIV.enforce_stored_classes(), 1)
            e = L.RIV.by_id(999002)
            self.assertEqual((e["cls"], e["attrs"]), (2, {"MakeId": 19, "CarClassId": 2}))
            self.assertIn('CarClassId="2"', L.game_options_xml(e))
            self.assertEqual(L.RIV.enforce_stored_classes(), 0)
        finally:
            with L.DB.tx() as c:
                c.execute("DELETE FROM rival_events WHERE id=999002")

    def test_rivals_events_are_stored_once(self):
        ev = L.RIV.events()
        self.assertTrue(ev)
        self.assertEqual(L.DB.one("SELECT COUNT(*) FROM rival_events")[0] >= len(ev), True)
        L.RIV._cache = None
        self.assertEqual([e["id"] for e in L.RIV.events()], [e["id"] for e in ev])


if __name__ == "__main__":
    unittest.main()
