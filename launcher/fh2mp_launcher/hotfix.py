"""The hotfix document: what the launcher does about a crash between releases, without shipping new code.

The server hands out {"doc": base64 JSON, "sig": base64 Ed25519 signature} at /v1/hotfix (signed with release/hotfix.py; the public key is
HOTFIX_PUBKEY_B64). The document is data only:

    {"schema": 1, "serial": 3, "env": "prod", "expires": "2026-12-01T00:00:00Z", "notice": "...",
     "rules": [{"id": "honk-speedometer", "why": "crash at 0xdffe... (issue #356)",
                "match": {"launcher": ">=1.1.5", "shim": "<=1.1.6", "os": ["windows"], "mods": ["<sha256 of a DLL>"]},
                "switches": ["NO_HONK"],                                  -> fh2mp-session.txt switches= (shim/src/session_switches.inc)
                "disableMods": [{"sha256": "...", "reason": "..."}],      -> fh2-mods\\platform-disabled.txt, while the rule matches
                "refusePlay": "text"}]}                                   -> Play is refused with that text

A rule applies when every part of its match holds (an absent part holds). The newest serial seen is kept: an older document (a replayed
one) is refused, and the last good copy is used when the server cannot be reached. Mods this launcher turned off are turned back on when no
rule names them any more; a mod the player turned off stays off.
"""
import base64
import calendar
import hashlib
import json
import os
import re
import sys
import time

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

from . import logs, paths
from .constants import HOTFIX_PUBKEY_B64, MODS_DIR
from .updater import vkey

CACHE_FILE = "hotfix.json"          # the last good signed document, in the cache folder
OURS_FILE = "hotfix-disabled.txt"   # the mods this launcher put on the platform's disabled list, one file name per line
DISABLED_LIST = "platform-disabled.txt"
SWITCH_RE = re.compile(r"^NO_[A-Z0-9_]{1,29}$")
SHA_RE = re.compile(r"^[0-9a-f]{64}$")
MAX_RULES = 64


class HotfixError(Exception):
    pass


def verify(signed, pubkey_b64=None):
    """{"doc", "sig"} -> the document, checked: signature, schema, serial, expiry format and every rule's shape."""
    pub = pubkey_b64 if pubkey_b64 is not None else HOTFIX_PUBKEY_B64
    try:
        raw = base64.b64decode(signed["doc"], validate=True)
        sig = base64.b64decode(signed["sig"], validate=True)
    except (KeyError, TypeError, ValueError):
        raise HotfixError("the hotfix document is not in the signed form") from None
    try:
        Ed25519PublicKey.from_public_bytes(base64.b64decode(pub)).verify(sig, raw)
    except (InvalidSignature, ValueError):
        raise HotfixError("the hotfix document's signature is not valid") from None
    try:
        doc = json.loads(raw)
    except ValueError:
        raise HotfixError("the hotfix document is not JSON") from None
    if not isinstance(doc, dict) or doc.get("schema") != 1 or not isinstance(doc.get("serial"), int) or doc["serial"] < 1:
        raise HotfixError("the hotfix document is not in a format this launcher understands")
    expires_at(doc)
    rules = doc.get("rules", [])
    if not isinstance(rules, list) or len(rules) > MAX_RULES:
        raise HotfixError("the hotfix document's rules are not a list")
    for r in rules:
        _check_rule(r)
    return doc


def _check_rule(r):
    if not isinstance(r, dict) or not isinstance(r.get("id"), str) or not r["id"]:
        raise HotfixError("a hotfix rule has no id")
    m = r.get("match", {})
    if not isinstance(m, dict) or set(m) - {"launcher", "shim", "os", "mods"}:
        raise HotfixError(f"hotfix rule {r['id']}: unknown match")
    for k in ("launcher", "shim"):
        if k in m and not _range_ok(m[k]):
            raise HotfixError(f"hotfix rule {r['id']}: {k} must be like '<=1.1.5'")
    if "os" in m and not (isinstance(m["os"], list) and all(o in ("windows", "linux") for o in m["os"])):
        raise HotfixError(f"hotfix rule {r['id']}: os must list windows and/or linux")
    if "mods" in m and not (isinstance(m["mods"], list) and all(isinstance(s, str) and SHA_RE.match(s) for s in m["mods"])):
        raise HotfixError(f"hotfix rule {r['id']}: mods must be SHA-256 hex digests")
    sw = r.get("switches", [])
    if not isinstance(sw, list) or not all(isinstance(s, str) and SWITCH_RE.match(s) for s in sw):
        raise HotfixError(f"hotfix rule {r['id']}: switches must be NO_ names")
    dm = r.get("disableMods", [])
    if not isinstance(dm, list) or not all(isinstance(d, dict) and SHA_RE.match(str(d.get("sha256", ""))) for d in dm):
        raise HotfixError(f"hotfix rule {r['id']}: disableMods needs a sha256 for each mod")
    if "refusePlay" in r and not (isinstance(r["refusePlay"], str) and r["refusePlay"]):
        raise HotfixError(f"hotfix rule {r['id']}: refusePlay must be a text")


_RANGE = re.compile(r"^(<=|>=|<|>|==)?\s*(\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?)$")


def _range_ok(spec):
    return isinstance(spec, str) and all(_RANGE.match(p.strip()) for p in spec.split(","))


def version_in(version, spec):
    """'1.1.5' in '>=1.1.3,<1.1.6'. An empty version (not installed) is in no range."""
    if not version:
        return False
    have = vkey(version)[:3]
    for part in spec.split(","):
        op, v = _RANGE.match(part.strip()).groups()
        want = vkey(v)[:3]
        ok = {"<=": have <= want, ">=": have >= want, "<": have < want, ">": have > want}.get(op or "==", have == want)
        if not ok:
            return False
    return True


def expires_at(doc):
    """-> epoch seconds, or None for no expiry."""
    e = doc.get("expires")
    if e is None:
        return None
    try:
        return calendar.timegm(time.strptime(str(e), "%Y-%m-%dT%H:%M:%SZ"))
    except ValueError:
        raise HotfixError("the hotfix document's expires is not like 2026-12-01T00:00:00Z") from None


class Result:
    """What the rules that apply ask for."""

    def __init__(self, doc=None):
        self.serial = doc["serial"] if doc else 0
        self.notice = (doc or {}).get("notice", "")
        self.rules = []            # ids of the rules that apply
        self.switches = []         # NO_ names, in order, once each
        self.disable = {}          # sha256 -> reason
        self.refuse = ""

    def add(self, rule):
        self.rules.append(rule["id"])
        for s in rule.get("switches", []):
            if s not in self.switches:
                self.switches.append(s)
        for d in rule.get("disableMods", []):
            self.disable.setdefault(d["sha256"], d.get("reason", "") or rule.get("why", "") or rule["id"])
        if rule.get("refusePlay") and not self.refuse:
            self.refuse = rule["refusePlay"]


def this_os():
    return "windows" if sys.platform.startswith("win") else "linux"


def mod_hashes(folder):
    """{sha256: file name} of the DLLs in fh2-mods and of the loaders next to azimuth.exe."""
    out = {}
    places = [(os.path.join(folder, MODS_DIR), lambda n: n.lower().endswith(".dll"))]
    places.append((folder, lambda n: n.lower() in ("winmm.dll", "dwrite.dll", "d3dcompiler_47.dll")))
    for d, want in places:
        try:
            names = os.listdir(d)
        except OSError:
            continue
        for n in names:
            p = os.path.join(d, n)
            if not want(n) or not os.path.isfile(p):
                continue
            try:
                with open(p, "rb") as f:
                    out[hashlib.sha256(f.read()).hexdigest()] = os.path.relpath(p, folder).replace("\\", "/")
            except OSError as e:
                logs.swallowed("hotfix", "hotfix.hash_failed", e, file=n)
    return out


def evaluate(doc, launcher_version, shim_version, mods, os_name=None, now=None, env=None):
    """The rules of `doc` that apply to this setup. `mods` is mod_hashes(). An expired document, or one made for another environment than the
    server's (`env`, "prod" / "dev"), asks for nothing."""
    res = Result(doc)
    if doc is None:
        return res
    if doc.get("env") and env and doc["env"] != env:
        logs.warn("hotfix", "hotfix.other_env", doc_env=doc["env"], server_env=env, serial=doc["serial"])
        return Result()
    exp = expires_at(doc)
    if exp is not None and (now if now is not None else time.time()) >= exp:
        return Result()
    os_name = os_name or this_os()
    for r in doc.get("rules", []):
        m = r.get("match", {})
        if "launcher" in m and not version_in(launcher_version, m["launcher"]):
            continue
        if "shim" in m and not version_in(shim_version, m["shim"]):
            continue
        if "os" in m and os_name not in m["os"]:
            continue
        if "mods" in m and not any(s in mods for s in m["mods"]):
            continue
        res.add(r)
    return res


class Store:
    """The last good document, kept in the cache folder, and the serial rule. The file holds {"signed": ...} for a document in force, or
    {"floor": n} once the server stopped serving document n: then no document applies, and n or older is never taken again (a replay)."""

    def __init__(self, cache_dir=None):
        self.path = os.path.join(cache_dir or paths.cache_dir(), CACHE_FILE)

    def load(self):
        """-> (document in force or None, the lowest serial still taken)."""
        try:
            with open(self.path, encoding="utf-8") as f:
                kept = json.load(f)
            if isinstance(kept.get("floor"), int):
                return None, kept["floor"] + 1
            doc = verify(kept["signed"])
            return doc, doc["serial"]
        except (OSError, ValueError, KeyError, TypeError, AttributeError, HotfixError) as e:
            if not isinstance(e, FileNotFoundError):
                logs.swallowed("hotfix", "hotfix.cache_unreadable", e)
            return None, 0

    def _keep(self, obj):
        paths.atomic_write(self.path, json.dumps(obj).encode(), 0o644)

    def update(self, signed):
        """What the server sent -> the document to use. None for `signed` means the server could not be asked: the kept one stays."""
        old, floor = self.load()
        if signed is None:
            return old
        if not signed.get("doc"):
            if old:
                self._keep({"floor": old["serial"]})
                logs.info("hotfix", "hotfix.withdrawn", serial=old["serial"])
            return None
        try:
            doc = verify(signed)
        except HotfixError as e:
            logs.warn("hotfix", "hotfix.refused", error=str(e), kept_serial=old["serial"] if old else None)
            return old
        if doc["serial"] < floor:
            logs.warn("hotfix", "hotfix.older_refused", served=doc["serial"], lowest_taken=floor)
            return old
        if not old or doc["serial"] != old["serial"]:
            self._keep({"signed": signed})
            logs.info("hotfix", "hotfix.stored", serial=doc["serial"], rules=len(doc.get("rules", [])))
        return doc


def _read_lines(path):
    try:
        with open(path, encoding="utf-8-sig") as f:
            return [ln.strip() for ln in f.read().splitlines() if ln.strip()]
    except FileNotFoundError:
        return []


def apply_mods(folder, res, mods):
    """Puts the mods `res` disables on the platform's disabled list and takes off the ones this launcher put there that no rule names now.
    -> the file names turned off now (for the Play notes)."""
    mods_dir = os.path.join(folder, MODS_DIR)
    listed = _read_lines(os.path.join(mods_dir, DISABLED_LIST))
    ours = _read_lines(os.path.join(mods_dir, OURS_FILE))
    want = set()
    for sha in res.disable:
        rel = mods.get(sha, "")
        if rel.lower().startswith(MODS_DIR.lower() + "/"):       # the platform's list only covers fh2-mods; loaders are left to the mod manager
            want.add(rel.split("/", 1)[1])
    low = {n.lower() for n in listed}
    added = [n for n in sorted(want) if n.lower() not in low]
    released = [n for n in ours if n.lower() not in {w.lower() for w in want}]
    if not added and not released:
        return []
    new_list = [n for n in listed if n.lower() not in {r.lower() for r in released}] + added
    new_ours = [n for n in ours if n not in released] + added
    os.makedirs(mods_dir, exist_ok=True)
    paths.atomic_write(os.path.join(mods_dir, DISABLED_LIST), ("\n".join(new_list) + "\n").encode("utf-8"), 0o644)
    paths.atomic_write(os.path.join(mods_dir, OURS_FILE), ("\n".join(new_ours) + ("\n" if new_ours else "")).encode("utf-8"), 0o644)
    for n in added:
        logs.warn("hotfix", "hotfix.mod_disabled", mod=n, serial=res.serial, rules=",".join(res.rules))
    for n in released:
        logs.info("hotfix", "hotfix.mod_enabled", mod=n, serial=res.serial)
    return added
