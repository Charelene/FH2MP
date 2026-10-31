"""ghost.py on small made-up ghosts (server side only, never sent to a game client), laid out as the game writes them."""
import os
import struct
import sys
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ghost


def chunks(raw, size=4096):
    out = b""
    for i in range(0, len(raw), size):
        part = raw[i:i + size]
        z = zlib.compress(part)
        out += struct.pack("<II", len(z), len(part)) + z
    return out


def blob(header, stream):
    a, b = chunks(header), chunks(stream)
    return struct.pack("<II", len(a), len(b)) + a + b


def pos_payload(x, y, z, a=1.0, b=0.0, steer=0.0, gear=3, rpm=0.5):
    q = lambda v, lo, span, top: int(round((v - lo) / span * top))     # noqa: E731
    bits = q(x, ghost.X_MIN, ghost.X_SPAN, ghost.X_MAX_Q) | q(y, ghost.Y_MIN, ghost.Y_SPAN, ghost.Y_MAX_Q) << 22 | q(z, ghost.Z_MIN, ghost.Z_SPAN, ghost.Z_MAX_Q) << 42
    packed = int(a * 127) | int(b * 63) << 7
    return (bits.to_bytes(8, "little") + bytes(6) + bytes([0, gear << 4]) + struct.pack("<b", int(steer * 127)) +
            struct.pack("<H", packed) + struct.pack("<H", int(rpm * 65535)))


def record(t, packet):
    return struct.pack("<f", t) + packet + struct.pack("<I", len(packet) - 1)


def update(msg, obj, payload):
    return bytes([2 | msg << 3]) + struct.pack("<H", obj) + payload


def stream(points, obj=0x90):
    return b"".join(record(t, update(ghost.MSG_CAR_POS, obj, pos_payload(x, y, z))) for t, x, y, z in points)


def drive(seconds=20.0, speed=40.0, hz=5, y=50.0):
    return [(i / hz, 100.0 + speed * i / hz, y, 200.0) for i in range(int(seconds * hz) + 1)]


class Parsing(unittest.TestCase):
    def test_positions_come_back_in_metres(self):
        g = ghost.parse(blob(b"h" * 5000, stream(drive())))
        self.assertEqual(len(g.header), 5000)
        self.assertEqual(len(g.frames), 101)
        f = g.frames[10]
        self.assertAlmostEqual(f.t, 2.0)
        self.assertAlmostEqual(f.x, 180.0, delta=0.01)
        self.assertAlmostEqual(f.y, 50.0, delta=0.01)
        self.assertAlmostEqual(f.z, 200.0, delta=0.01)
        self.assertEqual(f.gear, 3)
        self.assertAlmostEqual(f.rpm, 0.5, delta=0.001)
        self.assertAlmostEqual(g.last_t, 20.0)

    def test_the_inputs_field_order_matches_the_game(self):
        f = ghost._pos(1.0, pos_payload(0, 0, 0, a=1.0, b=1.0, steer=-1.0, gear=5))
        self.assertEqual((f.input_a, f.input_b, f.steer, f.gear), (1.0, 1.0, -1.0, 5))

    def test_keyframes_nest_packets_and_carry_position_and_velocity(self):
        key = struct.pack("<fff", 1, 2, 3) + bytes(4) + struct.pack("<fff", 40, 1, -2) + bytes(ghost.MSG_SIZES[2] - 28)
        inner = [update(ghost.MSG_CAR_KEYFRAME, 0x90, key), update(ghost.MSG_CAR_POS, 0x90, pos_payload(1, 2, 3))]
        attach = bytes([0]) + bytes(7)
        keyframe = bytes([1]) + struct.pack("<I", 3) + attach + b"".join(inner)
        g = ghost.parse(blob(b"h", record(0.0, keyframe) + stream(drive())))
        self.assertEqual(len(g.keyframes), 1)
        self.assertEqual(g.keyframes[0][1:], (1.0, 2.0, 3.0, 40.0, 1.0, -2.0))
        self.assertEqual(g.counts[ghost.MSG_CAR_KEYFRAME], 1)

    def test_a_message_without_a_fixed_size_carries_its_own(self):
        var = bytes([2 | 9 << 3]) + struct.pack("<H", 1) + struct.pack("<I", 3) + b"abc"
        g = ghost.parse(blob(b"h", record(0.0, var) + stream(drive())))
        self.assertEqual(g.counts[9], 1)

    def test_frames_of_the_car_that_drove_most_are_kept(self):
        g = ghost.parse(blob(b"h", stream(drive(2), obj=7) + stream([(21 + i, 0, 0, 0) for i in range(2)], obj=8)))
        self.assertEqual(len(g.frames), 11)


class Refusals(unittest.TestCase):
    def assert_refused(self, data):
        with self.assertRaises(ghost.GhostError):
            ghost.parse(data)

    def test_rubbish_and_empty(self):
        good = blob(b"h", stream(drive(2)))
        for bad in (b"", b"abc", b"\0" * 8, good[:-3], good + b"x", b"\xff" * 64):
            self.assert_refused(bad)

    def test_an_oversized_blob_is_not_even_read(self):
        self.assert_refused(b"\0" * (ghost.MAX_BLOB + 1))

    def test_a_small_upload_cannot_inflate_past_its_declared_size(self):
        z = zlib.compress(b"\0" * (1 << 24), 9)
        a = struct.pack("<II", len(z), 1 << 12) + z
        self.assert_refused(struct.pack("<II", len(a), 0) + a)
        a = struct.pack("<II", len(z), 1 << 24) + z
        self.assert_refused(struct.pack("<II", len(a), 0) + a)

    def test_chunks_cannot_add_up_past_the_limit(self):
        z = zlib.compress(b"\0" * ghost.MAX_CHUNK_RAW, 9)
        one = struct.pack("<II", len(z), ghost.MAX_CHUNK_RAW) + z
        many = one * (ghost.MAX_RAW // ghost.MAX_CHUNK_RAW + 2)
        self.assert_refused(struct.pack("<II", len(many), 0) + many)

    def test_a_wrong_record_size_stops_the_walk(self):
        s = bytearray(stream(drive(2)))
        s[28] ^= 1                      # the first record's trailing size
        self.assert_refused(blob(b"h", bytes(s)))

    def test_times_must_not_run_backwards_or_be_absurd(self):
        self.assert_refused(blob(b"h", stream([(5, 0, 0, 0), (4, 0, 0, 0)])))
        self.assert_refused(blob(b"h", stream([(float("nan"), 0, 0, 0)])))
        self.assert_refused(blob(b"h", stream([(-1, 0, 0, 0)])))

    def test_a_packet_cannot_claim_to_run_past_the_stream(self):
        var = bytes([2 | 9 << 3]) + struct.pack("<H", 1) + struct.pack("<I", 1 << 30)
        self.assert_refused(blob(b"h", struct.pack("<f", 0.1) + var))

    def test_a_huge_keyframe_count_is_refused(self):
        self.assert_refused(blob(b"h", struct.pack("<f", 0.1) + bytes([1]) + struct.pack("<I", 1 << 30) + bytes(8)))

    def test_a_ghost_without_car_frames_is_not_a_ghost(self):
        self.assert_refused(blob(b"h", b""))
        self.assert_refused(blob(b"h", record(0.1, update(ghost.MSG_SCORE_UPDATE, 1, bytes(5)))))


if __name__ == "__main__":
    unittest.main()
