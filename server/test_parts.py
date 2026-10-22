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


def stock_car(car):
    """The part list of a stock car: every slot holds its owner's stock row (or -1 where the owner has none)."""
    words = [P.PART_NONE] * P.PART_SLOTS
    for k in (0, 1, 2):
        words[k] = P.STOCK[P.SLOT_TABLE[k]][car]
    for k, t in P.SLOT_TABLE.items():
        if k in (0, 1, 2):
            continue
        owner = parts._expected_owner(t, words)
        words[k] = P.STOCK.get(t, {}).get(owner, P.PART_NONE)
    return words


class Neutralize(unittest.TestCase):
    INTEGRA = 368                                          # Acura Integra Type R '01: engine rows 368000-368002 (368002 = a swap)

    def test_a_stock_car_is_left_alone(self):
        w = stock_car(self.INTEGRA)
        self.assertEqual(parts.neutralize(w), (w, [], []))

    def test_a_made_up_engine_row_becomes_the_stock_engine(self):
        w = stock_car(self.INTEGRA)
        w[0] = 368335                                      # seen on prod 2026-10-22 01:26:08, lobby wiped at 01:26:09
        fixed, changes, odd = parts.neutralize(w)
        self.assertEqual(changes, [(0, 368335, 368000, "no such id")])
        self.assertEqual(fixed, stock_car(self.INTEGRA))

    def test_a_real_engine_swap_is_kept(self):
        w = stock_car(self.INTEGRA)
        w[0] = 368002
        eng = P.INSTALLS["Engine"][368002]
        for k, t in P.SLOT_TABLE.items():
            if P.OWNER_KIND.get(t) == "engine":
                w[k] = P.STOCK.get(t, {}).get(eng, P.PART_NONE)
        self.assertEqual(parts.neutralize(w), (w, [], []))

    def test_an_id_of_another_table_is_only_reported(self):
        w = stock_car(self.INTEGRA)
        w[4] = 0x1f2894                                    # Brakes slot, an id of another table (prod 2026-10-21 22:47)
        fixed, changes, odd = parts.neutralize(w)
        self.assertEqual((fixed, changes), (w, []))
        self.assertEqual([s for s, _, _ in odd], [4])

    def test_an_id_of_another_table_in_the_camshaft_slot_is_replaced(self):
        w = stock_car(self.INTEGRA)
        w[12] = P.STOCK["Brakes"][self.INTEGRA]            # not a camshaft row: no torque curve for the others
        fixed, changes, _ = parts.neutralize(w)
        self.assertEqual(changes, [(12, w[12], stock_car(self.INTEGRA)[12], "an id of another part table")])

    def test_the_electric_car_keeps_its_motor_row(self):
        w = [P.PART_NONE] * P.PART_SLOTS
        w[0], w[2] = P.STOCK["Motor"][2101], P.STOCK["CarBody"][2101]   # slot 0 holding the Motor row is a guess; it must stay either way
        w[42] = P.STOCK["MotorParts"][P.INSTALLS["Motor"][w[0]]]
        fixed, changes, _ = parts.neutralize(w)
        self.assertEqual((fixed, changes), (w, []))

    def test_a_missing_optional_part_becomes_none(self):
        w = stock_car(self.INTEGRA)
        k = next(k for k, t in P.SLOT_TABLE.items() if t == "EngineTurboTwin")
        w[k] = 98765432
        fixed, changes, _ = parts.neutralize(w)
        eng = P.INSTALLS["Engine"][w[0]]
        self.assertEqual(fixed[k], P.STOCK["EngineTurboTwin"].get(eng, P.PART_NONE))

    def test_a_row_of_another_engine_is_only_reported(self):
        w = stock_car(self.INTEGRA)
        other = next(i for i, o in P.ROW_OWNER["EngineCamshaft"].items() if o != P.INSTALLS["Engine"][w[0]])
        w[12] = other
        fixed, changes, odd = parts.neutralize(w)
        self.assertEqual((fixed, changes), (w, []))
        self.assertEqual([s for s, _, _ in odd], [12])

    def test_without_a_car_body_row_nothing_is_rewritten(self):
        w = stock_car(self.INTEGRA)
        w[0], w[2] = 0x83567b2, 0x6bffffff                 # the misaligned reads seen on prod
        self.assertEqual(parts.neutralize(w)[1], [])

    def test_the_rewritten_object_keeps_every_other_bit(self):
        w = stock_car(self.INTEGRA)
        w[0] = 368335
        size, v = player_object(dict(enumerate(w)), 910)
        fixed, _, _ = parts.neutralize(w)
        v2 = parts.write_parts(v, 910, fixed)
        mask = ((1 << (32 * P.PART_SLOTS)) - 1) << 910
        self.assertEqual(v2 & ~mask, v & ~mask)
        self.assertEqual(parts.find_parts(size, v2), (910, fixed))


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
        bad, hide, _ = w.check_parts(a, size, v)
        self.assertTrue(hide)
        self.assertEqual(a.model_id, 283)
        self.assertEqual(S.CAR_POLICY, "kick")
        self.assertIn("283", a.kick_reason)

    def test_a_made_up_engine_is_relayed_as_the_stock_engine_and_nobody_is_kicked(self):
        w, (a, b) = self.world()
        cars = stock_car(368)
        cars[0] = 368335
        size, v = player_object(dict(enumerate(cars)), 910)
        bad, hide, v2 = w.check_parts(a, size, v)
        self.assertTrue(bad)
        self.assertFalse(hide)
        self.assertIsNone(a.kick_reason)
        self.assertEqual(parts.find_parts(size, v2)[1][0], 368000)

    def test_a_normal_car_is_left_alone(self):
        w, (a, b) = self.world()
        size, v = self.object_for(1110)                     # 1994 Mazda Miata
        bad, hide, _ = w.check_parts(a, size, v)
        self.assertFalse(hide)
        self.assertFalse(bad)
        self.assertEqual(a.model_id, 1110)
        self.assertIsNone(a.kick_reason)


if __name__ == "__main__":
    unittest.main()
