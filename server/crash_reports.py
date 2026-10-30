"""Crash reports from the launcher (POST /v1/crash-reports, see launcher/fh2mp_launcher/crashreport.py): kept on disk and turned into a GitHub issue.

The issue is public, so it carries only the summary, the Mod Platform's crash report and the end of azimuth.log, each passed through redact();
fh2mp.log (server addresses, ids, requests), the platform and launcher logs and the crash dump stay in FH2MP_CRASH_DIR/<id>/ (the id is in the
issue; security audit 2026-10-25 F05). Every attachment has a download link through the website's staff-only panel.
A crash that was reported before (same exception and address) is added to the open issue
as a comment instead of a new issue; if that issue was closed, a new one is opened.

    FH2MP_CRASH_DIR       where reports are kept (needed; without it and a token the endpoint answers 503)
    FH2MP_GITHUB_TOKEN    token that may create issues in the repository (fine-grained: Issues read/write); unset = reports are only kept
    FH2MP_GITHUB_REPO     owner/name, default Charelene/FH2MP
    FH2MP_CRASH_LINK_BASE website's protected attachment URL prefix (default https://forza.scuffed.af/panel/crashes)
"""
import base64
import binascii
import json
import os
import re
import secrets
import shutil
import threading
import time
import urllib.error
import urllib.request

import auth

MAX_BODY = 96 << 20    # 32 MiB of dumps (base64) + 8 MiB of text, including worst-case JSON escaping
FIELDS = {"date": "Date", "system": "System", "gpu": "GPU", "driver": "Driver's version", "launcher": "Launcher version",
          "azimuth": "Azimuth version", "extraOptions": "FH2 Extra Options version", "name": "Your FH2MP name"}
MAX_TEXT = 8 << 20
MAX_FILE = 2 << 20
MAX_FILES = 64
MAX_DUMP = 32 << 20    # combined Azimuth and Mod Platform dumps
LINK_BASE = "https://forza.scuffed.af/panel/crashes"
_FILE_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,119}\.(?:log|txt|dmp)\Z")
KEEP_DAYS = 90
ISSUE_LABELS = ["crash-report"]          # not "bug": that label is for what players report themselves, so the two kinds can be told apart
BODY_LIMIT = 60_000                                  # GitHub refuses issue bodies over 65536 characters
_FAIL_RE = re.compile(r"\b(CRASHED|STOPPED)\b")
_ADDRESS = r"(?:[A-Za-z0-9_.-]+\+)?0x[0-9a-fA-F]+"
_CODE_RE = re.compile(r"CRASHED exception (0x[0-9a-fA-F]+) at (" + _ADDRESS + r")")
_STOP_RE = re.compile(r"STOPPED (.*?) at (" + _ADDRESS + r")")
_ID_RE = re.compile(r"^[0-9a-f]{16}$")


# Secrets and personal data that may be in a log line: e-mail addresses, IP addresses (not loopback; dotted version numbers such as a driver
# or game build are left alone), credentials in URLs, Authorization / cookie headers, key=value secrets and long base64 runs (keys, seeds).
# The launcher scrubs with the same rules (crashreport.scrub); this is the second pass before anything is public. Patterns cannot promise to catch everything (security audit 2026-10-25 F05), which is why the public issue gets only a summary.
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


def redact(text):
    """Text with the _SECRETS patterns blanked, and session tokens (fh2.*), for anything that goes into a public issue."""
    text = re.sub(r"\bfh2\.[A-Za-z0-9_.\-]{16,}", "<token>", text)
    for rx, rep in _SECRETS:
        text = rx.sub(rep, text)
    return text


def clean_line(v, limit=200):
    """One printable line: control characters (newlines too) become spaces."""
    return re.sub(r"[\x00-\x1f\x7f]+", " ", str(v)).strip()[:limit]


def file_name(name, binary=False):
    return isinstance(name, str) and bool(_FILE_RE.fullmatch(name)) and ".." not in name and name.endswith(".dmp") == binary


def decode_dump(value):
    if not isinstance(value, str) or len(value) > 4 * ((MAX_DUMP + 2) // 3):
        raise auth.AuthError(413, "too_large", "the crash dump is too big")
    try:
        data = base64.b64decode(value, validate=True)
    except (binascii.Error, ValueError):
        raise auth.AuthError(400, "bad_dump", "the crash dump is not valid base64") from None
    if len(data) > MAX_DUMP:
        raise auth.AuthError(413, "too_large", "the crash dump is too big")
    if not data.startswith(b"MDMP"):
        raise auth.AuthError(400, "bad_dump", "that is not a minidump")
    return data


def validate(d):
    """-> {"fields": {...}, "files": {name: text}, "dump": bytes | None}; raises auth.AuthError(400/413) for anything else."""
    if not isinstance(d, dict) or d.get("v") != 1:
        raise auth.AuthError(400, "bad_request", "unknown report format")
    f = d.get("fields")
    if not isinstance(f, dict):
        raise auth.AuthError(400, "bad_request", "no fields")
    fields = {k: clean_line(f.get(k, "")) for k in FIELDS}
    files, total_text = {}, 0
    raw_files, raw_attachments = d.get("files") or {}, d.get("attachments") or {}
    if not isinstance(raw_files, dict) or not isinstance(raw_attachments, dict) or len(raw_files) + len(raw_attachments) > MAX_FILES:
        raise auth.AuthError(400, "bad_request", "invalid file list")
    for name, text in raw_files.items():
        if not file_name(name) or not isinstance(text, str):
            raise auth.AuthError(400, "bad_request", "unknown file")
        total_text += len(text.encode("utf-8"))
        if len(text.encode("utf-8")) > MAX_FILE or total_text > MAX_TEXT:
            raise auth.AuthError(413, "too_large", f"{name} is too big")
        files[name] = text
    dump = None
    if d.get("dump"):
        dump = decode_dump(d["dump"])
    attachments = {}
    total_dump = len(dump or b"")
    for name, value in raw_attachments.items():
        if not file_name(name, binary=True) or name == "crash.dmp":
            raise auth.AuthError(400, "bad_request", "invalid attachment name")
        attachments[name] = decode_dump(value)
        total_dump += len(attachments[name])
        if total_dump > MAX_DUMP:
            raise auth.AuthError(413, "too_large", "the crash dumps are too big")
    if not any(files.values()) and dump is None and not attachments:
        raise auth.AuthError(400, "bad_request", "empty report")
    return {"fields": fields, "files": files, "dump": dump, "attachments": attachments, "dumpStatus": clean_line(d.get("dumpStatus", ""))}


def failure_line(azimuth_log):
    """First failure in the most recent failing session, before failures in crash handling itself.

    A subsequent launcher-only session may already have started when the report is submitted. Keep the previous failure in that case,
    but do not let a secondary Wine/dbghelp or Azimuth worker exception replace its cause (#37, #39–42).
    """
    failure = ""
    first_in_session = ""
    for ln in azimuth_log.splitlines():
        if re.search(r"\bsession \S+ began ", ln):
            first_in_session = ""
        if not first_in_session and _FAIL_RE.search(ln) and "CreateDXGIFactory2" not in ln:
            first_in_session = failure = ln
    if not failure:
        return ""
    ln = failure
    ln = re.sub(r"^\d+ tid=\d+ ", "", ln)
    return clean_line(ln.split(" (modified:")[0], 300)


def signature(line):
    """-> (key, title) naming a crash by its exception and address, so the same crash from different players lands in one issue."""
    m = _CODE_RE.search(line)
    if m:
        return f"{m.group(1).lower()}@{m.group(2).lower()}", f"Crash {m.group(1)} at {m.group(2)}"
    m = _STOP_RE.search(line)
    if m:
        what = re.sub(r"[^\w .,:+()/-]", "", re.sub(r"0x[0-9a-fA-F]+", "0x", m.group(1)))[:70].strip()
        return f"stop:{what}@{m.group(2).lower()}", f"Azimuth stop at {m.group(2)}: {what}"
    return "", "Game crash (no failure line in azimuth.log)"


def _code(v):
    """Inline code that cannot break out of its table cell or ping anyone."""
    return "`" + clean_line(v).replace("`", "'").replace("|", "/") + "`"


def _fence(text):
    longest = max((len(m) for m in re.findall(r"`+", text)), default=0)
    return "`" * max(3, longest + 1)


def _tail_block(title, text, lines, budget):
    text = "\n".join(text.splitlines()[-lines:])
    if len(text) > budget:
        text = text[-budget:].split("\n", 1)[-1]
    if not text.strip():
        return ""
    f = _fence(text)
    return f"<details><summary>{title}</summary>\n\n{f}text\n{text}\n{f}\n\n</details>\n"


def summary_table(rid, r, line):
    rows = [("Crash", redact(line) or "not found in azimuth.log"), *((FIELDS[k], redact(r["fields"][k])) for k in FIELDS), ("Report id", rid)]
    return "| | |\n|-|-|\n" + "".join(f"| {k} | {_code(v) if v else '-'} |\n" for k, v in rows)


def file_links(rid, r, link_base=LINK_BASE):
    names = [n for n, t in r["files"].items() if t]
    if r["dump"] is not None:
        names.append("crash.dmp")
    names.extend(r.get("attachments", {}))
    links = []
    labels = set()
    for name in sorted(names):
        # Older launchers prefixed every additional log with a path hash.
        base = re.sub(r"^extra-[0-9a-f]{10}-", "", name)
        if re.fullmatch(r"infinity(?:[.-].*)?\.log", base, re.IGNORECASE):
            continue
        label = base
        stem, ext = os.path.splitext(base)
        number = 2
        while label.lower() in labels:
            label = f"{stem}-{number}{ext}"
            number += 1
        labels.add(label.lower())
        links.append(f"- [{label}]({link_base.rstrip('/')}/{rid}/{name}/download/)")
    return "\n".join(links)


def issue_body(rid, r, line, link_base=LINK_BASE):
    head = "This issue was created automatically by launcher's crash reporter.\n\n" + summary_table(rid, r, line)
    blocks = [_tail_block("FH2 Mod Platform crash report", redact(r["files"].get("mod-crash.txt", "")), 80, 8_000),
              _tail_block("azimuth.log, last 40 lines", redact(r["files"].get("azimuth.log", "")), 40, 10_000)]
    return (head + "\n\n" + file_links(rid, r, link_base) + "\n\n" + "\n".join(b for b in blocks if b))[:BODY_LIMIT]


def comment_body(rid, r, link_base=LINK_BASE):
    f = r["fields"]
    return ("Another report of this crash.\n\n| | |\n|-|-|\n" + "".join(
        f"| {FIELDS[k]} | {_code(redact(f[k])) if f[k] else '-'} |\n" for k in ("date", "system", "gpu", "driver", "launcher", "azimuth", "extraOptions", "name"))
        + f"| Report id | `{rid}` |\n\n" + file_links(rid, r, link_base))


class GitHub:
    """The three issue calls this needs. `urlopen` is replaceable for tests."""

    def __init__(self, repo, token, urlopen=urllib.request.urlopen):
        self.repo, self.token, self.urlopen = repo, token, urlopen

    def _call(self, method, path, body=None):
        req = urllib.request.Request(f"https://api.github.com/repos/{self.repo}{path}", method=method,
                                     data=json.dumps(body).encode() if body is not None else None)
        req.add_header("Authorization", f"Bearer {self.token}")
        req.add_header("Accept", "application/vnd.github+json")
        req.add_header("X-GitHub-Api-Version", "2022-11-28")
        req.add_header("User-Agent", "fh2mp-server")
        with self.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read() or b"{}")

    def issue_state(self, number):
        return self._call("GET", f"/issues/{int(number)}").get("state", "")

    def create_issue(self, title, body):
        return int(self._call("POST", "/issues", {"title": title, "body": body, "labels": ISSUE_LABELS})["number"])

    def comment(self, number, body):
        self._call("POST", f"/issues/{int(number)}/comments", {"body": body})


class CrashReports:
    def __init__(self, directory, github=None, log=lambda m: None, background=True, link_base=LINK_BASE):
        self.dir, self.github, self.log, self.background = directory, github, log, background
        self._lock = threading.Lock()
        self.link_base = link_base

    @classmethod
    def from_env(cls, log=lambda m: None):
        d, token = os.environ.get("FH2MP_CRASH_DIR", ""), os.environ.get("FH2MP_GITHUB_TOKEN", "")
        if not d and not token:
            return None
        gh = GitHub(os.environ.get("FH2MP_GITHUB_REPO", "Charelene/FH2MP"), token) if token else None
        return cls(d, gh, log, link_base=os.environ.get("FH2MP_CRASH_LINK_BASE", LINK_BASE))

    # ---- intake
    def submit(self, payload):
        """Check and keep one report, start the GitHub part. -> the report id."""
        r = validate(payload)
        rid = secrets.token_hex(8)
        line = failure_line(r["files"].get("azimuth.log", ""))
        if self.dir:
            self._store(rid, r, line)
        if self.github:
            args = (rid, r, line)
            if self.background:
                threading.Thread(target=self.publish, args=args, daemon=True).start()
            else:
                self.publish(*args)
        self.log(f"  crash report {rid} {line[:100]!r}")
        return rid

    def _store(self, rid, r, line):
        d = os.path.join(self.dir, rid)
        try:
            os.makedirs(d, mode=0o700)
            for name, text in r["files"].items():
                with open(os.path.join(d, name), "w", encoding="utf-8") as f:
                    f.write(text)
            if r["dump"] is not None:
                with open(os.path.join(d, "crash.dmp"), "wb") as f:
                    f.write(r["dump"])
            for name, data in r.get("attachments", {}).items():
                with open(os.path.join(d, name), "wb") as f:
                    f.write(data)
            self._write_meta(rid, {"received": int(time.time()), "fields": r["fields"], "failure": line, "issue": None,
                                   "dumpStatus": r.get("dumpStatus", "")})
            self._prune()
        except OSError as e:
            self.log(f"  crash report {rid}: could not store it: {e}")
            raise auth.AuthError(503, "storage_failed", "could not store the crash report") from None

    def download(self, rid, name):
        if not self.dir or not _ID_RE.fullmatch(rid) or not (file_name(name) or file_name(name, binary=True)):
            raise auth.AuthError(404, "not_found", "no such attachment")
        folder = os.path.join(self.dir, rid)
        path = os.path.join(folder, name)
        if os.path.islink(folder) or os.path.islink(path):
            raise auth.AuthError(404, "not_found", "no such attachment")
        limit = MAX_DUMP if name.endswith(".dmp") else MAX_FILE
        try:
            with open(path, "rb") as f:
                data = f.read(limit + 1)
        except OSError:
            raise auth.AuthError(404, "not_found", "no such attachment") from None
        if len(data) > limit:
            raise auth.AuthError(413, "too_large", "the attachment is too big")
        return data

    def _write_meta(self, rid, meta):
        p = os.path.join(self.dir, rid, "report.json")
        with open(p + ".tmp", "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=1)
        os.replace(p + ".tmp", p)

    def _prune(self):
        cutoff = time.time() - KEEP_DAYS * 86400
        for n in os.listdir(self.dir):
            p = os.path.join(self.dir, n)
            if _ID_RE.match(n) and os.path.isdir(p) and os.path.getmtime(p) < cutoff:
                shutil.rmtree(p, ignore_errors=True)

    # ---- GitHub
    def _index_path(self):
        return os.path.join(self.dir, "issues.json") if self.dir else ""

    def _index(self):
        try:
            with open(self._index_path(), encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError, TypeError):
            return {}

    def publish(self, rid, r, line):
        """Comment on the open issue for this crash, or open one. Never raises: a failure is logged and the report stays on disk."""
        try:
            key, title = signature(line)
            with self._lock:                     # two players crashing at once must not open two issues
                index = self._index() if self.dir else {}
                number = index.get(key) if key else None
                if number and self.github.issue_state(number) == "open":
                    self.github.comment(number, comment_body(rid, r, self.link_base))
                else:
                    number = self.github.create_issue(title, issue_body(rid, r, line, self.link_base))
                    if key and self.dir:
                        index[key] = number
                        with open(self._index_path() + ".tmp", "w", encoding="utf-8") as f:
                            json.dump(index, f)
                        os.replace(self._index_path() + ".tmp", self._index_path())
            if self.dir:
                try:
                    meta_path = os.path.join(self.dir, rid, "report.json")
                    with open(meta_path, encoding="utf-8") as f:
                        meta = json.load(f)
                    meta["issue"] = number
                    self._write_meta(rid, meta)
                except (OSError, ValueError):
                    pass
            self.log(f"  crash report {rid}: issue #{number}")
            return number
        except (urllib.error.URLError, OSError, ValueError, KeyError) as e:
            self.log(f"  crash report {rid}: GitHub failed: {type(e).__name__}: {e}")
            return None
