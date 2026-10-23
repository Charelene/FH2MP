"""Rental cars for the events (rentals.py, rivals.RivalCatalog.usable_themes, tools/make_rentals_db.py)."""
import os
import sqlite3
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rentals  # noqa: E402
import rivals  # noqa: E402

CATALOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data", "gamedata.sqlite")


class Plan(unittest.TestCase):
    def test_every_theme_gets_the_rentals_it_is_short_of(self):
        themes = {"a": {1, 2, 3, 4}, "b": {3, 4, 5, 6}, "c": {7}, "d": {1, 7, 8}}
        picks, short = rentals.plan_extra(themes, {2}, range(1, 9), target=2)
        have = {2, *picks}
        for k, cars in themes.items():
            self.assertGreaterEqual(len(cars & have), min(2, len(cars)), k)
        self.assertEqual(short, {})
        self.assertNotIn(2, picks)

    def test_a_car_that_closes_the_most_gaps_comes_first_and_ties_go_by_id(self):
        themes = {"a": {1, 5}, "b": {2, 5}, "c": {3, 5}}
        picks, _ = rentals.plan_extra(themes, set(), [1, 2, 3, 5], target=1)
        self.assertEqual(picks, [5])

    def test_cars_that_may_not_be_added_leave_the_theme_short_and_are_reported(self):
        picks, short = rentals.plan_extra({"a": {1, 2}}, set(), [1], target=2)
        self.assertEqual((picks, short), ([1], {"a": 1}))

    def test_enough(self):
        self.assertTrue(rentals.enough({1, 2, 3}, {1, 2}))
        self.assertFalse(rentals.enough({1, 2, 3}, {1}))
        self.assertTrue(rentals.enough({1}, {1}))            # a one-car theme needs only that car
        self.assertFalse(rentals.enough({1, 2}, set()))


@unittest.skipUnless(os.path.exists(CATALOG), "needs server/data/gamedata.sqlite (built from the game install)")
class RealCatalog(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db = sqlite3.connect(f"file:{CATALOG}?mode=ro", uri=True)

        class Cars:
            db = cls.db
        cls.cat = rivals.RivalCatalog(Cars())

    def test_stock_rentals_leave_most_events_without_one(self):
        """2026-10-22: 'Retro Icons Cup, C class' and the other themes offered no rental at all."""
        rent = self.cat.rentals()
        short = [t for t in self.cat.themes() if not t.get("avoid") and t["kind"] != "model"
                 and not rentals.enough({r[0] for r in self.cat.cars_for(t["attrs"])}, rent)]
        self.assertGreater(len(short), 40)

    def test_with_the_planned_rentals_every_theme_has_two(self):
        from car_policy import BLOCKED_CARS
        themes = [t for t in self.cat.themes() if not t.get("avoid") and t["kind"] != "model"]
        cars = {i: {r[0] for r in self.cat.cars_for(t["attrs"])} for i, t in enumerate(themes)}
        picks, short = rentals.plan_extra(cars, self.cat.rentals(), [r[0] for r in self.db.execute("SELECT id FROM v_cars")
                                                                      if r[0] not in BLOCKED_CARS])
        self.assertEqual(short, {})
        self.assertLess(len(picks), 60)
        full = self.cat.rentals() | set(picks)
        for i, t in enumerate(themes):
            self.assertTrue(rentals.enough(cars[i], full), t["title"])

    def test_events_are_only_generated_where_a_rental_fits(self):
        rent = self.cat.rentals()
        usable = self.cat.usable_themes()
        self.assertTrue(usable)
        for t in usable:
            self.assertTrue(rentals.enough({r[0] for r in self.cat.cars_for(t["attrs"])}, rent), t["title"])
        self.assertLess(len(usable), len([t for t in self.cat.themes() if not t.get("avoid")]))

    def test_no_rental_table_means_no_filter(self):
        class Cars:
            db = sqlite3.connect(":memory:")
        cat = rivals.RivalCatalog(Cars())
        self.assertEqual(cat.rentals(), set())


class ToolOutput(unittest.TestCase):
    @unittest.skipUnless(os.path.exists(CATALOG), "needs server/data/gamedata.sqlite")
    def test_the_tool_adds_only_rental_rows_and_keeps_the_rest(self):
        import subprocess
        root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
        with tempfile.TemporaryDirectory() as d:
            stock = os.path.join(d, "stock.slt")
            c = sqlite3.connect(stock)
            c.execute("CREATE TABLE Data_Car (Id INTEGER PRIMARY KEY)")
            c.execute("CREATE TABLE RentalCars ([CarId] INTEGER PRIMARY KEY, FOREIGN KEY (CarId) REFERENCES Data_Car(Id))")
            src = sqlite3.connect(f"file:{CATALOG}?mode=ro", uri=True)
            ids = [r[0] for r in src.execute("SELECT Id FROM Data_Car")]
            c.executemany("INSERT INTO Data_Car VALUES (?)", [(i,) for i in ids])
            c.executemany("INSERT INTO RentalCars VALUES (?)", [(r[0],) for r in src.execute("SELECT CarId FROM RentalCars")])
            c.commit()
            c.close()
            out = os.path.join(d, "out", "canonical.slt")
            r = subprocess.run([sys.executable, "-I", os.path.join(root, "tools", "make_rentals_db.py"), "--stock", stock, "--out", out,
                                "--catalog", CATALOG], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            o = sqlite3.connect(out)
            self.assertGreater(o.execute("SELECT COUNT(*) FROM RentalCars").fetchone()[0], 22)
            self.assertEqual(o.execute("SELECT COUNT(*) FROM Data_Car").fetchone()[0], len(ids))
            self.assertEqual(o.execute("PRAGMA integrity_check").fetchone()[0], "ok")
            o.close()
            st = sqlite3.connect(stock)
            self.assertEqual(st.execute("SELECT COUNT(*) FROM RentalCars").fetchone()[0], 22)      # the stock file is untouched
            st.close()


if __name__ == "__main__":
    unittest.main()
