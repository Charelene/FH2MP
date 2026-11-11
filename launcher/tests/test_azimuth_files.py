"""Azimuth's own FFmpeg DLLs must be the supported build's: a 1.0.4 azimuth.exe over an older install stopped at start (issue #450)."""
import hashlib
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from fh2mp_launcher import azimuth, core  # noqa: E402
from fh2mp_launcher.constants import AZIMUTH_VERSION  # noqa: E402

EXE = b"azimuth exe stand-in"
GOOD = {"data/bin/avcodec-62.dll": b"avcodec 1.0.4", "data/bin/avutil-60.dll": b"avutil 1.0.4"}


def sha(b):
    return hashlib.sha256(b).hexdigest()


class AzimuthFiles(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.folder = self.tmp.name
        with open(os.path.join(self.folder, "azimuth.exe"), "wb") as f:
            f.write(EXE)
        self.builds = [{"version": AZIMUTH_VERSION, "sha256": sha(EXE)}]
        patcher = mock.patch.object(azimuth, "AZIMUTH_RUNTIME_FILES", {k: sha(v) for k, v in GOOD.items()})
        patcher.start()
        self.addCleanup(patcher.stop)

    def write(self, files):
        for rel, data in files.items():
            path = os.path.join(self.folder, *rel.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)

    def test_matching_files_are_supported(self):
        self.write(GOOD)
        info = azimuth.inspect(self.folder, self.builds)
        self.assertTrue(info.supported)
        self.assertEqual(info.runtime_bad, [])

    def test_an_older_avcodec_makes_the_folder_unsupported(self):
        self.write({**GOOD, "data/bin/avcodec-62.dll": b"avcodec from 0.9.10"})
        info = azimuth.inspect(self.folder, self.builds)
        self.assertFalse(info.supported)
        self.assertEqual(info.runtime_bad, ["data/bin/avcodec-62.dll"])
        text = core.azimuth_files_text(info)
        self.assertIn("avcodec-62.dll", text)
        self.assertIn(f"install Azimuth {AZIMUTH_VERSION} into this folder", text)

    def test_a_missing_dll_counts(self):
        self.write({"data/bin/avcodec-62.dll": GOOD["data/bin/avcodec-62.dll"]})
        self.assertEqual(azimuth.inspect(self.folder, self.builds).runtime_bad, ["data/bin/avutil-60.dll"])

    def test_only_the_pinned_version_is_checked(self):
        info = azimuth.inspect(self.folder, [{"version": "9.9.9", "sha256": sha(EXE)}])
        self.assertTrue(info.supported)
        self.assertEqual(info.runtime_bad, [])


if __name__ == "__main__":
    unittest.main()
