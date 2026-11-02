"""LobbyPlayer.PlayerFlags bit 0 is the VIP flag the player lists draw a crown for (CPlayer::vfunc_23 0x3264d0 sets it from Main/IsVip). With Extra Options
every player is a VIP, so the server clears it in what it relays, stores and tells the owner (Server.hide_vip).

The input is the player-object create of a real client: the capture of a dev test account (docs/protocol.md, CAPTURE player create), whose PlayerFlags is 1."""
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fh2mp_server as S
import player_setup as P
import test_race_flow as T

REAL_CREATE_BITS = 2708
REAL_CREATE = "030e0a200d000fa00c800de00ee00d800dc00c400620066006e0060007802084e7b20e002001604236f03c12003c021f012501000000269f900aa008600a800ac0890040d400000080141898c9cf54ea22361015cc5b3c50d5c615439055c5653ed09514d8341058618a1d4bd61931010080daffffffff1a68a7040068a7040068a704c0ffffff3f68a7040068a7040068a7040068a7040068a7040068a7040068a7040068a704001697020016970200169702001697020016970200169702001697020016970200169702c0ffffffffffffff3f16970200169702c0ffffffffffffffffffffffffffffff3f169702006c5f02006c5f02006c5f02006c5f020068a7040068a7040068a7040068a7040068a7040068a7040068a7040068a704c0ffffff7f1e0000e02f0000e02f0000e02f0000e02f0000e02f0000e02f0000e02f0000e02f0000e06f6400000000a64c9a6e06"


def real_create():
    return T.Bits(int.from_bytes(bytes.fromhex(REAL_CREATE), "little"), REAL_CREATE_BITS)


def flags_of(body):
    """PlayerFlags (var 31) of a player-object write, None when it carries none."""
    field = P.fields(body.size, body.value).get(S.PLAYER_VAR_FLAGS)
    return None if field is None else field[2]


class VipCrown(unittest.TestCase):
    drain, send, advance = T.RaceFlow.drain, T.RaceFlow.send, T.RaceFlow.advance
    setUp, join = T.RaceFlow.setUp, T.RaceFlow.join

    def player_create(self, client, body=None):
        self.send(client, T.obj(1, body or real_create(), 0, create=True))

    def seen(self, client):
        """(own?, kind, PlayerFlags) of every player-object write of the client that carries PlayerFlags."""
        out = []
        for kind, (a, b), _oid, owner, body in client.players:
            try:
                value = flags_of(T.Bits(body.value, body.size))
            except ValueError:
                continue
            if value is not None:
                out.append(((a, b) == (1, 1), kind, value))
        return out

    def test_the_real_client_really_is_a_vip(self):
        self.assertEqual(flags_of(real_create()) & S.PLAYER_FLAG_VIP, 1)

    def test_the_others_get_the_player_without_the_vip_bit(self):
        self.player_create(self.a)
        got = [v for own, kind, v in self.seen(self.b) if not own]
        self.assertTrue(got, "b never received a's player")
        self.assertTrue(all(v & S.PLAYER_FLAG_VIP == 0 for v in got), got)
        self.assertEqual(got[-1], flags_of(real_create()) & ~S.PLAYER_FLAG_VIP)    # nothing else in the flags is touched

    def test_the_owner_is_told_too_so_its_own_list_has_no_crown(self):
        self.player_create(self.a)
        own = [v for is_own, kind, v in self.seen(self.a) if is_own]
        self.assertEqual(own[-1:], [flags_of(real_create()) & ~S.PLAYER_FLAG_VIP])

    def test_a_player_who_joins_later_is_replayed_the_cleared_flags(self):
        self.player_create(self.a)
        late = self.join(3)
        got = [v for own, kind, v in self.seen(late) if not own]
        self.assertTrue(got)
        self.assertTrue(all(v & S.PLAYER_FLAG_VIP == 0 for v in got), got)

    def test_a_later_write_of_the_flag_is_cleared_as_well(self):
        self.player_create(self.a)
        before = len(self.seen(self.b))
        update = T.Bits().put(S.PLAYER_VAR_FLAGS, 8).put(1, 8)
        self.send(self.a, T.obj(1, update, 1))
        new = self.seen(self.b)[before:]
        self.assertEqual([v for own, kind, v in new if not own], [0])

    def test_it_can_be_switched_off(self):
        with mock.patch.dict(os.environ, {"FH2MP_KEEP_VIP": "1"}):
            self.player_create(self.a)
        got = [v for own, kind, v in self.seen(self.b) if not own]
        self.assertEqual(got[-1] & S.PLAYER_FLAG_VIP, 1)


if __name__ == "__main__":
    unittest.main()
