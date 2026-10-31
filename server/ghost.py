"""Reads the ghost a client uploads for a Rivals run (Scoreboard.UploadFile). Nothing here trusts the client's bytes: every size is
checked, and anything that does not walk cleanly raises GhostError. Layout recovered from the game (build 1.0.0.64), docs/protocol.md.

Blob: u32 header section length, u32 stream section length, then both sections, each a run of compression chunks (u32 compressed size,
u32 raw size, zlib; IOSys::CCompressionStream, as for liveries). The header section holds the setup; the stream is the replay.
Stream: records of f32 time, one packet (below), u32 size of that packet without its time (the game seeks backwards with it).
Packet: u8 header, low 3 bits = kind (0 attach, 1 keyframe, 2 update; FUN_1413c8750), high 5 bits = message id for updates.
  Attach: 7 bytes. Keyframe: u32 count, then that many packets without time or size. Update: u16 object id, u32 size only if the
  message id has no fixed size (table filled by FUN_1402249a0), payload.
"""
import collections
import struct
import zlib

MAX_BLOB = 1 << 20        # stored ghost; the largest of 17,000 real ones is 362 KB (2026-10-30)
MAX_RAW = 8 << 20         # both sections inflated
MAX_CHUNK_RAW = 1 << 16   # the game writes 4096-byte chunks
MAX_KEYFRAME_PACKETS = 4096

MSG_CAR_POS = 0           # CMsgCarPosRaw
MSG_CAR_KEYFRAME = 2      # CMsgCarKeyframe
MSG_SCORE_UPDATE = 4      # msgInProgressScoreUpdate
MSG_LAP_SCORE = 5         # msgLapScore
MSG_CAR_COLLISION = 7     # CMsgCarCollision
MSG_SIZES = {0: 0x15, 2: 0xca, 3: 4, 4: 5, 5: 0x1d, 6: 0x2f, 7: 0x18, 8: 0xa4, 0xb: 1, 0xc: 0x5ac, 0xd: 1, 0xe: 0x10, 0xf: 0x54, 0x10: 2,
             0x12: 0x60, 0x13: 0x28, 0x14: 0x50, 0x19: 4, 0x1a: 4, 0x1c: 0xc}      # the rest carry their size

# World box the game quantises CMsgCarPosRaw against (FUN_141047cd0); fitted on 5,062 keyframes, worst error 5 mm.
X_MIN, X_SPAN = -16000.0, 32000.0
Y_MIN, Y_SPAN = -1000.0, 2020.0
Z_MIN, Z_SPAN = -16000.0, 32000.0
X_MAX_Q, Y_MAX_Q, Z_MAX_Q = (1 << 22) - 1, (1 << 20) - 1, (1 << 22) - 1

Frame = collections.namedtuple("Frame", "t x y z input_a input_b steer input_d input_e gear flags rpm")
Keyframe = collections.namedtuple("Keyframe", "t x y z vx vy vz")
Ghost = collections.namedtuple("Ghost", "header frames keyframes last_t counts")


class GhostError(ValueError):
    pass


def _chunks(section):
    out, o = [], 0
    total = 0
    while o < len(section):
        if o + 8 > len(section):
            raise GhostError("truncated chunk header")
        comp, raw = struct.unpack_from("<II", section, o)
        o += 8
        if comp > len(section) - o or raw > MAX_CHUNK_RAW:
            raise GhostError("chunk size out of range")
        total += raw
        if total > MAX_RAW:
            raise GhostError("inflates beyond the limit")
        d = zlib.decompressobj()
        try:
            data = d.decompress(section[o:o + comp], raw + 1)     # never more than the declared size, however small the upload
        except zlib.error as e:
            raise GhostError("bad zlib data") from e
        if len(data) != raw or not d.eof:
            raise GhostError("chunk does not match its declared size")
        out.append(data)
        o += comp
    return b"".join(out)


def sections(blob):
    """The inflated (header, stream) of a stored ghost."""
    if not 8 <= len(blob) <= MAX_BLOB:
        raise GhostError("size out of range")
    head, rest = struct.unpack_from("<II", blob, 0)
    if head + rest + 8 != len(blob):
        raise GhostError("section lengths do not match the blob")
    return _chunks(blob[8:8 + head]), _chunks(blob[8 + head:])


def _packet_end(d, o):
    """Offset just past the packet at `o` (its header byte)."""
    if o >= len(d):
        raise GhostError("truncated packet")
    h = d[o]
    kind, o = h & 7, o + 1
    if kind == 0:
        end = o + 7
    elif kind == 1:
        if o + 4 > len(d):
            raise GhostError("truncated keyframe")
        n = struct.unpack_from("<I", d, o)[0]
        if n > MAX_KEYFRAME_PACKETS:
            raise GhostError("keyframe too large")
        end = o + 4
        for _ in range(n):
            end = _packet_end(d, end)
    elif kind == 2:
        size = MSG_SIZES.get(h >> 3, 0)
        o += 2
        if not size:
            if o + 4 > len(d):
                raise GhostError("truncated update")
            size = struct.unpack_from("<I", d, o)[0]
            o += 4
        end = o + size
    else:
        raise GhostError("unknown packet kind")
    if end > len(d):
        raise GhostError("packet runs past the stream")
    return end


def _update(d, o):
    """(message id, object id, payload) of the update packet at `o`."""
    msg = d[o] >> 3
    start = o + 3
    if not MSG_SIZES.get(msg, 0):
        start += 4
    return msg, struct.unpack_from("<H", d, o + 1)[0], d[start:_packet_end(d, o)]


def _pos(t, p):
    """CMsgCarPosRaw (FUN_141047cd0). The inputs are named where the data settled them; see docs/protocol.md."""
    v = int.from_bytes(p[:8], "little")
    packed = p[17] | p[18] << 8
    return Frame(t,
                 X_MIN + (v & X_MAX_Q) / X_MAX_Q * X_SPAN,
                 Y_MIN + (v >> 22 & Y_MAX_Q) / Y_MAX_Q * Y_SPAN,
                 Z_MIN + (v >> 42 & Z_MAX_Q) / Z_MAX_Q * Z_SPAN,
                 (packed & 0x7f) / 127, (packed >> 7 & 0x3f) / 63,
                 struct.unpack_from("<b", p, 16)[0] / 127,
                 p[14] / 255, (p[15] & 0xf) / 15, p[15] >> 4, packed >> 13,
                 struct.unpack_from("<H", p, 19)[0] / 65535)


def parse(blob):
    """A stored ghost as a Ghost. `frames` and `keyframes` belong to the car with the most position records."""
    header, s = sections(blob)
    per_car, keys, counts = collections.defaultdict(list), collections.defaultdict(list), collections.Counter()
    o, last_t = 0, -1.0

    def take(t, at):
        msg, obj, p = _update(s, at)
        counts[msg] += 1
        if msg == MSG_CAR_POS:
            per_car[obj].append(_pos(t, p))
        elif msg == MSG_CAR_KEYFRAME:
            keys[obj].append(Keyframe(t, *struct.unpack_from("<fff", p, 0), *struct.unpack_from("<fff", p, 16)))

    while o < len(s):
        if o + 5 > len(s):
            raise GhostError("truncated record")
        t = struct.unpack_from("<f", s, o)[0]
        if not 0 <= t < 86400:
            raise GhostError("record time out of range")
        if t < last_t:
            raise GhostError("record times run backwards")
        last_t, start = t, o + 4
        end = _packet_end(s, start)
        if end + 4 > len(s) or struct.unpack_from("<I", s, end)[0] != end - start - 1:
            raise GhostError("record size does not match")
        kind = s[start] & 7
        if kind == 2:
            take(t, start)
        elif kind == 1:
            at = start + 5
            for _ in range(struct.unpack_from("<I", s, start + 1)[0]):
                if s[at] & 7 == 2:
                    take(t, at)
                at = _packet_end(s, at)
        o = end + 4
    if not per_car:
        raise GhostError("no car frames")
    car = max(per_car, key=lambda k: len(per_car[k]))
    return Ghost(header, per_car[car], keys.get(car, []), last_t, counts)
