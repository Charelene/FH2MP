"""LobbyPlayer.PlayerStatus (player var 19): CarSelect -> PreRace during an event's setup is the moment a participant left its event car select.
The server logs when each participant did, and how many are done (Server.note_player_status)."""
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fh2mp_server as S
import test_race_flow as T

CAR_SELECT, IN_FREEROAM, PRE_RACE = 1, 2, 3


class CarSelectStatus(unittest.TestCase):
    drain, send, advance, setUp, join = T.RaceFlow.drain, T.RaceFlow.send, T.RaceFlow.advance, T.RaceFlow.setUp, T.RaceFlow.join
    select, grant, options, car, ready, request = (T.RaceFlow.select, T.RaceFlow.grant, T.RaceFlow.options, T.RaceFlow.car, T.RaceFlow.ready,
                                                   T.RaceFlow.request)

    def status(self, client, value):
        update = T.Bits().put(S.PLAYER_VAR_STATUS, 8).put(value, 4)
        self.send(client, T.obj(1, update, client.player_seq))
        client.player_seq = (client.player_seq + 1) % 255

    def lines(self):
        return [line for line in self.logs if "event car select:" in line and " left it " in line]

    def test_each_participant_is_logged_when_it_leaves_the_car_select(self):
        self.select()
        self.status(self.a, CAR_SELECT)
        self.status(self.b, CAR_SELECT)
        self.assertEqual(self.lines(), [])
        self.advance(3)
        self.status(self.a, PRE_RACE)
        self.assertEqual(len(self.lines()), 1)
        self.assertIn("1/2 participant(s) done", self.lines()[0])
        self.assertNotIn("everybody", self.lines()[0])
        self.advance(4)
        self.status(self.b, PRE_RACE)
        self.assertEqual(len(self.lines()), 2)
        self.assertIn("2/2 participant(s) done, everybody is", self.lines()[1])
        first, second = (float(re.search(r"left it ([0-9.]+) s", line).group(1)) for line in self.lines())
        self.assertAlmostEqual(second - first, 4.0, delta=0.6)

    def test_a_repeated_status_is_not_a_second_pick(self):
        self.select()
        self.status(self.a, PRE_RACE)           # first status seen: it is in the wait already (a client that skipped the car select), one pick
        self.status(self.a, CAR_SELECT)
        self.status(self.a, CAR_SELECT)
        self.status(self.a, PRE_RACE)
        self.status(self.a, PRE_RACE)
        self.assertEqual(len(self.lines()), 1)

    def test_nothing_is_logged_outside_an_event_setup(self):
        self.status(self.a, CAR_SELECT)
        self.status(self.a, PRE_RACE)
        self.assertEqual(self.lines(), [])


if __name__ == "__main__":
    unittest.main()
