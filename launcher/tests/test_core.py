"""Headless tests of the launcher core. Server-facing tests need FH2MP_TEST_SERVER (http://127.0.0.1:port of a test LSP)."""
import hashlib
import json
import os
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from fh2mp_launcher import azimuth, core, identity, shim, updater  # noqa: E402
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
        # known-answer test shared with server/auth.py: a fixed seed must sign a fixed message identically
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

    def me(self, on):
        from types import SimpleNamespace
        from fh2mp_launcher import core
        me = SimpleNamespace(settings={"tcp_fallback": on}, tunnel=None, game_env={"FH2MP_SERVER_PORT": "5000"})
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


class GamerpicSyncTests(unittest.TestCase):
    """sync_gamerpic: the server's picture reaches both Gamerpic.png files at every start (GitHub issue: the mod's default picture stayed)."""

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
                body = data if "ok.bin" in self.path else data[:-1] + b"X"
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
        with self.assertRaises(launch.LaunchError):
            launch.resolve_proton(d)                              # present but not executable
        if sys.platform != "win32":
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
        self.assertEqual(env["WINEDLLOVERRIDES"], "winmm,d3dcompiler_47=n,b")      # Extra Options and the FH2 Mod Platform


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


def platform_dll(version):
    """A stand-in d3dcompiler_47.dll with the FH2 Mod Platform's version resource strings."""
    u = lambda s: s.encode("utf-16-le")  # noqa: E731
    return b"MZ" + u("ProductName\0\0FH2 Mod Platform\0") + u("ProductVersion\0") + b"\x00\x00" + u(version + "\0")


def platform_files(version="2.7.1"):
    return {"d3dcompiler_47.dll": platform_dll(version), "LICENSE-FH2ModPlatform.txt": b"MIT License Trinculo54",
            "LICENSE-MinHook.txt": b"MinHook BSD"}


class ShimTests(unittest.TestCase):
    DLL = b"MZ....FH2MP-SHIM-VERSION:0.1.0:END...."

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
            self.assertEqual(shim.platform_installed(d), ("platform", "2.7.1"))
            for n in ("LICENSE-FH2ModPlatform.txt", "LICENSE-MinHook.txt"):
                self.assertTrue(os.path.isfile(os.path.join(d, n)), n)
            self.assertTrue(any("FH2 Mod Platform 2.7.1" in s for s in did))
            self.assertFalse(os.path.exists(os.path.join(d, "winmm.dll")))

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

    def test_platform_newer_kept_older_and_foreign_replaced(self):
        with tempfile.TemporaryDirectory() as d:
            self.write(platform_dll("2.8.0"), d, "d3dcompiler_47.dll")
            shim.install(d, self.DLL, platform=platform_files())
            self.assertEqual(shim.platform_installed(d), ("platform", "2.8.0"))          # another mod's newer copy stays
            self.write(platform_dll("2.6.1"), d, "d3dcompiler_47.dll")
            self.assertEqual(shim.installed(d), ("old", "0.1.0"))                        # too old for this shim: install again
            shim.install(d, self.DLL, platform=platform_files())
            self.assertEqual(shim.platform_installed(d), ("platform", "2.7.1"))
            self.write(b"MZ fh2wheel loader", d, "d3dcompiler_47.dll")                     # fh2wheel's own loader
            self.assertEqual(shim.platform_installed(d), ("foreign", ""))
            did = shim.install(d, self.DLL, platform=platform_files())
            self.assertEqual(shim.platform_installed(d), ("platform", "2.7.1"))
            self.assertEqual(self.read(d, "d3dcompiler_47.dll.old"), b"MZ fh2wheel loader")
            self.assertTrue(any("d3dcompiler_47.dll.old" in s for s in did))

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
            self.assertEqual(shim.platform_installed(d), ("platform", "2.7.1"))

    def test_shim_zip(self):
        from fh2mp_launcher import core, updater
        with tempfile.TemporaryDirectory() as d:
            zp = os.path.join(d, "fh2mp-shim-0.1.0.zip")
            with zipfile.ZipFile(zp, "w") as z:
                z.writestr("fh2-mods/FH2MP.dll", self.DLL)
                z.writestr("fh2mp/LICENSE.txt", b"GPL")
            self.assertEqual(core.read_shim_zip(zp), self.DLL)
            with zipfile.ZipFile(zp, "w") as z:
                z.writestr("winmm.dll", self.DLL)                                          # an rc5 shim zip
            with self.assertRaises(updater.UpdateError):
                core.read_shim_zip(zp)

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
        info = azimuth.inspect(RIG)
        self.assertEqual((info.supported, info.version), (True, "0.9.10"))


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
        files = {"Azimuth/winmm.dll": b"MZ-eo", "Azimuth/FH2_ExtraOptions.ini": b"ini", "Azimuth/data/bin/azimuth-tls.dll": b"stub",
                 "Mezcal/xg.dll": b"other"}
        data, pin = fake_release("extraOptions", files, "Azimuth/winmm.dll")
        self.assertEqual(upstream.extract("extraOptions", data, pin), {"winmm.dll": b"MZ-eo", "FH2_ExtraOptions.ini": b"ini"})
        with self.assertRaises(upstream.UpstreamError):
            upstream.extract("extraOptions", data + b"x", pin)                         # not the pinned zip
        with self.assertRaises(upstream.UpstreamError):
            upstream.extract("extraOptions", data, dict(pin, dllSha256="0" * 64))     # the DLL inside is not the pinned build
        pdata, ppin = fake_release("platform", {"d3dcompiler_47.dll": b"MZ-plat", "LICENSE.txt": b"MIT", "LICENSE-MinHook.txt": b"BSD",
                                                "README.txt": b"r"}, "d3dcompiler_47.dll")
        self.assertEqual(upstream.extract("platform", pdata, ppin),
                         {"d3dcompiler_47.dll": b"MZ-plat", "LICENSE-FH2ModPlatform.txt": b"MIT", "LICENSE-MinHook.txt": b"BSD"})

    def test_pins_point_at_the_authors_releases(self):
        from fh2mp_launcher.constants import UPSTREAM
        self.assertTrue(UPSTREAM["platform"]["url"].startswith("https://github.com/Trinculo54/fh2-mod-platform/releases/download/"))
        self.assertTrue(UPSTREAM["extraOptions"]["url"].startswith("https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions/releases/download/"))
        self.assertEqual(set(UPSTREAM["platform"]["files"].values()), set(shim.PLATFORM_FILES))


class ExtraOptionsTests(unittest.TestCase):
    INI = b"; Forza Horizon 2 Extra Options\n; Release v0.2.88\n; Compatible with Azimuth 0.9.10\n[Global]\nEnabled=1\n"

    def setUp(self):
        self._saved = set(shim.SUPPORTED_EXTRA_SHA256)
        for b in (b"MZ-mod-dll", b"MZ-release"):                                        # stand-ins for the one supported build
            shim.SUPPORTED_EXTRA_SHA256.add(hashlib.sha256(b).hexdigest())
        self.addCleanup(lambda: (shim.SUPPORTED_EXTRA_SHA256.clear(), shim.SUPPORTED_EXTRA_SHA256.update(self._saved)))

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
            self.assertEqual(shim.install_extra(az, self.release()), [])
            self.assertEqual(self.read(az, "winmm.dll"), b"MZ-mod-dll")

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
            os.utime(os.path.dirname(p), (when, when))
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
        self.home = tempfile.TemporaryDirectory()
        self.old = {k: os.environ.get(k) for k in ("FH2MP_DEV", "FH2MP_LAUNCHER_HOME", "XDG_CONFIG_HOME", "FH2MP_NO_KEYRING")}
        os.environ["FH2MP_NO_KEYRING"] = "1"
        os.environ.pop("FH2MP_LAUNCHER_HOME", None)
        os.environ["XDG_CONFIG_HOME"] = self.home.name

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
        self.assertTrue(dev.endswith("fh2mp-dev"))
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
            for name, body in (("azimuth.exe", b"MZ"), ("d3dcompiler_47.dll", platform_dll("2.7.1"))):     # a bare DLL needs the platform
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
            self.assertFalse(lc.shim_outdated())

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
