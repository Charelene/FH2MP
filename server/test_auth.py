"""Tests for the accountless identity (server/auth.py), the launcher API and the LSP's enforcement of session tokens:
FH2MP_PG_DSN=... python3 -I server/test_auth.py   (scratch database; `public` is dropped and recreated, like test_db.py / test_lsp.py)."""
import json
import os
import struct
import sys
import threading
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import auth  # noqa: E402
import launcher_api  # noqa: E402
import pngtool  # noqa: E402
import social  # noqa: E402
import test_db  # noqa: E402
import zlib  # noqa: E402

auth.POW_BASE_BITS = 8          # keep the tests quick


class Device:
    """What the launcher does, in a dozen lines: a key, a challenge, a proof of work, a signature."""

    def __init__(self, seed=None):
        self.sk = Ed25519PrivateKey.from_private_bytes(seed) if seed else Ed25519PrivateKey.generate()
        self.pub = self.sk.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)

    def solve(self, chal_raw, bits):
        n = 0
        while not auth.pow_ok(chal_raw, self.pub, n, bits):
            n += 1
        return n

    def sign(self, realm, purpose, chal_raw, *fields):
        return self.sk.sign(auth.sign_message(realm, purpose, chal_raw, *fields))

    def register_args(self, svc, tag, ip="203.0.113.5"):
        chal, bits, _ttl = svc.challenge(ip)
        raw = auth.unb64u(chal)
        return dict(ip=ip, challenge=chal, pubkey_b64=auth.b64u(self.pub), gamertag=tag, pow_counter=self.solve(raw, bits),
                    sig_b64=auth.b64u(self.sign(svc.env, "register", raw, self.pub, tag.encode())))

    def login_args(self, svc, ip="203.0.113.5"):
        chal, _bits, _ttl = svc.challenge(ip)
        raw = auth.unb64u(chal)
        return dict(ip=ip, challenge=chal, pubkey_b64=auth.b64u(self.pub), sig_b64=auth.b64u(self.sign(svc.env, "login", raw, self.pub)))


class Clock:
    def __init__(self):
        self.t = 1_800_000_000.0

    def __call__(self):
        return self.t


class AuthCase(unittest.TestCase):
    def setUp(self):
        self.db = test_db.fresh_db()
        self.clock = Clock()
        self.svc = auth.AuthService(self.db, env="test", mode="required", clock=self.clock)

    def tearDown(self):
        self.db.close()

    def code(self, fn, *a, **kw):
        with self.assertRaises(auth.AuthError) as cm:
            fn(*a, **kw)
        return cm.exception.code


class RegisterTests(AuthCase):
    def test_register_then_login(self):
        d = Device()
        acct = self.svc.register(**d.register_args(self.svc, "Speedy Gonzo"))
        self.assertEqual(acct.xuid & 0xFFFF000000000000, 0x0009000000000000)
        self.assertGreaterEqual(acct.xuid & 0xFFFFFFFF, 0x10000)
        token, a2, ttl = self.svc.login(**d.login_args(self.svc), client="launcher/1.0")
        self.assertEqual(a2.xuid, acct.xuid)
        self.assertTrue(token.startswith("fh2."))
        who = self.svc.authenticate(token, "203.0.113.5")
        self.assertEqual((who.xuid, who.gamertag), (acct.xuid, "Speedy Gonzo"))
        self.assertEqual(self.db.one("SELECT gamertag FROM players WHERE xuid=?", (acct.xuid,))[0], "Speedy Gonzo")

    def test_token_is_stored_only_as_a_hash(self):
        d = Device()
        self.svc.register(**d.register_args(self.svc, "Hashy"))
        token, _a, _t = self.svc.login(**d.login_args(self.svc))
        for (h,) in self.db.query("SELECT token_hash FROM auth_sessions"):
            self.assertNotIn(token.encode(), bytes(h))
            self.assertEqual(len(bytes(h)), 32)

    def test_challenge_is_single_use(self):
        d = Device()
        a = d.register_args(self.svc, "Replayer")
        self.svc.register(**a)
        self.assertEqual(self.code(self.svc.register, **a), "challenge_used")
        la = d.login_args(self.svc)
        self.svc.login(**la)
        self.assertEqual(self.code(self.svc.login, **la), "challenge_used")

    def test_challenge_expires(self):
        d = Device()
        a = d.register_args(self.svc, "Slowpoke")
        self.clock.t += auth.CHALLENGE_TTL + 1
        self.assertEqual(self.code(self.svc.register, **a), "challenge_expired")

    def test_forged_challenge_rejected(self):
        d = Device()
        a = d.register_args(self.svc, "Forger")
        raw = bytearray(auth.unb64u(a["challenge"]))
        raw[10] ^= 1
        a["challenge"] = auth.b64u(bytes(raw))
        self.assertEqual(self.code(self.svc.register, **a), "bad_challenge")

    def test_signature_must_cover_the_gamertag_and_the_realm(self):
        d = Device()
        a = d.register_args(self.svc, "Honest")
        a["gamertag"] = "Dishonest"
        self.assertEqual(self.code(self.svc.register, **a), "bad_signature")
        # a signature made for another realm (dev) is worthless here
        chal, bits, _ = self.svc.challenge("203.0.113.5")
        raw = auth.unb64u(chal)
        e = Device()
        args = dict(ip="203.0.113.5", challenge=chal, pubkey_b64=auth.b64u(e.pub), gamertag="Other", pow_counter=e.solve(raw, bits),
                    sig_b64=auth.b64u(e.sign("dev", "register", raw, e.pub, b"Other")))
        self.assertEqual(self.code(self.svc.register, **args), "bad_signature")

    def test_someone_elses_key_cannot_sign(self):
        victim, attacker = Device(), Device()
        self.svc.register(**victim.register_args(self.svc, "Victim"))
        chal, _b, _t = self.svc.challenge("198.51.100.9")
        raw = auth.unb64u(chal)
        args = dict(ip="198.51.100.9", challenge=chal, pubkey_b64=auth.b64u(victim.pub),
                    sig_b64=auth.b64u(attacker.sign("test", "login", raw, victim.pub)))
        self.assertEqual(self.code(self.svc.login, **args), "bad_signature")

    def test_proof_of_work_is_checked(self):
        d = Device()
        a = d.register_args(self.svc, "Lazy")
        raw = auth.unb64u(a["challenge"])
        a["pow_counter"] = next(n for n in range(10000) if not auth.pow_ok(raw, d.pub, n, 8))
        self.assertEqual(self.code(self.svc.register, **a), "bad_pow")

    def test_pow_gets_harder_under_load(self):
        for i in range(25):
            self.svc.register(**Device().register_args(self.svc, f"Racer{i:02d}x", ip=f"10.0.{i}.1"))
        self.assertGreater(self.svc.pow_bits(), 8)

    def test_gamertag_rules(self):
        for bad in ("ab", "x" * 16, "two  spaces", "emoji\U0001F600x", "semi;colon", "Admin", "Adm1n", "ADM-IN", "moderator1",
                    "xX_Staff_Xx", "Player123", "Turn_10", "hitler", "H1tler"):
            self.assertEqual(self.code(auth.validate_gamertag, bad), "bad_gamertag", bad)
        for good in ("abc", "Speedy Gonzo", "Drift-King_99", "x" * 15, "A.B.C", "Spicy Pepper", "Mike"):
            self.assertEqual(auth.validate_gamertag(good), good)
        self.assertEqual(auth.validate_gamertag("  padded  "), "padded")

    def test_gamertags_are_unique_after_folding(self):
        self.svc.register(**Device().register_args(self.svc, "Fast Eddie"))
        for dup in ("fast eddie", "FASTEDDIE", "Fast_Eddie", "F4st Eddie", "Fa5t.Edd1e"):
            self.assertEqual(self.code(self.svc.register, **Device().register_args(self.svc, dup)), "gamertag_taken", dup)

    def test_one_account_per_key(self):
        d = Device()
        self.svc.register(**d.register_args(self.svc, "First One"))
        self.assertEqual(self.code(self.svc.register, **d.register_args(self.svc, "Second One")), "key_exists")

    def test_per_ip_daily_limit(self):
        self.svc.per_ip_daily = 2
        for i in range(2):
            self.svc.register(**Device().register_args(self.svc, f"Limited{i}"))
        self.assertEqual(self.code(self.svc.register, **Device().register_args(self.svc, "Limited9")), "rate_limited")
        self.clock.t += 86401
        self.svc.register(**Device().register_args(self.svc, "Limited9"))          # a day later it works again

    def test_closed_and_invite_registration(self):
        self.svc.registration = "closed"
        self.assertEqual(self.code(self.svc.register, **Device().register_args(self.svc, "Nope")), "registration_closed")
        self.svc.registration = "invite"
        self.assertEqual(self.code(self.svc.register, **Device().register_args(self.svc, "NoCode")), "invite_required")
        self.svc.add_invite("beta-1234", uses=1)
        self.svc.register(**Device().register_args(self.svc, "WithCode"), invite="BETA-1234")
        self.assertEqual(self.code(self.svc.register, **Device().register_args(self.svc, "Reused"), invite="beta-1234"), "invite_required")

    def test_xuids_are_server_assigned_and_distinct(self):
        ids = {self.svc.register(**Device().register_args(self.svc, f"Unique{i:02d}", ip=f"10.1.{i}.1")).xuid for i in range(12)}
        self.assertEqual(len(ids), 12)


class SessionTests(AuthCase):
    def setUp(self):
        super().setUp()
        self.d = Device()
        self.acct = self.svc.register(**self.d.register_args(self.svc, "Session Guy"))

    def login(self, ip="203.0.113.5"):
        return self.svc.login(**self.d.login_args(self.svc, ip))[0]

    def test_unknown_key_cannot_log_in(self):
        self.assertEqual(self.code(self.svc.login, **Device().login_args(self.svc)), "unknown_account")

    def test_new_login_replaces_the_old_session(self):
        t1 = self.login()
        t2 = self.login()
        self.assertEqual(self.code(self.svc.authenticate, t1, "203.0.113.5"), "invalid_token")
        self.assertEqual(self.svc.authenticate(t2, "203.0.113.5").xuid, self.acct.xuid)

    def test_token_is_bound_to_the_address(self):
        t = self.login()
        self.assertEqual(self.code(self.svc.authenticate, t, "198.51.100.1"), "ip_changed")
        self.assertEqual(self.svc.authenticate(t, "::ffff:203.0.113.5").xuid, self.acct.xuid)       # v4-mapped form is the same address
        self.svc.token_ip = "off"
        self.assertEqual(self.svc.authenticate(t, "198.51.100.1").xuid, self.acct.xuid)

    def test_garbage_tokens(self):
        for bad in ("", "x", "fh2.", "fh2." + "A" * 43, "Bearer fh2.abc", "fh2." + "A" * 200):
            self.assertEqual(self.code(self.svc.authenticate, bad, "203.0.113.5"), "invalid_token")

    def test_idle_expiry_slides_with_use(self):
        t = self.login()
        for _ in range(4):                                    # use it every 10 h: never idle for 12 h
            self.clock.t += 10 * 3600
            self.svc.authenticate(t, "203.0.113.5")
        self.clock.t += 20 * 3600 + 1                          # now 60 h in and 20 h idle
        self.assertEqual(self.code(self.svc.authenticate, t, "203.0.113.5"), "token_expired")

    def test_absolute_expiry(self):
        t = self.login()
        for _ in range(8):
            self.clock.t += 10 * 3600
            try:
                self.svc.authenticate(t, "203.0.113.5")
            except auth.AuthError as e:
                self.assertEqual(e.code, "token_expired")
                self.assertGreaterEqual(self.clock.t - 1_800_000_000.0, auth.SESSION_MAX)
                return
        self.fail("the session never expired")

    def test_other_process_sees_expiry_after_cache(self):
        t = self.login()
        self.svc.authenticate(t, "203.0.113.5")
        self.db.one("SELECT 1")
        with self.db.tx() as c:
            c.execute("DELETE FROM auth_sessions")           # revoked behind this process's back
        self.assertEqual(self.svc.authenticate(t, "203.0.113.5").xuid, self.acct.xuid)       # cached for a moment
        self.clock.t += auth.CACHE_SECONDS + 1
        self.assertEqual(self.code(self.svc.authenticate, t, "203.0.113.5"), "invalid_token")

    def test_ban_kills_the_session_at_once_and_blocks_login(self):
        t = self.login()
        self.svc.authenticate(t, "203.0.113.5")
        self.svc.set_banned(self.acct.xuid, "cheating")
        self.assertEqual(self.code(self.svc.authenticate, t, "203.0.113.5"), "invalid_token")
        self.assertEqual(self.code(self.svc.login, **self.d.login_args(self.svc)), "banned")
        self.svc.set_banned(self.acct.xuid, None)
        self.login()

    def test_logout(self):
        t = self.login()
        self.svc.logout(t)
        self.assertEqual(self.code(self.svc.authenticate, t, "203.0.113.5"), "invalid_token")

    def test_rename_cooldown_and_uniqueness(self):
        other = Device()
        self.svc.register(**other.register_args(self.svc, "Taken Name"))
        self.assertEqual(self.svc.rename(self.acct.xuid, "Brand New"), "Brand New")
        self.assertEqual(self.code(self.svc.rename, self.acct.xuid, "Another"), "rename_cooldown")
        self.clock.t += auth.TAG_COOLDOWN + 1
        self.assertEqual(self.code(self.svc.rename, self.acct.xuid, "taken_name"), "gamertag_taken")
        self.assertEqual(self.svc.rename(self.acct.xuid, "BRAND NEW"), "BRAND NEW")       # re-casing your own name is allowed
        self.assertEqual(self.db.one("SELECT gamertag FROM players WHERE xuid=?", (self.acct.xuid,))[0], "BRAND NEW")

    def test_login_rate_limit(self):
        for _ in range(12):
            self.login()
        self.assertEqual(self.code(self.svc.login, **self.d.login_args(self.svc)), "rate_limited")


def make_png(w, h):
    rows = [bytes((x * 3 % 256, y * 5 % 256, 128, 255)[i % 4] for x in range(w) for i in range(4)) for y in range(h)]
    return pngtool.encode(w, h, 4, rows)


class ApiTests(AuthCase):
    def setUp(self):
        super().setUp()
        self.ctx = launcher_api.Ctx(self.svc, social.SocialStore(self.db), launcher_api.LauncherConfig(), lambda: 7,
                                    lsp_public={"url": "http://x"}, game_public={"host": "h", "port": 5000})
        self.d = Device()

    def call(self, method, path, body=b"", token=None, ip="203.0.113.5"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        h = {"Authorization": f"Bearer {token}"} if token else {}
        return launcher_api.handle(self.ctx, method, path, h, body, ip)

    def signup(self, tag="Api Guy"):
        a = self.d.register_args(self.svc, tag)
        st, obj, _ = self.call("POST", "/v1/auth/register", {"challenge": a["challenge"], "pubkey": a["pubkey_b64"], "gamertag": tag,
                                                              "pow": a["pow_counter"], "sig": a["sig_b64"]})
        self.assertEqual(st, 201, obj)
        st, ch, _ = self.call("POST", "/v1/auth/challenge")
        raw = auth.unb64u(ch["challenge"])
        st, obj, _ = self.call("POST", "/v1/auth/login", {"challenge": ch["challenge"], "pubkey": auth.b64u(self.d.pub),
                                                           "sig": auth.b64u(self.d.sign("test", "login", raw, self.d.pub)), "client": "t"})
        self.assertEqual(st, 200, obj)
        return obj["token"], obj["account"]

    def test_status_and_config_are_public(self):
        st, obj, _ = self.call("GET", "/v1/status")
        self.assertEqual((st, obj["online"], obj["env"]), (200, 7, "test"))
        st, obj, _ = self.call("GET", "/v1/launcher/config")
        self.assertEqual(st, 200)
        self.assertEqual(obj["links"]["website"], "https://forza.scuffed.af")
        self.assertEqual(obj["links"]["patreon"], "")
        self.assertEqual(obj["game"], {"host": "h", "port": 5000})

    def test_version_at_least(self):
        f = launcher_api.version_at_least
        self.assertTrue(all(f(h, "0.0.0") for h in ("", "junk", "1.0.0-rc1")))
        self.assertEqual([f(h, "1.0.0-rc4") for h in ("", "junk", "1.0.0-rc1", "1.0.0-rc3", "0.9.9")], [False] * 5)
        self.assertEqual([f(h, "1.0.0-rc4") for h in ("1.0.0-rc4", "1.0.0-rc10", "1.0.0", "1.0.1", "1.1.0-rc1", "1.0.0-rc2-debug")], [True] * 6)
        self.assertFalse(f("1.0.0-rc9", "1.0.0"))                 # a pre-release is older than its release

    def test_old_launcher_cannot_sign_in(self):
        a = self.d.register_args(self.svc, "Old Timer")
        st, _o, _ = self.call("POST", "/v1/auth/register", {"challenge": a["challenge"], "pubkey": a["pubkey_b64"], "gamertag": "Old Timer",
                                                            "pow": a["pow_counter"], "sig": a["sig_b64"]})
        self.assertEqual(st, 201)
        self.ctx.config.overlay = {"minLauncher": "1.0.0-rc4"}

        def login(client):
            st, ch, _ = self.call("POST", "/v1/auth/challenge")
            raw = auth.unb64u(ch["challenge"])
            return self.call("POST", "/v1/auth/login", {"challenge": ch["challenge"], "pubkey": auth.b64u(self.d.pub), "client": client,
                                                        "sig": auth.b64u(self.d.sign("test", "login", raw, self.d.pub))})[:2]
        st, obj = login("launcher/1.0.0-rc1 linux")
        self.assertEqual((st, obj["error"]), (426, "launcher_too_old"))
        self.assertIn("1.0.0-rc1", obj["message"])
        self.assertEqual(login("launcher/1.0.0-rc4 windows")[0], 200)
        self.assertEqual(login("dev_client.py")[0], 200)           # test tools are not launchers

    def test_config_file_is_reloaded(self):
        import tempfile
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump({"links": {"patreon": "https://example.invalid/p"}, "motd": "hello", "evil": 1}, f)
        try:
            cfg = launcher_api.LauncherConfig(f.name)
            self.assertEqual(cfg.get()["links"]["patreon"], "https://example.invalid/p")
            self.assertEqual(cfg.get()["links"]["website"], "https://forza.scuffed.af")
            self.assertNotIn("evil", cfg.get())
            with open(f.name, "w") as g:
                g.write("{ not json")                       # a broken edit keeps the last good copy
            os.utime(f.name, (1, 2))
            self.assertEqual(cfg.get()["motd"], "hello")
        finally:
            os.unlink(f.name)

    def test_me_needs_a_token(self):
        for m, p in (("GET", "/v1/me"), ("PATCH", "/v1/me"), ("PUT", "/v1/me/gamerpic"), ("POST", "/v1/auth/logout")):
            st, obj, _ = self.call(m, p)
            self.assertEqual((st, obj["error"]), (401, "invalid_token"), (m, p))

    def test_profile_and_gamerpic(self):
        token, acct = self.signup()
        st, me, _ = self.call("GET", "/v1/me", token=token)
        self.assertEqual((st, me["gamertag"], me["gamerpic"]), (200, "Api Guy", False))
        st, obj, _ = self.call("PUT", "/v1/me/gamerpic", make_png(64, 64), token=token)
        self.assertEqual(st, 200, obj)
        st, png, ctype = self.call("GET", f"/v1/gamerpic/{acct['xuid']}.png")
        self.assertEqual((st, ctype, png[:8]), (200, "image/png", b"\x89PNG\r\n\x1a\n"))
        self.assertEqual(pngtool.decode(png)[:2], (64, 64))
        self.assertTrue(self.call("GET", "/v1/me", token=token)[1]["gamerpic"])
        self.assertEqual(self.call("DELETE", "/v1/me/gamerpic", token=token)[0], 200)

    def test_gamerpic_is_validated(self):
        token, _ = self.signup()
        put = lambda b: self.call("PUT", "/v1/me/gamerpic", b, token=token)               # noqa: E731
        self.assertEqual(put(b"not a png")[1]["error"], "bad_image")
        self.assertEqual(put(make_png(16, 16))[1]["error"], "bad_image")                  # too small
        self.assertEqual(put(make_png(600, 40))[1]["error"], "bad_image")                 # too large
        # a decompression bomb: a tiny file declaring 512x512 whose data inflates to far more than that
        ihdr = struct.pack(">IIBBBBB", 512, 512, 8, 6, 0, 0, 0)

        def chunk(t, d):
            return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
        bomb = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(b"\0" * 50_000_000)) + chunk(b"IEND", b"")
        self.assertLess(len(bomb), 100_000)
        self.assertEqual(put(bomb)[1]["error"], "bad_image")
        huge = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 60000, 60000, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(b"\0" * 1000)) + chunk(b"IEND", b"")
        self.assertEqual(put(huge)[1]["error"], "bad_image")

    def test_rename_through_the_api(self):
        token, _ = self.signup()
        st, obj, _ = self.call("PATCH", "/v1/me", {"gamertag": "Renamed One"}, token=token)
        self.assertEqual((st, obj["gamertag"]), (200, "Renamed One"))
        st, obj, _ = self.call("PATCH", "/v1/me", {"gamertag": "Again Two"}, token=token)
        self.assertEqual((st, obj["error"]), (429, "rename_cooldown"))

    def test_gamertag_check(self):
        self.signup("Existing")
        self.assertEqual(self.call("GET", "/v1/gamertags/check?name=Brand%20New")[1], {"valid": True, "available": True, "reason": ""})
        self.assertFalse(self.call("GET", "/v1/gamertags/check?name=existing")[1]["available"])
        self.assertFalse(self.call("GET", "/v1/gamertags/check?name=admin")[1]["valid"])

    def test_malformed_requests_never_raise(self):
        for body in (b"", b"[]", b"{", b'{"challenge": 5}', b'{"challenge": "x", "pubkey": "!!", "sig": "", "gamertag": null, "pow": "a"}'):
            for p in ("/v1/auth/register", "/v1/auth/login"):
                st, obj, _ = self.call("POST", p, body)
                self.assertIn(st, (400, 401, 429), (p, body, obj))
        self.assertEqual(self.call("GET", "/v1/nope")[0], 404)


class HttpTests(unittest.TestCase):
    """The real LSP handler over a socket: tokens are required, the claimed X-UserId is ignored, admin paths are not reachable through the proxy."""

    @classmethod
    def setUpClass(cls):
        import fh2mp_lsp as L
        test_db.fresh_db().close()
        L.setup()
        cls.L = L
        L.AUTH.mode = "required"
        L.AUTH.token_ip = "exact"
        cls.srv = ThreadingHTTPServer(("127.0.0.1", 0), L.Handler)
        cls.port = cls.srv.server_address[1]
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()
        cls.d = Device()
        cls.token, cls.acct = cls.register(cls.d, "Http Guy")

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()

    @classmethod
    def req(cls, method, path, headers=None, body=b""):
        r = urllib.request.Request(f"http://127.0.0.1:{cls.port}{path}", data=body or None, method=method, headers=headers or {})
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, resp.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

    @classmethod
    def register(cls, d, tag):
        A = cls.L.AUTH
        _st, ch = cls.req("POST", "/v1/auth/challenge")
        ch = json.loads(ch)
        raw = auth.unb64u(ch["challenge"])
        sig = d.sign(A.env, "register", raw, d.pub, tag.encode())
        st, out = cls.req("POST", "/v1/auth/register", body=json.dumps({"challenge": ch["challenge"], "pubkey": auth.b64u(d.pub), "gamertag": tag,
                                                                         "pow": d.solve(raw, ch["powBits"]), "sig": auth.b64u(sig)}).encode())
        assert st == 201, out
        _st, ch = cls.req("POST", "/v1/auth/challenge")
        ch = json.loads(ch)
        raw = auth.unb64u(ch["challenge"])
        st, out = cls.req("POST", "/v1/auth/login", body=json.dumps({"challenge": ch["challenge"], "pubkey": auth.b64u(d.pub),
                                                                      "sig": auth.b64u(d.sign(A.env, "login", raw, d.pub))}).encode())
        assert st == 200, out
        out = json.loads(out)
        return out["token"], out["account"]

    def game_headers(self, **extra):
        return {"X-FH2MP-Host": "achievements.xboxlive.com", "X-UserId": "0x9000000001234", **extra}

    def test_game_traffic_needs_a_token(self):
        st, body = self.req("GET", "/titles/1/achievements", self.game_headers())
        self.assertEqual((st, json.loads(body)["error"]), (401, "login_required"))
        st, body = self.req("GET", "/titles/1/achievements", self.game_headers(**{"X-FH2MP-Token": "fh2.bogus"}))
        self.assertEqual((st, json.loads(body)["error"]), (401, "invalid_token"))
        st, _ = self.req("GET", "/titles/1/achievements", self.game_headers(**{"X-FH2MP-Token": self.token}))
        self.assertEqual(st, 404)                                           # reached the (fake) achievements handler

    def test_claimed_xuid_is_ignored(self):
        L = self.L
        L.social.LAST_POLL.pop(0x9000000001234, None)
        self.req("GET", "/titles/1/achievements", self.game_headers(**{"X-FH2MP-Token": self.token}))
        self.assertFalse(L.social.online(0x9000000001234))
        self.assertTrue(L.social.online(self.acct["xuid"]))

    def test_legacy_clients_only_when_optional(self):
        self.L.AUTH.mode = "optional"
        try:
            st, _ = self.req("GET", "/titles/1/achievements", self.game_headers())
            self.assertEqual(st, 404)
            st, body = self.req("GET", "/titles/1/achievements", self.game_headers(**{"X-FH2MP-Token": "fh2.bogus"}))
            self.assertEqual(st, 401)                                       # a bad token is an error, not a downgrade
        finally:
            self.L.AUTH.mode = "required"

    def test_token_from_another_address_is_refused(self):
        st, body = self.req("GET", "/titles/1/achievements", self.game_headers(**{"X-FH2MP-Token": self.token, "X-Real-IP": "198.51.100.77"}))
        self.assertEqual((st, json.loads(body)["error"]), (401, "ip_changed"))

    def test_admin_is_not_reachable_through_the_proxy(self):
        self.assertEqual(self.req("GET", "/api/players")[0], 200)
        self.assertEqual(self.req("GET", "/api/players", {"X-Real-IP": "198.51.100.77"})[0], 403)
        self.assertEqual(self.req("GET", "/admin/list", {"X-Forwarded-For": "198.51.100.77"})[0], 403)

    def test_status_over_http(self):
        st, body = self.req("GET", "/v1/status")
        self.assertEqual(st, 200)
        self.assertIn("online", json.loads(body))


if __name__ == "__main__":
    unittest.main(verbosity=1)
