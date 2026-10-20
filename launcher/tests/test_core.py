"""Headless tests of the launcher core. Server-facing tests need FH2MP_TEST_SERVER (http://127.0.0.1:port of a test LSP)."""
import hashlib
import json
import os
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from fh2mp_launcher import azimuth, identity, shim, updater  # noqa: E402
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


class UpdaterTests(unittest.TestCase):
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


class ShimTests(unittest.TestCase):
    DLL = b"MZ....FH2MP-SHIM-VERSION:0.1.0:END...."

    def test_install_over_foreign_dll_moves_it(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "winmm.dll"), "wb") as f:
                f.write(b"MZ extra options")
            with open(os.path.join(d, "FH2_ExtraOptions.ini"), "w") as f:
                f.write("x")
            did = shim.install(d, self.DLL)
            self.assertEqual(shim.installed(d), ("ours", "0.1.0"))
            with open(os.path.join(d, "fh2extra", "winmm.dll"), "rb") as f:
                self.assertEqual(f.read(), b"MZ extra options")
            self.assertTrue(os.path.exists(os.path.join(d, "fh2extra", "FH2_ExtraOptions.ini")))
            self.assertTrue(any("moved" in x for x in did))

    def test_update_keeps_previous_and_rejects_junk(self):
        with tempfile.TemporaryDirectory() as d:
            shim.install(d, self.DLL)
            shim.install(d, self.DLL.replace(b"0.1.0", b"0.2.0"))
            self.assertEqual(shim.installed(d), ("ours", "0.2.0"))
            self.assertEqual(shim.inspect_dll(os.path.join(d, "winmm.dll.prev")), ("ours", "0.1.0"))
            with self.assertRaises(shim.ShimError):
                shim.install(d, b"not a shim")
            with self.assertRaises(shim.ShimError):
                shim.install(d, self.DLL, expect_sha256="0" * 64)

    def test_uninstall_restores_extra_options(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "winmm.dll"), "wb") as f:
                f.write(b"MZ extra options")
            shim.install(d, self.DLL)
            shim.uninstall(d)
            self.assertEqual(shim.installed(d), ("foreign", ""))

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


class ExtraOptionsTests(unittest.TestCase):
    INI = b"; Forza Horizon 2 Extra Options\n; Release v0.2.88\n; Compatible with Azimuth 0.9.10\n[Global]\nEnabled=1\n"

    def setUp(self):
        self._saved = set(shim.SUPPORTED_EXTRA_SHA256)
        for b in (b"MZ-mod-dll", b"MZ-new-dll", b"MZ-bundled-mod", b"MZ-other"):          # stand-ins for the one supported build
            shim.SUPPORTED_EXTRA_SHA256.add(hashlib.sha256(b).hexdigest())
        self.addCleanup(lambda: (shim.SUPPORTED_EXTRA_SHA256.clear(), shim.SUPPORTED_EXTRA_SHA256.update(self._saved)))

    def make_mod(self, d, nested=True):
        base = os.path.join(d, "fh2extra") if nested else d
        os.makedirs(base, exist_ok=True)
        for n, b in (("winmm.dll", b"MZ-mod-dll"), ("FH2_ExtraOptions.ini", self.INI), ("FH2_ExtraOptions.dat", b"user-options")):
            with open(os.path.join(base, n), "wb") as f:
                f.write(b)
        return base

    def test_import_from_another_azimuth_folder_and_zip(self):
        with tempfile.TemporaryDirectory() as old, tempfile.TemporaryDirectory() as new, tempfile.TemporaryDirectory() as z:
            self.make_mod(old)
            self.assertEqual(shim.inspect_extra_source(os.path.join(old, "fh2extra"))["version"], "0.2.88")
            self.assertEqual(shim.extra_options_installed(new), (False, "", ""))
            self.assertIn("0.2.88", shim.import_extra_options(new, old)[0])
            self.assertEqual(shim.extra_options_installed(new), (True, "0.2.88", "0.9.10"))
            with open(os.path.join(new, "fh2extra", "FH2_ExtraOptions.dat"), "rb") as f:
                self.assertEqual(f.read(), b"user-options")
            # a zip with the usual Azimuth/ sub folder; existing settings are kept, the DLL is replaced
            with open(os.path.join(new, "fh2extra", "FH2_ExtraOptions.ini"), "ab") as f:
                f.write(b"Mine=1\n")
            import zipfile
            zp = os.path.join(z, "FH2_Extra_Options_0.2.88-release.zip")
            with zipfile.ZipFile(zp, "w") as zf:
                zf.writestr("Azimuth/winmm.dll", b"MZ-new-dll")
                zf.writestr("Azimuth/FH2_ExtraOptions.ini", self.INI)
                zf.writestr("Mezcal/xg.dll", b"other")
            shim.import_extra_options(new, zp)
            with open(os.path.join(new, "fh2extra", "winmm.dll"), "rb") as f:
                self.assertEqual(f.read(), b"MZ-new-dll")
            with open(os.path.join(new, "fh2extra", "FH2_ExtraOptions.ini"), "rb") as f:
                self.assertIn(b"Mine=1", f.read())

    def make_package(self, d, dll=b"MZ-bundled-mod", source=True):
        import zipfile
        zp = os.path.join(d, "fh2-extra-options-0.2.98.zip")
        with zipfile.ZipFile(zp, "w") as zf:
            zf.writestr("winmm.dll", dll)
            zf.writestr("FH2_ExtraOptions.ini", self.INI)
            zf.writestr("LICENSE.txt", b"GNU GENERAL PUBLIC LICENSE Version 3")
            zf.writestr("NOTICE.txt", b"by RokkuDayo")
            if source:
                zf.writestr("source/ForzaHorizon2_ExtraOptions-1ff0535f.zip", b"PK-src")
        return zp, hashlib.sha256(dll).hexdigest()

    def test_bundled_install_only_when_missing_and_keeps_licence_and_source(self):
        with tempfile.TemporaryDirectory() as pkg, tempfile.TemporaryDirectory() as az:
            zp, sha = self.make_package(pkg)
            did = shim.install_bundled_extra(az, zp, sha)
            self.assertIn("RokkuDayo", did[0])
            base = os.path.join(az, "fh2extra")
            for n in ("winmm.dll", "FH2_ExtraOptions.ini", "LICENSE.txt", "NOTICE.txt", os.path.join("source", "ForzaHorizon2_ExtraOptions-1ff0535f.zip")):
                self.assertTrue(os.path.isfile(os.path.join(base, n)), n)
            self.assertEqual(shim.extra_options_installed(az), (True, "0.2.88", "0.9.10"))
            # a second call (the supported mod is there now) changes nothing, even with another supported DLL in the package
            os.makedirs(os.path.join(pkg, "second"))
            zp2, sha2 = self.make_package(os.path.join(pkg, "second"), dll=b"MZ-other")
            self.assertEqual(shim.install_bundled_extra(az, zp2, sha2), [])
            with open(os.path.join(base, "winmm.dll"), "rb") as f:
                self.assertEqual(f.read(), b"MZ-bundled-mod")
            # another version (not the supported build): kept aside, the supported one goes in, settings stay
            with open(os.path.join(base, "winmm.dll"), "wb") as f:
                f.write(b"MZ-0.2.88-old")
            with open(os.path.join(base, "FH2_ExtraOptions.ini"), "ab") as f:
                f.write(b"Mine=1\n")
            did = shim.install_bundled_extra(az, zp, sha)
            self.assertIn("aside", did[0])
            with open(os.path.join(base, "winmm.dll"), "rb") as f:
                self.assertEqual(f.read(), b"MZ-bundled-mod")
            with open(os.path.join(base, "winmm.dll.unsupported-1"), "rb") as f:
                self.assertEqual(f.read(), b"MZ-0.2.88-old")
            with open(os.path.join(base, "FH2_ExtraOptions.ini"), "rb") as f:
                self.assertIn(b"Mine=1", f.read())

    def test_only_the_supported_build_is_imported(self):
        with tempfile.TemporaryDirectory() as old, tempfile.TemporaryDirectory() as new:
            base = self.make_mod(old)
            with open(os.path.join(base, "winmm.dll"), "wb") as f:
                f.write(b"MZ-some-older-release")
            self.assertIsNone(shim.inspect_extra_source(base))
            with self.assertRaises(shim.ShimError):
                shim.import_extra_options(new, old)
            self.assertFalse(shim.extra_options_supported(new))

    def test_bundled_install_checks_the_signed_hash_and_what_it_is(self):
        with tempfile.TemporaryDirectory() as pkg, tempfile.TemporaryDirectory() as az:
            zp, _sha = self.make_package(pkg)
            with self.assertRaises(shim.ShimError):
                shim.install_bundled_extra(az, zp, "0" * 64)                      # not what the manifest signed
            self.assertFalse(os.path.exists(os.path.join(az, "fh2extra", "winmm.dll")))
            ours, osha = self.make_package(pkg, dll=b"MZ FH2MP-SHIM-VERSION:1.0.0:END")
            with self.assertRaises(shim.ShimError):
                shim.install_bundled_extra(az, ours, osha)                        # our own shim is not the mod

    def test_refuses_what_is_not_the_mod(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "winmm.dll"), "wb") as f:
                f.write(b"MZ FH2MP-SHIM-VERSION:1.0.0:END")                 # our own shim is not the mod
            with open(os.path.join(d, "FH2_ExtraOptions.ini"), "wb") as f:
                f.write(self.INI)
            with self.assertRaises(shim.ShimError):
                shim.import_extra_options(tempfile.mkdtemp(), d)
            import zipfile
            zp = os.path.join(d, "old.zip")
            with zipfile.ZipFile(zp, "w") as zf:
                zf.writestr("Azimuth/azimuth-tls.dll", b"x")                  # the 0.2.74 layout: for an older Azimuth
                zf.writestr("Azimuth/FH2_ExtraOptions.ini", self.INI)
            with self.assertRaises(shim.ShimError):
                shim.import_extra_options(tempfile.mkdtemp(), zp)


class SaveAdoptionTests(unittest.TestCase):
    def make(self, d, *names):
        for n in names:
            p = os.path.join(d, "data", "titles", "1A96545B", "saves", "player-1", n, "revisions")
            os.makedirs(p)
            with open(os.path.join(p, "profile.bin"), "w") as f:
                f.write(n)

    def test_new_account_inherits_the_default_profile(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d, "User_9000000000000", "User_900001C7CE3F5")
            note = azimuth.adopt_saves(d, 0x000900004ADDC5F7)
            self.assertIn("User_9000000000000 -> User_900004ADDC5F7", note)
            root = os.path.join(d, "data", "titles", "1A96545B", "saves", "player-1")
            with open(os.path.join(root, "User_900004ADDC5F7", "revisions", "profile.bin")) as f:
                self.assertEqual(f.read(), "User_9000000000000")
            self.assertTrue(os.path.isdir(os.path.join(root, "User_9000000000000")))        # the original stays
            self.assertEqual(azimuth.adopt_saves(d, 0x000900004ADDC5F7), "")               # once only; never overwrites

    def test_falls_back_to_the_newest_profile_and_ignores_backups(self):
        with tempfile.TemporaryDirectory() as d:
            self.make(d, "User_AAA", "User_BBB", "User_CCC.fresh-backup")
            root = os.path.join(d, "data", "titles", "1A96545B", "saves", "player-1")
            os.utime(os.path.join(root, "User_AAA"), (1, 1))
            self.assertIn("User_BBB ->", azimuth.adopt_saves(d, 0x9000000000042))

    def test_nothing_to_adopt(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(azimuth.adopt_saves(d, 0x9000000000042), "")


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
            for name, body in (("azimuth.exe", b"MZ"),):
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


if __name__ == "__main__":
    unittest.main()
