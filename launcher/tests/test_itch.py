"""Browser OAuth callback integrity, cancellation and butler's PKCE handover."""
import base64
import hashlib
import os
import queue
import sys
import threading
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock
from urllib.error import HTTPError
from urllib.parse import parse_qs, urlencode, urlsplit
from urllib.request import urlopen, Request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from fh2mp_launcher import itch, updater


class BrowserOAuth(unittest.TestCase):
    def callback(self, login, params, expected=200, host=None, path=None):
        request = Request(login.redirect_uri + (path or '') + '?' + urlencode(params, doseq=True), headers={'Host': host} if host else {})
        try:
            with urlopen(request, timeout=3) as response:
                self.assertEqual(response.status, expected)
                self.assertEqual(response.headers['Cache-Control'], 'no-store')
                self.assertNotIn('test-auth-code', response.read().decode())
        except HTTPError as e:
            with e:
                self.assertEqual(e.code, expected)

    def client_thread(self, fn):
        errors = queue.Queue()
        def work():
            try:
                fn()
            except BaseException as e:
                errors.put(e)
        thread = threading.Thread(target=work)
        thread.start()
        self.addCleanup(lambda: thread.join(timeout=4))
        return thread, errors

    def test_fresh_pkce_state_and_callback_checks_before_accepting_code(self):
        login = itch.BrowserLogin(itch.CLIENT_ID)
        self.addCleanup(login.close)
        params = parse_qs(urlsplit(login.url).query)
        self.assertEqual(params['client_id'], [itch.CLIENT_ID])
        self.assertEqual(params['response_type'], ['code'])
        self.assertEqual(params['code_challenge_method'], ['S256'])
        challenge = base64.urlsafe_b64encode(hashlib.sha256(login.verifier.encode()).digest()).rstrip(b'=').decode()
        self.assertEqual(params['code_challenge'], [challenge])
        self.assertNotIn(login.verifier, login.url)
        self.assertEqual(params['redirect_uri'], [login.redirect_uri])
        def send():
            self.callback(login, {'state': 'wrong', 'code': 'evil'}, 400)
            self.callback(login, {'state': 'é', 'code': 'evil'}, 400)
            self.callback(login, {'state': login.state, 'code': 'evil'}, 400, host='attacker.invalid')
            self.callback(login, {'state': login.state, 'code': 'evil'}, 400, path='/other')
            self.callback(login, {'state': login.state, 'code': ['one', 'two']}, 400)
            self.callback(login, {'state': login.state, 'code': 'test-auth-code'})
        thread, errors = self.client_thread(send)
        self.assertEqual(login.wait(timeout=5), 'test-auth-code')
        thread.join(timeout=4)
        if not errors.empty():
            raise errors.get()

    def test_decline_does_not_return_a_code(self):
        login = itch.BrowserLogin(itch.CLIENT_ID)
        self.addCleanup(login.close)
        thread, errors = self.client_thread(lambda: self.callback(login, {'state': login.state, 'error': 'access_denied'}))
        with self.assertRaisesRegex(updater.UpdateError, 'declined'):
            login.wait(timeout=5)
        thread.join(timeout=4)
        if not errors.empty():
            raise errors.get()
        self.assertIsNone(login.code)

    def test_cancellation_and_timeout_do_not_hang(self):
        login = itch.BrowserLogin(itch.CLIENT_ID)
        self.addCleanup(login.close)
        with self.assertRaisesRegex(updater.UpdateError, 'cancelled'):
            login.wait(cancelled=lambda: True)
        with self.assertRaisesRegex(updater.UpdateError, 'timed out'):
            login.wait(timeout=0)

    def test_busy_registered_port_uses_another_loopback_port(self):
        import socket
        listener = socket.socket()
        self.addCleanup(listener.close)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', 34567))
        listener.listen()
        login = itch.BrowserLogin(itch.CLIENT_ID)
        self.addCleanup(login.close)
        self.assertNotEqual(login.server.server_port, 34567)
        self.assertEqual(urlsplit(login.redirect_uri).hostname, '127.0.0.1')
        self.assertEqual(urlsplit(login.redirect_uri).path, '/callback')

    def test_new_attempt_has_independent_state_and_verifier(self):
        first = itch.BrowserLogin(itch.CLIENT_ID)
        second = itch.BrowserLogin(itch.CLIENT_ID)
        self.addCleanup(first.close)
        self.addCleanup(second.close)
        self.assertNotEqual(first.state, second.state)
        self.assertNotEqual(first.verifier, second.verifier)


class ButlerLogin(unittest.TestCase):
    def butler(self):
        obj = itch.Butler.__new__(itch.Butler)
        obj.cancelled = lambda: False
        return obj

    def test_browser_code_exchanged_by_butler_with_the_same_verifier_and_redirect(self):
        obj = self.butler()
        obj.call = mock.Mock(side_effect=[{'profiles': []}, {'profile': {'id': 7}}])
        open_browser = mock.Mock()
        with mock.patch.object(itch, 'BrowserLogin') as factory:
            login = factory.return_value
            login.url = 'https://itch.io/user/oauth?test'
            login.verifier = 'private-verifier'
            login.redirect_uri = 'http://127.0.0.1:34567/callback'
            login.wait.return_value = 'one-time-code'
            self.assertEqual(obj.login(open_browser), {'id': 7})
            open_browser.assert_called_once_with({'url': login.url})
            obj.call.assert_called_with('Profile.LoginWithOAuthCode', {
                'code': 'one-time-code', 'codeVerifier': login.verifier,
                'redirectUri': login.redirect_uri, 'clientId': itch.CLIENT_ID,
            })
            login.close.assert_called_once()

    def test_saved_profile_does_not_open_browser(self):
        obj = self.butler()
        obj.call = mock.Mock(side_effect=[{'profiles': [{'id': 7}]}, {'profile': {'id': 7}}])
        browser = mock.Mock()
        self.assertEqual(obj.login(browser), {'id': 7})
        browser.assert_not_called()
        obj.call.assert_called_with('Profile.UseSavedLogin', {'profileId': 7})

    def test_exchange_failure_closes_listener(self):
        obj = self.butler()
        obj.call = mock.Mock(side_effect=[{'profiles': []}, updater.UpdateError('exchange failed')])
        with mock.patch.object(itch, 'BrowserLogin') as factory:
            login = factory.return_value
            with self.assertRaises(updater.UpdateError):
                obj.login(mock.Mock())
            login.close.assert_called_once()

    def test_download_with_upload_scope_never_requires_privileged_game_metadata(self):
        obj = self.butler()
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / 'azimuth'
            def call(method, params, **kwargs):
                if method == 'Fetch.GameUploads':
                    return {'uploads': [{'id': 42, 'filename': 'azimuth-windows.zip'}]}
                if method == 'Install.Queue':
                    self.assertEqual(params['game']['id'], itch.GAME_ID)
                    self.assertEqual(params['upload']['id'], 42)
                    self.assertTrue(params['ignoreInstallers'])
                    return {'stagingFolder': str(Path(tmp) / 'staging')}
                if method == 'Install.Perform':
                    (target / '.itch').mkdir(parents=True)
                    (target / 'azimuth.exe').write_bytes(b'MZ official-upload-fixture')
                    (target / '.itch/receipt.json').write_text('{}')
                    return {}
                raise AssertionError('The download token cannot call ' + method)
            obj.call = mock.Mock(side_effect=call)
            result = obj.download({'id': 7}, tmp)
            with zipfile.ZipFile(result) as z:
                self.assertEqual(z.namelist(), ['azimuth.exe'])
