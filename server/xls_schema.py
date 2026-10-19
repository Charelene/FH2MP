#!/usr/bin/env python3
"""Dump Turn 10 "Xls" web-service struct schemas from the user's own forza_durango_release_final.exe.

No game data is stored in the repo: the schema is read from the exe passed on the command line.

How the schema is laid out (FH2 1.0.0.64, recovered from Xls::BinaryReader, FUN_140ee5880 and friends):
  * A struct descriptor object lives in .data and is filled at startup by a 32-byte thunk
        lea rdx,[entries] ; lea rcx,[descriptor] ; mov r8d,count ; jmp register
    We scan .text for those thunks to map descriptor VA -> (entries VA, count).
  * `entries` is an array of 0x60-byte field entries in .rdata:
        +0x00 u32 type   +0x08 char* name   +0x10 i64 offset in the C struct (0x7fffffff = n/a)
        +0x30 i64 offset of the array-count field   +0x38 i64 offset of the length field
        +0x40 u32 fixed size / element size   +0x48 nested struct descriptor VA   +0x50 element entry VA
        +0x58 u32 flags
  * Wire format (big endian): 0 u8, 1/2 u16, 3/4 u32, 5/6/0x13 u64, 7 u32, 9 string (u32 byte length +
    UTF-8), 0xc nested struct, 0xe array (u32 count + elements), 0x10 DateTime (as a string,
    "YYYY-MM-DDTHH:MM:SS[.fff]Z"), 0x11 bool (u8), 0x12 GUID (u32,u16,u16,8 raw), 0x14 blob (u32 len + bytes).

Usage: python3 -I server/xls_schema.py <exe> <StructOrCallName> [...]   e.g. ForzaUserLogon_Results

The server does not need the exe at run time: `export_tables` copies the descriptors into the tables xls_struct / xls_field /
xls_name of the game catalog bundle (tools/export_gamedata.py; tools/load_gamedata_pg.py loads it into PostgreSQL), and `TableSource`
serves the same interface from them.
"""
import re
import struct
import sys

TYPE_NAMES = {
    0: "u8", 1: "u16", 2: "i16", 3: "i32", 4: "u32", 5: "i64", 6: "u64", 7: "f32?", 9: "string",
    0xC: "struct", 0xE: "array", 0x10: "datetime", 0x11: "bool", 0x12: "guid", 0x13: "u64b", 0x14: "blob",
}


class Exe:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt = struct.unpack_from("<H", d, pe + 20)[0]
        self.base = struct.unpack_from("<Q", d, pe + 24 + 24)[0]
        self.secs = []
        o = pe + 24 + opt
        for i in range(nsec):
            n, vs, va, rs, ro = struct.unpack_from("<8sIIII", d, o + i * 40)
            self.secs.append((n.rstrip(b"\0").decode(), va, vs, ro, rs))

    def off(self, va):
        rva = va - self.base
        for _, sva, vs, ro, rs in self.secs:
            if sva <= rva < sva + max(vs, rs):
                return ro + rva - sva
        return None

    def read(self, va, n):
        o = self.off(va)
        return self.d[o:o + n] if o is not None else b""

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack("<I", b)[0] if len(b) == 4 else 0

    def u64(self, va):
        b = self.read(va, 8)
        return struct.unpack("<Q", b)[0] if len(b) == 8 else 0

    def cstr(self, va):
        b = self.read(va, 160)
        return b.split(b"\0")[0].decode("latin1")

    def section(self, name):
        for n, va, vs, ro, rs in self.secs:
            if n == name:
                return va, vs, ro, rs


def scan_thunks(exe):
    """descriptor VA -> (entries VA, count)"""
    va, vs, ro, rs = exe.section(".text")
    text = exe.d[ro:ro + rs]
    out = {}
    pat = b"\x48\x8d\x15"
    i = 0
    while True:
        i = text.find(pat, i)
        if i < 0:
            break
        if text[i + 7:i + 10] == b"\x48\x8d\x0d" and text[i + 14:i + 16] == b"\x41\xb8" and text[i + 20] == 0xE9:
            ent = exe.base + va + i + 7 + struct.unpack_from("<i", text, i + 3)[0]
            des = exe.base + va + i + 14 + struct.unpack_from("<i", text, i + 10)[0]
            cnt = struct.unpack_from("<I", text, i + 16)[0]
            out[des] = (ent, cnt)
        i += 1
    return out


class Field:
    def __init__(self, exe, va):
        self.va = va
        self.type = exe.u32(va)
        self.name = exe.cstr(exe.u64(va + 8)) if exe.u64(va + 8) else ""
        self.offset = exe.u64(va + 0x10)
        self.size = exe.u32(va + 0x40)
        self.nested = exe.u64(va + 0x48)
        self.elem = exe.u64(va + 0x50)
        self.flags = exe.u32(va + 0x58)
        self.count_off = exe.u64(va + 0x38)  # offset of the element-count field (arrays), 0x7fffffff if none


class ExeSource:
    """Schema straight from the exe: field(va), fields(desc), find(name), count()."""

    def __init__(self, exe):
        self.exe = exe
        self.thunks = scan_thunks(exe)

    def field(self, va):
        return Field(self.exe, va)

    def fields(self, desc):
        return fields_of(self.exe, self.thunks, desc)

    def find(self, name):
        return find_entry_by_name(self.exe, name)

    def count(self):
        return len(self.thunks)


class _Row(Field):
    """A Field loaded from the database instead of the exe."""

    def __init__(self, r):
        self.va, self.type, self.name, self.offset, self.size = r["va"], r["type"], r["name"], r["offset"], r["size"]
        self.nested, self.elem, self.flags, self.count_off = r["nested"], r["elem"], r["flags"], r["count_off"]


class TableSource:
    """Same interface as ExeSource, read from the tables written by export_tables. `db` is anything with query(sql) -> rows
    (db.Database: the `game` schema) or an sqlite3 connection (the exported bundle)."""

    def __init__(self, db):
        rows = db.query if hasattr(db, "query") else (lambda sql: db.execute(sql).fetchall())
        self._fields = {}
        for r in rows('SELECT va, type, name, "offset", size, nested, elem, flags, count_off FROM xls_field'):
            self._fields[r[0]] = _Row(dict(zip(("va", "type", "name", "offset", "size", "nested", "elem", "flags", "count_off"), r)))
        self._structs = {d: (e, c) for d, e, c in rows("SELECT desc_va, entries, count FROM xls_struct")}
        self._names = {}
        for n, va in rows("SELECT name, va FROM xls_name ORDER BY seq"):
            self._names.setdefault(n, []).append(va)

    def field(self, va):
        return self._fields[va]

    def fields(self, desc):
        if desc not in self._structs:
            return None
        ent, cnt = self._structs[desc]
        return [self._fields[ent + 0x60 * i] for i in range(cnt)]

    def find(self, name):
        return list(self._names.get(name, []))

    def count(self):
        return len(self._structs)


def export_tables(exe, conn):
    """Writes every struct descriptor, field entry and call name of the exe into `conn` (tables xls_struct, xls_field, xls_name).
    Returns (structs, fields, names)."""
    src = ExeSource(exe)
    conn.executescript("""
        DROP TABLE IF EXISTS xls_struct; DROP TABLE IF EXISTS xls_field; DROP TABLE IF EXISTS xls_name;
        CREATE TABLE xls_struct (desc_va INTEGER PRIMARY KEY, entries INTEGER NOT NULL, count INTEGER NOT NULL);
        CREATE TABLE xls_field (va INTEGER PRIMARY KEY, type INTEGER NOT NULL, name TEXT NOT NULL, "offset" INTEGER NOT NULL,
                                size INTEGER NOT NULL, nested INTEGER NOT NULL, elem INTEGER NOT NULL, flags INTEGER NOT NULL,
                                count_off INTEGER NOT NULL);
        CREATE TABLE xls_name (seq INTEGER PRIMARY KEY, name TEXT NOT NULL, va INTEGER NOT NULL);
        CREATE INDEX ix_xls_name ON xls_name(name);
    """)
    seen = {}

    def add(va):
        if va in seen or not va:
            return
        f = Field(exe, va)
        seen[va] = f
        if f.type == 0xC and f.nested in src.thunks:
            for g in src.fields(f.nested):
                add(g.va)
        if f.type == 0xE and f.elem:
            add(f.elem)

    for desc, (ent, cnt) in src.thunks.items():
        for i in range(cnt):
            add(ent + 0x60 * i)
    calls = sorted({m.decode() for m in re.findall(rb"(?<=\0)([A-Za-z0-9_]*(?:Parameters|Results))(?=\0)", exe.d)})
    names = []
    for n in calls:
        for va in src.find(n):
            add(va)
            names.append((n, va))
    conn.executemany("INSERT INTO xls_struct VALUES (?,?,?)", [(d, e, c) for d, (e, c) in src.thunks.items()])
    conn.executemany("INSERT INTO xls_field VALUES (?,?,?,?,?,?,?,?,?)",
                     [(f.va, f.type, f.name, f.offset, f.size, f.nested, f.elem, f.flags, f.count_off) for f in seen.values()])
    conn.executemany("INSERT INTO xls_name (name, va) VALUES (?,?)", names)
    return len(src.thunks), len(seen), len(names)


def fields_of(exe, thunks, desc):
    if desc not in thunks:
        return None
    ent, cnt = thunks[desc]
    return [Field(exe, ent + 0x60 * i) for i in range(cnt)]


def dump(exe, thunks, fields, indent=0, seen=None):
    pad = "  " * indent
    for f in fields:
        tn = TYPE_NAMES.get(f.type, f"type{f.type:#x}")
        line = f"{pad}{f.name or '(anon)'}: {tn}"
        if f.type == 0xC:
            sub = fields_of(exe, thunks, f.nested)
            print(line + (" {" if sub is not None else f" {{unresolved desc {f.nested:#x}}}"))
            if sub:
                dump(exe, thunks, sub, indent + 1)
                print(f"{pad}}}")
        elif f.type == 0xE:
            el = Field(exe, f.elem) if f.elem else None
            if el is not None and el.type == 0xC:
                sub = fields_of(exe, thunks, el.nested)
                print(line + f" of struct (elem size {el.size:#x}) {{")
                if sub:
                    dump(exe, thunks, sub, indent + 1)
                print(f"{pad}}}")
            else:
                print(line + f" of {TYPE_NAMES.get(el.type, hex(el.type)) if el else '?'}")
        else:
            print(line + (f" (fixed {f.size})" if f.type in (9, 0x14) and f.size else ""))


def find_entry_by_name(exe, name):
    """VA of the first descriptor entry whose name pointer points at `name`."""
    needle = b"\0" + name.encode() + b"\0"
    i = exe.d.find(needle)
    if i < 0:
        return []
    # string VA
    sva = None
    for _, va, vs, ro, rs in exe.secs:
        if ro <= i + 1 < ro + rs:
            sva = exe.base + va + (i + 1 - ro)
    if sva is None:
        return []
    ptr = struct.pack("<Q", sva)
    res = []
    j = 0
    while True:
        j = exe.d.find(ptr, j)
        if j < 0:
            break
        for _, va, vs, ro, rs in exe.secs:
            if ro <= j < ro + rs:
                res.append(exe.base + va + j - ro - 8)  # entry VA (name is at +8)
        j += 1
    return res


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    exe = Exe(sys.argv[1])
    thunks = scan_thunks(exe)
    print(f"# {len(thunks)} registered struct descriptors", file=sys.stderr)
    for name in sys.argv[2:]:
        print(f"== {name}")
        hits = find_entry_by_name(exe, name)
        if not hits:
            print("  (no entry with that name)")
        for ev in hits:
            f = Field(exe, ev)
            print(f"  entry @ {ev:#x} type={TYPE_NAMES.get(f.type, hex(f.type))} nested={f.nested:#x}")
            if f.type == 0xC:
                sub = fields_of(exe, thunks, f.nested)
                if sub:
                    dump(exe, thunks, sub, 2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
