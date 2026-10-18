"""Resolves the game's hashed string ids (`_&<number>` in gamedbRC.slt) to English text, read in place from the user's install.

Format facts (media/stripped/stringtables/<LANG>.zip -> <Table>.str, "LSB2"), recovered from the data, not from a spec:
- section 1 starts at 0x24: u32 size, u32 string count, ... then the pool of NUL-terminated UTF-16LE strings at the end of the
  section (followed by one empty string);
- the pool is stored in ascending id order, and the ids the database uses for one table are exactly the pool's ids.
So when the database holds exactly as many distinct ids (over the columns that reference the table) as the pool has strings, sorting
the ids and zipping them with the pool gives id -> text without decoding the hash tables [confirmed on Tracks: track 856 ->
"Sisteron Citadel Circuit" / "Citadel Circuit" / "CITADEL CIRCUIT" / its description, 1148 = 287 tracks x 4 columns].
When the counts do not match, `resolve` returns None rather than guess. Nothing is copied out of the install.
"""
import os
import re
import struct
import zipfile


def read_pool(zip_path, member):
    """The list of strings (pool order) of one .str table in a language zip."""
    with zipfile.ZipFile(zip_path) as z:
        d = z.read(member)
    size, count = struct.unpack_from("<II", d, 0x24)
    region = d[0x24:0x24 + size]
    parts, start = [], 0
    for i in range(0, len(region) - 1, 2):
        if region[i] == 0 and region[i + 1] == 0:
            parts.append(region[start:i].decode("utf-16-le", "replace"))
            start = i + 2
    return parts[-(count + 1):-1] if len(parts) > count else []


def ids_in(db, columns):
    """Distinct hashed ids (ints) held by (table, column) pairs of the game database."""
    out = set()
    for table, col in columns:
        for (v,) in db.execute(f'SELECT "{col}" FROM "{table}"'):
            if isinstance(v, str) and re.fullmatch(r"_&\d+", v):
                out.add(int(v[2:]))
    return out


def resolve(zip_path, member, db, columns):
    """{id: text} for a table, or None if the id count does not match the pool size."""
    pool = read_pool(zip_path, member)
    ids = sorted(ids_in(db, columns))
    if not pool or len(ids) != len(pool):
        return None
    return dict(zip(ids, pool))


def zip_for(mount, lang="EN"):
    return os.path.join(mount, "media", "stripped", "stringtables", f"{lang}.zip")
