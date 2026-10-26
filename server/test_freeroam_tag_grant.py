"""Free roam tag events (King / Infection read off the leader's GameOptions) and the GameOptions ownership grant of a non-leader.

The payloads are GameOptions objects as the game server logged them on prod (2026-10-19/20); the Road Trip ones belong to events whose mode is
known from the game database (142 King, 136 StreetRace, 37 LapsRace)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S  # noqa: E402
import gameopts

PAYLOADS = {
    "FR_KING": "001c0c7004240f000014e0bc0000e080c003cb40c08381240094989999f959626666ee87899999b99f260622eb6445136900007043a701000008a116a995ed6c01b6bd0d0000f00737000050a0dc027403d82d803780de0ccabb1e",
    "FR_KING_EARLIER": "001c0c70042c0f00001400bd0000e0804012004accccccfc2c313333f7c3c4ccccdc4f13049299ac681a0d00000080340000b821d422b5af992dc0b6b7010000fee00600000a945b806e00bb05f006d09b4179d3a3def500",
    "RT_KING": "001c0c7004a40f00001490bd0000e080c003cb40200900256666667e96989999fb61626666ee27b24e56349106000037741a000080106a915ad9ce1660dbdb0000007f7003000005ca2d403780dd027803e8cda0bceb01",
    "RT_STREETRACE": "00000c4004ac0e000014e0040100e04040120c4a000000fe2c010000f8c3040000e08f6427235a460b000000200d000000003548cd6b6703b0cd6d00002041b8010000fee406a01bc26e08bc21f406509ef4aa67bdec05",
    "RT_LAPS": "0300015f03000005900700003cb00c0422ab6445cb6802000000a701000008bd56b6ea612f",
    "FR_CIRCUIT": "0300013002000005ec0300003cb00c043c1888ac92152da30d0000009c06000020f45ad9aa87bd",
    "FR_SPRINT": "04fc0e000014205a0000f0c03210f08020b24a56b48c16000000701a000080d06b659b5ef500",
}
SIZES = {"FR_KING": 727, "FR_KING_EARLIER": 698, "RT_KING": 691, "RT_STREETRACE": 691, "RT_LAPS": 294, "FR_CIRCUIT": 312, "FR_SPRINT": 298}


def options(name):
    return SIZES[name], int.from_bytes(bytes.fromhex(PAYLOADS[name]), "little")


class FakeSock:
    def __getattr__(self, name):
        return lambda *a, **k: 0


def world(roadtrip=False):
    w = S.Server(0, None, sock=FakeSock(), log_file=None, roadtrip=roadtrip) if roadtrip else S.Server(0, None, sock=FakeSock(), log_file=None)
    peers = [S.Peer(("10.0.0.%d" % i, 5000 + i)) for i in (1, 2)]
    for i, p in enumerate(peers):
        p.connected = True
        p.owner_id = i + 1
        p.car_id = i
        p.choices = (3, 0.750)
        p.console_vars[4] = 1
        w.peers[p.addr] = p
    return w, peers


class TagModeFromGameOptions(unittest.TestCase):
    def test_king_payloads_are_recognised(self):
        for name in ("FR_KING", "FR_KING_EARLIER", "RT_KING"):
            self.assertEqual(S.game_options_tag_mode(options(name)), S.RACE_MODE_KING, name)

    def test_infection_header_is_recognised(self):
        # only the header is known (dev log 2026-10-20, Infection picked in Free Roam: 667 bits, 04600c2000); the rest is made up
        for size in (667, 731, 826):
            self.assertEqual(S.game_options_tag_mode((size, 0xABCDEF0000000000 | 0x04600C2000)), S.RACE_MODE_INFECTION, size)
        self.assertEqual(S.game_options_tag_mode((453, 0x1B600C2000)), S.RACE_MODE_INFECTION)    # dev 20:57: the same event, later in the session
        self.assertIsNone(S.game_options_tag_mode((300, 0x04600C2000)))              # a short options object is never a tag event
        for header in (0x04400C0000, 0x04200C0000, 0x1B400C0000, 0x1B200C0000):                                  # the other long headers seen on prod stay "not a tag mode"
            self.assertIsNone(S.game_options_tag_mode((800, header)))

    def test_short_king_options_are_still_king(self):
        # dev 2026-10-20: King picked in Free Roam gave 453 bits, header 1b700c1c00 (bits 10-12 set); the old 600-bit minimum missed it
        self.assertEqual(S.game_options_tag_mode((453, 0x1B700C1C00)), S.RACE_MODE_KING)
        self.assertIsNone(S.game_options_tag_mode((300, 0x1B700C1C00)))

    def test_readiness_left_over_from_the_last_event_does_not_start_the_grid(self):
        w, (a, b) = world()
        for q in (a, b):
            q.console_vars[S.CONSOLE_VAR_HAS_LOADED_CARS] = 1      # stale: reported for the previous event
            q.console_vars[S.CONSOLE_VAR_HAS_GRID] = 1
            q.console_vars[S.CONSOLE_VAR_RENDER_READY] = 1
        w.game_options = options("FR_KING")
        w.start_event(b)
        w.maybe_begin_race()
        self.assertFalse(w.event["begun"])                           # nobody has loaded yet
        a.console_vars[S.CONSOLE_VAR_HAS_LOADED_CARS] = 1
        w.maybe_begin_race()
        self.assertFalse(w.event["begun"])                           # one of two
        b.console_vars[S.CONSOLE_VAR_HAS_LOADED_CARS] = 1
        w.maybe_begin_race()
        self.assertTrue(w.event["begun"])

    def test_first_it_car_is_drawn_while_the_grid_is_up_and_the_race_end_waits_for_go(self):
        import time
        w, (a, b) = world()
        values = gameopts.fields(*options("FR_KING"))
        values.update({0: (8, 8), 3: (4, 6)})
        w.game_options = gameopts.encode_fields(values)
        w.start_event(a)
        for q in (a, b):
            q.console_vars[S.CONSOLE_VAR_HAS_LOADED_CARS] = 1
        w.maybe_begin_race()
        tag = w.event["tag"]
        self.assertIsNotNone(tag["assign_at"])                       # scheduled with the grid, not at GO
        self.assertFalse(tag["assigned"])
        tag["assign_at"] = time.monotonic() - 1
        w.tag_tick(time.monotonic())
        self.assertTrue(tag["assigned"])
        self.assertEqual(sorted(tag["buckets"].values()), [0, 1])
        w.tag_tick(time.monotonic() + 10)                            # no end time before GO: must neither crash nor end the event
        self.assertFalse(w.event.get("over"))
        w.go_race()
        self.assertIsNotNone(tag["end_at"])

    def test_tag_event_ends_when_every_client_has_sent_its_final_keyframe(self):
        import time
        w, (a, b) = world()
        w.game_options = options("FR_KING")
        w.start_event(a)
        for q in (a, b):
            q.console_vars[S.CONSOLE_VAR_HAS_LOADED_CARS] = 1
        w.maybe_begin_race()
        w.go_race()
        w.note_score_keyframe(a)                                     # too early after GO: ignored
        w.check_race_end(time.monotonic())
        self.assertFalse(w.event.get("over"))
        w.event["racing_since"] -= 60                                # the clients' 60 s limit has passed
        w.note_score_keyframe(a)
        w.check_race_end(time.monotonic())
        self.assertFalse(w.event.get("over"))                        # only one of two
        w.note_score_keyframe(b)
        w.check_race_end(time.monotonic())
        self.assertTrue(w.event.get("over"))

    def test_free_roam_infection_start_sets_up_infection_without_guessing(self):
        w, (a, b) = world()
        values = gameopts.fields(*options("FR_KING"))
        values.update({0: (8, 8), 3: (4, 6)})
        w.game_options = gameopts.encode_fields(values)
        w.start_event(a)
        tag = w.event["tag"]
        self.assertEqual(tag["mode"]["name"], "Infection")
        self.assertFalse(tag["guessed"])

    def test_other_events_are_not_tag_modes(self):
        for name in ("RT_STREETRACE", "RT_LAPS", "FR_CIRCUIT", "FR_SPRINT"):
            self.assertIsNone(S.game_options_tag_mode(options(name)), name)
        self.assertIsNone(S.game_options_tag_mode(None))

    def test_free_roam_event_start_sets_up_tag_state(self):
        w, (a, b) = world()
        w.game_options = options("FR_KING")
        w.start_event(a)
        tag = w.event["tag"]
        self.assertEqual(tag["mode"]["name"], "King")
        self.assertTrue(tag["guessed"])
        self.assertIsNone(w.event["numlaps"])

    def test_free_roam_circuit_race_gets_no_tag_state(self):
        for name in ("FR_CIRCUIT", "FR_SPRINT"):
            w, (a, b) = world()
            w.game_options = options(name)
            for q in (a, b):
                q.choices = (gameopts.car_rule(w.game_options)[0], 0.850)
            w.start_event(a)
            self.assertNotIn("tag", w.event, name)

    def test_no_game_options_means_no_tag_state(self):
        w, (a, b) = world()
        w.start_event(a)
        self.assertNotIn("tag", w.event)

    def test_guessed_king_turns_into_infection_when_the_it_car_stays_it(self):
        w, (a, b) = world()
        w.game_options = options("FR_KING")
        w.start_event(a)
        tag = w.event["tag"]
        w.event["racing"] = True
        tag["assigned"] = True
        tag["buckets"] = {a: S.TAG_BUCKET_IT, b: S.TAG_BUCKET_NOT_IT}
        w.tag_send = lambda why: None
        w.tag_collision(a, ("own", 0, 0), ("relay", 1, 2), 1, 1, 0, 1)         # It stays It, the other becomes It: Infection
        self.assertEqual(tag["mode"]["name"], "Infection")
        self.assertFalse(tag["guessed"])
        self.assertEqual(tag["buckets"], {a: 1, b: 1})

    def test_guessed_king_stays_king_when_the_crown_changes_hands(self):
        w, (a, b) = world()
        w.game_options = options("FR_KING")
        w.start_event(a)
        tag = w.event["tag"]
        w.event["racing"] = True
        tag["assigned"] = True
        tag["buckets"] = {a: S.TAG_BUCKET_IT, b: S.TAG_BUCKET_NOT_IT}
        w.tag_send = lambda why: None
        w.tag_collision(a, ("own", 0, 0), ("relay", 1, 2), 1, 0, 0, 1)         # It -> 0, other -> It: King
        self.assertEqual(tag["mode"]["name"], "King")
        self.assertEqual(tag["buckets"], {a: 0, b: 1})

    def test_road_trip_world_does_not_use_the_guess(self):
        w, (a, b) = world(roadtrip=True)
        w.game_options = options("FR_KING")
        w.start_event(a)
        self.assertNotIn("tag", w.event)               # Road Trip decides from its own event table, as before


class OwnershipGrant(unittest.TestCase):
    def decode(self, write):
        bw = S.BitWriter()
        write(bw)
        r = S.BitReader(bw.tobytes())
        out = {"type": r.bits(7), "router": r.bits(4), "a": r.bits(1), "b": r.bits(1)}
        out["id"] = r.bits(S.ROUTER_BITS[out["router"]][0])
        out["seq"] = r.bits(8)
        out["size"] = r.bits(14)
        out["payload"] = r.bits(out["size"]) if out["size"] else 0
        return out, bw.pos

    def test_default_grant_is_unchanged(self):
        d, pos = self.decode(S.grant_ownership(6, 0, S.ROUTER_BITS[6][0]))
        self.assertEqual((d["type"], d["router"], d["a"], d["b"], d["seq"], d["size"]), (S.MSG_OBJECT_GRANT_OWNERSHIP, 6, 1, 0, 0, 0))
        self.assertEqual(pos, 7 + 4 + 1 + 1 + S.ROUTER_BITS[6][0] + 8 + 14)       # no state: the same length as before

    def test_grant_can_carry_state(self):
        size, payload = options("FR_KING")
        d, pos = self.decode(S.grant_ownership(6, 0, S.ROUTER_BITS[6][0], 3, size, payload))
        self.assertEqual((d["seq"], d["size"], d["payload"]), (3, size, payload))

    def test_grant_advances_sequence_without_overwriting_pending_ui_choices(self):
        w, (a, b) = world()
        w.game_options = options("FR_KING")
        b.gopts_seq = 1                                  # send_game_options already numbered one update for it
        d, _ = self.decode(w.ownership_grant(b, S.ROUTER_LOBBY, S.TYPE_GAME_OPTIONS, 0, S.ROUTER_BITS[S.ROUTER_LOBBY][0]))
        self.assertEqual((d["seq"], d["size"]), (1, 0))
        self.assertEqual(b.gopts_seq, 2)

    def test_everybody_else_is_answered_as_before(self):
        w, (a, b) = world()
        bits = S.ROUTER_BITS[S.ROUTER_LOBBY][0]
        w.game_options = options("FR_KING")
        d, _ = self.decode(w.ownership_grant(a, S.ROUTER_LOBBY, S.TYPE_GAME_OPTIONS, 0, bits))   # never forwarded anything (gopts_seq 0)
        self.assertEqual((d["seq"], d["size"]), (0, 0))
        b.gopts_seq = 1
        w.game_options = None                                                                    # nothing stored
        d, _ = self.decode(w.ownership_grant(b, S.ROUTER_LOBBY, S.TYPE_GAME_OPTIONS, 0, bits))
        self.assertEqual((d["seq"], d["size"]), (0, 0))
        w.game_options = options("FR_KING")
        d, _ = self.decode(w.ownership_grant(b, 3, 1, 0, S.ROUTER_BITS[3][0]))                   # another router
        self.assertEqual((d["seq"], d["size"]), (0, 0))


if __name__ == "__main__":
    unittest.main()
