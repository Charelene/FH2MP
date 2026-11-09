"""The car database the server plays with (native/internal/gamedbsync): checked at every start, downloaded when the player's copy differs.

The launcher keeps it in <Azimuth>/fh2mp/gamedbRC.slt. The shim copies it to <Mount>/mediaoverride/Stripped/gamedbRC.slt once every
FH2 Mod Platform mod has started (the platform writes the database it builds from other mods' SQL to the same file, or deletes it), so
the game reads exactly the server's copy. FH2 Extra Options loads mediaoverride before mediapc and the original media
(FH2_ExtraOptions.ini, [Files] EnableMediaOverrides), so a car pack that replaced the database in mediapc, or an edited original, does
not change what the game reads online. Without Extra Options (or with its overrides off) the copy would be ignored, so play is refused.
"""
import hashlib
import json
import lzma
import os
import re
import sqlite3

from . import azimuth, paths, shim
from .constants import DATA_DIR

LOCAL = (DATA_DIR, "gamedbRC.slt")
PLACED = (DATA_DIR, "gamedb-placed.txt")  # the shim's list of its copies in mediaoverride ("size date", newest last)
BASE_PARTS = (DATA_DIR, "base-parts.txt")  # the parts and stock cars of the server's database, for a game that keeps its own car packs
# The part tables (after List_Upgrade) in CarPartSlotTable order, as the shim's picker check reads them (shim/src/picker_pi.inc g_ppi_table, g_ppi_motor).
PART_TABLES = ("Engine", "Drivetrain", "CarBody", "Motor", "Brakes", "SpringDamper", "AntiSwayFront", "AntiSwayRear", "TireCompound", "RearWing",
               "RimSizeFront", "RimSizeRear", "EngineCamshaft", "EngineValves", "EngineDisplacement", "EnginePistonsCompression", "EngineFuelSystem",
               "EngineIgnition", "EngineExhaust", "EngineIntake", "EngineFlywheel", "EngineManifold", "EngineRestrictorPlate", "EngineOilCooling",
               "EngineTurboSingle", "EngineTurboTwin", "EngineCSC", "EngineDSC", "EngineIntercooler", "DrivetrainClutch", "DrivetrainTransmission",
               "DrivetrainDriveline", "DrivetrainDifferential", "CarBodyFrontBumper", "CarBodyRearBumper", "CarBodyHood", "CarBodySideSkirt",
               "CarBodyTireWidthFront", "CarBodyTireWidthRear", "CarBodyWeight", "CarBodyChassisStiffness", "MotorParts")
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
        raise GameDbError(f"FH2 Extra Options {shim.EXTRA_OPTIONS_VERSION} is not installed in your Azimuth folder (its FH2_ExtraOptions.ini is missing). "
                          "Turn on \"Install FH2 Extra Options\" in Settings and press Play, or copy the files from the Azimuth folder of "
                          "its release into yours.") from None
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


def write_base_parts(folder):
    """A game that keeps its own car packs reads its own database, in which a car pack's parts look like the game's. The shim needs the
    server's parts to tell them apart (picker_pi.inc: the event car picker shows such a car with its stock class and PI; the event car gets
    stock parts in their slots). Reads the server's database, already in fh2mp/ (sync), and writes fh2mp/base-parts.txt:
    "p <table> <id>" for every part row, "c <car id> <class id> <performance index in millionths>" for every car. -> the number of lines.
    Raises GameDbError when the database cannot be read."""
    src = os.path.join(folder, *LOCAL)
    lines = ["fh2mp-base-parts 1"]
    try:
        con = sqlite3.connect(f"file:{src}?mode=ro", uri=True)
        try:
            have = {r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")}
            for t in PART_TABLES:
                if "List_Upgrade" + t in have:
                    lines += [f"p {t} {i}" for (i,) in con.execute(f"SELECT Id FROM List_Upgrade{t} ORDER BY Id") if isinstance(i, int) and i >= 0]
            for car, cls, pi in con.execute("SELECT Id, ClassID, PerformanceIndex FROM Data_Car ORDER BY Id"):
                if isinstance(car, int) and car >= 0 and isinstance(cls, int) and 0 <= cls <= 255 and isinstance(pi, (int, float)) and 0 <= pi < 1000:
                    lines.append(f"c {car} {cls} {round(float(pi) * 1e6)}")      # millionths: the shim reads no floats
        finally:
            con.close()
    except sqlite3.Error as e:
        raise GameDbError(f"The server's car database could not be read ({e}).") from None
    paths.atomic_write(os.path.join(folder, *BASE_PARTS), "".join(ln + "\n" for ln in lines).encode("ascii"), 0o644)
    return len(lines)


STANDIN_CANDIDATES = 5


def _pack_cars(folder):
    """The cars the Extra Options mods in <folder>/mods add (mod.json naming a database.json with Data_Car rows) -> {car id: (bucket, class,
    performance index)}; a mod whose files cannot be read is skipped."""
    out = {}
    mods = os.path.join(folder, "mods")
    try:
        names = sorted(os.listdir(mods))
    except OSError:
        return out
    for name in names:
        try:
            with open(os.path.join(mods, name, "mod.json"), encoding="utf-8-sig") as f:
                manifest = json.load(f)
            rel = manifest.get("database") if isinstance(manifest, dict) else None
            if not isinstance(rel, str) or os.path.isabs(rel) or ".." in rel.replace("\\", "/").split("/"):
                continue
            with open(os.path.join(mods, name, rel), encoding="utf-8-sig") as f:
                db = json.load(f)
            tables = db.get("tables", {})
            buckets = {r.get("CarId"): r.get("CarBucket") for r in tables.get("Data_Car_Buckets", {}).get("insert", []) if isinstance(r, dict)}
            for r in tables.get("Data_Car", {}).get("insert", []):
                car = r.get("Id") if isinstance(r, dict) else None
                if isinstance(car, int) and car > 0:
                    out[car] = (buckets.get(car), r.get("ClassID"), r.get("PerformanceIndex"))
        except (OSError, ValueError, AttributeError):
            continue
    return out


def custom_standins(folder):
    """For each car a car pack adds that the server's database (fh2mp/gamedbRC.slt) lacks: up to STANDIN_CANDIDATES stock cars, closest first,
    for the game server to show the others instead of its one fixed stand-in car (it takes the first it accepts). Closest = the same car category
    (Data_Car_Buckets, e.g. Extreme Track Toys) with the nearest performance index, then the same class with the nearest performance index.
    -> "<custom>:<a>/<b>...,..." for PUT /v1/me/standins, "" when there is nothing to send. Raises GameDbError when the database cannot be read."""
    packs = _pack_cars(folder)
    if not packs:
        return ""
    try:
        con = sqlite3.connect(f"file:{os.path.join(folder, *LOCAL)}?mode=ro", uri=True)
        try:
            stock = {r[0]: (r[1], r[2]) for r in con.execute("SELECT Id, ClassID, PerformanceIndex FROM Data_Car")
                     if isinstance(r[0], int) and isinstance(r[2], (int, float))}
            bucket = {r[0]: r[1] for r in con.execute("SELECT CarId, CarBucket FROM Data_Car_Buckets")}
        finally:
            con.close()
    except sqlite3.Error as e:
        raise GameDbError(f"The server's car database could not be read ({e}).") from None
    items = []
    for car in sorted(packs):
        if car in stock or len(items) >= 64:
            continue
        b, cls, pi = packs[car]
        pi = float(pi) if isinstance(pi, (int, float)) else 0.0
        ranked = sorted(((0 if b is not None and bucket.get(s) == b else 1 if cls is not None and stock[s][0] == cls else 2,
                          abs(float(stock[s][1]) - pi), s) for s in stock))
        picks = [s for rank, _, s in ranked if rank < 2][:STANDIN_CANDIDATES]
        # a car without a candidate is still listed (candidate 0, never a car): the server then knows this game has it, so players with the
        # same car pack see it as it is (game.RealCarsFor)
        items.append(f"{car}:" + ("/".join(str(s) for s in picks) or "0"))
    return ",".join(items)

