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


stock_car = parts.stock_list


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

    # The part list of the object that wiped a prod lobby on 2026-10-23 (11 clients in one second): car 2179 with 14 ids the game does not have
    # (engine row 3104018, 2503088, 2632208, 91001005 ...). 26 of 41 ids known, so find_parts (34 needed) did not see it and it was relayed as it was.
    WIPING_LIST = [3104018, 2179000, 2179000, -1, 2503088, 2501596, 2179003, 2179003, 2503189, 2179000, 2179001, 2179001, 679003, 2632208, 679000,
                   2632147, 2632182, 2632180, 2632170, 2632183, 679003, -1, 91001005, 679003, 2596593, -1, -1, -1, -1, 679003, 2256003, 2256003,
                   2256003, 2256003, 2179000, 2179000, 2179000, 2179000, 2514043, 2514043, 2179003, 2179002, 92001010]

    def wiping_object(self, body=None):
        words = [w & 0xFFFFFFFF for w in self.WIPING_LIST]
        if body is not None:
            words[2] = body
        return player_object(dict(enumerate(words)), 842, size=3641, seed=7)

    def test_a_list_with_many_unknown_ids_is_found_by_its_shape_and_cleaned(self):
        w, (a, b) = self.world()
        w.log = lambda *_: None
        size, v = self.wiping_object()
        self.assertIsNone(parts.find_parts(size, v))        # the hole: the strict finder gives up
        self.assertEqual(parts.find_shaped(size, v)[0], 842)
        bad, hide, v2 = w.check_parts(a, size, v)
        self.assertTrue(bad)
        self.assertFalse(hide)
        self.assertNotEqual(v2, v)
        found = parts.find_parts(size, v2)                  # what the others get is a list every client can build
        self.assertEqual(found[0], 842)
        self.assertEqual(found[1][2], 2179000)              # the car itself stays
        self.assertEqual([x for x in found[1] if x != P.PART_NONE and x not in P.ALL_IDS], [])
        self.assertEqual(v2 & ~(((1 << (32 * P.PART_SLOTS)) - 1) << 842), v & ~(((1 << (32 * P.PART_SLOTS)) - 1) << 842))   # nothing else touched

    def test_a_list_with_ids_in_the_slots_without_a_part_type_is_found_and_cleaned(self):
        """Prod 2026-10-24 19:36:31: a mixed car (body 2287, rim / tire width / restrictor plate / motor ids in no catalog table, 9x ids in
        slots 3, 22, 26 and 42) was "no part list found" and relayed untouched; three clients and a rig died within a second."""
        w, (a, b) = self.world()
        w.log = lambda *_: None
        words = [P.PART_NONE] * P.PART_SLOTS
        for k, t in P.SLOT_TABLE.items():
            words[k] = min(P.TABLE_IDS[t])
        words[2] = 2179000
        for k, x in ((3, 94001010), (11, 900228713), (22, 91000980), (26, 93001005), (38, 1228700018), (39, 1228700039), (42, 92000995)):
            words[k] = x
        size, v = player_object(dict(enumerate(words)), 760, size=3799, seed=11)
        self.assertIsNone(parts.find_parts(size, v))
        self.assertIsNone(parts.find_shaped(size, v))
        self.assertEqual(parts.find_loose(size, v)[0], 760)
        bad, hide, v2 = w.check_parts(a, size, v)
        self.assertTrue(bad)
        self.assertFalse(hide)
        found = parts.find_parts(size, v2)                  # what the others get: a list every client can build
        self.assertEqual(found[0], 760)
        self.assertEqual(found[1][2], 2179000)
        self.assertEqual([x for x in found[1] if x != P.PART_NONE and x not in P.ALL_IDS], [])
        self.assertEqual((found[1][3], found[1][26]), (P.PART_NONE, P.PART_NONE))

    def test_noise_is_not_taken_for_a_loose_list(self):
        size, v = player_object({}, 0, size=3799, seed=5)
        self.assertIsNone(parts.find_loose(size, v))

    def test_a_list_whose_body_is_no_car_of_the_game_gets_the_stand_in_parts(self):
        import fh2mp_server as S
        w, (a, b) = self.world()
        w.log = lambda *_: None
        size, v = self.wiping_object(body=7777000)
        bad, hide, v2 = w.check_parts(a, size, v)
        self.assertTrue(bad)
        self.assertFalse(hide)
        found = parts.find_parts(size, v2)
        self.assertEqual(found[1][2], S.STANDIN_CAR * 1000)

    def test_noise_without_the_shape_is_not_taken_for_a_part_list(self):
        import random
        random.seed(11)
        for size in (1502, 2629, 3641):
            self.assertIsNone(parts.find_shaped(size, random.getrandbits(size)))

    def test_an_unreadable_object_is_dumped_whole_to_the_log_at_most_four_times_per_player(self):
        import random
        w, (a, b) = self.world()
        logged = []
        w.log = logged.append
        random.seed(5)
        size = 3641
        v = random.getrandbits(size)                        # no part list in noise (the shape needs two -1 words 800 bits apart)
        for _ in range(6):
            bad, hide, v2 = w.check_parts(a, size, v)
            self.assertEqual((bad, hide, v2), (False, False, v))        # still relayed as it came: this is a probe, not a rule
        odd = [l for l in logged if "ODD OBJECT" in l]
        self.assertEqual(len(odd), 4)
        self.assertIn(f"{size} bits hex={v.to_bytes((size + 7) // 8, 'little').hex()}", odd[0])
        self.assertIn(f"owner {a.owner_id} xuid {a.xuid:#x}", odd[0])

    def test_a_normal_car_is_left_alone(self):
        w, (a, b) = self.world()
        size, v = self.object_for(1110)                     # 1994 Mazda Miata
        bad, hide, _ = w.check_parts(a, size, v)
        self.assertFalse(hide)
        self.assertFalse(bad)
        self.assertEqual(a.model_id, 1110)
        self.assertIsNone(a.kick_reason)


def choices_bits(cls, pi, with_class=True):
    """A PlayerChoices run as a client sends it: [81 class] [82 PI] [83..87 ratings], 8-bit id + 32-bit value each."""
    import struct
    v, pos = 0, 0
    items = ([(81, cls & 0xFFFFFFFF)] if with_class else []) + [(82, struct.unpack("<I", struct.pack("<f", pi))[0])] + \
        [(83 + k, struct.unpack("<I", struct.pack("<f", 5.0 + k))[0]) for k in range(5)]
    for vid, raw in items:
        v |= (vid | (raw << 8)) << pos
        pos += 40
    return pos, v


class RentalChoices(unittest.TestCase):
    """2026-10-22, D'Orcia Circuit: every rental showed "R 999" on the starting grid (class R, the class's maximum PI)."""
    JAG = 1496                                         # JAG_XKRS_12, in RentalCars: class 3 (A), PI 0.730175

    def world(self):
        import test_freeroam_tag_grant as T
        return T.world()

    def test_the_codec_round_trips(self):
        bits, v = choices_bits(7, 1.1)
        found = parts.find_choices(bits, v)
        self.assertEqual(found, (48, 8))
        self.assertEqual(round(parts.read_choices(v, found)[1], 3), 1.1)
        self.assertEqual(parts.read_choices(v, found)[0], 7)
        v2 = parts.write_choices(v, found, 3, 0.730175)
        cls, pi = parts.read_choices(v2, found)
        self.assertEqual((cls, round(pi, 6)), (3, 0.730175))
        bits, v = choices_bits(0, 0.5, with_class=False)         # the class is left out when it did not change
        self.assertEqual(parts.find_choices(bits, v), (8, None))

    def test_noise_is_never_taken_for_choices(self):
        import random
        for seed in range(300):
            self.assertIsNone(parts.find_choices(2600, random.Random(seed).getrandbits(2600)), seed)

    def test_a_rental_that_joins_with_class_R_is_corrected_in_place(self):
        import fh2mp_server as S
        w, (a, b) = self.world()
        size, v = player_object(dict(enumerate(stock_car(self.JAG))), 910)
        bits, ch = choices_bits(7, 1.1)
        v = (v & ~(((1 << bits) - 1) << 353)) | (ch << 353)
        bad, hide, v2 = w.check_parts(a, size, v)
        self.assertEqual(a.rental_car, self.JAG)
        cls, pi = parts.read_choices(v2, parts.find_choices(size, v2))
        self.assertEqual((cls, round(pi, 4)), (S.CAR_CHOICES[self.JAG][0], round(S.CAR_CHOICES[self.JAG][1], 4)))
        self.assertEqual(S.CAR_CHOICES[self.JAG][0], 3)

    def test_a_rental_picked_later_gets_a_correction_update_after_its_parts(self):
        import fh2mp_server as S
        w, (a, b) = self.world()
        relayed = []
        w.remember_and_relay = lambda *x: relayed.append(x)
        bits, ch = choices_bits(7, 1.1)                          # the choices update comes first ...
        self.assertEqual(w.note_choices(a, bits, ch), ch)         # ... and the car is not known yet: left as sent
        self.assertEqual(a.choices[0], 7)
        size, v = player_object(dict(enumerate(stock_car(self.JAG))), 48, size=1502)    # ... then the car change with the part list
        w.check_parts(a, size, v)
        self.assertEqual(len(relayed), 1)
        _peer, mtype, router, oid, tid, seq, nbits, value = relayed[0]
        self.assertEqual((mtype, router, nbits), (S.MSG_OBJECT_UPDATE, 1, 80))
        self.assertEqual((value & 0xFF, (value >> 40) & 0xFF), (81, 82))          # [81 class][82 PI]
        cls, pi = parts.read_choices(value, (48, 8))
        self.assertEqual(cls, 3)
        self.assertAlmostEqual(pi, S.CAR_CHOICES[self.JAG][1], places=4)
        w.check_parts(a, size, v)                                # the same car again: nothing more to say
        self.assertEqual(len(relayed), 1)

    def test_the_short_class_R_update_after_a_rentals_parts_is_corrected(self):
        """Dev 2026-10-25, Road Trip car select: [81..87] with the real class, then the part list, then an 80-bit [81]=7 [82]=1.0 from the
        rental's own game; the standings of the others showed "R 999"."""
        import fh2mp_server as S
        w, (a, b) = self.world()
        w.remember_and_relay = lambda *x: None
        bits, ch = choices_bits(*S.CAR_CHOICES[self.JAG])
        w.note_choices(a, bits, ch)
        size, v = player_object(dict(enumerate(stock_car(self.JAG))), 48, size=1502)
        w.check_parts(a, size, v)
        nbits, short = parts.choices_update(7, 1.0)
        out = w.short_rental_choices(a, nbits, short)
        cls, pi = parts.read_short_choices(out, parts.find_short_choices(nbits, out))
        self.assertEqual(cls, 3)
        self.assertAlmostEqual(pi, S.CAR_CHOICES[self.JAG][1], places=4)
        w2, (c, _d) = self.world()                                 # a car that is not a rental keeps what its game sent
        self.assertEqual(w2.short_rental_choices(c, nbits, short), short)
        self.assertIsNone(parts.find_short_choices(40, 27 | (722772 << 8)))      # other 40-bit vars (id 27) are not choices

    def test_a_later_choices_update_of_a_rental_is_corrected_too(self):
        import fh2mp_server as S
        w, (a, b) = self.world()
        a.rental_car = self.JAG
        bits, ch = choices_bits(7, 1.1)
        out = w.note_choices(a, bits, ch)
        cls, pi = parts.read_choices(out, parts.find_choices(bits, out))
        self.assertEqual(cls, 3)
        self.assertAlmostEqual(pi, S.CAR_CHOICES[self.JAG][1], places=4)

    def test_cars_that_are_not_rentals_keep_what_their_game_sent(self):
        w, (a, b) = self.world()
        size, v = player_object(dict(enumerate(stock_car(368))), 910)
        bits, ch = choices_bits(4, 0.80)
        v = (v & ~(((1 << bits) - 1) << 353)) | (ch << 353)
        bad, hide, v2 = w.check_parts(a, size, v)
        self.assertIsNone(a.rental_car)
        self.assertEqual(v2, v)
        self.assertEqual(w.note_choices(a, bits, ch), ch)
        a.rental_car = self.JAG                                    # a stale rental flag must not touch a real class
        self.assertEqual(w.note_choices(a, bits, ch), ch)


class CarChangeUpdates(unittest.TestCase):
    """#42: an update that changes the car carries the part list at bit 48 (dev captures: 1462 and 1502 bits, every one of them)."""

    def test_the_list_is_found_at_bit_48_of_a_short_update(self):
        for size in (1462, 1502, 1664):
            size, v = player_object(dict(enumerate(stock_car(368))), 48, size=size)
            self.assertEqual(parts.find_parts(size, v), (48, stock_car(368)), size)

    def test_a_bad_part_in_a_car_change_is_fixed_too(self):
        import test_freeroam_tag_grant as T
        w, (a, b) = T.world()
        cars = stock_car(368)
        cars[0] = 368335
        size, v = player_object(dict(enumerate(cars)), 48, size=1462)
        bad, hide, v2 = w.check_parts(a, size, v)
        self.assertTrue(bad)
        self.assertEqual(parts.find_parts(size, v2)[1][0], 368000)

    def test_noise_alone_is_never_taken_for_a_list(self):
        import random
        for seed in range(200):
            rnd = random.Random(seed)
            self.assertIsNone(parts.find_parts(1502, rnd.getrandbits(1502)), seed)


class CarPacks(unittest.TestCase):
    """Cars that are not in the catalog (car packs): the server's own synced database makes them known; any other one is shown to the
    others as NUL_Car_00 (or removed, by policy), because building an unknown car crashes their game."""
    PACK = 9001

    def setUp(self):
        import copy
        import fh2mp_server as S
        self.S = S
        saved = {n: copy.deepcopy(getattr(P, n)) for n in ("TABLE_IDS", "ALL_IDS", "OWNER_KIND", "ROW_OWNER", "STOCK", "INSTALLS")}
        saved_s = (S.KNOWN_CARS, dict(S.TOP_SPEED_KMH), S.UNKNOWN_CAR_POLICY, S.SYNC_GAMEDB, dict(S._sync), dict(S.CAR_CHOICES), S.RENTAL_CARS)

        def restore():
            for n, v in saved.items():
                setattr(P, n, v)
            S.KNOWN_CARS, top, S.UNKNOWN_CAR_POLICY, S.SYNC_GAMEDB, sync, choices, S.RENTAL_CARS = saved_s
            S.CAR_CHOICES.clear()
            S.CAR_CHOICES.update(choices)
            S.TOP_SPEED_KMH.clear()
            S.TOP_SPEED_KMH.update(top)
            S._sync.update(sync)
        self.addCleanup(restore)

    def pack_car(self):
        """A pack car built the importer's way: its own rows (car id x 1000) on a stock car's engine (the Integra's, a donor)."""
        words = stock_car(368)
        for k, t in P.SLOT_TABLE.items():
            if P.OWNER_KIND.get(t) == "car" and words[k] != P.PART_NONE:
                words[k] = self.PACK * 1000
        return words

    def world(self):
        import test_freeroam_tag_grant as T
        return T.world()

    def test_an_unknown_pack_car_is_found_where_the_normal_search_fails(self):
        size, v = player_object(dict(enumerate(self.pack_car())), 919)
        self.assertIsNone(parts.find_parts(size, v))
        self.assertEqual(parts.find_unknown_car(size, v), (919, self.pack_car()))
        size, v = player_object(dict(enumerate(stock_car(368))), 919)          # a catalog car is never "unknown"
        self.assertIsNone(parts.find_unknown_car(size, v))

    def test_the_others_see_the_stand_in_and_nobody_is_kicked(self):
        self.S.UNKNOWN_CAR_POLICY = "standin"
        w, (a, b) = self.world()
        size, v = player_object(dict(enumerate(self.pack_car())), 910)
        bad, hide, v2 = w.check_parts(a, size, v)
        self.assertEqual((bad, hide, a.kick_reason, a.model_id), (True, False, None, self.PACK))
        self.assertEqual(parts.find_parts(size, v2), (910, parts.stock_list(self.S.STANDIN_CAR)))
        self.assertEqual(parts.stock_list(1215)[2], 1215000)                     # NUL_Car_00 has a body row

    def test_kick_policy_removes_the_owner(self):
        self.S.UNKNOWN_CAR_POLICY = "kick"
        w, (a, b) = self.world()
        size, v = player_object(dict(enumerate(self.pack_car())), 910)
        bad, hide, _ = w.check_parts(a, size, v)
        self.assertTrue(hide)
        self.assertIn(str(self.PACK), a.kick_reason)

    def test_a_pack_car_in_the_synced_database_is_a_normal_car(self):
        import sqlite3
        import tempfile
        path = os.path.join(tempfile.mkdtemp(), "gamedbRC.slt")
        c = sqlite3.connect(path)
        c.execute('CREATE TABLE Data_Car (Id INTEGER, MediaName TEXT, "TopSpeed-mph" REAL, ClassID INTEGER, PerformanceIndex REAL)')
        c.execute("INSERT INTO Data_Car VALUES (?, 'ABC_Pack_20', 150, 3, 0.7)", (self.PACK,))
        c.execute("CREATE TABLE RentalCars (CarId INTEGER)")
        c.execute("INSERT INTO RentalCars VALUES (?)", (self.PACK,))
        for t, extra in (("Engine", "EngineID"), ("Drivetrain", "DrivetrainID"), ("Motor", "MotorID")):
            c.execute(f"CREATE TABLE List_Upgrade{t} (Id INTEGER, Ordinal INTEGER, {extra} INTEGER, IsStock INTEGER)")
        for t in [t for t in P.TABLE_IDS if P.OWNER_KIND.get(t) == "car" and t not in ("Engine", "Drivetrain", "Motor")]:
            c.execute(f"CREATE TABLE List_Upgrade{t} (Id INTEGER, Ordinal INTEGER, IsStock INTEGER)")
            c.execute(f"INSERT INTO List_Upgrade{t} VALUES (?, ?, 1)", (self.PACK * 1000, self.PACK))
        c.execute("INSERT INTO List_UpgradeEngine VALUES (?, ?, ?, 1)", (self.PACK * 1000, self.PACK, P.INSTALLS["Engine"][368000]))
        c.execute("INSERT INTO List_UpgradeDrivetrain VALUES (?, ?, ?, 1)", (self.PACK * 1000, self.PACK, P.INSTALLS["Drivetrain"][368000]))
        c.commit()
        c.close()
        self.S.SYNC_GAMEDB, self.S._sync["checked"] = path, -1e9
        w, (a, b) = self.world()
        size, v = player_object(dict(enumerate(self.pack_car())), 910)
        bad, hide, v2 = w.check_parts(a, size, v)
        self.assertIn(self.PACK, self.S.KNOWN_CARS)
        self.assertEqual(self.S.TOP_SPEED_KMH[self.PACK], 241)
        self.assertEqual((bad, hide, v2, a.model_id, a.kick_reason), (False, False, v, self.PACK, None))
        self.assertEqual(parts.stock_list(self.PACK), self.pack_car())
        self.assertEqual(self.S.CAR_CHOICES[self.PACK], (3, 0.7))              # a pack car's class and PI, and its rental flag, come from the synced database
        self.assertIn(self.PACK, self.S.RENTAL_CARS)
        self.assertFalse(self.S.load_sync_catalog(every=0))                     # unchanged file: not read again


class ChoicesSanity(unittest.TestCase):
    def test_nan_and_absurd_values_are_replaced_and_normal_ones_left_alone(self):
        import math
        import struct
        import test_freeroam_tag_grant as T
        bits, ch = choices_bits(3, 0.74)
        found = parts.find_choices(bits, ch)
        self.assertEqual(parts.sanitize_choices(ch, found), (ch, []))
        nan = struct.unpack("<I", struct.pack("<f", float("nan")))[0]
        bad = (ch & ~(0xFFFFFFFF << found[0])) | (nan << found[0])                         # PerfIndex = NaN
        bad = (bad & ~(0xFFFFFFFF << (found[0] + 40))) | (struct.unpack("<I", struct.pack("<f", 1e30))[0] << (found[0] + 40))   # rating 1 = 1e30
        out, notes = parts.sanitize_choices(bad, found)
        cls, pi = parts.read_choices(out, found)
        self.assertEqual((cls, pi), (3, 0.75))
        self.assertEqual(len(notes), 2)
        w, (a, b) = T.world()
        out2 = w.note_choices(a, bits, bad)
        self.assertTrue(math.isfinite(parts.read_choices(out2, found)[1]))
        self.assertEqual(a.choices[0], 3)


if __name__ == "__main__":
    unittest.main()
