"""Bucket-list co-op over the real server and a wire reader written from the client's own readers (bucketlist.py, docs/dlc-recon-2026-10-27.md).

The 74-bit PlayerState update and the (15, 5) of RequestSynchronize are what a real dev client sent on 2026-10-29 18:13 (server log, router 12
id 0, then message 63); everything the server sends is read back by the independent Receiver of test_race_flow, which reads the keys the way
FUN_1417ba4d0 does. This checks delivery and pairing; whether the game then starts the challenge needs two real clients.
"""
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bucketlist
import fh2mp_server as S
from test_race_flow import Bits, MemorySocket, Receiver, message, packet

# the update the real client sent with the request: vars 1 (CurrentSlotId) = 15, 2 (CurrentChallengeId) = 5, 3 (RequestingPartner) = 1
REQUESTING = Bits().put(1, 3).put(15, 32).put(2, 3).put(5, 32).put(3, 3).put(1, 1)
# and the one that cancelled it
CANCELLED = Bits().put(1, 3).put(0, 32).put(2, 3).put(0, 32).put(3, 3).put(0, 1)


def ready(value=2):
    """The client reports State (var 4, 3 bits) after BucketListLocalPlayerReady (dev capture 2026-10-29: 1 then 2)."""
    return Bits().put(4, 3).put(value, 3)


def signed(value):
    return value & 0xFFFFFFFF


def bucket_object(body=None, seq=0, create=False):
    out = message(7 if create else 8, (12, 4), (0, 1), (0, 1), (0, 4))        # router 12, plain key (A=0, B=0), id 0
    if create:
        out.put(0, 2)                                                          # object type 0: BucketListPlayerStateSynced (2 type bits)
    body = body or Bits()
    return out.put(seq, 8).put(body.size, 14).put(body.value, body.size)


class BucketListCoop(unittest.TestCase):
    def setUp(self):
        self.now = 1000.0
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(S.time, 'monotonic', side_effect=lambda: self.now).start()
        mock.patch.object(S.socket, 'socket', side_effect=AssertionError('offline tests must not open sockets')).start()
        mock.patch.object(S, 'RESULT_SINK', None).start()
        mock.patch.object(bucketlist, 'STATUS', 'off').start()        # the status messages have their own tests
        self.sock = MemorySocket()
        self.w = S.Server(0, None, sock=self.sock)
        self.logs = []
        self.w.log = self.logs.append
        self.clients = {}
        self.a, self.b = self.join(1), self.join(2)

    def drain(self):
        self.w.flush(float('inf'))
        sent, self.sock.sent = self.sock.sent, []
        for data, addr in sent:
            self.clients[addr].receive(data)

    def send(self, client, *messages):
        self.w.handle(packet(messages), client.addr)
        self.drain()

    def join(self, number):
        from test_race_flow import obj, fields
        client = Receiver(('192.0.2.' + str(number), 5000 + number))
        self.clients[client.addr] = client
        hello = message(0).text('6CC152FD').text('Flagship').put(0, 2).put(0x9000000010000 + number, 64)
        hello.put(2, 32).put(0, 32).put(7, 32)
        self.send(client, hello)
        self.send(client, message(1, (0, 2), (6, 3), (client.counter, 32), (7, 32)))
        self.send(client, obj(3, fields({0: 0, 3: 0, 4: 0, 5: 0}, dict.fromkeys(range(7), 1), 4), create=True), message(34))
        return client

    def appear(self, *clients):
        for c in clients:
            self.send(c, bucket_object(create=True))

    def request(self, client, slot=15, challenge=5, state=REQUESTING):
        self.send(client, bucket_object(state, seq=1), message(63).put(slot, 32).put(challenge, 32))

    def test_a_clients_player_state_reaches_the_others_and_late_joiners(self):
        self.appear(self.a)
        self.assertEqual(sorted(self.b.bucket_owner), [0])
        c = self.join(3)
        self.assertEqual(sorted(c.bucket_owner), [0], 'a late joiner was not given the existing bucket-list object')

    def slot_entries(self, client):
        """What the client has been told about the slot array, read with 6-bit variable ids: {entry: [slot, challenge, owner]}."""
        entries = {}
        for kind, tid, oid, _seq, body in client.slot_messages:
            self.assertEqual(oid, 0)
            if kind == 7:
                self.assertEqual(tid, 1, 'the slot array is object type 1 of router 12')
            body.pos = 0
            while body.pos < body.size:
                vid = body.take(6)
                value = body.take(32)
                value = value - (1 << 32) if value >> 31 else value
                entries.setdefault(vid // 3, [0, 0, 0])[vid % 3] = value
        return entries

    def test_every_client_is_given_the_slot_array_when_it_joins(self):
        self.assertEqual([m[0] for m in self.a.slot_messages], [7])
        self.assertEqual(self.slot_entries(self.a), {})
        self.assertEqual(self.a.slot_messages[0][3], 1, 'the create is message 1 of the object')

    def test_a_waiting_request_claims_a_slot_for_everybody_and_pairing_frees_it(self):
        self.appear(self.a, self.b)
        self.request(self.a)
        owner_a = self.b.bucket_owner[0]
        for client in (self.a, self.b):
            self.assertEqual(self.slot_entries(client), {0: [15, 5, owner_a]}, 'the waiting player owns the slot (its relay owner id)')
        seqs = [m[3] for m in self.b.slot_messages]
        self.assertEqual(seqs, list(range(1, len(seqs) + 1)), 'the object is numbered by the server per client')
        self.request(self.b)
        for client in (self.a, self.b):
            self.assertEqual(self.slot_entries(client), {0: [0, 0, 0]}, 'a paired slot is free again')

    def test_a_cancelled_request_or_a_player_who_leaves_frees_the_slot(self):
        self.appear(self.a, self.b)
        self.request(self.a)
        self.send(self.a, bucket_object(CANCELLED, seq=2), message(64))
        self.assertEqual(self.slot_entries(self.b), {0: [0, 0, 0]})
        self.request(self.a)
        self.w.drop_peer(self.w.peers[self.a.addr], 'test')
        self.drain()
        self.assertEqual(self.slot_entries(self.b), {0: [0, 0, 0]})

    def test_a_joiner_is_told_about_slots_that_are_already_taken(self):
        self.appear(self.a)
        self.request(self.a)
        c = self.join(3)
        self.assertEqual(self.slot_entries(c), {0: [15, 5, c.bucket_owner.get(0, 0) or self.slot_entries(c)[0][2]]})
        self.assertEqual(self.slot_entries(c)[0][:2], [15, 5])

    def test_twelve_slots_at_most(self):
        table = bucketlist.SlotTable()
        for slot in range(1, 13):
            self.assertEqual(table.claim(slot, 1, 1), [slot - 1])
        self.assertIsNone(table.claim(13, 1, 1))
        self.assertEqual(table.claim(5, 9, 2), [4], 'an existing slot is taken over, not duplicated')

    def test_the_captured_player_state_update_is_understood(self):
        state = bucketlist.decode_player_state(REQUESTING.size, REQUESTING.value)
        self.assertEqual((REQUESTING.size, state), (74, {'CurrentSlotId': 15, 'CurrentChallengeId': 5, 'RequestingPartner': 1}))
        state = bucketlist.decode_player_state(CANCELLED.size, CANCELLED.value)
        self.assertEqual(state, {'CurrentSlotId': 0, 'CurrentChallengeId': 0, 'RequestingPartner': 0})
        self.appear(self.a)
        self.request(self.a)
        self.assertTrue([l for l in self.logs if "'CurrentSlotId': 15" in l])

    def test_two_players_at_the_same_challenge_are_paired_and_started_when_both_are_ready(self):
        self.appear(self.a, self.b)
        self.request(self.a)
        self.assertEqual((self.a.bucket, self.b.bucket), ([], []), 'a lone request got an answer')
        self.request(self.b)
        owner_a, owner_b = self.b.bucket_owner[0], self.a.bucket_owner[0]
        for client, partner_owner in ((self.a, owner_b), (self.b, owner_a)):
            self.assertEqual([m[0] for m in client.bucket], ['partner'], 'the start time must not come with the partner reply')
            _kind, key, value, found, name = client.bucket[0]
            self.assertEqual(value, 0 if client is self.a else 1, 'each player needs its own start position (a waited, b joined)')
            self.assertEqual(key, ('relay', 0, partner_owner), 'the partner must be keyed the way the receiver knows its PlayerState')
            self.assertTrue(found)
            self.assertLessEqual(len(name), 36)
        # the clients cancel the request right after the partner was found; the pairing must survive that
        self.send(self.a, bucket_object(CANCELLED, seq=2), message(64))
        self.send(self.b, bucket_object(CANCELLED, seq=2), message(64))
        self.send(self.a, bucket_object(ready(1), seq=3))
        self.send(self.b, bucket_object(ready(1), seq=3))
        self.send(self.a, bucket_object(ready(2), seq=4))
        self.assertEqual([m[0] for m in self.a.bucket + self.b.bucket], ['partner', 'partner'], 'started before the partner was ready')
        self.send(self.b, bucket_object(ready(2), seq=4))
        for client in (self.a, self.b):
            self.assertEqual([m[0] for m in client.bucket], ['partner', 'start'], client.bucket)
            start = client.bucket[1][1]
            self.assertNotEqual(start, 0)
            self.assertGreater((start - self.w.now_ms()) & 0xFFFFFFFF, 0, 'the start time is already past')
        self.assertEqual(self.a.bucket[1][1], self.b.bucket[1][1], 'both clients must start together')
        self.send(self.a, bucket_object(ready(2), seq=5))
        self.assertEqual(len(self.a.bucket), 2, 'the start time was sent twice')

    def test_a_joiner_that_sends_only_its_player_state_is_paired(self):
        # dev 2026-10-30 00:18:13-15: owner 2 sent the state and the request message and waited; owner 1 pressed Start at the same slot and
        # challenge and its packet held only the state (RequestingPartner, slot 15, challenge 5). Both gave up after 15 s.
        self.appear(self.a, self.b)
        self.request(self.b)
        self.assertEqual((self.a.bucket, self.b.bucket), ([], []))
        self.send(self.a, bucket_object(REQUESTING, seq=1))             # no message 63
        self.assertEqual(self.a.bucket, [], 'paired before the request message had a chance to follow')
        self.now += 1.0
        self.w.tick()
        self.drain()
        for client in (self.a, self.b):
            self.assertEqual([m[0] for m in client.bucket], ['partner'], client.bucket)

    def test_a_state_followed_by_its_request_message_is_not_requested_twice(self):
        self.appear(self.a, self.b)
        self.request(self.a)
        self.now += 1.0
        self.w.tick()
        self.drain()
        self.assertEqual((self.a.bucket, self.b.bucket), ([], []), 'the fallback replaced a request that had its message')
        self.request(self.b)
        self.now += 1.0
        self.w.tick()
        self.drain()
        for client in (self.a, self.b):
            self.assertEqual([m[0] for m in client.bucket], ['partner'], client.bucket)

    def test_a_cancelled_state_is_not_turned_into_a_request(self):
        self.appear(self.a, self.b)
        self.request(self.b)
        self.send(self.a, bucket_object(REQUESTING, seq=1))
        self.send(self.a, bucket_object(CANCELLED, seq=2))
        self.now += 1.0
        self.w.tick()
        self.drain()
        self.assertEqual((self.a.bucket, self.b.bucket), ([], []))

    def test_different_challenges_or_slots_do_not_pair(self):
        self.appear(self.a, self.b)
        self.request(self.a, 15, 5)
        self.request(self.b, 15, 6)
        self.request(self.b, 14, 5)
        self.assertEqual((self.a.bucket, self.b.bucket), ([], []))

    def test_a_cancelled_request_is_not_joined(self):
        self.appear(self.a, self.b)
        self.request(self.a)
        self.send(self.a, bucket_object(CANCELLED, seq=2), message(64))
        self.request(self.b)
        self.assertEqual((self.a.bucket, self.b.bucket), ([], []))

    def test_a_request_that_nobody_joined_expires(self):
        self.appear(self.a, self.b)
        self.request(self.a)
        self.now += bucketlist.WAIT_SECONDS + 1
        self.request(self.b)
        self.assertEqual((self.a.bucket, self.b.bucket), ([], []))

    def test_a_player_without_a_player_state_object_is_ignored(self):
        self.appear(self.b)
        self.send(self.a, message(63).put(15, 32).put(5, 32))
        self.request(self.b)
        self.assertEqual((self.a.bucket, self.b.bucket), ([], []))
        self.assertTrue([l for l in self.logs if 'no bucket-list player state' in l])

    def test_a_finished_challenge_frees_both_players_for_the_next_one(self):
        self.appear(self.a, self.b)
        self.request(self.a)
        self.request(self.b)
        self.a.bucket.clear(), self.b.bucket.clear()
        self.send(self.a, message(62).put(1, 1))
        self.request(self.a)
        self.assertEqual(self.b.bucket, [], 'the finished partner was paired again without asking')
        self.request(self.b)
        self.assertEqual([m[0] for m in self.a.bucket], ['partner'])

    def test_a_player_who_leaves_takes_the_waiting_request_with_it(self):
        self.appear(self.a, self.b)
        self.request(self.a)
        self.w.drop_peer(self.w.peers[self.a.addr], 'test')
        self.request(self.b)
        self.assertEqual(self.b.bucket, [])

    def test_ownership_of_bucket_list_objects_is_never_granted(self):
        self.appear(self.a)
        before = len(self.a.responses)
        self.send(self.a, message(10, (12, 4), (1, 2), (0, 1), (0, 1), (0, 4)), message(41, (0, 4), (1, 1)))
        self.assertEqual(len(self.a.responses), before + 1, 'the Ready request after it was lost')
        self.assertFalse([l for l in self.logs if 'RequestOwnership router=12' in l and 'grant' in l.split('->')[-1] and 'ignored' not in l])

    def test_a_waiting_player_is_announced_to_everybody_else(self):
        c = self.join(3)
        with mock.patch.object(bucketlist, 'STATUS', 'challenge'):
            self.appear(self.a, self.b, c)
            self.request(self.a)
        waiting = [m for m in c.bucket if m[0] == 'waiting']
        self.assertEqual(len(waiting), 1)
        self.assertEqual(waiting[0][1], ('relay', 0, self.b.bucket_owner[0]), 'the waiting player is keyed as the receiver knows it')
        self.assertEqual(waiting[0][2], 5, 'the s32 is the challenge')
        self.assertEqual(self.b.bucket[0][0], 'waiting')
        self.assertEqual([m[0] for m in self.a.bucket], [], 'the waiting player is not told about itself')

    def test_the_status_messages_are_on_by_default(self):
        import importlib
        mock.patch.stopall()
        self.assertEqual(importlib.reload(bucketlist).STATUS, 'challenge')

    def test_status_messages_reach_the_others_when_enabled(self):
        c = self.join(3)
        with mock.patch.object(bucketlist, 'STATUS', 'challenge'):
            self.appear(self.a, self.b, c)
            self.request(self.a)
            self.assertEqual([m for m in c.bucket if m[0] == 'waiting'][0][2], 5)
            self.assertEqual(self.b.bucket[0][0], 'waiting')
            self.request(self.b)
            self.assertIn('started', [m[0] for m in c.bucket])


if __name__ == '__main__':
    unittest.main()
