"""The part list check (parts.py) on made-up player objects: the array is placed at a few bit offsets in noise."""
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import part_ids as P  # noqa: E402
import parts  # noqa: E402


def player_object(slot_values, offset, size=2600, seed=1):
    """A bit string of `size` bits: random noise with the 43-word part list written at `offset`."""
    rnd = random.Random(seed)
    v = rnd.getrandbits(size)
    for k in range(P.PART_SLOTS):
        w = slot_values.get(k, P.PART_NONE)
        v = (v & ~(0xFFFFFFFF << (offset + 32 * k))) | (w << (offset + 32 * k))
    return size, v


def stock_list():
    """Every slot that has a table gets the first id of that table; slots 3 and 26 stay -1."""
    return {k: min(P.TABLE_IDS[t]) for k, t in P.SLOT_TABLE.items()}


class PartCheck(unittest.TestCase):
    def test_a_good_car_is_found_at_any_offset_and_passes(self):
        for off in (910, 919, 700, 1000):
            size, v = player_object(stock_list(), off)
            found = parts.find_parts(size, v)
            self.assertIsNotNone(found, off)
            self.assertEqual(found[0], off)
            self.assertEqual(parts.audit_parts(found[1]), [], off)

    def test_the_camshaft_slot_is_slot_12(self):
        self.assertEqual(P.SLOT_TABLE[12], "EngineCamshaft")
        self.assertNotIn(3, P.SLOT_TABLE)
        self.assertNotIn(26, P.SLOT_TABLE)

    def test_an_unknown_camshaft_id_is_reported_with_its_slot(self):
        cars = stock_list()
        cars[12] = 558777
        size, v = player_object(cars, 910)
        offset, words = parts.find_parts(size, v)
        issues = parts.audit_parts(words)
        self.assertEqual([(s, w) for s, w, _ in issues], [(12, 558777)])
        self.assertIn("no such id", issues[0][2])

    def test_an_id_of_another_table_in_the_wrong_slot_is_reported_but_exists(self):
        cars = stock_list()
        cars[12] = min(P.TABLE_IDS["Brakes"])         # exists, but not as a camshaft
        if cars[12] in P.TABLE_IDS["EngineCamshaft"]:
            self.skipTest("the first brake id is also a camshaft id")
        size, v = player_object(cars, 910)
        offset, words = parts.find_parts(size, v)
        issues = parts.audit_parts(words)
        self.assertEqual(len(issues), 1)
        self.assertIn("another part table", issues[0][2])

    def test_noise_without_a_part_list_finds_nothing(self):
        rnd = random.Random(5)
        self.assertIsNone(parts.find_parts(2600, rnd.getrandbits(2600)))


class CarFromParts(unittest.TestCase):
    """The car id is read from slot 2 (CarBody row = car id x 1000) and handed to the car policy."""

    def world(self):
        import test_freeroam_tag_grant as T
        return T.world()

    def object_for(self, car_id):
        cars = stock_list()
        cars[2] = car_id * 1000
        return player_object(cars, 910)

    def test_a_traffic_car_is_withheld_and_its_owner_removed(self):
        import fh2mp_server as S
        w, (a, b) = self.world()
        size, v = self.object_for(283)                      # VW Bora VR6 '03: a traffic-only model
        bad, hide = w.check_parts(a, size, v)
        self.assertTrue(hide)
        self.assertEqual(a.model_id, 283)
        self.assertEqual(S.CAR_POLICY, "kick")
        self.assertIn("283", a.kick_reason)

    def test_a_normal_car_is_left_alone(self):
        w, (a, b) = self.world()
        size, v = self.object_for(1110)                     # 1994 Mazda Miata
        bad, hide = w.check_parts(a, size, v)
        self.assertFalse(hide)
        self.assertFalse(bad)
        self.assertEqual(a.model_id, 1110)
        self.assertIsNone(a.kick_reason)


if __name__ == "__main__":
    unittest.main()
