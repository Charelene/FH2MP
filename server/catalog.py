"""Game catalog: the user's own game database and string tables mirrored into one derived SQLite file.

`server/data/gamedata.sqlite` is built from the install (never committed: server/data is git-ignored, and it stays on this machine
with the LSP) so the stand-in can answer questions in SQL instead of reopening the game's files and guessing:
- every table of media/stripped/gamedbRC.slt, copied as is (288 tables) with an index on each table's first column,
- `strings(id, src, text)`: the English text of every hashed `_&<n>` id, resolved by server/strtab.py (47 of the 49 tables that hold
  ids resolve directly; the rest are retried with column subsets and reported in `catalog_meta`),
- `routes(track_id, route, length_m, start_finish_m, closed)`: race route geometry from media/tracks/anthem/trackroutes/*.nt,
- views `v_cars`, `v_tracks`, `v_makes`, `v_bodies`, `v_countries`, `v_drive_types`, `v_classes` with the names joined in.
It is rebuilt when the install's fingerprint (size + mtime of the database and of the language zip) changes. The LSP attaches it to the
state database as `game` (Database.attach) so scores, events and cars can be joined.
"""
import itertools
import math
import os
import re
import sqlite3
import time
import zipfile

import strtab

LOC = re.compile(r"<Name value='([^']*)'/>.*?value\._41=\"([-0-9.e+]+)\" value\._42=\"([-0-9.e+]+)\" value\._43=\"([-0-9.e+]+)\"", re.S)

VIEWS = """
CREATE VIEW v_cars AS
SELECT c.Id AS id, c.MediaName AS media, n.text AS name, ms.text AS model_short, c.MakeID AS make_id, mk.name AS make,
       c.ModelId AS model_id, c.Year AS year, c.ClassID AS class_id, cl.letter AS class_letter, c.PerformanceIndex AS pi,
       c.FamilyBodyID AS body_id, b.name AS body, c.FamilyModelID AS family_model_id, c.DriveTypeID AS drive_id, d.name AS drive,
       c.CountryID AS country_id, co.name AS country, c.CarTypeID AS car_type_id, c.\"TopSpeed-mph\" * 0.44704 AS top_speed_ms,
       c.SimPeakPower AS peak_power, c.CurbWeight AS curb_weight
FROM Data_Car c
LEFT JOIN strings n  ON n.id  = CAST(substr(c.DisplayName, 3) AS INTEGER)
LEFT JOIN strings ms ON ms.id = CAST(substr(c.ModelShort, 3) AS INTEGER)
LEFT JOIN v_makes mk ON mk.id = c.MakeID
LEFT JOIN v_bodies b ON b.id = c.FamilyBodyID
LEFT JOIN v_drive_types d ON d.id = c.DriveTypeID
LEFT JOIN v_countries co ON co.id = c.CountryID
LEFT JOIN v_classes cl ON cl.id = c.ClassID;
"""
PRE_VIEWS = """
CREATE VIEW v_makes AS SELECT m.ID AS id, s.text AS name, m.ManufacturerCode AS code, m.CountryID AS country_id
  FROM List_CarMake m LEFT JOIN strings s ON s.id = CAST(substr(m.DisplayName, 3) AS INTEGER);
CREATE VIEW v_bodies AS SELECT b.Id AS id, s.text AS name
  FROM List_FamilyBody b LEFT JOIN strings s ON s.id = CAST(substr(b.DisplayName, 3) AS INTEGER);
CREATE VIEW v_countries AS SELECT c.CountryID AS id, s.text AS name, c.RegionID AS region_id
  FROM List_Country c LEFT JOIN strings s ON s.id = CAST(substr(c.Country, 3) AS INTEGER);
CREATE VIEW v_drive_types AS SELECT d.ID AS id, d.DriveType AS code, s.text AS name
  FROM List_DriveType d LEFT JOIN strings s ON s.id = CAST(substr(d.DisplayName, 3) AS INTEGER);
CREATE VIEW v_classes AS SELECT c.Id AS id, CASE c.Id WHEN 0 THEN 'D' WHEN 1 THEN 'C' WHEN 2 THEN 'B' WHEN 3 THEN 'A'
  WHEN 4 THEN 'S1' WHEN 5 THEN 'S2' ELSE s.text END AS letter, s.text AS name, c.MaxPerformanceIndex AS max_pi,
  c.MaxDisplayPerformanceIndex AS max_display_pi
  FROM CarClasses c LEFT JOIN strings s ON s.id = CAST(substr(c.DisplayName, 3) AS INTEGER);
CREATE VIEW v_tracks AS SELECT t.id AS id, t.Route AS route, t.Active AS active, f.text AS name, sh.text AS short_name, d.text AS description,
  r.length_m AS length_m, r.start_finish_m AS start_finish_m, COALESCE(r.closed, 0) AS closed
  FROM Tracks t
  LEFT JOIN strings f  ON f.id  = CAST(substr(t.DisplayName, 3) AS INTEGER)
  LEFT JOIN strings sh ON sh.id = CAST(substr(t.ShortDisplayName, 3) AS INTEGER)
  LEFT JOIN strings d  ON d.id  = CAST(substr(t.Description, 3) AS INTEGER)
  LEFT JOIN routes r   ON r.track_id = t.id;
"""


def sources(mount, lang="EN"):
    return (os.path.join(mount, "media", "stripped", "gamedbRC.slt"), strtab.zip_for(mount, lang))


def fingerprint(mount, lang="EN"):
    parts = []
    for p in sources(mount, lang):
        st = os.stat(p)
        parts.append(f"{os.path.basename(p)}:{st.st_size}:{int(st.st_mtime)}")
    return "|".join(parts) + "|v1"


def parse_route(path):
    """(length_m, start_finish_m) from a route .nt file, or None. See CarDb.route_info for the reasoning."""
    with open(path, "rb") as f:
        xml = f.read().decode("latin1")
    locs = {n: (float(x), float(y), float(z)) for n, x, y, z in LOC.findall(xml)}
    wp = [locs[n] for n in sorted(locs) if re.fullmatch(r"route_waypoint_\d+", n)]
    fin, st = locs.get("finish_line"), locs.get("start_location_000")
    pts = wp + ([fin] if fin else [])
    if len(pts) < 2 or not fin or not st:
        return None
    length = sum(math.dist(pts[i], pts[i + 1]) for i in range(len(pts) - 1))
    return (length, math.dist(st, fin)) if length > 100 else None


def _id_columns(src):
    """{table: [columns holding `_&<n>` string ids]}"""
    out = {}
    for (t,) in src.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'"):
        for _cid, col, *_ in src.execute(f'PRAGMA table_info("{t}")'):
            try:
                hit = src.execute(f'SELECT 1 FROM "{t}" WHERE "{col}" LIKE \'\\_&%\' ESCAPE \'\\\' LIMIT 1').fetchone()
            except sqlite3.Error:
                hit = None
            if hit:
                out.setdefault(t, []).append(col)
    return out


def build(mount, out_path, lang="EN"):
    """Builds the catalog atomically. Returns {"tables", "strings", "unresolved", "routes"}."""
    slt, zpath = sources(mount, lang)
    tmp = out_path + ".tmp"
    for suffix in ("", "-wal", "-shm", "-journal"):
        try:
            os.remove(tmp + suffix)
        except OSError:
            pass
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    src = sqlite3.connect(f"file:{slt}?mode=ro&immutable=1", uri=True)
    tables = [r[0] for r in src.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name")]
    # 1. mirror every table (CREATE TABLE AS keeps the data and column affinity; the game's keys and indexes are not needed read-only)
    out = sqlite3.connect(f"file:{tmp}?mode=rwc", uri=True)
    out.execute("PRAGMA journal_mode=OFF")
    out.execute("ATTACH DATABASE ? AS g", (f"file:{slt}?mode=ro&immutable=1",))
    for t in tables:
        out.execute(f'CREATE TABLE main."{t}" AS SELECT * FROM g."{t}"')
        first = next(iter(src.execute(f'PRAGMA table_info("{t}")')), None)
        if first:
            out.execute(f'CREATE INDEX "ix_{t}_0" ON "{t}" ("{first[1]}")')
    out.commit()
    out.execute("DETACH DATABASE g")
    # 2. strings
    out.execute("CREATE TABLE strings (id INTEGER PRIMARY KEY, src TEXT NOT NULL, text TEXT NOT NULL)")
    unresolved, nstr = [], 0
    idcols = _id_columns(src)
    with zipfile.ZipFile(zpath) as z:
        members = {n for n in z.namelist() if n.endswith(".str")}
    for t, cols in sorted(idcols.items()):
        member = t + ".str"
        if member not in members:
            unresolved.append(f"{t}: no {member}")
            continue
        res = None
        for k in range(len(cols), 0, -1):
            for sub in itertools.combinations(cols, k):
                res = strtab.resolve(zpath, member, src, [(t, c) for c in sub])
                if res:
                    break
            if res:
                break
        if not res:
            unresolved.append(f"{t}: id count differs from the {member} pool")
            continue
        out.executemany("INSERT OR IGNORE INTO strings (id, src, text) VALUES (?,?,?)", [(i, t, s) for i, s in res.items()])
        nstr += len(res)
    # 3. routes
    out.execute("CREATE TABLE routes (track_id INTEGER PRIMARY KEY, route INTEGER, length_m REAL, start_finish_m REAL, closed INTEGER)")
    nroutes = 0
    base = os.path.join(mount, "media", "tracks", "anthem", "trackroutes")
    for tid, route in src.execute("SELECT id, Route FROM Tracks WHERE Route IS NOT NULL"):
        p = os.path.join(base, f"route{int(route)}.nt")
        try:
            info = parse_route(p)
        except (OSError, ValueError):
            info = None
        if info:
            out.execute("INSERT INTO routes VALUES (?,?,?,?,?)", (tid, route, info[0], info[1], int(info[1] < 150)))
            nroutes += 1
    # 4. views + meta
    out.executescript(PRE_VIEWS + VIEWS)
    out.execute("CREATE TABLE catalog_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)")
    out.executemany("INSERT INTO catalog_meta VALUES (?,?)", [
        ("fingerprint", fingerprint(mount, lang)), ("built", str(int(time.time()))), ("lang", lang),
        ("unresolved", "; ".join(unresolved))])
    out.commit()
    out.close()
    src.close()
    os.replace(tmp, out_path)
    return {"tables": len(tables), "strings": nstr, "unresolved": unresolved, "routes": nroutes}


def ensure(mount, out_path, lang="EN", log=print):
    """Path of an up-to-date catalog, (re)building it when missing or when the install changed."""
    fp = fingerprint(mount, lang)
    if os.path.exists(out_path):
        try:
            c = sqlite3.connect(f"file:{out_path}?mode=ro", uri=True)
            row = c.execute("SELECT value FROM catalog_meta WHERE key='fingerprint'").fetchone()
            c.close()
            if row and row[0] == fp:
                return out_path
        except sqlite3.Error:
            pass
    t0 = time.time()
    r = build(mount, out_path, lang)
    log(f"catalog built in {time.time() - t0:.1f}s: {r['tables']} tables, {r['strings']} strings, {r['routes']} routes, "
        f"unresolved: {r['unresolved'] or 'none'}")
    return out_path
