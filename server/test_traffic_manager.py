"""traffic_manager.py against the game's own TrafficManager: each case names the function it follows (build 1.0.0.64)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import traffic  # noqa: E402
import traffic_manager as T  # noqa: E402

MODEL = 100


class Rec(T.Out):
    def __init__(self):
        self.log = []

    def __getattribute__(self, name):
        if name in ("add", "remove", "granted", "canceled", "withdrawn", "take", "drop", "denied"):
            return lambda key, *a: self.log.append((name, key, a[0].vid if a and hasattr(a[0], "vid") else (a[0] if a else None)))
        return object.__getattribute__(self, name)


def manager(*names, pos=(0.0, 0.0, 0.0)):
    out = Rec()
    m = T.TrafficManager(out)
    for n in names:
        m.join(n)
        m.set_position(n, pos)
        m.set_universe(n, 0)
        m.model_loaded(n, MODEL)
    return m, out


def spawn(m, key, vid=None, model=MODEL, pos=(1.0, 0.0, 0.0), now=0.0):
    m.request(key, 1)
    before = set(m.reserved)
    m._grant(m.players[key].universe)
    got = (set(m.reserved) - before).pop()
    m.spawned(key, got, model, None, now)
    m.car_position(key, got, pos)
    m._grant(m.players[key].universe)
    return got


class Holding(unittest.TestCase):
    def test_a_car_reaches_players_inside_their_radius_only(self):
        """UpdatePerFrame 0x1957e00"""
        m, out = manager("a", "b", "c")
        m.set_position("c", (1000.0, 0.0, 0.0))
        vid = spawn(m, "a")
        m.tick(10000.0)
        self.assertEqual(m.cars[vid].holders, {"a", "b"})
        self.assertIn(("add", "b", vid), out.log)
        self.assertNotIn(("add", "c", vid), out.log)

    def test_a_car_with_no_position_goes_nowhere(self):
        m, out = manager("a", "b")
        m.request("a", 1)
        m._grant(0)
        vid = next(iter(m.reserved))
        m.spawned("a", vid, MODEL, None, 0.0)
        m.tick(10000.0)
        self.assertEqual(m.cars[vid].holders, {"a"})

    def test_a_car_goes_only_to_players_with_its_model_loaded(self):
        """helper_1955a90"""
        m, out = manager("a", "b")
        m.model_unloaded("b", MODEL)
        vid = spawn(m, "a")
        m.tick(10000.0)
        self.assertEqual(m.cars[vid].holders, {"a"})

    def test_a_car_being_removed_is_not_added_back_until_the_client_confirms(self):
        """helper_1955a90, OnTrafficRemoved 0x1957680"""
        m, out = manager("a", "b")
        vid = spawn(m, "a")
        m.tick(10000.0)
        m.despawn("b", vid, 10000.0)
        m.tick(10200.0)
        self.assertNotIn("b", m.cars[vid].holders)
        m.removed("b", vid)
        m.tick(10400.0)
        self.assertIn("b", m.cars[vid].holders)

    def test_despawn_takes_the_car_from_that_player_only(self):
        """OnRequestDespawn 0x1956390, RemoveVehicleFromPlayer 0x1957130: ours used to delete it for everyone"""
        m, out = manager("a", "b")
        vid = spawn(m, "a")
        m.tick(10000.0)
        m.despawn("b", vid, 10000.0)
        self.assertIn(vid, m.cars)
        self.assertIn(("remove", "b", vid), out.log)
        self.assertNotIn(("remove", "a", vid), out.log)

    def test_the_owner_culling_a_car_passes_it_to_a_holder(self):
        """0x1956ee0, 0x1956430"""
        m, out = manager("a", "b")
        vid = spawn(m, "a")
        m.tick(100.0)
        m.can_take("b", vid)
        m.despawn("a", vid, 200.0)
        self.assertEqual(m.cars[vid].owner, "b")
        self.assertIn(("take", "b", vid), out.log)

    def test_the_last_holder_leaving_ends_the_car(self):
        m, out = manager("a")
        vid = spawn(m, "a")
        m.despawn("a", vid, 0.0)
        self.assertNotIn(vid, m.cars)

    def test_a_full_player_trades_its_farthest_car_for_a_closer_one(self):
        """0x1957e00, RebalanceByDistance 0x1956720"""
        m, out = manager("a", "b")
        m.set_position("b", (5000.0, 0.0, 0.0))
        m.requests.clear()
        for i in range(12):
            vid = i
            m.reserved[vid] = "b"
            m.players["b"].granted.add(vid)
            m.spawned("b", vid, MODEL, None, 0.0)
            m.car_position("b", vid, (5000.0 + 10 * i, 0.0, 0.0))
        p = m.players["b"]
        self.assertLess(p.capacity(), 1)
        m.set_position("a", (5000.0, 0.0, 0.0))
        m.reserved[50] = "a"
        m.players["a"].granted.add(50)
        m.spawned("a", 50, MODEL, None, 0.0)
        m.car_position("a", 50, (5000.0, 1.0, 0.0))
        m.tick(100.0)
        self.assertIn(50, p.held)
        self.assertNotIn(11, p.held)
        self.assertIn(("remove", "b", 11), out.log)

    def test_a_full_player_keeps_what_it_has_when_the_new_car_is_not_closer(self):
        m, out = manager("a", "b")
        for i in range(12):
            m.reserved[i] = "b"
            m.players["b"].granted.add(i)
            m.spawned("b", i, MODEL, None, 0.0)
            m.car_position("b", i, (1.0, 0.0, 0.0))
        m.reserved[50] = "a"
        m.players["a"].granted.add(50)
        m.spawned("a", 50, MODEL, None, 0.0)
        m.car_position("a", 50, (100.0, 0.0, 0.0))
        m.tick(100.0)
        self.assertNotIn(50, m.players["b"].held)

    def test_an_evicted_car_with_no_holder_left_is_not_handed_to_the_next_player(self):
        m, out = manager("a", "b")
        for i in range(12):
            m.reserved[i] = "a"
            m.players["a"].granted.add(i)
            m.spawned("a", i, MODEL, None, 0.0)
            m.car_position("a", i, (float(100 + 10 * i), 0.0, 0.0))
        m.reserved[50] = "b"
        m.players["b"].granted.add(50)
        m.spawned("b", 50, MODEL, None, 0.0)
        m.car_position("b", 50, (1.0, 0.0, 0.0))
        m.tick(100.0)                                       # the first player evicts its farthest for 50, ending it before the second is walked
        for p in m.players.values():
            self.assertTrue(all(v in m.cars for v in p.held))

    def test_the_last_car_of_a_model_is_not_evicted(self):
        """0x1956720: model count must be above 1"""
        m, out = manager("a")
        m.reserved[1] = "a"
        m.players["a"].granted.add(1)
        m.spawned("a", 1, MODEL, None, 0.0)
        m.car_position("a", 1, (10.0, 0.0, 0.0))
        self.assertEqual(m._farthest(m.players["a"]), (None, -1.0))

    def test_capacity_is_ten_plus_models_in_use_minus_cars(self):
        """helper_1956690"""
        m, out = manager("a")
        self.assertEqual(m.players["a"].capacity(), 10)
        spawn(m, "a")
        self.assertEqual(m.players["a"].capacity(), 10)
        spawn(m, "a", pos=(2.0, 0.0, 0.0))
        self.assertEqual(m.players["a"].capacity(), 9)

    def test_more_than_eighteen_spawned_drops_the_farthest(self):
        """AssignVehicleToPlayer 0x1955080"""
        m, out = manager("a")
        for i in range(19):
            m.reserved[i] = "a"
            m.players["a"].granted.add(i)
            m.spawned("a", i, MODEL, None, 0.0)
            m.car_position("a", i, (float(i + 1), 0.0, 0.0))
        self.assertEqual(len(m.players["a"].held), 18)
        self.assertNotIn(17, m.players["a"].held)           # the 19th has no position yet, so the farthest known one went


class Spawning(unittest.TestCase):
    def test_ids_are_the_lowest_free_ones_below_216(self):
        """helper_19580c0"""
        m, out = manager("a")
        m.request("a", 3)
        m._grant(0)
        self.assertEqual(sorted(m.reserved), [0, 1, 2])
        m.cars = {i: None for i in range(216)}
        m.reserved.clear()
        m.players["a"].granted.clear()
        m.requests[0] = [["a", 1]]
        m._grant(0)
        self.assertEqual(m.reserved, {})

    def test_a_request_waits_while_grants_are_unanswered(self):
        """OnRequestTraffic 0x19578a0"""
        m, out = manager("a")
        m.request("a", 1)
        m._grant(0)
        m.request("a", 1)
        self.assertEqual(m.requests[0], [])

    def test_two_players_close_together_do_not_spawn_at_once(self):
        """helper_1955b30: a grant in progress blocks anyone within the sum of the radii"""
        m, out = manager("a", "b")
        m.request("a", 1)
        m.request("b", 1)
        m._grant(0)
        self.assertEqual(len(m.reserved), 1)
        self.assertEqual(list(m.reserved.values()), ["a"])

    def test_two_players_far_apart_spawn_at_once(self):
        m, out = manager("a", "b")
        m.set_position("b", (1000.0, 0.0, 0.0))
        m.request("a", 1)
        m.request("b", 1)
        m._grant(0)
        self.assertEqual(sorted(m.reserved.values()), ["a", "b"])

    def test_a_car_without_a_position_blocks_spawns_nearby(self):
        """queue +0x38, helper_19580c0's first loop"""
        m, out = manager("a", "b")
        m.request("a", 1)
        m._grant(0)
        vid = next(iter(m.reserved))
        m.spawned("a", vid, MODEL, None, 0.0)
        m.request("b", 1)
        m._grant(0)
        self.assertFalse(m.players["b"].granted)
        m.car_position("a", vid, (1.0, 0.0, 0.0))
        m._grant(0)
        self.assertTrue(m.players["b"].granted)

    def test_a_client_with_unready_cars_gets_no_spawns(self):
        """helper_1955b30: every held car must have CanTakeOwnership"""
        m, out = manager("a", "b")
        vid = spawn(m, "a")
        m.tick(100.0)
        m.request("b", 1)
        m._grant(0)
        self.assertFalse(m.players["b"].granted)
        m.can_take("b", vid)
        m._grant(0)
        self.assertTrue(m.players["b"].granted)

    def test_withdraw_frees_the_ids_and_a_late_spawn_is_ignored(self):
        """OnWithdrawTrafficRequest 0x1958470, helper_19579a0's canceled list"""
        m, out = manager("a")
        m.request("a", 1)
        m._grant(0)
        vid = next(iter(m.reserved))
        m.withdraw("a")
        self.assertEqual(m.reserved, {})
        self.assertIn(("withdrawn", "a", None), out.log)
        self.assertFalse(m.spawned("a", vid, MODEL, None, 0.0))
        self.assertNotIn(vid, m.cars)

    def test_an_ungranted_id_is_refused(self):
        m, out = manager("a")
        self.assertFalse(m.spawned("a", 7, MODEL, None, 0.0))


class Ownership(unittest.TestCase):
    def two_holders(self):
        m, out = manager("a", "b")
        vid = spawn(m, "a", pos=(100.0, 0.0, 0.0))
        m.tick(0.0)
        m.can_take("b", vid)
        return m, out, vid

    def test_a_car_moves_to_the_closer_holder(self):
        """Rebalance 0x1955210"""
        m, out, vid = self.two_holders()
        m.set_position("a", (240.0, 0.0, 0.0))
        m.set_position("b", (105.0, 0.0, 0.0))
        m.tick(20000.0)
        self.assertEqual(m.cars[vid].owner, "b")
        self.assertIn(("drop", "a", vid), out.log)
        self.assertIn(("take", "b", vid), out.log)

    def test_an_exact_tie_between_unorderable_players_does_not_stop_the_tick(self):
        """dev 2026-11-01: the keys are Peer objects; a tie on score and car made the sort compare them"""
        a, b = object(), object()
        m, out = manager(a, b)
        vid = spawn(m, a, pos=(1.0, 0.0, 0.0))
        m.tick(0.0)
        m.can_take(b, vid)
        m.set_position(a, (111.0, 0.0, 0.0))      # 110 - 10 for the owner
        m.set_position(b, (101.0, 0.0, 0.0))      # 100
        m.request(a, 1)
        m.tick(20000.0)
        self.assertIn(m.cars[vid].owner, (a, b))
        self.assertTrue(m.players[a].granted)    # _grant ran after _rebalance

    def test_the_owner_keeps_a_car_on_a_near_tie(self):
        """-10 for the current owner"""
        m, out, vid = self.two_holders()
        m.set_position("a", (105.0, 0.0, 0.0))
        m.set_position("b", (97.0, 0.0, 0.0))
        m.tick(20000.0)
        self.assertEqual(m.cars[vid].owner, "a")

    def test_a_recent_change_protects_the_owner_more(self):
        """up to -100 within 10 s of the last change"""
        m, out, vid = self.two_holders()
        m.set_position("a", (150.0, 0.0, 0.0))
        m.set_position("b", (100.0, 0.0, 0.0))
        m.tick(3000.0)                            # age 3 s: bonus 10 + 70
        self.assertEqual(m.cars[vid].owner, "a")
        m.tick(10500.0)                           # past the window: 50 m closer wins
        self.assertEqual(m.cars[vid].owner, "b")

    def test_no_change_within_two_seconds(self):
        """IsVehicleOwnershipStale 0x1955e10"""
        m, out, vid = self.two_holders()
        m.set_position("a", (249.0, 0.0, 0.0))
        m.set_position("b", (100.0, 0.0, 0.0))
        m.tick(1500.0)
        self.assertEqual(m.cars[vid].owner, "a")

    def test_a_player_with_unready_cars_takes_nothing(self):
        m, out, vid = self.two_holders()
        m.players["b"].can_take.clear()
        m.set_position("a", (249.0, 0.0, 0.0))
        m.set_position("b", (100.0, 0.0, 0.0))
        m.tick(20000.0)
        self.assertEqual(m.cars[vid].owner, "a")

    def test_at_most_two_changes_per_player_per_round(self):
        """budget 10, cost 5: the old owner's lower-ranked entries then keep the other two for this pass, and the next pass moves them"""
        m, out = manager("a", "b")
        ids = [spawn(m, "a", pos=(100.0 + i, 0.0, 0.0)) for i in range(4)]
        m.tick(0.0)
        for vid in ids:
            m.can_take("b", vid)
        m.set_position("a", (249.0, 0.0, 0.0))
        m.set_position("b", (100.0, 0.0, 0.0))
        m.tick(20000.0)
        self.assertEqual(sorted(m.cars[v].owner for v in ids), ["a", "a", "b", "b"])
        m.tick(20200.0)
        self.assertEqual({m.cars[v].owner for v in ids}, {"b"})

    def test_a_collision_request_moves_the_car_after_the_gap(self):
        """OnRequestOwnership 0x1956a20"""
        m, out, vid = self.two_holders()
        self.assertFalse(m.request_ownership("b", vid, 1000.0))
        self.assertIn(("denied", "b", vid), out.log)
        self.assertTrue(m.request_ownership("b", vid, 3000.0))
        self.assertEqual(m.cars[vid].owner, "b")

    def test_a_car_not_held_cannot_be_requested(self):
        m, out = manager("a", "b")
        m.set_position("b", (5000.0, 0.0, 0.0))
        vid = spawn(m, "a")
        self.assertFalse(m.request_ownership("b", vid, 9000.0))


class Leaving(unittest.TestCase):
    def test_a_leaver_cars_pass_to_holders_and_the_rest_end(self):
        """ReleasePlayerState 0x1956db0"""
        m, out = manager("a", "b")
        shared = spawn(m, "a")
        alone = spawn(m, "a", pos=(1.0, 0.0, 0.0))
        m.players["b"].models.clear()
        m.players["b"].models[MODEL] = 0
        m._hold(m.players["b"], m.cars[shared])
        m.can_take("b", shared)
        m.leave("a")
        self.assertEqual(m.cars[shared].owner, "b")
        self.assertNotIn(alone, m.cars)
        self.assertNotIn("a", m.players)

    def test_changing_universe_releases_everything_and_says_so(self):
        """SetPlayerUniverse 0x1954e60"""
        m, out = manager("a")
        vid = spawn(m, "a")
        m.set_universe("a", 1)
        self.assertIn(("remove", "a", vid), out.log)
        self.assertNotIn(vid, m.cars)

    def test_universes_do_not_share_cars(self):
        m, out = manager("a", "b")
        m.set_universe("b", 1)
        vid = spawn(m, "a")
        m.tick(100.0)
        self.assertEqual(m.cars[vid].holders, {"a"})


class Wire(unittest.TestCase):
    class Peer:
        def __init__(self, port, pos=(0.0, 0.0, 0.0)):
            self.addr, self.pos = ("10.0.0.1", port), pos

    class Reader:
        def __init__(self, values):
            self.values = list(values)

        def bits(self, _):
            return self.values.pop(0)

    def world(self):
        sent = []
        peers = [self.Peer(1), self.Peer(2), self.Peer(3, (9000.0, 0.0, 0.0))]
        w = traffic.TrafficWorld(lambda p, ms, fast=False: sent.extend((p, m) for m in ms), lambda: peers, lambda t: None, models=[MODEL])
        for p in peers:
            w.join(p)
            w.manager.set_universe(p, 0)
            w.on_message(p, traffic.S_CAR_MODEL_LOADED, 0xFF, MODEL, 0, 0, 0, 0)
            w.manager.set_position(p, p.pos)
        return w, peers, sent

    def test_updates_reach_holders_only_and_stay_aligned(self):
        w, (a, b, c), sent = self.world()
        w.on_message(a, traffic.S_REQUEST_TRAFFIC, 1, 250, 0, 0, 0, 0)
        w.manager._grant(0)
        vid = next(iter(w.manager.reserved))
        w.on_message(a, traffic.S_TRAFFIC_SPAWNED, vid, MODEL, 0, 0, 0, 0)
        self.assertEqual(w.manager.cars[vid].owner, a)
        one = traffic.struct.unpack("<I", traffic.struct.pack("<f", 5.0))[0]
        fields = [vid] + [0] * 3 + [one, 0, 0]
        w.on_update(a, traffic.MSG_SERVER_ROUTE_PROGRESS, self.Reader(fields))
        self.assertEqual(w.manager.cars[vid].pos, (5.0, 0.0, 0.0))
        w.manager.tick(20000.0)
        self.assertEqual(w.manager.cars[vid].holders, {a, b})
        sent.clear()
        w.on_update(a, traffic.MSG_SERVER_ROUTE_PROGRESS, self.Reader(fields))
        self.assertEqual([p for p, _ in sent], [b])

    def test_updates_from_anyone_but_the_owner_are_dropped_after_reading(self):
        w, (a, b, c), sent = self.world()
        r = self.Reader([0] * 7)
        w.on_update(b, traffic.MSG_SERVER_ROUTE_PROGRESS, r)
        self.assertEqual(r.values, [])
        self.assertEqual(sent[-1][0], c)      # nothing new since the join

    def test_the_radius_comes_from_the_kinds_that_carry_it(self):
        w, (a, b, c), sent = self.world()
        w.on_message(a, traffic.S_REQUEST_TRAFFIC, 1, 180, 0, 0, 0, 0)
        self.assertEqual(w.manager.players[a].radius, 180)
        w.on_message(a, traffic.S_CAR_MODEL_LOADED, 0xFF, 77, 0, 0, 0, 0)
        self.assertEqual(w.manager.players[a].radius, 180)

    def test_initial_join_complete_waits_for_the_models_and_follows_the_cars(self):
        """SyncTrafficModule 0x729ca0 loads models while +0xad waits, and leaves "Syncing traffic" once InitialJoinComplete came and its
        pending AddTraffic cars spawned: so the cars around the joiner go first"""
        import time
        w, (a, b, c), sent = self.world()
        told = []
        push = w.push
        w.push = lambda peer, kind, **f: (told.append((peer, kind)), push(peer, kind, **f))
        w.on_message(a, traffic.S_REQUEST_TRAFFIC, 1, 250, 0, 0, 0, 0)
        w.manager._grant(0)
        vid = next(iter(w.manager.reserved))
        w.on_message(a, traffic.S_TRAFFIC_SPAWNED, vid, MODEL, 0, 0, 0, 0)
        one = traffic.struct.unpack("<I", traffic.struct.pack("<f", 5.0))[0]
        w.on_update(a, traffic.MSG_SERVER_ROUTE_PROGRESS, self.Reader([vid] + [0] * 3 + [one, 0, 0]))
        d = self.Peer(4)
        w.join(d)
        now = time.monotonic()
        w.tick(now)
        self.assertNotIn((d, traffic.C_INITIAL_JOIN_COMPLETE), told)          # no models yet
        w.on_message(d, traffic.S_CAR_MODEL_LOADED, 0xFF, MODEL, 0, 0, 0, 0)
        w.tick(now + 1.0)
        mine = [k for p, k in told if p is d]
        self.assertIn(traffic.C_ADD_TRAFFIC, mine)
        self.assertLess(mine.index(traffic.C_ADD_TRAFFIC), mine.index(traffic.C_INITIAL_JOIN_COMPLETE))

    def test_a_new_holder_gets_the_states_in_the_order_the_owner_sent_them(self):
        """a car that stopped (Stationary) and drives again (FullRoute) must not be handed over parked"""
        d = traffic.CarData((1, MODEL, 0, 0, 0))
        w, _, _ = self.world()
        for mtype, fields in ((traffic.MSG_SERVER_FULL_SYNC, "sync"), (traffic.MSG_SERVER_STATIONARY, "stop"), (traffic.MSG_SERVER_FULL_ROUTE, "route")):
            w.arrivals += 1
            d.order[mtype] = w.arrivals
            setattr(d, {traffic.MSG_SERVER_FULL_SYNC: "full_sync", traffic.MSG_SERVER_STATIONARY: "stationary",
                        traffic.MSG_SERVER_FULL_ROUTE: "full_route"}[mtype], fields)
        self.assertEqual([f for _, f in d.latest()], ["sync", "route"])
        d.order[traffic.MSG_SERVER_STATIONARY] = w.arrivals + 1                 # stopped again: that is the newest state
        self.assertEqual([f for _, f in d.latest()], ["sync", "route", "stop"])

    def test_initial_join_complete_is_not_held_forever(self):
        w, (a, b, c), sent = self.world()
        told = []
        push = w.push
        w.push = lambda peer, kind, **f: (told.append((peer, kind)), push(peer, kind, **f))
        d = self.Peer(4)
        w.join(d)
        w.tick(w.state[d].join_until + 0.1)                                    # a client on local traffic never reports models
        self.assertIn((d, traffic.C_INITIAL_JOIN_COMPLETE), told)

    def test_a_flood_is_ignored(self):
        w, (a, b, c), sent = self.world()
        for _ in range(traffic.MAX_MESSAGES_PER_S + 50):
            w.on_message(a, traffic.S_WITHDRAW_REQUEST, 0, 0, 0, 0, 0, 0)
        self.assertLessEqual(w.counters["WithdrawRequestAccepted"], traffic.MAX_MESSAGES_PER_S)


if __name__ == "__main__":
    unittest.main()
