"""Another Forza in the FH2 folder (#470: FH3 1.0.125.0 "Opus" ships the same Mount\\forza_durango_release_final.exe as FH2 1.0.0.64
"Anthem"), its crashes kept out of FH2MP reports, the binding-tier stop's advice (#467) and the GPU named when Azimuth did not (#467)."""
import os
import shutil
import sys
import tempfile
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from fh2mp_launcher import azimuth, crashreport, setup, settings as settings_mod  # noqa: E402

MANIFEST = ('<?xml version="1.0" encoding="utf-8"?>\r\n<Package xmlns="http://schemas.microsoft.com/appx/2010/manifest">\r\n'
            '  <Identity Name="{name}" Publisher="CN=Microsoft Corporation, O=Microsoft Corporation, L=Redmond, S=Washington, C=US" '
            'Version="{version}" ProcessorArchitecture="x64" />\r\n</Package>\r\n')


def game_dir(root, name, version, manifest=True):
    d = os.path.join(root, name)
    os.makedirs(os.path.join(d, "Mount"))
    os.makedirs(os.path.join(d, "EmbeddedXvd"))
    with open(os.path.join(d, "Mount", "forza_durango_release_final.exe"), "wb") as f:
        f.write(b"MZ")
    if manifest:
        with open(os.path.join(d, "Mount", "AppxManifest.xml"), "w", encoding="utf-8") as f:
            f.write(MANIFEST.format(name=name, version=version))
    return d


def ini(folder, game_folder, game=azimuth.GAME_ID):
    os.makedirs(os.path.join(folder, "data"), exist_ok=True)
    text = f"[launcher]\r\ngame={game}\r\n[title-{azimuth.GAME_ID}]\r\nfolder={game_folder}\r\n"
    with open(os.path.join(folder, "data", "azimuth.ini"), "wb") as f:
        f.write(text.encode("utf-16"))


class Folders(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root, True)
        self.fh2 = game_dir(self.root, "Anthem", "1.0.0.64")
        self.fh3 = game_dir(self.root, "Opus", "1.0.125.0")

    def test_the_manifest_tells_fh3_from_fh2(self):
        self.assertEqual(azimuth.package_identity(self.fh3), ("Opus", "1.0.125.0"))
        self.assertEqual(azimuth.other_game_name(self.fh2), "")
        self.assertEqual(azimuth.other_game_name(self.fh3), "Forza Horizon 3 (Opus 1.0.125.0)")
        self.assertTrue(azimuth.is_game_dir(self.fh2))
        self.assertFalse(azimuth.is_game_dir(self.fh3))

    def test_no_manifest_or_an_unknown_package(self):
        bare = game_dir(self.root, "Bare", "", manifest=False)
        self.assertEqual(azimuth.other_game_name(bare), "")             # nothing to go by: the exe check decides
        self.assertTrue(azimuth.is_game_dir(bare))
        other = game_dir(self.root, "Mystery", "2.0.0.1")
        self.assertEqual(azimuth.other_game_name(other), "another game (Mystery 2.0.0.1)")
        self.assertEqual(azimuth.other_game_name(os.path.join(self.root, "missing")), "")
        self.assertEqual(azimuth.other_game_name(r"C:\Games\ForzaHorizon2" if os.name != "nt" else "Q:\\nowhere"), "")

    def test_setup_refuses_the_fh3_folder(self):
        self.assertEqual(setup.game_folder(self.fh2), os.path.abspath(self.fh2))
        with self.assertRaisesRegex(ValueError, "Forza Horizon 3"):
            setup.game_folder(self.fh3)

    def test_an_fh2_entry_that_points_at_fh3_is_not_a_configured_game(self):
        az = os.path.join(self.root, "Azimuth")
        with open(os.path.join(self.root, "azimuth.exe"), "wb"):
            pass
        ini(az, self.fh3)
        self.assertFalse(azimuth.has_game_configured(az))
        ini(az, self.fh2)
        self.assertTrue(azimuth.has_game_configured(az))

    def test_inspect_names_the_other_game(self):
        az = os.path.join(self.root, "Azimuth")
        os.makedirs(az)
        with open(os.path.join(az, "azimuth.exe"), "wb") as f:
            f.write(b"MZ")
        ini(az, self.fh3)
        info = azimuth.inspect(az)
        self.assertFalse(info.game_ok)
        self.assertEqual(info.game_other, "Forza Horizon 3 (Opus 1.0.125.0)")
        ini(az, self.fh2)
        info = azimuth.inspect(az)
        self.assertTrue(info.game_ok)
        self.assertEqual(info.game_other, "")


class Reports(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.root, True)
        self.fh2 = game_dir(self.root, "Anthem", "1.0.0.64")
        self.fh3 = game_dir(self.root, "Opus", "1.0.125.0")
        self.az = os.path.join(self.root, "Azimuth")
        os.makedirs(os.path.join(self.az, "data", "logs"))
        env = mock.patch.dict(os.environ, {"FH2MP_LAUNCHER_HOME": os.path.join(self.root, "home")})
        env.start()
        self.addCleanup(env.stop)
        self.st = settings_mod.Settings(os.path.join(self.root, "settings.json"))
        self.st["azimuth_dir"] = self.az

    def session(self, game, line, now):
        stamp = crashreport.datetime.datetime.fromtimestamp(now - 30, crashreport.datetime.timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
        log = os.path.join(self.az, "data", "logs", "azimuth.log")
        with open(log, "w", encoding="utf-8") as f:
            f.write(f"100 tid=1 session S began {stamp} UTC: Azimuth 1.0.4 (azimuth.exe) on Windows 10.0.19045\n"
                    f"120 tid=1 settings: render height 1080, 60 fps, HDR off; game folder {game}, from its title's section in "
                    f"{self.az}\\data\\azimuth.ini\n{line}\n")
        os.utime(log, (now, now))

    def test_a_crash_of_fh3_is_not_offered_and_is_handled(self):
        now = time.time()
        self.st["crash_handled"] = now - 1000
        self.session(self.fh3, "300 tid=5 CRASHED exception 0x80000003 at game+0x367b9f on thread 5", now)
        self.assertIsNone(crashreport.check(self.st, now))
        self.assertGreaterEqual(self.st["crash_handled"], now)
        self.session(self.fh2, "300 tid=5 CRASHED exception 0xc0000005 at game+0x12e0df5 on thread 5", now + 5)
        crash = crashreport.check(self.st, now + 6)
        self.assertIsNotNone(crash)
        self.assertEqual(crash.game, self.fh2)

    def test_binding_tier_stop_gets_its_advice(self):
        crash = crashreport.Crash("folder", "STOPPED the D3D12 driver's resource binding tier is below 3 at azimuth+0x573e3", None, 0.0)
        hint = crashreport.known_cause(crash)
        self.assertIn("binding tier 3", hint)
        self.assertIn("High performance", hint)


@unittest.skipUnless(os.name == "nt", "the registry fallback is Windows'")
class SystemGpu(unittest.TestCase):
    def test_names_come_from_the_display_class(self):
        gpu, driver = crashreport.system_gpu()
        self.assertIsInstance(gpu, str)
        self.assertNotIn("Microsoft Basic", gpu)
        if gpu:
            self.assertTrue(driver)


if __name__ == "__main__":
    unittest.main()
