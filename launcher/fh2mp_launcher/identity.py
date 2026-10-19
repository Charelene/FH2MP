"""The player's identity: an Ed25519 key pair made on this machine. There is no password and no e-mail; the key *is* the account.

The server only ever sees the public key. The private key (a 32-byte seed) is kept by keystore.py, and can be written down as a
recovery code to move the account to another computer. Whoever has the code has the account, so it is shown once and never sent anywhere.
"""
import base64
import hashlib
import struct

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

SIGN_PREFIX = b"FH2MP-AUTH-v1\0"          # keep identical to server/auth.py
RECOVERY_PREFIX = "FH2R1"


def b64u(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def unb64u(s):
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


def sign_message(realm, purpose, challenge_raw, *fields):
    out = SIGN_PREFIX + realm.encode() + b"\0" + purpose.encode() + b"\0" + challenge_raw
    for f in fields:
        out += struct.pack(">H", len(f)) + f
    return out


def pow_ok(challenge_raw, pubkey, counter, bits):
    h = hashlib.sha256(challenge_raw + pubkey + struct.pack(">Q", counter)).digest()
    return int.from_bytes(h, "big") >> (256 - bits) == 0


def solve_pow(challenge_raw, pubkey, bits, progress=None, cancelled=lambda: False):
    """Hashcash: find a counter so SHA-256(challenge || pubkey || counter) starts with `bits` zero bits (a second or two at the usual
    difficulty). Hot loop kept tight; checks for cancellation every 2^15 tries."""
    prefix = challenge_raw + pubkey
    shift = 256 - bits
    sha, pack = hashlib.sha256, struct.Struct(">Q").pack
    n = 0
    while True:
        for _ in range(1 << 15):
            if int.from_bytes(sha(prefix + pack(n)).digest(), "big") >> shift == 0:
                return n
            n += 1
        if cancelled():
            raise InterruptedError("cancelled")
        if progress:
            progress(n)


class RecoveryError(ValueError):
    pass


def _check(seed):
    return hashlib.sha256(b"FH2MP-recovery-v1" + seed).digest()[:4]


class Identity:
    def __init__(self, seed):
        if len(seed) != 32:
            raise ValueError("a seed is 32 bytes")
        self._seed = bytes(seed)
        self._sk = Ed25519PrivateKey.from_private_bytes(self._seed)
        self.pubkey = self._sk.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)

    @classmethod
    def generate(cls):
        return cls(Ed25519PrivateKey.generate().private_bytes(
            serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption()))

    @property
    def seed(self):
        return self._seed

    @property
    def pubkey_b64(self):
        return b64u(self.pubkey)

    def sign(self, realm, purpose, challenge_raw, *fields):
        return self._sk.sign(sign_message(realm, purpose, challenge_raw, *fields))

    # ---- recovery code: FH2R1-XXXXX-XXXXX-...  (Base32 of seed + 4 check bytes, so a typo is noticed)
    def recovery_code(self):
        body = base64.b32encode(self._seed + _check(self._seed)).decode().rstrip("=")
        return RECOVERY_PREFIX + "-" + "-".join(body[i:i + 5] for i in range(0, len(body), 5))

    @classmethod
    def from_recovery_code(cls, code):
        s = "".join(ch for ch in code.upper() if ch.isalnum())
        if not s.startswith(RECOVERY_PREFIX):
            raise RecoveryError("that is not an FH2MP recovery code")
        body = s[len(RECOVERY_PREFIX):]
        try:
            raw = base64.b32decode(body + "=" * (-len(body) % 8))
        except ValueError:
            raise RecoveryError("the code contains characters that cannot be part of it")
        if len(raw) != 36 or raw[32:] != _check(raw[:32]):
            raise RecoveryError("the code is incomplete or has a typo")
        return cls(raw[:32])
