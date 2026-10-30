"""Setup migration, package gates, safe upgrades and the wizard's branches."""
import hashlib
import os
from pathlib import Path
import stat
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from fh2mp_launcher import core, identity, setup, shim, upstream, updater
from fh2mp_launcher.settings import Settings
from fh2mp_launcher.version import __version__


def host(version):
    return b'MZ' + 'FileVersion\0'.encode('utf-16-le') + '\0'.encode('utf-16-le') + version.encode('utf-16-le') + b'\0\0'


class SetupFiles(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = mock.patch.dict(os.environ, FH2MP_LAUNCHER_HOME=str(self.root / 'config'))
        self.env.start()
        self.addCleanup(self.env.stop)
        builds = [{'version': '1.0.3', 'sha256': hashlib.sha256(host('1.0.3')).hexdigest()}]
        patch = mock.patch.object(setup, 'BUILTIN_AZIMUTH_BUILDS', builds)
        patch.start()
        self.addCleanup(patch.stop)
        helpers = {f'azimuth-tls-{n}.dll': hashlib.sha256(data).hexdigest()
                   for n, data in ((1, b'new one'), (2, b'new two'))}
        patch = mock.patch.object(setup, 'AZIMUTH_TLS_HELPERS', helpers)
        patch.start()
        self.addCleanup(patch.stop)
        self.folder = self.root / 'azimuth'
        self.archive = self.root / 'azimuth.zip'

    def zip(self, files=None):
        with zipfile.ZipFile(self.archive, 'w') as z:
            contents = {'data/bin/azimuth-tls-1.dll': b'new one', 'data/bin/azimuth-tls-2.dll': b'new two'}
            contents.update(files or {'azimuth.exe': host('1.0.3'), 'data/bin/runtime.dll': b'new',
                                     'data/azimuth.ini': b'defaults'})
            for n, value in contents.items():
                z.writestr(n, value)
        return str(self.archive)

    def test_upgrade_preserves_settings_saves_and_keeps_replaced_files(self):
        (self.folder / 'data/saves/user').mkdir(parents=True)
        (self.folder / 'azimuth.exe').write_bytes(host('1.0.1'))
        (self.folder / 'data/azimuth.ini').write_bytes(b'custom settings')
        save = self.folder / 'data/saves/user/BinarySaveState'
        save.write_bytes(b'progress')
        setup.install_azimuth(self.zip(), str(self.folder))
        self.assertEqual((self.folder / 'data/azimuth.ini').read_bytes(), b'custom settings')
        self.assertEqual(save.read_bytes(), b'progress')
        self.assertEqual((self.folder / 'azimuth.exe').read_bytes(), host('1.0.3'))
        backup = next(self.folder.glob('azimuth-before-setup-*'))
        self.assertEqual((backup / 'azimuth.exe').read_bytes(), host('1.0.1'))

    def test_rejected_archive_never_touches_an_existing_install(self):
        self.folder.mkdir()
        exe = self.folder / 'azimuth.exe'
        exe.write_bytes(host('1.0.1'))
        bad = [
            {'azimuth.exe': host('1.0.1')},
            {'azimuth.exe': host('1.0.3') + b'not the pinned build'},
            {'azimuth.exe': host('1.0.3'), '../escaped': b'x'},
            {'azimuth.exe': host('1.0.3'), 'C:/escaped': b'x'},
            {'azimuth.exe': host('1.0.3'), 'AZIMUTH.EXE': b'x'},
            {'azimuth.exe': host('1.0.3'), 'data/evil.': b'x'},
        ]
        for files in bad:
            with self.subTest(files=list(files)), self.assertRaises(ValueError):
                setup.install_azimuth(self.zip(files), str(self.folder))
            self.assertEqual(exe.read_bytes(), host('1.0.1'))
            self.assertEqual(list(self.folder.iterdir()), [exe])

    def test_incomplete_host_update_is_rejected_before_any_write(self):
        self.folder.mkdir()
        original = host('1.0.2')
        (self.folder / 'azimuth.exe').write_bytes(original)
        for helper in (None, b'wrong helper'):
            with self.subTest(helper=helper), zipfile.ZipFile(self.archive, 'w') as z:
                z.writestr('azimuth.exe', host('1.0.3'))
                if helper is not None:
                    z.writestr('data/bin/azimuth-tls-1.dll', helper)
            with self.assertRaisesRegex(ValueError, 'incomplete'):
                setup.install_azimuth(str(self.archive), str(self.folder))
            self.assertEqual((self.folder / 'azimuth.exe').read_bytes(), original)
            self.assertEqual(list(self.folder.iterdir()), [self.folder / 'azimuth.exe'])

    def test_symlink_archive_is_rejected(self):
        with zipfile.ZipFile(self.archive, 'w') as z:
            z.writestr('azimuth.exe', host('1.0.3'))
            info = zipfile.ZipInfo('data/link')
            info.external_attr = (stat.S_IFLNK | 0o777) << 16
            z.writestr(info, '/tmp/target')
        with self.assertRaises(ValueError):
            setup.install_azimuth(str(self.archive), str(self.folder))
        self.assertFalse(self.folder.exists())

    @unittest.skipIf(os.name == 'nt', 'requires POSIX symlinks')
    def test_existing_symlink_cannot_redirect_an_upgrade(self):
        self.folder.mkdir()
        (self.folder / 'azimuth.exe').write_bytes(host('1.0.1'))
        outside = self.root / 'outside'
        outside.mkdir()
        (self.folder / 'data').symlink_to(outside, target_is_directory=True)
        with self.assertRaises(ValueError):
            setup.install_azimuth(self.zip(), str(self.folder))
        self.assertEqual(list(outside.iterdir()), [])
        self.assertEqual((self.folder / 'azimuth.exe').read_bytes(), host('1.0.1'))

    def test_failed_write_rolls_back_executable(self):
        self.folder.mkdir()
        (self.folder / 'azimuth.exe').write_bytes(host('1.0.1'))
        original = setup.paths.atomic_write
        def fail(path, data, mode):
            if path.endswith('runtime.dll'):
                raise OSError('disk full')
            original(path, data, mode)
        with mock.patch.object(setup.paths, 'atomic_write', side_effect=fail), self.assertRaises(OSError):
            setup.install_azimuth(self.zip(), str(self.folder))
        self.assertEqual((self.folder / 'azimuth.exe').read_bytes(), host('1.0.1'))
        self.assertFalse((self.folder / 'data/bin/runtime.dll').exists())

    def test_only_the_supplied_host_is_accepted_and_server_cannot_add_builds(self):
        self.assertTrue(setup.version_ok('1.0.4'))
        for version in ['0.9.10', '1.0.1', '1.0.2', '1.0.3', '2.0.0', '']:
            self.assertFalse(setup.version_ok(version))
        lc = core.Launcher(Settings(str(self.root / 'settings.json')), load_identity=False)
        lc.server = {'supportedAzimuth': [{'version': '9.0.0', 'sha256': '0' * 64}]}
        self.assertEqual([b['version'] for b in lc.builds()], ['1.0.4'])

    def test_get_requires_current_signed_release_and_install_is_separate(self):
        lc = core.Launcher(Settings(str(self.root / 'settings.json')), load_identity=False)
        packages = setup.Packages(lc)
        data = f'MZ FH2MP-SHIM-VERSION:{__version__}:END'.encode()
        manifest = {'tag': f'v{__version__}', 'shim': {'version': __version__, 'dllSha256': hashlib.sha256(data).hexdigest()}}
        lc.settings['azimuth_dir'] = str(self.folder)
        with mock.patch.object(updater, 'Updater') as factory, mock.patch.object(core, 'read_shim_zip', return_value=data), mock.patch.object(shim, 'install') as install:
            up = factory.return_value
            up.fetch_manifest.return_value = {'tag': 'v1.1.0', 'shim': {'version': '1.1.0'}}
            with self.assertRaises(updater.UpdateError):
                packages.get_shim()
            up.download.assert_not_called()
            up.fetch_manifest.return_value = {'tag': 'v999.0.0', 'shim': {'version': '999.0.0'}}
            with self.assertRaises(updater.UpdateError):
                packages.get_shim()
            up.download.assert_not_called()
            up.fetch_manifest.return_value = manifest
            self.assertEqual(packages.get_shim(), __version__)
            up.fetch_manifest.assert_called_with(tag=f'v{__version__}')
            install.assert_not_called()
            packages.install_shim()
            install.assert_called_once_with(str(self.folder), data, manifest['shim']['dllSha256'])

    def test_same_version_shim_is_repaired_when_its_signed_hash_changes(self):
        lc = core.Launcher(Settings(str(self.root / 'settings.json')), load_identity=False)
        lc.settings['azimuth_dir'] = str(self.folder)
        target = self.folder / 'fh2-mods/FH2MP.dll'
        target.parent.mkdir(parents=True)
        dll = f'MZ FH2MP-SHIM-VERSION:{__version__}:END release candidate'.encode()
        target.write_bytes(dll)
        lc.manifest = {'tag': f'v{__version__}', 'shim': {'version': __version__, 'dllSha256': hashlib.sha256(dll).hexdigest()}}
        packages = setup.Packages(lc)
        self.assertTrue(packages.shim_current())
        target.write_bytes(dll + b'older candidate with the same marker')
        self.assertFalse(packages.shim_current())


class WizardFlow(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        from fh2mp_launcher.ui.wizard import Wizard
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.env = mock.patch.dict(os.environ, FH2MP_LAUNCHER_HOME=str(self.root / 'config'))
        self.env.start()
        self.addCleanup(self.env.stop)
        builds = [{'version': '1.0.3', 'sha256': hashlib.sha256(host('1.0.3')).hexdigest()}]
        patch = mock.patch.object(setup, 'BUILTIN_AZIMUTH_BUILDS', builds)
        patch.start()
        self.addCleanup(patch.stop)
        self.lc = core.Launcher(Settings(str(self.root / 'settings.json')), load_identity=False)
        self.w = Wizard(self.lc)
        self.addCleanup(self.w.deleteLater)

    def test_game_validation_and_old_install_routes_to_update_without_destination(self):
        self.w.game_edit.setText(str(self.root))
        self.w._game_next()
        self.assertEqual(self.w.stack.currentIndex(), self.w.WELCOME)
        (self.root / 'Mount').mkdir()
        (self.root / 'Mount/forza_durango_release_final.exe').write_bytes(b'MZ game')
        self.w._game_next()
        self.assertEqual(self.w.stack.currentIndex(), self.w.AZIMUTH)
        az = self.root / 'azimuth'
        az.mkdir()
        (az / 'azimuth.exe').write_bytes(host('1.0.1'))
        self.w.az_edit.setText(str(az))
        self.w._az_next()
        self.assertEqual(self.w.stack.currentIndex(), self.w.ACQUIRE)
        self.assertEqual(self.w.update_folder, str(az))
        self.w.archive = self.w._validated_archive = 'selected.zip'
        with mock.patch.object(self.w, '_install_azimuth') as install:
            self.w._archive_next()
            install.assert_called_once()
            self.assertEqual(install.call_args.args[0], str(az))

    def test_fresh_install_asks_destination_and_does_not_mark_complete_on_cancel(self):
        self.w.need_az.setChecked(True)
        self.w._az_next()
        self.w.archive = self.w._validated_archive = 'selected.zip'
        self.w._archive_next()
        self.assertEqual(self.w.stack.currentIndex(), self.w.DESTINATION)
        self.w.reject()
        self.assertEqual(self.lc.settings['setup_revision'], 0)

    def test_no_azimuth_hides_the_entire_folder_picker(self):
        self.w.stack.setCurrentIndex(self.w.AZIMUTH)
        self.w.show()
        self.w.need_az.setChecked(True)
        self.assertTrue(self.w.az_folder_box.isHidden())
        self.assertFalse(self.w.az_edit.isVisible())
        self.w.have_az.setChecked(True)
        self.assertTrue(self.w.az_edit.isVisible())
        self.assertLessEqual(self.w.height(), 400)

    def test_download_methods_show_only_the_selected_controls(self):
        self.w.stack.setCurrentIndex(self.w.ACQUIRE)
        self.w.show()
        self.assertTrue(self.w.auto_az.isChecked(), 'the download from the FH2MP server is the default: it needs no sign-in')
        self.assertTrue(self.w.manual_box.isHidden())
        self.assertTrue(self.w.auto_box.isVisible())
        self.assertTrue(self.w.itch_get.isVisible() and self.w.itch_get.isEnabled())
        self.assertFalse(self.w.itch_login.isVisible())
        self.w.manual_az.setChecked(True)
        self.assertTrue(self.w.auto_box.isHidden())
        self.assertTrue(self.w.manual_box.isVisible())

    def test_bad_zip_is_rejected_on_selection_before_destination_or_install(self):
        from PySide6.QtTest import QTest
        archive = self.root / 'old.zip'
        with zipfile.ZipFile(archive, 'w') as z:
            z.writestr('azimuth.exe', host('1.0.1'))
        self.w.stack.setCurrentIndex(self.w.ACQUIRE)
        self.w._select_archive(str(archive))
        for _ in range(200):
            if not self.w._busy_setup:
                break
            QTest.qWait(10)
        self.assertFalse(self.w._busy_setup)
        self.assertEqual(self.w.stack.currentIndex(), self.w.ACQUIRE)
        self.assertFalse(self.w.acquire_next.isEnabled())
        self.assertFalse(self.w.acquire_banner.isHidden())
        self.assertEqual(self.w.archive, '')
        self.w._archive_next()
        self.assertEqual(self.w.stack.currentIndex(), self.w.ACQUIRE)

    def test_install_button_uses_footer_and_completed_get_is_hidden(self):
        self.assertGreaterEqual(self.w.dep_next.nav_row.indexOf(self.w.dep_install), 0)
        self.assertGreaterEqual(self.w.mod_next.nav_row.indexOf(self.w.mod_install), 0)
        self.assertTrue(self.w.dep_next.isHidden())
        self.assertTrue(self.w.mod_next.isHidden())

    def test_requested_heading_equal_margins_and_no_small_caps_labels(self):
        from PySide6.QtWidgets import QLabel
        self.assertIn("Let's set up your launcher", [label.text() for label in self.w.findChildren(QLabel)])
        self.assertIsNone(self.w.findChild(QLabel, 'setup-step'))
        for index in range(self.w.stack.count()):
            margins = self.w.stack.widget(index).layout().contentsMargins()
            self.assertEqual((margins.left(), margins.top(), margins.right(), margins.bottom()), (24, 24, 24, 24))

    def test_successful_installs_advance_without_an_extra_next_click(self):
        def immediate(work, ok, banner, bar=None, failed=None):
            ok(work(mock.Mock(cancelled=False)))
        with mock.patch.object(self.w, '_operation', side_effect=immediate), mock.patch.object(self.w.packages, 'install_dependencies'), mock.patch.object(self.w, '_enter_multiplayer') as next_page:
            self.w._install_dependencies()
            next_page.assert_called_once()
        with mock.patch.object(self.w, '_operation', side_effect=immediate), mock.patch.object(self.w.packages, 'install_shim'), mock.patch.object(self.w, '_after_multiplayer') as next_page:
            self.w._install_multiplayer()
            next_page.assert_called_once()

    def test_form_grouping_and_content_sized_pages(self):
        from PySide6.QtWidgets import QLabel
        w = self.w
        w.show()
        self.app.processEvents()
        self.assertEqual(w.game_folder_label.text(), 'Game folder (the one containing Mount)')
        self.assertIs(w.game_folder_label.parentWidget(), w.game_edit.parentWidget())
        self.assertEqual(w.game_folder_label.parentWidget().layout().spacing(), 6)
        page = w.stack.currentWidget()
        heading = page.findChild(QLabel, 'setup-heading')
        self.assertEqual(heading.parentWidget().layout().spacing(), 10)
        self.assertLess(w.height(), 330)
        w.stack.setCurrentIndex(w.AZIMUTH)
        w.need_az.setChecked(True)
        for _ in range(3):
            self.app.processEvents()
        self.assertTrue(w.az_folder_box.isHidden())
        self.assertLess(w.height(), 260)
        before = w.height()
        w.az_banner.show_message('Select the folder containing azimuth.exe.', 'bad')
        for _ in range(4):
            self.app.processEvents()
        self.assertGreater(w.height(), before)
        w.close()

    def test_validated_automatic_download_has_no_stale_download_button(self):
        self.w.stack.setCurrentIndex(self.w.ACQUIRE)
        self.w.itch_profile = {'id': 7}
        self.w.auto_az.setChecked(True)
        def immediate(work, ok, banner, bar=None, failed=None):
            ok('1.0.3')
        with mock.patch.object(self.w, '_operation', side_effect=immediate), mock.patch.object(self.w, '_archive_next') as advance:
            self.w._select_archive('verified.zip')
            advance.assert_called_once()
        self.w._refresh_acquire_controls()
        self.assertTrue(self.w.itch_get.isHidden())
        self.assertTrue(self.w.acquire_next.isEnabled())

    def test_destination_accepts_an_existing_compatible_azimuth(self):
        folder = self.root / 'existing'
        folder.mkdir()
        (folder / 'azimuth.exe').write_bytes(host('1.0.3'))
        self.w.destination_edit.setText(str(folder))
        with mock.patch.object(self.lc, 'builds', return_value=setup.BUILTIN_AZIMUTH_BUILDS), mock.patch.object(self.w, '_use_azimuth') as use:
            self.w._fresh_install()
            use.assert_called_once_with(str(folder))

    @unittest.skipIf(os.name == 'nt', 'Linux setup')
    def test_linux_uses_a_default_prefix_without_exposing_a_required_picker(self):
        from fh2mp_launcher import launch, linux_support
        with mock.patch.object(launch, 'find_proton', return_value='/proton'), mock.patch.object(launch, 'resolve_proton', return_value='/proton'), mock.patch.object(launch, 'detect_prefix', return_value=''), mock.patch.object(linux_support, 'ready', return_value=False):
            self.w._after_multiplayer()
        self.assertTrue(self.w.prefix_edit.text())
        self.assertTrue(self.w.linux_options.isHidden())
        self.assertEqual(self.w.stack.currentIndex(), self.w.LINUX)

    def test_existing_identity_is_signed_in_without_registration(self):
        self.lc.identity = identity.Identity.generate()
        def immediate(work, ok, banner, bar=None, failed=None):
            ok(work(mock.Mock(cancelled=False)))
        with mock.patch.object(self.w, '_operation', side_effect=immediate), mock.patch.object(self.lc, 'sign_in', return_value={'xuid': 9}) as sign_in:
            self.w._account_or_done()
            sign_in.assert_called_once()
            self.assertEqual(self.w.stack.currentIndex(), self.w.DONE)
            self.assertEqual(self.lc.settings['setup_revision'], 0)
            self.w._finish_setup()
            self.assertEqual(Settings(self.lc.settings.path)['setup_revision'], setup.REVISION)

    def test_unknown_account_routes_to_registration_and_recovery_without_losing_key(self):
        from fh2mp_launcher.api import ApiError
        self.lc.identity = identity.Identity.generate()
        original = self.lc.identity
        def inline(work, ok, error, progress=None):
            try:
                result = work(mock.Mock(cancelled=False))
            except Exception as e:
                error(e)
            else:
                ok(result)
            return mock.Mock()
        self.w.stack.setCurrentIndex(self.w.LINUX)
        with mock.patch('fh2mp_launcher.ui.wizard.run_async', side_effect=inline), mock.patch.object(self.lc, 'sign_in', side_effect=ApiError(404, 'unknown_account')):
            self.w._account_or_done()
        self.assertEqual(self.w.stack.currentIndex(), self.w.ACCOUNT)
        self.assertTrue(self.w.existing_key)
        self.assertFalse(self.w.mode_restore.isHidden())
        self.assertIs(self.lc.identity, original)
        self.assertEqual(self.lc.settings['setup_revision'], 0)

    def test_account_connection_failure_is_visible_and_does_not_finish_setup(self):
        from fh2mp_launcher.api import ApiError
        self.lc.identity = identity.Identity.generate()
        def inline(work, ok, error, progress=None):
            error(ApiError(0, 'unreachable', 'Connection failed'))
            return mock.Mock()
        self.w.stack.setCurrentIndex(self.w.LINUX)
        with mock.patch('fh2mp_launcher.ui.wizard.run_async', side_effect=inline):
            self.w._account_or_done()
        self.assertEqual(self.w.stack.currentIndex(), self.w.LINUX)
        self.assertFalse(self.w.linux_banner.isHidden())
        self.assertFalse(self.w._busy_setup)
        self.assertEqual(self.lc.settings['setup_revision'], 0)

    def test_dependencies_and_multiplayer_cannot_advance_before_install(self):
        self.assertFalse(self.w.dep_next.isEnabled())
        self.assertTrue(self.w.dep_install.isHidden())
        self.assertFalse(self.w.mod_next.isEnabled())
        self.assertTrue(self.w.mod_install.isHidden())
        with self.assertRaises(ValueError):
            self.w.packages.install_dependencies()
        with self.assertRaises(ValueError):
            self.w.packages.install_shim()

    def test_existing_offline_install_still_gets_setup_migration(self):
        from fh2mp_launcher.ui.main_window import MainWindow
        self.lc.settings['offline_mode'] = True
        self.lc.settings['azimuth_dir'] = str(self.root)
        self.lc.identity_loaded = True
        win = MainWindow(self.lc)
        self.addCleanup(win.deleteLater)
        win.poll.stop()
        win.status_timer.stop()
        with mock.patch.object(win.community, 'refresh'), mock.patch('fh2mp_launcher.ui.main_window.QTimer.singleShot') as schedule:
            win.start()
            schedule.assert_called_once_with(0, win.run_wizard)

    def test_the_setup_wizard_never_opens_a_second_time_on_top_of_itself(self):
        # Windows, 2026-10-30: start() ran again while the first wizard was installing the add-ons; the second opened on the next page and the two
        # windows overlapped. It runs its own event loop, so the second run_wizard happens inside the first one's exec().
        from fh2mp_launcher.ui.main_window import MainWindow
        self.lc.settings['offline_mode'] = True
        self.lc.settings['azimuth_dir'] = str(self.root)
        self.lc.identity_loaded = True
        win = MainWindow(self.lc)
        self.addCleanup(win.deleteLater)
        win.poll.stop()
        win.status_timer.stop()
        built = []

        class FakeWizard:
            raised = 0

            def __init__(self, lc, parent=None, **kw):
                built.append(self)

            def exec(self):
                win.run_wizard()
                win._signup_existing()
                return 0

            def raise_(self):
                self.raised += 1

            def activateWindow(self):
                pass
        with mock.patch('fh2mp_launcher.ui.main_window.Wizard', FakeWizard), mock.patch('fh2mp_launcher.ui.main_window.QMessageBox'):
            win.run_wizard()
        self.assertEqual(len(built), 1)
        self.assertEqual(built[0].raised, 2)
        self.assertIsNone(win._wizard)


class UpgradeLeftovers(SetupFiles):
    def test_upgrade_retires_obsolete_tls_helper_into_backup(self):
        (self.folder / 'data/bin').mkdir(parents=True)
        (self.folder / 'azimuth.exe').write_bytes(host('1.0.1'))
        (self.folder / 'data/bin/azimuth-tls.dll').write_bytes(b'old helper')
        archive = self.zip({'azimuth.exe': host('1.0.3'), 'data/bin/azimuth-tls-1.dll': b'new one',
                            'data/bin/azimuth-tls-2.dll': b'new two'})
        setup.install_azimuth(archive, str(self.folder))
        self.assertFalse((self.folder / 'data/bin/azimuth-tls.dll').exists())
        backup = next(self.folder.glob('azimuth-before-setup-*'))
        self.assertEqual((backup / 'data/bin/azimuth-tls.dll').read_bytes(), b'old helper')

    def test_extra_options_upgrade_refreshes_plugin_but_preserves_preferences(self):
        from fh2mp_launcher.constants import UPSTREAM
        self.folder.mkdir()
        (self.folder / 'azimuth.exe').write_bytes(host('1.0.3'))
        plugin = 'mods/Extra Options - Garage Revving/plugins/EO_GarageEngineRev.dll'
        prefs = plugin[:-3] + 'ini'
        dll = b'MZ-new-extra'
        files = {'winmm.dll': dll, 'FH2_ExtraOptions.ini': b'defaults', plugin: b'new-plugin', prefs: b'defaults'}
        pin = dict(UPSTREAM['extraOptions'], dllSha256=hashlib.sha256(dll).hexdigest(),
                   files={n: n for n in files}, fileSha256={plugin: hashlib.sha256(b'new-plugin').hexdigest()})
        (self.folder / plugin).parent.mkdir(parents=True)
        (self.folder / plugin).write_bytes(b'old-plugin')
        (self.folder / prefs).write_bytes(b'custom plugin settings')
        (self.folder / 'FH2_ExtraOptions.ini').write_bytes(b'custom mod settings')
        (self.folder / 'winmm.dll').write_bytes(dll)
        with mock.patch.dict(UPSTREAM, extraOptions=pin):
            self.assertFalse(shim.extra_options_complete(str(self.folder)))
            shim.install_extra(str(self.folder), files)
            self.assertTrue(shim.extra_options_complete(str(self.folder)))
        self.assertEqual((self.folder / plugin).read_bytes(), b'new-plugin')
        self.assertEqual((self.folder / prefs).read_bytes(), b'custom plugin settings')
        self.assertEqual((self.folder / 'FH2_ExtraOptions.ini').read_bytes(), b'custom mod settings')
