"""Progress counters use current save snapshots, independently of the XP ledger."""
import os
import sys
import unittest
from unittest.mock import patch
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_ops import FakeDb
from test_db import sub
import scoreboard


class ProgressBoards(unittest.TestCase):
    def setUp(self):
        self.db = FakeDb()
        self.addCleanup(self.db.c.close)
        self.store = scoreboard.ScoreStore(self.db, ['No invented competitors'])

    def test_latest_save_counts_replace_old_higher_totals_and_update(self):
        for st, old, current in ((9, 58, 29), (10, 236, 200)):
            for value in (old, current):
                self.store.submit(1, 'Driver', st, sub(value, track=65535))
            self.store.submit(2, 'Rival', st, sub(current - 1, track=65535))
            self.assertEqual(self.store.best_of(1, 4, st, 65535)['score'], current)
            self.store.submit(1, 'Driver', st, sub(current + 1, track=65535))
            self.assertEqual(self.store.top(4, st)[0]['score'], current + 1)
            self.assertEqual(self.store.rival_for(2, 4, st, 65535)['score'], current + 1)

    def test_xp_cannot_become_bonus_boards_and_does_not_create_rows(self):
        self.db.execute('INSERT INTO player_xp VALUES (?, ?, ?)', (1, 173834., 0))
        self.assertIsNone(self.store.best_of(1, 4, 9, 65535))
        self.store.submit(1, 'Driver', 9, sub(29, track=65535))
        self.assertEqual(self.store.best_of(1, 4, 9, 65535)['score'], 29)
        self.assertEqual(self.store.xp_of(1), 173834.)
        self.assertEqual(self.store.xp_top()[0]['score'], 173834.)

    def test_impossible_counts_rejected_and_historical_rows_hidden(self):
        for st, maximum in ((9, 150), (10, 316)):
            for invalid in (maximum + 1, 173834, 0.5):
                self.assertIsNone(self.store.submit(1, 'Driver', st, sub(invalid, track=65535)))
            row = self.store.submit(1, 'Driver', st, sub(maximum, track=65535))
            self.db.execute('UPDATE score_submissions SET score=173834 WHERE id=?', (row['id'],))
            self.assertEqual(self.store.top(4, st), [])
            with patch.object(scoreboard, 'FAKE_ROWS', 100):
                rows, mine, real, fake = self.store.rows(1, 4, st, 65535, 1, 100)
            self.assertEqual((rows, mine, real, fake), ([], None, 0, 0))

if __name__ == '__main__':
    unittest.main()
