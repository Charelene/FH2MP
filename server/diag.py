"""Shared pieces of the server's logging: levels, per-subsystem verbosity, rate limiting, exception text and secret scrubbing.

Line shape (both the game server and the LSP): `HH:MM:SS [LEVEL ]<context> <message>`. INFO is the default and carries no level word, so the
many parsers of existing lines (xp_audit, site_stats, tools/) keep working; WARN, ERROR and DEBUG lines start with their word. Fields that
matter are written `key=value` (kv()), which is both readable and greppable.

Levels:  ERROR  an operation genuinely failed.   WARN  unexpected or degraded, worth a look.   INFO  lifecycle and state changes.
         DEBUG  detail, off by default.
Verbosity: FH2MP_LOG_LEVEL=INFO|DEBUG|WARN|ERROR (default INFO); FH2MP_LOG_SUB="grid=DEBUG,reliable=DEBUG" raises one subsystem only.
"""
import os
import re
import threading
import time
import traceback

LEVELS = {"DEBUG": 10, "INFO": 20, "WARN": 30, "ERROR": 40}


def _parse_subs(text):
    out = {}
    for item in (text or "").split(","):
        name, _, level = item.partition("=")
        if name.strip() and level.strip().upper() in LEVELS:
            out[name.strip()] = LEVELS[level.strip().upper()]
    return out


class Verbosity:
    """Which lines are written. Read once from the environment; tests pass their own."""

    def __init__(self, level=None, subs=None):
        self.base = LEVELS.get((level if level is not None else os.environ.get("FH2MP_LOG_LEVEL", "INFO")).upper(), LEVELS["INFO"])
        self.subs = _parse_subs(subs if subs is not None else os.environ.get("FH2MP_LOG_SUB", ""))

    def enabled(self, level, sub=None):
        return LEVELS[level] >= self.subs.get(sub, self.base) if sub else LEVELS[level] >= self.base


class RateLimit:
    """At most one line per key per `interval` seconds; the next line that gets through says how many it replaced."""

    def __init__(self, clock=time.monotonic, limit=2048):
        self.clock, self.limit, self.lock = clock, limit, threading.Lock()
        self.state = {}                       # key -> (next allowed time, suppressed count)

    def allow(self, key, interval):
        """-> (write?, suppressed since the last written line)"""
        now = self.clock()
        with self.lock:
            nxt, skipped = self.state.get(key, (0.0, 0))
            if now < nxt:
                self.state[key] = (nxt, skipped + 1)
                return False, 0
            if len(self.state) >= self.limit:
                self.state.clear()
            self.state[key] = (now + interval, 0)
            return True, skipped


_SECRETS = re.compile(r"(fh2\.[A-Za-z0-9_\-]{8,}|Bearer\s+\S+|X-FH2MP-Token\S*\s*[:=]\s*\S+|(?:token|secret|password|private[_ ]?key)\s*[=:]\s*\S+)", re.I)


def scrub(text):
    """Session tokens, bearer headers and key=value secrets never reach a log line."""
    return _SECRETS.sub("<redacted>", text)


def kv(**fields):
    """`a=1 b='two words'`, in the order given; None fields are left out."""
    parts = []
    for k, v in fields.items():
        if v is None:
            continue
        s = str(v)
        parts.append(f"{k}={s!r}" if (not s or any(c in s for c in " \t'\"")) else f"{k}={s}")
    return " ".join(parts)


def where(exc):
    """`TypeName: message at file.py:123 in function` (innermost frame), for the one line that says what failed and where."""
    tb = traceback.extract_tb(exc.__traceback__)
    at = f" at {os.path.basename(tb[-1].filename)}:{tb[-1].lineno} in {tb[-1].name}" if tb else ""
    return scrub(f"{type(exc).__name__}: {exc}{at}")


def stack(exc, limit=12):
    """The traceback as one indented block (for ERROR lines and the log file, never for a hot path)."""
    return scrub("".join(traceback.format_exception(type(exc), exc, exc.__traceback__, limit=-limit)).rstrip())
