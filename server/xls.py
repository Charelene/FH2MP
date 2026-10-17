"""Encoder/decoder for Turn 10 "Xls" web-service structs (bin/xtsw), driven by the user's own exe.

Schemas are read at startup from forza_durango_release_final.exe with server/xls_schema.py, so no game
data is shipped in this repo. Wire format is documented in docs/lsp.md and server/xls_schema.py.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xls_schema as S  # noqa: E402

FIXED = {0: ">B", 1: ">H", 2: ">h", 3: ">i", 4: ">I", 5: ">q", 6: ">Q", 0x13: ">Q", 7: ">f", 0x11: ">?"}


class Schema:
    def __init__(self, exe_path):
        self.exe = S.Exe(exe_path)
        self.thunks = S.scan_thunks(self.exe)

    def call_struct(self, entry_name):
        """Root field list of e.g. 'ForzaUserLogon_Results' (a single wrapper field holding the data)."""
        for ev in S.find_entry_by_name(self.exe, entry_name):
            f = S.Field(self.exe, ev)
            if f.type == 0xC:
                fl = S.fields_of(self.exe, self.thunks, f.nested)
                if fl is not None:
                    return fl
        raise KeyError(entry_name)

    def sub(self, f):
        return S.fields_of(self.exe, self.thunks, f.nested)


class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def take(self, n):
        if self.p + n > len(self.d):
            raise ValueError("short read")
        b = self.d[self.p:self.p + n]
        self.p += n
        return b


def _read_value(sc, rd, f):
    t = f.type
    if t in FIXED:
        fmt = FIXED[t]
        return struct.unpack(fmt, rd.take(struct.calcsize(fmt)))[0]
    if t == 9 or t == 0x10:
        n = struct.unpack(">I", rd.take(4))[0]
        return rd.take(n).decode("utf-8", "replace")
    if t == 0x14:
        n = struct.unpack(">I", rd.take(4))[0]
        return bytes(rd.take(n))
    if t == 0x12:
        return bytes(rd.take(16))
    if t == 0xC:
        return read_struct(sc, rd, sc.sub(f))
    if t == 0xE:
        n = struct.unpack(">I", rd.take(4))[0]
        el = S.Field(sc.exe, f.elem)
        return [_read_value(sc, rd, el) for _ in range(n)]
    raise ValueError(f"unsupported type {t:#x} for {f.name}")


def read_struct(sc, rd, fields):
    return {f.name: _read_value(sc, rd, f) for f in fields}


def decode(sc, fields, data):
    rd = Reader(data)
    v = read_struct(sc, rd, fields)
    return v, len(data) - rd.p


def _write_value(sc, out, f, v):
    t = f.type
    if t == 2:  # same 16 bits on the wire; notificationType 0x8502 etc. exceed the signed range
        out += struct.pack(">H", v & 0xFFFF)
    elif t in FIXED:
        out += struct.pack(FIXED[t], v)
    elif t == 9 or t == 0x10:
        b = v.encode("utf-8")
        out += struct.pack(">I", len(b)) + b
    elif t == 0x14:
        out += struct.pack(">I", len(v)) + bytes(v)
    elif t == 0x12:
        out += bytes(v)
    elif t == 0xC:
        write_struct(sc, out, sc.sub(f), v)
    elif t == 0xE:
        el = S.Field(sc.exe, f.elem)
        if f.size and el.size and len(v) > f.size // el.size:
            raise ValueError(f"{f.name}: {len(v)} elements exceeds the client's capacity {f.size // el.size}")
        out += struct.pack(">I", len(v))
        for x in v:
            _write_value(sc, out, el, x)
    else:
        raise ValueError(f"unsupported type {t:#x} for {f.name}")


def write_struct(sc, out, fields, value):
    for f in fields:
        _write_value(sc, out, f, value[f.name])


def default_value(sc, f):
    t = f.type
    if t in FIXED:
        return False if t == 0x11 else (0.0 if t == 7 else 0)
    if t == 9:
        return ""
    if t == 0x10:
        return "1970-01-01T00:00:00Z"
    if t == 0x14:
        return b""
    if t == 0x12:
        return bytes(16)
    if t == 0xC:
        return default_struct(sc, sc.sub(f))
    if t == 0xE:
        return []
    raise ValueError(f"unsupported type {t:#x}")


def default_struct(sc, fields):
    return {f.name: default_value(sc, f) for f in fields}


def encode(sc, fields, value):
    out = bytearray()
    write_struct(sc, out, fields, value)
    return bytes(out)
