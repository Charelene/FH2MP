"""Native-schema parts validation and pre-relay policy tests; no real sockets or game input."""
import json
import os
from pathlib import Path
import struct
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S
import parts
import player_setup as C
from test_freeroam_tag_grant import world

INCIDENTS = json.loads(Path(__file__).with_name('test_parts_incidents.json').read_text())


def values(size, payload):
    return {k: v for k, (_offset, _width, v) in C.fields(size, payload).items()}


def wire(words, extra=None):
    return C.encode({**(extra or {}), C.PARTS: C.parts_value(words)})


def authority(message):
    v, n = S.bake(message)
    r = S.BitReader(v.to_bytes((n + 7) // 8, 'little'))
    assert (r.bits(7), r.bits(4), r.bits(2)) == (S.MSG_OBJECT_UPDATE, 1, 3)
    oid, seq, bits = r.bits(S.ROUTER_BITS[1][0]), r.bits(8), r.bits(14)
    return oid, seq, values(bits, r.bits(bits))


class Schema(unittest.TestCase):
    def test_captured_fields_are_located_without_validating_their_values(self):
        for f, offset in zip(INCIDENTS, (48, 897, 88)):
            p = int.from_bytes(bytes.fromhex(f['hex']), 'little')
            fields = C.fields(f['bits'], p)
            self.assertEqual(fields[C.PARTS][:2], (offset, 1376))
            self.assertEqual(C.encode(values(f['bits'], p)), (f['bits'], p))
            words = C.part_words(fields[C.PARTS][2])
            self.assertIsNone(parts.find_parts(f['bits'], p))
            self.assertIsNone(parts.find_shaped(f['bits'], p))
            self.assertIsNone(parts.find_loose(f['bits'], p))
            self.assertTrue(parts.integrity_errors(words))

    def test_values_cannot_be_interpreted_as_variable_headers(self):
        body = {121: (107 | (368335 << 8)), 158: 107, 161: 0xFFFFFFFF}
        self.assertEqual(values(*C.encode(body)), body)
        self.assertNotIn(C.PARTS, values(*C.encode(body)))

    def test_truncation_duplicates_unknown_fields_and_bad_strings_are_rejected(self):
        for size, payload in [(8, 107), (16, 200), (18, 12 | (12 << 9)),
                              (13, 3 | (31 << 8)), (7, 12), (9, 1 << 10)]:
            with self.subTest(size=size, payload=payload), self.assertRaises(ValueError):
                C.fields(size, payload)

    def test_every_stock_catalog_car_passes_integrity_validation(self):
        for car in parts.P.STOCK['CarBody']:
            stock = parts.stock_list(car)
            self.assertEqual(parts.integrity_errors(stock), [], car)


class Policies(unittest.TestCase):
    def setUp(self):
        self.w, (self.a, self.b) = world()
        self.w.log = lambda *_: None
        self.a.entered = self.b.entered = True
        self.policy = mock.patch.object(S, 'PART_POLICY', 'fix')
        self.policy.start()
        self.addCleanup(self.policy.stop)
        # #88 is a blocked traffic car, independently of its parts. Test the
        # parts policy separately; traffic rejection has its own regression.
        self.cars = mock.patch.object(S, 'CAR_POLICY', 'log')
        self.cars.start()
        self.addCleanup(self.cars.stop)

    def incident(self, f):
        return f['bits'], int.from_bytes(bytes.fromhex(f['hex']), 'little')

    def test_all_captured_inputs_are_repaired_for_owner_and_receivers(self):
        for f in INCIDENTS:
            with self.subTest(issue=f['n']), mock.patch.object(self.w, 'send') as send:
                self.a.parts_repair_car = None
                original_size, original = self.incident(f)
                before = values(original_size, original)
                bad, hide, size, payload = self.w.check_parts(self.a, original_size, original, oid=1)
                self.assertTrue(bad)
                self.assertFalse(hide)
                after = values(size, payload)
                car = C.part_words(before[C.PARTS])[2] // 1000
                self.assertEqual(C.part_words(after[C.PARTS]), parts.stock_list(car))
                self.assertEqual(after[C.USE_STOCK], 1)
                self.assertEqual(after[121], 0)
                self.assertTrue(all(after[v] == 0xBF800000 for v in C.TUNE_IDS if v != 121))
                self.assertEqual(after[81], S.CAR_CHOICES[car][0])
                self.assertAlmostEqual(struct.unpack('<f', struct.pack('<I', after[82]))[0], S.CAR_CHOICES[car][1], places=6)
                _peer, messages = send.call_args.args
                oid, _seq, owner = authority(messages[0])
                self.assertEqual(oid, 1)
                for vid in owner:
                    self.assertEqual(owner[vid], after[vid])
                for vid in before.keys() - owner.keys():
                    self.assertEqual(before[vid], after[vid])
                self.assertIsNone(self.a.kick_reason)

    def test_kick_and_combined_policies_withhold_every_captured_object(self):
        for policy in ('kick', 'fix+kick'):
            for f in INCIDENTS:
                with self.subTest(policy=policy, issue=f['n']), mock.patch.object(S, 'PART_POLICY', policy), \
                        mock.patch.object(self.w, 'send') as send:
                    self.a.kick_reason = self.a.parts_repair_car = None
                    bad, hide, _size, _payload = self.w.check_parts(self.a, *self.incident(f))
                    self.assertEqual((bad, hide), (True, True))
                    self.assertIsNotNone(self.a.kick_reason)
                    self.assertEqual(send.called, policy == 'fix+kick')

    def test_valid_supported_engine_swap_is_not_detuned(self):
        words = parts.stock_list(368)
        words[0] = 368002
        engine = parts.P.INSTALLS['Engine'][words[0]]
        for slot, table in parts.P.SLOT_TABLE.items():
            if parts.P.OWNER_KIND.get(table) == 'engine':
                words[slot] = parts.P.STOCK.get(table, {}).get(engine, parts.NONE)
        incoming = wire(words, {110: 0x40800000})
        with mock.patch.object(self.w, 'send') as send:
            self.assertEqual(self.w.check_parts(self.a, *incoming), (False, False, *incoming))
            send.assert_not_called()

    def test_invalid_values_do_not_need_any_recognizable_part_ids(self):
        words = [0x12345678] * 43
        words[2] = 368000
        bad, hide, size, payload = self.w.check_parts(self.a, *wire(words))
        self.assertEqual((bad, hide), (True, False))
        self.assertEqual(C.part_words(values(size, payload)[C.PARTS]), parts.stock_list(368))

    def test_required_camshaft_and_cross_owner_rows_trigger_repair(self):
        for slot, value in [(12, parts.NONE), (0, 1110000), (4, parts.P.STOCK['Brakes'][1110])]:
            words = parts.stock_list(368)
            words[slot] = value
            self.a.parts_repair_car = None
            bad, hide, size, payload = self.w.check_parts(self.a, *wire(words))
            self.assertEqual((bad, hide), (True, False))
            self.assertEqual(C.part_words(values(size, payload)[C.PARTS]), parts.stock_list(368))

    def test_later_tune_and_pi_deltas_cannot_restore_a_repaired_setup(self):
        self.w.check_parts(self.a, *self.incident(INCIDENTS[0]))
        incoming = C.encode({81: 7, 82: 0x3F800000, 110: 0x7F800000, 144: 0})
        bad, hide, size, payload = self.w.check_parts(self.a, *incoming)
        self.assertEqual((bad, hide), (True, False))
        after = values(size, payload)
        self.assertEqual(after[110], 0xBF800000)
        self.assertEqual(after[144], 1)
        self.assertEqual(after[81], S.CAR_CHOICES[1581][0])
        self.assertNotEqual(after[82], 0x3F800000)
        # A different valid car gets its own normal setup and upgrades.
        new_car = wire(parts.stock_list(368), {103: 368})
        self.assertEqual(self.w.check_parts(self.a, *new_car), (False, False, *new_car))
        self.assertIsNone(self.a.parts_repair_car)

    def test_unrelated_short_deltas_remain_unchanged(self):
        for body in ({27: 1234}, {12: 1, 19: 2}, {158: 3, 161: 4}):
            incoming = C.encode(body)
            self.assertEqual(self.w.check_parts(self.a, *incoming), (False, False, *incoming))

    def test_a_model_only_delta_cannot_unlock_a_repaired_car(self):
        self.w.check_parts(self.a, *self.incident(INCIDENTS[0]))
        self.assertTrue(self.w.check_parts(self.a, *C.encode({103: 368}))[1])
        self.assertEqual(self.a.parts_repair_car, 1581)

    def test_combined_policy_sends_repair_before_the_kick_can_drop_its_queue(self):
        sent = []
        self.w.sock = mock.Mock()
        self.w.sock.sendto.side_effect = lambda data, addr: sent.append((data, addr))
        with mock.patch.object(S, 'PART_POLICY', 'fix+kick'):
            bad, hide, _bits, _payload = self.w.check_parts(self.a, *self.incident(INCIDENTS[0]))
        self.assertEqual((bad, hide), (True, True))
        self.assertTrue(sent)
        self.assertEqual(sent[-1][1], self.a.addr)
        self.assertEqual(self.a.outbox, [])
        self.assertIsNotNone(self.a.kick_reason)

    def test_model_body_mismatch_and_malformed_serialization_are_withheld_without_kick(self):
        for incoming in [wire(parts.stock_list(368), {103: 1110}), (8, 107), (40, 255)]:
            self.assertTrue(self.w.check_parts(self.a, *incoming)[1])
            self.assertIsNone(self.a.kick_reason)

    def receive_object(self, size, payload, create=True):
        def message(w):
            w.bits(S.MSG_OBJECT_CREATE if create else S.MSG_OBJECT_UPDATE, 7)
            w.bits(1, 4)
            w.bits(0, 2)
            w.bits(0, S.ROUTER_BITS[1][0])
            w.bits(0, 8)
            w.bits(size, 14)
            w.bits(payload, size)
        self.w.handle(S.build_packet([message]), self.a.addr)

    def test_kick_blocks_cache_and_all_receivers_at_the_packet_handler(self):
        for policy in ('kick', 'fix+kick'):
            with mock.patch.object(S, 'PART_POLICY', policy):
                self.a.kick_reason = self.a.parts_repair_car = None
                self.receive_object(*self.incident(INCIDENTS[1]))
                self.assertNotIn((1, 0), self.a.objs)
                self.assertEqual(self.b.outbox, [])
                self.assertIsNotNone(self.a.kick_reason)

    def test_replay_cache_contains_only_repaired_payload(self):
        self.receive_object(*self.incident(INCIDENTS[1]))
        hist = self.a.objs[(1, 0)]
        self.assertEqual(len(hist), 1)
        _kind, _tid, _seq, bits, payload = hist[0]
        after = values(bits, payload)
        self.assertEqual(C.part_words(after[C.PARTS]), parts.stock_list(1581))
        self.assertEqual(after[C.USE_STOCK], 1)
        self.assertTrue(self.b.outbox)

    def test_malformed_object_is_not_cached_or_forwarded(self):
        self.receive_object(8, 107)
        self.assertEqual(self.a.objs, {})
        self.assertEqual(self.b.outbox, [])
        self.assertIsNone(self.a.kick_reason)


if __name__ == '__main__':
    unittest.main()
