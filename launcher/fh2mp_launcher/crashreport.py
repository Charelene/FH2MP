"""Crash reports: notice that the game crashed, collect what the player is shown and sends, scrub it. No Qt here (headless tests).

What counts as a crash: Azimuth's own log (data\\logs\\azimuth.log) says `CRASHED` (an exception) or `STOPPED` (it refused to go on, for
example a graphics call it cannot do) in its last session, or it wrote a minidump (data\\logs\\*.dmp) since `since`. The graphics-check stop
(CreateDXGIFactory2, GitHub #10) has its own advice and is not reported.
"""
import base64
import datetime
import glob
import os
import platform
import re
import time

from . import azimuth, paths, shim
from .version import __version__

MAX_FILE = 2 << 20
MAX_TEXT = 8 << 20
MAX_FILES = 64
PLATFORM_LOGS_KEPT = 3            # per-session log copies of the Mod Platform (fh2-mods/platform-logs) attached to a report
LIMITS = dict.fromkeys(("azimuth.log", "fh2mp.log", "platform.log", "mod-crash.txt", "launcher.log"), MAX_FILE)
MAX_DUMP = 32 << 20    # combined Azimuth and matching Mod Platform dumps

_FAIL_RE = re.compile(r"\b(CRASHED|STOPPED)\b")
_BEGAN_RE = re.compile(r"\bsession \S+ began \d{4}-\d\d-\d\d [\d:]+ UTC: (.*)$")
_GPU_RE = re.compile(r"\bgraphics adapter (.+?) \(vendor 0x[0-9a-fA-F]+, device 0x[0-9a-fA-F]+\), driver ([^;\s]+)")

# The player's account name on the computer is in file paths (Z:\home\<name>\..., C:\Users\<name>\..., /home/<name>/...) and a session
# token is never meant to leave the machine: both are blanked before anything is shown or sent.
_SCRUBS = [
    (re.compile(r"(?i)([A-Z]:[\\/]+(?:Users|home)[\\/]+)[^\\/\r\n\"'<>|:]+(?=[\\/])"), r"\1<user>"),
    (re.compile(r"((?:/home|/Users)/)[^/\s\"'<>|:]+"), r"\1<user>"),
    (re.compile(r"\bfh2\.[A-Za-z0-9_.\-]{16,}"), "<token>"),
]


# Secrets and personal data that may be in a log line: e-mail addresses, IP addresses (not loopback; dotted version numbers such as a driver
# or game build are left alone), credentials in URLs, Authorization / cookie headers, key=value secrets and long base64 runs (keys, seeds).
# Patterns cannot promise to catch everything (security audit 2026-10-25 F05), which is why the public issue gets only a summary.
_IP4 = re.compile(r"(?<![\w.])(?:25[0-5]|2[0-4]\d|1?\d?\d)(?:\.(?:25[0-5]|2[0-4]\d|1?\d?\d)){3}(?![\w.])")
_VERSION_WORDS = ("version", "driver", "build", "ver", "v")


def _ip4(m):
    ip, before = m.group(0), m.string[max(0, m.start() - 12):m.start()].lower().rstrip(" =:")
    if ip.startswith(("127.", "0.")) or ip.endswith(".0") or ip == "1.0.0.64" or before.endswith(_VERSION_WORDS):
        return ip
    return "<ip>"


def _b64(m):
    s = m.group(0)
    if s.count("/") >= 2 or not (re.search(r"[a-z]", s) and re.search(r"[A-Z]", s) and re.search(r"\d", s)):
        return s                    # a path, a hex digest or a word: not a key
    return "<secret>"


_SECRETS = [
    (re.compile(r"(?i)\b([a-z][a-z0-9+.-]*://)[^/\s:@]+:[^/\s@]+@"), r"\1<credentials>@"),
    (re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}"), "<email>"),
    (re.compile(r"(?i)\b(authorization|proxy-authorization|x-fh2mp-token|cookie|set-cookie)(\s*[:=]\s*)[^\r\n]+"), r"\1\2<secret>"),
    (re.compile(r"(?i)\b(bearer|basic|xbl3\.0)\s+[^\s\"']{8,}"), r"\1 <secret>"),
    (re.compile(r"(?i)\b(recovery[_-]?\w*|mnemonic|seed[ _-]?phrase)(\s*[=:]\s*)[^\r\n]+"), r"\1\2<secret>"),
    (re.compile(r"(?i)\b(password|passwd|pwd|secret|token|api[_-]?key|private[_-]?key|seed)(\s*[=:]\s*)"
                r"(\"[^\"]*\"|'[^']*'|[^\s,;]+)"), r"\1\2<secret>"),
    (_IP4, _ip4),
    (re.compile(r"(?i)(?<![\w:])(?:[23][0-9a-f]{3}|f[cd][0-9a-f]{2}|fe80)(?::[0-9a-f]{0,4}){2,7}(?![\w:])"), "<ip>"),
    (re.compile(r"(?<![A-Za-z0-9+/_-])[A-Za-z0-9+/_-]{40,}={0,2}"), _b64),
]


class Crash:
    """What was found: the failure line from azimuth.log (may be ''), the newest dump (may be None) and when it happened (epoch seconds)."""

    def __init__(self, folder, line, dump, when, seen=None, mod_crash=None):
        self.folder, self.line, self.dump, self.when = folder, line, dump, when
        self.mod_crash = mod_crash          # the FH2 Mod Platform's own crash report (fh2-mods/crashes/crash-*.txt: call stack, hooks), or None
        self.seen = seen or when            # newest file time of the evidence: once handled, nothing older is offered again


def scrub(text):
    for rx, rep in _SCRUBS + _SECRETS:
        text = rx.sub(rep, text)
    return text


def logs_dir(folder):
    return os.path.join(folder, "data", "logs")


def _read_tail(path, limit):
    """The last `limit` bytes of a text file, starting at a line start, as text. '' when unreadable."""
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - limit))
            data = f.read()
            if size > limit and b"\n" in data:
                data = data.split(b"\n", 1)[1]
    except OSError:
        return ""
    return data.decode("utf-8", "replace")


def _cut(text, limit):
    """The end of `text` that fits in `limit` bytes, starting at a line start."""
    data = text.encode()
    if len(data) <= limit:
        return text
    data = data[-limit:]
    return (data.split(b"\n", 1)[1] if b"\n" in data else data).decode("utf-8", "ignore")


def _last_session(text):
    lines = text.splitlines()
    start = max((i for i, ln in enumerate(lines) if _BEGAN_RE.search(ln)), default=0)
    return lines[start:]


def _session_start(lines):
    for ln in lines:
        if not _BEGAN_RE.search(ln):
            continue
        began = re.search(r"\bbegan (\d{4}-\d\d-\d\d [\d:]+) UTC:", ln)
        if began:
            try:
                return datetime.datetime.strptime(began.group(1), "%Y-%m-%d %H:%M:%S").replace(tzinfo=datetime.timezone.utc).timestamp()
            except ValueError:
                pass
    return None


def find_crash(folder, since):
    """-> Crash for the newest failure Azimuth recorded after `since` (epoch seconds), or None."""
    if not folder or not os.path.isdir(folder):
        return None
    d = logs_dir(folder)
    dump, dump_t = None, 0.0
    try:
        for n in os.listdir(d):
            p = os.path.join(d, n)
            if n.lower().endswith(".dmp") and os.path.isfile(p) and os.path.getmtime(p) > since and os.path.getmtime(p) > dump_t:
                dump, dump_t = p, os.path.getmtime(p)
    except OSError:
        pass
    mod_crash, mod_t = None, 0.0
    cdir = os.path.join(folder, "fh2-mods", "crashes")
    try:
        for n in os.listdir(cdir):
            p = os.path.join(cdir, n)
            if n.lower().endswith(".txt") and os.path.isfile(p) and os.path.getmtime(p) > max(since, mod_t):
                mod_crash, mod_t = p, os.path.getmtime(p)
    except OSError:
        pass
    line, log_t, session = "", 0.0, []
    next_session_t = None
    log = os.path.join(d, "azimuth.log")
    try:
        log_t = os.path.getmtime(log)
    except OSError:
        pass
    if log_t > since:
        lines = _read_tail(log, LIMITS["azimuth.log"]).splitlines()
        session = _last_session("\n".join(lines))
        fails = [ln for ln in session if _FAIL_RE.search(ln) and "CreateDXGIFactory2" not in ln]
        line = fails[0] if fails else ""
        if dump and not line:
            marker = os.path.basename(dump)
            end = next((i for i in range(len(lines) - 1, -1, -1)
                        if "crash dump " in lines[i] and marker in lines[i]), None)
            if end is not None:
                start = max((i for i in range(end) if _BEGAN_RE.search(lines[i])), default=0)
                session = lines[start:end]
                next_session_t = _session_start(lines[end + 1:])
                fails = [ln for ln in session if _FAIL_RE.search(ln) and "CreateDXGIFactory2" not in ln]
                line = fails[0] if fails else ""
    # A crash before the platform finishes starting cannot produce a new mod report. Do not attach a previous session's physics
    # stack to a newer platform/audio crash (#40, #45). The session's UTC start is independent of Azimuth's uptime counter.
    start_t = _session_start(session)
    if mod_crash and ((start_t is not None and mod_t < start_t) or (next_session_t is not None and mod_t >= next_session_t)):
        mod_crash, mod_t = None, 0.0
    if not dump and not line and not mod_crash:
        return None
    return Crash(folder, line, dump, dump_t or log_t or mod_t, max(dump_t, mod_t, log_t if line else 0.0), mod_crash)


def _session_info(log_text):
    """-> (system, gpu, driver) from the last session in azimuth.log; '' for what it does not say."""
    system = gpu = driver = ""
    for ln in _last_session(log_text):
        m = _BEGAN_RE.search(ln)
        if m:
            system = re.sub(r"^Azimuth \S+ \([^)]*\) on ", "", m.group(1)).strip()
        m = _GPU_RE.search(ln)
        if m:
            gpu, driver = m.group(1), m.group(2)
    return system, gpu, driver


def check(settings, now=None):
    """-> the Crash to offer a report for, or None. The first call only notes the time (crashes from before the launcher knew about reports
    are not offered); with the question turned off nothing is offered and the time moves on, so turning it on later does not bring old ones."""
    now = time.time() if now is None else now
    if not settings["crash_handled"] or not settings["crash_report_ask"]:
        settings["crash_handled"] = now
        settings.save()
        return None
    folder = settings["azimuth_dir"]
    # A later launch supersedes old dump files kept beside the cumulative log.
    # Those files must not turn a healthy session's exit into a new crash report.
    latest = _last_session(_read_tail(os.path.join(logs_dir(folder), "azimuth.log"), LIMITS["azimuth.log"]))
    start = _session_start(latest)
    since = max(settings["crash_handled"], start or 0)
    return find_crash(folder, since)


def handled(settings, crash):
    settings["crash_handled"] = max(settings["crash_handled"], crash.seen)
    settings.save()


class Report:
    def __init__(self, fields, files, dump, dump_path, dump_note="", attachments=(), notes=()):
        self.fields = fields                  # [(label, value)] in the order the player sees them
        self.files = files                    # [(name, path, text sent)]
        self.dump = dump                      # bytes or None
        self.dump_path = dump_path
        self.dump_note = dump_note
        self.attachments = attachments       # [(name, local path, bytes)] for matching secondary dumps
        self.notes = notes

    def sizes(self, preview_dir=None):
        """[(label, path, bytes sent)] for the player's list of files. With `preview_dir` the text files are written there exactly as they
        will be sent (scrubbed, cut) and the paths point at those copies, so what the player opens is what leaves the computer."""
        out = []
        for n, p, t in self.files:
            if not t:
                continue                    # a log that does not exist is not listed
            if preview_dir:
                try:
                    os.makedirs(preview_dir, exist_ok=True)
                    p = os.path.join(preview_dir, n)
                    with open(p, "w", encoding="utf-8", newline="") as f:
                        f.write(t)
                except OSError:
                    p = None
            out.append((n, p, len(t.encode())))
        if self.dump is not None:
            out.insert(1, ("crashdump", self.dump_path, len(self.dump)))
        elif self.dump_note:
            out.insert(1, ("Crash dump not attached: " + self.dump_note, self.dump_path, 0))
        out.extend((name, path, len(data)) for name, path, data in self.attachments)
        out.extend((note, None, 0) for note in self.notes)
        return out

    def payload(self):
        names = {"Date": "date", "System": "system", "GPU": "gpu", "Driver's version": "driver", "Launcher version": "launcher",
                 "Azimuth version": "azimuth", "FH2 Extra Options version": "extraOptions", "Your FH2MP name": "name"}
        body = {"v": 1, "fields": {names[k]: v for k, v in self.fields}, "files": {n: t for n, _p, t in self.files if t}}
        if self.dump is not None:
            body["dump"] = base64.b64encode(self.dump).decode()
        elif self.dump_note:
            body["dumpStatus"] = self.dump_note
        if self.attachments:
            body["attachments"] = {name: base64.b64encode(data).decode() for name, _, data in self.attachments}
        return body


def diagnostic_logs(folder, used):
    """Known diagnostic folders only: no saves, session/config files, binaries, or unrelated crash sessions."""
    root = os.path.realpath(folder)
    names = set(LIMITS)
    for pattern in ("*.log", "data/logs/*.log", "data/bin/*.log", "fh2-mods/*.log", "fh2-mods/*/*.log", "fh2extra/*.log"):
        found = sorted(glob.glob(os.path.join(folder, pattern)))
        if pattern == "fh2-mods/*/*.log":
            # Mod Platform 2.8.0 keeps a copy of every session's log in fh2-mods/platform-logs: only the newest few belong to a report
            kept = [x for x in found if os.path.basename(os.path.dirname(x)).lower() != "platform-logs"]
            newest = sorted((x for x in found if x not in kept), key=os.path.getmtime, reverse=True)[:PLATFORM_LOGS_KEPT]
            found = kept + sorted(newest)
        for path in found:
            if re.fullmatch(r"infinity(?:[.-].*)?\.log", os.path.basename(path), re.IGNORECASE):
                continue
            real = os.path.realpath(path)
            if real in used or os.path.commonpath((root, real)) != root or not os.path.isfile(path):
                continue
            used.add(real)
            base = re.sub(r"[^A-Za-z0-9_.-]|\.{2,}", "_", os.path.basename(path))[-70:]
            if not base[0].isalnum():
                base = "log-" + base
            name = base
            stem, ext = os.path.splitext(base)
            number = 2
            while name.lower() in names:
                name = f"{stem}-{number}{ext}"
                number += 1
            names.add(name.lower())
            yield name, path


def build(lc, crash):
    """The report for `crash`: the fields and files exactly as they will be sent (paths scrubbed, big logs cut to their end)."""
    folder = crash.folder
    az_path = os.path.join(logs_dir(folder), "azimuth.log")
    full = scrub(_read_tail(az_path, 4_000_000))                     # the session line is far from the end in a long session
    system, gpu, driver = _session_info(full)
    az = _cut(full, LIMITS["azimuth.log"])
    info = azimuth.inspect(folder, lc.builds())
    eo_installed, eo_version, _t = shim.extra_options_installed(folder)
    launcher = __version__ + (" (dev)" if paths.is_dev() else "")
    when = datetime.datetime.fromtimestamp(crash.when, datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    fields = [
        ("Date", when),
        ("System", system or f"{platform.system()} {platform.release()}"),
        ("GPU", gpu or "unknown"),
        ("Driver's version", driver or "unknown"),
        ("Launcher version", launcher),
        ("Azimuth version", info.version or "unknown"),
        ("FH2 Extra Options version", (eo_version or "unknown") if eo_installed else "not installed"),
        ("Your FH2MP name", (lc.account or {}).get("gamertag") or lc.settings["gamertag"] or "unknown"),
    ]
    files = [("azimuth.log", az_path, az)]
    extra = [("fh2mp.log", os.path.join(shim.bin_dir(folder), "fh2mp.log")), ("platform.log", os.path.join(folder, "fh2-mods", "platform.log"))]
    if crash.mod_crash:
        extra.append(("mod-crash.txt", crash.mod_crash))
    extra.append(("launcher.log", os.path.join(paths.cache_dir(), "launcher.log")))
    used = {os.path.realpath(p) for _, p, _ in files} | {os.path.realpath(p) for _, p in extra}
    extra.extend(diagnostic_logs(folder, used))
    candidates = [("azimuth.log", az_path), *extra]
    candidates = [(name, path) for name, path in candidates if os.path.isfile(path)]
    skipped = max(0, len(candidates) - (MAX_FILES - 2))
    candidates = candidates[:MAX_FILES - 2]
    per_file = min(MAX_FILE, MAX_TEXT // max(1, len(candidates)))
    files = [(name, path, _cut(full if name == "azimuth.log" else scrub(_read_tail(path, per_file)), per_file))
             for name, path in candidates]
    dump, dump_note = None, "No dump file found"
    if crash.dump:
        try:
            size = os.path.getsize(crash.dump)
            if size <= MAX_DUMP:
                with open(crash.dump, "rb") as f:
                    dump = f.read(MAX_DUMP + 1)
                if len(dump) > MAX_DUMP:
                    dump = None
                    dump_note = f"Dump exceeds {MAX_DUMP // (1 << 20)} MB limit"
            else:
                dump_note = f"Dump is {human_size(size)}; exceeds {MAX_DUMP // (1 << 20)} MB limit"
        except OSError:
            dump_note = "Dump file could not be read"
    attachments, notes = [], []
    if skipped:
        notes.append(f"{skipped} additional logs exceed the 64-file report limit")
    if crash.mod_crash:
        path = os.path.splitext(crash.mod_crash)[0] + ".dmp"
        if os.path.isfile(path) and os.path.realpath(path) != os.path.realpath(crash.dump or ""):
            remaining = MAX_DUMP - len(dump or b"")
            try:
                if os.path.getsize(path) > remaining:
                    notes.append("Mod Platform dump exceeds the combined 32 MiB dump limit")
                else:
                    with open(path, "rb") as f:
                        data = f.read(remaining + 1)
                    if len(data) <= remaining and data.startswith(b"MDMP"):
                        attachments.append(("mod-crash.dmp", path, data))
            except OSError:
                notes.append("Mod Platform dump could not be read")
    return Report(fields, files, dump, crash.dump, dump_note if dump is None else "", attachments, notes)


def human_size(n):
    return f"{n / 1024:.0f} KB" if n >= 1024 else f"{n} B"
