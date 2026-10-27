"""Social polling shares the game token and respects offline/session boundaries."""
import os
import sys
import threading
import unittest
from unittest.mock import Mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from fh2mp_launcher.api import ApiError
from fh2mp_launcher.core import Launcher, PlayBlocked


class Sessions(unittest.TestCase):
    def launcher(self, token='live', running=False):
        lc = Launcher.__new__(Launcher)
        lc._session_lock = threading.RLock()
        lc.token = token
        lc.settings = {'offline_mode': False}
        lc.game_running = Mock(return_value=running)
        lc._sign_in = Mock(side_effect=lambda: setattr(lc, 'token', 'new'))
        lc.api = Mock()
        return lc

    def test_social_poll_uses_existing_game_session(self):
        lc = self.launcher(running=True)
        lc.api().community.return_value = {'friends': []}
        self.assertEqual(lc.community(), {'friends': []})
        lc.api().community.assert_called_once_with('live', '', 'GET', None)
        lc._sign_in.assert_not_called()

    def test_expired_live_token_is_never_silently_replaced(self):
        lc = self.launcher(running=True)
        lc.api().community.side_effect = ApiError(401, 'token_expired', 'expired')
        with self.assertRaises(ApiError): lc.community()
        lc._sign_in.assert_not_called()
        lc = self.launcher(token='', running=True)
        with self.assertRaises(PlayBlocked): lc.community()
        lc._sign_in.assert_not_called()

    def test_expired_idle_session_reauthenticates_once(self):
        lc = self.launcher()
        lc.api().community.side_effect = [ApiError(401, 'token_expired', 'expired'), {'friends': []}]
        self.assertEqual(lc.community(), {'friends': []})
        lc._sign_in.assert_called_once()
        self.assertEqual(lc.api().community.call_args.args[0], 'new')

    def test_revoked_session_polling_does_not_replace_another_launchers_login(self):
        for running in (False, True):
            lc = self.launcher(running=running)
            lc.api().community.side_effect = ApiError(401, 'invalid_token')
            for _ in range(15):
                with self.assertRaises(ApiError):
                    lc.community()
            lc._sign_in.assert_not_called()
            self.assertEqual(lc.token, 'live')

    def test_offline_has_no_social_network_calls(self):
        lc = self.launcher()
        lc.settings['offline_mode'] = True
        with self.assertRaises(PlayBlocked): lc.community()
        lc.api.assert_not_called()

    def test_network_failure_does_not_reauthenticate_or_repeat_a_mutation(self):
        lc = self.launcher()
        lc.api().community.side_effect = ApiError(503, 'unavailable', 'try later')
        with self.assertRaises(ApiError): lc.community('/chat/2', 'POST', {'text': 'hello'})
        lc._sign_in.assert_not_called()
        self.assertEqual(lc.api().community.call_count, 1)


if __name__ == '__main__':
    unittest.main()
