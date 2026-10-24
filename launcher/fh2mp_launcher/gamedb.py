"""The car database the server plays with (server/gamedb_sync.py): checked at every start, downloaded when the player's copy differs.

The launcher keeps it in <Azimuth>/fh2mp/gamedbRC.slt. The shim copies it to <Mount>/mediaoverride/Stripped/gamedbRC.slt once every
FH2 Mod Platform mod has started (the platform writes the database it builds from other mods' SQL to the same file, or deletes it), so
the game reads exactly the server's copy. FH2 Extra Options loads mediaoverride before mediapc and the original media
(FH2_ExtraOptions.ini, [Files] EnableMediaOverrides), so a car pack that replaced the database in mediapc, or an edited original, does
not change what the game reads online. Without Extra Options (or with its overrides off) the copy would be ignored, so play is refused.
"""
import hashlib
import lzma
import os
import re

from . import azimuth, paths, shim
from .constants import DATA_DIR

LOCAL = (DATA_DIR, "gamedbRC.slt")
PLACED = (DATA_DIR, "gamedb-placed.txt")  # the shim's list of its copies in mediaoverride ("size date", newest last)
MAX_SIZE = 64 * 1024 * 1024


class GameDbError(Exception):
    pass


def session_path():
    """The synced database as the shim's session names it: relative to the Azimuth folder."""
    return "\\".join(LOCAL)


def ensure_media_overrides(folder):
    """Turns EnableMediaOverrides on in the Extra Options settings. -> True when it changed. Raises GameDbError without Extra Options."""
    path = os.path.join(folder, shim.EXTRA_INI)
    try:
        with open(path, "rb") as f:
            text = f.read().decode("utf-8", "replace")
    except OSError:
        raise GameDbError("FH2 Extra Options is missing: reinstall the shim from the launcher.") from None
    nl = "\r\n" if "\r\n" in text else "\n"
    m = re.search(r"(?m)^[ \t]*EnableMediaOverrides[ \t]*=[ \t]*([^\r\n]*?)[ \t]*(?=\r?$)", text)
    if m:
        if m.group(1) == "1":
            return False
        text = text[:m.start(1)] + "1" + text[m.end(1):]
    elif re.search(r"(?m)^\[Files\][ \t]*\r?$", text):
        text = re.sub(r"(?m)^(\[Files\][ \t]*)(\r?)$", lambda s: s.group(1) + nl + "EnableMediaOverrides=1" + s.group(2), text, count=1)
    else:
        text = text.rstrip("\r\n") + nl + nl + "[Files]" + nl + "EnableMediaOverrides=1" + nl
    paths.atomic_write(path, text.encode("utf-8"), 0o644)
    return True


def sync(api, token, m, folder, progress=None):
    """Makes <Azimuth folder>/fh2mp/gamedbRC.slt the database `m` (the server's /v1/gamedb answer) describes; the shim puts it in the
    game's mediaoverride folder. -> a short note, '' when it already is. Raises GameDbError when the copy cannot be put in place (play
    must not go on with another database)."""
    want, size, path = str(m.get("sha256", "")).lower(), int(m.get("size", 0)), str(m.get("path", ""))
    if not re.fullmatch(r"[0-9a-f]{64}", want) or not 0 < size <= MAX_SIZE or not path.startswith("/v1/gamedb/"):
        raise GameDbError("The server's car database description is not valid.")
    if not folder or not os.path.isdir(folder):
        raise GameDbError("Choose your Azimuth folder first.")
    target = os.path.join(folder, *LOCAL)
    if os.path.isfile(target) and azimuth.sha256_file(target) == want:
        return ""
    if progress:
        progress("Downloading the car database...")
    blob = api.gamedb_download(token, path, MAX_SIZE)
    try:
        d = lzma.LZMADecompressor(format=lzma.FORMAT_XZ, memlimit=256 * 1024 * 1024)
        data = d.decompress(blob, max_length=size + 1)
    except lzma.LZMAError:
        raise GameDbError("The car database download was damaged. Press Play to try again.") from None
    if len(data) != size or hashlib.sha256(data).hexdigest() != want:
        raise GameDbError("The car database download was damaged. Press Play to try again.")
    os.makedirs(os.path.dirname(target), exist_ok=True)
    remember_copy(folder)
    paths.atomic_write(target, data, 0o644)
    return "car database updated"


def remember_copy(folder):
    """Before the copy is replaced: its size and date (100 ns ticks since 1601, as Windows keeps it) go last in fh2mp/gamedb-placed.txt,
    which keeps the last 4. The shim copies it to mediaoverride with that date, so the copy there is still known as FH2MP's once the
    server's database has changed, and is not kept as another mod's car database (shim/src/gamedb_keep.inc). Never blocks playing."""
    try:
        st = os.stat(os.path.join(folder, *LOCAL))
    except OSError:
        return
    entry = f"{st.st_size} {st.st_mtime_ns // 100 + 116444736000000000}"
    path = os.path.join(folder, *PLACED)
    try:
        with open(path, "rb") as f:
            lines = [ln for ln in f.read().decode("ascii", "replace").splitlines() if re.fullmatch(r"\d+ \d+", ln)]
    except OSError:
        lines = []
    lines = [ln for ln in lines if ln != entry] + [entry]
    try:
        paths.atomic_write(path, "".join(ln + "\n" for ln in lines[-4:]).encode("ascii"), 0o644)
    except OSError:
        pass
