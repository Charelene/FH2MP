"""User-generated content store: paint jobs (liveries), tunes, vinyl groups, with likes and downloads.

The game's content services are three layers (call layouts recovered from the exe, see docs/lsp.md):
- `Xls.WebServices.Content` + `Forza.WebServices.LargeFileUpload`: a numbered large blob (`contentFileId`), uploaded in chunks
  (`UploadContent{contentId, payloadOffset, payloadTotalSize, uploadData}`) and downloaded in chunks;
- `Forza.WebServices.ContentVersioning`: immutable "versioned" items (livery, layer group, tune) with a 64-bit id, a creator and
  a reference count (`CreateVersioned*`, `RetrieveVersioned*ById`, `AddReferenceTo*`, `RemoveReferenceTo*`);
- `Forza.WebServices.Storefront*`: the public listing of a versioned item (title, keywords, preview, counters) that other
  players search (`Search*Files`), fetch (`GetFileHeavyWeightData`) and like.
Everything is stored in the state database (PostgreSQL, db.py). Which request carries a livery's layer data is still being learned from
live traffic, so every blob and versioned row keeps whatever the client sent.
"""
import json
import lzma
import struct
import time
import zlib


MAGIC = b"\x89FZ"        # prefix of a packed value: MAGIC + codec byte (0 = stored, 1 = LZMA) + payload; rows without it are legacy raw bytes


def pack(b):
    """Compresses a stored blob as hard as it goes (LZMA, preset 9 extreme); keeps the raw bytes when that does not help."""
    if not b:
        return b
    b = bytes(b)
    if b[:3] == MAGIC:
        return b
    c = lzma.compress(b, format=lzma.FORMAT_ALONE, preset=9 | lzma.PRESET_EXTREME)
    return MAGIC + b"\x01" + c if len(c) + 4 < len(b) else MAGIC + b"\x00" + b


def unpack(b):
    if not b:
        return b"" if b is None else bytes(b)
    b = bytes(b)
    if b[:3] != MAGIC:
        return b
    return lzma.decompress(b[4:]) if b[3] == 1 else b[4:]


def livery_chunk(blob):
    """Checks a livery blob in the game's compression-stream form (IOSys::CCompressionStream, zlib mode, recovered from the exe):
    one chunk = u32 LE compressed size + u32 LE raw size + that many zlib bytes. Returns the inflated 'vlrc' container or None."""
    try:
        comp, raw = struct.unpack("<II", blob[:8])
        out = zlib.decompress(blob[8:8 + comp])
        return out if len(out) == raw and len(blob) == 8 + comp else None
    except (struct.error, zlib.error):
        return None


def strip_livery_prefix(data):
    """Rows stored before 2026-10-18 kept the 13 bytes in front of the blob (flag, creator xuid, blob length): returns the bare blob."""
    if data and len(data) > 13 and livery_chunk(data) is None and int.from_bytes(data[9:13], "big") == len(data) - 13:
        return data[13:]
    return data


def parse_livery_upload(body):
    """`OnlineProfile.UpdateLivery` request body (no descriptor in the exe, layout read from the real uploads and the client's own
    reader): u32 name length + name, i32 carId, u16 layer count, u8 flag, u64 creator xuid, then a blob (u32 BE length + bytes) in the
    compression-stream form of `livery_chunk`. `payload` is that bare blob: the client runs `RetrieveVersionedLiveryById.data`
    through the same stream class (FUN_1409b8390), so it must be served exactly like this: anything else decodes to 0 bytes and the
    save task then copies a garbage size and crashes (2026-10-18). Returns {"name","car_id","layers","container","payload"} or None."""
    try:
        n = struct.unpack(">I", body[:4])[0]
        name = body[4:4 + n].decode("utf-8", "replace")
        car_id, layers = struct.unpack(">IH", body[4 + n:4 + n + 6])
        p = 4 + n + 6 + 1 + 8
        ln = struct.unpack(">I", body[p:p + 4])[0]
        blob = body[p + 4:p + 4 + ln]
        container = livery_chunk(blob)
        if container is None:
            return None
        return {"name": name, "car_id": car_id, "layers": layers, "container": container, "payload": blob}
    except (struct.error, ValueError):
        return None


# ugc_files columns except `preview` (a packed blob): list queries never carry it, file_get / files() unpack and return it
LIST_COLS = ("id, kind, type, owner, title, description, created, payload_size, versioned_id, blob_id, car_id, make_id, model_id, "
             "class_id, keyword1, keyword2, env, track, pi, downloads, likes, uses, views, popularity, removed")


class UgcStore:
    def __init__(self, db):
        self.db = db

    # ---------------------------------------------------------------- large blobs (Content / LargeFileUpload)
    def blob_create(self, owner, name, total):
        with self.db.tx() as c:
            return c.execute("INSERT INTO ugc_blobs (owner, user_file_name, total_size, received, data, created) VALUES (?,?,?,0,?,?)",
                             (owner, name or "", int(total), b"", time.time())).lastrowid

    def blob_write(self, blob_id, offset, data):
        """Writes a chunk at `offset`; returns the number of bytes received so far (None for an unknown id)."""
        with self.db.tx() as c:
            r = c.execute("SELECT data FROM ugc_blobs WHERE id=?", (blob_id,)).fetchone()
            if r is None:
                return None
            buf = bytearray(unpack(r["data"]))
            if offset > len(buf):
                buf.extend(b"\0" * (offset - len(buf)))
            buf[offset:offset + len(data)] = data
            c.execute("UPDATE ugc_blobs SET data=?, received=? WHERE id=?", (pack(bytes(buf)), len(buf), blob_id))
            return len(buf)

    def blob_read(self, blob_id, offset, size):
        """(chunk, is_last, total) or None."""
        r = self.db.one("SELECT data, total_size FROM ugc_blobs WHERE id=?", (blob_id,))
        if r is None:
            return None
        data = unpack(r["data"])
        chunk = data[offset:offset + size] if size else data[offset:]
        return chunk, offset + len(chunk) >= len(data), len(data)

    def blob_by_name(self, owner, name):
        r = self.db.one("SELECT id, data FROM ugc_blobs WHERE owner=? AND user_file_name=? ORDER BY id DESC LIMIT 1", (owner, name))
        return (r["id"], unpack(r["data"])) if r else None

    # ---------------------------------------------------------------- versioned items
    def versioned_create(self, kind, owner, name="", car_id=None, thumb=b"", layers=0, parts=None, tuning=None, data=None):
        with self.db.tx() as c:
            return c.execute("INSERT INTO ugc_versioned (kind, car_id, name, creator, layers, data, thumb, thumb_small, parts, tuning, refs, created) "
                             "VALUES (?,?,?,?,?,?,?,?,?,?,1,?)",
                             (kind, car_id, name, owner, layers, data, thumb, thumb, json.dumps(parts) if parts is not None else None,
                              json.dumps(tuning) if tuning is not None else None, time.time())).lastrowid

    def versioned_get(self, vid):
        r = self.db.one("SELECT * FROM ugc_versioned WHERE id=?", (vid,))
        if not r:
            return None
        d = dict(r)
        for k in ("data", "thumb", "thumb_small"):
            d[k] = unpack(d[k])
        return d

    def versioned_set_data(self, vid, data):
        with self.db.tx() as c:
            c.execute("UPDATE ugc_versioned SET data=? WHERE id=?", (pack(data), vid))

    def versioned_set_thumbs(self, vid, big, small):
        with self.db.tx() as c:
            c.execute("UPDATE ugc_versioned SET thumb=?, thumb_small=? WHERE id=?", (pack(big), pack(small), vid))

    def versioned_ref(self, vid, delta):
        with self.db.tx() as c:
            c.execute("UPDATE ugc_versioned SET refs = GREATEST(0, refs + ?) WHERE id=?", (delta, vid))

    # ---------------------------------------------------------------- storefront files
    def file_create(self, kind, owner, title, description, ftype, versioned_id, payload_size=0, preview=b"", blob_id=None,
                    car_id=None, make_id=None, model_id=None, class_id=None, kw1=0, kw2=0, env=None, track=None, pi=None):
        with self.db.tx() as c:
            return c.execute(
                "INSERT INTO ugc_files (kind, type, owner, title, description, created, payload_size, preview, versioned_id, blob_id, "
                "car_id, make_id, model_id, class_id, keyword1, keyword2, env, track, pi) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                (kind, ftype, owner, title[:33], description[:513], time.time(), payload_size, pack(preview), versioned_id, blob_id, car_id,
                 make_id, model_id, class_id, kw1, kw2, env, track, pi)).lastrowid

    def file_get(self, fid):
        r = self.db.one("SELECT * FROM ugc_files WHERE id=? AND removed=0", (fid,))
        if not r:
            return None
        d = dict(r)
        d["preview"] = unpack(d["preview"])
        return d

    def files(self, ids):
        out = []
        for i in ids:
            f = self.file_get(i)
            if f:
                out.append(f)
        return out

    def mine(self, owner, ftype, limit):
        return [dict(r) for r in self.db.query(
            f"SELECT {LIST_COLS} FROM ugc_files WHERE owner=? AND type=? AND removed=0 ORDER BY id DESC LIMIT ?", (owner, ftype, limit))]

    def search(self, kind, make=0, model=0, cls=-1, kw1=0, kw2=0, env=0, track=0, layers=0, gamertag="", text="", limit=50):
        """Public files of a kind, newest first; 0 / -1 mean "any" [guess: the client's "no filter" values are unknown]."""
        where, args = ["f.kind=?", "f.removed=0"], [kind]
        for col, val in (("make_id", make), ("model_id", model), ("keyword1", kw1), ("keyword2", kw2), ("env", env), ("track", track)):
            if val and val > 0:
                where.append(f"f.{col}=?")
                args.append(val)
        if cls is not None and cls >= 0:
            where.append("f.class_id=?")
            args.append(cls)
        if text:
            where.append("(f.title ILIKE ? OR f.description ILIKE ?)")
            args += [f"%{text}%", f"%{text}%"]
        if gamertag:
            where.append("f.owner IN (SELECT xuid FROM players WHERE lower(gamertag)=lower(?))")
            args.append(gamertag)
        return [dict(r) for r in self.db.query(
            f"SELECT {', '.join('f.' + c.strip() for c in LIST_COLS.split(','))} FROM ugc_files f WHERE {' AND '.join(where)} ORDER BY f.likes DESC, f.id DESC LIMIT ?", args + [limit])]

    def curated(self, ctype, car_id, limit):
        """Recommended content: files of a type for a car (any car when `car_id` <= 0), most liked first."""
        return [dict(r) for r in self.db.query(
            f"SELECT {LIST_COLS} FROM ugc_files WHERE removed=0 AND type=? AND (?<=0 OR car_id=?) ORDER BY likes DESC, id DESC LIMIT ?",
            (ctype, car_id, car_id, limit))]

    def bump(self, fid, column):
        assert column in ("views", "likes", "uses", "downloads")
        with self.db.tx() as c:
            c.execute(f"UPDATE ugc_files SET {column}={column}+1 WHERE id=?", (fid,))

    def remove(self, fid, owner):
        with self.db.tx() as c:
            return c.execute("UPDATE ugc_files SET removed=1 WHERE id=? AND owner=?", (fid, owner)).rowcount

    def retitle(self, fid, owner, title=None, description=None):
        with self.db.tx() as c:
            if title is not None:
                c.execute("UPDATE ugc_files SET title=? WHERE id=? AND owner=?", (title[:33], fid, owner))
            if description is not None:
                c.execute("UPDATE ugc_files SET description=? WHERE id=? AND owner=?", (description[:513], fid, owner))

    def count(self, kind=None):
        r = self.db.one("SELECT COUNT(*) FROM ugc_files WHERE removed=0" + (" AND kind=?" if kind else ""), (kind,) if kind else ())
        return r[0]

    def recompress(self):
        """Packs every stored blob that is still raw (rows written before packing existed). Returns (bytes before, bytes after) of the
        rows that were rewritten, (0, 0) when everything was already packed."""
        before = after = 0
        with self.db.tx() as c:
            for table, cols in (("ugc_blobs", ("data",)), ("ugc_versioned", ("data", "thumb", "thumb_small")), ("ugc_files", ("preview",))):
                for r in c.execute(f"SELECT id, {', '.join(cols)} FROM {table}").fetchall():
                    for col in cols:
                        v = r[col]
                        if not v:
                            continue
                        v = bytes(v)
                        if v[:3] != MAGIC:
                            nv = pack(v)
                            c.execute(f"UPDATE {table} SET {col}=? WHERE id=?", (nv, r["id"]))
                            before += len(v)
                            after += len(nv)
        return before, after

    def repair_liveries(self):
        """Drops the stale 13-byte prefix from livery rows stored by the first parser. Returns the number of rows fixed."""
        n = 0
        with self.db.tx() as c:
            for r in c.execute("SELECT id, data FROM ugc_versioned WHERE kind='livery' AND data IS NOT NULL").fetchall():
                old = unpack(r["data"])
                new = strip_livery_prefix(old)
                if new != old:
                    c.execute("UPDATE ugc_versioned SET data=? WHERE id=?", (pack(new), r["id"]))
                    n += 1
        return n

    def stored_bytes(self):
        return self.db.one("SELECT (SELECT COALESCE(SUM(LENGTH(data)),0) FROM ugc_blobs) + (SELECT COALESCE(SUM(LENGTH(data)+LENGTH(thumb)+LENGTH(thumb_small)),0) "
                           "FROM ugc_versioned) + (SELECT COALESCE(SUM(LENGTH(preview)),0) FROM ugc_files)")[0]
