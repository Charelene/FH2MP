"""The car database endpoints (/v1/gamedb): only for signed-in launchers, the file as it is now, 404 when the server sets none."""
import hashlib
import json
import lzma
import os
import sqlite3
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamedb_sync  # noqa: E402
import launcher_api  # noqa: E402
import social  # noqa: E402
from test_auth import AuthCase, Device  # noqa: E402


def make_db(path, cars):
    if os.path.exists(path):
        os.unlink(path)
    c = sqlite3.connect(path)
    c.execute("CREATE TABLE Data_Car (Id INTEGER PRIMARY KEY, MediaName TEXT)")
    c.executemany("INSERT INTO Data_Car VALUES (?, ?)", cars)
    c.commit()
    c.close()
    with open(path, "rb") as f:
        return f.read()


class GameDbApiTests(AuthCase):
    def setUp(self):
        super().setUp()
        self.dir = tempfile.mkdtemp()
        self.path = os.path.join(self.dir, "gamedbRC.slt")
        self.ctx = launcher_api.Ctx(self.svc, social.SocialStore(self.db), launcher_api.LauncherConfig(), lambda: 0,
                                    gamedb=gamedb_sync.GameDb(self.path))
        d = Device()
        self.svc.register(**d.register_args(self.svc, "Car Guy"))
        self.token, _a, _t = self.svc.login(**d.login_args(self.svc), client="t")

    def call(self, path, token=None):
        h = {"Authorization": f"Bearer {token}"} if token else {}
        return launcher_api.handle(self.ctx, "GET", path, h, b"", "203.0.113.5")

    def test_needs_a_session(self):
        make_db(self.path, [(1, "a")])
        self.assertEqual(self.call("/v1/gamedb")[0], 401)
        self.assertEqual(self.call(gamedb_sync.DOWNLOAD_PATH)[0], 401)

    def test_none_set(self):
        os.environ.pop("FH2MP_SYNC_GAMEDB", None)
        self.ctx.gamedb = gamedb_sync.GameDb()
        st, obj, _ = self.call("/v1/gamedb", self.token)
        self.assertEqual((st, obj["error"]), (404, "no_gamedb"))
        self.assertEqual(self.call(gamedb_sync.DOWNLOAD_PATH, self.token)[0], 404)

    def test_manifest_and_download_follow_the_file(self):
        raw = make_db(self.path, [(1, "a"), (2, "b")])
        st, m, _ = self.call("/v1/gamedb", self.token)
        self.assertEqual(st, 200, m)
        self.assertEqual((m["sha256"], m["size"], m["path"]), (hashlib.sha256(raw).hexdigest(), len(raw), gamedb_sync.DOWNLOAD_PATH))
        json.dumps(m)
        st, blob, ctype = self.call(m["path"], self.token)
        self.assertEqual((st, ctype, lzma.decompress(blob)), (200, "application/x-xz", raw))
        raw2 = make_db(self.path, [(1, "a"), (2, "b"), (3, "new car")])          # a new car pack goes live without a restart
        os.utime(self.path, ns=(1, 1))
        st, m2, _ = self.call("/v1/gamedb", self.token)
        self.assertEqual(m2["sha256"], hashlib.sha256(raw2).hexdigest())

    def test_not_a_database_is_not_served(self):
        with open(self.path, "wb") as f:
            f.write(b"junk" * 100)
        self.assertEqual(self.call("/v1/gamedb", self.token)[0], 404)

    def test_last_good_copy_survives_a_missing_file(self):
        raw = make_db(self.path, [(1, "a")])
        self.call("/v1/gamedb", self.token)
        os.unlink(self.path)
        st, m, _ = self.call("/v1/gamedb", self.token)
        self.assertEqual((st, m["sha256"]), (200, hashlib.sha256(raw).hexdigest()))

    def test_downloads_are_rate_limited(self):
        make_db(self.path, [(1, "a")])
        codes = [self.call(gamedb_sync.DOWNLOAD_PATH, self.token)[0] for _ in range(12)]
        self.assertEqual(codes[:10], [200] * 10)
        self.assertEqual(codes[-1], 429)


if __name__ == "__main__":
    unittest.main()
