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


if __name__ == "__main__":
    unittest.main()
