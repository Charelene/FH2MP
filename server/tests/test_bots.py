"""Bot players (bots.py): a dev-only copy of one real player under an identity of its own. The objects and the physics are the source's; the user id and
the place differ. Nothing here sends to a game client: the world's outboxes are read."""
import math
import os
import struct
import sys
import unittest
from unittest import mock

os.environ.setdefault("FH2MP_CONTROL_PORT", "0")
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import bots  # noqa: E402
import capacity  # noqa: E402
import fh2mp_server as S  # noqa: E402
from test_security import make_hub  # noqa: E402

XUID = 0x9000000300001


def body_at(x, y, z, stamp=1000):
    body = int.from_bytes(struct.pack("<3f", x, y, z), "little")
    body |= (((1 << (S.CAR_PREDICTION_BODY_BITS - 32 - 96)) - 1) & 0x5A5A5A5A5A5A5A5A5A5A5A5A5A5A5A5A) << 96
    return body | (stamp << (S.CAR_PREDICTION_BODY_BITS - 32))


def position(body):
    return struct.unpack("<3f", (body & ((1 << 96) - 1)).to_bytes(12, "little"))


class FakeSock:
    def __getattr__(self, name):
        return lambda *a, **k: 0


def room(real=2):
    w = S.Server(0, None, sock=FakeSock(), log_file=None)
    w.log = lambda *a, **k: None
    peers = []
    for i in range(real):
        p = S.Peer(("10.0.0.%d" % (i + 1), 5000 + i))
        p.connected = p.entered = p.introduced = True
        p.owner_id, p.xuid, p.car_id, p.model_id = i + 1, XUID + i, i + 1, 77
        p.world = ("",)
        p.lobby_type = 2
        # a player object that carries the user id, a car object, and a console
        p.objs[(1, 1)] = [(S.MSG_OBJECT_CREATE, 1, 0, 200, (0xABC << 130) | (p.xuid << 20) | 5)]
        p.objs[(2, 2)] = [(S.MSG_OBJECT_CREATE, 1, 0, 120, 0x1234)]
        p.objs[(3, 3)] = [(S.MSG_OBJECT_CREATE, 1, 0, 90, 0x4321)]
        p.relay_next = {k: len(v) for k, v in p.objs.items()}
        p.car_prediction = (p.car_id, body_at(100.0 + i, 5.0, 200.0), S.CAR_PREDICTION_BODY_BITS)
        w.peers[p.addr] = p
        peers.append(p)
    return w, peers


def bot_env():
    return mock.patch.dict(os.environ, {"FH2MP_BOTS": "1", "FH2MP_ENV": "dev"})


class Helpers(unittest.TestCase):
    def test_a_user_id_is_replaced_wherever_it_sits_and_nothing_else_is(self):
        payload = (0xFFFF << 150) | (XUID << 3) | 5
        got, n = bots.replace_user_id(payload, 220, XUID, 0x9F00000000001)
        self.assertEqual(n, 1)
        self.assertEqual(got, (0xFFFF << 150) | (0x9F00000000001 << 3) | 5)
        same, none = bots.replace_user_id(payload, 220, 0x1111, 0x2222)
        self.assertEqual((same, none), (payload, 0))

    def test_a_position_moves_sideways_and_every_other_bit_stays(self):
        body = body_at(100.0, 5.0, 200.0, stamp=0xABCDEF)
        moved = bots.shifted_body(body, 6.0, -3.0)
        self.assertEqual(position(moved), (106.0, 5.0, 197.0))
        self.assertEqual(moved >> 96, body >> 96)

    def test_a_position_that_is_not_a_number_is_left_alone(self):
        body = int.from_bytes(struct.pack("<3f", math.nan, 1.0, 2.0), "little") | (7 << 200)
        self.assertEqual(bots.shifted_body(body, 6.0, 6.0), body)

    def test_the_ring_places_eight_to_a_ring_and_far_enough_from_the_source_to_never_touch_it(self):
        spots = [bots.ring_offset(k) for k in range(24)]
        self.assertEqual(len(set((round(a, 3), round(b, 3)) for a, b in spots)), 24)
        self.assertTrue(all(math.hypot(a, b) >= bots.RING_START_M - 1e-6 for a, b in spots))
        self.assertAlmostEqual(math.hypot(*spots[0]), 30.0)
        self.assertAlmostEqual(math.hypot(*spots[8]), 42.0)
        self.assertAlmostEqual(math.hypot(*spots[16]), 54.0)
        # two bots are never closer than a car length or two to each other either
        closest = min(math.hypot(a[0] - b[0], a[1] - b[1]) for i, a in enumerate(spots) for b in spots[i + 1:])
        self.assertGreater(closest, 10.0)


class Adding(unittest.TestCase):
    def setUp(self):
        self.w, (self.a, self.b) = room()
        for p in (self.a, self.b):
            p.outbox.clear()

    def test_a_bot_is_a_player_with_an_owner_id_and_an_identity_of_its_own(self):
        added, why = bots.add(self.w, 3, 12)
        self.assertEqual((added, why), (3, ""))
        owners = [b.owner_id for b in self.w.bots]
        self.assertEqual(len(set(owners + [1, 2])), 5)
        self.assertEqual(len({b.xuid for b in self.w.bots} | {self.a.xuid, self.b.xuid}), 5)
        self.assertTrue(all(b.xuid >= bots.BOT_XUID_BASE for b in self.w.bots))
        self.assertTrue(all(b.addr not in self.w.peers for b in self.w.bots))        # no socket: no loop sees them as a connection

    def test_the_objects_are_the_sources_with_the_bots_user_id(self):
        bots.add(self.w, 1, 12)
        bot = self.w.bots[0]
        src = bot.bot_source
        self.assertEqual(set(bot.objs), {(1, 1), (2, 2), (3, 3)})
        mtype, tid, seq, size, payload = bot.objs[(1, 1)][0]
        self.assertEqual(payload, (0xABC << 130) | (bot.xuid << 20) | 5)
        self.assertEqual(src.objs[(1, 1)][0][4], (0xABC << 130) | (src.xuid << 20) | 5)    # the source's own copy is untouched
        self.assertEqual(bot.objs[(2, 2)][0][4], 0x1234)

    def test_real_clients_are_told_about_the_bot_and_it_does_not_count_twice(self):
        made, real = [], S.relay_object_message
        with mock.patch.object(S, "relay_object_message", lambda mtype, router, oid, owner, *a: made.append((router, owner)) or real(mtype, router, oid, owner, *a)):
            bots.add(self.w, 1, 12)
        owner = self.w.bots[0].owner_id
        self.assertEqual(sorted(r for r, o in made if o == owner), [1, 1, 2, 2, 3, 3])      # player, car and console, once for each real client
        self.assertEqual(self.w.owner_ids_taken() & {b.owner_id for b in self.w.bots}, {self.w.bots[0].owner_id})

    def test_a_newcomer_is_shown_the_bots_too(self):
        bots.add(self.w, 2, 12)
        new = S.Peer(("10.0.0.9", 5009))
        new.connected, new.world, new.owner_id = True, ("",), 9
        self.w.peers[new.addr] = new
        made = []
        real = S.relay_object_message
        with mock.patch.object(S, "relay_object_message", lambda mtype, router, oid, owner, *a: made.append(owner) or real(mtype, router, oid, owner, *a)):
            self.w.replay_to(new)
        self.assertEqual({b.owner_id for b in self.w.bots} & set(made), {b.owner_id for b in self.w.bots})

    def test_the_bots_car_follows_the_sources_prediction_a_few_metres_away(self):
        bots.add(self.w, 1, 12)
        bot = self.w.bots[0]
        src = bot.bot_source
        for p in (self.a, self.b):
            p.outbox.clear()
        src.car_prediction = (src.car_id, body_at(300.0, 9.0, 400.0, stamp=2000), S.CAR_PREDICTION_BODY_BITS)
        self.w.relay_prediction(src)
        got = [m for m in (self.b if src is self.a else self.a).outbox if (m[0] >> 16) & 255 == bot.owner_id]
        self.assertTrue(got, "the other real player gets a prediction from the bot")
        value, size = got[-1]
        r = S.BitReader(value.to_bytes((size + 7) // 8, "little"))
        r.bits(7), r.bits(1), r.bits(1), r.bits(7), r.bits(8)
        x, y, z = position(r.bits(S.CAR_PREDICTION_BODY_BITS))
        dx, dz = bot.bot_offset
        self.assertEqual((x, y, z), (300.0 + dx, 9.0, 400.0 + dz))
        self.assertEqual(bot.car_prediction_stamp, 2000)

    def test_the_world_size_is_never_passed_and_a_reason_is_given(self):
        added, why = bots.add(self.w, 30, 12)
        self.assertEqual(len(self.w.bots) + 2, 12)
        self.assertEqual(added, 10)
        self.assertIn("12", why)

    def test_no_source_no_bot(self):
        empty, _peers = room(real=0)
        self.assertEqual(bots.add(empty, 2, 12)[0], 0)
        lonely, (p,) = room(real=1)
        p.car_prediction = None
        self.assertEqual(bots.add(lonely, 2, 12)[0], 0)

    def test_the_hubs_seat_count_and_population_include_the_bots(self):
        hub = make_hub()
        with mock.patch.dict(os.environ, {capacity.ENV: "24"}):
            addrs = []
            for i in range(2):
                xuid = XUID + i
                hub.assign[xuid] = "roam"
                addr = ("10.5.0.%d" % (i + 1), 5000)
                hub.handle(S.build_packet([lambda w, xuid=xuid: (w.bits(S.MSG_OPEN_CONNECTION, 7), w.string(S.VERSION_STRING), w.string(S.GAME_STRING),
                                                                 w.bits(0, 2), w.bits(xuid, 64), w.bits(2, 32), w.bits(0, 32), w.bits(7, 32))]), addr)
                addrs.append(addr)
            world = hub.route[addrs[0]]
            world.log = lambda *a, **k: None
            for a in addrs:
                p = world.peers[a]
                p.connected = p.entered = p.introduced = True
                p.owner_id = addrs.index(a) + 1
                p.car_id, p.model_id = 5, 5
                p.objs[(1, 1)] = [(S.MSG_OBJECT_CREATE, 1, 0, 100, p.xuid)]
                p.objs[(2, 2)] = [(S.MSG_OBJECT_CREATE, 1, 0, 100, 1)]
                p.car_prediction = (5, body_at(1.0, 2.0, 3.0), S.CAR_PREDICTION_BODY_BITS)
            bots.add(world, 5, capacity.for_type(2))
            self.assertEqual(hub.world_load(world), 7)
            pop = hub.populations()["roam"]
            self.assertEqual(len(pop), 7)


class Removing(unittest.TestCase):
    def setUp(self):
        self.w, (self.a, self.b) = room()
        bots.add(self.w, 3, 12)

    def test_clearing_deletes_the_objects_for_everyone_and_rests_the_owner_ids(self):
        owners = {b.owner_id for b in self.w.bots}
        for p in (self.a, self.b):
            p.outbox.clear()
        self.assertEqual(bots.clear(self.w), 3)
        self.assertEqual(self.w.bots, [])
        self.assertEqual(owners & self.w.owner_ids_taken(), owners)                  # quarantined like a leaver's
        self.assertTrue(self.a.outbox and self.b.outbox)

    def test_when_the_last_real_player_goes_the_bots_go(self):
        self.w.peers.clear()
        self.w.bots_next = 0.0
        bots.maintain(self.w)
        self.assertEqual(self.w.bots, [])

    def test_a_bot_whose_source_left_follows_another_player(self):
        gone = self.a
        followers = [b for b in self.w.bots if b.bot_source is gone]
        self.assertTrue(followers)
        del self.w.peers[gone.addr]
        gone.connected = False
        self.w.bots_next = 0.0
        bots.maintain(self.w)
        self.assertEqual(len(self.w.bots), 3)
        self.assertTrue(all(b.bot_source is self.b for b in self.w.bots))


class Switch(unittest.TestCase):
    def test_off_unless_asked_for_and_never_on_prod(self):
        hub = make_hub()
        with mock.patch.dict(os.environ, {"FH2MP_ENV": "dev"}):
            os.environ.pop("FH2MP_BOTS", None)
            self.assertTrue(bots.command(hub, ["bots", "status"]).startswith("off"))
            self.assertFalse(bots.enabled())
        with mock.patch.dict(os.environ, {"FH2MP_BOTS": "1", "FH2MP_ENV": "prod"}):
            self.assertTrue(bots.command(hub, ["bots", "add", "3"]).startswith("refused"))
            self.assertFalse(bots.enabled())

    def test_the_control_command_adds_and_clears(self):
        hub = make_hub()
        w, _peers = room()
        w.hub_key = (2, "roam")
        hub.worlds[w.hub_key] = w
        with bot_env(), mock.patch.dict(os.environ, {capacity.ENV: "24"}):
            self.assertIn("added 4 of 4", bots.command(hub, ["bots", "add", "4"]))
            self.assertEqual(len(w.bots), 4)
            self.assertIn("4 bot(s)", bots.command(hub, ["bots", "status"]))
            self.assertIn("cleared 4", bots.command(hub, ["bots", "clear"]))
            self.assertIn("usage", bots.command(hub, ["bots", "add", "x"]))
            self.assertIn("no Free Roam world", bots.command(hub, ["bots", "add", "1", "nowhere"]))


if __name__ == "__main__":
    unittest.main()
