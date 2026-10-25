"""Offline solo play must work without an account, shim or network connection."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from fh2mp_launcher import azimuth, core, launch, shim, updater
from fh2mp_launcher.settings import Settings


class OfflineFixture:
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.folder = Path(self.tmp.name)
        (self.folder / "azimuth.exe").write_bytes(b"test azimuth")
        azimuth.configure_game(str(self.folder), r"C:\Games\FH2")
        self.settings = Settings(str(self.folder / "settings.json"))
        self.settings["azimuth_dir"] = str(self.folder)
        self.settings["offline_mode"] = True
        self.lc = core.Launcher(self.settings, load_identity=False)
        self.lc.server = {"supportedAzimuth": [{"version": "0.9.10", "sha256": hashlib.sha256(b"test azimuth").hexdigest()}],
                          "maintenance": "Closed", "minLauncher": "999.0.0", "minShim": "999.0.0"}
        self.api = mock.patch.object(core.Launcher, "api", side_effect=AssertionError("offline network access")).start()
        self.updates = mock.patch.object(updater.Updater, "fetch_manifest", side_effect=AssertionError("offline update access")).start()
        self.spawn = mock.patch.object(launch, "start", return_value=mock.Mock(poll=mock.Mock(return_value=None))).start()
        self.addCleanup(mock.patch.stopall)

    def disabled_mods(self):
        path = self.folder / "fh2-mods" / shim.DISABLED_LIST
        return path.read_text().split() if path.exists() else []


class OfflineModeTests(OfflineFixture, unittest.TestCase):
    def test_setting_survives_restart(self):
        self.assertFalse(Settings(str(self.folder / "new.json"))["offline_mode"])
        self.settings.save()
        self.assertTrue(Settings(self.settings.path)["offline_mode"])

    def test_solo_play_without_identity_or_shim_during_maintenance(self):
        self.lc.token = "old-online-token"
        self.lc.game_env = {"FH2MP_TUNNEL": "1", "FH2MP_SERVER_PORT": "1234"}
        self.lc.gamedb_synced = True
        proxy = mock.Mock()
        self.lc.tunnel = proxy
        shim.write_session(str(self.folder), 123, "old-online-token", "https://example.com")
        session = Path(shim.bin_dir(str(self.folder))) / shim.SESSION_FILE
        self.assertTrue(session.exists())
        self.lc.play()
        self.assertTrue(self.lc.game_running())
        self.assertFalse(session.exists())
        self.assertEqual(self.lc.token, "")
        self.assertEqual(self.lc.game_env, {"FH2MP_OFFLINE": "1"})
        self.assertFalse(self.lc.gamedb_synced)
        proxy.stop.assert_called_once()
        self.assertIn(shim.SHIM_DLL, self.disabled_mods())
        self.spawn.assert_called_once_with(self.settings, str(self.folder), skip_launcher=True, extra_env={"FH2MP_OFFLINE": "1"})
        self.lc.after_game()
        self.assertNotIn(shim.SHIM_DLL, self.disabled_mods())
        self.api.assert_not_called()
        self.updates.assert_not_called()

    def test_keeps_previously_disabled_mods_after_game_or_launch_failure(self):
        for failure in (None, launch.LaunchError("bad command"), ValueError("bad arguments")):
            with self.subTest(failure=failure):
                shim.disable(str(self.folder))
                path = self.folder / "fh2-mods" / shim.DISABLED_LIST
                path.write_text("other.dll\n" + shim.SHIM_DLL + "\n")
                self.spawn.side_effect = failure
                if failure:
                    with self.assertRaises(type(failure)):
                        self.lc.play()
                else:
                    self.lc.play()
                    self.lc.after_game()
                self.assertEqual(self.disabled_mods(), ["other.dll", shim.SHIM_DLL])
                self.assertFalse(self.lc.offline_run)

    def test_failed_launch_restores_mod_enabled_state(self):
        self.spawn.side_effect = launch.LaunchError("Proton missing")
        with self.assertRaises(launch.LaunchError):
            self.lc.play()
        self.assertNotIn(shim.SHIM_DLL, self.disabled_mods())
        self.assertFalse(self.lc.offline_run)

    def test_account_save_id_is_kept_without_online_session(self):
        xuid = 0x0009000012345678
        self.settings["xuid"] = xuid
        profile = self.folder / "data" / "titles" / azimuth.TITLE_DIR / "saves" / "player-1" / f"User_{xuid:X}"
        profile.mkdir(parents=True)
        save = profile / "progress.bin"
        save.write_bytes(b"existing progress")
        dll = self.folder / "fh2-mods" / shim.SHIM_DLL
        dll.parent.mkdir(exist_ok=True)
        dll.write_bytes(b"FH2MP-SHIM-VERSION:1.0.0:END" + "FH2MP_OFFLINE".encode("utf-16-le"))
        (self.folder / shim.PLATFORM_DLL).write_bytes(("FH2 Mod Platform\0ProductVersion\0" + shim.PLATFORM_VERSION).encode("utf-16-le"))
        shim.disable(str(self.folder))
        self.lc.play()
        self.assertEqual(self.lc.game_env, {"FH2MP_OFFLINE": "1", "FH2MP_XUID": f"{xuid:x}"})
        self.assertNotIn(shim.SHIM_DLL, self.disabled_mods())
        self.assertEqual(save.read_bytes(), b"existing progress")
        self.assertEqual(list(profile.parent.iterdir()), [profile])
        self.assertFalse((Path(shim.bin_dir(str(self.folder))) / shim.SESSION_FILE).exists())
        self.lc.after_game()
        self.assertIn(shim.SHIM_DLL, self.disabled_mods())
        self.api.assert_not_called()

    def test_old_shim_cannot_silently_open_another_profile(self):
        self.settings["xuid"] = 0x0009000012345678
        with self.assertRaises(core.PlayBlocked) as err:
            self.lc.play()
        self.assertEqual(err.exception.code, "offline_shim_outdated")
        self.spawn.assert_not_called()

    def test_missing_game_still_allows_azimuth_setup(self):
        (self.folder / "data" / "azimuth.ini").unlink()
        with mock.patch.object(azimuth, "find_game_installs", return_value=[]):
            with self.assertRaises(core.PlayBlocked) as err:
                self.lc.play()
            self.assertEqual(err.exception.code, "no_game")
            self.spawn.assert_not_called()
            self.lc.play(azimuth_window=True)
        self.spawn.assert_called_once_with(self.settings, str(self.folder), skip_launcher=False, extra_env={"FH2MP_OFFLINE": "1"})

    def test_online_setting_restores_normal_sign_in_flow(self):
        self.settings["offline_mode"] = False
        with self.assertRaisesRegex(AssertionError, "offline network access"):
            self.lc.play()
        self.spawn.assert_not_called()

    def test_missing_azimuth_still_blocks_launch(self):
        (self.folder / "azimuth.exe").unlink()
        with self.assertRaises(core.PlayBlocked) as err:
            self.lc.play()
        self.assertEqual(err.exception.code, "no_azimuth")
        self.spawn.assert_not_called()


class OfflineWindowTests(OfflineFixture, unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def test_saved_offline_start_skips_keyring_signup_and_background_work(self):
        from fh2mp_launcher.ui.main_window import MainWindow
        with mock.patch("fh2mp_launcher.ui.main_window.run_async", side_effect=AssertionError("background work in offline mode")):
            win = MainWindow(self.lc)
            self.addCleanup(win.deleteLater)
            win.poll.stop()
            win.status_timer.stop()
            win.start()
            win.refresh_server()
            win.refresh_all()
            win._load_avatar()
            win._tap()
            self.assertTrue(win.play.isEnabled())
            self.assertEqual(win.play.text(), "PLAY")
            self.assertFalse(win.offline_icon.isHidden())
            self.assertIsNone(win._problem())
            self.assertFalse(self.lc.identity_loaded)
            with mock.patch.object(win, "on_play_offline") as solo:
                win.on_play(azimuth_window=True)
                solo.assert_called_once_with(False, azimuth_window=True)

    def test_settings_toggle_is_saved(self):
        from fh2mp_launcher.ui.dialogs import SettingsDialog
        with mock.patch.object(launch, "find_proton", return_value=""), mock.patch.object(launch, "detect_prefix", return_value=""):
            dialog = SettingsDialog(self.lc)
            self.addCleanup(dialog.deleteLater)
            dialog.offline.setChecked(False)
            dialog._save()
            self.assertFalse(Settings(self.settings.path)["offline_mode"])


if __name__ == "__main__":
    unittest.main()
