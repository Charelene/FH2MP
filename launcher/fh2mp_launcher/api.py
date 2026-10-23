"""Client for the server's /v1 API (server/launcher_api.py). Standard library only; certificates are always verified."""
import datetime
import email.utils
import http.client
import json
import os
import socket
import ssl
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

from . import identity as ident
from . import paths
from .constants import API_BASES, USER_AGENT
from .version import __version__


class ApiError(Exception):
    def __init__(self, status, code, message="", detail=""):
        super().__init__(message or code)
        self.status, self.code, self.message = status, code, message or code
        self.detail = detail          # for "unreachable": why, in plain words (shown to the player and written to the log)
        self.uncertain = False        # a request that was sent but whose answer never arrived: the server may have carried it out


def describe_error(exc):
    """A short plain-words reason for a failed connection (the exception is whatever urllib raised)."""
    reason = getattr(exc, "reason", exc)
    text = str(reason)
    if isinstance(reason, ssl.SSLCertVerificationError):
        return "the server's certificate was not trusted: " + (getattr(reason, "verify_message", "") or text)
    if isinstance(reason, ssl.SSLError):
        return "the secure connection failed: " + text
    if isinstance(reason, (socket.timeout, TimeoutError)) or "timed out" in text:
        return "timed out"
    if isinstance(reason, socket.gaierror):
        return "name lookup failed: " + text
    if isinstance(reason, ConnectionRefusedError):
        return "connection refused"
    if isinstance(reason, ConnectionResetError):
        return "connection reset"
    return f"{type(reason).__name__}: {text}"


def _proxy_host():
    """The system proxy urllib will use for https, host only (never credentials); '' when there is none."""
    try:
        p = urllib.request.getproxies().get("https") or urllib.request.getproxies().get("http") or ""
        return urllib.parse.urlparse(p if "//" in p else "//" + p).hostname or "" if p else ""
    except Exception:  # noqa: BLE001
        return ""


def log_failure(method, path, exc, detail):
    """One line per failed call in <cache>/launcher.log, so a player can send it when asked. Best effort, never raises, no secrets."""
    try:
        f = os.path.join(paths.cache_dir(), "launcher.log")
        if os.path.exists(f) and os.path.getsize(f) > 200_000:
            os.replace(f, f + ".1")
        line = (f"{time.strftime('%Y-%m-%d %H:%M:%S')} {method} {path.split('?')[0]} failed: {detail} [{type(exc).__name__}] "
                f"proxy={_proxy_host() or 'none'} {ssl.OPENSSL_VERSION} python={sys.version.split()[0]} launcher={__version__}\n")
        with open(f, "a", encoding="utf-8") as fh:
            fh.write(line)
    except Exception:  # noqa: BLE001
        pass


def log_note(text):
    """One informational line in <cache>/launcher.log (what the launcher did before starting the game). Best effort, never raises."""
    try:
        f = os.path.join(paths.cache_dir(), "launcher.log")
        if os.path.exists(f) and os.path.getsize(f) > 200_000:
            os.replace(f, f + ".1")
        with open(f, "a", encoding="utf-8") as fh:
            fh.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {text} launcher={__version__}\n")
    except Exception:  # noqa: BLE001
        pass


_USE_BUNDLED = False      # set once the Windows certificate store failed and the bundled roots worked: later calls start with those


def _bundled_context():
    """TLS context that trusts only the bundled root certificates (certifi). The Windows store is the better default, but it can hold
    stale entries (an expired Let's Encrypt cross-sign, say) that make OpenSSL reject a perfectly good server certificate."""
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except Exception:  # noqa: BLE001
        return None


def clock_hint(base):
    """'' or a sentence about the computer's clock. A certificate that 'has expired' is nearly always a wrong clock; the server's plain-HTTP
    redirect carries a Date header, which needs no certificate, so compare it with the local time."""
    try:
        u = urllib.parse.urlparse(base)
        c = http.client.HTTPConnection(u.hostname, 80 if u.scheme == "https" else (u.port or 80), timeout=6)
        c._create_connection = _create_connection_v4
        c.request("HEAD", "/")
        stamp = c.getresponse().getheader("Date")
        c.close()
        server = email.utils.parsedate_to_datetime(stamp)
        now = datetime.datetime.now(datetime.timezone.utc)
        skew = (now - server).total_seconds()
        if abs(skew) < 600:
            return ""
        mins = int(abs(skew) // 60)
        span = f"{mins // 1440} days" if mins >= 2880 else f"{mins // 60} hours" if mins >= 120 else f"{mins} minutes"
        return (f"; your computer's clock is about {span} {'ahead of' if skew > 0 else 'behind'} the server's "
                f"(it says {now:%Y-%m-%d %H:%M} UTC, the server {server:%Y-%m-%d %H:%M} UTC): set the date and time automatically")
    except Exception:  # noqa: BLE001
        return ""


def presented_certificate(base):
    """'' or a sentence saying who issued the certificate that actually answers at the server's address and when it is valid, read without
    trusting it. Tells a stale Windows certificate store (Let's Encrypt, valid) from something in the way (a VPN or security suite
    presenting its own certificate). Only used to explain a certificate error."""
    try:
        u = urllib.parse.urlparse(base)
        host, port = u.hostname, u.port or 443
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        with _create_connection_v4((host, port), 6) as raw, ctx.wrap_socket(raw, server_hostname=host) as s:
            der = s.getpeercert(True)
        from cryptography import x509
        from cryptography.x509.oid import NameOID
        c = x509.load_der_x509_certificate(der)
        who = " / ".join(a.value for oid in (NameOID.ORGANIZATION_NAME, NameOID.COMMON_NAME) for a in c.issuer.get_attributes_for_oid(oid)) or "unknown"
        start = getattr(c, "not_valid_before_utc", None) or c.not_valid_before
        end = getattr(c, "not_valid_after_utc", None) or c.not_valid_after
        return f"; the certificate shown to this computer is issued by {who}, valid {start:%Y-%m-%d} to {end:%Y-%m-%d}"
    except Exception:  # noqa: BLE001
        return ""


def _create_connection_v4(address, timeout=socket._GLOBAL_DEFAULT_TIMEOUT, source_address=None):
    """socket.create_connection over IPv4 only. The server publishes no IPv6 address, and a VPN or adapter that half-supports IPv6 can
    stall or break a connection that tries it first."""
    host, port = address
    err = None
    for fam, typ, proto, _canon, sa in socket.getaddrinfo(host, port, socket.AF_INET, socket.SOCK_STREAM):
        s = None
        try:
            s = socket.socket(fam, typ, proto)
            if timeout is not socket._GLOBAL_DEFAULT_TIMEOUT:
                s.settimeout(timeout)
            if source_address:
                s.bind(source_address)
            s.connect(sa)
            return s
        except OSError as e:
            err = e
            if s is not None:
                s.close()
    raise err or OSError("no IPv4 address for " + str(host))


class _V4HTTPSHandler(urllib.request.HTTPSHandler):
    """HTTPS, certificates verified as usual, connecting over IPv4 only."""

    def https_open(self, req):
        return self.do_open(self._connection, req, context=self._context)

    @staticmethod
    def _connection(host, **kw):
        c = http.client.HTTPSConnection(host, **kw)
        c._create_connection = _create_connection_v4
        return c


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
    def _opener(self, use_proxy, ctx=None):
        """IPv4-only HTTPS; the system proxy only when asked for."""
        return urllib.request.build_opener(urllib.request.ProxyHandler(None if use_proxy else {}), _V4HTTPSHandler(context=ctx or self._ctx))

    def _call(self, method, path, body=None, token=None, raw=False, ctype="application/json", limit=4_000_000):
        data = None
        if body is not None:
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
        def make_request():                 # a fresh one per attempt: going through a proxy rewrites the request in place
            req = urllib.request.Request(path if path.startswith("https://") else self.base + path, data=data, method=method)
            req.add_header("User-Agent", f"{USER_AGENT}/{__version__}")
            if data is not None:
                req.add_header("Content-Type", ctype)
            if token:
                req.add_header("Authorization", f"Bearer {token}")
            return req
        # A VPN client or security suite often sets a system proxy; if the connection through it fails, try once without it (a VPN
        # should not be able to break the launcher). The failures are all logged, the last one is what the player sees.
        # If the Windows certificate store makes the server's certificate fail, try once with the bundled root certificates (see
        # _bundled_context); a wrong clock fails both, and then the message says so.
        global _USE_BUNDLED
        proxy = _proxy_host()
        use_bundled = _USE_BUNDLED and _bundled_context() is not None
        fell_back = False
        for _round in (0, 1):
            ctx = _bundled_context() if use_bundled else self._ctx
            details, last = [], None
            for use_proxy in ([True, False] if proxy else [False]):
                try:
                    with self._opener(use_proxy, ctx).open(make_request(), timeout=self.timeout) as r:
                        payload = r.read(limit)
                    break
                except urllib.error.HTTPError as e:
                    payload = e.read(100_000)
                    try:
                        d = json.loads(payload)
                        raise ApiError(e.code, d.get("error", "http_error"), d.get("message", "")) from None
                    except ValueError:
                        raise ApiError(e.code, "http_error", f"server answered {e.code}") from None
                except (urllib.error.URLError, OSError, ssl.SSLError) as e:
                    d = describe_error(e) + (f" (through the proxy {proxy})" if use_proxy else "")
                    log_failure(method, path, e, d + (" [bundled roots]" if use_bundled else ""))
                    details.append(d)
                    last = e
            else:
                cert_error = isinstance(getattr(last, "reason", last), ssl.SSLCertVerificationError)
                if cert_error and not use_bundled and _bundled_context() is not None:
                    use_bundled = fell_back = True
                    continue
                detail = "; then without the proxy: ".join(details)
                if cert_error:
                    detail += clock_hint(self.base) + presented_certificate(self.base)
                raise ApiError(0, "unreachable", f"cannot reach the server ({detail})", detail)
            if fell_back:
                _USE_BUNDLED = True             # the system store was the problem: start with the bundled roots from now on
            break
        if raw:
            return payload
        try:
            return json.loads(payload or b"{}")
        except ValueError:
            log_failure(method, path, ValueError("not JSON"), "the answer was not understood")
            raise ApiError(0, "bad_response", "the server sent something unexpected") from None

    # ---- public
    def status(self):
        return self._call("GET", "/v1/status")

    def config(self):
        c = self._call("GET", "/v1/launcher/config")
        self.realm = c.get("env")
        return c

    def home(self):
        return self._call("GET", "/v1/home")

    def asset(self, name):
        return self._call("GET", "/v1/launcher/assets/" + urllib.parse.quote(name), raw=True)

    def challenges(self, top=3):
        return self._call("GET", f"/v1/challenges?top={int(top)}")

    def leaderboards(self, limit=10):
        return self._call("GET", f"/v1/leaderboards?limit={int(limit)}")

    def get_json(self, url):
        """JSON from another https address (the website's public stats)."""
        if not url.startswith("https://"):
            raise ApiError(0, "bad_url", "only https addresses are read")
        return self._call("GET", url, limit=200_000)

    def image(self, url, limit=4_000_000):
        """A picture the server's config names (hero, news cards): https only, at most `limit` bytes."""
        if not url.startswith("https://"):
            raise ApiError(0, "bad_url", "pictures are only fetched over https")
        return self._call("GET", url, raw=True, limit=limit)

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
        try:
            return self._call("POST", "/v1/auth/register", body)["account"]
        except ApiError as e:
            if e.code in ("unreachable", "bad_response"):
                e.uncertain = True          # the request was sent: the account may exist even though the answer did not arrive
            raise

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

    def gamedb(self, token):
        """-> {name, sha256, size, xzSize, path} of the car database the server plays with, or None when it sets none."""
        try:
            return self._call("GET", "/v1/gamedb", token=token)
        except ApiError as e:
            if e.code == "no_gamedb" or e.status == 404:          # 404: a server from before car-database sync sets none
                return None
            raise

    def gamedb_download(self, token, path, limit):
        return self._call("GET", path, token=token, raw=True, limit=limit)

    def logout(self, token):
        try:
            self._call("POST", "/v1/auth/logout", {}, token=token)
        except ApiError:
            pass
