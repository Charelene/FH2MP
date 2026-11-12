"""Play says when the drive with Azimuth or the game is nearly full: saves and caches failed with error 112 before a crash (#463, #464)."""
import collections
import os
import shutil
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from fh2mp_launcher import core  # noqa: E402

Usage = collections.namedtuple("Usage", "total used free")
GB = 1 << 30


class LowDisk(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.folder, True)

    def free(self, n):
        return mock.patch.object(core.shutil, "disk_usage", return_value=Usage(500 * GB, 500 * GB - n, n))

    def test_nearly_full_drive_is_named_once(self):
        with self.free(300 << 20):
            low = core.low_disk([self.folder, self.folder, os.path.join(self.folder, ".")])
        self.assertEqual(len(low), 1)
        self.assertEqual(low[0][1], 300 << 20)

    def test_enough_space_says_nothing(self):
        with self.free(40 * GB):
            self.assertEqual(core.low_disk([self.folder]), [])

    def test_missing_or_unmeasurable_folders_are_left_out(self):
        self.assertEqual(core.low_disk(["", None, os.path.join(self.folder, "nope"), "Z:\\home\\player\\Games\\FH2"]), [])
        with mock.patch.object(core.shutil, "disk_usage", side_effect=OSError("denied")):
            self.assertEqual(core.low_disk([self.folder]), [])

    def test_check_disk_warns_and_logs(self):
        lc = types.SimpleNamespace(settings={"azimuth_dir": self.folder})
        with self.free(512 << 20), mock.patch.object(core.logs, "warn") as warn:
            text = core.Launcher.check_disk(lc)
        self.assertIn("0.5 GB free", text)
        self.assertEqual(lc.disk_warning, text)
        self.assertEqual(warn.call_args[0][:2], ("play", "game.disk_low"))
        with self.free(40 * GB):
            self.assertEqual(core.Launcher.check_disk(lc), "")
        self.assertEqual(lc.disk_warning, "")


if __name__ == "__main__":
    unittest.main()
