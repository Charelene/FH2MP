"""The car database sync (gamedb.py): the server's copy lands in fh2mp\\ of the Azimuth folder (the shim puts it in mediaoverride), is
checked by hash, and a bad download never replaces it."""
import hashlib
import lzma
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from fh2mp_launcher import gamedb  # noqa: E402

DB = b"SQLite format 3\x00" + b"cars" * 500


class FakeApi:
    def __init__(self, data=DB, blob=None):
        self.data, self.blob, self.downloads = data, blob, 0

    def manifest(self):
        return {"name": "gamedbRC.slt", "sha256": hashlib.sha256(self.data).hexdigest(), "size": len(self.data),
                "path": "/v1/gamedb/gamedbRC.slt.xz"}

    def gamedb_download(self, token, path, limit):
        self.downloads += 1
        return self.blob if self.blob is not None else lzma.compress(self.data)


class SyncTests(unittest.TestCase):
    def setUp(self):
        self.mount = tempfile.mkdtemp()          # the Azimuth folder
        self.target = os.path.join(self.mount, "fh2mp", "gamedbRC.slt")

    def read(self):
        with open(self.target, "rb") as f:
            return f.read()

    def test_downloads_once_then_keeps_it(self):
        api = FakeApi()
        self.assertEqual(gamedb.sync(api, "t", api.manifest(), self.mount), "car database updated")
        self.assertEqual(self.read(), DB)
        self.assertEqual(gamedb.sync(api, "t", api.manifest(), self.mount), "")
        self.assertEqual(api.downloads, 1)

    def test_a_tampered_copy_is_replaced(self):
        api = FakeApi()
        gamedb.sync(api, "t", api.manifest(), self.mount)
        with open(self.target, "r+b") as f:
            f.write(b"SQLite format 3\x00PANZER")
        self.assertEqual(gamedb.sync(api, "t", api.manifest(), self.mount), "car database updated")
        self.assertEqual(self.read(), DB)

    def test_a_bad_download_never_replaces_the_copy(self):
        good = FakeApi()
        gamedb.sync(good, "t", good.manifest(), self.mount)
        newer = DB + b"newcar"
        for blob in (lzma.compress(b"something else"), b"not xz at all", lzma.compress(newer + b"x" * 10_000)):
            api = FakeApi(newer, blob)
            with self.assertRaises(gamedb.GameDbError):
                gamedb.sync(api, "t", api.manifest(), self.mount)
            self.assertEqual(self.read(), DB)

    def test_bad_descriptions_and_no_game(self):
        api = FakeApi()
        for change in ({"sha256": "zz"}, {"size": 0}, {"size": gamedb.MAX_SIZE + 1}, {"path": "https://elsewhere/x"}):
            with self.assertRaises(gamedb.GameDbError):
                gamedb.sync(api, "t", {**api.manifest(), **change}, self.mount)
        with self.assertRaises(gamedb.GameDbError):
            gamedb.sync(api, "t", api.manifest(), "")


class MediaOverrideTests(unittest.TestCase):
    def ini(self, text):
        folder = tempfile.mkdtemp()
        path = os.path.join(folder, "FH2_ExtraOptions.ini")             # next to azimuth.exe, with Extra Options' winmm.dll
        with open(path, "wb") as f:
            f.write(text)
        return folder, path

    def test_turned_on_and_left_alone(self):
        folder, path = self.ini(b"[Global]\r\nEnabled=1\r\n[Files]\r\n; loose assets\r\nEnableMediaOverrides=0\r\n")
        self.assertTrue(gamedb.ensure_media_overrides(folder))
        with open(path, "rb") as f:
            self.assertEqual(f.read(), b"[Global]\r\nEnabled=1\r\n[Files]\r\n; loose assets\r\nEnableMediaOverrides=1\r\n")
        self.assertFalse(gamedb.ensure_media_overrides(folder))

    def test_added_when_missing(self):
        folder, path = self.ini(b"[Global]\r\nEnabled=1\r\n[Files]\r\nOther=2\r\n")
        self.assertTrue(gamedb.ensure_media_overrides(folder))
        with open(path, "rb") as f:
            self.assertEqual(f.read(), b"[Global]\r\nEnabled=1\r\n[Files]\r\nEnableMediaOverrides=1\r\nOther=2\r\n")
        folder, path = self.ini(b"[Global]\nEnabled=1\n")
        gamedb.ensure_media_overrides(folder)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), b"[Global]\nEnabled=1\n\n[Files]\nEnableMediaOverrides=1\n")

    def test_no_extra_options(self):
        with self.assertRaises(gamedb.GameDbError):
            gamedb.ensure_media_overrides(tempfile.mkdtemp())


class SessionTests(unittest.TestCase):
    def test_the_session_tells_the_shim_where_the_database_is(self):
        from fh2mp_launcher import shim
        az = tempfile.mkdtemp()
        shim.write_session(az, 0x9000000000abc, "fh2.t", "http://x", gamedb.session_path())
        with open(os.path.join(az, "fh2mp-session.txt"), "rb") as f:
            self.assertEqual(f.read(), b"xuid=9000000000abc\ntoken=fh2.t\ngamedb=fh2mp\\gamedbRC.slt\n")      # relative to the Azimuth folder
        shim.write_session(az, 1, "t", "http://x")
        with open(os.path.join(az, "fh2mp-session.txt"), "rb") as f:
            self.assertNotIn(b"gamedb", f.read())


if __name__ == "__main__":
    unittest.main()
