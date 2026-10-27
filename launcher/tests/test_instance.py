"""One launcher owns a profile; exiting or crashing releases that ownership."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from fh2mp_launcher.app import lock_profile


class ProfileInstance(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from PySide6.QtWidgets import QApplication
        cls.app = QApplication.instance() or QApplication([])

    def test_same_profile_is_exclusive_and_unlock_allows_reopening(self):
        with tempfile.TemporaryDirectory() as folder:
            first = lock_profile(folder)
            self.assertIsNotNone(first)
            try:
                self.assertIsNone(lock_profile(folder))
                other = Path(folder) / 'other-profile'
                other.mkdir()
                independent = lock_profile(str(other))
                self.assertIsNotNone(independent)
                independent.unlock()
            finally:
                first.unlock()
            replacement = lock_profile(folder)
            self.assertIsNotNone(replacement)
            replacement.unlock()

    def test_crashed_owner_does_not_block_next_start(self):
        with tempfile.TemporaryDirectory() as folder:
            script = ('import os,sys; sys.path.insert(0,sys.argv[1]); '
                      'from fh2mp_launcher.app import lock_profile; '
                      'lock=lock_profile(sys.argv[2]); os._exit(0 if lock else 1)')
            subprocess.run([sys.executable, '-I', '-c', script,
                            str(Path(__file__).resolve().parents[1]), folder], check=True)
            replacement = lock_profile(folder)
            self.assertIsNotNone(replacement)
            replacement.unlock()

    def test_second_process_activates_owner_without_taking_its_lock(self):
        from fh2mp_launcher.instance import InstanceServer
        with tempfile.TemporaryDirectory() as folder:
            owner = lock_profile(folder)
            self.assertIsNotNone(owner)
            server = InstanceServer(folder)
            activated = []
            server.activated.connect(lambda: activated.append(True))
            try:
                script = ('import sys;sys.path.insert(0,sys.argv[1]);'
                          'from PySide6.QtCore import QCoreApplication;app=QCoreApplication([]);'
                          'from fh2mp_launcher.app import lock_profile;'
                          'from fh2mp_launcher.instance import request_activation;'
                          'assert lock_profile(sys.argv[2]) is None;'
                          'sys.exit(0 if request_activation(sys.argv[2]) else 1)')
                subprocess.run([sys.executable, '-I', '-c', script,
                                str(Path(__file__).resolve().parents[1]), folder], check=True, timeout=5)
                deadline = time.monotonic() + 2
                while not activated and time.monotonic() < deadline:
                    self.app.processEvents()
                self.assertEqual(activated, [True])
                self.assertIsNone(lock_profile(folder))
            finally:
                server.close()
                owner.unlock()

    def test_profile_access_failure_is_not_reported_as_another_instance(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaisesRegex(OSError, 'profile folder'):
                lock_profile(str(Path(folder) / 'missing'))
