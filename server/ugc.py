"""User-generated content store: paint jobs (liveries), tunes, vinyl groups, photos, with likes and downloads.

The game's content services are three layers (call layouts recovered from the exe, see docs/lsp.md):
- `Xls.WebServices.Content` + `Forza.WebServices.LargeFileUpload`: a numbered large blob (`contentFileId`), uploaded in chunks
  (`UploadContent{contentId, payloadOffset, payloadTotalSize, uploadData}`) and downloaded in chunks;
- `Forza.WebServices.ContentVersioning`: immutable "versioned" items (livery, layer group, tune) with a 64-bit id, a creator and
  a reference count (`CreateVersioned*`, `RetrieveVersioned*ById`, `AddReferenceTo*`, `RemoveReferenceTo*`);
- `Forza.WebServices.Storefront*`: the public listing of a versioned item (title, keywords, preview, counters) that other
  players search (`Search*Files`), fetch (`GetFileHeavyWeightData`) and like.
Photos have no versioned item: `StorefrontPhoto.Create` returns the storefront file id and the client then uploads the full picture with
`UploadContent{contentId = that file id}` (prod logs), so a photo's file id doubles as its content id (`content_blob`).
Everything is stored in the state database (PostgreSQL, db.py). Which request carries a livery's layer data is still being learned from
live traffic, so every blob and versioned row keeps whatever the client sent.
"""
import json
import lzma
import shutil
import struct
import subprocess
import time
import zlib

import diag

LOG = diag.get("ugc")
_RATE = diag.RateLimit()
DAMAGED_LOG_SECONDS = 600.0


MAX_REQUEST_BODY = 16 << 20       # largest HTTP body the LSP reads from a game client
MAX_BLOB_SIZE = 8 << 20           # one uploaded blob (a livery container, a tune, a vinyl group), declared total size
MAX_CHUNK = MAX_BLOB_SIZE         # one upload chunk
MAX_LIVERY_RAW = 16 << 20         # a livery's inflated container
MAX_BLOBS_PER_OWNER = 400
MAX_BYTES_PER_OWNER = 256 << 20   # declared sizes of one account's blobs, together


class LimitError(ValueError):
    """An upload that breaks a size, offset or quota limit."""


MAGIC = b"\x89FZ"        # prefix of a packed value: MAGIC + codec byte (0 = stored, 1 = LZMA) + payload; rows without it are legacy raw bytes


MAX_UNPACKED = 32 << 20  # largest value unpack() will expand (twice the biggest legitimate blob: an inflated livery container)


CONTENT_KINDS = ("photo", "replay")     # storefront kinds whose payload is uploaded with UploadContent under the file id


PHOTO_SIZE = (1920, 1080)         # the Xbox One's native photo size: the game shows a photo of any other size cropped
PHOTO_MAX_DIM = 8192              # larger uploads are left alone (the resizer would need too much memory)
PHOTO_QUALITY = 90
PHOTO_TIMEOUT = 30


def jpeg_size(data):
    """(width, height) from the first start-of-frame marker of a JPEG, None for anything that is not a well-formed JPEG header."""
    data = bytes(data)
    if data[:3] != b"\xff\xd8\xff":
        return None
    i = 2
    try:
        while i + 4 <= len(data):
            if data[i] != 0xFF:
                return None
            m = data[i + 1]
            if m == 0xFF:
                i += 1
                continue
            if m in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
                h, w = struct.unpack(">HH", data[i + 5:i + 9])
                return (w, h) if w and h else None
            if m in (0xD8, 0x01) or 0xD0 <= m <= 0xD7:
                i += 2
                continue
            i += 2 + struct.unpack(">H", data[i + 2:i + 4])[0]
    except struct.error:
        pass
    return None


def photo_1080p(data):
    """A photo upload as 1920x1080 JPEG. The game saves photos at the player's render resolution but shows them for a 1080p screen, so a
    photo of any other size appears cropped to every viewer. Other aspect ratios are scaled to cover the frame and cut in the middle.
    Returns `data` unchanged when it already has that size, is not a JPEG (or is implausibly large) or ImageMagick is missing or fails."""
    size = jpeg_size(data)
    if size is None or size == PHOTO_SIZE or max(size) > PHOTO_MAX_DIM:
        return data
    tool = shutil.which("magick") or shutil.which("convert")
    if tool is None:
        ok, skipped = _RATE.allow("photo-no-tool", DAMAGED_LOG_SECONDS)
        if ok:
            LOG.warn(f"photo stays {size[0]}x{size[1]} instead of {PHOTO_SIZE[0]}x{PHOTO_SIZE[1]}: ImageMagick (magick / convert) is not installed, so the game shows it cropped"
                     + (f" ({skipped} photos since the last line)" if skipped else ""))
        return data
    w, h = PHOTO_SIZE
    # `jpeg:` pins the decoder to JPEG, whatever the bytes claim to be
    cmd = [tool, "-limit", "memory", "512MiB", "-limit", "map", "512MiB", "-limit", "disk", "0", "jpeg:-", "-auto-orient", "-strip",
           "-resize", f"{w}x{h}^", "-gravity", "center", "-extent", f"{w}x{h}", "-quality", str(PHOTO_QUALITY), "jpeg:-"]
    try:
        out = subprocess.run(cmd, input=bytes(data), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=PHOTO_TIMEOUT, check=True).stdout
    except (OSError, subprocess.SubprocessError) as e:
        LOG.warn(f"photo {size[0]}x{size[1]} ({len(data)} bytes) not converted to {PHOTO_SIZE[0]}x{PHOTO_SIZE[1]}, stored as uploaded: {type(e).__name__}: {e}")
        return data
    if jpeg_size(out) == PHOTO_SIZE and len(out) <= MAX_BLOB_SIZE:
        return out
    LOG.warn(f"photo {size[0]}x{size[1]} ({len(data)} bytes) not converted: the converter produced {jpeg_size(out)} in {len(out)} bytes "
             f"(wanted {PHOTO_SIZE} within {MAX_BLOB_SIZE} bytes), stored as uploaded")
    return data


def pack(b):
    """Compresses a stored blob as hard as it goes (LZMA, preset 9 extreme); keeps the raw bytes when that does not help.
    Always encodes: bytes that already start with MAGIC are client data, not our storage format, and get wrapped like any other."""
    if not b:
        return b
    b = bytes(b)
    c = lzma.compress(b, format=lzma.FORMAT_ALONE, preset=9 | lzma.PRESET_EXTREME)
    return MAGIC + b"\x01" + c if len(c) + 4 < len(b) else MAGIC + b"\x00" + b


def pack_stored(b):
    """pack() without compressing: used for a blob that is still being uploaded, so every chunk does not re-run LZMA over the whole blob."""
    return MAGIC + b"\x00" + bytes(b) if b else b""


def unpack(b):
    """Inverse of pack (rows without MAGIC are legacy raw bytes). The expansion is capped at MAX_UNPACKED."""
    if not b:
        return b"" if b is None else bytes(b)
    b = bytes(b)
    if b[:3] != MAGIC or len(b) < 4:
        return b
    if b[3] == 0:
        return b[4:]
    if b[3] != 1:
        raise LimitError(f"stored value has an unknown codec {b[3]}")
    d = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE, memlimit=128 << 20)    # pack() needs ~65 MiB (preset 9); a header asking more is not ours
    try:
        out = d.decompress(b[4:], MAX_UNPACKED + 1)
    except lzma.LZMAError as e:
        raise LimitError(f"stored value is corrupt: {e}")
    if len(out) > MAX_UNPACKED:
        raise LimitError("stored value expands beyond the limit")
    return out


def _livery_chunk(blob):
    """(inflated container, None) or (None, why it is not one)."""
    try:
        comp, raw = struct.unpack("<II", blob[:8])
        if len(blob) != 8 + comp or raw > MAX_LIVERY_RAW:
            return None, (f"header says {comp} compressed bytes (blob has {len(blob) - 8}) and {raw} raw bytes (limit {MAX_LIVERY_RAW})")
        d = zlib.decompressobj()
        out = d.decompress(blob[8:], raw + 1)
        if len(out) == raw and not d.unconsumed_tail and d.eof:
            return out, None
        return None, f"inflates to {len(out)} bytes, the header says {raw}" + ("" if d.eof else " and the zlib stream does not end")
    except struct.error:
        return None, f"blob is {len(blob)} bytes, shorter than the 8 byte chunk header"
    except zlib.error as e:
        return None, f"zlib: {e}"


def livery_chunk(blob):
    """Checks a livery blob in the game's compression-stream form (IOSys::CCompressionStream, zlib mode, recovered from the exe):
    one chunk = u32 LE compressed size + u32 LE raw size + that many zlib bytes. Returns the inflated 'vlrc' container or None.
    The inflate never produces more than the declared raw size (itself capped), so a small upload cannot expand into gigabytes."""
    return _livery_chunk(blob)[0]


def strip_livery_prefix(data):
    """Rows stored before 2026-10-18 kept the 13 bytes in front of the blob (flag, creator xuid, blob length): returns the bare blob."""
    if data and len(data) > 13 and livery_chunk(data) is None and int.from_bytes(data[9:13], "big") == len(data) - 13:
        return data[13:]
    return data


def parse_layergroup_upload(body):
    """`OnlineProfile.UpdateLiveryGroup` request body: u32 name length + name, u16 layer count, u8 flag, u64 creator xuid, then the blob
    (u32 length + bytes). The client builds the call like UpdateLivery's without the car id (FUN_140f07ce0 against FUN_140f07b50); the blob is
    kept and served exactly as sent, as a design's is. Returns {"name", "layers", "payload"} or None when the body does not fit that layout."""
    try:
        n = struct.unpack(">I", body[:4])[0]
        name = body[4:4 + n].decode("utf-8", "replace")
        if len(name.encode()) > 4096:
            LOG.warn(f"UpdateLiveryGroup body ({len(body)} bytes) refused: name of {len(name.encode())} bytes (limit 4096)")
            return None
        (layers,) = struct.unpack(">H", body[4 + n:6 + n])
        p = 6 + n + 1 + 8
        ln = struct.unpack(">I", body[p:p + 4])[0]
        blob = body[p + 4:p + 4 + ln]
        if len(blob) != ln or ln == 0:
            LOG.warn(f"UpdateLiveryGroup body ({len(body)} bytes, name {name[:30]!r}) is not a vinyl group: {ln} bytes declared, {len(blob)} present")
            return None
        return {"name": name, "layers": layers, "payload": blob}
    except (struct.error, ValueError) as e:
        LOG.warn(f"UpdateLiveryGroup body ({len(body)} bytes) does not follow the recovered layout: {type(e).__name__}: {e}")
        return None


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
        container, why = _livery_chunk(blob)
        if container is None:
            LOG.warn(f"UpdateLivery body ({len(body)} bytes, name {name[:30]!r}, car {car_id}) is not a livery: the blob of {ln} declared bytes ({len(blob)} present) {why}")
            return None
        return {"name": name, "car_id": car_id, "layers": layers, "container": container, "payload": blob}
    except (struct.error, ValueError) as e:
        LOG.warn(f"UpdateLivery body ({len(body)} bytes) does not follow the recovered layout: {type(e).__name__}: {e}")
        return None


# a listed file whose payload is uploaded after the listing (photos) shows up once the upload is complete
COMPLETE = (f"(f.blob_id IS NULL OR f.kind NOT IN ({', '.join(repr(k) for k in CONTENT_KINDS)}) "
            "OR EXISTS (SELECT 1 FROM ugc_blobs b WHERE b.id = f.blob_id AND b.received >= b.total_size))")

# ugc_files columns except `preview` (a packed blob): list queries never carry it, file_get / files() unpack and return it
LIST_COLS = ("id, kind, type, owner, title, description, created, payload_size, versioned_id, blob_id, car_id, make_id, model_id, "
             "class_id, keyword1, keyword2, env, track, pi, downloads, likes, uses, views, popularity, removed")


# how well a file is liked, for ordering lists and for the star rating: a like says the most, a use (the item was put on a car or in a race)
# less, a download least
SCORE_SQL = "(f.likes * 3 + f.uses * 2 + f.downloads)"
STAR_STEPS = (1, 6, 20, 60, 150)


def popularity(f):
    """The 0-5 PopularityRating the client shows (a byte in the file lists): one more star at each step of the like/use/download score."""
    score = f["likes"] * 3 + f["uses"] * 2 + f["downloads"]
    return sum(score >= t for t in STAR_STEPS)


class UgcStore:
    def __init__(self, db):
        self.db = db

    # ---------------------------------------------------------------- large blobs (Content / LargeFileUpload)
    def blob_create(self, owner, name, total):
        total = int(total)
        if not 0 <= total <= MAX_BLOB_SIZE:
            raise LimitError(f"a blob may be at most {MAX_BLOB_SIZE} bytes (declared {total})")
        with self.db.tx() as c:
            q = c.execute("SELECT COUNT(*) AS n, COALESCE(SUM(total_size), 0) AS b FROM ugc_blobs WHERE owner=?", (owner,)).fetchone()
            if q["n"] >= MAX_BLOBS_PER_OWNER or q["b"] + total > MAX_BYTES_PER_OWNER:
                raise LimitError(f"upload quota of this account reached ({q['n']} of {MAX_BLOBS_PER_OWNER} blobs, {q['b']} + {total} of {MAX_BYTES_PER_OWNER} bytes)")
            return c.execute("INSERT INTO ugc_blobs (owner, user_file_name, total_size, received, data, created) VALUES (?,?,?,0,?,?)",
                             (owner, name or "", total, b"", time.time())).lastrowid

    def blob_write(self, blob_id, offset, data, owner, finish=None):
        """Writes a chunk at `offset` into a blob of `owner`; returns the number of bytes received so far (None for an unknown id or
        somebody else's blob). The chunk, its offset and the end of the write are checked against the declared total size BEFORE
        any buffer is allocated. `finish(bytes) -> bytes` converts the completed blob before it counts as complete (so nobody is served
        the original); a blob and its listing then carry the converted size, the return value still counts the uploaded bytes."""
        offset = int(offset)
        if offset < 0 or len(data) > MAX_CHUNK:
            raise LimitError(f"bad upload chunk (offset {offset}, {len(data)} bytes, limit {MAX_CHUNK} per chunk) for blob {blob_id}")
        with self.db.tx() as c:
            r = c.execute("SELECT data, total_size, received FROM ugc_blobs WHERE id=? AND owner=?", (blob_id, owner)).fetchone()
            if r is None:
                return None
            if finish is not None and int(r["received"]) >= int(r["total_size"]):
                return int(r["received"])             # a repeated last chunk: the stored blob is already converted
            if offset + len(data) > min(int(r["total_size"]), MAX_BLOB_SIZE):
                raise LimitError(f"upload chunk beyond the declared size (blob {blob_id}: offset {offset} + {len(data)} bytes, declared {r['total_size']})")
            buf = bytearray(unpack(r["data"]))
            if offset > len(buf):
                buf.extend(b"\0" * (offset - len(buf)))
            buf[offset:offset + len(data)] = data
            done = len(buf) >= int(r["total_size"])
            uploaded = len(buf)
            if done and finish is not None:
                buf = bytearray(finish(bytes(buf)))
                if len(buf) != uploaded:
                    c.execute("UPDATE ugc_blobs SET total_size=? WHERE id=? AND owner=?", (len(buf), blob_id, owner))
                    c.execute("UPDATE ugc_files SET payload_size=? WHERE blob_id=?", (len(buf), blob_id))
            n = c.execute("UPDATE ugc_blobs SET data=?, received=? WHERE id=? AND owner=?",
                          ((pack if done else pack_stored)(bytes(buf)), len(buf), blob_id, owner)).rowcount
            return uploaded if n else None

    def blob_read(self, blob_id, offset, size):
        """(chunk, is_last, total) or None."""
        r = self.db.one("SELECT data, total_size FROM ugc_blobs WHERE id=?", (blob_id,))
        if r is None:
            return None
        data = unpack(r["data"])
        chunk = data[offset:offset + size] if size else data[offset:]
        return chunk, offset + len(chunk) >= len(data), len(data)

    def content_blob(self, content_id):
        """The blob behind a Content.Upload/DownloadContent id: a photo's (or replay's) storefront file id names its blob; any other id is a
        blob id from LargeFileUpload.Create. File ids win on a clash: the game only uses UploadContent for photos (prod logs)."""
        r = self.db.one(f"SELECT blob_id FROM ugc_files WHERE id=? AND kind IN ({','.join('?' * len(CONTENT_KINDS))}) AND blob_id IS NOT NULL",
                        (content_id, *CONTENT_KINDS))
        return r["blob_id"] if r else content_id

    def is_photo(self, content_id):
        return self.db.one("SELECT 1 FROM ugc_files WHERE id=? AND kind='photo' AND blob_id IS NOT NULL", (content_id,)) is not None

    def blob_by_name(self, owner, name):
        r = self.db.one("SELECT id, data FROM ugc_blobs WHERE owner=? AND user_file_name=? ORDER BY id DESC LIMIT 1", (owner, name))
        return (r["id"], unpack(r["data"])) if r else None

    # ---------------------------------------------------------------- versioned items
    def versioned_create(self, kind, owner, name="", car_id=None, thumb=b"", layers=0, parts=None, tuning=None, data=None):
        layers = max(0, int(layers or 0))
        with self.db.tx() as c:
            return c.execute("INSERT INTO ugc_versioned (kind, car_id, name, creator, layers, data, thumb, thumb_small, parts, tuning, refs, created) "
                             "VALUES (?,?,?,?,?,?,?,?,?,?,1,?)",
                             (kind, car_id, name, owner, layers, pack(data) if data is not None else None, pack(thumb), pack(thumb),
                              json.dumps(parts) if parts is not None else None,
                              json.dumps(tuning) if tuning is not None else None, time.time())).lastrowid

    def versioned_get(self, vid):
        r = self.db.one("SELECT * FROM ugc_versioned WHERE id=?", (vid,))
        if not r:
            return None
        d = dict(r)
        for k in ("data", "thumb", "thumb_small"):
            try:
                d[k] = unpack(d[k])
            except LimitError as e:
                d[k] = b""              # a damaged row reads as empty instead of failing every listing that contains it
                ok, skipped = _RATE.allow(("damaged", vid, k), DAMAGED_LOG_SECONDS)
                if ok:
                    LOG.error(f"versioned item {vid} ({d.get('kind')}, creator {d.get('creator')}) has a damaged {k}, served empty: {e}")
        return d

    def versioned_set_data(self, vid, data, creator=None):
        """`creator` (when given) is part of the WHERE clause: only the item's creator can change it. Returns the rows changed."""
        with self.db.tx() as c:
            if creator is None:
                return c.execute("UPDATE ugc_versioned SET data=? WHERE id=?", (pack(data), vid)).rowcount
            return c.execute("UPDATE ugc_versioned SET data=? WHERE id=? AND creator=?", (pack(data), vid, creator)).rowcount

    def versioned_set_thumbs(self, vid, big, small, creator=None):
        with self.db.tx() as c:
            if creator is None:
                return c.execute("UPDATE ugc_versioned SET thumb=?, thumb_small=? WHERE id=?", (pack(big), pack(small), vid)).rowcount
            return c.execute("UPDATE ugc_versioned SET thumb=?, thumb_small=? WHERE id=? AND creator=?",
                             (pack(big), pack(small), vid, creator)).rowcount

    def versioned_set_layers(self, vid, layers, creator):
        layers = max(0, int(layers))            # the client sends -1 for "unknown"; a stored count is never negative (it feeds the layer filter)
        with self.db.tx() as c:
            return c.execute("UPDATE ugc_versioned SET layers=? WHERE id=? AND creator=?", (layers, vid, creator)).rowcount

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
        """Public files of a kind, best liked first (then newest); 0 / -1 mean "any" [guess: the client's "no filter" values are unknown]."""
        where, args = ["f.kind=?", "f.removed=0", COMPLETE], [kind]
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
            f"SELECT {', '.join('f.' + c.strip() for c in LIST_COLS.split(','))} FROM ugc_files f WHERE {' AND '.join(where)} ORDER BY {SCORE_SQL} DESC, f.id DESC LIMIT ?", args + [limit])]

    def curated(self, ctype, car_id, limit):
        """Recommended content: files of a type for a car (any car when `car_id` <= 0), most liked first."""
        return [dict(r) for r in self.db.query(
            f"SELECT {', '.join('f.' + c.strip() for c in LIST_COLS.split(','))} FROM ugc_files f WHERE f.removed=0 AND f.type=? "
            f"AND (?<=0 OR f.car_id=?) ORDER BY {SCORE_SQL} DESC, f.id DESC LIMIT ?",
            (ctype, car_id, car_id, limit))]

    def popular(self, kind, limit, since=None):
        """Complete public files of a kind, best liked first (then most viewed, newest); `since` keeps files shared after that time."""
        where, args = ["f.kind=?", "f.removed=0", COMPLETE], [kind]
        if since is not None:
            where.append("f.created >= ?")
            args.append(since)
        return [dict(r) for r in self.db.query(
            f"SELECT {', '.join('f.' + c.strip() for c in LIST_COLS.split(','))} FROM ugc_files f WHERE {' AND '.join(where)} "
            f"ORDER BY {SCORE_SQL} DESC, f.views DESC, f.id DESC LIMIT ?", args + [limit])]

    def bump(self, fid, column):
        assert column in ("views", "likes", "uses", "downloads")
        with self.db.tx() as c:
            c.execute(f"UPDATE ugc_files SET {column}={column}+1 WHERE id=?", (fid,))

    def record_action(self, fid, xuid, action, column):
        """Counts `column` of file `fid` once for this player and action; True when it counted, False for a repeat, the file's own creator
        or an unknown / removed file. (A download or a like is one per player and file, whatever the client sends again.)"""
        assert action in ("download", "like") and column in ("downloads", "likes")
        with self.db.tx() as c:
            f = c.execute("SELECT owner FROM ugc_files WHERE id=? AND removed=0", (fid,)).fetchone()
            if f is None or f["owner"] == xuid:
                return False
            if not c.execute("INSERT INTO ugc_actions (file_id, xuid, action, at) VALUES (?,?,?,?) ON CONFLICT DO NOTHING",
                             (fid, xuid, action, time.time())).rowcount:
                return False
            c.execute(f"UPDATE ugc_files SET {column}={column}+1 WHERE id=?", (fid,))
            return True

    def file_of_versioned(self, vid):
        """The newest public file that lists a versioned item (a design shared twice counts for its latest listing), or None."""
        r = self.db.one("SELECT id FROM ugc_files WHERE versioned_id=? AND removed=0 ORDER BY id DESC LIMIT 1", (vid,))
        return r["id"] if r else None

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

    def repair_photos(self):
        """Converts completed photo uploads that are not 1920x1080 yet (see photo_1080p). Returns the number of photos rewritten."""
        n = 0
        for r in self.db.query("SELECT f.id AS fid, b.id AS bid, b.data FROM ugc_files f JOIN ugc_blobs b ON b.id = f.blob_id "
                               "WHERE f.kind='photo' AND b.received >= b.total_size AND b.total_size > 0"):
            old = unpack(r["data"])
            new = photo_1080p(old)
            if new == old:
                continue
            with self.db.tx() as c:
                c.execute("UPDATE ugc_blobs SET data=?, total_size=?, received=? WHERE id=?", (pack(new), len(new), len(new), r["bid"]))
                c.execute("UPDATE ugc_files SET payload_size=? WHERE id=?", (len(new), r["fid"]))
            n += 1
        return n

    def stored_bytes(self):
        return self.db.one("SELECT (SELECT COALESCE(SUM(LENGTH(data)),0) FROM ugc_blobs) + (SELECT COALESCE(SUM(LENGTH(data)+LENGTH(thumb)+LENGTH(thumb_small)),0) "
                           "FROM ugc_versioned) + (SELECT COALESCE(SUM(LENGTH(preview)),0) FROM ugc_files)")[0]
