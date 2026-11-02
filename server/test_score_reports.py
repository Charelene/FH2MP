"""Submission reporting/moderation contracts, on the same SQLite fixtures as the operator API tests."""
import hashlib
import json
import os
import sys
import unittest
from unittest.mock import patch
from types import SimpleNamespace

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_ops import make_lsp, submit, call
import score_reports
import fh2mp_server as game


def identity(n):
    return hashlib.sha256(str(n).encode()).hexdigest()


class Reports(unittest.TestCase):
    def setUp(self):
        self.lsp = make_lsp()
        self.addCleanup(self.lsp.DB.c.close)
        submit(self.lsp.SB, 1, 'Alice', 80)
        self.sid = self.lsp.DB.one('SELECT id FROM score_submissions')[0]

    def report(self, browser, ip=None):
        return score_reports.report(self.lsp, self.sid, {'browser': identity(browser), 'ip': identity(ip if ip is not None else browser)})

    def test_duplicate_by_either_identity_and_exact_threshold(self):
        self.assertTrue(self.report(1)['reported'])
        self.assertFalse(self.report(1, 2)['reported'])
        self.assertFalse(self.report(2, 1)['reported'])
        for i in range(3, 11):
            self.report(i)
        row = self.lsp.SB.top(3, 1, -1, 1000)[0]
        self.assertEqual(row['reports'], 9)
        self.assertFalse(row['suspicious'])
        self.report(11)
        self.assertTrue(self.lsp.SB.top(3, 1, -1, 1000)[0]['suspicious'])

    def test_hide_promotes_next_best_and_preserves_raw_run_and_ghost(self):
        submit(self.lsp.SB, 1, 'Alice', 90)
        submit(self.lsp.SB, 2, 'Bob', 85)
        self.lsp.DB.execute('INSERT INTO score_ghosts VALUES (?, ?, ?)', (self.sid, b'replay', 1))
        score_reports.moderate(self.lsp, self.sid, 'hide')
        score_reports.moderate(self.lsp, self.sid, 'hide')  # idempotent
        rows, mine, real, _ = self.lsp.SB.rows(1, 3, 1, -1, 1, 25, 1000)
        self.assertEqual([(r['xuid'], r['score']) for r in rows], [(2, 85), (1, 90)])
        self.assertEqual(mine['position'], 2)
        self.assertEqual(self.lsp.SB.count(), 3)
        self.assertEqual(self.lsp.SB.ghost(self.sid), b'replay')
        self.assertTrue(score_reports.queue(self.lsp)['entries'][0]['hidden'])
        self.assertEqual(call(self.lsp, 'POST', f'/ops/v1/reports/{self.sid}', {'browser': identity(1), 'ip': identity(1)})[0], 404)

    def test_dismiss_retains_reports_and_does_not_allow_repeat(self):
        for i in range(10):
            self.report(i)
        score_reports.moderate(self.lsp, self.sid, 'dismiss')
        self.assertEqual(score_reports.queue(self.lsp)['entries'], [])
        self.assertFalse(self.lsp.SB.top(3, 1, -1, 1000)[0]['suspicious'])
        self.assertFalse(self.report(1)['reported'])
        self.assertEqual(self.lsp.DB.one('SELECT COUNT(*) FROM score_reports')[0], 10)

    def test_new_submission_does_not_inherit_reports(self):
        self.report(1)
        submit(self.lsp.SB, 1, 'Alice', 79)
        self.assertEqual(self.lsp.SB.top(3, 1, -1, 1000)[0]['reports'], 0)

    def test_invalid_requests_and_unknown_run(self):
        self.assertEqual(call(self.lsp, 'POST', f'/ops/v1/reports/{self.sid}', {})[0], 400)
        self.assertEqual(call(self.lsp, 'POST', '/ops/v1/reports/999/hide', {})[0], 404)
        self.assertEqual(call(self.lsp, 'POST', f'/ops/v1/reports/{self.sid}/delete', {})[0], 400)

    def test_report_rate_limit_is_durable(self):
        for i in range(21):
            submit(self.lsp.SB, 1, 'Alice', 100 + i)
            sid = self.lsp.DB.one('SELECT MAX(id) FROM score_submissions')[0]
            status, _ = call(self.lsp, 'POST', f'/ops/v1/reports/{sid}', {'browser': identity(1), 'ip': identity(1)})
            self.assertEqual(status, 200 if i < 20 else 429)


class Lobbies(unittest.TestCase):
    def test_occupied_worlds_and_live_road_trip_stage(self):
        def world(n, state=0):
            return SimpleNamespace(peers={i: SimpleNamespace(connected=True, xuid=100+i) for i in range(n)}, rt_state=state, rt_idx=2)
        hub = SimpleNamespace(worlds={(1, 'trip'): world(3, game.RT_DESTINATION_VOTE), (2, ''): world(12),
                                     (2, 'second-roam'): world(2), (1, 'second-trip'): world(1, game.RT_CAR_SELECT), (2, 'empty'): world(0), (3, 'meet'): world(2)}, WORLD_CAPACITY=12)
        rows = game.Hub.lobby_status(hub)['lobbies']
        self.assertEqual(len(rows), 4)
        trip = next(r for r in rows if r['kind'] == 'roadtrip')
        self.assertEqual(trip['status'], 'Voting for a destination')
        self.assertEqual(trip['players'], 3)
        self.assertEqual(trip['memberXuids'], ['64', '65', '66'])
        hub.worlds[(1, 'trip')].rt_state = game.RT_IN_EVENT
        self.assertEqual(next(r for r in game.Hub.lobby_status(hub)['lobbies'] if r['kind'] == 'roadtrip')['status'], 'Racing')

    def test_lobby_nicknames_resolved_without_exposing_ids(self):
        lsp = make_lsp()
        self.addCleanup(lsp.DB.c.close)
        submit(lsp.SB, 3, '<Driver>', 80)
        lsp.xbl.hub_lobbies = lambda: {'lobbies': [{'kind': 'freeroam', 'id': 'one', 'players': 2,
                                                   'capacity': 12, 'memberXuids': ['1', '3']}]}
        status, data = call(lsp, 'GET', '/ops/v1/lobbies')
        self.assertEqual(status, 200)
        self.assertEqual(data['lobbies'][0]['members'], ['<Driver>', 'Alice'])
        self.assertNotIn('memberXuids', data['lobbies'][0])

    def test_map_status_lists_cars_of_public_worlds_that_have_a_position(self):
        def peer(i, pos):
            return SimpleNamespace(connected=True, xuid=100 + i, owner_id=i, pos=pos, model_id=7)
        roam = SimpleNamespace(peers={1: peer(1, (10.4, 99.0, -20.6)), 2: peer(2, None)}, rt_state=0, rt_idx=0)
        trip = SimpleNamespace(peers={3: peer(3, (1.0, 2.0, 3.0))}, rt_state=game.RT_DESTINATION_VOTE, rt_idx=0)
        meet = SimpleNamespace(peers={4: peer(4, (5.0, 6.0, 7.0))}, rt_state=0, rt_idx=0)
        hub = SimpleNamespace(worlds={(2, 'one'): roam, (1, 'trip'): trip, (3, 'meet'): meet}, WORLD_CAPACITY=12)
        hub.lobby_status = lambda: game.Hub.lobby_status(hub)
        rows = {r['kind']: r for r in game.Hub.map_status(hub)['lobbies']}
        self.assertEqual(set(rows), {'freeroam', 'roadtrip'})
        self.assertEqual(rows['freeroam']['cars'], [{'id': 1, 'xuid': '65', 'x': 10, 'z': -21, 'car': 7}])
        self.assertEqual(rows['roadtrip']['cars'][0]['z'], 3)

    def test_positions_name_drivers_without_exposing_ids(self):
        lsp = make_lsp()
        self.addCleanup(lsp.DB.c.close)
        submit(lsp.SB, 3, '<Driver>', 80)
        lsp.xbl.hub_positions = lambda: {'lobbies': [{'kind': 'freeroam', 'id': 'one', 'players': 1, 'capacity': 12, 'memberXuids': ['3'],
                                                      'cars': [{'id': 2, 'xuid': '3', 'x': 5, 'z': -6, 'car': 7}]}]}
        status, data = call(lsp, 'GET', '/ops/v1/positions')
        self.assertEqual(status, 200)
        self.assertEqual(data['lobbies'][0]['cars'], [{'id': 2, 'name': '<Driver>', 'x': 5, 'z': -6, 'car': 'Test Car'}])
        self.assertNotIn('memberXuids', data['lobbies'][0])
        self.assertNotIn('xuid', json.dumps(data))
        lsp.xbl.hub_positions = lambda: None
        self.assertEqual(call(lsp, 'GET', '/ops/v1/positions')[0], 503)

    def test_ops_reports_unavailable_when_hub_is_down(self):
        lsp = make_lsp()
        self.addCleanup(lsp.DB.c.close)
        lsp.xbl.hub_lobbies = lambda: None
        self.assertEqual(call(lsp, 'GET', '/ops/v1/lobbies')[0], 503)
        lsp.xbl.hub_lobbies = lambda: {'lobbies': []}
        self.assertEqual(call(lsp, 'GET', '/ops/v1/lobbies'), (200, {'lobbies': []}))

class GhostFlag(unittest.TestCase):
    """A flag verdict from the ghost rules shows on the website like a run with enough reports: the red stop sign, low confidence."""
    def setUp(self):
        self.lsp = make_lsp()
        self.addCleanup(self.lsp.DB.c.close)
        submit(self.lsp.SB, 1, 'Alice', 80)
        self.sid = self.lsp.DB.one('SELECT id FROM score_submissions')[0]

    def verdict(self, verdict, reviewed=0):
        self.lsp.DB.execute("INSERT INTO score_verdicts (submission_id, version, verdict, reasons, measurements, decided_at, reviewed) "
                            "VALUES (?, 1, ?, '[]', '{}', 0, ?)", (self.sid, verdict, reviewed))

    def row(self):
        return self.lsp.SB.top(3, 1, -1, 1000)[0]

    def test_a_flag_verdict_marks_the_run_and_says_why(self):
        self.assertFalse(self.row()['suspicious'])
        self.verdict('flag')
        self.assertTrue(self.row()['suspicious'])
        self.assertEqual(self.row()['flaggedBy'], 'ghost')

    def test_a_reviewed_flag_and_a_passing_verdict_do_not_mark_it(self):
        self.verdict('flag', reviewed=1)
        self.assertFalse(self.row()['suspicious'])
        self.lsp.DB.execute('DELETE FROM score_verdicts')
        self.verdict('pass')
        self.assertFalse(self.row()['suspicious'])

    def test_dismissing_the_run_clears_the_flag(self):
        self.verdict('flag')
        score_reports.moderate(self.lsp, self.sid, 'dismiss')
        self.assertFalse(self.row()['suspicious'])

    def test_reports_and_a_flag_together_are_named(self):
        self.verdict('flag')
        for i in range(10):
            score_reports.report(self.lsp, self.sid, {'browser': identity(i), 'ip': identity(i)})
        self.assertEqual(self.row()['flaggedBy'], 'reports and ghost')


if __name__ == '__main__':
    unittest.main()
