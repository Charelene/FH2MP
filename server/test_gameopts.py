"""Event options: the Teams switch of a Free Roam event (gameopts.py) and the teams it produces (fh2mp_server.assign_teams)."""
import os
import random
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S  # noqa: E402
import gameopts  # noqa: E402

# real payloads: a Road Trip team race (dev server 2026-10-22, 316 bits, NumTeams = 2) and a Free Roam A-class circuit race (prod, 247 bits)
TEAM = (316, "04640e00001470860000f0c0321088ac92adac68192d000000e03400000021d66cadaf95ad7ad80b")
NORMAL = (247, "0300013002000005ec0300001c00671100e0c14065461b000000608dd6901e")


def parsed(p):
    return p[0], int.from_bytes(bytes.fromhex(p[1]), "little")


class NumTeams(unittest.TestCase):
    def test_real_payloads(self):
        self.assertEqual(gameopts.num_teams(*parsed(TEAM)), 2)
        self.assertTrue(gameopts.teams_on(parsed(TEAM)))
        self.assertEqual(gameopts.num_teams(*parsed(NORMAL)), 0)         # the game writes NumTeams = 0 with TeamScoringType
        self.assertFalse(gameopts.teams_on(parsed(NORMAL)))
        self.assertFalse(gameopts.teams_on(None))

    def test_built_payloads(self):
        def stream(*items):
            v, pos = 0, 0
            for vid, width, val in items:
                v |= (vid | (val << 10)) << pos
                pos += 10 + width
            return pos, v
        self.assertEqual(gameopts.num_teams(*stream((3, 3, 0), (428, 2, 2), (429, 2, 1), (500, 1, 1))), 2)
        self.assertEqual(gameopts.num_teams(*stream((428, 2, 0), (429, 2, 0))), 0)
        self.assertFalse(gameopts.teams_on(stream((428, 2, 1), (429, 2, 0))))
        # a lone [428][2] without the scoring type behind it is not trusted
        self.assertIsNone(gameopts.num_teams(*stream((428, 2, 2), (500, 2, 0))))

    def test_noise_almost_never_says_teams(self):
        hits = sum(gameopts.teams_on((800, random.Random(seed).getrandbits(800))) for seed in range(3000))
        self.assertLessEqual(hits, 3)                 # expected about 0.6: 800 offsets x 1 in 4 million


class FreeRoamTeams(unittest.TestCase):
    def world(self, n=4):
        import test_freeroam_tag_grant as T
        w = S.Server(0, None, sock=T.FakeSock(), log_file=None)
        peers = []
        for i in range(1, n + 1):
            p = S.Peer(("10.0.0.%d" % i, 5000 + i))
            p.connected, p.owner_id = True, i
            p.console_vars[4] = 1
            p.choices = (3, 0.750)
            w.peers[p.addr] = p
            peers.append(p)
        writes = []
        w.set_player_team = lambda q, t: (writes.append((q.owner_id, t)), setattr(q, "team_id", t))
        w.set_console_bool = lambda *a: None
        w.set_session_host_bool = lambda *a: None
        return w, peers, writes

    def test_teams_on_splits_the_players_and_the_grid_alternates(self):
        w, peers, writes = self.world()
        w.game_options = parsed(TEAM)
        w.start_event(peers[0])
        self.assertEqual(sorted(t for _, t in writes), [0, 0, 1, 1])
        self.assertEqual(len(w.event["order"]), 4)
        self.assertEqual([w.event["team"][q] for q in w.event["order"]], [0, 1, 0, 1])

    def test_teams_off_leaves_everybody_alone(self):
        w, peers, writes = self.world()
        w.game_options = parsed(NORMAL)
        w.start_event(peers[0])
        self.assertEqual(writes, [])
        self.assertNotIn("team", w.event)

    def test_a_player_left_on_a_team_by_an_earlier_event_is_cleared(self):
        w, peers, writes = self.world(2)
        w.game_options = parsed(NORMAL)
        peers[1].team_id = 1
        w.start_event(peers[0])
        self.assertEqual(writes, [(2, 0)])


class LapLimits(unittest.TestCase):
    @staticmethod
    def laps(value):
        return 42, gameopts.NUM_LAPS_ID | (value << 10)

    def test_real_payloads(self):
        from test_freeroam_tag_grant import options
        for name, expected in (("FR_CIRCUIT", 3), ("FR_SPRINT", 1), ("RT_LAPS", 2)):
            size, payload = options(name)
            self.assertEqual(gameopts.lap_field(size, payload)[1], expected)
            self.assertEqual(gameopts.bounded_laps(size, payload), (size, payload, expected))
        self.assertEqual(gameopts.lap_field(*parsed(NORMAL))[1], 3)
        self.assertEqual(gameopts.lap_field(*parsed(TEAM))[1], 1)

    def test_zero_huge_and_valid_counts(self):
        for raw, expected in ((0, 1), (1, 1), (3, 3), (10, 10), (11, 10), (999999, 10), (0xFFFFFFFF, 10)):
            size, payload, laps = gameopts.bounded_laps(*self.laps(raw))
            self.assertEqual(laps, expected)
            self.assertEqual(gameopts.lap_field(size, payload)[1], expected)

    def test_missing_count_is_inserted_before_the_suffix(self):
        size, payload = 12, gameopts.NUM_TEAMS_ID | (2 << 10)
        n, v, laps = gameopts.bounded_laps(size, payload, default=7)
        self.assertEqual((n, laps), (67, 7))
        self.assertEqual(v >> 55, payload)
        self.assertEqual(gameopts.bounded_laps(n, v, default=1), (n, v, 7))

    def test_other_fields_are_unchanged(self):
        size, payload, _ = gameopts.bounded_laps(*parsed(NORMAL))
        offset, _ = gameopts.lap_field(size, payload)
        mask = 0xFFFFFFFF << (offset + 10)
        oversized = (payload & ~mask) | (1000 << (offset + 10))
        n, v, _ = gameopts.bounded_laps(size, oversized)
        self.assertEqual(n, size)
        self.assertEqual(v & ~mask, payload & ~mask)
        self.assertEqual(gameopts.num_teams(n, v), 0)

    def test_a_lap_id_inside_a_track_value_is_not_modified(self):
        fake = gameopts.NUM_LAPS_ID | (500000 << 10)
        size, payload = 42, 4 | ((fake & 0xFFFFFFFF) << 10)
        n, v, _ = gameopts.bounded_laps(size, payload)
        self.assertEqual(v & ((1 << size) - 1), payload)
        self.assertEqual(gameopts.lap_field(n, v)[1], 1)

    def test_bucket_names_and_restriction_lists_are_skipped_by_their_native_sizes(self):
        w = S.BitWriter()
        w.bits(22, 10)  # first bucket name
        w.bits(2, 5)
        w.bits(ord('A'), 16)
        w.bits(ord('B'), 16)
        w.bits(67, 10)  # blacklist (50 u16s)
        w.bits((1 << 800) - 1, 800)
        w.bits(419, 10)
        w.bits(100, 32)
        n, v, laps = gameopts.bounded_laps(w.pos, w.value)
        self.assertEqual(laps, 10)
        self.assertEqual(v & ((1 << (w.pos - 42)) - 1), w.value & ((1 << (w.pos - 42)) - 1))

    def test_none_end_condition_is_replaced_even_with_a_positive_lap_count(self):
        payload = 418 | ((419 | (5 << 10)) << 13)
        n, v, laps = gameopts.bounded_laps(55, payload)
        self.assertEqual(gameopts.end_condition_type(n, v), 1)
        self.assertEqual(laps, 5)

    def test_timed_and_other_native_end_conditions_are_preserved(self):
        for condition in range(2, 7):
            payload = 418 | (condition << 10) | ((419 | (3 << 10)) << 13)
            n, v, laps = gameopts.bounded_laps(55, payload)
            self.assertEqual((n, v, laps), (55, payload, 3))

    def test_truncated_or_out_of_order_prefix_is_rejected(self):
        for size, payload in ((9, 419), (41, 419), (15, 22 | (25 << 10)), (20, 4 | (3 << 10))):
            with self.assertRaises(ValueError):
                gameopts.bounded_laps(size, payload)


class LapSync(unittest.TestCase):
    def setUp(self):
        from test_freeroam_tag_grant import world
        self.w, (self.host, self.guest) = world()
        self.w.log = lambda *a: None
        for q in (self.host, self.guest):
            q.entered = True
            q.choices = (3, 0.750)

    def write_options(self, size, payload, seq=20):
        msg = S.transfer_update_message(S.ROUTER_LOBBY, 0, S.LOBBY_ID_BITS, seq, size, payload)
        self.w.handle(S.build_packet([msg]), self.host.addr)

    def read_options(self, q):
        value, bits = q.outbox[-1]
        r = S.BitReader(value.to_bytes((bits + 7) // 8, 'little'))
        self.assertEqual(r.bits(7), S.MSG_OBJECT_UPDATE)
        self.assertEqual(r.bits(4), S.ROUTER_LOBBY)
        self.assertEqual((r.bits(1), r.bits(1), r.bits(S.LOBBY_ID_BITS)), (1, 0, 0))
        seq = r.bits(8)
        size = r.bits(14)
        return seq, size, r.bits(size)

    def test_host_and_guest_receive_the_same_capped_options(self):
        self.write_options(*LapLimits.laps(1000))
        host_seq, size, payload = self.read_options(self.host)
        self.assertEqual(host_seq, 21)  # follows the host's own write, so it accepts the correction
        self.assertEqual(self.read_options(self.guest)[1:], (size, payload))
        self.assertEqual(gameopts.lap_field(size, payload)[1], 10)
        self.assertEqual(self.w.free_roam_laps, 10)
        self.w.start_event(self.host)
        self.assertEqual(self.w.event['numlaps'], 10)

    def test_zero_is_corrected_on_the_host_as_well(self):
        self.write_options(*LapLimits.laps(0))
        self.assertEqual(gameopts.lap_field(*self.read_options(self.host)[1:])[1], 1)

    def test_echo_stops_and_a_sparse_update_retains_the_count(self):
        self.write_options(*LapLimits.laps(100))
        options = self.w.game_options
        guest_count = len(self.guest.outbox)
        self.host.outbox.clear()
        self.write_options(*options, seq=22)
        self.assertEqual(self.w.game_options, options)
        self.assertEqual(len(self.guest.outbox), guest_count)
        self.assertEqual(self.host.outbox, [])
        self.write_options(18, 6 | (3 << 10), seq=23)  # TimeOfDayIndex only
        self.assertEqual(gameopts.lap_field(*self.w.game_options)[1], 10)
        self.assertEqual(self.host.outbox, [])  # preserve other pending host UI edits
        self.assertEqual(gameopts.fields(*self.read_options(self.guest)[1:])[6], (8, 3))

    def test_default_is_explicit_before_the_start_flags(self):
        self.w.set_console_bool = lambda *a: self.assertTrue(self.host.outbox and self.guest.outbox)
        self.w.set_session_host_bool = lambda *a: None
        self.w.start_event(self.host)
        self.assertEqual(self.w.event['numlaps'], 1)
        self.assertEqual(gameopts.lap_field(*self.w.game_options)[1], 1)

    def test_road_trip_count_is_not_clamped(self):
        self.w.roadtrip = True
        self.write_options(*LapLimits.laps(25))
        self.assertEqual(gameopts.lap_field(*self.w.game_options)[1], 25)
        self.assertFalse(self.host.outbox)

    def test_lap_zero_and_early_keyframes_do_not_finish_a_circuit(self):
        self.w.game_options = parsed(NORMAL)
        self.w.start_event(self.host)
        ev = self.w.event
        ev.update(racing=True, finished={})
        def score(lap):
            val = lambda t: int.from_bytes(S.struct.pack('<d', t), 'little')
            self.w.note_lap_score(self.host, lap, [val(60 * max(1, lap)), val(60), val(60)])
        score(0)
        score(1)
        self.w.note_score_keyframe(self.host)
        self.assertNotIn(self.host, ev['finished'])
        score(2)
        self.assertNotIn(self.host, ev['finished'])
        score(3)
        self.assertIn(self.host, ev['finished'])

    def test_options_edits_cannot_change_a_race_that_is_loading_or_running(self):
        self.write_options(*LapLimits.laps(3))
        self.w.start_event(self.host)
        for racing in (False, True):
            self.w.event['racing'] = racing
            self.write_options(*LapLimits.laps(9), seq=30 + racing)
            self.assertEqual(self.w.free_roam_laps, 3)
            self.assertEqual(gameopts.lap_field(*self.w.game_options)[1], 3)
            self.assertEqual(gameopts.lap_field(*self.read_options(self.host)[1:])[1], 3)
        self.w.event['over'] = True
        self.write_options(*LapLimits.laps(9), seq=32)
        self.assertEqual(self.w.free_roam_laps, 9)

    def test_timed_mode_survives_a_sparse_update_and_does_not_use_lap_completion(self):
        self.write_options(13, 418 | (2 << 10))
        self.write_options(18, 6 | (3 << 10), seq=22)
        self.assertEqual(gameopts.end_condition_type(*self.w.game_options), 2)
        self.w.start_event(self.host)
        self.assertIsNone(self.w.event['numlaps'])


class RaceRestrictions(unittest.TestCase):
    setUp = LapSync.setUp
    write_options = LapSync.write_options
    read_options = LapSync.read_options
    # Consecutive writes by the South Scramble host, prod 2026-10-25.
    PRIOR = (716, "001c0c70044c0f000014b0be0000e080800702490028313333f3b3c4ccccdc0f133333733f4d084866b2a269b400000000d20000e086508bd4be66b600dbde060000f8831b000028506e01ba01ec16c01b406f06e555ef7ad803")
    SOUTH = (795, "00000c0004f80c00001420090000e040c003cb40200906250000007f96000000fc61020000f0a789ffc82ad9c98896d1040000004803000000380d000040083548cd6b653b1b806d6e03000009c20d0000f0273700dd107643e00da13780f2a867bdec05")

    def setup_b(self):
        self.write_options(*gameopts.encode_fields({30: (8, 2)}))
        self.host.choices, self.guest.choices = (2, .671), (2, .698)
        self.w.log = lambda *a: None

    def test_captured_delta_retains_b_and_late_join_receives_it(self):
        self.write_options(*parsed(self.PRIOR))
        self.write_options(*parsed(self.SOUTH), seq=22)
        self.assertEqual(gameopts.car_rule(self.w.game_options), (2, 0, 0))
        self.assertEqual(gameopts.fields(*self.w.game_options)[77], (8, 255))
        self.assertEqual(gameopts.fields(*self.w.game_options)[14], (8, 1))
        self.assertEqual(gameopts.fields(*self.read_options(self.guest)[1:])[30], (8, 2))
        self.host.choices, self.guest.choices = (2, .671), (1, .583)
        self.w.start_event(self.host)
        self.assertEqual(self.w.event['people'], [self.host])

    def test_explicit_unrestricted_clears_the_old_class(self):
        self.setup_b()
        self.write_options(*gameopts.encode_fields({30: (8, 255)}), seq=22)
        self.assertEqual(gameopts.car_rule(self.w.game_options)[0], 255)
        self.guest.choices = (1, .583)
        self.w.start_event(self.host)
        self.assertIn(self.guest, self.w.event['people'])

    def test_wrong_class_and_unknown_car_cannot_enter_grid(self):
        for choices in ((1, .583), (3, .750), None):
            with self.subTest(choices=choices):
                self.setUp()
                self.setup_b()
                self.guest.choices = choices
                self.w.start_event(self.host)
                self.assertEqual(self.w.event['people'], [self.host])

    def test_wrong_class_leader_cancels_start(self):
        self.setup_b()
        self.host.choices = (1, .583)
        self.w.start_event(self.host)
        self.assertIsNone(self.w.event)

    def test_car_change_while_loading_cannot_bypass_the_rule(self):
        self.setup_b()
        self.w.start_event(self.host)
        for q in (self.host, self.guest):
            q.console_vars[S.CONSOLE_VAR_HAS_LOADED_CARS] = 1
        self.guest.choices = (1, .583)
        self.w.maybe_begin_race()
        self.assertIsNone(self.w.event)
        self.assertEqual(self.host.console_vars[S.CONSOLE_VAR_PARTICIPATING_IN_RACE], 0)

    def test_car_change_on_grid_cannot_bypass_the_rule(self):
        self.setup_b()
        self.w.start_event(self.host)
        self.guest.choices = (1, .583)
        self.w.go_race()
        self.assertIsNone(self.w.event)

    def test_rule_is_fixed_for_the_event_that_is_loading(self):
        self.setup_b()
        self.w.start_event(self.host)
        self.write_options(*gameopts.encode_fields({30: (8, 255)}), seq=22)
        self.assertEqual(gameopts.car_rule(self.w.game_options)[0], 2)
        self.assertEqual(gameopts.car_rule(self.read_options(self.host)[1:])[0], 2)
        self.guest.choices = (1, .583)
        self.w.maybe_begin_race()
        self.assertIsNone(self.w.event)

    def test_malformed_delta_does_not_erase_restrictions(self):
        self.setup_b()
        before = self.w.game_options
        self.write_options(17, 30 | (255 << 10), seq=22)
        self.assertEqual(self.w.game_options, before)

    def test_previous_owner_cannot_override_new_leader(self):
        self.setup_b()
        self.w.ownership_grant(self.guest, S.ROUTER_LOBBY, S.TYPE_GAME_OPTIONS, 0, S.LOBBY_ID_BITS)
        before = self.w.game_options
        self.write_options(*gameopts.encode_fields({30: (8, 255)}), seq=22)
        self.assertEqual(self.w.game_options, before)

    def test_pi_boundaries_use_display_pi_and_reject_nonfinite(self):
        rule = (2, 650, 700)
        for pi in (.650, .671, .700):
            self.assertTrue(gameopts.accepts_car(rule, (2, pi)))
        for pi in (.649, .701, float('nan'), float('inf')):
            self.assertFalse(gameopts.accepts_car(rule, (2, pi)))

    def test_ready_request_rejects_c_class_in_b_event(self):
        self.setup_b()
        self.guest.choices = (1, .583)
        self.w.pending_race = (9999999999, self.host)
        def ready(w):
            w.bits(S.MSG_READY_REQUEST, 7)
            w.bits(1, 1)
            w.bits(0, 1)
            w.bits(0, 2)
            w.bits(1, 1)
        self.w.handle(S.build_packet([ready]), self.guest.addr)
        self.assertEqual(self.guest.console_vars[4], 0)
        responses = [m for m in self.guest.outbox if m[0] & 127 == S.MSG_READY_REQUEST_RESPONSE]
        self.assertEqual((responses[-1][0] >> (7 + S.LOBBY_EVENT_KEY_BITS)) & 3, 1)


if __name__ == "__main__":
    unittest.main()
