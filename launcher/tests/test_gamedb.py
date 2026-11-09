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

    def test_the_copy_it_replaces_is_remembered_for_the_shim(self):
        placed = os.path.join(self.mount, "fh2mp", "gamedb-placed.txt")

        def stamp():
            st = os.stat(self.target)
            return f"{st.st_size} {st.st_mtime_ns // 100 + 116444736000000000}"   # Windows' last-write time, as the shim reads it

        def lines():
            with open(placed, "rb") as f:
                return f.read().decode().splitlines()

        api = FakeApi()
        gamedb.sync(api, "t", api.manifest(), self.mount)
        self.assertFalse(os.path.exists(placed))                 # nothing replaced
        first = stamp()
        with open(placed, "wb") as f:                            # the shim's own lines, and one that isn't a stamp
            f.write(b"1 2\n3 4\nnot a stamp\n5 6\n")
        api = FakeApi(DB + b"newer")
        gamedb.sync(api, "t", api.manifest(), self.mount)
        self.assertEqual(lines(), ["1 2", "3 4", "5 6", first])
        second = stamp()
        api = FakeApi(DB + b"newest")
        gamedb.sync(api, "t", api.manifest(), self.mount)
        self.assertEqual(lines(), ["3 4", "5 6", first, second])   # the last 4
        self.assertEqual(gamedb.sync(api, "t", api.manifest(), self.mount), "")
        self.assertEqual(lines(), ["3 4", "5 6", first, second])   # unchanged when the copy is
        os.makedirs(placed + ".tmp" + str(os.getpid()))            # can't be written: playing goes on
        api = FakeApi(DB + b"again")
        self.assertEqual(gamedb.sync(api, "t", api.manifest(), self.mount), "car database updated")


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


class BasePartsTests(unittest.TestCase):
    def test_lists_the_part_tables_and_the_stock_cars_of_the_servers_database(self):
        import sqlite3
        az = tempfile.mkdtemp()
        os.makedirs(os.path.join(az, "fh2mp"))
        con = sqlite3.connect(os.path.join(az, *gamedb.LOCAL))
        con.executescript("CREATE TABLE List_UpgradeEngine (Id INTEGER); INSERT INTO List_UpgradeEngine VALUES (12), (5);"
                          "CREATE TABLE List_UpgradeTireCompound (Id INTEGER); INSERT INTO List_UpgradeTireCompound VALUES (900);"
                          "CREATE TABLE List_UpgradeCarBodyWeightPack (Id INTEGER); INSERT INTO List_UpgradeCarBodyWeightPack VALUES (7);"
                          "CREATE TABLE Data_Car (Id INTEGER, ClassID INTEGER, PerformanceIndex REAL); INSERT INTO Data_Car VALUES (1005, 3, 0.512);")
        con.commit()
        con.close()
        self.assertEqual(gamedb.write_base_parts(az), 5)
        with open(os.path.join(az, *gamedb.BASE_PARTS), "rb") as f:
            self.assertEqual(f.read().decode("ascii").splitlines(),
                             ["fh2mp-base-parts 1", "p Engine 5", "p Engine 12", "p TireCompound 900", "c 1005 3 512000"])   # tables the shim does not check are left out

    def test_an_unreadable_database_is_an_error(self):
        az = tempfile.mkdtemp()
        os.makedirs(os.path.join(az, "fh2mp"))
        with open(os.path.join(az, *gamedb.LOCAL), "wb") as f:
            f.write(b"not a database" * 100)
        with self.assertRaises(gamedb.GameDbError):
            gamedb.write_base_parts(az)


class StandinTests(unittest.TestCase):
    def test_a_custom_car_gets_the_closest_stock_cars_of_its_category(self):
        import json
        import sqlite3
        az = tempfile.mkdtemp()
        os.makedirs(os.path.join(az, "fh2mp"))
        con = sqlite3.connect(os.path.join(az, *gamedb.LOCAL))
        con.executescript("CREATE TABLE Data_Car (Id INTEGER, ClassID INTEGER, PerformanceIndex REAL);"
                          "CREATE TABLE Data_Car_Buckets (CarId INTEGER, CarBucket INTEGER, BucketHero INTEGER);"
                          "INSERT INTO Data_Car VALUES (1, 5, 0.95), (2, 4, 0.80), (3, 7, 1.0), (4, 5, 0.99);"
                          "INSERT INTO Data_Car_Buckets VALUES (1, 26, 0), (2, 26, 0), (3, 10, 0), (4, 3, 0);")
        con.commit()
        con.close()
        mod = os.path.join(az, "mods", "Pack")
        os.makedirs(mod)
        with open(os.path.join(mod, "mod.json"), "w") as f:
            json.dump({"database": "database.json"}, f)
        with open(os.path.join(mod, "database.json"), "w") as f:
            json.dump({"tables": {"Data_Car": {"insert": [{"Id": 9024, "ClassID": 7, "PerformanceIndex": 1.1}, {"Id": 2, "ClassID": 4}]},
                                  "Data_Car_Buckets": {"insert": [{"CarId": 9024, "CarBucket": 26}]}}}, f)
        # same category first (closest PI first), then the same class; car 2 is the server's own, so it is no custom car
        self.assertEqual(gamedb.custom_standins(az), "9024:1/2/3")
        with open(os.path.join(mod, "database.json"), "w") as f:
            json.dump({"tables": {"Data_Car": {"insert": [{"Id": 9024, "ClassID": 7, "PerformanceIndex": 1.1}, {"Id": 9031, "ClassID": 99}]},
                                  "Data_Car_Buckets": {"insert": [{"CarId": 9024, "CarBucket": 26}]}}}, f)
        self.assertEqual(gamedb.custom_standins(az), "9024:1/2/3,9031:0")    # no candidate: listed all the same, so the server knows it is here
        bad = os.path.join(az, "mods", "Evil")
        os.makedirs(bad)
        with open(os.path.join(bad, "mod.json"), "w") as f:
            json.dump({"database": "../../fh2mp/gamedbRC.slt"}, f)
        self.assertEqual(gamedb.custom_standins(az), "9024:1/2/3,9031:0")   # a path out of the mod's folder is not read

    def test_no_car_packs_nothing_to_send(self):
        self.assertEqual(gamedb.custom_standins(tempfile.mkdtemp()), "")
