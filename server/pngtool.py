"""Minimal PNG downscaler (standard library only): the server makes the small design thumbnail the client expects from the big preview.

Supports what the game writes: 8-bit RGBA / RGB / grey+alpha / grey, non-interlaced. The small thumbnail of a design is 192x108 and the
big one 816x459 (both RGBA, measured on the player's own saved designs), so area averaging by 4.25 is needed; no external library.
"""
import struct
import zlib

_SIG = b"\x89PNG\r\n\x1a\n"
_CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}


MAX_DIM = 1024        # default size limit of decode(): the biggest picture the server handles is the 816x459 design preview


def _chunks(png):
    p = 8
    while p + 12 <= len(png):
        n, t = struct.unpack(">I4s", png[p:p + 8])
        if p + 12 + n > len(png):
            raise ValueError("truncated PNG chunk")
        yield t, png[p + 8:p + 8 + n]
        p += 12 + n


def decode(png, max_dim=MAX_DIM):
    """-> (width, height, channels, rows) with rows as a list of bytearrays (unfiltered).
    The header is the first chunk and the only one, and its dimensions are checked here, before anything is allocated."""
    if png[:8] != _SIG:
        raise ValueError("not a PNG")
    idat, w = [], None
    for i, (t, d) in enumerate(_chunks(png)):
        if t == b"IHDR":
            if i != 0 or w is not None or len(d) != 13:
                raise ValueError("bad or repeated PNG header")
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", d)
            if depth != 8 or interlace != 0 or ctype not in _CHANNELS:
                raise ValueError(f"unsupported PNG (depth {depth}, colour type {ctype}, interlace {interlace})")
            if not (1 <= w <= max_dim and 1 <= h <= max_dim):
                raise ValueError(f"PNG dimensions {w}x{h} are outside 1..{max_dim}")
        elif t == b"IDAT":
            if w is None:
                raise ValueError("no IHDR")
            idat.append(d)
    if w is None:
        raise ValueError("no IHDR")
    ch = _CHANNELS[ctype]
    stride = w * ch
    # bounded inflate: the data of a w x h image is exactly this long, so a small file cannot expand into gigabytes
    want = h * (stride + 1)
    d = zlib.decompressobj()
    raw = d.decompress(b"".join(idat), want + 1)
    if len(raw) < want or d.unconsumed_tail:
        raise ValueError("PNG data has the wrong size")
    rows, prev = [], bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if f == 1:                                        # Sub
            for i in range(ch, stride):
                line[i] = (line[i] + line[i - ch]) & 255
        elif f == 2:                                      # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:                                      # Average
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:                                      # Paeth
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                b = prev[i]
                c = prev[i - ch] if i >= ch else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        rows.append(line)
        prev = line
    return w, h, ch, rows


def encode(w, h, ch, rows):
    ctype = {1: 0, 2: 4, 3: 2, 4: 6}[ch]

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    return (_SIG + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def downscale(png, nw, nh):
    """Area-averaged copy of `png` at nw x nh (alpha averaged with the colour channels, which is fine for thumbnails)."""
    w, h, ch, rows = decode(png)
    xs = [(int(x * w / nw), max(int(x * w / nw) + 1, int((x + 1) * w / nw))) for x in range(nw)]
    out = []
    for y in range(nh):
        y0 = int(y * h / nh)
        y1 = max(y0 + 1, int((y + 1) * h / nh))
        # sum the source rows of this band per column, then average columns
        band = rows[y0:y1]
        line = bytearray(nw * ch)
        for x, (x0, x1) in enumerate(xs):
            cnt = (x1 - x0) * len(band)
            for c in range(ch):
                s = 0
                for r in band:
                    s += sum(r[x0 * ch + c:x1 * ch:ch])
                line[x * ch + c] = s // cnt
        out.append(line)
    return encode(nw, nh, ch, out)
