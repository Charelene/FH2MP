"""Private stdio connection to itch.io's official butlerd. Credentials stay in its private database."""
import hashlib
import base64
import hmac
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
import platform
import queue
import secrets
import subprocess
import sys
import threading
import time
import uuid
import zipfile
from urllib.parse import parse_qs, urlencode, urlsplit

from . import buildcfg, launch, paths, updater

VERSION = '15.32.0'
GAME_ID = 5100516
CLIENT_ID = '02819c7d7be9395c90106e73a9ce878a'
PINS = {
    'linux': (10599491, '5db7b053fc9b578b86ffaf3520a12b952eb95c9225171cc5720dfbc9fe4574b3'),
    'windows': (10682772, '3b5b0ecbdc881fb5394a9094bce13dfcfc602e9c586224b44adc9f859eaf1222'),
}


def client_id():
    return os.environ.get('FH2MP_ITCH_CLIENT_ID') or buildcfg.get('itch_client_id') or CLIENT_ID


class BrowserLogin:
    """One short-lived loopback callback, with fresh PKCE and state for every attempt."""
    def __init__(self, client):
        self.verifier = secrets.token_urlsafe(64)
        self.state = secrets.token_urlsafe(32)
        self.code = None
        self.denied = False
        owner = self

        class Handler(BaseHTTPRequestHandler):
            def setup(self):
                super().setup()
                self.connection.settimeout(2)

            def log_message(self, *args):
                pass  # Never log callback URLs: they contain authorization codes.

            def do_GET(self):
                parsed = urlsplit(self.path)
                try:
                    query = parse_qs(parsed.query, keep_blank_values=True, max_num_fields=20)
                except ValueError:
                    query = {}
                state = query.get('state', [])
                valid = (len(self.path) <= 8192 and parsed.path == '/callback'
                         and self.headers.get('Host') == f'127.0.0.1:{owner.server.server_port}'
                         and len(state) == 1 and hmac.compare_digest(state[0].encode(), owner.state.encode()))
                code, error = query.get('code', []), query.get('error', [])
                if valid and len(code) == 1 and code[0] and not error:
                    owner.code = code[0]
                elif valid and len(error) == 1 and error[0] and not code:
                    owner.denied = True
                else:
                    valid = False
                text = 'You can return to FH2MP.' if valid else 'Invalid sign-in callback. Return to the itch.io sign-in page.'
                body = text.encode()
                self.send_response(200 if valid else 400)
                self.send_header('Content-Type', 'text/plain; charset=utf-8')
                self.send_header('Content-Length', str(len(body)))
                self.send_header('Cache-Control', 'no-store')
                self.send_header('Referrer-Policy', 'no-referrer')
                self.send_header('Content-Security-Policy', "default-src 'none'")
                self.end_headers()
                self.wfile.write(body)

        try:
            self.server = HTTPServer(('127.0.0.1', 34567), Handler)
        except OSError:
            self.server = HTTPServer(('127.0.0.1', 0), Handler)
        self.server.timeout = .2
        self.redirect_uri = f'http://127.0.0.1:{self.server.server_port}/callback'
        challenge = base64.urlsafe_b64encode(hashlib.sha256(self.verifier.encode('ascii')).digest()).rstrip(b'=').decode('ascii')
        self.url = 'https://itch.io/user/oauth?' + urlencode({
            'client_id': client, 'redirect_uri': self.redirect_uri, 'response_type': 'code',
            'scope': 'profile:me profile:owned game:view:uploads', 'state': self.state,
            'code_challenge': challenge, 'code_challenge_method': 'S256',
        })

    def wait(self, cancelled=lambda: False, timeout=300):
        deadline = time.monotonic() + timeout
        while not self.code and not self.denied:
            if cancelled():
                raise updater.UpdateError('Sign-in cancelled.')
            if time.monotonic() >= deadline:
                raise updater.UpdateError('itch.io sign-in timed out. Select Login with itch.io to try again.')
            self.server.handle_request()
        if self.denied:
            raise updater.UpdateError('itch.io sign-in was declined. You can try again or use the manual ZIP option.')
        return self.code

    def close(self):
        self.server.server_close()


def binary(progress=None, cancelled=lambda: False):
    system = 'windows' if sys.platform == 'win32' else 'linux'
    if platform.machine().lower() not in ('x86_64', 'amd64') or sys.platform not in ('win32', 'linux'):
        raise ValueError('Automatic itch downloads require Windows or Linux x64. Use the manual ZIP option.')
    name = f'butler-{system}-amd64.zip'
    size, digest = PINS[system]
    archive = updater.Updater().download_url(f'https://github.com/itchio/butler/releases/download/v{VERSION}/{name}',
                                            dict(name=name, size=size, sha256=digest), progress, cancelled)
    folder = os.path.join(paths.cache_dir(), 'butler', VERSION)
    os.makedirs(folder, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            leaf = info.filename.split('/')[-1]
            if leaf not in ('butler', 'butler.exe', '7z.so', 'libc7zip.so', '7z.dll', 'c7zip.dll'):
                raise ValueError('Unexpected file in the butler package.')
            data = z.read(info)
            target = os.path.join(folder, leaf)
            if not os.path.isfile(target) or az_hash(target) != hashlib.sha256(data).hexdigest():
                paths.atomic_write(target, data, 0o700)
    return os.path.join(folder, 'butler.exe' if system == 'windows' else 'butler')


def az_hash(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


class Butler:
    def __init__(self, progress=None, cancelled=lambda: False):
        self.cancelled = cancelled
        self._request_id = 0
        folder = os.path.join(paths.config_dir(), 'itch')
        os.makedirs(folder, mode=0o700, exist_ok=True)
        if sys.platform != 'win32':
            os.chmod(folder, 0o700)
        exe = binary(progress, cancelled)
        self.process = subprocess.Popen([exe, '--json', '--dbpath', os.path.join(folder, 'butler.db'),
                                         'daemon', '--transport', 'stdio'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, env=launch.host_env(),
                                        **({'creationflags': subprocess.CREATE_NO_WINDOW} if sys.platform == 'win32' else {}))
        self.messages = queue.Queue()
        def read():
            try:
                for line in self.process.stdout:
                    self.messages.put(json.loads(line))
            finally:
                self.messages.put(None)
        threading.Thread(target=read, daemon=True).start()

    def _send(self, value):
        self.process.stdin.write(json.dumps(value).encode() + b'\n')
        self.process.stdin.flush()

    def call(self, method, params, progress=None, timeout=900):
        self._request_id += 1
        ident = self._request_id
        self._send(dict(jsonrpc='2.0', id=ident, method=method, params=params))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.cancelled():
                self.close()
                raise updater.UpdateError('Setup cancelled.')
            try:
                msg = self.messages.get(timeout=.2)
            except queue.Empty:
                continue
            if msg is None:
                raise updater.UpdateError('itch downloader closed unexpectedly. Retry or use the manual ZIP option.')
            if msg.get('id') == ident and ('result' in msg or 'error' in msg):
                if msg.get('error'):
                    # Avoid logging raw daemon replies; they may contain credentials or signed URLs.
                    raise updater.UpdateError(f'itch.io could not complete {method}. Retry or use the manual ZIP option.')
                return msg.get('result') or {}
            name, value = msg.get('method', ''), msg.get('params') or {}
            if name == 'Progress' and progress:
                progress(int(value.get('progress', 0) * 100), 100)
            if 'id' in msg and name:
                self._send(dict(jsonrpc='2.0', id=msg['id'], error={'code': -32601, 'message': 'Unsupported request'}))
        self.close()
        raise updater.UpdateError('itch.io timed out. Retry or use the manual ZIP option.')

    def login(self, challenge):
        profiles = self.call('Profile.List', {}).get('profiles') or []
        if profiles:
            return self.call('Profile.UseSavedLogin', {'profileId': profiles[0]['id']})['profile']
        if not client_id():
            raise ValueError('itch.io browser login is not configured in this launcher build. Use the manual ZIP option.')
        login = BrowserLogin(client_id())
        try:
            challenge({'url': login.url})
            code = login.wait(self.cancelled)
            return self.call('Profile.LoginWithOAuthCode', {
                'code': code, 'codeVerifier': login.verifier,
                'redirectUri': login.redirect_uri, 'clientId': client_id(),
            })['profile']
        finally:
            login.close()

    def download(self, profile, destination, progress=None):
        # The scoped download token can list/download uploads, but cannot use
        # itch's privileged game-metadata endpoint. This launcher's target is fixed.
        game = {'id': GAME_ID, 'title': 'Azimuth', 'url': 'https://almost-there-games.itch.io/azimuth', 'type': 'default'}
        uploads = self.call('Fetch.GameUploads', {'gameId': GAME_ID, 'fresh': True}).get('uploads') or []
        uploads = [u for u in uploads if u.get('filename') == 'azimuth-windows.zip']
        if len(uploads) != 1:
            raise ValueError('The official Azimuth Windows ZIP was not found on itch.io. Use the manual download option.')
        staging = os.path.join(destination, 'staging')
        target = os.path.join(destination, 'azimuth')
        item = self.call('Install.Queue', {'game': game, 'upload': uploads[0], 'profileId': profile['id'],
                         'noCave': True, 'installFolder': target, 'stagingFolder': staging, 'ignoreInstallers': True})
        self.call('Install.Perform', {'id': uuid.uuid4().hex, 'stagingFolder': item['stagingFolder']}, progress=progress)
        # Repackage the downloaded files for the same validation/installation path as manual ZIPs.
        archive = os.path.join(destination, 'azimuth.zip')
        with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
            for root, dirs, files in os.walk(target, followlinks=False):
                if any(os.path.islink(os.path.join(root, n)) for n in dirs + files):
                    raise ValueError('Unexpected symbolic link in the Azimuth download.')
                if root == target:
                    dirs[:] = [name for name in dirs if name != '.itch']
                for name in files:
                    path = os.path.join(root, name)
                    z.write(path, os.path.relpath(path, target))
        return archive

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3)
        for stream in (self.process.stdin, self.process.stdout):
            if stream:
                stream.close()
