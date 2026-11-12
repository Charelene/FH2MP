"""Headless tests of the launcher core. Server-facing tests need FH2MP_TEST_SERVER (http://127.0.0.1:port of a test LSP)."""
import hashlib
import json
import os
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from fh2mp_launcher import azimuth, core, identity, shim, updater  # noqa: E402
from fh2mp_launcher.settings import Settings  # noqa: E402
from cryptography.hazmat.primitives import serialization  # noqa: E402
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey  # noqa: E402

RIG = os.environ.get("FH2MP_TEST_AZIMUTH", "")


class IdentityTests(unittest.TestCase):
    def test_recovery_code_round_trip(self):
        a = identity.Identity.generate()
        code = a.recovery_code()
        self.assertTrue(code.startswith("FH2R1-"))
        b = identity.Identity.from_recovery_code(code.lower().replace("-", " "))
        self.assertEqual(a.pubkey, b.pubkey)

    def test_recovery_code_detects_typos(self):
        code = identity.Identity.generate().recovery_code()
        for i in (8, 20, 40):
            bad = code[:i] + ("B" if code[i] != "B" else "C") + code[i + 1:]
            with self.assertRaises(identity.RecoveryError):
                identity.Identity.from_recovery_code(bad)
        for junk in ("", "hello", "FH2R1-AAAA", code[:-6]):
            with self.assertRaises(identity.RecoveryError):
                identity.Identity.from_recovery_code(junk)

    def test_pow_and_signature_match_the_server_format(self):
        i = identity.Identity.generate()
        chal = os.urandom(41)
        n = identity.solve_pow(chal, i.pubkey, 10)
        self.assertTrue(identity.pow_ok(chal, i.pubkey, n, 10))
        # known-answer test shared with the LSP (native/internal/auth): a fixed seed must sign a fixed message identically
        j = identity.Identity(bytes(range(32)))
        msg = identity.sign_message("test", "login", b"\x01" * 41, j.pubkey)
        self.assertEqual(hashlib.sha256(msg).hexdigest(), hashlib.sha256(
            b"FH2MP-AUTH-v1\0test\0login\0" + b"\x01" * 41 + b"\x00\x20" + j.pubkey).hexdigest())
        self.assertEqual(len(j.sign("test", "login", b"\x01" * 41, j.pubkey)), 64)


NOW = 1_792_000_000


class HomeCardTests(unittest.TestCase):
    HOME = {"motd": "Restart at 18:00", "news": [{"tag": "patch notes", "title": "rc6", "url": "javascript:x", "image": "http://insecure/x.png"},
                                                 {"text": "no title"}],
            "heroImage": "http://insecure/h.png",
            "release": {"tag": "v1.0.0-rc5", "name": "FH2MP v1.0.0-rc5", "url": "https://github.com/Charelene/FH2MP/releases/tag/v1.0.0-rc5",
                        "body": "> **Release candidate 5.**\n\n## What's new\n### Added\n- The launcher tells you what to do when **Azimuth** "
                                "stops at [the GPU check](x)."},
            "azimuth": {"latest": "0.9.10", "supported": "0.9.10",
                        "posts": [{"title": "0.10.0 progress", "url": "https://x.itch.io/p", "date": NOW - 3 * 86400, "summary": "Soon."}]},
            "rotation": {"endsAt": NOW + 9 * 86400},
            "events": [{"name": "90s Heroes Cup", "class": "B", "track": "Sisteron Circuit", "km": 4.2, "laps": 3,
                        "rows": [{"position": 1, "gamertag": "Bob", "score": 88.25, "car": "Test Car"}]},
                       {"name": "Classics", "class": ""}],
            "records": [{"gamertag": "Bob", "event": "90s Heroes Cup", "score": 88.25, "car": "Test Car", "class": "B", "at": NOW - 7200}],
            "cars": [{"car": "Nissan Skyline", "runs": 34}, {"car": "Ford Focus", "runs": 12}],
            "stats": {"online": 50, "accounts": 1103, "clubs": 93, "runs": 29401, "designs": 80}}

    def lc(self, home=None, server=None, releases=(), stats=None):
        lc = core.Launcher.__new__(core.Launcher)
        lc.home, lc.server, lc.releases, lc.stats = home, server or {}, list(releases), stats or {}
        return lc

    def test_order_and_shapes(self):
        lc = self.lc(self.HOME)
        cards = lc.home_cards(now=NOW)
        self.assertEqual([(c["kind"], c["tag"], c["title"]) for c in cards],
                         [("news", "PATCH NOTES", "rc6"), ("news", "PATCH NOTES", "FH2MP v1.0.0-rc5"), ("challenge", "MONTHLY CHALLENGE", "90s Heroes Cup"),
                          ("record", "NEW RECORD", "Bob"), ("news", "AZIMUTH DEVLOG", "0.10.0 progress"), ("car", "CAR OF THE WEEK", "Nissan Skyline"),
                          ("community", "COMMUNITY", "1,103 drivers"), ("challenge", "MONTHLY CHALLENGE", "Classics")])
        news, rel, ev, rec, az, car, com, ev2 = cards
        self.assertEqual((news["url"], news["image"]), ("", ""))                                        # unsafe links dropped
        self.assertEqual((rel["text"], rel["glyph"]), ("The launcher tells you what to do when Azimuth stops at the GPU check.", ""))
        self.assertIn("Release candidate 5", rel["body"])
        self.assertEqual((ev["text"], ev["glyph"], ev["action"], ev["action_label"]), ("Best: Bob · 1:28.250", "B", "play", "Beat these times"))
        self.assertEqual(ev["facts"], [("Track", "Sisteron Circuit"), ("Length", "4.2 km"), ("Laps", "3"), ("Ends", "in 9 days")])
        self.assertEqual(ev["rows"], [{"pos": 1, "name": "Bob", "value": "1:28.250", "extra": "Test Car"}])
        self.assertEqual((ev2["text"], ev2["action_label"], ev2["rows"]), ("No times yet. Set the first one!", "Set the first time", []))
        self.assertEqual((rec["text"], rec["glyph"]), ("90s Heroes Cup · 1:28.250 · 2 h ago", "#1"))
        self.assertEqual((az["text"], az["url"], az["link"]), ("Soon.", "https://x.itch.io/p", "Read on itch.io"))
        self.assertEqual(car["rows"][1], {"pos": 2, "name": "Ford Focus", "value": "12 runs", "extra": ""})
        self.assertEqual(com["text"], "93 car clubs · 29,401 rivals times")
        self.assertIn(("Online now", "50"), com["facts"])
        self.assertEqual((lc.motd, lc.hero_image), ("Restart at 18:00", ""))
        self.assertEqual(self.lc({}).home_cards(), [])

    def test_older_server_falls_back_to_public_sources(self):
        lc = self.lc(None, {"motd": "Hi", "latestAzimuth": "0.9.10", "online": 50}, [self.HOME["release"]],
                     {"available": True, "accounts": 1103, "clubs": 93, "scores": 29401, "userContent": 80})
        cards = lc.home_cards()
        self.assertEqual([c["tag"] for c in cards], ["PATCH NOTES", "COMMUNITY"])
        self.assertIn(("Online now", "50"), cards[1]["facts"])
        self.assertEqual(lc.motd, "Hi")

    def test_formatting(self):
        self.assertEqual([core.countdown(t, now=0) for t in (None, -5, 30, 600, 7200, 3 * 86400)],
                         ["", "", "ends in 1 min", "ends in 10 min", "ends in 2 h", "ends in 3 days"])
        self.assertEqual([core.lap_time(x) for x in (None, 0, 59.5, 88.25, 3725.0)], ["", "", "0:59.500", "1:28.250", "62:05.000"])
        self.assertEqual([core.ago(NOW - d, now=NOW) for d in (5, 300, 7200, 3 * 86400)], ["just now", "5 min ago", "2 h ago", "3 days ago"])
        self.assertTrue(core.release_summary("- " + "x" * 300).endswith("…"))


class TcpFallbackTests(unittest.TestCase):
    """Launcher.start_tunnel: off by default; on, the game is pointed at a local proxy."""

    def me(self, on, auto=False):
        from types import SimpleNamespace
        from fh2mp_launcher import core
        me = SimpleNamespace(settings={"tcp_fallback": on, "tcp_auto": auto}, tunnel=None, game_env={"FH2MP_SERVER_PORT": "5000"})
        me.stop_tunnel = lambda: core.Launcher.stop_tunnel(me)
        return me

    def test_off_changes_nothing(self):
        from fh2mp_launcher import core
        me = self.me(False)
        self.assertEqual(core.Launcher.start_tunnel(me, {"host": "h", "port": 5000, "tcp": 5000}), "")
        self.assertEqual(me.game_env, {"FH2MP_SERVER_PORT": "5000"})
        self.assertIsNone(me.tunnel)

    def test_a_server_without_tcp_keeps_udp(self):
        from fh2mp_launcher import core
        me = self.me(True)
        note = core.Launcher.start_tunnel(me, {"host": "h", "port": 5000})
        self.assertIn("using UDP", note)
        self.assertEqual(me.game_env, {"FH2MP_SERVER_PORT": "5000"})

    def test_on_points_the_game_at_the_local_proxy(self):
        from fh2mp_launcher import core
        me = self.me(True)
        try:
            note = core.Launcher.start_tunnel(me, {"host": "127.0.0.1", "port": 5000, "tcp": 5000})
            self.assertEqual(note, "connecting over TCP")
            self.assertEqual(me.game_env["FH2MP_TUNNEL"], "1")
            self.assertEqual(me.game_env["FH2MP_SERVER_PORT"], str(me.tunnel.port))
            self.assertNotEqual(me.game_env["FH2MP_SERVER_PORT"], "5000")
        finally:
            core.Launcher.stop_tunnel(me)
        self.assertIsNone(me.tunnel)


    def test_automatic_stays_on_udp_when_the_server_answers_the_probe(self):
        import socket
        import threading
        from fh2mp_launcher import core
        echo = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        echo.bind(("127.0.0.1", 0))
        echo.settimeout(5)
        self.addCleanup(echo.close)

        def serve():
            try:
                data, addr = echo.recvfrom(256)
                echo.sendto(data, addr)             # what the game server's probe does (native/internal/game/side.go)
            except OSError:
                pass
        threading.Thread(target=serve, daemon=True).start()
        me = self.me(False, auto=True)
        game = {"host": "127.0.0.1", "port": echo.getsockname()[1], "tcp": 5000, "probe": 1}
        self.assertEqual(core.Launcher.start_tunnel(me, game), "")
        self.assertIsNone(me.tunnel)
        self.assertEqual(me.game_env, {"FH2MP_SERVER_PORT": "5000"})

    def test_automatic_switches_to_tcp_when_the_probe_gets_no_answer(self):
        from unittest import mock
        from fh2mp_launcher import core, tunnel
        me = self.me(False, auto=True)
        game = {"host": "127.0.0.1", "port": 5000, "tcp": 5000, "probe": 1}
        try:
            with mock.patch.object(tunnel, "probe_udp", return_value=False) as probe:
                note = core.Launcher.start_tunnel(me, game)
            probe.assert_called_once_with("127.0.0.1", 5000)
            self.assertIn("blocks UDP", note)
            self.assertEqual(me.game_env["FH2MP_TUNNEL"], "1")
        finally:
            core.Launcher.stop_tunnel(me)

    def test_automatic_never_probes_a_server_that_does_not_answer_probes_or_offers_no_tcp(self):
        from unittest import mock
        from fh2mp_launcher import core, tunnel
        with mock.patch.object(tunnel, "probe_udp", side_effect=AssertionError("probed")):
            for game in ({"host": "h", "port": 5000, "tcp": 5000},                     # an older server: silence would mean nothing
                         {"host": "h", "port": 5000, "probe": 1},                       # nothing to switch to
                         {"host": "h", "port": 5000, "tcp": 5000, "probe": True}):      # only exactly 1 is the advertisement
                me = self.me(False, auto=True)
                self.assertEqual(core.Launcher.start_tunnel(me, game), "")
                self.assertIsNone(me.tunnel)
        me = self.me(False, auto=False)                                                 # the setting turned off
        with mock.patch.object(tunnel, "probe_udp", side_effect=AssertionError("probed")):
            self.assertEqual(core.Launcher.start_tunnel(me, {"host": "h", "port": 5000, "tcp": 5000, "probe": 1}), "")

    def test_the_probe_helper_matches_the_servers_framing(self):
        """The game server (native/internal/game/side.go probeHandle) answers a probe only when it is exactly 11 + 8 + 8 bytes with the magic at
        bytes 11..19, and answers with the same bytes. This stand-in applies that rule, so the helper's framing stays pinned to the server's."""
        import socket
        import threading
        from fh2mp_launcher import tunnel
        SERVER_MAGIC, SERVER_LENGTH = b"FH2MPPN1", 11 + 8 + 8        # side.go: probeMagic, probeLength
        self.assertEqual(tunnel.PROBE_MAGIC, SERVER_MAGIC)
        srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        srv.bind(("127.0.0.1", 0))
        srv.settimeout(5)
        self.addCleanup(srv.close)

        def serve():
            try:
                data, addr = srv.recvfrom(256)
                if len(data) == SERVER_LENGTH and data[11:19] == SERVER_MAGIC:
                    srv.sendto(data, addr)           # the server's rule: the same bytes back, nothing else
            except OSError:
                pass
        threading.Thread(target=serve, daemon=True).start()
        self.assertTrue(tunnel.probe_udp("127.0.0.1", srv.getsockname()[1], tries=1, wait=2.0))
        dead = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        dead.bind(("127.0.0.1", 0))
        self.addCleanup(dead.close)
        self.assertFalse(tunnel.probe_udp("127.0.0.1", dead.getsockname()[1], tries=1, wait=0.2))        # nobody answers


class GamerpicSyncTests(unittest.TestCase):
    """Account pictures reach the version's actual file, without repeated writes."""

    def test_native_picture_is_cropped_and_synced_without_extra_options(self):
        from PySide6.QtGui import QImage, QColor
        from fh2mp_launcher.ui.images import png_bytes
        with tempfile.TemporaryDirectory() as folder, tempfile.TemporaryDirectory() as cfg:
            with open(os.path.join(folder, "azimuth.exe"), "wb") as f:
                f.write(b"MZ native build by 1.0.1\0")
            os.makedirs(os.path.join(folder, "data"))
            target = os.path.join(folder, "data", "player.png")
            with open(target, "wb") as f:
                f.write(b"original native picture")
            with open(os.path.join(folder, "data", "player-frame.txt"), "wb") as f:
                f.write(b"user crop settings")
            image = QImage(300, 100, QImage.Format.Format_RGBA8888)
            for x in range(300):
                for y in range(100):
                    image.setPixelColor(x, y, QColor("red" if x < 100 else "green" if x < 200 else "blue"))
            raw = png_bytes(image)
            me, _unused = self.fake(folder, api_png=raw)
            me.gamerpic_converter = lambda _raw: self.fail("Native sync must not use the legacy 1080 converter")
            with mock.patch("fh2mp_launcher.paths.config_dir", return_value=cfg):
                self.assertIn("updated", core.Launcher.sync_gamerpic(me))
                decoded = QImage(target)
                self.assertEqual((decoded.width(), decoded.height()), (256, 256))
                self.assertEqual(decoded.pixelColor(0, 0), QColor("green"))
                self.assertEqual(decoded.pixelColor(255, 255), QColor("green"))
                stamp = os.stat(target).st_mtime_ns
                self.assertEqual(core.Launcher.sync_gamerpic(me), "")
                self.assertEqual(os.stat(target).st_mtime_ns, stamp)
            self.assertFalse(os.path.exists(os.path.join(folder, "Gamerpic.png")))
            with open(target + ".before-fh2mp", "rb") as f:
                self.assertEqual(f.read(), b"original native picture")
            with open(os.path.join(folder, "data", "player-frame.txt"), "rb") as f:
                self.assertEqual(f.read(), b"user crop settings")
            with self.assertRaises(ValueError):
                azimuth.write_gamerpic(folder, b"invalid image")
            self.assertEqual(os.stat(target).st_mtime_ns, stamp)
            os.remove(target + ".before-fh2mp")
            image.fill(QColor("blue"))
            with mock.patch("fh2mp_launcher.paths.atomic_write", side_effect=FileNotFoundError("backup failed")):
                with self.assertRaises(FileNotFoundError):
                    azimuth.write_gamerpic(folder, png_bytes(image))
            self.assertEqual(os.stat(target).st_mtime_ns, stamp)

    def test_native_picture_uses_cached_image_when_server_is_unavailable(self):
        from PySide6.QtGui import QImage, QColor
        from fh2mp_launcher.ui.images import png_bytes
        with tempfile.TemporaryDirectory() as folder:
            with open(os.path.join(folder, "azimuth.exe"), "wb") as f:
                f.write(b"MZ native build by 1.0.0\0")
            image = QImage(1080, 1080, QImage.Format.Format_RGB888)
            image.fill(QColor("blue"))
            me, cfg = self.fake(folder, fail=True, kept=png_bytes(image))
            with mock.patch("fh2mp_launcher.paths.config_dir", return_value=cfg):
                self.assertIn("updated", core.Launcher.sync_gamerpic(me))
            decoded = QImage(os.path.join(folder, "data", "player.png"))
            self.assertEqual((decoded.width(), decoded.height()), (256, 256))
            self.assertEqual(decoded.pixelColor(128, 128), QColor("blue"))

    def fake(self, folder, api_png=b"PNGDATA" * 20, fail=False, kept=None):
        from types import SimpleNamespace
        cfg = tempfile.mkdtemp()
        if kept:
            with open(os.path.join(cfg, "gamerpic.png"), "wb") as f:
                f.write(kept)

        class Api:
            def gamerpic(self, xuid):
                if fail:
                    raise OSError("offline")
                return api_png
        me = SimpleNamespace(settings={"azimuth_dir": folder}, account={"xuid": 7}, gamerpic_converter=None, api=lambda: Api())
        return me, cfg

    def test_writes_the_picture_then_leaves_it_alone(self):
        from fh2mp_launcher import core, paths
        folder = tempfile.mkdtemp()
        me, cfg = self.fake(folder)
        real = paths.config_dir
        paths.config_dir = lambda: cfg
        try:
            self.assertIn("updated", core.Launcher.sync_gamerpic(me))
            with open(os.path.join(folder, "Gamerpic.png"), "rb") as f:
                self.assertEqual(f.read(), b"PNGDATA" * 20)
            self.assertFalse(os.path.exists(os.path.join(folder, "fh2extra")))
            self.assertEqual(core.Launcher.sync_gamerpic(me), "")          # nothing to do the second time
        finally:
            paths.config_dir = real

    def test_offline_uses_the_kept_copy(self):
        from fh2mp_launcher import core, paths
        folder = tempfile.mkdtemp()
        me, cfg = self.fake(folder, fail=True, kept=b"KEPT" * 30)
        real = paths.config_dir
        paths.config_dir = lambda: cfg
        try:
            self.assertIn("updated", core.Launcher.sync_gamerpic(me))
            with open(os.path.join(folder, "Gamerpic.png"), "rb") as f:
                self.assertEqual(f.read(), b"KEPT" * 30)
        finally:
            paths.config_dir = real


class UpdaterTests(unittest.TestCase):
    def _portable(self, members):
        import tempfile
        import zipfile
        d = tempfile.mkdtemp()
        here = os.path.join(d, "FH2MP")
        os.makedirs(here)
        exe = os.path.join(here, "fh2mp-launcher.exe")
        open(exe, "wb").close()
        zp = os.path.join(d, "p.zip")
        with zipfile.ZipFile(zp, "w") as z:
            for n in members:
                z.writestr(n, "x")
        return d, here, exe, zp

    def test_portable_flavour_has_no_uninstaller(self):
        d, here, exe, _zp = self._portable(["fh2mp-launcher.exe"])
        real = (sys.platform, sys.executable, getattr(sys, "frozen", None))
        sys.platform, sys.executable, sys.frozen = "win32", exe, True
        try:
            self.assertEqual(updater.launcher_flavour(), "windows-x64-portable")
            open(os.path.join(here, "unins000.exe"), "wb").close()
            self.assertEqual(updater.launcher_flavour(), "windows-x64-installer")
        finally:
            sys.platform, sys.executable = real[0], real[1]
            del sys.frozen

    def test_portable_update_stages_the_zip_and_hands_over_to_a_script(self):
        import subprocess
        d, here, exe, zp = self._portable(["fh2mp-launcher.exe", "_internal/x.dll"])
        launched = []
        real = (subprocess.Popen, sys.executable)
        subprocess.Popen = lambda *a, **k: launched.append(a[0])
        sys.executable = exe
        try:
            self.assertEqual(updater._apply_portable_zip(zp), "exit")
            script = launched[0][-1]
            text = open(script).read()
            stage = os.path.dirname(script)
            self.assertTrue(os.path.isfile(os.path.join(stage, "_internal", "x.dll")))
            self.assertIn(f'robocopy "{stage}" "{here}"', text)
            _d2, _h2, _e2, bad = self._portable(["../evil.txt", "fh2mp-launcher.exe"])
            with self.assertRaises(updater.UpdateError):
                updater._apply_portable_zip(bad)
            _d3, _h3, _e3, nolauncher = self._portable(["other.txt"])
            with self.assertRaises(updater.UpdateError):
                updater._apply_portable_zip(nolauncher)
        finally:
            subprocess.Popen, sys.executable = real

    def test_a_distrusted_system_store_falls_back_to_the_bundled_roots(self):
        import ssl
        import urllib.error
        import urllib.request
        calls = []
        verify_error = ssl.SSLCertVerificationError("unable to get local issuer certificate")

        class Reply:
            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

            def read(self, n):
                return b"{}"

        def fake_urlopen(req, timeout=None, context=None):
            calls.append(context)
            if len(calls) == 1:
                raise urllib.error.URLError(verify_error)
            return Reply()
        real = urllib.request.urlopen
        urllib.request.urlopen = fake_urlopen
        try:
            up = updater.Updater()
            self.assertEqual(up._get("https://api.github.com/x", 100), b"{}")
        finally:
            urllib.request.urlopen = real
        self.assertEqual(len(calls), 2)
        self.assertIsNot(calls[0], calls[1])          # the retry used the bundled roots (the AppImage's OpenSSL does not trust the system store)

    def setUp(self):
        self.sk = Ed25519PrivateKey.generate()
        import base64
        self.pub = base64.b64encode(self.sk.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)).decode()
        self.b64 = base64.b64encode

    def manifest(self, **kw):
        m = {"schema": 1, "tag": "v1.2.3", "version": "1.2.3", "shim": {"version": "1.2.3", "name": "x.zip", "sha256": "0" * 64, "size": 1}}
        m.update(kw)
        raw = json.dumps(m).encode()
        return raw, self.b64(self.sk.sign(raw)).decode()

    def test_good_signature(self):
        raw, sig = self.manifest()
        self.assertEqual(updater.verify_manifest(raw, sig, self.pub)["tag"], "v1.2.3")

    def test_tampered_or_foreign_manifest_rejected(self):
        raw, sig = self.manifest()
        with self.assertRaises(updater.UpdateError):
            updater.verify_manifest(raw.replace(b"1.2.3", b"1.2.4"), sig, self.pub)
        other = Ed25519PrivateKey.generate()
        with self.assertRaises(updater.UpdateError):
            updater.verify_manifest(raw, self.b64(other.sign(raw)).decode(), self.pub)
        with self.assertRaises(updater.UpdateError):
            updater.verify_manifest(raw, sig, "")           # a build without a release key accepts nothing

    def test_bad_tag_or_schema_rejected(self):
        for kw in ({"tag": "../../evil"}, {"tag": "v1;rm"}, {"schema": 2}):
            raw, sig = self.manifest(**kw)
            with self.assertRaises(updater.UpdateError):
                updater.verify_manifest(raw, sig, self.pub)

    def test_asset_urls_are_built_not_taken(self):
        u = updater.Updater(pubkey_b64=self.pub)
        self.assertEqual(u.asset_url("v1.2.3", "a.zip"), "https://github.com/Charelene/FH2MP/releases/download/v1.2.3/a.zip")
        for bad in ("../x", "a/b", "a b", "", ".hidden"):
            with self.assertRaises(updater.UpdateError):
                u.asset_url("v1.2.3", bad)

    def test_version_order(self):
        self.assertTrue(updater.newer("0.2.0", "0.1.9"))
        self.assertTrue(updater.newer("1.0.0", "1.0.0-rc1"))
        self.assertFalse(updater.newer("1.0.0", "1.0.0"))
        self.assertFalse(updater.newer("0.9.9", "0.10.0"))
        self.assertTrue(updater.newer("1.0.0-rc10", "1.0.0-rc9"))

    def test_at_least(self):
        self.assertTrue(all(updater.at_least(h, "0.0.0") for h in ("", "junk")))
        self.assertEqual([updater.at_least(h, "1.0.0-rc4") for h in ("", "1.0.0-rc1", "1.0.0-rc3", "0.9.9")], [False] * 4)
        self.assertEqual([updater.at_least(h, "1.0.0-rc4") for h in ("1.0.0-rc4", "1.0.0-rc10", "1.0.0", "1.0.0-rc2-debug")], [True] * 4)

    def test_certificate_fallback_covers_the_download_and_later_updaters(self):
        import ssl
        import urllib.error
        import urllib.request
        from unittest import mock
        bundled = object()
        seen = []

        class Resp:
            def __init__(self, body):
                self.body = body

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

            def read(self, n=-1):
                b, self.body = self.body, b""
                return b

        def fake(req, timeout=None, context=None):
            seen.append(context)
            if context is not bundled:
                raise urllib.error.URLError(ssl.SSLCertVerificationError("unable to get local issuer certificate"))
            return Resp(b"payload")
        data = b"payload"
        entry = {"name": "f.bin", "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}
        with mock.patch.object(updater, "_bundled_context", lambda: bundled), mock.patch.object(urllib.request, "urlopen", fake), \
                mock.patch.object(updater, "_USE_BUNDLED", None), tempfile.TemporaryDirectory() as d:
            self.assertEqual(updater.Updater(base="https://x.invalid")._get("https://x.invalid/a", 100), b"payload")     # the manifest check
            n = len(seen)
            u2 = updater.Updater(base="https://x.invalid")                      # the shim download makes a new Updater
            with open(u2.download("v1.0.0", entry, dest_dir=d), "rb") as f:
                self.assertEqual(f.read(), data)
            self.assertEqual(seen[n:], [bundled])                              # straight to the bundled roots, no failed first attempt
        with mock.patch.object(updater, "_bundled_context", lambda: bundled), mock.patch.object(urllib.request, "urlopen", fake), \
                mock.patch.object(updater, "_USE_BUNDLED", None), tempfile.TemporaryDirectory() as d:
            seen.clear()                                                        # a download with no earlier request retries by itself
            p = updater.Updater(base="https://x.invalid").download("v1.0.0", dict(entry, name="g.bin"), dest_dir=d)
            self.assertTrue(os.path.isfile(p))
            self.assertEqual(len(seen), 2)

    def test_download_checks_hash(self):
        import http.server
        import threading
        data = os.urandom(50000)
        good = hashlib.sha256(data).hexdigest()

        class H(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                body = data if "ok.bin" in self.path else data[:-1] + bytes([data[-1] ^ 1])
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass
        srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        self.addCleanup(srv.server_close)
        u = updater.Updater(pubkey_b64=self.pub, base=f"http://127.0.0.1:{srv.server_address[1]}")
        with tempfile.TemporaryDirectory() as d:
            p = u.download("v1.2.3", {"name": "ok.bin", "sha256": good, "size": len(data)}, dest_dir=d)
            with open(p, "rb") as f:
                self.assertEqual(f.read(), data)
            with self.assertRaises(updater.UpdateError):
                u.download("v1.2.3", {"name": "evil.bin", "sha256": good, "size": len(data)}, dest_dir=d)
            self.assertFalse(os.path.exists(os.path.join(d, "evil.bin")))
            with self.assertRaises(updater.UpdateError):
                u.download("v1.2.3", {"name": "ok.bin", "sha256": good, "size": 10}, dest_dir=tempfile.mkdtemp())
        srv.shutdown()


class HostEnvTests(unittest.TestCase):
    """GitHub #10: Proton started from the AppImage/PyInstaller launcher inherited the bundle's old libstdc++ and Vulkan did not load."""

    def setUp(self):
        from fh2mp_launcher import launch
        self.launch = launch
        self.bundle = tempfile.mkdtemp()
        self.addCleanup(lambda: delattr(sys, "_MEIPASS") if hasattr(sys, "_MEIPASS") else None)
        sys._MEIPASS = self.bundle

    @unittest.skipIf(sys.platform == "win32", "Linux only")
    def test_bundle_is_removed_and_the_original_restored(self):
        env = self.launch.host_env({"LD_LIBRARY_PATH": self.bundle + ":/usr/lib", "LD_LIBRARY_PATH_ORIG": "/opt/mine",
                                    "PATH": "/usr/bin:" + self.bundle, "HOME": "/home/x", "APPDIR": "/tmp/.mount_x", "_PYI_ARCHIVE_FILE": "x"})
        self.assertEqual(env["LD_LIBRARY_PATH"], "/opt/mine")
        self.assertNotIn("LD_LIBRARY_PATH_ORIG", env)
        self.assertEqual(env["PATH"], "/usr/bin")
        self.assertEqual(env["HOME"], "/home/x")
        self.assertNotIn("APPDIR", env)
        self.assertNotIn("_PYI_ARCHIVE_FILE", env)

    @unittest.skipIf(sys.platform == "win32", "Linux only")
    def test_nothing_left_of_the_bundle_means_no_variable_and_other_entries_stay(self):
        env = self.launch.host_env({"LD_LIBRARY_PATH": self.bundle + "/sub:" + self.bundle})
        self.assertNotIn("LD_LIBRARY_PATH", env)
        env = self.launch.host_env({"LD_LIBRARY_PATH": "/usr/lib32:" + self.bundle, "QT_PLUGIN_PATH": self.bundle + "/qt"})
        self.assertEqual(env["LD_LIBRARY_PATH"], "/usr/lib32")
        self.assertNotIn("QT_PLUGIN_PATH", env)


class ProtonPathTests(unittest.TestCase):
    """Issue 37: Settings > Proton pointed at the folder; executing the folder failed with EACCES."""

    def test_folder_script_and_errors(self):
        from fh2mp_launcher import launch
        d = tempfile.mkdtemp()
        self.assertEqual(launch.resolve_proton(""), "")
        with self.assertRaises(launch.LaunchError):
            launch.resolve_proton(d)                              # a folder without the script
        script = os.path.join(d, "proton")
        open(script, "w").write("#!/bin/sh\n")
        os.chmod(script, 0o644)
        if sys.platform != "win32":
            with self.assertRaises(launch.LaunchError):
                launch.resolve_proton(d)                          # Unix execute permissions
            os.chmod(script, 0o755)
            self.assertEqual(launch.resolve_proton(d), script)    # the folder is accepted
            self.assertEqual(launch.resolve_proton(d + "/"), script)
            self.assertEqual(launch.resolve_proton(script), script)

    @unittest.skipIf(sys.platform == "win32", "Linux only")
    def test_build_command_runs_the_script_inside_the_folder(self):
        from fh2mp_launcher import launch
        from fh2mp_launcher.settings import Settings
        d = tempfile.mkdtemp()
        script = os.path.join(d, "proton")
        open(script, "w").write("#!/bin/sh\n")
        os.chmod(script, 0o755)
        st = Settings(os.path.join(d, "s.json"))
        st["proton"], st["launch_mode"], st["prefix"] = d, "proton", os.path.join(d, "pfx")
        argv, env, _ = launch.build_command(st, d)
        self.assertEqual(argv[:2], [script, "run"])
        self.assertEqual(env["WINEDLLOVERRIDES"], "winmm,dwrite,d3dcompiler_47=n,b")      # Extra Options and the FH2 Mod Platform


class NoProtonTests(unittest.TestCase):
    @unittest.skipIf(sys.platform == "win32", "Linux only")
    def test_auto_never_picks_the_system_wine(self):
        from unittest import mock
        from fh2mp_launcher import launch
        from fh2mp_launcher.settings import Settings
        d = tempfile.mkdtemp()
        st = Settings(os.path.join(d, "s.json"))
        with mock.patch.object(launch, "find_proton", return_value=""), mock.patch.object(launch.shutil, "which", return_value="/usr/bin/wine"):
            st["launch_mode"] = "auto"
            with self.assertRaises(launch.LaunchError) as e:
                launch.build_command(st, d)
            self.assertIn("No Proton found", str(e.exception))
            st["launch_mode"] = "wine"                       # still available when asked for
            self.assertEqual(launch.build_command(st, d)[0][0], "/usr/bin/wine")


class GpuCheckTests(unittest.TestCase):
    """GitHub #10: Azimuth stops at CreateDXGIFactory2 when `proton run` starts it outside Steam on AMD."""

    def test_gpu_check_failure_reads_the_last_session_only(self):
        import time
        from fh2mp_launcher import launch
        d = tempfile.mkdtemp()
        os.makedirs(os.path.join(d, "data", "logs"))
        log = os.path.join(d, "data", "logs", "azimuth.log")
        open(log, "w").write("1 tid=1 session A began x\n2 tid=2 STOPPED CreateDXGIFactory2 HRESULT=0x80004005 at azimuth+0x595c3\n"
                             "3 tid=1 session B began x\n4 tid=1 graphics adapter AMD\n")
        self.assertFalse(launch.gpu_check_failure(d))               # the failure belongs to the earlier session
        open(log, "a").write("5 tid=2 STOPPED CreateDXGIFactory2 HRESULT=0x80004005 at azimuth+0x595c3\n")
        self.assertTrue(launch.gpu_check_failure(d))
        self.assertFalse(launch.gpu_check_failure(d, since=time.time() + 100))   # a log older than this launch says nothing
        self.assertFalse(launch.gpu_check_failure(tempfile.mkdtemp()))

    def test_gpu_check_failure_reads_only_the_end_of_a_long_log(self):
        from fh2mp_launcher import launch
        d = tempfile.mkdtemp()
        os.makedirs(os.path.join(d, "data", "logs"))
        log = os.path.join(d, "data", "logs", "azimuth.log")
        with open(log, "w") as f:
            f.write("0 tid=1 session A began x\n" + "1 tid=1 filler line of an old session\n" * 20000)     # ~760 KB, never read whole
            f.write("2 tid=1 session B began x\n3 tid=2 STOPPED CreateDXGIFactory2 HRESULT=0x80004005 at azimuth+0x595c3\n")
        self.assertGreater(os.path.getsize(log), 512 * 1024)
        real = open
        with mock.patch("builtins.open", side_effect=lambda p, *a, **k: self.fail("read the whole log") if p == log and "b" not in (a[0] if a else k.get("mode", "r")) else real(p, *a, **k)):
            self.assertTrue(launch.gpu_check_failure(d))


class CacheSizeTests(unittest.TestCase):
    """Azimuth's pipeline cache is serialised whole at each save (#378: out of memory 730499427 bytes at azimuth+0x207a92): its size goes to launcher.log."""

    def test_cache_sizes(self):
        from fh2mp_launcher import launch
        d = tempfile.mkdtemp()
        self.assertIsNone(launch.cache_sizes(d))
        cache = os.path.join(d, "data", "cache")
        os.makedirs(os.path.join(cache, "graphics"))
        for name, mb in (("pipelines-10de-2208-0020000000100842-cf293de09ce2a312.bin", 3), ("pipelines-10de-2208-0020000000100842-cf293de09ce2a312.bin.next", 2),
                         ("translated-23b220b1f9c5b061.pack", 1), ("xg-layouts.bin", 5)):
            with open(os.path.join(cache, name), "wb") as f:
                f.truncate(mb << 20)
        self.assertEqual(launch.cache_sizes(d), {"pipelines_mb": 5, "pipelines_files": 2, "translated_mb": 1})

    def test_start_logs_the_cache_sizes(self):
        from fh2mp_launcher import launch
        d = tempfile.mkdtemp()
        os.makedirs(os.path.join(d, "data", "cache"))
        with open(os.path.join(d, "data", "cache", "pipelines-1002-73ff-1-2.bin"), "wb") as f:
            f.truncate(7 << 20)

        class P:
            pass
        with mock.patch.object(launch, "build_command", lambda *a, **k: (["x"], {}, ".")), \
                mock.patch.object(launch.subprocess, "Popen", lambda *a, **k: P()), \
                mock.patch.object(launch.logs, "info") as info:
            launch.start(Settings(), d)
        calls = {c.args[1]: c.kwargs for c in info.call_args_list}
        self.assertEqual(calls["game.caches"], {"pipelines_mb": 7, "pipelines_files": 1, "translated_mb": 0})
        self.assertIn("game.started", calls)


def platform_dll(version):
    """A stand-in d3dcompiler_47.dll with the FH2 Mod Platform's version resource strings."""
    u = lambda s: s.encode("utf-16-le")  # noqa: E731
    return b"MZ" + u("ProductName\0\0FH2 Mod Platform\0") + u("ProductVersion\0") + b"\x00\x00" + u(version + "\0")


def platform_files(version="2.8.0"):
    return {"d3dcompiler_47.dll": platform_dll(version), "DWrite.dll": platform_dll(version), "LICENSE-FH2ModPlatform.txt": b"MIT License Trinculo54",
            "LICENSE-MinHook.txt": b"MinHook BSD"}


class ShimTests(unittest.TestCase):
    DLL = b"MZ....FH2MP-SHIM-VERSION:0.1.0:END...."

    def setUp(self):
        patch = mock.patch.object(shim, 'PLATFORM_DLL_SHA256', hashlib.sha256(platform_dll('2.8.0')).hexdigest())
        patch.start()
        self.addCleanup(patch.stop)

    def read(self, *parts):
        with open(os.path.join(*parts), "rb") as f:
            return f.read()

    def write(self, data, *parts):
        os.makedirs(os.path.dirname(os.path.join(*parts)), exist_ok=True)
        with open(os.path.join(*parts), "wb") as f:
            f.write(data)

    def test_install_puts_the_shim_in_fh2_mods_with_the_platform(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(shim.installed(d), ("none", ""))
            did = shim.install(d, self.DLL, platform=platform_files())
            self.assertEqual(shim.installed(d), ("ours", "0.1.0"))
            self.assertEqual(self.read(d, "fh2-mods", "FH2MP.dll"), self.DLL)
            self.assertEqual(shim.platform_installed(d), ("platform", "2.8.0"))
            for n in ("LICENSE-FH2ModPlatform.txt", "LICENSE-MinHook.txt"):
                self.assertTrue(os.path.isfile(os.path.join(d, n)), n)
            self.assertTrue(any("FH2 Mod Platform 2.8.0" in s for s in did))
            self.assertFalse(os.path.exists(os.path.join(d, "winmm.dll")))

    def locked(self, locked_path):
        """os.replace as Windows does it for a DLL a running game has loaded: renaming it works, replacing it is "Access is denied"."""
        real = os.replace

        def replace(src, dst, *a, **k):
            if os.path.abspath(dst) == os.path.abspath(locked_path) and os.path.exists(dst):
                raise PermissionError(13, "Access is denied", src, None, dst)
            return real(src, dst, *a, **k)
        return mock.patch("os.replace", replace)

    def test_a_loaded_shim_is_renamed_aside_and_replaced_on_windows(self):
        with tempfile.TemporaryDirectory() as d:
            shim.install(d, self.DLL, platform=platform_files())
            new = b"MZ....FH2MP-SHIM-VERSION:0.2.0:END...."
            with self.locked(os.path.join(d, "fh2-mods", "FH2MP.dll")), mock.patch("sys.platform", "win32"):
                shim.install(d, new, platform=platform_files())
            self.assertEqual(self.read(d, "fh2-mods", "FH2MP.dll"), new)
            self.assertEqual([n for n in os.listdir(os.path.join(d, "fh2-mods")) if ".old" in n or ".tmp" in n], [])

    def test_an_install_that_cannot_work_names_the_file(self):
        with tempfile.TemporaryDirectory() as d:
            shim.install(d, self.DLL, platform=platform_files())
            target = os.path.join(d, "fh2-mods", "FH2MP.dll")
            with mock.patch("os.replace", side_effect=PermissionError(13, "Access is denied", target)):
                with self.assertRaises(shim.ShimError) as cm:
                    shim.install(d, b"MZ....FH2MP-SHIM-VERSION:0.2.0:END....", platform=platform_files())
            self.assertIn("Access is denied", str(cm.exception))
            self.assertIn("FH2MP.dll", str(cm.exception))
            self.assertEqual(self.read(target), self.DLL)

    def test_loaded_platform_upgrade_uses_rename_instead_of_delete(self):
        with tempfile.TemporaryDirectory() as d:
            target = self.write(platform_dll("2.7.1"), d, "d3dcompiler_47.dll")
            target = os.path.join(d, "d3dcompiler_47.dll")
            unlink = os.unlink

            def locked_delete(path, *args, **kwargs):
                if os.path.abspath(path) == os.path.abspath(target):
                    raise PermissionError("DLL is loaded")
                return unlink(path, *args, **kwargs)
            with self.locked(target), mock.patch("sys.platform", "win32"), mock.patch("os.unlink", side_effect=locked_delete):
                shim.install(d, self.DLL, platform=platform_files())
            self.assertEqual(self.read(target), platform_files()["d3dcompiler_47.dll"])

    def test_both_platform_loaders_are_required_and_repaired(self):
        with tempfile.TemporaryDirectory() as d:
            shim.install(d, self.DLL, platform=platform_files())
            self.assertTrue(shim.platform_ok(d))
            os.unlink(os.path.join(d, 'DWrite.dll'))
            self.assertFalse(shim.platform_ok(d))
            self.assertEqual(shim.installed(d)[0], 'old')
            shim.install(d, self.DLL, platform=platform_files())
            self.write(b'MZ foreign dwrite', d, 'DWrite.dll')
            self.assertFalse(shim.platform_ok(d))
            shim.install(d, self.DLL, platform=platform_files())
            self.assertTrue(shim.platform_ok(d))
            self.assertEqual(self.read(d, 'DWrite.dll.old'), b'MZ foreign dwrite')

    @unittest.skipIf(sys.platform in ('win32', 'darwin'), 'needs a case-sensitive file system')
    def test_conflicting_dwrite_case_variant_is_backed_up(self):
        with tempfile.TemporaryDirectory() as d:
            shim.install(d, self.DLL, platform=platform_files())
            self.write(b'MZ old dwrite loader', d, 'dwrite.dll')
            self.assertFalse(shim.platform_ok(d))
            shim.install(d, self.DLL, platform=platform_files())
            self.assertEqual(self.read(d, 'dwrite.dll.old'), b'MZ old dwrite loader')
            self.assertTrue(shim.platform_ok(d))

    def test_the_winmm_layout_is_moved_over(self):
        """Up to rc5: our winmm.dll next to azimuth.exe, Extra Options (with its settings) in fh2extra."""
        with tempfile.TemporaryDirectory() as d:
            self.write(b"MZ FH2MP-SHIM-VERSION:1.0.0-rc5:END", d, "winmm.dll")
            self.write(b"MZ extra options", d, "fh2extra", "winmm.dll")
            for n, b in (("FH2_ExtraOptions.ini", b"Mine=1"), ("FH2_ExtraOptions.dat", b"dat"), ("Gamerpic.png", b"pic"),
                         ("LICENSE.txt", b"GPL"), ("NOTICE.txt", b"notice")):
                self.write(b, d, "fh2extra", n)
            self.assertEqual(shim.installed(d), ("old", "1.0.0-rc5"))
            did = shim.install(d, self.DLL, platform=platform_files())
            self.assertEqual(shim.installed(d), ("ours", "0.1.0"))
            self.assertEqual(self.read(d, "winmm.dll"), b"MZ extra options")
            self.assertEqual(self.read(d, "FH2_ExtraOptions.ini"), b"Mine=1")
            self.assertEqual(self.read(d, "FH2_ExtraOptions.dat"), b"dat")
            for n in ("LICENSE.txt", "NOTICE.txt"):                      # the GPL notices stay with the source
                self.assertTrue(os.path.isfile(os.path.join(d, "fh2extra", n)), n)
            self.assertFalse(os.path.exists(os.path.join(d, "fh2extra", "winmm.dll")))
            self.assertFalse(os.path.exists(os.path.join(d, "fh2extra", "Gamerpic.png")))
            self.assertTrue(any("removed the old winmm.dll shim" in s for s in did))

    def test_platform_newer_older_and_foreign_are_replaced_with_exact_pin(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(platform_dll("2.8.0"), d, "d3dcompiler_47.dll")
            shim.install(d, self.DLL, platform=platform_files())
            self.assertEqual(shim.platform_installed(d), ("platform", "2.8.0"))          # only the supplied build is supported
            self.write(platform_dll("2.7.2"), d, "d3dcompiler_47.dll")
            self.assertEqual(shim.installed(d), ("old", "0.1.0"))                        # too old for this shim: install again
            shim.install(d, self.DLL, platform=platform_files())
            self.assertEqual(shim.platform_installed(d), ("platform", "2.8.0"))
            self.write(b"MZ fh2wheel loader", d, "d3dcompiler_47.dll")                     # fh2wheel's own loader
            self.assertEqual(shim.platform_installed(d), ("foreign", ""))
            did = shim.install(d, self.DLL, platform=platform_files())
            self.assertEqual(shim.platform_installed(d), ("platform", "2.8.0"))
            self.assertEqual(self.read(d, "d3dcompiler_47.dll.old"), b"MZ fh2wheel loader")
            self.assertTrue(any("d3dcompiler_47.dll.old" in s for s in did))

    @unittest.skipIf(sys.platform in ("win32", "darwin"), "needs a case-sensitive file system")
    def test_another_loader_in_other_letter_case_is_moved_aside(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(platform_dll("2.8.0"), d, "d3dcompiler_47.dll")
            self.write(b"MZ radio mod loader", d, "D3DCompiler_47.dll")          # Wine would load this one
            self.assertEqual(shim.platform_installed(d), ("foreign", ""))
            shim.install(d, self.DLL, platform=platform_files())
            self.assertEqual(sorted(n for n in os.listdir(d) if n.lower() == "d3dcompiler_47.dll"), ["d3dcompiler_47.dll"])
            self.assertEqual(self.read(d, "D3DCompiler_47.dll.old"), b"MZ radio mod loader")
            self.assertEqual(shim.installed(d), ("ours", "0.1.0"))

    def test_a_local_build_needs_the_platform_there(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(shim.ShimError):
                shim.install(d, self.DLL)                                                    # no platform there and none given
            shim.install(d, self.DLL, platform=platform_files())
            shim.install(d, self.DLL.replace(b"0.1.0", b"0.1.1-debug"))
            self.assertEqual(shim.installed(d), ("ours", "0.1.1-debug"))

    def test_update_keeps_previous_and_rejects_junk(self):
        with tempfile.TemporaryDirectory() as d:
            shim.install(d, self.DLL, platform=platform_files())
            shim.install(d, self.DLL.replace(b"0.1.0", b"0.2.0"), platform=platform_files())
            self.assertEqual(shim.installed(d), ("ours", "0.2.0"))
            self.assertEqual(shim.inspect_dll(os.path.join(d, "fh2-mods", "FH2MP.dll.prev")), ("ours", "0.1.0"))
            with self.assertRaises(shim.ShimError):
                shim.install(d, b"not a shim", platform=platform_files())
            with self.assertRaises(shim.ShimError):
                shim.install(d, self.DLL, expect_sha256="0" * 64, platform=platform_files())

    def test_turned_off_on_the_mods_tab_is_turned_back_on(self):
        with tempfile.TemporaryDirectory() as d:
            shim.install(d, self.DLL, platform=platform_files())
            self.assertFalse(shim.enable(d))
            self.write(b"# turned off on the MODS tab\nWreckingBall.dll\nfh2mp.dll\n", d, "fh2-mods", "platform-disabled.txt")
            self.assertTrue(shim.enable(d))
            self.assertEqual(self.read(d, "fh2-mods", "platform-disabled.txt"), b"# turned off on the MODS tab\nWreckingBall.dll\n")
            self.write(b"FH2MP.dll\n", d, "fh2-mods", "platform-disabled.txt")
            self.assertTrue(any("back on" in s for s in shim.install(d, self.DLL, platform=platform_files())))

    def test_uninstall_leaves_the_platform_and_extra_options(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(b"MZ extra options", d, "winmm.dll")
            shim.install(d, self.DLL, platform=platform_files())
            shim.uninstall(d)
            self.assertEqual(shim.installed(d), ("none", ""))
            self.assertEqual(self.read(d, "winmm.dll"), b"MZ extra options")
            self.assertEqual(shim.platform_installed(d), ("platform", "2.8.0"))

    def test_shim_zip(self):
        from fh2mp_launcher import core, updater
        with tempfile.TemporaryDirectory() as d:
            zp = os.path.join(d, "fh2mp-shim-0.1.0.zip")
            with zipfile.ZipFile(zp, "w") as z:
                z.writestr("fh2-mods/FH2MP.dll", self.DLL)
                z.writestr("fh2mp/LICENSE.txt", b"GPL")
            self.assertEqual(core.read_shim_zip(zp), self.DLL)
            with zipfile.ZipFile(zp, "w") as z:
                z.writestr("fh2-mods\\FH2MP.dll", self.DLL)                                 # Compress-Archive on Windows
            self.assertEqual(core.read_shim_zip(zp), self.DLL)
            with zipfile.ZipFile(zp, "w") as z:
                z.writestr("winmm.dll", self.DLL)                                          # an rc5 shim zip
            with self.assertRaises(updater.UpdateError):
                core.read_shim_zip(zp)

    def test_the_guide_in_the_shim_zip(self):
        from fh2mp_launcher import core
        with tempfile.TemporaryDirectory() as d:
            zp = os.path.join(d, "fh2mp-shim-0.1.0.zip")
            with zipfile.ZipFile(zp, "w") as z:
                z.writestr("fh2-mods/FH2MP.dll", self.DLL)
            self.assertIsNone(core.read_shim_guide(zp))                                       # a shim from before the Guide shipped
            with zipfile.ZipFile(zp, "w") as z:
                z.writestr("fh2-mods\\FH2MP.dll", self.DLL)
                z.writestr("fh2-mods\\FH2MPGuide.dll", b"MZ guide")                          # Compress-Archive on Windows
            self.assertEqual(core.read_shim_guide(zp), b"MZ guide")

    def test_install_puts_the_guide_next_to_the_shim(self):
        guide = b"MZ the guide"
        with tempfile.TemporaryDirectory() as d:
            did = shim.install(d, self.DLL, platform=platform_files(), guide=guide, guide_sha256=hashlib.sha256(guide).hexdigest())
            self.assertEqual(self.read(d, "fh2-mods", "FH2MPGuide.dll"), guide)
            self.assertTrue(any("Guide" in x for x in did))
            shim.install(d, self.DLL, platform=platform_files(), guide=b"MZ a newer guide")
            self.assertEqual(self.read(d, "fh2-mods", "FH2MPGuide.dll"), b"MZ a newer guide")
            self.assertEqual(self.read(d, "fh2-mods", "FH2MPGuide.dll.prev"), guide)
            with self.assertRaises(shim.ShimError):
                shim.install(d, self.DLL, platform=platform_files(), guide=b"MZ tampered", guide_sha256=hashlib.sha256(guide).hexdigest())
            self.assertEqual(self.read(d, "fh2-mods", "FH2MPGuide.dll"), b"MZ a newer guide")
            shim.install(d, self.DLL, platform=platform_files())                               # no Guide given: the installed one stays
            self.assertEqual(self.read(d, "fh2-mods", "FH2MPGuide.dll"), b"MZ a newer guide")
            shim.uninstall(d)
            self.assertFalse(os.path.exists(os.path.join(d, "fh2-mods", "FH2MPGuide.dll")))
            self.assertFalse(os.path.exists(os.path.join(d, "fh2-mods", "FH2MPGuide.dll.prev")))

    def test_session_file(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "data", "bin"))
            shim.write_session(d, 0x900001234ABCD, "fh2.tok", "https://x.example")
            p = os.path.join(d, "data", "bin", "fh2mp-session.txt")
            with open(p) as f:
                self.assertEqual(f.read(), "xuid=900001234abcd\ntoken=fh2.tok\n")
            if os.name == "posix":
                self.assertEqual(os.stat(p).st_mode & 0o777, 0o600)
            shim.clear_session(d)
            self.assertFalse(os.path.exists(p))


class AzimuthTests(unittest.TestCase):
    def test_version_resource_wins_over_historical_migration_strings(self):
        with tempfile.TemporaryDirectory() as folder:
            exe = os.path.join(folder, "azimuth.exe")
            for version in ("1.0.0", "1.0.1", "1.0.1.0"):
                with open(exe, "wb") as f:
                    f.write(b"MZ settings migrated by 0.9.10\0 then by 1.0.0\0")
                    f.write("FileVersion\0".encode("utf-16-le"))
                    f.write((version + "\0").encode("utf-16-le"))
                self.assertEqual(azimuth.exe_version(exe), version)
                self.assertTrue(azimuth.uses_native_gamerpic(folder))

    def test_shared_library_uses_fh2_instead_of_first_or_selected_title(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d)
            text = ("[launcher]\r\ngame=deadbeef\r\n"
                    "[title-deadbeef]\r\nfolder=C:\\OtherGame\r\ncontent=other-dlc\r\n"
                    "[title-1A96545B]\r\nfolder=C:\\Games\\ForzaHorizon2\r\ncontent=storm-island\r\n")
            path = os.path.join(d, "data", "azimuth.ini")
            with open(path, "wb") as f:
                f.write(text.encode("utf-16"))
            self.assertEqual(azimuth.inspect(d).game_folder, "C:\\Games\\ForzaHorizon2")
            self.assertFalse(azimuth.has_game_configured(d))
            azimuth.configure_game(d, azimuth.fh2_title(azimuth.read_ini(d))["folder"], preserve_content=True)
            self.assertTrue(azimuth.has_game_configured(d))
            ini = azimuth.read_ini(d)
            self.assertEqual(ini["title-deadbeef"], {"folder": "C:\\OtherGame", "content": "other-dlc"})
            self.assertEqual(ini["title-1A96545B"]["content"], "storm-island")
            self.assertEqual(len([k for k in ini if k.lower() == "title-1a96545b"]), 1)

    def test_other_game_configuration_does_not_count_as_fh2(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d)
            with open(os.path.join(d, "data", "azimuth.ini"), "wb") as f:
                f.write("[launcher]\ngame=deadbeef\n[title-deadbeef]\nfolder=C:\\OtherGame\n".encode("utf-16"))
            self.assertFalse(azimuth.has_game_configured(d))
            self.assertEqual(azimuth.inspect(d).game_folder, "")

    def make(self, d, exe=b"MZ \x000.9.10\x00\x00: Azimuth data"):
        with open(os.path.join(d, "azimuth.exe"), "wb") as f:
            f.write(exe)
        os.makedirs(os.path.join(d, "data"))
        ini = "[launcher]\r\nresolution=720\r\nplayer_name=Old Name\r\nsettings_version=1\r\ngame=1a96545b\r\n[title-1a96545b]\r\nfolder=C:\\Games\\ForzaHorizon2\r\n"
        with open(os.path.join(d, "data", "azimuth.ini"), "wb") as f:
            f.write(ini.encode("utf-16"))

    def test_inspect_and_support(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d)
            self.assertFalse(azimuth.inspect(d).supported)
            h = hashlib.sha256(b"MZ \x000.9.10\x00\x00: Azimuth data").hexdigest()
            info = azimuth.inspect(d, [{"version": "0.9.10", "sha256": h}])
            self.assertEqual((info.found, info.supported, info.version), (True, True, "0.9.10"))
            self.assertEqual(info.game_folder, "C:\\Games\\ForzaHorizon2")
        self.assertFalse(azimuth.inspect("/nonexistent").found)

    def test_ingame_name_follows_azimuths_rule(self):
        for tag, want in (("Mike", "Mike"), ("Speedy Gonzo", "Speedy Gonzo"), ("Red_Malone", "Red Malone"), ("Xx-Reiner-xX", "Xx Reiner xX"),
                          ("OsM4n.03", "OsM4n 03"), ("a__b", "a b"), ("4K", "X4K"), ("47fox", "X47fox"), ("000", "X000"),
                          ("2Cute4ULauCar", "X2Cute4ULauCar"), ("Mr. Sant Ana", "Mr Sant Ana"), ("KYO_YOUTUBER_BR", "KYO YOUTUBER BR"),
                          ("Gamer_Sports_Fan_99", "Gamer Sports Fa"), ("___", "Player 1"), ("", "Player 1")):
            got = azimuth.ingame_name(tag)
            self.assertEqual(got, want, tag)
            self.assertRegex(got, r"^[A-Za-z][A-Za-z0-9]*( [A-Za-z0-9]+)*$")
            self.assertLessEqual(len(got), 15)

    def test_player_name_edit_preserves_the_file(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d)
            self.assertTrue(azimuth.set_player_name(d, "New Name"))
            self.assertFalse(azimuth.set_player_name(d, "New Name"))
            ini = azimuth.read_ini(d)
            self.assertEqual(ini["launcher"]["player_name"], "New Name")
            self.assertEqual(ini["launcher"]["resolution"], "720")
            self.assertEqual(ini["title-1a96545b"]["folder"], "C:\\Games\\ForzaHorizon2")
            with open(os.path.join(d, "data", "azimuth.ini"), "rb") as f:
                raw = f.read()
            self.assertEqual(raw[:2], b"\xff\xfe")
            self.assertIn(b"\r\x00\n\x00", raw)

    @unittest.skipUnless(RIG and os.path.isfile(os.path.join(RIG, "azimuth.exe")), "set FH2MP_TEST_AZIMUTH to a real Azimuth folder")
    def test_real_azimuth_is_recognised(self):
        from fh2mp_launcher.constants import AZIMUTH_VERSION
        info = azimuth.inspect(RIG)
        self.assertEqual((info.supported, info.version), (True, AZIMUTH_VERSION))

    def test_find_game_installs_survives_an_unreadable_link(self):
        """GitHub #19: a Windows junction that cannot be followed made realpath raise inside the setup wizard."""
        with tempfile.TemporaryDirectory() as home:
            os.makedirs(os.path.join(home, "Games", "x", "Mount"))
            with mock.patch("os.path.expanduser", lambda p: p.replace("~", home)), \
                    mock.patch("os.path.realpath", side_effect=OSError(1920, "cannot access")):
                self.assertEqual(azimuth.find_game_installs(), [])


def fake_release(kind, files, dll_name):
    """A stand-in for a pinned upstream release: a zip with `files` and the pin (constants.UPSTREAM shape) that matches it."""
    import io
    from fh2mp_launcher.constants import UPSTREAM
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for n, b in files.items():
            z.writestr(n, b)
    data = buf.getvalue()
    pin = dict(UPSTREAM[kind], sha256=hashlib.sha256(data).hexdigest(), dllSha256=hashlib.sha256(files[dll_name]).hexdigest())
    return data, pin



class UpstreamTests(unittest.TestCase):
    def test_extract_takes_only_the_pinned_build(self):
        from fh2mp_launcher import upstream
        # 0.6.0's layout: one winmm.dll at the root for FH2, FH3 and FM5; only the FH2 files the pin names are taken
        files = {"winmm.dll": b"MZ-eo", "FH2_ExtraOptions.ini": b"ini", "FH3_ExtraOptions.ini": b"fh3", "FM5_ExtraOptions.ini": b"fm5",
                 "mods/Forza Horizon 2/P/mod.json": b"{}", "mods/Forza Horizon 2/P/plugins/P.lib": b"lib",
                 "mods/Forza Motorsport 5/Q/mod.json": b"{}"}
        data, pin = fake_release("extraOptions", files, "winmm.dll")
        pin['files'] = {n: n for n in ('winmm.dll', 'FH2_ExtraOptions.ini', 'mods/Forza Horizon 2/P/mod.json')}
        self.assertEqual(upstream.extract("extraOptions", data, pin),
                         {"winmm.dll": b"MZ-eo", "FH2_ExtraOptions.ini": b"ini", "mods/Forza Horizon 2/P/mod.json": b"{}"})
        with self.assertRaises(upstream.UpstreamError):
            upstream.extract("extraOptions", data + b"x", pin)                         # not the pinned zip
        with self.assertRaises(upstream.UpstreamError):
            upstream.extract("extraOptions", data, dict(pin, dllSha256="0" * 64))     # the DLL inside is not the pinned build
        pdata, ppin = fake_release("platform", {"d3dcompiler_47.dll": b"MZ-plat", "DWrite.dll": b"MZ-plat", "LICENSE.txt": b"MIT", "LICENSE-MinHook.txt": b"BSD",
                                                "README.txt": b"r"}, "d3dcompiler_47.dll")
        self.assertEqual(upstream.extract("platform", pdata, ppin),
                         {"d3dcompiler_47.dll": b"MZ-plat", "DWrite.dll": b"MZ-plat", "LICENSE-FH2ModPlatform.txt": b"MIT", "LICENSE-MinHook.txt": b"BSD"})

    def test_pins_point_at_the_authors_releases(self):
        from fh2mp_launcher.constants import UPSTREAM
        self.assertTrue(UPSTREAM["platform"]["url"].startswith("https://github.com/Trinculo54/fh2-mod-platform/releases/download/"))
        self.assertTrue(UPSTREAM["extraOptions"]["url"].startswith("https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions/releases/download/"))
        self.assertEqual(set(UPSTREAM["platform"]["files"].values()), set(shim.PLATFORM_FILES))
        eo = UPSTREAM["extraOptions"]
        # Extra Options 0.6.0: the FH2 files only (no FH3/FM5 settings or mods, no import library), nothing at the old mods root, and every
        # file but the settings pinned by hash
        self.assertTrue(all(n in ("winmm.dll", "FH2_ExtraOptions.ini") or n.startswith("mods/Forza Horizon 2/") for n in eo["files"].values()))
        self.assertFalse([n for n in eo["files"] if n.endswith(".lib")])
        self.assertEqual(set(eo["fileSha256"]), {n for n in eo["files"].values() if not n.endswith(".ini") and n != "winmm.dll"})


class OfflineTests(unittest.TestCase):
    def test_disable_and_enable_round_trip(self):
        """GitHub #17: offline play switches FH2MP off on the platform's MODS list and back on afterwards; other mods' entries stay."""
        with tempfile.TemporaryDirectory() as az:
            lst = os.path.join(az, "fh2-mods", shim.DISABLED_LIST)
            self.assertFalse(shim.enable(az))
            self.assertTrue(shim.disable(az))
            self.assertFalse(shim.disable(az))
            with open(lst) as f:
                self.assertEqual(f.read().split(), [shim.SHIM_DLL])
            with open(lst, "w") as f:
                f.write("other.dll\n" + shim.SHIM_DLL + "\n")
            self.assertTrue(shim.enable(az))
            with open(lst) as f:
                self.assertEqual(f.read().split(), ["other.dll"])
            self.assertTrue(shim.disable(az))
            with open(lst) as f:
                self.assertEqual(f.read().split(), ["other.dll", shim.SHIM_DLL])


    def test_the_guide_goes_off_and_on_with_fh2mp(self):
        """Offline play turns the Guide off with FH2MP (it is part of FH2MP online) and on again with it."""
        with tempfile.TemporaryDirectory() as az:
            os.makedirs(os.path.join(az, "fh2-mods"))
            with open(os.path.join(az, "fh2-mods", shim.GUIDE_DLL), "wb") as f:
                f.write(b"MZ guide")
            lst = os.path.join(az, "fh2-mods", shim.DISABLED_LIST)
            self.assertTrue(shim.disable(az))
            with open(lst) as f:
                self.assertEqual(f.read().split(), [shim.SHIM_DLL, shim.GUIDE_DLL])
            self.assertFalse(shim.disable(az))
            self.assertTrue(shim.enable(az))
            with open(lst) as f:
                self.assertEqual(f.read().split(), [])
            with open(lst, "w") as f:                                                         # an older launcher turned only FH2MP off
                f.write(shim.SHIM_DLL + "\n")
            self.assertFalse(shim.disable(az))                                                # FH2MP was off already; the Guide joins it
            with open(lst) as f:
                self.assertEqual(f.read().split(), [shim.SHIM_DLL, shim.GUIDE_DLL])


class ExtraOptionsTests(unittest.TestCase):
    INI = b"; Forza Horizon 2 Extra Options\n; Release v0.2.88\n; Compatible with Azimuth 0.9.10\n[Global]\nEnabled=1\n"

    def setUp(self):
        self._saved = set(shim.SUPPORTED_EXTRA_SHA256)
        for b in (b"MZ-mod-dll", b"MZ-release"):                                        # stand-ins for the one supported build
            shim.SUPPORTED_EXTRA_SHA256.add(hashlib.sha256(b).hexdigest())
        self.addCleanup(lambda: (shim.SUPPORTED_EXTRA_SHA256.clear(), shim.SUPPORTED_EXTRA_SHA256.update(self._saved)))
        from fh2mp_launcher.constants import UPSTREAM
        pin = dict(UPSTREAM['extraOptions'], files={'Azimuth/winmm.dll': 'winmm.dll', 'Azimuth/FH2_ExtraOptions.ini': 'FH2_ExtraOptions.ini'}, fileSha256={})
        patch = mock.patch.dict(UPSTREAM, extraOptions=pin)
        patch.start()
        self.addCleanup(patch.stop)

    def test_only_one_extra_options_pin_is_supplied(self):
        from fh2mp_launcher.constants import UPSTREAM
        self.assertEqual(set(UPSTREAM), {'platform', 'extraOptions', 'honkHonk', 'anna', 'achievements'})
        self.assertEqual(UPSTREAM['extraOptions']['version'], '0.6.0')
        self.assertEqual(UPSTREAM['extraOptions']['dll'], 'winmm.dll')
        with tempfile.TemporaryDirectory() as az:
            self.write(b'MZ-0.3.0-old', az, 'winmm.dll')
            self.assertFalse(shim.extra_options_supported(az))
            shim.install_extra(az, self.release())
            self.assertTrue(shim.extra_options_supported(az))

    def read(self, *parts):
        with open(os.path.join(*parts), "rb") as f:
            return f.read()

    def write(self, data, *parts):
        os.makedirs(os.path.dirname(os.path.join(*parts)), exist_ok=True)
        with open(os.path.join(*parts), "wb") as f:
            f.write(data)

    def release(self):
        return {"winmm.dll": b"MZ-release", "FH2_ExtraOptions.ini": self.INI}

    def test_installs_next_to_azimuth_and_keeps_settings(self):
        with tempfile.TemporaryDirectory() as az:
            self.assertFalse(shim.extra_options_supported(az))
            did = shim.install_extra(az, self.release())
            self.assertIn("RokkuDayo", did[-1])
            self.assertEqual(self.read(az, "winmm.dll"), b"MZ-release")
            self.assertEqual(shim.extra_options_installed(az), (True, "0.2.88", "0.9.10"))
            self.assertTrue(shim.extra_options_supported(az))
            # another build there: kept aside in fh2extra, the release goes in, the player's settings stay
            self.write(b"MZ-0.2.88-old", az, "winmm.dll")
            self.write(self.INI + b"Mine=1\n", az, "FH2_ExtraOptions.ini")
            did = shim.install_extra(az, self.release())
            self.assertTrue(any("unsupported-1" in s for s in did))
            self.assertEqual(self.read(az, "winmm.dll"), b"MZ-release")
            self.assertEqual(self.read(az, "fh2extra", "winmm.dll.unsupported-1"), b"MZ-0.2.88-old")
            self.assertIn(b"Mine=1", self.read(az, "FH2_ExtraOptions.ini"))

    def test_the_pinned_build_there_is_left_alone(self):
        with tempfile.TemporaryDirectory() as az:
            self.write(b"MZ-mod-dll", az, "winmm.dll")
            self.write(self.INI + b"Mine=1\n", az, "FH2_ExtraOptions.ini")
            self.assertTrue(shim.extra_options_complete(az))
            self.assertEqual(shim.install_extra(az, self.release()), [])
            self.assertEqual(self.read(az, "winmm.dll"), b"MZ-mod-dll")
            self.assertIn(b"Mine=1", self.read(az, "FH2_ExtraOptions.ini"))

    def test_the_pinned_build_without_its_settings_file_gets_the_default_one(self):
        """GitHub #20: "FH2 Extra Options is missing" with the mod's DLL there but not its ini."""
        with tempfile.TemporaryDirectory() as az:
            self.write(b"MZ-mod-dll", az, "winmm.dll")
            self.assertFalse(shim.extra_options_complete(az))
            self.assertTrue(shim.install_extra(az, self.release()))
            self.assertEqual(self.read(az, "winmm.dll"), b"MZ-mod-dll")
            self.assertEqual(self.read(az, "FH2_ExtraOptions.ini"), self.INI)
            self.assertTrue(shim.extra_options_complete(az))

    def retired(self):
        """shim.RETIRED_EXTRA_OPTIONS with stand-in 0.4.4 files: the DLL b"MZ-0.4.4" and one package, its mod.json and plugin DLL."""
        h = lambda b: hashlib.sha256(b).hexdigest()  # noqa: E731
        return mock.patch.dict(shim.RETIRED_EXTRA_OPTIONS, dllSha256=h(b"MZ-0.4.4"), packages={
            "Extra Options - Weather Control": {"mod.json": h(b"wc-json"), "plugins/EO_WeatherControl.dll": h(b"MZ-wc-1.0")},
            "Extra Options - Garage Revving": {"mod.json": h(b"gr-json"), "plugins/EO_GarageEngineRev.dll": h(b"MZ-gr")}})

    def test_the_0_4_4_build_is_updated_and_kept_under_its_version(self):
        with tempfile.TemporaryDirectory() as az, self.retired():
            self.write(b"MZ-0.4.4", az, "winmm.dll")
            self.write(self.INI + b"Mine=1\n", az, "FH2_ExtraOptions.ini")
            did = shim.install_extra(az, self.release())
            self.assertIn("updated FH2 Extra Options 0.4.4 to", did[-1])
            self.assertEqual(self.read(az, "fh2extra", "winmm.dll.0.4.4"), b"MZ-0.4.4")
            self.assertEqual(self.read(az, "winmm.dll"), b"MZ-release")
            self.assertIn(b"Mine=1", self.read(az, "FH2_ExtraOptions.ini"))
            self.write(b"MZ-0.4.4", az, "winmm.dll")                              # again (a 1.3.2 launcher put it back): the first copy stays
            shim.install_extra(az, self.release())
            self.assertEqual(self.read(az, "fh2extra", "winmm.dll.0.4.4-2"), b"MZ-0.4.4")

    def test_packages_go_to_the_fh2_folder_and_the_old_bundled_ones_are_retired(self):
        """0.6.0 brings its packages in mods/Forza Horizon 2; the 0.4.4 copies earlier launchers put at the mods root move to
        fh2extra/mods-0.4.4 (with the player's settings in them) when they are exactly the 0.4.4 files, a changed one stays."""
        pkg = "mods/Forza Horizon 2/Extra Options - Weather Control"
        release = dict(self.release(), **{f"{pkg}/mod.json": b"wc-json-1.1", f"{pkg}/plugins/EO_WeatherControl.dll": b"MZ-wc-1.1",
                                          f"{pkg}/plugins/EO_WeatherControl.ini": b"[TimeLock]\nEnabled=0\n"})
        with tempfile.TemporaryDirectory() as az, self.retired():
            old_wc, old_gr = os.path.join(az, "mods", "Extra Options - Weather Control"), os.path.join(az, "mods", "Extra Options - Garage Revving")
            self.write(b"wc-json", old_wc, "mod.json")
            self.write(b"MZ-wc-1.0", old_wc, "plugins", "EO_WeatherControl.dll")
            self.write(b"[TimeLock]\nHour=20\n", old_wc, "plugins", "EO_WeatherControl.ini")
            self.write(b"gr-json", old_gr, "mod.json")
            self.write(b"MZ-gr-changed", old_gr, "plugins", "EO_GarageEngineRev.dll")
            self.write(b"MZ-car", az, "mods", "Some Car Pack", "mod.json")        # the player's own: Extra Options moves it, not FH2MP
            self.write(b"Mine\n", az, *pkg.split("/"), "plugins", "EO_WeatherControl.ini")   # settings already there are kept
            did = shim.install_extra(az, release)
            self.assertEqual(self.read(az, *pkg.split("/"), "plugins", "EO_WeatherControl.dll"), b"MZ-wc-1.1")
            self.assertEqual(self.read(az, *pkg.split("/"), "plugins", "EO_WeatherControl.ini"), b"Mine\n")
            moved = os.path.join(az, "fh2extra", "mods-0.4.4", "Extra Options - Weather Control")
            self.assertFalse(os.path.exists(old_wc))
            self.assertEqual(self.read(moved, "plugins", "EO_WeatherControl.ini"), b"[TimeLock]\nHour=20\n")
            self.assertTrue(any("mods-0.4.4" in s for s in did))
            self.assertEqual(self.read(old_gr, "plugins", "EO_GarageEngineRev.dll"), b"MZ-gr-changed")
            self.assertTrue(os.path.isdir(os.path.join(az, "mods", "Some Car Pack")))

    def test_two_mod_lists_are_put_right_only_when_nothing_is_lost(self):
        """Extra Options 0.6.0 loads no mods at all with both mods/loadorder.txt and mods/Forza Horizon 2/loadorder.txt (a player went
        0.6.0 -> 0.4.4 -> 0.6.0). When every root package is already in mods/Forza Horizon 2 the root list is renamed; else both stay."""
        with tempfile.TemporaryDirectory() as az:
            self.assertEqual(shim.eo_layout_repair(az), ([], ""))                   # no mods folder
            self.write(b"Pack\n", az, "mods", "loadorder.txt")
            self.write(b"x", az, "mods", "Pack", "mod.json")
            self.assertEqual(shim.eo_layout_repair(az), ([], ""))                   # only the old list: Extra Options moves it itself
            self.write(b"Pack\n", az, "mods", "Forza Horizon 2", "loadorder.txt")
            did, note = shim.eo_layout_repair(az)                                   # Pack is only at the root: it would be lost
            self.assertEqual(did, [])
            self.assertIn("two mod lists", note)
            self.assertIn("Pack", note)
            self.assertTrue(os.path.isfile(os.path.join(az, "mods", "loadorder.txt")))
            self.write(b"x", az, "mods", "Forza Horizon 2", "Pack", "mod.json")
            os.makedirs(os.path.join(az, "mods", "Cache", "fh2-db"))                # 0.6.0's own folders are no packages
            os.makedirs(os.path.join(az, "mods", "Forza Motorsport 5"))
            did, note = shim.eo_layout_repair(az)
            self.assertEqual(note, "")
            self.assertTrue(did)
            self.assertFalse(os.path.exists(os.path.join(az, "mods", "loadorder.txt")))
            self.assertEqual(self.read(az, "mods", "loadorder.txt" + shim.EO_OLD_ORDER_SUFFIX), b"Pack\n")
            self.assertEqual(self.read(az, "mods", "Forza Horizon 2", "loadorder.txt"), b"Pack\n")

    def tls_rig(self, az, with_backup=True):
        """azimuth.exe that names the SHA-256 of its own helper, an old Extra Options in its place, the mod's backup as .old."""
        stock, old_mod = b"MZ-stock-helper", b"MZ-extra-options-0.2.74-patch-dll"
        self.write(b"MZ azimuth " + hashlib.sha256(stock).hexdigest().encode() + b" end", az, "azimuth.exe")
        self.write(old_mod, az, "data", "bin", "azimuth-tls.dll")
        if with_backup:
            self.write(stock, az, "data", "bin", "azimuth-tls.dll.old")
        return stock, old_mod

    def test_an_old_extra_options_helper_is_replaced_by_azimuths_own(self):
        """GitHub #20: the 0.2.74 azimuth-tls.dll next to 0.2.98 patches the game twice (LoadLibraryExW import, garage_xml_parse_hook)."""
        with tempfile.TemporaryDirectory() as az:
            stock, old_mod = self.tls_rig(az)
            self.assertTrue(shim.tls_helper_modified(az))
            did = shim.restore_tls_helper(az)
            self.assertTrue(did)
            self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll"), stock)
            self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll.replaced"), old_mod)
            self.assertFalse(shim.tls_helper_modified(az))
            self.assertEqual(shim.restore_tls_helper(az), [])                          # nothing to do the second time

    def test_a_stock_or_absent_helper_is_left_alone(self):
        with tempfile.TemporaryDirectory() as az:
            self.assertFalse(shim.tls_helper_modified(az))                              # no helper at all
            stock, _ = self.tls_rig(az, with_backup=False)
            self.write(stock, az, "data", "bin", "azimuth-tls.dll")
            self.assertFalse(shim.tls_helper_modified(az))
            self.assertEqual(shim.restore_tls_helper(az), [])

    def test_an_old_helper_without_a_backup_is_explained(self):
        with tempfile.TemporaryDirectory() as az:
            _, old_mod = self.tls_rig(az, with_backup=False)
            with self.assertRaises(shim.ShimError) as cm:
                shim.restore_tls_helper(az)
            self.assertIn("azimuth-tls.dll", str(cm.exception))
            self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll"), old_mod)  # untouched

    def test_numbered_helpers_retire_a_legacy_file_without_a_legacy_backup(self):
        with tempfile.TemporaryDirectory() as az:
            exe, old = b"MZ-modern-host", b"MZ-obsolete-shared-helper"
            helpers = {"azimuth-tls-1.dll": b"MZ-title-one", "azimuth-tls-2.dll": b"MZ-title-two"}
            self.write(exe, az, "azimuth.exe")
            self.write(old, az, "data", "bin", "azimuth-tls.dll")
            self.write(b"earlier backup", az, "data", "bin", "azimuth-tls.dll.replaced")
            for name, data in helpers.items():
                self.write(data, az, "data", "bin", name)
            builds = [{"sha256": hashlib.sha256(exe).hexdigest()}]
            hashes = {name: hashlib.sha256(data).hexdigest() for name, data in helpers.items()}
            with mock.patch.object(shim, "BUILTIN_AZIMUTH_BUILDS", builds), mock.patch.object(shim, "AZIMUTH_TLS_HELPERS", hashes):
                self.assertTrue(shim.restore_tls_helper(az))
                self.assertFalse(os.path.exists(os.path.join(az, shim.TLS_HELPER)))
                self.assertEqual(shim.restore_tls_helper(az), [])
            self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll.replaced-2"), old)
            self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll.replaced"), b"earlier backup")
            for name, data in helpers.items():
                self.assertEqual(self.read(az, "data", "bin", name), data)

    def test_unverified_numbered_helpers_do_not_retire_the_legacy_file(self):
        for invalid in (None, b"MZ-wrong-version"):
            with self.subTest(helper=invalid), tempfile.TemporaryDirectory() as az:
                exe, old = b"MZ-modern-host", b"MZ-obsolete-shared-helper"
                self.write(exe, az, "azimuth.exe")
                self.write(old, az, "data", "bin", "azimuth-tls.dll")
                if invalid is not None:
                    self.write(invalid, az, "data", "bin", "azimuth-tls-1.dll")
                with mock.patch.object(shim, "BUILTIN_AZIMUTH_BUILDS", [{"sha256": hashlib.sha256(exe).hexdigest()}]):
                    with self.assertRaises(shim.ShimError) as cm:
                        shim.restore_tls_helper(az)
                self.assertNotIn("Copy data\\bin\\azimuth-tls.dll", str(cm.exception))
                self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll"), old)

    def test_helper_mismatch_does_not_identify_an_unknown_file_as_extra_options(self):
        with tempfile.TemporaryDirectory() as az:
            self.tls_rig(az, with_backup=False)
            unknown = b"MZ unknown or damaged helper"
            self.write(unknown, az, "data", "bin", "azimuth-tls.dll")
            with self.assertRaises(shim.ShimError) as cm:
                shim.restore_tls_helper(az)
            message = str(cm.exception)
            self.assertNotIn("is an old FH2 Extra Options file", message)
            self.assertIn(os.path.join(az, shim.TLS_HELPER), message)
            self.assertIn(hashlib.sha256(unknown).hexdigest(), message)

    def test_helper_recovery_checks_other_backups_against_this_executable(self):
        with tempfile.TemporaryDirectory() as az:
            stock, old_mod = self.tls_rig(az)
            self.write(b"MZ wrong backup", az, "data", "bin", "azimuth-tls.dll.old")
            self.write(stock, az, "data", "bin", "azimuth-tls.dll.bak")
            shim.restore_tls_helper(az)
            self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll"), stock)
            self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll.replaced"), old_mod)
            self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll.bak"), stock)

    def test_unreadable_helper_is_an_error_instead_of_a_successful_check(self):
        with tempfile.TemporaryDirectory() as az:
            self.tls_rig(az)
            with mock.patch("builtins.open", side_effect=PermissionError(13, "Access denied")):
                with self.assertRaises(shim.ShimError):
                    shim.restore_tls_helper(az)

    def test_failed_helper_replacement_keeps_current_and_original_bytes(self):
        for target, name in ((shim.shutil, "copy2"), (shim.paths, "atomic_write")):
            with self.subTest(stage=name), tempfile.TemporaryDirectory() as az:
                stock, old_mod = self.tls_rig(az)
                with mock.patch.object(target, name, side_effect=PermissionError(13, "Access denied")):
                    with self.assertRaises(shim.ShimError):
                        shim.restore_tls_helper(az)
                self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll"), old_mod)
                self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll.old"), stock)

    def test_wrong_version_backups_are_preserved_and_rejected(self):
        with tempfile.TemporaryDirectory() as az:
            _, old_mod = self.tls_rig(az, with_backup=False)
            for suffix in ("old", "bak", "backup", "old-2", "replaced"):
                self.write(b"MZ another Azimuth version", az, "data", "bin", "azimuth-tls.dll." + suffix)
            with self.assertRaises(shim.ShimError):
                shim.restore_tls_helper(az)
            self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll"), old_mod)
            self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll.old"), b"MZ another Azimuth version")

    def test_repeated_helper_repair_keeps_previous_recovery_files(self):
        with tempfile.TemporaryDirectory() as az:
            stock, old_mod = self.tls_rig(az)
            shim.restore_tls_helper(az)
            # Another installer replaces it again between launches.
            self.write(b"MZ newly replaced", az, "data", "bin", "azimuth-tls.dll")
            shim.restore_tls_helper(az)
            self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll"), stock)
            self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll.replaced"), old_mod)
            self.assertEqual(self.read(az, "data", "bin", "azimuth-tls.dll.replaced-2"), b"MZ newly replaced")

    def test_the_rc5_layout_is_moved_over_first(self):
        with tempfile.TemporaryDirectory() as az:
            self.write(b"MZ-mod-dll", az, "fh2extra", "winmm.dll")                     # the supported mod in fh2extra ...
            self.write(self.INI, az, "fh2extra", "FH2_ExtraOptions.ini")
            self.write(b"MZ FH2MP-SHIM-VERSION:1.0.0-rc5:END", az, "winmm.dll")         # ... behind our old winmm.dll
            self.assertFalse(shim.extra_options_supported(az))
            self.assertTrue(any("back next to azimuth.exe" in s for s in shim.install_extra(az, self.release())))
            self.assertEqual(self.read(az, "winmm.dll"), b"MZ-mod-dll")                  # the player's own copy, not a download

    def test_refuses_what_is_not_the_mod(self):
        with tempfile.TemporaryDirectory() as az:
            with self.assertRaises(shim.ShimError):
                shim.install_extra(az, {"winmm.dll": b"MZ FH2MP-SHIM-VERSION:1.0.0:END"})      # our own shim is not the mod
            with self.assertRaises(shim.ShimError):
                shim.install_extra(az, {"winmm.dll": b"MZ-some-other-build"})
            self.assertFalse(os.path.exists(os.path.join(az, "winmm.dll")))


class SaveAdoptionTests(unittest.TestCase):
    """The layout Azimuth writes: User_<HEX>/revisions/<hex>/data/{BinarySaveState,SaveState,...}. A profile the game made for the prologue has
    a 4-byte BinarySaveState, one with progress about 110 KB (checked on the rigs)."""
    TITLES = ("data", "titles", "1A96545B", "saves", "player-1")

    def make(self, d, name, progress=True, base=TITLES, rev="00000000000008dd", when=None):
        p = os.path.join(d, *base, name, "revisions", rev, "data")
        os.makedirs(p)
        with open(os.path.join(p, "BinarySaveState"), "wb") as f:
            f.write(name.encode().ljust(112994 if progress else 4, b"\0"))
        if when is not None:
            os.utime(os.path.join(p, "BinarySaveState"), (when, when))
        return os.path.join(d, *base, name)

    def state(self, profile):
        rev = azimuth._newest_revision(profile)
        with open(os.path.join(rev, "data", "BinarySaveState"), "rb") as f:
            return f.read().rstrip(b"\0").decode()

    def test_new_account_inherits_the_default_profile(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d, "User_9000000000000")
            self.make(d, "User_900001C7CE3F5")
            note = azimuth.adopt_saves(d, 0x000900004ADDC5F7)
            self.assertIn("User_9000000000000 -> User_900004ADDC5F7", note)
            root = os.path.join(d, *self.TITLES)
            self.assertEqual(self.state(os.path.join(root, "User_900004ADDC5F7")), "User_9000000000000")
            self.assertTrue(os.path.isdir(os.path.join(root, "User_9000000000000")))        # the original stays
            self.assertEqual(azimuth.adopt_saves(d, 0x000900004ADDC5F7), "")               # once only; never overwrites progress

    def test_falls_back_to_the_newest_profile_and_ignores_backups_and_empty_ones(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d, "User_AAA", when=1)
            self.make(d, "User_BBB", when=2)
            self.make(d, "User_DDD", progress=False, when=3)
            self.make(d, "User_CCC.fresh-backup", when=4)
            self.assertIn("User_BBB ->", azimuth.adopt_saves(d, 0x9000000000042))

    def test_an_empty_profile_from_the_prologue_is_replaced_and_kept(self):
        """#36: the game made an empty User_<xuid> because the save was not found; every later Play skipped adoption."""
        with tempfile.TemporaryDirectory() as d:
            self.make(d, "User_9000000000042", progress=False, rev="000000000000000a")
            self.make(d, "User_9000000000000")
            self.assertIn("User_9000000000000 -> User_9000000000042", azimuth.adopt_saves(d, 0x9000000000042))
            root = os.path.join(d, *self.TITLES)
            self.assertEqual(self.state(os.path.join(root, "User_9000000000042")), "User_9000000000000")
            kept = [n for n in os.listdir(root) if n.startswith("User_9000000000042.fresh-")]
            self.assertEqual(len(kept), 1)
            self.assertFalse(azimuth.has_progress(os.path.join(root, kept[0])))

    def test_the_old_layout_and_azimuths_migration_backup_are_found(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d, "User_9000000000000", base=("data", "saves", "player-1"))         # Azimuth 0.9.7
            self.assertIn("-> User_9000000000042", azimuth.adopt_saves(d, 0x9000000000042))
            self.assertTrue(azimuth.has_progress(os.path.join(d, *self.TITLES, "User_9000000000042")))    # created where 0.9.10 reads it
        with tempfile.TemporaryDirectory() as d:
            self.make(d, "User_9000000000000", base=("data", "saves-backup-20261017T031818-821-232", "player-1"))
            self.assertIn("-> User_9000000000042", azimuth.adopt_saves(d, 0x9000000000042))

    def test_a_save_in_another_azimuth_folder_is_found(self):
        """A new Azimuth release is unpacked into a new folder: the save is in the old one (zts / Rattzy)."""
        with tempfile.TemporaryDirectory() as new, tempfile.TemporaryDirectory() as old:
            self.make(old, "User_9000000000000", base=("data", "saves", "player-1"))
            self.make(old, "User_9000000000042", when=1)                                  # the account's own save wins over the default
            note = azimuth.adopt_saves(new, 0x9000000000042, [old])
            self.assertIn(f"User_9000000000042 (from {os.path.join(old, 'data', 'titles', '1A96545B', 'saves')}) -> User_9000000000042", note)

    def test_nothing_to_adopt(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(azimuth.adopt_saves(d, 0x9000000000042), "")
            self.make(d, "User_9000000000000", progress=False)
            self.assertEqual(azimuth.adopt_saves(d, 0x9000000000042), "")
            self.assertFalse(os.path.exists(os.path.join(d, *self.TITLES, "User_9000000000042")))


class DevModeTests(unittest.TestCase):
    def setUp(self):
        patch = mock.patch.object(shim, 'PLATFORM_DLL_SHA256', hashlib.sha256(platform_dll('2.8.0')).hexdigest())
        patch.start()
        self.addCleanup(patch.stop)
        self.home = tempfile.TemporaryDirectory()
        self.old = {k: os.environ.get(k) for k in ("FH2MP_DEV", "FH2MP_LAUNCHER_HOME", "XDG_CONFIG_HOME", "FH2MP_NO_KEYRING",
                                                  "APPDATA", "LOCALAPPDATA", "XDG_CACHE_HOME")}
        os.environ["FH2MP_NO_KEYRING"] = "1"
        os.environ.pop("FH2MP_LAUNCHER_HOME", None)
        os.environ["XDG_CONFIG_HOME"] = self.home.name
        os.environ["XDG_CACHE_HOME"] = self.home.name
        os.environ["APPDATA"] = self.home.name
        os.environ["LOCALAPPDATA"] = self.home.name

    def tearDown(self):
        for k, v in self.old.items():
            os.environ.pop(k, None) if v is None else os.environ.__setitem__(k, v)
        self.home.cleanup()

    def test_dev_has_its_own_folder_key_and_defaults(self):
        from fh2mp_launcher import keystore, paths
        from fh2mp_launcher.settings import Settings
        os.environ.pop("FH2MP_DEV", None)
        stable = paths.config_dir()
        os.environ["FH2MP_DEV"] = "1"
        dev = paths.config_dir()
        self.assertNotEqual(stable, dev)
        self.assertEqual(os.path.basename(dev).lower(), "fh2mp-dev")
        s = Settings()
        self.assertEqual((s["channel"], s["allow_insecure"], s["pin_shim"]), ("dev", True, True))
        keystore.save(identity.Identity.generate())
        self.assertTrue(os.path.isfile(os.path.join(dev, "identity.key")))
        os.environ.pop("FH2MP_DEV")
        self.assertIsNone(keystore.load())                      # the stable launcher does not see the dev key

    def test_debug_shim_is_never_replaced_and_local_install_needs_dev(self):
        from fh2mp_launcher import core
        from fh2mp_launcher.settings import Settings
        with tempfile.TemporaryDirectory() as d:
            for name, body in (("azimuth.exe", b"MZ"), ("d3dcompiler_47.dll", platform_dll("2.8.0")), ("DWrite.dll", platform_dll("2.8.0"))):     # a bare DLL needs the platform
                with open(os.path.join(d, name), "wb") as f:
                    f.write(body)
            dll = os.path.join(d, "local.dll")
            with open(dll, "wb") as f:
                f.write(b"MZ FH2MP-SHIM-VERSION:1.0.0-debug:END")
            os.environ.pop("FH2MP_DEV", None)
            lc = core.Launcher(Settings())
            lc.settings["azimuth_dir"] = d
            with self.assertRaises(core.PlayBlocked):
                lc.install_shim_file(dll)                       # players cannot install unsigned shims
            os.environ["FH2MP_DEV"] = "1"
            lc = core.Launcher(Settings())
            lc.settings["azimuth_dir"] = d
            lc.settings["pin_shim"] = False
            lc.install_shim_file(dll)
            self.assertEqual(shim.installed(d), ("ours", "1.0.0-debug"))
            self.assertTrue(lc.shim_pinned())                   # -debug is pinned even without the setting
            lc.manifest = {"tag": "v1.0.0", "shim": {"version": "1.0.0"}, "launcher": {"version": "1.0.0"}}
            self.assertIsNone(lc.update_status()["shim"])

    def test_too_old_launcher_and_shim_are_held_back(self):
        from fh2mp_launcher import core, version
        from fh2mp_launcher.settings import Settings
        os.environ.pop("FH2MP_DEV", None)
        with tempfile.TemporaryDirectory() as d:
            lc = core.Launcher(Settings(), load_identity=False)
            lc.settings["azimuth_dir"] = d
            lc.server = {"minLauncher": "0.0.0", "minShim": "0.0.0"}
            self.assertFalse(lc.launcher_outdated())
            lc.server = {"minLauncher": "99.0.0", "minShim": "1.0.0-rc4"}
            self.assertTrue(lc.launcher_outdated())
            self.assertEqual(lc.update_status()["minLauncherOk"], True)          # no manifest yet: nothing known
            lc.refresh_server = lambda: lc.server
            with self.assertRaises(core.PlayBlocked) as cm:
                lc.prepare_play()
            self.assertEqual(cm.exception.code, "launcher_outdated")
            lc.server = {"minLauncher": version.__version__, "minShim": "1.0.0-rc4"}
            self.assertFalse(lc.shim_outdated())                    # nothing installed: that is "no shim", not "old shim"
            lc.shim_state = lambda: ("ours", "1.0.0-rc3")
            self.assertTrue(lc.shim_outdated())
            lc.shim_state = lambda: ("ours", "1.0.0-rc4")
            self.assertTrue(lc.shim_outdated())
            lc.shim_state = lambda: ("ours", version.__version__)
            self.assertFalse(lc.shim_outdated())
            lc.shim_state = lambda: ("ours", version.__version__ + "-debug")
            self.assertTrue(lc.shim_outdated())                     # a released launcher does not pin it
            with mock.patch.object(core.paths, "is_dev", return_value=True):
                self.assertFalse(lc.shim_outdated())                # the dev launcher leaves a debug build alone

    def test_an_older_shim_of_ours_is_updated_without_play(self):
        from fh2mp_launcher import core, version
        from fh2mp_launcher.settings import Settings
        os.environ.pop("FH2MP_DEV", None)
        with tempfile.TemporaryDirectory() as d:
            lc = core.Launcher(Settings(), load_identity=False)
            lc.settings["azimuth_dir"] = d
            installed = []
            lc.install_shim = lambda *a, **k: installed.append(1)
            lc.game_running = lambda: False
            lc.manifest = {"tag": "v" + version.__version__, "shim": {"version": version.__version__}, "launcher": {"version": version.__version__}}

            def attempt(state, **kw):
                installed.clear()
                lc.shim_state = lambda: state
                for k, v in kw.items():
                    setattr(lc, k, v)
                return lc.auto_update_shim(), len(installed)
            self.assertEqual(attempt(("ours", "1.0.0-rc4")), (True, 1))                 # our shim, older than this launcher
            self.assertEqual(attempt(("ours", version.__version__)), (False, 0))        # already current
            self.assertEqual(attempt(("foreign", "")), (False, 0))                      # somebody else's DLL: only on the player's click
            self.assertEqual(attempt(("none", "")), (False, 0))
            self.assertEqual(attempt(("ours", "1.0.0-rc4"), game_running=lambda: True), (False, 0))     # the game has the DLL loaded
            lc.game_running = lambda: False
            lc.shim_pinned = lambda *a: True
            self.assertEqual(attempt(("ours", "1.0.0-rc4")), (False, 0))                # pinned (developer)
            lc.shim_pinned = lambda *a: False
            lc.manifest = None
            self.assertEqual(attempt(("ours", "1.0.0-rc4")), (False, 0))                # nothing known about releases yet

    def test_a_leftover_session_is_removed_unless_the_game_runs(self):
        from fh2mp_launcher import core, shim
        from fh2mp_launcher.settings import Settings
        os.environ.pop("FH2MP_DEV", None)
        with tempfile.TemporaryDirectory() as d:
            lc = core.Launcher(Settings(), load_identity=False)
            lc.settings["azimuth_dir"] = d
            session = os.path.join(shim.bin_dir(d), shim.SESSION_FILE)
            shim.write_session(d, 0x9000000000001, "fh2.token", "https://lsp.example")
            self.assertTrue(os.path.exists(session))
            lc.game_running = lambda: True
            self.assertFalse(lc.clear_leftover_session())          # the running game's launcher keeps what the game may still read
            self.assertTrue(os.path.exists(session))
            lc.game_running = lambda: False
            self.assertTrue(lc.clear_leftover_session())           # nothing running: a leftover would let the game online without Play
            self.assertFalse(os.path.exists(session))
            self.assertFalse(lc.clear_leftover_session())          # nothing left to remove
            lc.settings["azimuth_dir"] = ""
            self.assertFalse(lc.clear_leftover_session())          # no folder chosen yet

    def test_keeping_car_packs_skips_the_database_sync_and_names_none_in_the_session(self):
        from fh2mp_launcher import core
        from fh2mp_launcher.settings import Settings
        lc = core.Launcher(Settings(), load_identity=False)
        synced = []
        lc.sync_gamedb = lambda: synced.append(1) or "synced"
        lc.sync_base_parts = lambda: synced.append("parts") and ""
        lc.gamedb_synced = True                                   # left over from an earlier start
        lc.settings["keep_car_packs"] = True
        note = lc.car_database_step()
        self.assertEqual((synced, lc.gamedb_synced), (["parts"], False))   # nothing replaced, no database named for the shim, the part list refreshed
        self.assertIn("car packs are kept", note)
        lc.settings["keep_car_packs"] = False
        self.assertEqual((lc.car_database_step(), synced), ("synced", ["parts", 1]))          # the default is unchanged

    def test_a_part_list_that_cannot_be_refreshed_does_not_block_play(self):
        from fh2mp_launcher import core
        from fh2mp_launcher.settings import Settings
        with tempfile.TemporaryDirectory() as d:
            lc = core.Launcher(Settings(os.path.join(d, "settings.json")), load_identity=False)
            lc.settings["azimuth_dir"] = d

            class Down:
                def gamedb(self, token):
                    raise core.ApiError(0, "unreachable", "server unreachable")
            lc.api = lambda: Down()
            self.assertIn("couldn't refresh the list of the game's parts", lc.sync_base_parts())

    def test_the_setup_check_names_every_component_and_what_is_wrong(self):
        from fh2mp_launcher import core, shim, version
        from fh2mp_launcher.settings import Settings
        os.environ.pop("FH2MP_DEV", None)
        lc = core.Launcher(Settings(), load_identity=False)
        lc.server = {"minLauncher": "0.0.0", "minShim": "1.1.2"}
        lc.settings["azimuth_dir"] = ""
        rows = {r[0]: r for r in lc.status_lines()}
        self.assertEqual(rows["Azimuth folder"][3], False)                      # nothing chosen: the first thing to fix
        with tempfile.TemporaryDirectory() as d:
            lc.settings["azimuth_dir"] = d
            lc.shim_state = lambda: ("ours", "1.0.0-rc3")
            rows = {r[0]: r for r in lc.status_lines()}
            self.assertEqual(list(rows), ["FH2MP launcher", "Azimuth", "FH2MP shim", "FH2 Mod Platform", "FH2 Extra Options",
                                          "HonkHonk", "FH2 ANNA", "Achievements", "Session for the game", "Car database", "Connection", "Mode"])
            self.assertEqual(rows["Achievements"][1:], ("not installed", "1.0.2", None))   # optional: missing is not a fault
            self.assertEqual(rows["FH2MP shim"][1:], ("1.0.0-rc3", version.__version__ + " (server needs 1.1.2)", False))   # outdated: marked
            self.assertEqual(rows["Azimuth"][3], False)                          # no azimuth.exe in the folder
            self.assertEqual(rows["FH2 Mod Platform"][1], "not installed")
            self.assertEqual(rows["Session for the game"][1], "none yet: Play makes it")
            lc.shim_state = lambda: ("ours", version.__version__)
            self.assertEqual({r[0]: r for r in lc.status_lines()}["FH2MP shim"][3], True)
            shim.write_session(d, 0x9000000000001, "fh2.token", "https://lsp.example")
            self.assertIn("present", {r[0]: r for r in lc.status_lines()}["Session for the game"][1])
            lc.settings["keep_car_packs"] = True
            self.assertIn("custom cars stay out of online events", {r[0]: r for r in lc.status_lines()}["Car database"][1])
        text = repr(lc.status_lines())
        self.assertNotIn("fh2.token", text)                                       # the token never appears in what a player copies

    def test_extra_env_reaches_the_game(self):
        from fh2mp_launcher import launch
        from fh2mp_launcher.settings import Settings
        seen = {}

        class P:
            pass

        def fake_popen(argv, cwd=None, env=None, **kw):
            seen.update(env)
            return P()
        s = Settings()
        s["extra_env"] = "FH2MP_NET_TRAFFIC=1\n# comment\nFH2MP_TOKEN=evil\nBAD LINE\nFH2MP_XUID = 9000000000000"
        orig_cmd, orig_popen = launch.build_command, launch.subprocess.Popen
        launch.build_command = lambda *a, **k: (["x"], {}, ".")
        launch.subprocess.Popen = fake_popen
        try:
            launch.start(s, ".")
        finally:
            launch.build_command, launch.subprocess.Popen = orig_cmd, orig_popen
        self.assertEqual((seen.get("FH2MP_NET_TRAFFIC"), seen.get("FH2MP_XUID")), ("1", "9000000000000"))
        self.assertNotEqual(seen.get("FH2MP_TOKEN"), "evil")

    def test_game_port_from_the_server_reaches_the_shim(self):
        from fh2mp_launcher import core, launch
        from fh2mp_launcher.settings import Settings
        self.assertEqual(core.game_port_env({"host": "h", "port": 5100}), {"FH2MP_SERVER_PORT": "5100"})
        for bad in (None, {}, {"port": "5100"}, {"port": 80}, {"port": 65000}, {"port": True}, "5100"):
            self.assertEqual(core.game_port_env(bad), {}, bad)
        self.assertEqual(core.game_port_env({"host": "h", "port": 5100, "traffic": 1}),
                         {"FH2MP_SERVER_PORT": "5100", "FH2MP_NET_TRAFFIC": "1"})        # dev: networked traffic
        for off in (0, True, "1", None):
            self.assertNotIn("FH2MP_NET_TRAFFIC", core.game_port_env({"port": 5100, "traffic": off}), off)
        seen = {}

        class P:
            pass

        def fake_popen(argv, cwd=None, env=None, **kw):
            seen.update(env)
            return P()
        s = Settings()
        orig_cmd, orig_popen = launch.build_command, launch.subprocess.Popen
        launch.build_command = lambda *a, **k: (["x"], {}, ".")
        launch.subprocess.Popen = fake_popen
        try:
            launch.start(s, ".", extra_env={"FH2MP_SERVER_PORT": "5100"})
            self.assertEqual(seen.get("FH2MP_SERVER_PORT"), "5100")
            s["extra_env"] = "FH2MP_SERVER_PORT=5200"          # a developer's own setting wins
            launch.start(s, ".", extra_env={"FH2MP_SERVER_PORT": "5100"})
            self.assertEqual(seen.get("FH2MP_SERVER_PORT"), "5200")
        finally:
            launch.build_command, launch.subprocess.Popen = orig_cmd, orig_popen


if __name__ == "__main__":
    unittest.main()

class OnlineSetupNoteIsShownNeverRestarted(unittest.TestCase):
    """The shim writes fh2mp-online-setup.txt when a game run never began its online setup; the launcher only reports it (user
    decision 2026-11-01: no automatic restarts)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.az = os.path.join(self.tmp.name, "az")
        os.makedirs(os.path.join(self.az, "data", "bin"))
        self.lc = core.Launcher(Settings(os.path.join(self.tmp.name, "settings.json")), load_identity=False)
        self.lc.settings["azimuth_dir"] = self.az
        self.marker = os.path.join(self.az, "data", "bin", "fh2mp-online-setup.txt")
        self.old_marker = os.path.join(self.az, "data", "bin", "fh2mp-restart.txt")

    def note(self, text="online setup skipped", path=None):
        with open(path or self.marker, "w") as f:
            f.write(text)

    def running(self):
        p = mock.Mock()
        p.poll.return_value = None
        self.lc.process = p

    def test_a_running_online_game_with_a_note_reports_it_once(self):
        self.assertEqual(self.lc.online_setup_note(), "")
        self.running()
        self.assertEqual(self.lc.online_setup_note(), "", "no note, nothing to say")
        self.note()
        self.assertEqual(self.lc.online_setup_note(), "online setup skipped")
        self.assertFalse(os.path.exists(self.marker), "the note is read once")
        self.assertEqual(self.lc.online_setup_note(), "")
        self.note()
        self.lc.offline_run = True
        self.assertEqual(self.lc.online_setup_note(), "", "an offline run never has an online setup")
        self.lc.offline_run = False
        self.lc.process.poll.return_value = 0
        self.assertEqual(self.lc.online_setup_note(), "", "a game that has ended is not reported")

    def test_nothing_restarts_the_game(self):
        self.assertFalse(hasattr(self.lc, "restart_game"))
        self.assertFalse(hasattr(self.lc, "restart_requested"))
        self.running()
        self.note()
        with mock.patch.object(self.lc, "stop_game") as stop, mock.patch.object(self.lc, "play") as play:
            self.lc.online_setup_note()
        stop.assert_not_called()
        play.assert_not_called()

    def test_pressing_play_clears_old_notes_of_both_names(self):
        self.note()
        self.note(path=self.old_marker)
        self.lc.settings["offline_mode"] = True
        with mock.patch.object(self.lc, "play_offline"):
            self.lc.play()
        self.assertFalse(os.path.exists(self.marker))
        self.assertFalse(os.path.exists(self.old_marker))


class MirroredDownloads(unittest.TestCase):
    """The pinned third-party zips come from the FH2MP mirror first and the author's release second; both must be exactly the pinned bytes."""

    def setUp(self):
        from fh2mp_launcher import upstream
        from fh2mp_launcher.constants import UPSTREAM
        self.upstream, self.pin = upstream, UPSTREAM['platform']

    def updater(self, outcomes):
        calls = []

        class Fake:
            def download_url(_self, url, entry, progress=None, cancelled=lambda: False):
                calls.append(url)
                result = outcomes.pop(0)
                if isinstance(result, Exception):
                    raise result
                return result
        return Fake(), calls

    def test_the_pins_name_the_mirror_first_and_the_same_hashes(self):
        from fh2mp_launcher.constants import AZIMUTH_ARCHIVE, MIRROR_BASE, UPSTREAM
        for kind in ('platform', 'extraOptions', 'honkHonk'):
            self.assertTrue(UPSTREAM[kind]['mirror'].startswith(MIRROR_BASE))
            self.assertEqual(UPSTREAM[kind]['mirror'].rsplit('/', 1)[1], UPSTREAM[kind]['url'].rsplit('/', 1)[1])
        self.assertTrue(AZIMUTH_ARCHIVE['url'].startswith(MIRROR_BASE))
        self.assertEqual(len(AZIMUTH_ARCHIVE['sha256']), 64)

    def test_the_mirror_is_used_and_the_author_is_the_fallback(self):
        from fh2mp_launcher.updater import UpdateError
        with mock.patch.object(self.upstream, 'extract', return_value={'ok': b''}):
            up, calls = self.updater(['/tmp/x'])
            with mock.patch('builtins.open', mock.mock_open(read_data=b'zip')):
                self.assertEqual(self.upstream.fetch('platform', up), {'ok': b''})
            self.assertEqual(calls, [self.pin['mirror']])
            up, calls = self.updater([UpdateError('download failed (timed out)'), '/tmp/x'])
            with mock.patch('builtins.open', mock.mock_open(read_data=b'zip')):
                self.upstream.fetch('platform', up)
            self.assertEqual(calls, [self.pin['mirror'], self.pin['url']])
            up, calls = self.updater([UpdateError('the download does not match its checksum'), UpdateError('download failed (404)')])
            with self.assertRaises(UpdateError):
                self.upstream.fetch('platform', up)
            self.assertEqual(len(calls), 2)

    def test_a_cancelled_download_does_not_try_the_next_source(self):
        from fh2mp_launcher.updater import UpdateError
        up, calls = self.updater([UpdateError('cancelled')])
        with self.assertRaises(UpdateError):
            self.upstream.fetch('platform', up)
        self.assertEqual(len(calls), 1)

    def test_no_other_host_is_accepted(self):
        from fh2mp_launcher.constants import UPSTREAM
        pin = dict(UPSTREAM['platform'], mirror='https://example.com/x.zip')
        up, calls = self.updater([])
        with mock.patch.dict(UPSTREAM, platform=pin), self.assertRaises(self.upstream.UpstreamError):
            self.upstream.fetch('platform', up)
        self.assertEqual(calls, [])

    def test_azimuth_comes_from_the_mirror_with_its_pinned_size_and_hash(self):
        from fh2mp_launcher import setup
        from fh2mp_launcher.constants import AZIMUTH_ARCHIVE
        seen = {}

        class Fake:
            def __init__(self, channel=None):
                pass

            def download_url(self, url, entry, progress=None, cancelled=lambda: False):
                seen.update(url=url, entry=entry)
                return '/cache/azimuth.zip'
        lc = mock.Mock()
        lc.settings = {'channel': 'stable'}
        with mock.patch.object(setup.updater, 'Updater', Fake):
            self.assertEqual(setup.Packages(lc).get_azimuth(), '/cache/azimuth.zip')
        self.assertEqual(seen['url'], AZIMUTH_ARCHIVE['url'])
        self.assertEqual((seen['entry']['sha256'], seen['entry']['size']), (AZIMUTH_ARCHIVE['sha256'], AZIMUTH_ARCHIVE['size']))


class HonkHonkTests(unittest.TestCase):
    """HonkHonk is a pinned required add-on; Extra Options' stick buttons move out of its way once."""
    INI = (b"; Forza Horizon 2 Extra Options\r\n[Bindings]\r\n; Headlights require a single tap.\r\nHeadlightsXbox=RightStick\r\nHeadlightsKeyboard=F3\r\n"
           b"RoofXbox = rightstick \r\nRoofKeyboard=F4\r\nSignalLeftXbox=DPadLeft\r\n")

    def setUp(self):
        from fh2mp_launcher.constants import UPSTREAM
        self.dll, self.bank, self.ini = b"MZ-honk", b"FSB5-bank", b"[HonkHonk]\r\nSelectedHorn=0\r\n"
        self.files = {"fh2-mods/HonkHonk.dll": self.dll, "fh2-mods/HonkHonk.ini": self.ini, "fh2-mods/HonkHonk/HZ2_SFX_HonkHonk.bank": self.bank}
        pin = dict(UPSTREAM["honkHonk"], dllSha256=hashlib.sha256(self.dll).hexdigest(),
                   fileSha256={"fh2-mods/HonkHonk.dll": hashlib.sha256(self.dll).hexdigest(),
                               "fh2-mods/HonkHonk/HZ2_SFX_HonkHonk.bank": hashlib.sha256(self.bank).hexdigest()})
        patch = mock.patch.dict(UPSTREAM, honkHonk=pin)
        patch.start()
        self.addCleanup(patch.stop)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.az = self.tmp.name

    def path(self, rel):
        return os.path.join(self.az, *rel.split("/"))

    def test_the_real_pin_names_the_release_and_its_layout(self):
        from fh2mp_launcher.constants import UPSTREAM
        pin = UPSTREAM["honkHonk"]
        self.assertTrue(pin["url"].startswith("https://github.com/suderek22/FH2_HonkHonk/releases/download/v0.4.0-online/"))
        self.assertEqual(set(pin["files"].values()), {shim.HONK_DLL, shim.HONK_INI, shim.HONK_BANK})
        self.assertEqual(pin["fileSha256"][shim.HONK_DLL], pin["dllSha256"])

    def test_extract_takes_the_pinned_zip_and_maps_it_into_the_azimuth_folder(self):
        from fh2mp_launcher import upstream
        data, pin = fake_release("honkHonk", {"HonkHonk_v0.4.0/fh2-mods/HonkHonk.dll": self.dll, "HonkHonk_v0.4.0/fh2-mods/HonkHonk.ini": self.ini,
                                              "HonkHonk_v0.4.0/fh2-mods/HonkHonk/HZ2_SFX_HonkHonk.bank": self.bank, "HonkHonk_v0.4.0/README.md": b"r"},
                                 "HonkHonk_v0.4.0/fh2-mods/HonkHonk.dll")
        self.assertEqual(upstream.extract("honkHonk", data, pin), self.files)
        with self.assertRaises(upstream.UpstreamError):
            upstream.extract("honkHonk", data + b"x", pin)

    def test_install_puts_the_pinned_files_in_place_and_keeps_the_players_settings(self):
        self.assertFalse(shim.honk_complete(self.az))
        shim.install_honk(self.az, self.files)
        self.assertTrue(shim.honk_complete(self.az))
        self.assertEqual(shim.honk_installed(self.az), (True, shim.honk_version()))
        with open(self.path(shim.HONK_INI), "wb") as f:
            f.write(b"[HonkHonk]\r\nSelectedHorn=7\r\n")
        with open(self.path(shim.HONK_BANK), "wb") as f:
            f.write(b"edited")
        self.assertFalse(shim.honk_complete(self.az))
        did = shim.install_honk(self.az, self.files)
        self.assertTrue(shim.honk_complete(self.az))
        with open(self.path(shim.HONK_INI)) as f:
            self.assertIn("SelectedHorn=7", f.read())                                       # the choice survives a repair
        with open(self.path(shim.HONK_BANK) + ".before-fh2mp-setup", "rb") as f:
            self.assertEqual(f.read(), b"edited")                                           # a different bank is kept, not lost
        self.assertTrue(did)
        self.assertEqual(shim.install_honk(self.az, self.files), [])                       # nothing to do twice

    def test_another_honkhonk_dll_is_reported_and_replaced(self):
        os.makedirs(os.path.dirname(self.path(shim.HONK_DLL)))
        with open(self.path(shim.HONK_DLL), "wb") as f:
            f.write(b"MZ-other")
        self.assertEqual(shim.honk_installed(self.az), (True, "unknown"))
        shim.install_honk(self.az, self.files)
        self.assertEqual(shim.honk_installed(self.az), (True, shim.honk_version()))

    def test_sticks_are_swapped_only_where_they_are_still_the_default(self):
        new, n = shim.swap_stick_bindings_bytes(self.INI)
        self.assertEqual(n, 2)
        self.assertEqual(new, self.INI.replace(b"HeadlightsXbox=RightStick", b"HeadlightsXbox=LeftStick").replace(b"rightstick ", b"LeftStick "))
        custom = b"[Bindings]\r\nHeadlightsXbox=LB+RightStick\r\nRoofXbox=DPadDown\r\n"
        self.assertEqual(shim.swap_stick_bindings_bytes(custom), (custom, 0))
        lf = b"HeadlightsXbox=RightStick\nRoofXbox=RightStick\n"                          # LF files stay LF
        self.assertEqual(shim.swap_stick_bindings_bytes(lf)[0], b"HeadlightsXbox=LeftStick\nRoofXbox=LeftStick\n")

    def test_a_fresh_extra_options_install_writes_the_swapped_settings(self):
        from fh2mp_launcher.constants import UPSTREAM
        dll = b"MZ-eo"
        pin = dict(UPSTREAM["extraOptions"], dllSha256=hashlib.sha256(dll).hexdigest(), files={}, fileSha256={})
        saved = set(shim.SUPPORTED_EXTRA_SHA256)
        shim.SUPPORTED_EXTRA_SHA256.add(hashlib.sha256(dll).hexdigest())
        self.addCleanup(lambda: (shim.SUPPORTED_EXTRA_SHA256.clear(), shim.SUPPORTED_EXTRA_SHA256.update(saved)))
        with mock.patch.dict(UPSTREAM, extraOptions=pin):
            shim.install_extra(self.az, {"winmm.dll": dll, "FH2_ExtraOptions.ini": self.INI})
        with open(os.path.join(self.az, "FH2_ExtraOptions.ini"), "rb") as f:
            self.assertEqual(f.read(), shim.swap_stick_bindings_bytes(self.INI)[0])

    def test_existing_settings_are_swapped_once_and_the_players_later_choice_stays(self):
        from fh2mp_launcher import core
        from fh2mp_launcher.settings import Settings
        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, FH2MP_LAUNCHER_HOME=home):
            lc = core.Launcher(Settings(os.path.join(home, "settings.json")), load_identity=False)
            lc.settings["azimuth_dir"] = self.az
            self.assertEqual(lc.ensure_controls(), [])                                      # no Extra Options settings yet: nothing decided
            self.assertFalse(lc.settings["controls_swapped"])
            ini = os.path.join(self.az, "FH2_ExtraOptions.ini")
            with open(ini, "wb") as f:
                f.write(self.INI)
            self.assertTrue(lc.ensure_controls())
            self.assertTrue(lc.settings["controls_swapped"])
            self.assertTrue(os.path.exists(ini + ".before-fh2mp-controls"))
            with open(ini, "wb") as f:
                f.write(self.INI)                                                           # the player puts RightStick back
            self.assertEqual(lc.ensure_controls(), [])
            with open(ini, "rb") as f:
                self.assertEqual(f.read(), self.INI)

    def write_note(self, text):
        d = os.path.join(self.az, "data", "bin")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, shim.CONTROLS_NOTE_FILE), "w", newline="") as f:
            f.write(text)

    def test_the_shims_controls_note_is_read(self):
        self.assertIsNone(shim.read_controls_note(self.az))
        self.write_note("horn=RightStick\r\nmovable=1\r\nwhy=moved\r\n")
        self.assertEqual(shim.read_controls_note(self.az), {"horn": "RightStick", "movable": True, "why": "moved"})
        self.write_note("horn=LeftStick\r\nmovable=0\r\nwhy=unknown HonkHonk build\r\n")
        self.assertEqual(shim.read_controls_note(self.az), {"horn": "LeftStick", "movable": False, "why": "unknown HonkHonk build"})
        self.write_note("garbage")
        self.assertIsNone(shim.read_controls_note(self.az))

    def test_restore_puts_only_the_left_stick_toggles_back(self):
        swapped = shim.swap_stick_bindings_bytes(self.INI)[0]
        self.assertEqual(shim.restore_stick_bindings_bytes(swapped), (self.INI.replace(b"rightstick ", b"RightStick "), 2))
        custom = b"[Bindings]\r\nHeadlightsXbox=LB+LeftStick\r\nRoofXbox=DPadDown\r\nAutodriveXbox=LeftStick\r\n"
        self.assertEqual(shim.restore_stick_bindings_bytes(custom), (custom, 0))

    def test_toggles_go_back_to_the_right_stick_when_the_horn_could_not_move(self):
        """A player who already has the swapped settings (controls_swapped, .before-fh2mp-controls) and a game whose shim could not put the horn on
        the right stick: headlights/roof return to RightStick with every other setting kept, and move again once a game can move the horn."""
        from fh2mp_launcher import core
        from fh2mp_launcher.settings import Settings
        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, FH2MP_LAUNCHER_HOME=home):
            lc = core.Launcher(Settings(os.path.join(home, "settings.json")), load_identity=False)
            lc.settings["azimuth_dir"] = self.az
            ini = os.path.join(self.az, "FH2_ExtraOptions.ini")
            with open(ini, "wb") as f:
                f.write(self.INI)
            self.assertTrue(lc.ensure_controls())
            self.assertTrue(lc.settings["controls_swapped"])
            with open(ini, "rb") as f:
                edited = f.read().replace(b"HeadlightsKeyboard=F3", b"HeadlightsKeyboard=F9")   # the player changes another setting afterwards
            with open(ini, "wb") as f:
                f.write(edited)
            self.write_note("horn=RightStick\r\nmovable=1\r\nwhy=moved\r\n")
            self.assertEqual(lc.ensure_controls(), [])                                      # the horn is on the right stick: nothing to do
            self.write_note("horn=LeftStick\r\nmovable=0\r\nwhy=unknown HonkHonk build\r\n")
            did = lc.ensure_controls()
            self.assertTrue(did and "unknown HonkHonk build" in did[0])
            self.assertFalse(lc.settings["controls_swapped"])
            with open(ini, "rb") as f:
                self.assertEqual(f.read(), shim.restore_stick_bindings_bytes(edited)[0])
            with open(ini, "rb") as f:
                text = f.read()
            self.assertIn(b"HeadlightsXbox=RightStick", text)
            self.assertIn(b"HeadlightsKeyboard=F9", text)
            self.assertIn(b"SignalLeftXbox=DPadLeft", text)
            self.assertTrue(os.path.exists(ini + ".before-fh2mp-restore"))
            self.assertEqual(lc.ensure_controls(), [])                                      # the same note again: no swap back and forth
            with open(ini, "rb") as f:
                self.assertIn(b"HeadlightsXbox=RightStick", f.read())
            self.write_note("horn=LeftStick\r\nmovable=1\r\nwhy=Extra Options uses the right stick\r\n")   # a later game could move it
            self.assertTrue(lc.ensure_controls())
            self.assertTrue(lc.settings["controls_swapped"])
            with open(ini, "rb") as f:
                self.assertIn(b"HeadlightsXbox=LeftStick", f.read())

    def test_play_stops_with_a_clear_message_when_honkhonk_cannot_be_installed(self):
        from fh2mp_launcher import core
        from fh2mp_launcher.settings import Settings
        with tempfile.TemporaryDirectory() as home, mock.patch.dict(os.environ, FH2MP_LAUNCHER_HOME=home):
            lc = core.Launcher(Settings(os.path.join(home, "settings.json")), load_identity=False)
            lc.settings["azimuth_dir"] = self.az
            self.assertEqual(lc.honk_state(), (False, ""))
            with mock.patch.object(core.upstream, "fetch", side_effect=core.upstream.UpstreamError("offline")):
                with self.assertRaises(core.updater.UpdateError):
                    lc.ensure_honk()
            with mock.patch.object(core.upstream, "fetch", return_value=self.files):
                self.assertTrue(lc.ensure_honk())
            self.assertEqual(lc.honk_state(), (True, shim.honk_version()))


class AchievementsModTests(unittest.TestCase):
    """The Achievements mod (shim.install_achievements, Launcher.ensure_achievements): optional. Only its DLL comes from the release; it is
    installed where there is none, with its update check off; a player's own Achievements.dll or settings file is never replaced."""

    def setUp(self):
        self.az = tempfile.mkdtemp()
        self.dll = b"MZ-achievements"

    def path(self, name):
        return os.path.join(self.az, "fh2-mods", name)

    def test_the_pin_takes_the_dll_only(self):
        from fh2mp_launcher import upstream
        from fh2mp_launcher.constants import UPSTREAM
        pin = UPSTREAM["achievements"]
        self.assertTrue(pin["url"].startswith("https://github.com/Trinculo54/fh2-achievements/releases/download/v1.0.2/"))
        self.assertEqual(pin["files"], {"fh2-mods/Achievements.dll": "fh2-mods/Achievements.dll"})
        data, pin = fake_release("achievements", {"fh2-mods/Achievements.dll": self.dll, "DWrite.dll": b"MZ-platform", "d3dcompiler_47.dll": b"MZ-platform",
                                                  "README.txt": b"r"}, "fh2-mods/Achievements.dll")
        self.assertEqual(upstream.extract("achievements", data, pin), {"fh2-mods/Achievements.dll": self.dll})

    def test_install_where_there_is_none_turns_the_update_check_off(self):
        from fh2mp_launcher.constants import UPSTREAM
        with mock.patch.dict(UPSTREAM["achievements"], dllSha256=hashlib.sha256(self.dll).hexdigest()):
            self.assertEqual(shim.achievements_installed(self.az), (False, ""))
            self.assertTrue(shim.install_achievements(self.az, {"fh2-mods/Achievements.dll": self.dll}))
            self.assertEqual(shim.achievements_installed(self.az), (True, "1.0.2"))
            with open(self.path("Achievements.ini"), "rb") as f:
                ini = f.read()
            self.assertIn(b"\r\nCheckUpdates=0\r\n", ini)
            self.assertIn(b"\r\nPopups=1\r\n", ini)
            self.assertEqual(shim.install_achievements(self.az, {"fh2-mods/Achievements.dll": self.dll}), [])
            self.assertFalse(os.path.exists(os.path.join(self.az, "DWrite.dll")))

    def test_the_players_own_build_and_settings_stay(self):
        os.makedirs(os.path.join(self.az, "fh2-mods"))
        with open(self.path("Achievements.dll"), "wb") as f:
            f.write(b"MZ-newer")
        self.assertEqual(shim.achievements_installed(self.az), (True, "unknown"))
        self.assertEqual(shim.install_achievements(self.az, {"fh2-mods/Achievements.dll": self.dll}), [])
        with open(self.path("Achievements.dll"), "rb") as f:
            self.assertEqual(f.read(), b"MZ-newer")
        self.assertFalse(os.path.exists(self.path("Achievements.ini")))
        # a missing DLL beside the player's settings file: the DLL comes, the file stays theirs
        os.remove(self.path("Achievements.dll"))
        with open(self.path("Achievements.ini"), "wb") as f:
            f.write(b"[Achievements]\r\nPopups=0\r\n")
        self.assertTrue(shim.install_achievements(self.az, {"fh2-mods/Achievements.dll": self.dll}))
        with open(self.path("Achievements.ini"), "rb") as f:
            self.assertEqual(f.read(), b"[Achievements]\r\nPopups=0\r\n")

    def test_the_launcher_installs_it_only_when_wanted_and_missing(self):
        from fh2mp_launcher import core, upstream
        from fh2mp_launcher.settings import Settings
        lc = core.Launcher(Settings(os.path.join(self.az, "settings.json")), load_identity=False)   # never the player's own settings file
        lc.settings["azimuth_dir"] = self.az
        with mock.patch.object(upstream, "fetch", return_value={"fh2-mods/Achievements.dll": self.dll}) as fetch:
            lc.settings["achievements"] = False
            self.assertEqual(lc.ensure_achievements(), [])
            fetch.assert_not_called()
            lc.settings["achievements"] = True
            self.assertTrue(lc.ensure_achievements())
            self.assertEqual(fetch.call_args[0][0], "achievements")
            self.assertEqual(lc.ensure_achievements(), [])       # there now: nothing is downloaded again
            self.assertEqual(fetch.call_count, 1)


class AchToastTests(unittest.TestCase):
    """The achievement toast's files (Launcher.ensure_ach_toast): only from the FH2MP mirror, checked by the pinned hash, downloaded once."""

    def test_the_pins_are_mirror_files_with_hashes(self):
        from fh2mp_launcher.constants import ACH_TOAST, MIRROR_BASE
        self.assertTrue(ACH_TOAST["base"].startswith(MIRROR_BASE))
        self.assertIn("ach-sound-rare.wav", ACH_TOAST["files"])
        self.assertIn("TrophyIntro.png", ACH_TOAST["files"])
        for name, (sha, size) in ACH_TOAST["files"].items():
            self.assertRegex(sha, r"^[0-9a-f]{64}$", name)
            self.assertGreater(size, 0, name)

    def test_files_in_place_are_not_downloaded_again(self):
        from fh2mp_launcher import core, updater
        from fh2mp_launcher.constants import ACH_TOAST
        from fh2mp_launcher.settings import Settings
        az = tempfile.mkdtemp()
        lc = core.Launcher(Settings(os.path.join(az, "settings.json")), load_identity=False)
        lc.settings["azimuth_dir"] = az
        data = b"RIFF-sound"
        files = {"ach-sound-rare.wav": (hashlib.sha256(data).hexdigest(), len(data))}
        def fake(self, url, entry, progress=None, cancelled=lambda: False, dest_dir=None):
            self_calls.append(url)
            path = os.path.join(dest_dir, entry["name"])
            with open(path, "wb") as f:
                f.write(data)
            return path
        self_calls = []
        with mock.patch.dict(ACH_TOAST, files=files), mock.patch.object(updater.Updater, "download_url", fake):
            lc.settings["achievements"] = False
            self.assertEqual(lc.ensure_ach_toast(), [])
            lc.settings["achievements"] = True
            self.assertTrue(lc.ensure_ach_toast())
            self.assertEqual(self_calls, [ACH_TOAST["base"] + "ach-sound-rare.wav"])
            self.assertEqual(lc.ensure_ach_toast(), [])
            self.assertEqual(len(self_calls), 1)


class AnnaTests(unittest.TestCase):
    """FH2 ANNA (shim.install_anna, anna_bindings): only its DLL comes from the release, the platform build in its zip stays out; Extra Options'
    D-pad autodrive and hazard buttons go to None only where they are still the defaults."""
    INI = (b"[Bindings]\r\n; comment AutodriveXbox=DPadDown\r\nAutodriveXbox=DPadDown\r\nAutodriveKeyboard=F2\r\n"
           b"hazardsxbox = dpadup \r\nHeadlightsXbox=LeftStick\r\n")

    def setUp(self):
        self.az = tempfile.mkdtemp()
        self.dll = b"MZ-anna"

    def test_the_pin_takes_the_dll_only(self):
        from fh2mp_launcher import upstream
        from fh2mp_launcher.constants import UPSTREAM
        pin = UPSTREAM["anna"]
        self.assertTrue(pin["url"].startswith("https://github.com/qiangqiang101/FH2_ANNA/releases/download/1.1.1/"))
        self.assertEqual(pin["files"], {"fh2-mods/FH2ANNA.dll": "fh2-mods/FH2ANNA.dll"})
        data, pin = fake_release("anna", {"fh2-mods/FH2ANNA.dll": self.dll, "DWrite.dll": b"MZ-platform", "README.md": b"r"}, "fh2-mods/FH2ANNA.dll")
        self.assertEqual(upstream.extract("anna", data, pin), {"fh2-mods/FH2ANNA.dll": self.dll})

    def test_install_keeps_another_build_and_reports_it(self):
        from fh2mp_launcher.constants import UPSTREAM
        with mock.patch.dict(UPSTREAM["anna"], dllSha256=hashlib.sha256(self.dll).hexdigest()):
            self.assertEqual(shim.anna_installed(self.az), (False, ""))
            os.makedirs(os.path.join(self.az, "fh2-mods"))
            with open(os.path.join(self.az, "fh2-mods", "FH2ANNA.dll"), "wb") as f:
                f.write(b"MZ-other")
            self.assertEqual(shim.anna_installed(self.az), (True, "unknown"))
            self.assertFalse(shim.anna_complete(self.az))
            self.assertTrue(shim.install_anna(self.az, {"fh2-mods/FH2ANNA.dll": self.dll}))
            self.assertTrue(shim.anna_complete(self.az))
            with open(os.path.join(self.az, "fh2-mods", "FH2ANNA.dll.before-fh2mp-setup"), "rb") as f:
                self.assertEqual(f.read(), b"MZ-other")
            self.assertEqual(shim.install_anna(self.az, {"fh2-mods/FH2ANNA.dll": self.dll}), [])
            self.assertFalse(os.path.exists(os.path.join(self.az, "DWrite.dll")))

    def test_extra_options_d_pad_buttons_go_to_none_only_from_their_defaults(self):
        new, n = shim.anna_bindings_bytes(self.INI)
        self.assertEqual(n, 2)
        self.assertEqual(new, self.INI.replace(b"\r\nAutodriveXbox=DPadDown", b"\r\nAutodriveXbox=None").replace(b"= dpadup ", b"= None "))
        mine = b"AutodriveXbox=DPadUp\r\nHazardsXbox=DPadDown\r\n"                  # swapped by the player: theirs
        self.assertEqual(shim.anna_bindings_bytes(mine), (mine, 0))

    def test_the_bindings_move_once(self):
        from fh2mp_launcher import core
        from fh2mp_launcher.settings import Settings
        with open(os.path.join(self.az, shim.EXTRA_INI), "wb") as f:
            f.write(self.INI)
        lc = core.Launcher(Settings(os.path.join(self.az, "settings.json")), load_identity=False)   # never the player's own settings file
        lc.settings["azimuth_dir"] = self.az
        lc.settings["anna_bindings_moved"] = False
        self.assertTrue(lc.ensure_anna_bindings())
        with open(os.path.join(self.az, shim.EXTRA_INI), "wb") as f:
            f.write(self.INI)                                                          # the player puts them back
        self.assertEqual(lc.ensure_anna_bindings(), [])
        with open(os.path.join(self.az, shim.EXTRA_INI), "rb") as f:
            self.assertEqual(f.read(), self.INI)
