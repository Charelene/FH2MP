"""Extra Options' aero unlock (UnlockAeroTuning) goes to 0 for online games and back to the player's value for offline play (shim.AERO_KEY)."""
import os
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from fh2mp_launcher import core, shim  # noqa: E402

INI = (b"[Experiments]\r\n"
       b"; Allows front and rear aero tuning without Forza race bodywork.\r\n"
       b"; UnlockAeroTuning=1 in this comment is left alone.\r\n"
       b"UnlockAeroTuning=1\r\n"
       b"\r\n"
       b"AllEngineSwaps=0\r\n")


class Settings(dict):
    saves = 0

    def save(self):
        self.saves += 1


def launcher(folder, running=False, remembered=""):
    return types.SimpleNamespace(settings=Settings(azimuth_dir=folder, eo_aero_restore=remembered), game_running=lambda: running)


class BytesTests(unittest.TestCase):
    def test_only_the_setting_line_changes(self):
        new, was = shim.aero_unlock_bytes(INI, b"0")
        self.assertEqual(was, b"1")
        self.assertEqual(new, INI.replace(b"UnlockAeroTuning=1\r\n", b"UnlockAeroTuning=0\r\n"))
        self.assertIn(b"; UnlockAeroTuning=1 in this comment", new)
        self.assertTrue(new.endswith(b"AllEngineSwaps=0\r\n"))

    def test_spacing_case_and_missing_key(self):
        new, was = shim.aero_unlock_bytes(b"unlockaerotuning = 1  \nX=1\n", b"0")
        self.assertEqual((new, was), (b"unlockaerotuning = 0  \nX=1\n", b"1"))
        self.assertEqual(shim.aero_unlock_bytes(b"X=1\n", b"0"), (b"X=1\n", None))


class FileTests(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.ini = os.path.join(self.d, shim.EXTRA_INI)
        with open(self.ini, "wb") as f:
            f.write(INI)

    def read(self):
        with open(self.ini, "rb") as f:
            return f.read()

    def test_off_then_restore(self):
        self.assertEqual(shim.aero_unlock_off(self.d), "1")
        self.assertIn(b"UnlockAeroTuning=0\r\n", self.read())
        self.assertTrue(os.path.exists(self.ini + ".before-fh2mp-aero"))
        self.assertEqual(shim.aero_unlock_off(self.d), "")                    # already off: nothing to remember
        self.assertTrue(shim.aero_unlock_restore(self.d, "1"))
        self.assertEqual(self.read(), INI)

    def test_a_value_the_player_set_since_is_kept(self):
        shim.aero_unlock_off(self.d)
        with open(self.ini, "wb") as f:
            f.write(INI.replace(b"UnlockAeroTuning=1", b"UnlockAeroTuning=2"))
        self.assertFalse(shim.aero_unlock_restore(self.d, "1"))
        self.assertIn(b"UnlockAeroTuning=2", self.read())

    def test_no_settings_file(self):
        empty = tempfile.mkdtemp()
        self.assertEqual(shim.aero_unlock_off(empty), "")
        self.assertFalse(shim.aero_unlock_restore(empty, "1"))


class LauncherTests(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        with open(os.path.join(self.d, shim.EXTRA_INI), "wb") as f:
            f.write(INI)

    def test_online_turns_it_off_and_remembers_the_value(self):
        lc = launcher(self.d)
        with mock.patch.object(core.logs, "info") as info:
            note = core.Launcher.aero_unlock_for_online(lc)
        self.assertIn("aero unlock is off for online play", note)
        self.assertEqual(lc.settings["eo_aero_restore"], "1")
        self.assertEqual(info.call_args.args[1], "eo.aero_unlock_off")
        self.assertEqual(core.Launcher.aero_unlock_for_online(lc), "")          # second online start: already off, value still remembered
        self.assertEqual(lc.settings["eo_aero_restore"], "1")

    def test_restore_after_the_game_and_only_without_a_running_game(self):
        lc = launcher(self.d)
        core.Launcher.aero_unlock_for_online(lc)
        running = launcher(self.d, running=True, remembered="1")
        self.assertFalse(core.Launcher.aero_unlock_restore(running))
        self.assertEqual(running.settings["eo_aero_restore"], "1")
        with mock.patch.object(core.logs, "info") as info:
            self.assertTrue(core.Launcher.aero_unlock_restore(lc))
        self.assertEqual(lc.settings["eo_aero_restore"], "")
        self.assertEqual(info.call_args.args[1], "eo.aero_unlock_restored")
        with open(os.path.join(self.d, shim.EXTRA_INI), "rb") as f:
            self.assertEqual(f.read(), INI)
        self.assertFalse(core.Launcher.aero_unlock_restore(lc))                 # nothing remembered: nothing to do

    def test_a_player_without_the_unlock_is_left_alone(self):
        with open(os.path.join(self.d, shim.EXTRA_INI), "wb") as f:
            f.write(INI.replace(b"UnlockAeroTuning=1", b"UnlockAeroTuning=0"))
        lc = launcher(self.d)
        self.assertEqual(core.Launcher.aero_unlock_for_online(lc), "")
        self.assertEqual((lc.settings["eo_aero_restore"], lc.settings.saves), ("", 0))


if __name__ == "__main__":
    unittest.main()
