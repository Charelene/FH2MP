"""The public `/v1/` API the player's launcher talks to: server status, launcher configuration, sign up / sign in, profile and gamerpic.

HTTP-free like auth.py: handle() takes the request pieces and returns (status, JSON-able object | bytes, content type), so it is tested
without sockets. The LSP maps its HTTP handler onto it (fh2mp_lsp.py, `_v1`).

    GET    /v1/status                 {env, online, auth, registration, time}                                   (public)
    GET    /v1/launcher/config        links, motd, news cards, hero image, minimum launcher, supported Azimuth builds (public)
    GET    /v1/challenges[?top=3]     this month's Monthly Rivals events and their best real times               (public, cached)
    GET    /v1/leaderboards[?limit=10] XP ladder and one board per Monthly Rivals event, real players only       (public, cached)
    GET    /v1/home                   everything the launcher's home page shows (home.py)                       (public, cached)
    GET    /v1/launcher/assets/<file> a picture or sound the operator put in FH2MP_LAUNCHER_ASSETS               (public)
    POST   /v1/auth/challenge         {challenge, powBits, ttl}                                                  (public)
    POST   /v1/auth/register          {challenge, pubkey, gamertag, pow, sig[, invite]} -> account               (public, signed)
    POST   /v1/auth/login             {challenge, pubkey, sig[, client]} -> {token, account, ...}                (public, signed)
    POST   /v1/auth/logout                                                                                       (Bearer)
    GET    /v1/gamertags/check?name=  {available, valid, reason}                                                 (public)
    GET    /v1/me                     the account                                                                (Bearer)
    PATCH  /v1/me                     {"gamertag": "..."}  (once a week)                                         (Bearer)
    PUT    /v1/me/gamerpic            body: PNG, at most 512x512 and 1 MB; it is decoded and re-encoded here    (Bearer)
    DELETE /v1/me/gamerpic            back to the generated picture                                              (Bearer)
    GET    /v1/gamerpic/<xuid>.png    anybody's picture                                                          (public)
    GET    /v1/gamedb                 {name, sha256, size, xzSize, path} of the car database to play with; 404 none (Bearer)
    GET    /v1/gamedb/gamedbRC.slt.xz that database, xz-compressed (gamedb_sync.py)                               (Bearer)

Errors: {"error": "<code>", "message": "..."} with the HTTP status of auth.AuthError.
"""
import json
import os
import re
import struct
import time
from urllib.parse import parse_qs, urlparse

import auth
import gamedb_sync
import pngtool
import social

MAX_BODY = 1_200_000
GAMERPIC_MAX_DIM = 512
GAMERPIC_MIN_DIM = 32
SERVER_VERSION = "1"

DEFAULT_CONFIG = {
    "motd": "",
    "links": {"website": "https://forza.scuffed.af", "patreon": "", "discord": ""},
    "minLauncher": "0.0.0",
    "minShim": "0.0.0",             # oldest shim (X-FH2MP-Shim) the web services answer; older ones are refused with a message
    "latestAzimuth": "0.9.10",
    # builds of azimuth.exe the shim's byte signatures were checked against: {"version", "sha256"}
    "supportedAzimuth": [],
    "maintenance": "",
    "heroImage": "",                # https URL of the launcher's home picture; empty = the drawn background
    # launcher home cards: {"tag": "PATCH NOTES", "title", "text", "url", "image"} (ops_api.validate_config checks them)
    "news": [],
}
ASSET_TYPES = {".png": "image/png", ".jpg": "image/jpeg", ".webp": "image/webp", ".wav": "audio/wav"}
ASSET_MAX = 4_000_000
BOARDS_TTL = 60                     # seconds a /v1/challenges or /v1/leaderboards answer is reused: every launcher asks on refresh


_VER_RE = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)(?:-rc(\d+))?(-debug)?$")


def version_at_least(have, need):
    """`have` ("1.0.0-rc3", "1.0.0", "1.0.0-rc4-debug") is the same as or later than `need`. A pre-release sorts before its release; our own
    debug builds always pass; an empty or unreadable `have` fails unless nothing is required (need "0.0.0" or empty)."""
    n = _VER_RE.match((need or "").strip())
    if not n or n.groups()[:3] == ("0", "0", "0"):
        return True
    h = _VER_RE.match((have or "").strip())
    if not h:
        return False
    if h.group(5):
        return True

    def key(m):
        return (int(m[1]), int(m[2]), int(m[3]), 0 if m[4] is not None else 1, int(m[4] or 0))
    return key(h) >= key(n)


class LauncherConfig:
    """launcher.json (path in FH2MP_LAUNCHER_CONFIG) merged over the defaults; re-read when the file changes, so the operator can set the
    Patreon link or a message of the day without restarting anything."""

    def __init__(self, path=None):
        self.path = path or os.environ.get("FH2MP_LAUNCHER_CONFIG", "")
        self._mtime = None
        self._data = dict(DEFAULT_CONFIG)
        self.overlay = {}                 # set at run time through the ops API (ops_api.py); wins over the file

    def get(self):
        data = self._file()
        if not self.overlay:
            return data
        merged = {**data, **{k: v for k, v in self.overlay.items() if k in DEFAULT_CONFIG and k != "links"}}
        merged["links"] = {**data["links"], **(self.overlay.get("links") or {})}
        return merged

    def _file(self):
        if self.path:
            try:
                m = os.stat(self.path).st_mtime
                if m != self._mtime:
                    with open(self.path, encoding="utf-8") as f:
                        loaded = json.load(f)
                    data = dict(DEFAULT_CONFIG)
                    data.update({k: v for k, v in loaded.items() if k in DEFAULT_CONFIG})
                    data["links"] = {**DEFAULT_CONFIG["links"], **(loaded.get("links") or {})}
                    self._data, self._mtime = data, m
            except (OSError, ValueError):
                pass                      # keep the last good copy
        return self._data


class Ctx:
    def __init__(self, auth_service, social_store, config, online_count, log=lambda m: None, on_login=None, lsp_public=None, game_public=None,
                 gamedb=None, boards=None, assets_dir=None):
        self.auth, self.social, self.config, self.online_count, self.log = auth_service, social_store, config, online_count, log
        self.on_login = on_login or (lambda xuid, ip: None)
        self.lsp_public, self.game_public = lsp_public or {}, game_public or {}
        self.gamedb = gamedb or gamedb_sync.GameDb("")
        self.boards = boards              # (kind "challenges" | "leaderboards", n) -> dict, from ops_api; None = not served
        self._boards_cache = {}
        self.assets_dir = os.environ.get("FH2MP_LAUNCHER_ASSETS", "") if assets_dir is None else assets_dir


def _asset(ctx, name):
    """A file from the operator's launcher assets folder (hero and card pictures, sounds); only plain names, known types, 4 MB."""
    ext = os.path.splitext(name)[1].lower()
    if not ctx.assets_dir or not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,63}\.[a-z0-9]{3,4}", name) or ext not in ASSET_TYPES:
        raise auth.AuthError(404, "not_found")
    try:
        with open(os.path.join(ctx.assets_dir, name), "rb") as f:
            data = f.read(ASSET_MAX + 1)
    except OSError:
        raise auth.AuthError(404, "not_found") from None
    if len(data) > ASSET_MAX:
        raise auth.AuthError(404, "not_found")
    return data, ASSET_TYPES[ext]


def _boards(ctx, kind, n):
    if ctx.boards is None:
        raise auth.AuthError(404, "not_found", "no leaderboards on this server")
    hit = ctx._boards_cache.get((kind, n))
    if hit and time.time() - hit[0] < BOARDS_TTL:
        return hit[1]
    obj = ctx.boards(kind, n)
    ctx._boards_cache[(kind, n)] = (time.time(), obj)
    return obj


def _qint(u, key, default, lo, hi):
    try:
        return max(lo, min(hi, int((parse_qs(u.query).get(key) or [default])[0])))
    except ValueError:
        return default


def _json(body):
    if len(body) > 64 * 1024:
        raise auth.AuthError(413, "too_large")
    try:
        d = json.loads(body or b"{}")
    except ValueError:
        raise auth.AuthError(400, "bad_request", "invalid JSON")
    if not isinstance(d, dict):
        raise auth.AuthError(400, "bad_request", "expected a JSON object")
    return d


def _account_json(a, extra=None):
    d = {"xuid": a.xuid, "xuidHex": f"{a.xuid:#018x}", "gamertag": a.gamertag}
    d.update(extra or {})
    return d


def _bearer(headers):
    h = headers.get("Authorization", "")
    return h[7:].strip() if h[:7].lower() == "bearer " else ""


def clean_gamerpic(png):
    """Validates an uploaded PNG and returns a freshly encoded one (the original bytes never reach other players' game clients)."""
    if png[:8] != b"\x89PNG\r\n\x1a\n" or len(png) > social.MAX_GAMERPIC:
        raise auth.AuthError(400, "bad_image", "send a PNG of at most 1 MB")
    if png[12:16] != b"IHDR":
        raise auth.AuthError(400, "bad_image", "that PNG cannot be read")
    w0, h0 = struct.unpack(">II", png[16:24])                      # checked before inflating, so a tiny file cannot claim gigabytes
    if not (GAMERPIC_MIN_DIM <= w0 <= GAMERPIC_MAX_DIM and GAMERPIC_MIN_DIM <= h0 <= GAMERPIC_MAX_DIM):
        raise auth.AuthError(400, "bad_image", f"the picture must be between {GAMERPIC_MIN_DIM} and {GAMERPIC_MAX_DIM} pixels each way")
    try:
        w, h, ch, rows = pngtool.decode(png, max_dim=GAMERPIC_MAX_DIM)
    except Exception:  # noqa: BLE001 - any malformed input
        raise auth.AuthError(400, "bad_image", "that PNG cannot be read (8-bit, not interlaced, please)")
    if not (GAMERPIC_MIN_DIM <= w <= GAMERPIC_MAX_DIM and GAMERPIC_MIN_DIM <= h <= GAMERPIC_MAX_DIM):
        raise auth.AuthError(400, "bad_image", f"the picture must be between {GAMERPIC_MIN_DIM} and {GAMERPIC_MAX_DIM} pixels each way")
    return pngtool.encode(w, h, ch, rows)


def _require_launcher(ctx, client):
    """Launchers announce themselves as "launcher/<version> <os>" at sign-in; one older than minLauncher is refused with a message it shows.
    Other clients (test tools) are not held to it."""
    m = re.match(r"^launcher/(\S+)", client)
    c = ctx.config.get()
    if m and not version_at_least(m.group(1), c["minLauncher"]):
        site = (c["links"] or {}).get("website") or ""
        raise auth.AuthError(426, "launcher_too_old", f"This launcher ({m.group(1)}) is out of date. Download the latest FH2MP launcher"
                             + (f" from {site}" if site else "") + ".")


def handle(ctx, method, path, headers, body, ip):
    """-> (status, object or bytes, content type). Never raises: errors become {"error", "message"}."""
    try:
        return _route(ctx, method, path, headers, body, ip)
    except auth.AuthError as e:
        return e.status, {"error": e.code, "message": e.message}, "application/json"


def _route(ctx, method, path, headers, body, ip):
    u = urlparse(path)
    p = u.path.rstrip("/")
    a = ctx.auth
    ok = "application/json"
    if p == "/v1/status" and method == "GET":
        return 200, {"env": a.env, "online": ctx.online_count(), "auth": a.mode, "registration": a.registration,
                     "server": SERVER_VERSION, "time": int(time.time()), "maintenance": ctx.config.get()["maintenance"]}, ok
    if p == "/v1/launcher/config" and method == "GET":
        c = ctx.config.get()
        return 200, {"env": a.env, "motd": c["motd"], "links": c["links"], "minLauncher": c["minLauncher"],
                     "minShim": c["minShim"], "heroImage": c["heroImage"], "news": c["news"],
                     "latestAzimuth": c["latestAzimuth"], "supportedAzimuth": c["supportedAzimuth"], "maintenance": c["maintenance"],
                     "online": ctx.online_count(), "registration": a.registration, "auth": a.mode,
                     "lsp": ctx.lsp_public, "game": ctx.game_public, "time": int(time.time())}, ok
    if p == "/v1/challenges" and method == "GET":
        return 200, _boards(ctx, "challenges", _qint(u, "top", 3, 0, 10)), ok
    if p == "/v1/leaderboards" and method == "GET":
        return 200, _boards(ctx, "leaderboards", _qint(u, "limit", 10, 1, 50)), ok
    if p == "/v1/home" and method == "GET":
        return 200, _boards(ctx, "home", 0), ok
    if p.startswith("/v1/launcher/assets/") and method == "GET":
        data, ctype = _asset(ctx, p[len("/v1/launcher/assets/"):])
        return 200, data, ctype
    if p == "/v1/auth/challenge" and method == "POST":
        chal, bits, ttl = a.challenge(ip)
        return 200, {"challenge": chal, "powBits": bits, "ttl": int(ttl), "realm": a.env}, ok
    if p == "/v1/auth/register" and method == "POST":
        d = _json(body)
        acct = a.register(ip, str(d.get("challenge", "")), str(d.get("pubkey", "")), str(d.get("gamertag", "")),
                          d.get("pow", -1), str(d.get("sig", "")), str(d.get("invite", "")))
        ctx.log(f"  v1 register {acct.xuid:#x} {acct.gamertag!r}")
        return 201, {"account": _account_json(acct)}, ok
    if p == "/v1/auth/login" and method == "POST":
        d = _json(body)
        _require_launcher(ctx, str(d.get("client", "")))
        token, acct, ttl = a.login(ip, str(d.get("challenge", "")), str(d.get("pubkey", "")), str(d.get("sig", "")), str(d.get("client", "")))
        ctx.on_login(acct.xuid, ip)
        ctx.log(f"  v1 login {acct.xuid:#x} {acct.gamertag!r}")
        return 200, {"token": token, "idleSeconds": int(ttl), "account": _account_json(acct), "lsp": ctx.lsp_public,
                     "game": ctx.game_public}, ok
    if p == "/v1/gamertags/check" and method == "GET":
        name = (parse_qs(u.query).get("name") or [""])[0]
        try:
            free = a.tag_available(ip, name)
            return 200, {"valid": True, "available": free, "reason": "" if free else "that gamertag is taken"}, ok
        except auth.AuthError as e:
            if e.status == 429:
                raise
            return 200, {"valid": False, "available": False, "reason": e.message}, ok
    if p.startswith("/v1/gamerpic/") and p.endswith(".png") and method == "GET":
        try:
            xuid = int(p[len("/v1/gamerpic/"):-4])
        except ValueError:
            raise auth.AuthError(404, "not_found")
        return 200, ctx.social.gamerpic(xuid), "image/png"

    if p == "/v1/auth/logout" and method == "POST":
        a.authenticate(_bearer(headers), ip)
        a.logout(_bearer(headers))
        return 200, {"ok": True}, ok
    if p == "/v1/gamedb" and method == "GET":
        a.authenticate(_bearer(headers), ip)
        m = ctx.gamedb.manifest()
        if m is None:
            raise auth.AuthError(404, "no_gamedb", "this server does not set a car database")
        return 200, m, ok
    if p == gamedb_sync.DOWNLOAD_PATH and method == "GET":
        acct = a.authenticate(_bearer(headers), ip)
        if not a.hit(("gamedb", acct.xuid), 10, 3600):
            raise auth.AuthError(429, "rate_limited", "too many car database downloads, wait a while")
        blob = ctx.gamedb.download()
        if blob is None:
            raise auth.AuthError(404, "no_gamedb", "this server does not set a car database")
        ctx.log(f"  v1 gamedb download {acct.xuid:#x} {len(blob)} bytes")
        return 200, blob, "application/x-xz"
    if p == "/v1/me" or p.startswith("/v1/me/"):
        acct = a.authenticate(_bearer(headers), ip)
        if p == "/v1/me" and method == "GET":
            r = a.account(acct.xuid)
            return 200, _account_json(acct, {"created": r["created"], "lastLogin": r["last_login"],
                                             "gamertagChangedAt": r["tag_changed"],
                                             "gamerpic": ctx.social.profile(acct.xuid)["_pic_version"] > 0}), ok
        if p == "/v1/me" and method == "PATCH":
            d = _json(body)
            tag = a.rename(acct.xuid, str(d.get("gamertag", "")))
            return 200, _account_json(auth.Account(acct.xuid, tag)), ok
        if p == "/v1/me/gamerpic" and method == "PUT":
            if len(body) > MAX_BODY:
                raise auth.AuthError(413, "too_large")
            if not a.hit(("gamerpic", acct.xuid), 6, 600):
                raise auth.AuthError(429, "rate_limited", "too many picture uploads, wait a few minutes")
            png = clean_gamerpic(body)
            ctx.social.set_gamerpic(acct.xuid, png)
            return 200, {"bytes": len(png)}, ok
        if p == "/v1/me/gamerpic" and method == "DELETE":
            ctx.social.clear_gamerpic(acct.xuid)
            return 200, {"ok": True}, ok
    raise auth.AuthError(404, "not_found", "unknown endpoint")
