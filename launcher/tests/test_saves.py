"""Save transfers must preserve recoverable copies even when disk operations fail."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from fh2mp_launcher import azimuth, core, saves
from fh2mp_launcher.settings import Settings


class SaveTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.profile(self.root / "User_9000000000000", b"old career")
        self.target = self.profile(self.root / "User_123", b"new career")

    def profile(self, path, data):
        p = path / "revisions" / "A" / "data"
        p.mkdir(parents=True)
        (p / "BinarySaveState").write_bytes(data.ljust(2048, b"\0"))
        (path / "pipeline-log.bin").write_bytes(data)
        return path

    def test_both_profiles_backed_up_and_can_be_restored(self):
        old_source, old_target = saves._manifest(self.source), saves._manifest(self.target)
        backup = Path(saves.copy_profile(self.source, self.target))
        self.assertEqual(saves._manifest(backup / "source"), old_source)
        self.assertEqual(saves._manifest(backup / "destination"), old_target)
        self.assertEqual(saves._manifest(self.source), old_source)
        self.assertEqual(saves._manifest(self.target), old_source)
        saves.copy_profile(backup / "destination", self.target)
        self.assertEqual(saves._manifest(self.target), old_target)

    def test_backup_failure_does_not_touch_destination(self):
        original = saves._manifest(self.target)
        real = saves._verified_copy
        def copy(source, target):
            if target.name == "destination":
                raise PermissionError("denied")
            real(source, target)
        with mock.patch.object(saves, "_verified_copy", side_effect=copy):
            with self.assertRaises(saves.SaveCopyError) as err:
                saves.copy_profile(self.source, self.target)
        self.assertTrue(err.exception.permission)
        self.assertEqual(saves._manifest(self.target), original)

    def test_install_failure_rolls_back_destination(self):
        original = saves._manifest(self.target)
        rename = Path.rename
        def fail_stage(path, target):
            if path.name == "profile":
                raise OSError("disk error")
            return rename(path, target)
        with mock.patch.object(Path, "rename", fail_stage):
            with self.assertRaises(saves.SaveCopyError):
                saves.copy_profile(self.source, self.target)
        self.assertEqual(saves._manifest(self.target), original)
        self.assertFalse(list(self.root.glob("*.adopting-*")))

    def test_corrupt_copy_is_detected_before_replacement(self):
        original = saves._manifest(self.target)
        copytree = saves.shutil.copytree
        def corrupt(source, target, *args, **kwargs):
            result = copytree(source, target, *args, **kwargs)
            if Path(target).name == "source":
                (Path(target) / "pipeline-log.bin").write_bytes(b"corrupt")
            return result
        with mock.patch.object(saves.shutil, "copytree", side_effect=corrupt):
            with self.assertRaises(saves.SaveCopyError):
                saves.copy_profile(self.source, self.target)
        self.assertEqual(saves._manifest(self.target), original)

    def test_same_profile_nested_and_linked_paths_rejected(self):
        for target in (self.source, self.source / "User_123"):
            with self.assertRaises(saves.SaveCopyError):
                saves.copy_profile(self.source, target)
        link = self.source / "link"
        link.symlink_to(self.target, target_is_directory=True)
        with self.assertRaises(saves.SaveCopyError):
            saves.copy_profile(self.source, self.target)

    def test_backup_root_link_rejected(self):
        (self.root / "fh2mp-save-backups").symlink_to(self.target, target_is_directory=True)
        with self.assertRaises(saves.SaveCopyError):
            saves.copy_profile(self.source, self.target)

    def test_repeated_transfers_keep_distinct_backups(self):
        first = saves.copy_profile(self.source, self.target)
        second = saves.copy_profile(self.source, self.target)
        self.assertNotEqual(first, second)
        self.assertTrue(Path(first).is_dir())

    def test_discovery_does_not_hide_permission_errors(self):
        with mock.patch.object(azimuth.os, "listdir", side_effect=PermissionError("denied")):
            with self.assertRaises(PermissionError):
                azimuth.save_profiles(self.root)
            with self.assertRaises(PermissionError):
                azimuth.has_progress(self.source)

    def test_other_games_are_not_adopted(self):
        self.profile(self.root / "data/titles/OTHER/saves/player-1/User_123", b"other game")
        self.assertEqual(azimuth.save_profiles(self.root), [])

    def test_unknown_destination_layout_is_preserved(self):
        target = Path(azimuth.account_save_path(self.root, 123))
        target.mkdir(parents=True)
        (target / "unknown-save").write_bytes(b"important")
        self.assertFalse(azimuth._needs_adoption(target))

    def test_elevation_is_scoped_to_helper_and_reports_cancellation(self):
        with mock.patch.object(saves.sys, "platform", "linux"), \
                mock.patch.object(saves.shutil, "which", return_value="/usr/bin/pkexec"), \
                mock.patch.object(saves.subprocess, "run", return_value=mock.Mock(returncode=126)) as run:
            with self.assertRaisesRegex(OSError, "cancelled or failed"):
                saves.elevated_copy(self.source, self.target)
        command = run.call_args.args[0]
        self.assertEqual(command[0], "/usr/bin/pkexec")
        self.assertEqual(command[-3:], ["--save-copy", str(self.source), str(self.target)])
        self.assertIn(str(Path(saves.__file__).resolve()), command)

    def test_source_helper_runs_outside_package_working_directory(self):
        target = self.root / "User_456"
        result = subprocess.run([sys.executable, saves.__file__, "--save-copy", str(self.source), str(target)],
                                cwd=self.tmp.name, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(saves._manifest(target), saves._manifest(self.source))

    def test_scan_retry_runs_the_same_adoption_with_explicit_folders(self):
        error = saves.SaveScanError(str(self.root), 123, ["another install"], PermissionError("denied"))
        with mock.patch.object(saves, "_elevate") as elevate:
            saves.retry_elevated(error)
        elevate.assert_called_once_with(["--save-adopt", str(self.root), "123", "another install"])

    def test_appimage_elevation_avoids_the_user_fuse_mount(self):
        with mock.patch.object(saves.sys, "platform", "linux"), mock.patch.object(saves.sys, "frozen", True, create=True), \
                mock.patch.dict(os.environ, {"APPIMAGE": "/tmp/FH2MP Launcher.AppImage"}), \
                mock.patch.object(saves.shutil, "which", return_value="/usr/bin/pkexec"), \
                mock.patch.object(saves.subprocess, "run", return_value=mock.Mock(returncode=0)) as run:
            saves.elevated_copy(self.source, self.target)
        self.assertEqual(run.call_args.args[0][:3], ["/usr/bin/pkexec", "/tmp/FH2MP Launcher.AppImage", "--appimage-extract-and-run"])
        self.assertEqual(run.call_args.kwargs["env"]["PYINSTALLER_RESET_ENVIRONMENT"], "1")

    def test_windows_elevation_uses_runas_and_waits_for_helper(self):
        import base64
        with mock.patch.object(saves.sys, "platform", "win32"), \
                mock.patch.object(saves.subprocess, "run", return_value=mock.Mock(returncode=0)) as run:
            saves.elevated_copy(self.source, self.target)
        command = run.call_args.args[0]
        self.assertEqual(command[0], "powershell.exe")
        script = base64.b64decode(command[-1]).decode("utf-16-le")
        self.assertIn("-Verb RunAs -Wait -PassThru", script)
        self.assertIn("--save-copy", script)

    def test_source_changing_after_backup_aborts_before_replacement(self):
        original = saves._manifest(self.target)
        real = saves._verified_copy
        def copy(source, target):
            real(source, target)
            if target.name == "source":
                (self.source / "pipeline-log.bin").write_bytes(b"changed during copy")
        with mock.patch.object(saves, "_verified_copy", side_effect=copy):
            with self.assertRaisesRegex(saves.SaveCopyError, "source changed after backup"):
                saves.copy_profile(self.source, self.target)
        self.assertEqual(saves._manifest(self.target), original)

    def test_offline_migration_failure_prevents_launch(self):
        settings = Settings(str(self.root / "settings.json"))
        settings["azimuth_dir"], settings["xuid"] = str(self.root), 123
        lc = core.Launcher(settings, load_identity=False)
        lc.azimuth_info = mock.Mock(return_value=mock.Mock(found=True, supported=True))
        lc.repair_tls_helper = mock.Mock()
        lc.ensure_game = mock.Mock()
        lc.shim_state = mock.Mock(return_value=("ours", "1.0.0"))
        with mock.patch.object(core.shim, "supports_offline", return_value=True), \
                mock.patch.object(core.azimuth, "has_game_configured", return_value=True), \
                mock.patch.object(azimuth, "find_candidates", return_value=[]), \
                mock.patch.object(azimuth, "adopt_saves", side_effect=PermissionError("scan denied")), \
                mock.patch.object(core.launch, "start") as start:
            with self.assertRaises(saves.SaveScanError):
                lc.play_offline()
            start.assert_not_called()

    def test_online_migration_failure_prevents_session_and_launch(self):
        settings = Settings(str(self.root / "settings.json"))
        settings["azimuth_dir"] = str(self.root)
        lc = core.Launcher(settings, load_identity=False)
        lc.refresh_server = mock.Mock()
        lc.launcher_outdated = mock.Mock(return_value=False)
        lc.azimuth_info = mock.Mock(return_value=mock.Mock(found=True, supported=True))
        lc.shim_state = mock.Mock(return_value=("ours", "1.0.0"))
        lc.shim_outdated = mock.Mock(return_value=False)
        lc.repair_tls_helper = mock.Mock()
        lc.ensure_extra_options = mock.Mock()
        lc.extra_state = mock.Mock(return_value=(True, "", ""))
        lc.ensure_game = mock.Mock()
        lc.sign_in = mock.Mock(return_value={"xuid": 123})
        with mock.patch.object(core.shim, "enable"), mock.patch.object(core.azimuth, "has_game_configured", return_value=True), \
                mock.patch.object(azimuth, "find_candidates", return_value=[]), \
                mock.patch.object(azimuth, "adopt_saves", side_effect=PermissionError("scan denied")), \
                mock.patch.object(core.shim, "write_session") as session, mock.patch.object(core.launch, "start") as start:
            with self.assertRaises(saves.SaveScanError) as err:
                lc.play()
            self.assertTrue(err.exception.permission)
            session.assert_not_called()
            start.assert_not_called()


class SaveManagerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def test_profiles_and_both_destinations_shown_without_writing(self):
        from fh2mp_launcher.ui.dialogs import SaveManagerDialog
        with tempfile.TemporaryDirectory() as d, mock.patch.object(azimuth, "find_candidates", return_value=[]):
            settings = Settings(str(Path(d) / "settings.json"))
            settings["xuid"] = 123
            lc = core.Launcher(settings, load_identity=False)
            p = Path(azimuth.account_save_path(d, 0x9000000000000))
            p.mkdir(parents=True)
            dlg = SaveManagerDialog(lc, d)
            self.assertEqual(dlg.source.currentData(), str(p))
            self.assertEqual(dlg.target.count(), 2)
            self.assertIn("Created:", dlg.info.text())
            self.assertTrue(dlg.options.isHidden())
            self.assertFalse(dlg.import_save.isEnabled())
            online = Path(azimuth.account_save_path(d, 123))
            for profile in (p, online):
                data = profile / "revisions/A/data"
                data.mkdir(parents=True)
                (data / "BinarySaveState").write_bytes(b"save")
            dlg._details()
            with mock.patch.object(dlg, "_copy") as copy:
                dlg.import_save.click()
                copy.assert_called_once_with(str(p), str(online))
                copy.reset_mock()
                dlg.export_save.click()
                copy.assert_called_once_with(str(online), str(p))
            from PySide6.QtWidgets import QLabel, QMessageBox
            labels = [label.text() for label in dlg.findChildren(QLabel)]
            self.assertIn("Azimuth", labels)
            self.assertIn("Online + offline", labels)
            self.assertIn("Import save", labels)
            self.assertIn("Export save", labels)
            self.assertEqual(dlg.account_path, str(online))
            self.assertEqual(dlg.default_path, str(p))
            self.assertEqual(dlg.account_status.text(), dlg._save_status(str(online)))
            with mock.patch.object(lc, "game_running", return_value=False), \
                    mock.patch.object(QMessageBox, "question", return_value=QMessageBox.StandardButton.No) as question, \
                    mock.patch.object(dlg, "_transfer") as transfer:
                dlg.import_save.click()
                self.assertIn("both online and offline FH2MP play", question.call_args.args[2])
                transfer.assert_not_called()
                question.return_value = QMessageBox.StandardButton.Yes
                dlg.import_save.click()
                transfer.assert_called_once_with(str(p), str(online))
                transfer.reset_mock()
                dlg.export_save.click()
                self.assertIn("Azimuth default progress", question.call_args.args[2])
                transfer.assert_called_once_with(str(online), str(p))
            self.assertFalse((p.parent / "fh2mp-save-backups").exists())
            dlg.close()


if __name__ == "__main__":
    unittest.main()
