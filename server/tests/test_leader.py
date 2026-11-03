"""PlayerData.IsLeader: the flag behind the "<gamertag> IS SETTING UP AN EVENT" toast of Online Free Roam.

Inputs are the messages the game itself sends (docs/re/netv2.md): RequestOwnership of the lobby's GameOptions when a player starts setting up
an event (States::NetworkGetGameOptionsOwnership), DropLeadership (msg 60, Nerd::Msg::DropLeadership::Send 0x17f7a50: type only) when the
public activation is cancelled, and MsgRequestRace (msg 38). Outputs are read with the independent wire reader of test_race_flow.Receiver,
which records the player-object (router 1) messages: a server update with both key flags set for the owner's own flag, a relay update with
the owner byte for everybody else. The client's reaction (CPlayer::vfunc_12_vt8 0x32a8c0 -> NetworkBecameLeader -> helper 0x7dc790) is not
simulated here: what a player's HUD does is decided by the flags these tests read, and is checked by hand with two real clients.
"""
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import fh2mp_server as S
import test_race_flow as T

IS_LEADER = 5          # PlayerData var 5 (player_setup._WIDTHS: 64, 1, 1, -16, 64, 1, ...), 8-bit ids, 1 bit value


def flags(client):
    """The IsLeader writes a client was given, in order: ('own', flag) for its own player object, ('relay', owner, flag) for another's."""
    out = []
    for kind, (a, b), _oid, owner, body in client.players:
        if kind != 8 or body.size != 8 + 1:
            continue
        copy = T.Bits(body.value, body.size)
        if copy.take(8) != IS_LEADER:
            continue
        flag = copy.take(1)
        out.append(('own', flag) if (a, b) == (1, 1) else ('relay', owner, flag))
    return out


def view(client):
    """Who the client believes is the leader after replaying every write it received: 'own' and/or relay owner ids."""
    state = {}
    for entry in flags(client):
        state[entry[0] if entry[0] == 'own' else entry[1]] = bool(entry[-1])
    return {who for who, flag in state.items() if flag}


class LeaderFlow(unittest.TestCase):
    drain, send, advance = T.RaceFlow.drain, T.RaceFlow.send, T.RaceFlow.advance
    grant, options, car, console, ready, request = (T.RaceFlow.grant, T.RaceFlow.options, T.RaceFlow.car, T.RaceFlow.console,
                                                    T.RaceFlow.ready, T.RaceFlow.request)
    select, go, finish = T.RaceFlow.select, T.RaceFlow.go, T.RaceFlow.finish

    def setUp(self):
        T.RaceFlow.setUp(self)
        self.mark()

    def join(self, number):
        client = T.RaceFlow.join(self, number)
        self.mark()
        return client

    def mark(self):
        """Entering tells every player 'not the leader' (checked in test_every_player_is_told...); the other tests look at what comes after."""
        self.base = {c.addr: len(flags(c)) for c in self.clients.values()}

    def f(self, client):
        return flags(client)[self.base[client.addr]:]

    def drop_leadership(self, client):
        self.send(client, T.message(S.MSG_DROP_LEADERSHIP))

    def relay_owner(self, client):
        return self.w.peers[client.addr].owner_id

    def test_starting_the_setup_shows_the_leader_to_everybody_but_gives_the_others_nothing_to_lead(self):
        c = self.join(3)
        self.grant(self.a)
        self.assertEqual(self.f(self.a), [('own', 1)])             # the leader's own game must know, or it would toast itself
        for client in (self.b, c):
            self.assertEqual(self.f(client), [('relay', self.relay_owner(self.a), 1)])
        self.assertEqual(view(self.b), {self.relay_owner(self.a)})
        self.assertEqual(view(self.a), {'own'})

    def test_a_second_player_is_refused_while_the_first_is_setting_up_and_gets_it_after_the_drop(self):
        c = self.join(3)
        self.grant(self.a)
        self.grant(self.b)
        oa, ob = self.relay_owner(self.a), self.relay_owner(self.b)
        self.assertEqual(self.w.leader, self.w.peers[self.a.addr])
        self.assertEqual(self.b.taken, [(6, (1, 0), 0, 0)], 'b was not told it is not the owner')    # message 12: OwnershipDenied for b
        self.assertEqual(self.f(c), [('relay', oa, 1)])
        self.drop_leadership(self.a)
        self.grant(self.b)
        # a bystander never sees two leaders: the old flag goes first
        self.assertEqual(self.f(c), [('relay', oa, 1), ('relay', oa, 0), ('relay', ob, 1)])
        self.assertEqual((view(self.a), view(self.b), view(c)), ({ob}, {'own'}, {ob}))

    def test_a_player_whose_setup_ended_must_ask_again_so_the_next_setup_is_seen(self):
        self.grant(self.a)
        self.assertEqual(self.a.taken, [])
        self.drop_leadership(self.a)
        self.assertEqual(len(self.a.taken), 1, 'the options were not taken back, so a never asks again')
        self.assertEqual(self.a.taken[0][:3], (6, (1, 0), 0))
        self.assertEqual(self.a.taken[0][3], 1, 'the take-back reused the number of the grant: the client drops it as old')
        self.grant(self.a)                                                   # what the game does on its next setup
        oa = self.relay_owner(self.a)
        self.assertEqual(self.f(self.b), [('relay', oa, 1), ('relay', oa, 0), ('relay', oa, 1)])

    def test_a_refused_request_does_not_change_who_leads(self):
        self.grant(self.a)
        self.grant(self.b)
        self.assertEqual(self.f(self.a), [('own', 1)])
        self.assertEqual(self.f(self.b), [('relay', self.relay_owner(self.a), 1)])

    def test_the_numbers_of_the_messages_to_a_client_keep_counting_through_a_refusal_and_a_later_grant(self):
        self.grant(self.a)
        self.grant(self.b)                                                   # refused: seq 0 for b
        self.drop_leadership(self.a)
        self.grant(self.b)                                                   # granted: the next seq of b
        self.assertEqual([t[3] for t in self.b.taken], [0])
        self.assertEqual(self.w.peers[self.b.addr].gopts_seq, 2)

    def test_dropping_leadership_clears_the_flag_for_everybody(self):
        self.grant(self.a)
        self.drop_leadership(self.a)
        oa = self.relay_owner(self.a)
        self.assertEqual(self.f(self.a), [('own', 1), ('own', 0)])
        self.assertEqual(self.f(self.b), [('relay', oa, 1), ('relay', oa, 0)])
        self.assertEqual((view(self.a), view(self.b)), (set(), set()))

    def test_a_player_that_is_not_the_leader_cannot_drop_somebody_elses_leadership(self):
        self.grant(self.a)
        before = (list(self.f(self.a)), list(self.f(self.b)))
        self.drop_leadership(self.b)
        self.assertEqual((self.f(self.a), self.f(self.b)), before)
        self.assertEqual(view(self.b), {self.relay_owner(self.a)})

    def test_requesting_the_race_while_leader_does_not_flicker_the_flag_and_the_event_end_clears_it(self):
        self.select()                                              # grant + options + cars + request, as the game does
        oa = self.relay_owner(self.a)
        self.assertEqual(self.f(self.b), [('relay', oa, 1)])        # set when the options were asked for, nothing at the race request
        self.assertEqual(self.f(self.a), [('own', 1)])
        self.go()
        self.finish()
        self.assertEqual(self.f(self.b), [('relay', oa, 1), ('relay', oa, 0)])
        self.assertEqual(self.f(self.a), [('own', 1), ('own', 0)])

    def test_a_request_for_a_new_race_after_the_old_event_keeps_the_requester_the_leader_without_flicker(self):
        self.select()
        self.go()
        self.finish()
        oa = self.relay_owner(self.a)
        self.grant(self.b)
        ob = self.relay_owner(self.b)
        self.assertEqual(view(self.a), {ob})
        self.assertEqual(view(self.b), {'own'})
        seen = (len(self.f(self.a)), len(self.f(self.b)))
        self.options(self.b, {4: 1002, 30: 3})
        self.car(self.b, 3, .750)
        self.car(self.a, 3, .750)
        self.request(self.b)
        self.assertEqual((len(self.f(self.a)), len(self.f(self.b))), seen, 'the requester was switched off and on again')
        self.assertNotIn(oa, view(self.b))

    def test_a_bystander_cannot_take_the_flag_while_an_event_is_running(self):
        self.select()
        c = self.join(3)
        oa = self.relay_owner(self.a)
        self.assertIsNotNone(self.w.event)
        self.assertEqual(view(c), {oa})
        before = list(self.f(c)), list(self.f(self.a)), list(self.f(self.b))
        self.grant(c)                                              # the server still hands out the options object, as before
        self.assertEqual((self.f(c), self.f(self.a), self.f(self.b)), before)
        self.assertEqual(self.w.leader, self.w.peers[self.a.addr])

    def test_the_flag_goes_when_the_leader_leaves_and_the_next_leader_is_set_alone(self):
        c = self.join(3)
        self.grant(self.a)
        self.w.drop_peer(self.w.peers[self.a.addr], 'test')
        self.assertIsNone(self.w.leader)
        self.drain()
        self.grant(c)
        self.assertEqual(self.f(self.b)[-1:], [('relay', self.relay_owner(c), 1)])
        self.assertNotIn(('relay', self.relay_owner(self.b), 0), self.f(self.b))
        self.assertEqual(self.f(c)[-1:], [('own', 1)])

    def test_a_player_who_joins_while_somebody_is_setting_up_is_told_who(self):
        self.grant(self.a)
        late = self.join(3)
        self.assertEqual(view(late), {self.relay_owner(self.a)})

    def test_a_player_who_joins_after_the_setup_ended_is_told_nobody_leads(self):
        self.grant(self.a)
        self.drop_leadership(self.a)
        late = self.join(3)
        self.assertEqual(view(late), set())

    def test_a_setup_that_is_abandoned_without_a_drop_expires(self):
        self.grant(self.a)
        self.advance(S.LEADER_SETUP_SECONDS + 2)
        oa = self.relay_owner(self.a)
        self.assertEqual(self.f(self.b)[-1:], [('relay', oa, 0)])
        self.assertEqual(view(self.a), set())

    def test_it_can_be_switched_off(self):
        with mock.patch.dict(os.environ, {'FH2MP_NO_LEADER': '1'}):
            self.grant(self.a)
            self.drop_leadership(self.a)
        self.assertEqual((self.f(self.a), self.f(self.b)), ([], []))


if __name__ == '__main__':
    unittest.main()
