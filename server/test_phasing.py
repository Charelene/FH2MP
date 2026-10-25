"""Race/free-roam isolation at the wire boundary, including stationary leavers."""
import os
import sys
import unittest

os.environ.setdefault("FH2MP_CONTROL_PORT", "0")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S


class FakeSock:
    def __getattr__(self, name):
        return lambda *a, **k: 0


class Phasing(unittest.TestCase):
    STATE_MASK = (1 << (S.CAR_PREDICTION_BODY_BITS - 32)) - 1
    def setUp(self):
        self.server = S.Server(0, None, sock=FakeSock(), log_file=None)
        self.server.log = lambda *a: None
        self.peers = []
        for i in range(4):
            p = S.Peer(("10.0.0.1", 5000 + i))
            p.connected = p.entered = True
            p.owner_id = p.private_universe = i + 1
            self.server.peers[p.addr] = p
            self.peers.append(p)
        self.a, self.b, self.c, self.d = self.peers
        self.server.event = {"people": [self.a, self.b], "finished": {}}

    def prediction(self, peer, universe=24):
        # Fill every other bit to catch corruption of controls, wheels or timestamp.
        body = ((1 << S.CAR_PREDICTION_BODY_BITS) - 1) & ~S.CAR_UNIVERSE_MASK
        body |= universe << S.CAR_UNIVERSE_OFFSET
        peer.car_prediction = (7, body, S.CAR_PREDICTION_BODY_BITS)
        self.server.relay_prediction(peer)
        return body

    def last_body(self, recipient, sender=None):
        messages = recipient.outbox
        if sender is not None:
            # A refresh contains several owners. Select the leaver's actual update.
            messages = [m for m in messages if (m[0] >> 16) & 255 == sender.owner_id]
        value, size = messages[-1]
        r = S.BitReader(value.to_bytes((size + 7) // 8, "little"))
        self.assertEqual(r.bits(7), S.MSG_CAR_PREDICTION_FULL)
        self.assertEqual((r.bits(1), r.bits(1), r.bits(7)), (0, 1, 7))
        r.bits(8)
        return r.bits(S.CAR_PREDICTION_BODY_BITS)

    def test_racers_and_outsiders_are_phased_in_both_directions(self):
        body = self.prediction(self.a)
        self.assertEqual(self.last_body(self.b), body)
        for q in (self.c, self.d):
            got = self.last_body(q)
            self.assertEqual((got >> S.CAR_UNIVERSE_OFFSET) & 31, self.a.private_universe)
            self.assertEqual(got & ~S.CAR_UNIVERSE_MASK, body & ~S.CAR_UNIVERSE_MASK)
        body = self.prediction(self.c)
        self.assertEqual(self.last_body(self.d), body)
        self.assertEqual((self.last_body(self.a) >> S.CAR_UNIVERSE_OFFSET) & 31, self.c.private_universe)

    def test_stationary_leaver_is_rephased_and_rejoins_free_roam(self):
        self.prediction(self.b)
        self.prediction(self.c)
        self.server.set_console_bool = lambda *a: None
        self.server.participant_left(self.b)
        self.assertEqual((self.last_body(self.a, self.b) >> S.CAR_UNIVERSE_OFFSET) & 31, self.b.private_universe)
        # The refresh sends c last to b: it is visible in free roam again.
        self.assertEqual(self.last_body(self.b) & self.STATE_MASK, self.c.car_prediction[1] & self.STATE_MASK)
        self.assertEqual(self.last_body(self.d, self.b) & self.STATE_MASK, self.b.car_prediction[1] & self.STATE_MASK)
        # The original timestamp was 0xffffffff; a refresh wraps to 0, which is newer
        # under the game's signed 32-bit delta comparison.
        self.assertEqual(self.last_body(self.a, self.b) >> (S.CAR_PREDICTION_BODY_BITS - 32), 0)

    def test_finished_leaver_is_isolated_even_when_result_flag_is_retained(self):
        self.server.event["finished"][self.b] = 1
        self.b.console_vars[S.CONSOLE_VAR_PARTICIPATING_IN_RACE] = 1
        self.server.participant_left(self.b)
        self.assertFalse(self.server.same_event(self.a, self.b))
        self.assertTrue(self.server.same_event(self.c, self.b))

    def test_start_and_end_refresh_existing_cars(self):
        self.server.event = None
        self.prediction(self.c)
        self.server.set_console_bool = lambda *a: None
        self.server.set_session_host_bool = lambda *a: None
        self.server.start_event(self.a)
        self.assertEqual((self.last_body(self.a) >> S.CAR_UNIVERSE_OFFSET) & 31, self.c.private_universe)
        self.server.end_event("test")
        self.assertEqual(self.last_body(self.a) & self.STATE_MASK, self.c.car_prediction[1] & self.STATE_MASK)

    def test_collisions_stay_within_the_roster_boundary(self):
        def collision(w):
            w.bits(S.MSG_CAR_COLLISION, 7)
            w.bits(0, 2)
            w.bits(7, 7)
            w.bits(0, 192)
        self.server.handle(S.build_packet([collision]), self.a.addr)
        self.assertTrue(self.b.outbox)
        self.assertFalse(self.c.outbox)
        self.assertFalse(self.d.outbox)
        self.server.handle(S.build_packet([collision]), self.c.addr)
        self.assertTrue(self.d.outbox)
        self.assertFalse(self.c.outbox)

    def test_other_worlds_never_receive_predictions(self):
        self.d.world = ("elsewhere",)
        self.prediction(self.a)
        self.assertFalse(self.d.outbox)

    def test_same_group_keeps_native_private_universes(self):
        body = self.prediction(self.a, universe=self.a.private_universe)
        self.assertEqual(self.last_body(self.b), body)

    def test_prediction_handler_and_following_message_keep_their_boundaries(self):
        body = 24 << S.CAR_UNIVERSE_OFFSET
        def prediction(w):
            w.bits(S.MSG_CAR_PREDICTION_FULL, 7)
            w.bits(0, 2)
            w.bits(7, 7)
            w.bits(body, S.CAR_PREDICTION_BODY_BITS)
        self.server.handle(S.build_packet([prediction, S.ping(17)]), self.a.addr)
        self.assertEqual(self.a.car_prediction, (7, body, S.CAR_PREDICTION_BODY_BITS))
        self.assertEqual(self.last_body(self.b), body)
        self.assertEqual((self.last_body(self.c) >> S.CAR_UNIVERSE_OFFSET) & 31, self.a.private_universe)
        self.assertEqual(self.a.outbox[-1][0] & 127, S.MSG_PONG)

    def test_repeated_refreshes_advance_time_and_do_not_mutate_cached_physics(self):
        body = self.prediction(self.a)
        self.server.refresh_car_phasing()
        self.server.refresh_car_phasing()
        self.assertEqual(self.last_body(self.b) >> (S.CAR_PREDICTION_BODY_BITS - 32), 1)
        self.assertEqual(self.a.car_prediction[1], body)

    def test_reordered_prediction_does_not_replace_the_rephase_cache(self):
        def packet(stamp):
            def prediction(w):
                w.bits(S.MSG_CAR_PREDICTION_FULL, 7)
                w.bits(0, 2)
                w.bits(7, 7)
                w.bits((stamp << (S.CAR_PREDICTION_BODY_BITS - 32)) | (24 << S.CAR_UNIVERSE_OFFSET),
                       S.CAR_PREDICTION_BODY_BITS)
            return S.build_packet([prediction])
        self.server.handle(packet(100), self.a.addr)
        original = self.a.car_prediction
        self.server.handle(packet(99), self.a.addr)
        self.assertEqual(self.a.car_prediction, original)
        self.server.refresh_car_phasing()
        self.assertEqual(self.last_body(self.b) >> (S.CAR_PREDICTION_BODY_BITS - 32), 101)


if __name__ == "__main__":
    unittest.main()
