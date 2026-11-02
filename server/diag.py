"""Shared pieces of the server's logging: levels, per-subsystem verbosity, rate limiting, exception text and secret scrubbing.

Line shape (both the game server and the LSP): `HH:MM:SS [LEVEL ]<context> <message>`. INFO is the default and carries no level word, so the
many parsers of existing lines (xp_audit, site_stats, tools/) keep working; WARN, ERROR and DEBUG lines start with their word. Fields that
matter are written `key=value` (kv()), which is both readable and greppable.

Levels:  ERROR  an operation genuinely failed.   WARN  unexpected or degraded, worth a look.   INFO  lifecycle and state changes.
         DEBUG  detail, off by default.   TRACE  per-packet detail, for one subsystem at a time ("packet=TRACE").
Verbosity: FH2MP_LOG_LEVEL=INFO|DEBUG|WARN|ERROR (default INFO); FH2MP_LOG_SUB="grid=DEBUG,reliable=DEBUG" raises one subsystem only.

Correlation: every Play of the launcher makes a run id (8 hex digits). The launcher sends it on its API calls and writes it in the session file; the shim
puts it in fh2mp.log and sends `<run>.<xhr number>` as X-FH2MP-Run; the LSP writes it on request and failure lines and tells the game server ("run <xuid>
<run>"), which writes it on join, removal and events.jsonl lines. `grep <run>` over launcher.log, fh2mp.log, lsp.log, server.log and events.jsonl follows one
game start. New lines use a stable event name followed by key=value fields (`run.seen run=... xuid=...`).
"""
import os
import re
import sys
import threading
import time
import traceback

LEVELS = {"TRACE": 5, "DEBUG": 10, "INFO": 20, "WARN": 30, "ERROR": 40}


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


_RUN = re.compile(r"[0-9a-f]{8}(?:\.[0-9]{1,10})?")


def run_id(text):
    """The run id a client sent (`ab12cd34` or `ab12cd34.57`, the second part being the shim's request number), "" for anything else: it comes from a
    header and goes into log lines."""
    return text if text and _RUN.fullmatch(text) else ""


_LOST = RateLimit()


def write_line(write, line):
    """Writes a log line. A full disk or a closed pipe must not turn a log line into a failed request or a dead game server: the loss goes to stderr, at most
    once a minute with the number of lines lost."""
    try:
        write(line)
    except (OSError, ValueError) as e:
        ok, skipped = _LOST.allow("lost", 60.0)
        if ok:
            try:
                sys.stderr.write(f"log.write_failed {kv(error=f'{type(e).__name__}: {e}', lost=skipped + 1)}\n")
                sys.stderr.flush()
            except (OSError, ValueError):
                pass


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


# ---- modules that have no log function of their own (parts.py, xbl.py, scoreboard.py, ...): `LOG = diag.get("parts")`, then
# LOG.warn("..."), LOG.exc("what was being done", exc). The process (game server, LSP) points the sink at its own log; until then lines go to stdout.
_sink = None


def set_sink(fn):
    """fn(level, sub, message) writes the line; the game server and the LSP call this once at start."""
    global _sink
    _sink = fn


class Logger:
    def __init__(self, sub):
        self.sub = sub

    def _emit(self, level, msg):
        if _sink is not None:
            _sink(level, self.sub, msg)
        elif VERBOSITY_DEFAULT.enabled(level, self.sub):
            print(f"{time.strftime('%H:%M:%S')} {'' if level == 'INFO' else level + ' '}{scrub(msg)}", flush=True)

    def trace(self, msg):
        self._emit("TRACE", f"{self.sub}: {msg}")

    def debug(self, msg):
        self._emit("DEBUG", f"{self.sub}: {msg}")

    def info(self, msg):
        self._emit("INFO", f"{self.sub}: {msg}")

    def warn(self, msg):
        self._emit("WARN", f"{self.sub}: {msg}")

    def error(self, msg):
        self._emit("ERROR", f"{self.sub}: {msg}")

    def exc(self, msg, exc, level="ERROR"):
        """A caught exception: what was being done, then type, message and the innermost frame; the stack follows at ERROR."""
        self._emit(level, f"{self.sub}: {msg}: {where(exc)}")
        if level == "ERROR":
            for row in stack(exc).splitlines():
                self._emit("ERROR", "  " + row)


VERBOSITY_DEFAULT = Verbosity()


def get(sub):
    return Logger(sub)
