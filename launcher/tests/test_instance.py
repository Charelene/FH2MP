"""One launcher owns a profile; exiting or crashing releases that ownership."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from fh2mp_launcher.app import lock_profile


class ProfileInstance(unittest.TestCase):
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
