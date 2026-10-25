"""Azimuth: find it, recognise the build, read and edit the little of its configuration that matters to us."""
import glob
import hashlib
import os
import re
from dataclasses import dataclass, field

from . import paths
from .constants import BUILTIN_AZIMUTH_BUILDS

EXE = "azimuth.exe"


@dataclass
class AzimuthInfo:
    folder: str
    found: bool = False
    sha256: str = ""
    version: str = ""                 # read from the exe ("0.9.10"), for display; support is decided by the hash
    supported: bool = False
    supported_version: str = ""
    game_folder: str = ""
    game_ok: bool = False
    notes: list = field(default_factory=list)

    @property
    def exe(self):
        return os.path.join(self.folder, EXE)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# Azimuth keeps its version next to "...: Azimuth " in a message ("\x000.9.10\x00\x00: Azimuth ") and in its settings migration note
_VERSION_RES = [re.compile(rb"\x00(\d{1,3}\.\d{1,3}\.\d{1,3})\x00+: Azimuth "), re.compile(rb"\bby (\d{1,3}\.\d{1,3}\.\d{1,3})\x00"),
                re.compile(rb"\bv(\d{1,3}\.\d{1,3}\.\d{1,3})\b")]


def exe_version(path):
    """The version string Azimuth carries (for display; support is decided by the hash); '' if none is found."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return ""
    for rx in _VERSION_RES:
        m = rx.search(data)
        if m:
            return m.group(1).decode()
    return ""


def read_ini(folder):
    """-> {section: {key: value}} from data/azimuth.ini (UTF-16 with BOM); {} if absent."""
    out, sec = {}, None
    try:
        text = _read_text(os.path.join(folder, "data", "azimuth.ini"))
    except OSError:
        return out
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("[") and line.endswith("]"):
            sec = out.setdefault(line[1:-1], {})
        elif "=" in line and sec is not None and not line.startswith(";"):
            k, v = line.split("=", 1)
            sec[k.strip()] = v.strip()
    return out


def _read_text(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:2] in (b"\xff\xfe", b"\xfe\xff"):
        return raw.decode("utf-16")
    if raw[:3] == b"\xef\xbb\xbf":
        return raw[3:].decode("utf-8")
    return raw.decode("utf-8", "replace")


def inspect(folder, builds=None):
    """Looks at an Azimuth folder. `builds`: [{"version", "sha256"}] the shim is verified against (server list + built-in)."""
    info = AzimuthInfo(folder=folder or "")
    if not folder or not os.path.isfile(os.path.join(folder, EXE)):
        info.notes.append("azimuth.exe was not found in that folder")
        return info
    info.found = True
    info.sha256 = sha256_file(info.exe)
    info.version = exe_version(info.exe)
    for b in list(builds or []) + BUILTIN_AZIMUTH_BUILDS:
        if b.get("sha256", "").lower() == info.sha256:
            info.supported, info.supported_version = True, b.get("version", "")
            break
    if not info.supported:
        info.notes.append("this Azimuth build has not been tested with FH2MP")
    ini = read_ini(folder)
    title = next((v for k, v in ini.items() if k.startswith("title-")), {})
    info.game_folder = title.get("folder", "")
    if info.game_folder:
        info.game_ok = _game_folder_exists(info.game_folder)
        if not info.game_ok:
            info.notes.append("the game folder set in Azimuth was not found from here")
    else:
        info.notes.append("Azimuth has no game folder set yet: run Azimuth's own launcher once and point it at Forza Horizon 2")
    return info


def _game_folder_exists(path):
    """On Linux the folder is a Windows path inside a Wine prefix (C:\\Games\\...), which cannot be checked from here."""
    if os.name != "nt" and re.match(r"^[A-Za-z]:[\\/]", path):
        return True
    return os.path.isdir(path)


def find_candidates(extra=()):
    """Folders that contain azimuth.exe in the places people unpack it (one or two levels deep)."""
    home = os.path.expanduser("~")
    roots = [*extra, os.path.join(home, "Downloads"), os.path.join(home, "Desktop"), os.path.join(home, "Documents"),
             os.path.join(home, "Games"), "C:\\Games", "C:\\Azimuth", os.getcwd()]
    seen, out = set(), []
    for d in steam_shortcut_folders():
        real = os.path.realpath(d)
        if real not in seen:
            seen.add(real)
            out.append(d)
    for root in roots:
        if not root or not os.path.isdir(root):
            continue
        try:
            first = [os.path.join(root, n) for n in os.listdir(root)]
        except OSError:
            continue
        for d in [root, *first]:
            for cand in (d, *([os.path.join(d, n) for n in _listdir(d)] if os.path.isdir(d) and d != root else [])):
                if os.path.isfile(os.path.join(cand, EXE)):
                    real = os.path.realpath(cand)
                    if real not in seen:
                        seen.add(real)
                        out.append(cand)
    return out


def _listdir(d):
    try:
        return [n for n in os.listdir(d) if not n.startswith(".")][:200]
    except OSError:
        return []


# ---- configuration we write

INGAME_NAME_MAX = 15
INGAME_FALLBACK = "Player 1"          # Azimuth's own default, which it also shows for any player_name it does not accept


def ingame_name(gamertag):
    """The gamertag as Azimuth accepts it for `player_name`: 1 to 15 letters, digits and single spaces, starting with a letter (the
    rule is written in azimuth.exe's default ini; anything else shows as "Player 1" in the game, 2026-10-23). Older accounts have '_', '.'
    and '-' in their names and some start with a digit: separators become spaces, other symbols go, and a name that would start with a
    digit gets an X in front ("4K" -> "X4K")."""
    text = re.sub(r"[._-]+", " ", gamertag or "")
    text = re.sub(r"[^A-Za-z0-9 ]", "", text)
    text = re.sub(r" +", " ", text).strip()
    if text and not text[0].isalpha():
        text = "X" + text
    return text[:INGAME_NAME_MAX].rstrip() or INGAME_FALLBACK


def set_player_name(folder, name):
    """Sets player_name in [launcher] of data/azimuth.ini, keeping the file's encoding and line endings. Returns True if it changed."""
    path = os.path.join(folder, "data", "azimuth.ini")
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError:
        return False
    enc = "utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8"
    text = raw.decode(enc)
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(nl)
    sec_start = next((i for i, l in enumerate(lines) if l.strip() == "[launcher]"), None)
    if sec_start is None:
        return False
    end = next((i for i in range(sec_start + 1, len(lines)) if lines[i].strip().startswith("[")), len(lines))
    for i in range(sec_start + 1, end):
        if lines[i].split("=", 1)[0].strip() == "player_name":
            if lines[i] == f"player_name={name}":
                return False
            lines[i] = f"player_name={name}"
            break
    else:
        lines.insert(end if end == len(lines) else end, f"player_name={name}")
    out = nl.join(lines)
    data = out.encode("utf-16") if enc == "utf-16" else out.encode("utf-8")      # python's utf-16 writes the BOM + native order (LE)
    paths.atomic_write(path, data, 0o644)
    return True


def write_gamerpic(folder, png):
    """Azimuth shows Gamerpic.png from its own folder as the local player's picture; FH2 Extra Options, next to it, reads the same file."""
    paths.atomic_write(os.path.join(folder, "Gamerpic.png"), png, 0o644)


# ---- the game install: found and written into Azimuth's own settings, so the player never has to

GAME_ID = "1a96545b"                   # Forza Horizon 2 in azimuth.ini ([title-1a96545b])
GAME_NAME = "Forza Horizon 2"


def _steam_roots():
    return [r for r in (os.path.expanduser(x) for x in ("~/.local/share/Steam", "~/.steam/steam", "~/.var/app/com.valvesoftware.Steam/.local/share/Steam"))
            if os.path.isdir(r)]


def is_game_dir(p):
    """The extracted Xbox One package: a Mount folder with the game exe, or at least Mount + EmbeddedXvd."""
    return os.path.isfile(os.path.join(p, "Mount", "forza_durango_release_final.exe")) or (
        os.path.isdir(os.path.join(p, "Mount")) and os.path.isdir(os.path.join(p, "EmbeddedXvd")))


def _subdirs(d):
    try:
        return [os.path.join(d, n) for n in sorted(os.listdir(d)) if not n.startswith(".") and os.path.isdir(os.path.join(d, n))]
    except OSError:
        return []


def find_game_installs():
    """-> [(host path, path as Azimuth/Windows sees it)]: Proton prefixes first (C:\\Games\\ForzaHorizon2), then plain folders (Z:\\home\\...)."""
    out, seen = [], set()

    def add(host, win):
        try:
            real = os.path.realpath(host)
            if real not in seen and is_game_dir(host):
                seen.add(real)
                out.append((host, win))
        except (OSError, ValueError):
            pass            # a junction or link Windows will not let us follow (C:\Documents and Settings): not where the game is
    for root in _steam_roots():
        for pre in _subdirs(os.path.join(root, "steamapps", "compatdata")):
            c = os.path.join(pre, "pfx", "drive_c")
            for top in _subdirs(c):
                for d in [top, *_subdirs(top)]:
                    add(d, "C:\\" + os.path.relpath(d, c).replace("/", "\\"))
    if os.name == "nt":
        for drive in "CDEFG":
            for top in (f"{drive}:\\", f"{drive}:\\Games", f"{drive}:\\XboxGames"):
                for d in [top, *_subdirs(top)]:
                    add(d, os.path.abspath(d))
    else:
        home = os.path.expanduser("~")
        for top in (home, os.path.join(home, "Games"), os.path.join(home, "Downloads"), os.path.join(home, "Documents"), os.path.join(home, "Desktop")):
            for d in [top, *_subdirs(top)]:
                for dd in [d, *_subdirs(d)]:
                    add(dd, "Z:" + dd.replace("/", "\\"))
    return out


def configure_game(folder, win_path):
    """Writes the game folder into Azimuth's data/azimuth.ini ([launcher] game=<id>, [title-<id>] folder=...). Keeps the file's encoding."""
    path = os.path.join(folder, "data", "azimuth.ini")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError:
        raw = "[launcher]\r\n".encode("utf-16")
    enc = "utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8"
    text = raw.decode(enc)
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(nl)

    def set_key(section, key, value):
        sec = next((i for i, l in enumerate(lines) if l.strip() == f"[{section}]"), None)
        if sec is None:
            lines[:] = [l for l in lines if l != ""] + [f"[{section}]", f"{key}={value}"]
            return
        end = next((i for i in range(sec + 1, len(lines)) if lines[i].strip().startswith("[")), len(lines))
        for i in range(sec + 1, end):
            if lines[i].split("=", 1)[0].strip() == key:
                lines[i] = f"{key}={value}"
                return
        lines.insert(end, f"{key}={value}")
    set_key("launcher", "game", GAME_ID)
    set_key(f"title-{GAME_ID}", "folder", win_path)
    set_key(f"title-{GAME_ID}", "content", "")
    set_key(f"title-{GAME_ID}", "name", GAME_NAME)
    out = nl.join(lines).rstrip(nl) + nl
    paths.atomic_write(path, out.encode(enc), 0o644)


def has_game_configured(folder):
    ini = read_ini(folder)
    return bool(ini.get("launcher", {}).get("game")) and bool(ini.get(f"title-{ini.get('launcher', {}).get('game')}", {}).get("folder"))


def steam_shortcut_folders():
    """Folders of azimuth.exe entries in the player's Steam non-Steam shortcuts (shortcuts.vdf is binary; the exe path is a plain string)."""
    found = []
    for root in _steam_roots():
        for vdf in glob.glob(os.path.join(root, "userdata", "*", "config", "shortcuts.vdf")):
            try:
                with open(vdf, "rb") as f:
                    data = f.read()
            except OSError:
                continue
            for m in re.finditer(rb'([A-Za-z]:[\\/][^\x00"]*?|/[^\x00"]*?)azimuth\.exe', data, re.I):
                d = os.path.dirname(m.group(0).decode("utf-8", "replace"))
                if os.path.isfile(os.path.join(d, EXE)) and d not in found:
                    found.append(d)
    return found


# ---- progress: Azimuth keeps saves per Xbox user id. 0.9.10: data/titles/<id>/saves/player-N/User_<HEX>; 0.9.7 and older: data/saves/player-N/User_<HEX>
# (0.9.10 moves those on its first start and keeps the old folder as data/saves-backup-<time>). Vanilla Azimuth uses one fixed id, an FH2MP account
# has its own, so without this a new account would open an empty profile and the player's campaign would look lost (docs/issues.md #36).

LEGACY_SAVE_DIR = "User_9000000000000"
TITLE_DIR = "1A96545B"                     # data/titles/<id> of Forza Horizon 2 (GAME_ID upper-case)
PROGRESS_MIN_BYTES = 1024                  # BinarySaveState of the newest revision: 4 bytes in a profile that only saw the prologue, ~110 KB with progress


def _newest_revision(profile):
    revs = [r for r in _save_listdir(os.path.join(profile, "revisions")) if re.fullmatch(r"[0-9a-fA-F]+", r)]
    return os.path.join(profile, "revisions", max(revs, key=lambda r: int(r, 16))) if revs else ""


def has_progress(profile):
    """The profile holds a real save (not just the empty one the game makes before the prologue)."""
    rev = _newest_revision(profile)
    try:
        return bool(rev) and os.path.getsize(os.path.join(rev, "data", "BinarySaveState")) >= PROGRESS_MIN_BYTES
    except FileNotFoundError:
        return False


def _saved_at(profile):
    rev = _newest_revision(profile)
    try:
        return os.path.getmtime(os.path.join(rev, "data", "BinarySaveState") if rev else profile)
    except OSError:
        return 0.0


def save_profiles(folder):
    """Every profile folder (User_<HEX>, not our .fresh-* / .adopting copies) in an Azimuth folder, in any of the layouts above."""
    bases = [os.path.join(folder, "data", "titles", TITLE_DIR, "saves"), os.path.join(folder, "data", "saves")]
    bases += [os.path.join(folder, "data", n) for n in _save_listdir(os.path.join(folder, "data")) if n.startswith("saves-backup-")]
    out = []
    for base in bases:
        for player in sorted(_save_listdir(base)):
            if not player.startswith("player-"):
                continue
            pdir = os.path.join(base, player)
            out += [os.path.join(pdir, n) for n in _save_listdir(pdir) if re.fullmatch(r"User_[0-9a-fA-F]+", n) and os.path.isdir(os.path.join(pdir, n))]
    return out


def _save_listdir(path):
    """Absent saves are normal; unreadable saves must block launch."""
    try:
        return os.listdir(path)
    except FileNotFoundError:
        return []


def account_save_path(folder, xuid):
    return os.path.join(folder, "data", "titles", TITLE_DIR, "saves", "player-1", f"User_{xuid:X}")


def _needs_adoption(profile):
    if has_progress(profile):
        return False
    # Unknown layouts may contain valuable data. Only replace a recognised empty
    # profile automatically; the manager can replace any profile explicitly.
    rev = _newest_revision(profile)
    if _save_listdir(profile) and (not rev or not os.path.isfile(os.path.join(rev, "data", "BinarySaveState"))):
        return False
    return True


def find_progress(folder, xuid, others=()):
    """The save to adopt for account `xuid`, or ''. Looks in this Azimuth folder first, then in the player's other ones (a new Azimuth release is
    usually unpacked next to the old one); in each, the account's own profile wins, then Azimuth's default one, then the most recently saved."""
    mine = f"User_{xuid:X}"
    for f in [folder, *others]:
        cands = [p for p in save_profiles(f) if has_progress(p)]
        if cands:
            return max(cands, key=lambda p: (os.path.basename(p) == mine, os.path.basename(p) == LEGACY_SAVE_DIR, _saved_at(p)))
    return ""


def adopt_saves(folder, xuid, others=(), copier=None):
    """Gives the account's profile the player's existing progress when it has none: copies the save find_progress picks into
    data/titles/<id>/saves/player-N/User_<xuid>. A profile with progress is never touched; an empty one (the game made it when the save was not
    found) is kept as User_<xuid>.fresh-<time>. The source stays where it is. -> description of what was done or ''."""
    target_name = f"User_{xuid:X}"
    base = os.path.join(folder, "data", "titles", TITLE_DIR, "saves")
    slots = [os.path.join(base, n) for n in sorted(_save_listdir(base)) if n.startswith("player-")] or [os.path.join(base, "player-1")]
    todo = [s for s in slots if _needs_adoption(os.path.join(s, target_name))]
    if not todo:
        return ""
    src = find_progress(folder, xuid, others)
    if not src:
        return ""
    done = []
    for pdir in todo:
        target = os.path.join(pdir, target_name)
        if os.path.realpath(src) == os.path.realpath(target):
            continue
        from .saves import copy_profile
        (copier or copy_profile)(src, target)
        where = "" if os.path.realpath(src).startswith(os.path.realpath(folder) + os.sep) else f" (from {os.path.dirname(os.path.dirname(src))})"
        done.append(f"{os.path.basename(src)}{where} -> {target_name}")
    return ", ".join(done)


# ---- Azimuth's own game settings (data/azimuth.ini [launcher]) that a player wants to change

SETTING_CHOICES = {          # key: (label, [(shown, value)]); values are what Azimuth writes itself
    "resolution": ("Resolution", [("720p", "720"), ("900p", "900"), ("1080p", "1080"), ("1440p", "1440"), ("4K", "2160")]),
    "frame_rate": ("Frame rate", [("30", "30"), ("60", "60"), ("90", "90"), ("120", "120"), ("144", "144")]),
    "frame_pacing": ("Frame pacing", [("Real-time", "real-time"), ("Fixed", "fixed")]),
    "vsync": ("V-Sync", [("On", "on"), ("Off", "off")]),
    "display": ("Display", [("Windowed", "windowed"), ("Fullscreen", "fullscreen")]),
    "antialiasing": ("Anti-aliasing", [("MSAA 2x", "msaax2"), ("MSAA 4x", "msaax4")]),
}


def get_settings(folder):
    return dict(read_ini(folder).get("launcher", {}))


def set_settings(folder, values):
    """Writes [launcher] keys into azimuth.ini, keeping everything else (and the file's encoding) as it is."""
    path = os.path.join(folder, "data", "azimuth.ini")
    with open(path, "rb") as f:
        raw = f.read()
    enc = "utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8"
    text = raw.decode(enc)
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.split(nl)
    sec = next((i for i, l in enumerate(lines) if l.strip() == "[launcher]"), None)
    if sec is None:
        lines = ["[launcher]"] + lines
        sec = 0
    end = next((i for i in range(sec + 1, len(lines)) if lines[i].strip().startswith("[")), len(lines))
    for key, value in values.items():
        for i in range(sec + 1, end):
            if lines[i].split("=", 1)[0].strip() == key:
                lines[i] = f"{key}={value}"
                break
        else:
            lines.insert(end, f"{key}={value}")
            end += 1
    paths.atomic_write(path, (nl.join(lines).rstrip(nl) + nl).encode(enc), 0o644)
