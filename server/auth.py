"""Accountless identity for FH2MP: no e-mail, no password, no third-party login.

An account is an Ed25519 key pair that the launcher creates on the player's machine. The private key never leaves it; the server stores
only the public key. The server assigns the Xbox user id (xuid) itself, so nobody can pick, guess or claim somebody else's id.

    register   GET  challenge -> solve the proof of work -> POST {challenge, pubkey, gamertag, pow, sig(challenge, pubkey, gamertag)}
    login      GET  challenge -> POST {challenge, pubkey, sig(challenge, pubkey)}  -> a session token
    play       every request of the game carries the token (X-FH2MP-Token); the server derives the xuid from it, never from the request

Why this holds up:
  * a login signature covers a server-made, single-use, 2-minute challenge and the realm (prod/dev), so it cannot be replayed, and a
    signature made for the dev server is worthless on prod;
  * session tokens are 256 random bits, stored only as SHA-256, bound to the IP that logged in, expire after 12 h without use (72 h at
    most) and a new login replaces the previous token, so one key plays one game at a time;
  * a database leak therefore exposes public keys and token hashes, nothing that can be used to sign in;
  * registrations cost the client a proof of work (adaptive: it gets harder while many accounts are being made), are rate limited
    per address, and can be switched to invite-only or closed;
  * gamertags are chosen once per week at most, ASCII only, unique after folding look-alike characters, with a reserved/blocked list.

Losing the key means losing the account, which is the price of having no e-mail; the launcher shows a recovery code (the key itself,
Base32 with a checksum) for the player to keep.

The module is HTTP-free: server/launcher_api.py maps requests onto it.
"""
import base64
import hashlib
import hmac
import os
import re
import secrets
import struct
import threading
import time
import unicodedata

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

XUID_BASE = 0x0009000000000000      # every FH2MP xuid is XUID_BASE | a random 32-bit number (the shape the game and the shim already use)
CHALLENGE_TTL = 120.0
SESSION_IDLE = 12 * 3600.0
SESSION_MAX = 72 * 3600.0
TOUCH_EVERY = 300.0                 # last_used / expiry are written at most this often
CACHE_SECONDS = 20.0                # a revoked or banned token stops working at most this long after the change (instantly in-process)
TAG_MIN, TAG_MAX = 3, 15
TAG_COOLDOWN = 7 * 86400.0
POW_BASE_BITS = 18
POW_MAX_BITS = 24
SIGN_PREFIX = b"FH2MP-AUTH-v1\0"

# The tables are migration 6 in db.py (accounts, auth_sessions, auth_nonces, auth_events, invites).


def udp_ticket_key(token):
    """The key a game client signs its UDP connection ticket with (hex): HMAC-SHA256(session token, "fh2mp udp ticket v1"). The shim
    derives it from the token the launcher hands it (udp_ticket_send in shim/src/fh2mp_mod.c); the LSP from the token it sees, and passes
    it to the game server with the sign-in announcement, so the game server never sees a token."""
    return hmac.new(token.encode(), b"fh2mp udp ticket v1", hashlib.sha256).hexdigest() if token else ""


class AuthError(Exception):
    def __init__(self, status, code, message=""):
        super().__init__(message or code)
        self.status, self.code, self.message = status, code, message or code


class Account:
    __slots__ = ("xuid", "gamertag", "pubkey")

    def __init__(self, xuid, gamertag, pubkey=b""):
        self.xuid, self.gamertag, self.pubkey = xuid, gamertag, pubkey


# ---------------------------------------------------------------------------------------------------- helpers

def b64u(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def unb64u(s):
    if not isinstance(s, str) or len(s) > 4096:
        raise AuthError(400, "bad_request", "malformed field")
    try:
        return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))
    except (ValueError, TypeError):
        raise AuthError(400, "bad_request", "malformed field")


def sign_message(realm, purpose, challenge_raw, *fields):
    """The exact bytes the client signs. Length-prefixed fields so no two field lists can produce the same message."""
    out = SIGN_PREFIX + realm.encode() + b"\0" + purpose.encode() + b"\0" + challenge_raw
    for f in fields:
        out += struct.pack(">H", len(f)) + f
    return out


def pow_ok(challenge_raw, pubkey, counter, bits):
    """Hashcash: SHA-256(challenge || pubkey || counter) must start with `bits` zero bits."""
    h = hashlib.sha256(challenge_raw + pubkey + struct.pack(">Q", counter)).digest()
    n = int.from_bytes(h, "big")
    return n >> (256 - bits) == 0


def norm_ip(ip):
    ip = (ip or "").strip()
    if ip.startswith("::ffff:"):
        ip = ip[7:]
    return ip


_FOLD = str.maketrans({"0": "o", "1": "i", "l": "i", "3": "e", "4": "a", "5": "s", "7": "t", "$": "s", "@": "a", "!": "i"})
# Azimuth only accepts "1 to 15 letters, digits and single spaces, starting with a letter" as the in-game name and shows "Player 1"
# for anything else (found in azimuth.exe, 2026-10-23), so new names follow that; older accounts keep theirs, the launcher turns them
# into a name Azimuth takes (launcher azimuth.ingame_name).
_TAG_OK = re.compile(r"^[A-Za-z][A-Za-z0-9]*(?: [A-Za-z0-9]+)*$")
RESERVED = ("admin", "moderator", "staff", "official", "support", "turn10", "microsoft", "xbox", "playground", "azimuth", "fh2mp",
            "server", "system", "host", "guest", "drivatar", "anonymous", "nobody", "everyone", "undefined", "null")


def fold_tag(tag):
    """The form compared for uniqueness: case, separators and look-alike digits/symbols removed ('Adm1n' == 'admin')."""
    t = unicodedata.normalize("NFKC", tag).lower().translate(_FOLD)
    return re.sub(r"[ ._-]+", "", t)


_BLOCKED_SUBSTR = tuple(fold_tag(x) for x in RESERVED)


def validate_gamertag(tag, extra_blocked=(), extra_words=()):
    """-> the trimmed gamertag, or AuthError(400, 'bad_gamertag'). The operator's blocklist (admin panel) comes in as `extra_blocked`
    (folded, blocked anywhere in a name) and `extra_words` (folded, blocked only as a whole word)."""
    tag = (tag or "").strip()
    if not (TAG_MIN <= len(tag) <= TAG_MAX):
        raise AuthError(400, "bad_gamertag", f"a gamertag has {TAG_MIN} to {TAG_MAX} characters")
    if not _TAG_OK.match(tag) or "  " in tag:
        raise AuthError(400, "bad_gamertag", "use letters, digits and single spaces, starting with a letter")
    key = fold_tag(tag)
    if len(key) < 2:
        raise AuthError(400, "bad_gamertag", "too short once spaces and symbols are ignored")
    words = {fold_tag(w) for w in re.split(r"[ ._-]+", tag)}
    if (any(bad in key for bad in _BLOCKED_SUBSTR + tuple(extra_blocked)) or words & set(extra_words)
            or re.fullmatch(r"player[ ._-]*\d*", tag.lower())):
        raise AuthError(400, "bad_gamertag", "that name is not allowed")
    return tag


class RateLimiter:
    """Sliding-window counters keyed by anything; in memory (a restart forgives, which is acceptable for these limits)."""

    def __init__(self):
        self._hits = {}
        self._lock = threading.Lock()

    def hit(self, key, limit, window):
        """Counts one event; True when it is within `limit` per `window` seconds."""
        now = time.monotonic()
        with self._lock:
            q = [t for t in self._hits.get(key, ()) if now - t < window]
            ok = len(q) < limit
            if ok:
                q.append(now)
            if q:
                self._hits[key] = q
            else:
                self._hits.pop(key, None)
            if len(self._hits) > 50000:        # bound the memory against address floods
                for k in list(self._hits)[:10000]:
                    del self._hits[k]
            return ok


def _load_blocklist(path):
    if not path or not os.path.isfile(path):
        return ()
    with open(path, encoding="utf-8") as f:
        return tuple(fold_tag(x.strip()) for x in f if x.strip() and not x.startswith("#"))


# ---------------------------------------------------------------------------------------------------- the service

MODES = ("off", "optional", "required")
REGISTRATIONS = ("open", "invite", "closed")
TOKEN_IP_MODES = ("exact", "off")


class AuthService:
    def __init__(self, db, env="prod", mode="optional", registration="open", token_ip="exact", clock=time.time):
        """mode: off (no checks, the old trust-the-client behaviour), optional (a valid token wins, none = legacy), required.
        registration: open, invite or closed. token_ip: exact (a token works from the IP that logged in), off."""
        for name, value, allowed in (("FH2MP_AUTH", mode, MODES), ("FH2MP_REGISTRATION", registration, REGISTRATIONS),
                                     ("FH2MP_TOKEN_IP", token_ip, TOKEN_IP_MODES)):
            if value not in allowed:            # a typo must not become a weaker setting (audit 2026-10-25 F08)
                raise ValueError(f"{name}={value!r}: expected one of {', '.join(allowed)}")
        self.db, self.env, self.mode, self.registration, self.token_ip = db, env, mode, registration, token_ip
        self.clock = clock
        self.limits = RateLimiter()
        self.blocked_file = _load_blocklist(os.environ.get("FH2MP_BLOCKLIST", ""))
        self.blocked = self.blocked_file            # + the admin panel's list (ops_api.apply_config)
        self.blocked_words = ()
        self._cache = {}                 # token hash -> dict
        self._cache_lock = threading.Lock()
        self._secret = self._load_secret()
        self.scale = float(os.environ.get("FH2MP_RATE_SCALE", "1"))      # load tests on the dev stack raise every limit (never on prod)
        self.per_ip_daily = int(os.environ.get("FH2MP_REG_PER_IP_DAY", "5")) * max(1, int(self.scale))
        self.global_hourly = int(os.environ.get("FH2MP_REG_PER_HOUR", "300")) * max(1, int(self.scale))
        pow_env = os.environ.get("FH2MP_POW_BITS", f"{POW_BASE_BITS},{POW_MAX_BITS}").split(",")      # "base,max"; dev tests lower both
        self.pow_base, self.pow_max = int(pow_env[0]), int(pow_env[-1])
        self.on_change = None            # callback(xuid) after a gamertag change / ban (the LSP drops its caches)
        self.on_revoke = None            # callback(xuid) after a ban or a kick: the game server must end that player's live connections

    def hit(self, key, limit, window):
        return self.limits.hit(key, max(1, int(limit * self.scale)), window)

    # ---- secrets and challenges
    def _load_secret(self):
        s = os.environ.get("FH2MP_AUTH_SECRET")
        if s:
            return s.encode()
        v = self.db.meta_get("auth_secret")
        if not v:
            v = secrets.token_hex(32)
            with self.db.tx() as c:
                c.execute("INSERT INTO meta (key, value) VALUES ('auth_secret', ?) ON CONFLICT (key) DO NOTHING", (v,))
            v = self.db.meta_get("auth_secret")
        return v.encode()

    def _mac(self, data):
        return hmac.new(self._secret, b"chal|" + self.env.encode() + b"|" + data, hashlib.sha256).digest()[:16]

    def pow_bits(self):
        """Proof-of-work difficulty for new registrations: harder while many accounts were made in the last 10 minutes."""
        n = self.db.one("SELECT COUNT(*) FROM accounts WHERE created > ?", (self.clock() - 600,))[0]
        return min(self.pow_max, self.pow_base + n // 10)

    def challenge(self, ip):
        """A stateless, MAC-protected challenge: timestamp | random | pow bits | mac. -> (challenge string, pow bits, ttl)."""
        if not self.hit(("chal", norm_ip(ip)), 60, 60):
            raise AuthError(429, "rate_limited", "too many requests, wait a minute")
        bits = self.pow_bits()
        body = struct.pack(">dB", self.clock(), bits) + secrets.token_bytes(16)
        raw = body + self._mac(body)
        return b64u(raw), bits, CHALLENGE_TTL

    def _open_challenge(self, chal):
        """Checks the MAC and age, then burns the challenge (single use). -> (raw bytes, pow bits)."""
        raw = unb64u(chal)
        if len(raw) != 8 + 1 + 16 + 16:
            raise AuthError(400, "bad_challenge")
        body, mac = raw[:-16], raw[-16:]
        if not hmac.compare_digest(mac, self._mac(body)):
            raise AuthError(400, "bad_challenge")
        ts, bits = struct.unpack(">dB", body[:9])
        now = self.clock()
        if not (now - CHALLENGE_TTL <= ts <= now + 5):
            raise AuthError(400, "challenge_expired", "the challenge is too old, ask for a new one")
        with self.db.tx() as c:
            c.execute("DELETE FROM auth_nonces WHERE expires < ?", (now,))
            if c.execute("INSERT INTO auth_nonces (nonce, expires) VALUES (?, ?) ON CONFLICT DO NOTHING",
                         (body[9:25], ts + CHALLENGE_TTL)).rowcount != 1:
                raise AuthError(400, "challenge_used", "that challenge was already used")
        return raw, bits

    def _verify(self, pubkey, signature, message):
        try:
            Ed25519PublicKey.from_public_bytes(pubkey).verify(signature, message)
        except (InvalidSignature, ValueError):
            return False
        return True

    def _event(self, kind, ip="", xuid=None, detail=""):
        try:
            with self.db.tx() as c:
                c.execute("INSERT INTO auth_events (at, kind, ip, xuid, detail) VALUES (?,?,?,?,?)",
                          (self.clock(), kind, norm_ip(ip), xuid, detail[:200]))
                if secrets.randbelow(200) == 0:
                    c.execute("DELETE FROM auth_events WHERE at < ?", (self.clock() - 90 * 86400,))
        except Exception:  # noqa: BLE001 - auditing must never break a login
            pass

    # ---- registration
    def register(self, ip, challenge, pubkey_b64, gamertag, pow_counter, sig_b64, invite=""):
        ip = norm_ip(ip)
        if self.registration == "closed":
            raise AuthError(403, "registration_closed", "new accounts are not being accepted right now")
        pubkey, sig = unb64u(pubkey_b64), unb64u(sig_b64)
        if len(pubkey) != 32 or len(sig) != 64 or not isinstance(pow_counter, int) or pow_counter < 0:
            raise AuthError(400, "bad_request", "malformed field")
        if not self.hit(("reg", ip), 10, 3600):
            raise AuthError(429, "rate_limited", "too many attempts from this address")
        tag = validate_gamertag(gamertag, self.blocked, self.blocked_words)
        raw, bits = self._open_challenge(challenge)
        if not pow_ok(raw, pubkey, pow_counter, bits):
            raise AuthError(400, "bad_pow", "proof of work does not meet the target")
        if not self._verify(pubkey, sig, sign_message(self.env, "register", raw, pubkey, tag.encode())):
            self._event("register_badsig", ip)
            raise AuthError(401, "bad_signature")
        now = self.clock()
        if self.db.one("SELECT COUNT(*) FROM accounts WHERE created_ip=? AND created > ?", (ip, now - 86400))[0] >= self.per_ip_daily:
            raise AuthError(429, "rate_limited", "too many accounts were created from this address today")
        if self.db.one("SELECT COUNT(*) FROM accounts WHERE created > ?", (now - 3600,))[0] >= self.global_hourly:
            raise AuthError(503, "busy", "a lot of accounts are being created, try again later")
        key = fold_tag(tag)
        with self.db.tx() as c:
            if self.registration == "invite":
                code = hashlib.sha256((invite or "").strip().upper().encode()).digest()
                if c.execute("UPDATE invites SET uses_left=uses_left-1 WHERE code_hash=? AND uses_left>0", (code,)).rowcount != 1:
                    raise AuthError(403, "invite_required", "a valid invite code is needed to register")
            if c.execute("SELECT 1 FROM accounts WHERE pubkey=?", (pubkey,)).fetchone():
                raise AuthError(409, "key_exists", "this key already has an account: sign in instead")
            if c.execute("SELECT 1 FROM accounts WHERE gamertag_key=?", (key,)).fetchone():
                raise AuthError(409, "gamertag_taken", "that gamertag is taken")
            for _ in range(20):
                xuid = XUID_BASE | (secrets.randbits(32) | 0x10000)
                if not c.execute("SELECT 1 FROM accounts WHERE xuid=? UNION ALL SELECT 1 FROM players WHERE xuid=?", (xuid, xuid)).fetchone():
                    break
            else:
                raise AuthError(503, "busy", "could not allocate an id")
            c.execute("INSERT INTO accounts (xuid, pubkey, gamertag, gamertag_key, created, created_ip) VALUES (?,?,?,?,?,?)",
                      (xuid, pubkey, tag, key, now, ip))
            c.execute("INSERT INTO players (xuid, gamertag, career_level, first_seen, last_seen) VALUES (?,?,0,?,?) "
                      "ON CONFLICT (xuid) DO UPDATE SET gamertag=excluded.gamertag", (xuid, tag, now, now))
        self._event("register", ip, xuid, tag)
        return Account(xuid, tag, pubkey)

    # ---- sessions
    def login(self, ip, challenge, pubkey_b64, sig_b64, client=""):
        """-> (token, Account, seconds the token stays valid when idle)."""
        ip = norm_ip(ip)
        pubkey, sig = unb64u(pubkey_b64), unb64u(sig_b64)
        if len(pubkey) != 32 or len(sig) != 64:
            raise AuthError(400, "bad_request", "malformed field")
        if not self.hit(("login", ip), 30, 60) or not self.hit(("login-key", pubkey), 12, 60):
            raise AuthError(429, "rate_limited", "too many sign-in attempts")
        raw, _bits = self._open_challenge(challenge)
        if not self._verify(pubkey, sig, sign_message(self.env, "login", raw, pubkey)):
            self._event("login_badsig", ip)
            raise AuthError(401, "bad_signature")
        row = self.db.one("SELECT xuid, gamertag, banned_at, ban_reason FROM accounts WHERE pubkey=?", (pubkey,))
        if row is None:
            raise AuthError(404, "unknown_account", "no account for this key: sign up first")
        if row["banned_at"]:
            self._event("login_banned", ip, row["xuid"])
            raise AuthError(403, "banned", row["ban_reason"] or "this account is banned")
        token = "fh2." + b64u(secrets.token_bytes(32))
        now = self.clock()
        with self.db.tx() as c:
            old = [bytes(r[0]) for r in c.execute("SELECT token_hash FROM auth_sessions WHERE xuid=?", (row["xuid"],)).fetchall()]
            c.execute("DELETE FROM auth_sessions WHERE xuid=?", (row["xuid"],))              # one active session per account
            c.execute("INSERT INTO auth_sessions (token_hash, xuid, ip, client, created, last_used, expires, hard_expires) VALUES (?,?,?,?,?,?,?,?)",
                      (self._hash(token), row["xuid"], ip, client[:120], now, now, now + SESSION_IDLE, now + SESSION_MAX))
            c.execute("UPDATE accounts SET last_login=? WHERE xuid=?", (now, row["xuid"]))
        self._forget(old)
        self._event("login", ip, row["xuid"], client[:80])
        return token, Account(row["xuid"], row["gamertag"], pubkey), SESSION_IDLE

    @staticmethod
    def _hash(token):
        return hashlib.sha256(token.encode()).digest()

    def _forget(self, hashes):
        with self._cache_lock:
            for h in hashes:
                self._cache.pop(h, None)

    def authenticate(self, token, ip):
        """-> Account for a live token used from the address that logged in; AuthError otherwise."""
        if not token or not token.startswith("fh2.") or len(token) > 80:
            raise AuthError(401, "invalid_token")
        h, now, ip = self._hash(token), self.clock(), norm_ip(ip)
        with self._cache_lock:
            ent = self._cache.get(h)
        if ent is None or now - ent["checked"] > CACHE_SECONDS:
            r = self.db.one("SELECT s.xuid, s.ip, s.expires, s.hard_expires, s.last_used, a.gamertag, a.banned_at, a.ban_reason "
                            "FROM auth_sessions s JOIN accounts a ON a.xuid = s.xuid WHERE s.token_hash=? AND s.revoked=0", (h,))
            if r is None:
                with self._cache_lock:
                    self._cache.pop(h, None)
                raise AuthError(401, "invalid_token")
            ent = {"xuid": r["xuid"], "ip": r["ip"], "expires": r["expires"], "hard": r["hard_expires"], "tag": r["gamertag"],
                   "banned": r["banned_at"], "reason": r["ban_reason"], "written": r["last_used"], "checked": now}
            with self._cache_lock:
                self._cache[h] = ent
        if ent["banned"]:
            raise AuthError(403, "banned", ent["reason"] or "this account is banned")
        if now > ent["expires"] or now > ent["hard"]:
            raise AuthError(401, "token_expired", "the session expired: sign in again from the launcher")
        if self.token_ip == "exact" and ip and ent["ip"] != ip:
            raise AuthError(401, "ip_changed", "your address changed: start the game from the launcher again")
        if now - ent["written"] > TOUCH_EVERY:
            ent["written"], ent["expires"] = now, min(now + SESSION_IDLE, ent["hard"])
            with self.db.tx() as c:
                c.execute("UPDATE auth_sessions SET last_used=?, expires=? WHERE token_hash=?", (now, ent["expires"], h))
        return Account(ent["xuid"], ent["tag"])

    def logout(self, token):
        h = self._hash(token)
        with self.db.tx() as c:
            c.execute("DELETE FROM auth_sessions WHERE token_hash=?", (h,))
        self._forget([h])

    # ---- account management
    def account(self, xuid):
        r = self.db.one("SELECT xuid, pubkey, gamertag, created, last_login, tag_changed, banned_at, ban_reason FROM accounts WHERE xuid=?", (xuid,))
        return r

    def rename(self, xuid, new_tag, force=False):
        tag = validate_gamertag(new_tag, self.blocked, self.blocked_words)
        key = fold_tag(tag)
        now = self.clock()
        with self.db.tx() as c:
            r = c.execute("SELECT tag_changed, gamertag_key FROM accounts WHERE xuid=?", (xuid,)).fetchone()
            if r is None:
                raise AuthError(404, "unknown_account")
            if not force and r["tag_changed"] and now - r["tag_changed"] < TAG_COOLDOWN:
                days = (TAG_COOLDOWN - (now - r["tag_changed"])) / 86400
                raise AuthError(429, "rename_cooldown", f"you can change your gamertag again in {days:.1f} days")
            if key != r["gamertag_key"] and c.execute("SELECT 1 FROM accounts WHERE gamertag_key=?", (key,)).fetchone():
                raise AuthError(409, "gamertag_taken", "that gamertag is taken")
            c.execute("UPDATE accounts SET gamertag=?, gamertag_key=?, tag_changed=? WHERE xuid=?", (tag, key, now, xuid))
            c.execute("UPDATE players SET gamertag=? WHERE xuid=?", (tag, xuid))
        with self._cache_lock:
            for ent in self._cache.values():
                if ent["xuid"] == xuid:
                    ent["tag"] = tag
        self._event("rename", "", xuid, tag)
        if self.on_change:
            self.on_change(xuid)
        return tag

    def tag_available(self, ip, tag):
        if not self.hit(("tagcheck", norm_ip(ip)), 40, 60):
            raise AuthError(429, "rate_limited", "slow down")
        tag = validate_gamertag(tag, self.blocked, self.blocked_words)
        return not self.db.one("SELECT 1 FROM accounts WHERE gamertag_key=?", (fold_tag(tag),))

    def set_banned(self, xuid, reason=None):
        """reason None = unban."""
        with self.db.tx() as c:
            n = c.execute("UPDATE accounts SET banned_at=?, ban_reason=? WHERE xuid=?",
                          (None if reason is None else self.clock(), reason or "", xuid)).rowcount
            if reason is not None:
                c.execute("DELETE FROM auth_sessions WHERE xuid=?", (xuid,))
        with self._cache_lock:
            for h, ent in list(self._cache.items()):
                if ent["xuid"] == xuid:
                    del self._cache[h]
        self._event("ban" if reason is not None else "unban", "", xuid, reason or "")
        if self.on_change:
            self.on_change(xuid)
        if reason is not None and self.on_revoke:
            self.on_revoke(xuid)
        return n > 0

    def revoke(self, xuid):
        with self.db.tx() as c:
            n = c.execute("DELETE FROM auth_sessions WHERE xuid=?", (xuid,)).rowcount
        with self._cache_lock:
            for h, ent in list(self._cache.items()):
                if ent["xuid"] == xuid:
                    del self._cache[h]
        if self.on_revoke:
            self.on_revoke(xuid)
        return n

    def add_invite(self, code, uses=1, note=""):
        with self.db.tx() as c:
            c.execute("INSERT INTO invites (code_hash, uses_left, note, created) VALUES (?,?,?,?) "
                      "ON CONFLICT (code_hash) DO UPDATE SET uses_left=excluded.uses_left", (hashlib.sha256(code.strip().upper().encode()).digest(), uses, note, self.clock()))

    def stats(self):
        now = self.clock()
        return {"accounts": self.db.one("SELECT COUNT(*) FROM accounts")[0],
                "sessions": self.db.one("SELECT COUNT(*) FROM auth_sessions WHERE expires > ?", (now,))[0],
                "registered_24h": self.db.one("SELECT COUNT(*) FROM accounts WHERE created > ?", (now - 86400,))[0]}
