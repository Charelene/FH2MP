"""<cache>/launcher.log: what the launcher did and why, for a player to send us and for the crash report.

Line shape: `2026-11-01T23:45:01.123-0500 WARN play play.blocked run=ab12cd34 code=no_shim elapsed_ms=12`, that is time, level, subsystem, a stable event
name, then key=value fields (the same shape as the server's diag.py, which also names the levels and the environment variables):
ERROR  an operation failed unexpectedly (with its stack).   WARN  degraded or refused, worth a look.   INFO  lifecycle and state changes.
DEBUG  detail, off by default.   TRACE  every API call.
FH2MP_LOG_LEVEL=DEBUG raises everything; FH2MP_LOG_SUB="api=DEBUG,tunnel=TRACE" one subsystem.

run=: every Play makes a run id. It goes to the server on the API calls and to the shim in the session file, so `grep <run>` over launcher.log, fh2mp.log and
the server's logs follows one game start. Before the first Play the id of this launcher process is used.

Nothing here raises into the launcher. Tokens, keys and the home folder never reach the file: scrub() runs on every line.
"""
import contextlib
import os
import re
import secrets
import sys
import threading
import time
import traceback

from . import paths

LEVELS = {"TRACE": 5, "DEBUG": 10, "INFO": 20, "WARN": 30, "ERROR": 40}
MAX_BYTES = 200_000           # then the file becomes launcher.log.1

# Refusals the player is told about in plain words: the failure is a fact about their setup or the network, not a bug in the launcher (no stack).
EXPECTED = {"PlayBlocked", "ApiError", "UpdateError", "ShimError", "LaunchError", "UpstreamError", "SaveCopyError", "SaveScanError", "ItchError"}


def _parse_subs(text):
    out = {}
    for item in (text or "").split(","):
        name, _, level = item.partition("=")
        if name.strip() and level.strip().upper() in LEVELS:
            out[name.strip()] = LEVELS[level.strip().upper()]
    return out


_base = LEVELS.get(os.environ.get("FH2MP_LOG_LEVEL", "INFO").upper(), LEVELS["INFO"])
_subs = _parse_subs(os.environ.get("FH2MP_LOG_SUB", ""))
_lock = threading.Lock()
_run = secrets.token_hex(4)
_every = {}                   # key -> (next allowed time, lines it replaced)
_local = threading.local()

_SECRETS = re.compile(r"(fh2\.[A-Za-z0-9_\-]{8,}|Bearer\s+\S+|X-FH2MP-Token\S*\s*[:=]\s*\S+|(?:token|secret|password|private[_ ]?key|seed)\s*[=:]\s*\S+)", re.I)


def scrub(text):
    text = _SECRETS.sub("<redacted>", text)
    home = os.path.expanduser("~")
    if home and home != "~":
        text = text.replace(home, "~").replace(home.replace("/", "\\"), "~")
    return text


def kv(**fields):
    """`a=1 b='two words'`, in the order given; None fields are left out."""
    parts = []
    for k, v in fields.items():
        if v is None:
            continue
        s = str(v)
        parts.append(f"{k}={s!r}" if (not s or any(c in s for c in " \t\n'\"")) else f"{k}={s}")
    return " ".join(parts)


def run():
    return _run


def new_run():
    """A new id for a game start. -> the id."""
    global _run
    _run = secrets.token_hex(4)
    return _run


def enabled(level, sub):
    return LEVELS[level] >= _subs.get(sub, _base)


def _write(text):
    try:
        path = os.path.join(paths.cache_dir(), "launcher.log")
        with _lock:
            if os.path.exists(path) and os.path.getsize(path) > MAX_BYTES:
                os.replace(path, path + ".1")
            with open(path, "a", encoding="utf-8") as f:
                f.write(text)
    except Exception:  # noqa: BLE001 - the log is the last place a failure can be reported, and logging must never stop the launcher
        pass


def event(level, sub, name, **fields):
    if not enabled(level, sub):
        return
    t = time.time()
    stamp = time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(t)) + f".{int(t * 1000) % 1000:03d}" + time.strftime("%z", time.localtime(t))
    text = kv(run=_run, **fields)
    _write(scrub(f"{stamp} {level} {sub} {name} {text}") + "\n")


def trace(sub, name, **fields):
    event("TRACE", sub, name, **fields)


def debug(sub, name, **fields):
    event("DEBUG", sub, name, **fields)


def info(sub, name, **fields):
    event("INFO", sub, name, **fields)


def warn(sub, name, **fields):
    event("WARN", sub, name, **fields)


def error(sub, name, **fields):
    event("ERROR", sub, name, **fields)


def every(interval, level, sub, name, key=None, **fields):
    """A repeating condition (a poll failing): one line per `interval` seconds per key, saying how many it stood in for."""
    k = (sub, name, key)
    now = time.monotonic()
    with _lock:
        nxt, skipped = _every.get(k, (0.0, 0))
        if now < nxt:
            _every[k] = (nxt, skipped + 1)
            return
        if len(_every) > 512:
            _every.clear()
        _every[k] = (now + interval, 0)
    event(level, sub, name, **fields, repeated=skipped or None)


def where(exc):
    """`TypeName: message at file.py:123 in function`, the innermost frame."""
    tb = traceback.extract_tb(exc.__traceback__)
    at = f" at {os.path.basename(tb[-1].filename)}:{tb[-1].lineno} in {tb[-1].name}" if tb else ""
    return f"{type(exc).__name__}: {exc}{at}"


def exc(level, sub, name, e, **fields):
    """A caught exception, once: a later layer that sees the same exception writes nothing. The stack follows at ERROR."""
    if getattr(e, "_fh2mp_logged", False):
        return
    with contextlib.suppress(AttributeError, TypeError):
        e._fh2mp_logged = True
    event(level, sub, name, **fields, error=where(e))
    if level == "ERROR" and enabled(level, sub):
        text = "".join(traceback.format_exception(type(e), e, e.__traceback__, limit=-12)).rstrip()
        _write("".join("  " + scrub(row) + "\n" for row in text.splitlines()))


def swallowed(sub, name, e, level="WARN", **fields):
    """A failure the code goes on from (a default, a retry, "not installed"). A missing file is the normal case of most of them: DEBUG. Anything else
    (no permission, a damaged file) is why a later step behaves oddly: at most one line per 5 minutes."""
    if isinstance(e, FileNotFoundError):
        debug(sub, name, **fields, error=where(e))
    else:
        every(300, level, sub, name, **fields, error=where(e))


def job_failed(job, e):
    """An exception that left a background job (ui/workers.py). The player sees it; this is the record of it. What was logged where it happened is not repeated."""
    if getattr(e, "_fh2mp_logged", False):
        return
    if type(e).__name__ in EXPECTED:
        every(60, "WARN", "job", "job.failed", key=(job, type(e).__name__, str(e)[:80]), job=job, error=where(e))
    else:
        exc("ERROR", "job", "job.crashed", e, job=job)


class Op:
    """One operation that goes through several steps: the failure line says how far it got and how long each step took, the success line the total."""

    def __init__(self, sub, name, fields):
        self.sub, self.name, self.fields = sub, name, fields
        self.t0 = self.last = time.monotonic()
        self.trail = []

    def step(self, name):
        now = time.monotonic()
        self.trail.append(f"{name}:{(now - self.last) * 1000:.0f}")
        self.last = now
        debug(self.sub, f"{self.name}.step", step=name, elapsed_ms=int((now - self.t0) * 1000))

    def _common(self):
        return dict(self.fields, elapsed_ms=int((time.monotonic() - self.t0) * 1000), steps=",".join(self.trail) or None)


@contextlib.contextmanager
def op(sub, name, **fields):
    """with logs.op("play", "play"): ...  logs.step("sign_in") marks progress from anywhere below it on this thread."""
    o = Op(sub, name, fields)
    outer = getattr(_local, "op", None)
    _local.op = o
    info(sub, f"{name}.start", **fields)
    try:
        yield o
    except BaseException as e:
        if type(e).__name__ in EXPECTED:
            exc("WARN", sub, f"{name}.blocked", e, **o._common(), code=getattr(e, "code", None) or None)
        else:
            exc("ERROR", sub, f"{name}.failed", e, **o._common())
        raise
    else:
        info(sub, f"{name}.done", **o._common())
    finally:
        _local.op = outer


def step(name):
    o = getattr(_local, "op", None)
    if o is not None:
        o.step(name)


def start(version):
    """The launcher opened: what is needed to read every later line. Crashes in Qt slots and threads go to this file too (they were only on a console nobody has)."""
    import platform
    import ssl
    info("launcher", "launcher.start", version=version, python=sys.version.split()[0], ssl=ssl.OPENSSL_VERSION, os=platform.platform(),
         frozen=bool(getattr(sys, "frozen", False)), dev=paths.is_dev() or None, level=None if _base == LEVELS["INFO"] else _base,
         subs=os.environ.get("FH2MP_LOG_SUB") or None)
    previous = sys.excepthook

    def hook(kind, value, tb):
        exc("ERROR", "launcher", "launcher.unhandled", value.with_traceback(tb))
        previous(kind, value, tb)
    sys.excepthook = hook

    def thread_hook(args):
        if args.exc_value is not None:
            exc("ERROR", "launcher", "launcher.thread_unhandled", args.exc_value.with_traceback(args.exc_traceback), thread=getattr(args.thread, "name", None))
    threading.excepthook = thread_hook
