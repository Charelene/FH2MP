"""Zen social cues follow new confirmed events, never history or repeated polls."""
import os
import sys
import unittest
import wave
from unittest.mock import Mock, patch

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from fh2mp_launcher.ui import sfx


class SocialSounds(unittest.TestCase):
    def setUp(self):
        self.emit = Mock()
        self.sounds = sfx.SocialNotifications(self.emit)
        self.context = ('dev', 'dev', 9)

    def snapshot(self, friends=(), latest=0, unread=0):
        self.sounds.snapshot(self.context, {'friends':[{'xuid':x} for x in friends],
                                          'chats':[{'id':'1','lastIncomingId':latest,'unread':unread}]})

    def test_startup_and_repeated_polls_are_quiet(self):
        self.snapshot((1,2), 30, 8)
        self.snapshot((1,2), 30, 8)
        self.emit.assert_not_called()

    def test_new_friend_sounds_once_not_for_request_or_presence(self):
        self.snapshot((1,))
        self.sounds.snapshot(self.context, {'friends':[{'xuid':1,'state':'playing'}], 'incoming':[{'xuid':2}]})
        self.emit.assert_not_called()
        self.snapshot((1,2))
        self.snapshot((1,2))
        self.emit.assert_called_once_with('friend_added')

    def test_accept_is_immediate_and_stale_poll_does_not_repeat_it(self):
        self.snapshot((1,))
        self.sounds.accepted(self.context, 2)
        self.snapshot((1,))
        self.snapshot((1,2))
        self.emit.assert_called_once_with('friend_added')

    def test_incoming_message_sounds_once_even_if_read_before_poll(self):
        self.snapshot((1,), 30)
        self.snapshot((1,), 31, 0)
        self.snapshot((1,), 31, 0)
        self.emit.assert_called_once_with('receive')

    def test_open_chat_and_summary_share_the_same_watermark(self):
        self.snapshot((1,), 30)
        self.sounds.messages(self.context, '1', [{'id':31,'sender':1}], 9)
        self.snapshot((1,), 31, 1)
        self.emit.assert_called_once_with('receive')

    def test_history_and_own_messages_never_sound(self):
        self.snapshot((1,), 30)
        self.sounds.messages(self.context, '1', [{'id':30,'sender':1},{'id':31,'sender':9}], 9)
        self.emit.assert_not_called()

    def test_switching_account_resets_silently_and_older_api_history_is_quiet(self):
        self.snapshot((1,), 30)
        self.context = ('dev', 'dev', 10)
        self.snapshot((2,3), 100)
        self.sounds.snapshot(('old-api', 'dev', 10), {'friends':[], 'chats':[{'id':'1','unread':4}]})
        self.sounds.messages(('old-api', 'dev', 10), '1', [{'id':50,'sender':1}], 10)
        self.emit.assert_not_called()

    def test_bundled_zen_files_use_existing_wav_playback(self):
        for cue in ('receive','friend_added'):
            with patch.object(sfx, 'play') as play:
                sfx.social(cue)
                path = play.call_args.args[0]
            with wave.open(path) as w:
                self.assertEqual((w.getnchannels(), w.getsampwidth()), (1,2))
                self.assertGreater(w.getnframes(), 0)
        with patch.object(sfx, 'play', side_effect=OSError('audio unavailable')):
            sfx.social('receive')


if __name__ == '__main__':
    unittest.main()
