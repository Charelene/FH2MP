"""Launcher social/lease regressions, without game clients or network traffic.

The store tests execute the selected real migration DDL on an isolated SQLite
connection (identity syntax adapted). test_community_pg runs the same store
contracts on real PostgreSQL migrations. Matcher tests use the real xbl matcher.
"""
import json
import os
import re
import sqlite3
import sys
import threading
import time
import unittest
from contextlib import contextmanager
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import auth
import community
import db
import launcher_api
import launcher_lobbies as leases
import social
import xbl


class MemoryDb:
    def __init__(self):
        self.conn = sqlite3.connect(':memory:')
        self.conn.row_factory = sqlite3.Row
        self.conn.create_function('GREATEST', -1, max)
        wanted = {'players', 'accounts', 'friendships', 'clubs', 'club_members', 'friend_requests',
                  'launcher_messages', 'launcher_chat_reads', 'player_profiles'}
        for migration in db.MIGRATIONS:
            for statement in db._statements(migration):
                table = re.match(r'CREATE TABLE\s+(\w+)', statement, re.I)
                if table and table[1] in wanted:
                    sql = statement
                    sql = re.sub(r'BIGINT GENERATED (?:BY DEFAULT|ALWAYS) AS IDENTITY PRIMARY KEY', 'INTEGER PRIMARY KEY AUTOINCREMENT', sql)
                    self.conn.execute(sql)

    def query(self, sql, args=()):
        return self.conn.execute(sql, args).fetchall()

    def one(self, sql, args=()):
        return self.conn.execute(sql, args).fetchone()

    def close(self):
        self.conn.close()

    @contextmanager
    def tx(self):
        with self.conn:
            yield self.conn


class SocialTests(unittest.TestCase):
    db_factory = MemoryDb

    def setUp(self):
        self.db = self.db_factory()
        with self.db.tx() as c:
            for i, tag in ((1, 'DriverOne'), (2, 'DriverTwo'), (3, 'ThirdDriver')):
                c.execute('INSERT INTO accounts(xuid,pubkey,gamertag,gamertag_key,created) VALUES (?,?,?,?,0)',
                          (i, bytes([i])*32, tag, auth.fold_tag(tag)))
                c.execute('INSERT INTO players(xuid,gamertag,first_seen,last_seen) VALUES (?,?,0,0)', (i, tag))
        self.social = social.SocialStore(self.db)

        def authenticate(token, ip):
            if token not in ('token1', 'token2', 'token3'):
                raise auth.AuthError(401, 'invalid_token')
            return auth.Account(int(token[-1]), 'Driver')
        self.ctx = SimpleNamespace(auth=SimpleNamespace(db=self.db, authenticate=authenticate, hit=lambda *args: True), social=self.social)
        self.s = community.Community(self.ctx)
        self.pop_patch = patch.object(xbl, 'hub_populations', return_value={})
        self.lobby_patch = patch.object(xbl, 'hub_lobbies', return_value={'lobbies': []})
        self.pop_patch.start(); self.lobby_patch.start()
        community.LAUNCHER_SEEN.clear(); social.LAST_POLL.clear(); leases.HOLDS.clear()

    def tearDown(self):
        self.pop_patch.stop(); self.lobby_patch.stop()
        self.db.close()

    def route(self, method, path='', body=None, token='token1'):
        return launcher_api.handle(self.ctx, method, '/v1/community'+path, {'Authorization': 'Bearer '+token},
                                   json.dumps(body or {}).encode(), '127.0.0.1')

    def friends(self):
        self.s.request(1, 'DriverTwo')
        self.s.respond(2, 1, True)

    def test_request_requires_recipient_acceptance_and_survives_new_store(self):
        self.s.request(1, 'drivertwo')
        self.assertFalse(self.social.are_friends(1, 2))
        self.assertEqual(self.s.snapshot(2)['incoming'][0]['xuid'], 1)
        with self.assertRaises(auth.AuthError):
            self.s.respond(3, 1, True)
        self.s.respond(2, 1, True)
        self.assertTrue(social.SocialStore(self.db).are_friends(1, 2))
        self.assertEqual(self.s.snapshot(1)['outgoing'], [])

    def test_self_unknown_and_id_requests(self):
        for value in ('DriverOne', 'unknown', str(2**80)):
            with self.assertRaises(auth.AuthError):
                self.s.request(1, value)
        self.s.request(1, '0x2')
        self.assertEqual(self.s.snapshot(1)['outgoing'][0]['xuid'], 2)

    def test_crossed_requests_accept_cleans_both_and_decline_does_not_friend(self):
        self.s.request(1, '2'); self.s.request(2, '1')
        self.s.respond(1, 2, True)
        self.assertEqual(self.db.one('SELECT COUNT(*) FROM friend_requests')[0], 0)
        self.s.request(1, '3'); self.s.respond(3, 1)
        self.assertFalse(self.social.are_friends(1, 3))

    def test_cancelling_stale_request_does_not_remove_accepted_friendship(self):
        self.friends()
        self.assertEqual(self.route('DELETE', '/friends/2/request')[0], 200)
        self.assertTrue(self.social.are_friends(1, 2))

    def test_reservation_api_never_exposes_internal_match_key(self):
        status, body, _ = self.route('POST', '/reservation', {'kind': 'freeroam'})
        self.assertEqual(status, 200)
        self.assertNotIn('key', body['reservation'])
        status, body, _ = self.route('GET', '/lobbies')
        self.assertEqual(status, 200)
        self.assertEqual(body['lobbies'][0]['held'], 1)
        self.route('DELETE', '/reservation')
        self.assertIsNone(leases.current(1))

    def test_authentication_and_actor_come_from_bearer(self):
        self.assertEqual(self.route('GET', token='bogus')[0], 401)
        self.route('POST', '/friends', {'player': 'DriverTwo', 'xuid': 3})
        self.assertEqual(self.s.snapshot(2)['incoming'][0]['xuid'], 1)

    def test_messages_are_persistent_private_and_revoked_on_unfriend(self):
        with self.assertRaises(auth.AuthError):
            self.s.send(1, '2', 'hello')
        self.friends()
        self.s.send(1, '2', '<img src=x> hello')
        self.assertEqual(community.Community(self.ctx).messages(2, '1')['messages'][0]['body'], '<img src=x> hello')
        with self.assertRaises(auth.AuthError):
            self.s.messages(3, '1')
        self.route('DELETE', '/friends/2')
        with self.assertRaises(auth.AuthError):
            self.s.messages(2, '1')

    def test_chat_urls_require_bearer_and_never_select_another_accounts_channel(self):
        self.friends()
        self.s.send(1, '2', 'private between one and two')
        for query in ('', '?token=token1', '?access_token=token1', '?xuid=1&channel=dm:1:2'):
            status, body, _ = launcher_api.handle(self.ctx, 'GET', '/v1/community/chat/2'+query,
                                                  {}, b'', '127.0.0.1')
            self.assertEqual(status, 401)
            self.assertNotIn('messages', body)
        self.assertEqual(self.route('GET', '/chat/2', token='token1')[1]['messages'][0]['body'],
                         'private between one and two')
        self.assertEqual(self.route('GET', '/chat/1', token='token3')[0], 403)
        self.s.request(3, 'DriverOne')
        self.s.respond(1, 3, True)
        # Even a friend of one participant only sees their own conversation.
        status, body, _ = self.route('GET', '/chat/1?xuid=2&channel=dm:1:2', token='token3')
        self.assertEqual(status, 200)
        self.assertEqual(body['messages'], [])

    def test_chat_validation_pagination_and_read_cursor(self):
        self.friends()
        for bad in ('', ' ', 'x'*2001, 'a\x00b', 17):
            with self.assertRaises(auth.AuthError):
                self.s.send(1, '2', bad)
        for i in range(105): self.s.send(1, '2', str(i))
        rows = self.s.messages(2, '1')['messages']
        self.assertEqual((len(rows), rows[0]['body'], rows[-1]['body']), (100, '5', '104'))
        self.assertEqual(self.s.messages(2, '1', rows[-1]['id'])['messages'], [])
        self.assertEqual(self.s.channels(2)[0]['unread'], 105)
        self.assertEqual(self.s.channels(2)[0]['lastIncomingId'], rows[-1]['id'])
        self.s.read(2, '1', rows[-1]['id'])
        self.s.read(2, '1', 0)
        self.assertEqual(self.s.channels(2)[0]['unread'], 0)
        self.assertEqual(self.s.channels(2)[0]['lastIncomingId'], rows[-1]['id'])
        self.s.send(2, '1', 'my own reply')
        self.assertEqual(self.s.channels(2)[0]['lastIncomingId'], rows[-1]['id'])

    def test_club_chat_rechecks_current_membership(self):
        with self.db.tx() as c:
            c.execute("INSERT INTO clubs(id,tag,name,created) VALUES (1,'TEST','Test Club',0)")
            for x in (1, 2):
                c.execute('INSERT INTO club_members(xuid,club_id,joined,status_changed) VALUES (?,1,0,0)', (x,))
        self.s.send(1, 'club', 'members only')
        self.assertEqual(self.s.messages(2, 'club')['messages'][0]['body'], 'members only')
        with self.assertRaises(auth.AuthError): self.s.messages(3, 'club')
        with self.db.tx() as c: c.execute('DELETE FROM club_members WHERE xuid=2')
        with self.assertRaises(auth.AuthError): self.s.messages(2, 'club')
        with self.assertRaises(auth.AuthError): self.s.send(2, 'club', 'after leaving')

    def test_launcher_presence_does_not_claim_game_presence(self):
        self.route('GET')
        self.assertEqual(self.s.profile(1)['state'], 'online')
        self.assertFalse(social.online(1))
        social.touch(1)
        self.assertEqual(self.s.profile(1)['state'], 'playing')

    def test_profile_picture_version_tracks_upload_and_removal(self):
        self.assertEqual(self.s.profile(1)['gamerpicVersion'], 0)
        with patch.object(social.time, 'time', return_value=1000):
            self.social.set_gamerpic(1, social.identicon(1))
        self.assertEqual(self.s.profile(1)['gamerpicVersion'], 1000)
        with patch.object(social.time, 'time', return_value=2000):
            self.social.clear_gamerpic(1)
        self.assertEqual(self.s.profile(1)['gamerpicVersion'], 2000)

    def test_rate_limited_mutations(self):
        self.ctx.auth.hit = lambda *args: False
        self.assertEqual(self.route('POST', '/friends', {'player': '2'})[0], 429)


class ReservationTests(unittest.TestCase):
    def setUp(self):
        xbl._SESSIONS.clear(); xbl._LOBBIES.clear(); xbl._RESERVED.clear(); leases.HOLDS.clear()
        self.pops = {}
        self.patch = patch.object(xbl, 'hub_populations', side_effect=lambda: self.pops)
        self.patch.start()
        # Captured native tickets use the same GSI for all three hoppers.
        self.attrs = {'gsiSetId': '00000000-0000-0000-0000-000000000102'}

    def tearDown(self):
        self.patch.stop(); leases.HOLDS.clear()

    def lobby(self, hopper='FreeRoam_qos', attrs=None, player=100):
        return xbl._lobby_for('s', hopper, attrs or self.attrs, None, str(player))

    def test_hold_picks_selected_lobby_instead_of_fullest_and_consumes_once(self):
        a = self.lobby()
        self.pops[a] = [f'{i:x}' for i in range(100, 112)]
        b = self.lobby(player=200)
        self.pops[a] = [f'{i:x}' for i in range(100, 110)]
        self.pops[b] = ['c8']
        leases.reserve(300, b)
        self.assertEqual(self.lobby(player=300), b)
        self.assertIsNone(leases.current(300))
        self.assertIn('300', xbl._RESERVED[b])

    def test_last_seat_concurrent_holds_only_one_wins(self):
        a = self.lobby()
        self.pops[a] = [f'{i:x}' for i in range(100, 111)]
        result = []
        def reserve(x):
            try: leases.reserve(x, a); result.append('ok')
            except auth.AuthError as e: result.append(e.code)
        threads = [threading.Thread(target=reserve, args=(x,)) for x in (300,301)]
        for t in threads:t.start()
        for t in threads:t.join()
        self.assertEqual(sorted(result), ['lobby_full','ok'])

    def test_hold_expiry_replacement_and_cancellation(self):
        a = self.lobby()
        first = leases.reserve(300, a)
        second = leases.reserve(300, kind='roadtrip')
        self.assertNotEqual(first['id'], second['id'])
        self.assertEqual(leases.seats(a), set())
        leases.HOLDS['300']['expiresAt'] = time.time()-1
        self.assertIsNone(leases.current(300))
        leases.reserve(300, a);leases.cancel(300)
        self.assertIsNone(leases.current(300))

    def test_mode_mismatch_preserves_hold_then_uses_real_matching_ticket(self):
        hold = leases.reserve(300, kind='roadtrip')
        self.assertNotEqual(self.lobby(player=300), hold['id'])
        self.assertIsNotNone(leases.current(300))
        attrs = {'gsiSetId': '00000000-0000-0000-0000-000000000102', 'Location': 5}
        name = self.lobby('RoadTrip_qos', attrs, 300)
        self.assertEqual(name, hold['id'])
        rec = xbl._SESSIONS[('s','defaulttemplate',name)]
        self.assertEqual(rec['constants']['custom']['gameServerPlatform']['gsiSetId'], attrs['gsiSetId'])

    def test_real_freeroam_ticket_uses_new_hold_instead_of_populated_lobby(self):
        # Native FreeRoam_qos capture: t2/fh2mp-lsp-142.req (also -452.req).
        a = self.lobby()
        self.pops[a] = ['64']
        hold = leases.reserve(300, kind='freeroam')
        ref = {'scid': 's', 'templateName': 'defaultTemplate_qos', 'name': 'requester'}
        xbl.put_session('/serviceconfigs/s/sessionTemplates/defaultTemplate_qos/sessions/requester',
                        b'{"members":{"me":{}}}', '300')
        attrs = {**self.attrs, 'hopperId': 2, 'skill': 0, 'NewPlayer': 1}
        status, ticket = xbl.create_ticket('/serviceconfigs/s/hoppers/FreeRoam_qos',
                                          json.dumps({'ticketSessionRef': ref, 'ticketAttributes': attrs}).encode(), '300')
        self.assertEqual(status, 200)
        with patch.object(xbl, 'announce_player'):
            status, result = xbl.get_ticket('/serviceconfigs/s/hoppers/FreeRoam_qos/tickets/' + ticket['ticketId'], '300')
        self.assertEqual(status, 200)
        self.assertEqual(result['targetSessionRef']['name'], hold['id'])
        self.assertIsNone(leases.current(300))
        self.assertEqual(result['ticketAttributes'], attrs)

    def test_carmeet_with_shared_gsi_does_not_consume_either_mode_hold(self):
        for kind in ('freeroam', 'roadtrip'):
            hold = leases.reserve(300, kind=kind)
            name = self.lobby('CarMeet_qos', {**self.attrs, 'hopperId': 3, 'Location': 6, 'ParentBucket': 2}, 300)
            self.assertNotEqual(name, hold['id'])
            self.assertEqual(leases.current(300)['id'], hold['id'])
            with self.assertRaises(auth.AuthError):
                leases.reserve(301, name)

    def test_existing_lobby_mode_comes_from_hopper_not_shared_gsi(self):
        for hopper, kind in [('FreeRoam_qos', 'freeroam'), ('RoadTrip_qos', 'roadtrip')]:
            name = self.lobby(hopper)
            hold = leases.reserve(300, name)
            self.assertEqual(hold['kind'], kind)
            other = 'RoadTrip_qos' if kind == 'freeroam' else 'FreeRoam_qos'
            self.assertNotEqual(self.lobby(other, player=300), name)
            self.assertEqual(leases.current(300)['id'], name)
            self.assertEqual(self.lobby(hopper, player=300), name)
            self.assertIsNone(leases.current(300))

    def test_existing_hold_cannot_cross_location(self):
        a = self.lobby()
        leases.reserve(300, a)
        self.assertNotEqual(self.lobby(attrs={**self.attrs,'Location':5}, player=300), a)
        self.assertEqual(leases.current(300)['id'], a)

    def test_quarantine_pool_and_server_unavailability_fail_closed(self):
        a = self.lobby(); self.pops[a] = ['64']; self.pops['!flagged'] = ['12c']
        with self.assertRaises(auth.AuthError):leases.reserve(300, a)
        self.pops = None
        with self.assertRaises(auth.AuthError):leases.reserve(300, a)

    def test_automatic_matchmaking_counts_launcher_hold(self):
        a = self.lobby(); self.pops[a] = [f'{i:x}' for i in range(100,111)]
        leases.reserve(300, a)
        self.assertNotEqual(self.lobby(player=301), a)
        self.assertEqual(self.lobby(player=300), a)

    def test_old_matchmaking_seats_do_not_block_launcher_reservations(self):
        a = self.lobby()
        self.pops[a] = [f'{i:x}' for i in range(100,111)]
        xbl._RESERVED[a]['400'] = time.time() - xbl.RESERVE_SECONDS - 1
        self.assertEqual(leases.reserve(300, a)['id'], a)


if __name__ == '__main__':
    unittest.main()
