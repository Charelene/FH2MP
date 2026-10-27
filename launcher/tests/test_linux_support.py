"""Automatic Proton selection and native compiler setup, without game execution."""
import hashlib
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from fh2mp_launcher import azimuth, launch, linux_support


class LinuxSupport(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.prefix = self.root / 'prefix'
        self.data = b'MZ-native-compiler-fixture'
        patch = mock.patch.object(linux_support, 'NATIVE_HASHES', {hashlib.sha256(self.data).hexdigest()})
        patch.start()
        self.addCleanup(patch.stop)

    def registry(self):
        (self.prefix / 'pfx').mkdir(parents=True, exist_ok=True)
        (self.prefix / 'pfx/system.reg').write_text('REGEDIT4')

    @unittest.skipIf(os.name == 'nt', 'Linux host path mapping')
    def test_prefix_is_detected_from_linux_selected_game_folder(self):
        prefix = self.root / 'Steam/steamapps/compatdata/123'
        game = prefix / 'pfx/drive_c/Games/FH2'
        game.mkdir(parents=True)
        folder = self.root / 'azimuth'
        folder.mkdir()
        azimuth.configure_game(str(folder), 'Z:' + str(game).replace('/', '\\'))
        self.assertEqual(launch.detect_prefix(str(folder)), str(prefix))

    def test_working_native_compiler_needs_no_download_or_initialization(self):
        self.registry()
        target = Path(linux_support.compiler_path(str(self.prefix)))
        target.parent.mkdir(parents=True)
        target.write_bytes(self.data)
        with mock.patch.object(linux_support, 'compiler_bytes') as download, mock.patch.object(linux_support, '_run') as run:
            linux_support.prepare({}, '/proton', str(self.prefix))
            download.assert_not_called()
            run.assert_not_called()
        self.assertTrue(linux_support.ready(str(self.prefix)))

    def test_existing_builtin_is_backed_up_and_replaced(self):
        self.registry()
        target = Path(linux_support.compiler_path(str(self.prefix)))
        target.parent.mkdir(parents=True)
        target.write_bytes(b'MZ-wine-builtin')
        with mock.patch.object(linux_support, 'compiler_bytes', return_value=self.data), mock.patch.object(linux_support, '_run') as run:
            linux_support.prepare({}, '/proton', str(self.prefix))
            run.assert_not_called()
        self.assertEqual(target.read_bytes(), self.data)
        self.assertEqual(Path(str(target) + '.before-fh2mp').read_bytes(), b'MZ-wine-builtin')

    def test_fresh_prefix_is_initialized_without_starting_azimuth(self):
        def initialize(argv, env, cancelled):
            self.assertEqual(argv, ['/chosen/proton', 'run', 'wineboot.exe', '-u'])
            self.assertEqual(env['STEAM_COMPAT_DATA_PATH'], str(self.prefix))
            self.registry()
        with mock.patch.object(linux_support, 'compiler_bytes', return_value=self.data), mock.patch.object(linux_support, '_run', side_effect=initialize):
            linux_support.prepare({}, '/chosen/proton', str(self.prefix))
        self.assertTrue(linux_support.ready(str(self.prefix)))

    def test_cancelled_setup_does_not_write_prefix(self):
        with mock.patch.object(linux_support, 'compiler_bytes', return_value=self.data):
            with self.assertRaisesRegex(Exception, 'cancelled'):
                linux_support.prepare({}, '/proton', str(self.prefix), cancelled=lambda: True)
        self.assertFalse(self.prefix.exists())

    def test_unrelated_nonempty_folder_is_never_used_as_a_new_prefix(self):
        self.prefix.mkdir()
        saved = self.prefix / 'personal-file'
        saved.write_text('keep')
        with mock.patch.object(linux_support, 'compiler_bytes', return_value=self.data), mock.patch.object(linux_support, '_run') as run:
            with self.assertRaisesRegex(Exception, 'dedicated'):
                linux_support.prepare({}, '/proton', str(self.prefix))
            run.assert_not_called()
        self.assertEqual(saved.read_text(), 'keep')
