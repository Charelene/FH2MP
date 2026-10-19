"""Client for the server's /v1 API (server/launcher_api.py). Standard library only; certificates are always verified."""
import json
import ssl
import urllib.error
import urllib.parse
import urllib.request

from . import identity as ident
from .constants import API_BASES, USER_AGENT
from .version import __version__


class ApiError(Exception):
    def __init__(self, status, code, message=""):
        super().__init__(message or code)
        self.status, self.code, self.message = status, code, message or code


def _is_local(host):
    return host in ("localhost", "127.0.0.1", "::1") or host.startswith("127.")


class Api:
    def __init__(self, base=None, channel="stable", allow_insecure=False, timeout=15.0):
        self.base = (base or API_BASES.get(channel, API_BASES["stable"])).rstrip("/")
        u = urllib.parse.urlparse(self.base)
        if u.scheme not in ("http", "https") or not u.hostname:
            raise ValueError(f"not a server address: {self.base!r}")
        if u.scheme == "http" and not (_is_local(u.hostname) or allow_insecure):
            raise ValueError("this server address is not encrypted (https): the sign-in would travel in the clear")
        self.timeout = timeout
        self.realm = None
        self._ctx = ssl.create_default_context()

    # ---- plumbing
    def _call(self, method, path, body=None, token=None, raw=False, ctype="application/json"):
        data = None
        if body is not None:
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
        req = urllib.request.Request(self.base + path, data=data, method=method)
        req.add_header("User-Agent", f"{USER_AGENT}/{__version__}")
        if data is not None:
            req.add_header("Content-Type", ctype)
        if token:
            req.add_header("Authorization", f"Bearer {token}")
        try:
            with urllib.request.urlopen(req, timeout=self.timeout, context=self._ctx) as r:
                payload = r.read(4_000_000)
        except urllib.error.HTTPError as e:
            payload = e.read(100_000)
            try:
                d = json.loads(payload)
                raise ApiError(e.code, d.get("error", "http_error"), d.get("message", "")) from None
            except ValueError:
                raise ApiError(e.code, "http_error", f"server answered {e.code}") from None
        except (urllib.error.URLError, OSError, ssl.SSLError) as e:
            reason = getattr(e, "reason", e)
            raise ApiError(0, "unreachable", f"cannot reach the server ({reason})") from None
        if raw:
            return payload
        try:
            return json.loads(payload or b"{}")
        except ValueError:
            raise ApiError(0, "bad_response", "the server sent something unexpected") from None

    # ---- public
    def status(self):
        return self._call("GET", "/v1/status")

    def config(self):
        c = self._call("GET", "/v1/launcher/config")
        self.realm = c.get("env")
        return c

    def check_name(self, name):
        return self._call("GET", "/v1/gamertags/check?name=" + urllib.parse.quote(name))

    def gamerpic(self, xuid):
        return self._call("GET", f"/v1/gamerpic/{int(xuid)}.png", raw=True)

    # ---- sign up / sign in
    def _challenge(self):
        ch = self._call("POST", "/v1/auth/challenge", {})
        self.realm = ch["realm"]
        return ch, ident.unb64u(ch["challenge"])

    def register(self, identity, gamertag, invite="", progress=None, cancelled=lambda: False):
        ch, raw = self._challenge()
        counter = ident.solve_pow(raw, identity.pubkey, ch["powBits"], progress, cancelled)
        sig = identity.sign(self.realm, "register", raw, identity.pubkey, gamertag.encode())
        body = {"challenge": ch["challenge"], "pubkey": identity.pubkey_b64, "gamertag": gamertag, "pow": counter, "sig": ident.b64u(sig)}
        if invite:
            body["invite"] = invite
        return self._call("POST", "/v1/auth/register", body)["account"]

    def login(self, identity, client=""):
        """-> dict(token, account, lsp, game, idleSeconds). ApiError(404, 'unknown_account') when this key has no account yet."""
        ch, raw = self._challenge()
        sig = identity.sign(self.realm, "login", raw, identity.pubkey)
        return self._call("POST", "/v1/auth/login", {"challenge": ch["challenge"], "pubkey": identity.pubkey_b64, "sig": ident.b64u(sig),
                                                      "client": client})

    # ---- signed in
    def me(self, token):
        return self._call("GET", "/v1/me", token=token)

    def rename(self, token, gamertag):
        return self._call("PATCH", "/v1/me", {"gamertag": gamertag}, token=token)

    def upload_gamerpic(self, token, png):
        return self._call("PUT", "/v1/me/gamerpic", png, token=token, ctype="image/png")

    def clear_gamerpic(self, token):
        return self._call("DELETE", "/v1/me/gamerpic", token=token)

    def logout(self, token):
        try:
            self._call("POST", "/v1/auth/logout", {}, token=token)
        except ApiError:
            pass
