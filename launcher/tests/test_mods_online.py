"""NitroSystem's "Load when playing online" (fh2-mods\\platform-online.txt) and the My setup rows for NitroSystem and Underglow."""
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from fh2mp_launcher import core, shim  # noqa: E402
from fh2mp_launcher.settings import Settings  # noqa: E402

WITH_SWITCH = b"MZ\x90\x00...FnFNitro_GetNitroPercent\x00FnFNitro_IsActive\x00FnFNitro_IsBlocked\x00FnFNitro_SetBlocked\x00..."
WITHOUT_SWITCH = b"MZ\x90\x00...FnFNitro_GetNitroPercent\x00..."


class NitroOnlineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.az = self.tmp.name
        os.makedirs(os.path.join(self.az, "fh2-mods"))

    def nitro(self, data):
        with open(os.path.join(self.az, "fh2-mods", shim.NITRO_DLL), "wb") as f:
            f.write(data)

    def online_text(self):
        with open(os.path.join(self.az, "fh2-mods", shim.ONLINE_LIST), encoding="utf-8-sig") as f:
            return f.read()

    def test_nothing_happens_without_nitrosystem(self):
        self.assertEqual(shim.nitro_online(self.az, ""), ("", ""))
        self.assertFalse(os.path.exists(os.path.join(self.az, "fh2-mods", shim.ONLINE_LIST)))

    def test_a_nitrosystem_with_the_switch_loads_online(self):
        self.nitro(WITH_SWITCH)
        self.assertEqual(shim.nitro_installed(self.az)[0], "switch")
        written, note = shim.nitro_online(self.az, "")
        self.assertEqual(written, "1")
        self.assertIn("loads for online play", note)
        text = self.online_text()
        self.assertTrue(text.startswith("# "))                       # the platform's own header
        self.assertIn("FnFNitro.dll\t1\n", text)
        self.assertEqual(shim.nitro_online(self.az, "1"), ("1", ""))  # nothing to do twice

    def test_a_nitrosystem_without_the_switch_stays_off_even_when_the_player_turned_it_on(self):
        self.nitro(WITHOUT_SWITCH)
        shim.set_online_value(self.az, "fnfnitro.dll", "1")           # the player's own choice on the MODS tab
        written, note = shim.nitro_online(self.az, "")
        self.assertEqual((written, shim.online_value(self.az, shim.NITRO_DLL)), ("0", "0"))
        self.assertIn("no off switch", note)
        self.assertEqual(self.online_text().lower().count("fnfnitro.dll"), 1)   # the line is replaced, not doubled

    def test_the_players_own_off_stays(self):
        self.nitro(WITH_SWITCH)
        shim.set_online_value(self.az, shim.NITRO_DLL, "0")            # turned off on the MODS tab, not by the launcher
        self.assertEqual(shim.nitro_online(self.az, ""), ("", ""))
        self.assertEqual(shim.online_value(self.az, shim.NITRO_DLL), "0")
        self.assertEqual(shim.nitro_online(self.az, "1"), ("1", ""))   # the launcher wrote 1, the player turned it off since: theirs
        self.assertEqual(shim.online_value(self.az, shim.NITRO_DLL), "0")

    def test_an_update_to_a_nitrosystem_with_the_switch_turns_the_launchers_off_back_on(self):
        self.nitro(WITHOUT_SWITCH)
        written, _ = shim.nitro_online(self.az, "")
        self.assertEqual(written, "0")
        self.nitro(WITH_SWITCH)
        written, note = shim.nitro_online(self.az, written)
        self.assertEqual((written, shim.online_value(self.az, shim.NITRO_DLL)), ("1", "1"))
        self.assertTrue(note)

    def test_other_mods_lines_and_comments_stay(self):
        path = os.path.join(self.az, "fh2-mods", shim.ONLINE_LIST)
        with open(path, "w", encoding="utf-8-sig", newline="") as f:
            f.write(shim.ONLINE_HEADER + "\r\nSomeMod.dll\t0\r\n")
        self.nitro(WITH_SWITCH)
        shim.nitro_online(self.az, "")
        self.assertEqual(shim.online_value(self.az, "SomeMod.dll"), "0")
        self.assertEqual(shim.online_value(self.az, shim.NITRO_DLL), "1")
        self.assertEqual(self.online_text().count("Load when playing online"), 1)

    def test_versions_come_from_the_mods_tab_list(self):
        self.nitro(WITH_SWITCH)
        with open(os.path.join(self.az, "fh2-mods", shim.UNDERGLOW_DLL), "wb") as f:
            f.write(b"MZ")
        with open(os.path.join(self.az, "fh2-mods", "platform-mods.txt"), "w", encoding="utf-8-sig") as f:
            f.write("# What the MODS tab remembers\nFnFNitro.dll\tNitroSystem\t2.0.0\tFast & Furious NOS\tqiangqiang101/NitroSystem\t1\n"
                    "Underglow.dll\tUnderglow\t1.1.0\tNeon\tqiangqiang101/FH2Underglow\t0\n")
        self.assertEqual(shim.nitro_installed(self.az), ("switch", "2.0.0"))
        self.assertEqual(shim.underglow_installed(self.az), (True, "1.1.0"))

    def ini(self):
        with open(os.path.join(self.az, "fh2-mods", "FnFNitro.ini"), encoding="utf-8", newline="") as f:
            return f.read()

    def put_ini(self, text):
        with open(os.path.join(self.az, "fh2-mods", "FnFNitro.ini"), "w", newline="") as f:
            f.write(text)

    def test_the_hud_theme_defaults_to_the_games_own_yellow_one(self):
        self.assertEqual(shim.nitro_hud_default(self.az), "")                       # no NitroSystem: nothing written
        self.nitro(WITH_SWITCH)
        self.assertTrue(shim.nitro_hud_default(self.az))
        self.assertEqual(self.ini(), "[Settings]\r\nHudTheme=Original\r\n")
        self.assertEqual(shim.nitro_hud_default(self.az), "")                       # once
        self.put_ini("[Settings]\r\nNitroEnabled=1\r\nHudTheme=FF\r\n")            # the player chose Fast & Furious
        self.assertEqual(shim.nitro_hud_default(self.az), "")
        self.assertIn("HudTheme=FF", self.ini())

    def test_the_hud_theme_goes_into_an_ini_that_has_none(self):
        self.nitro(WITH_SWITCH)
        self.put_ini("[Settings]\r\nNitroButton=LB\r\n\r\n[Upgrade]\r\nArea=1\r\n")
        shim.nitro_hud_default(self.az)
        self.assertEqual(self.ini(), "[Settings]\r\nHudTheme=Original\r\nNitroButton=LB\r\n\r\n[Upgrade]\r\nArea=1\r\n")

    def test_play_step_remembers_what_it_wrote(self):
        self.nitro(WITH_SWITCH)
        lc = core.Launcher(Settings(os.path.join(self.az, "settings.json")), load_identity=False)   # never the player's own settings file
        lc.settings["azimuth_dir"] = self.az
        self.assertTrue(lc.ensure_nitro_online())
        self.assertEqual(lc.settings["nitro_online_written"], "1")
        self.assertEqual(lc.ensure_nitro_online(), "")
        rows = {r[0]: r for r in lc.status_lines()}
        self.assertTrue(rows["NitroSystem"][3])


if __name__ == "__main__":
    unittest.main()
