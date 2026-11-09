// Package ghost reads the ghost a client uploads for a Rivals run (Scoreboard.UploadFile) and judges it (= server/ghost.py and ghost_check.py). Nothing
// here trusts the client's bytes: every size is checked, and anything that does not walk cleanly is a *Error.
//
// Blob: u32 header section length, u32 stream section length, then both sections, each a run of compression chunks (u32 compressed size, u32 raw size,
// zlib). The header section holds the setup; the stream is the replay. Stream: records of f32 time, one packet, u32 size of that packet without its
// time. Packet: u8 header, low 3 bits = kind (0 attach, 1 keyframe, 2 update), high 5 bits = message id for updates.
package ghost

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"io"
	"math"
)

// Limits and message ids.
const (
	MaxBlob             = 0x80000 // the wire limit for an uploaded blob
	MaxServed           = 0x3E800 // the client downloads into a buffer this size; a larger ghost must not be offered
	MaxRaw              = 8 << 20 // both sections inflated
	MaxChunkRaw         = 1 << 16 // the game writes 4096-byte chunks
	MaxKeyframePkts     = 4096
	MaxNesting          = 4
	MsgCarPos           = 0
	MsgCarKeyframe      = 2
	MsgScoreUpdate      = 4
	MsgLapScore         = 5
	MsgCarCollision     = 7
	xMin, xSpan         = -16000.0, 32000.0
	yMin, ySpan         = -1000.0, 2020.0
	zMin, zSpan         = -16000.0, 32000.0
	xMaxQ, yMaxQ, zMaxQ = (1 << 22) - 1, (1 << 20) - 1, (1 << 22) - 1
)

// MsgSizes are the fixed payload sizes; any other message carries its own.
var MsgSizes = map[int]int{0: 0x15, 2: 0xca, 3: 4, 4: 5, 5: 0x1d, 6: 0x2f, 7: 0x18, 8: 0xa4, 0xb: 1, 0xc: 0x5ac, 0xd: 1, 0xe: 0x10, 0xf: 0x54, 0x10: 2,
	0x12: 0x60, 0x13: 0x28, 0x14: 0x50, 0x19: 4, 0x1a: 4, 0x1c: 0xc}

// Frame is one recorded car position and its inputs.
type Frame struct {
	T, X, Y, Z                    float64
	InputA, InputB, Steer, InputD float64
	InputE                        float64
	Gear, Flags                   int
	Rpm                           float64
}

// Keyframe is one recorded position and velocity.
type Keyframe struct{ T, X, Y, Z, VX, VY, VZ float64 }

// Ghost is a parsed ghost: Frames and Keyframes belong to the car with the most position records.
type Ghost struct {
	Header    []byte
	Frames    []Frame
	Keyframes []Keyframe
	LastT     float64
	Counts    map[int]int
	Collision []float64 // times of the CMsgCarCollision records (any car)
}

// Error is a ghost that does not walk cleanly.
type Error struct{ Msg string }

func (e *Error) Error() string { return e.Msg }

func fail(m string) error { return &Error{m} }

func le32(b []byte, o int) uint32 { return binary.LittleEndian.Uint32(b[o:]) }

// inflate decompresses data without producing more than max bytes; eof says the stream ended properly.
func inflate(data []byte, max int) (out []byte, eof bool, err error) {
	zr, err := zlib.NewReader(bytes.NewReader(data))
	if err != nil {
		return nil, false, err
	}
	buf := make([]byte, 4096)
	for len(out) < max {
		n, e := zr.Read(buf[:min(len(buf), max-len(out))])
		out = append(out, buf[:n]...)
		if e == io.EOF {
			return out, true, nil
		}
		if e == io.ErrUnexpectedEOF {
			return out, false, nil
		}
		if e != nil {
			return nil, false, e
		}
	}
	return out, false, nil
}

func chunks(section []byte) ([]byte, error) {
	var out []byte
	o, total := 0, 0
	for o < len(section) {
		if o+8 > len(section) {
			return nil, fail("truncated chunk header")
		}
		comp, raw := int(le32(section, o)), int(le32(section, o+4))
		o += 8
		if comp > len(section)-o || raw > MaxChunkRaw {
			return nil, fail("chunk size out of range")
		}
		total += raw
		if total > MaxRaw {
			return nil, fail("inflates beyond the limit")
		}
		data, eof, err := inflate(section[o:o+comp], raw+1)
		if err != nil {
			return nil, fail("bad zlib data")
		}
		if len(data) != raw || !eof {
			return nil, fail("chunk does not match its declared size")
		}
		out = append(out, data...)
		o += comp
	}
	return out, nil
}

// Sections returns the inflated (header, stream) of a stored ghost.
func Sections(blob []byte) ([]byte, []byte, error) {
	if len(blob) < 8 || len(blob) > MaxBlob {
		return nil, nil, fail("size out of range")
	}
	head, rest := int(le32(blob, 0)), int(le32(blob, 4))
	if head+rest+8 != len(blob) {
		return nil, nil, fail("section lengths do not match the blob")
	}
	h, err := chunks(blob[8 : 8+head])
	if err != nil {
		return nil, nil, err
	}
	s, err := chunks(blob[8+head:])
	if err != nil {
		return nil, nil, err
	}
	return h, s, nil
}

// packetEnd is the offset just past the packet at o (its header byte).
func packetEnd(d []byte, o, depth int) (int, error) {
	if depth > MaxNesting {
		return 0, fail("keyframes nested too deep")
	}
	if o >= len(d) {
		return 0, fail("truncated packet")
	}
	h := d[o]
	kind := int(h & 7)
	o++
	var end int
	switch kind {
	case 0:
		end = o + 7
	case 1:
		if o+4 > len(d) {
			return 0, fail("truncated keyframe")
		}
		n := int(le32(d, o))
		if n > MaxKeyframePkts {
			return 0, fail("keyframe too large")
		}
		end = o + 4
		for i := 0; i < n; i++ {
			var err error
			if end, err = packetEnd(d, end, depth+1); err != nil {
				return 0, err
			}
		}
	case 2:
		size := MsgSizes[int(h>>3)]
		o += 2
		if size == 0 {
			if o+4 > len(d) {
				return 0, fail("truncated update")
			}
			size = int(le32(d, o))
			o += 4
		}
		end = o + size
	default:
		return 0, fail("unknown packet kind")
	}
	if end > len(d) {
		return 0, fail("packet runs past the stream")
	}
	return end, nil
}

// update is (message id, object id, payload) of the update packet at o.
func update(d []byte, o int) (int, int, []byte, error) {
	msg := int(d[o] >> 3)
	start := o + 3
	if MsgSizes[msg] == 0 {
		start += 4
	}
	end, err := packetEnd(d, o, 0)
	if err != nil {
		return 0, 0, nil, err
	}
	return msg, int(binary.LittleEndian.Uint16(d[o+1:])), d[start:end], nil
}

func pos(t float64, p []byte) Frame {
	v := binary.LittleEndian.Uint64(p[:8])
	packed := int(p[17]) | int(p[18])<<8
	return Frame{T: t,
		X:      xMin + float64(v&xMaxQ)/xMaxQ*xSpan,
		Y:      yMin + float64(v>>22&yMaxQ)/yMaxQ*ySpan,
		Z:      zMin + float64(v>>42&zMaxQ)/zMaxQ*zSpan,
		InputA: float64(packed&0x7f) / 127, InputB: float64(packed>>7&0x3f) / 63,
		Steer:  float64(int8(p[16])) / 127,
		InputD: float64(p[14]) / 255, InputE: float64(p[15]&0xf) / 15, Gear: int(p[15] >> 4), Flags: packed >> 13,
		Rpm: float64(binary.LittleEndian.Uint16(p[19:])) / 65535}
}

func f32(b []byte, o int) float64 { return float64(math.Float32frombits(le32(b, o))) }

// Parse reads a stored ghost.
func Parse(blob []byte) (*Ghost, error) {
	header, s, err := Sections(blob)
	if err != nil {
		return nil, err
	}
	per := map[int][]Frame{}
	keys := map[int][]Keyframe{}
	var order []int
	counts := map[int]int{}
	var hits []float64
	o, lastT := 0, -1.0
	take := func(t float64, at int) error {
		msg, obj, p, err := update(s, at)
		if err != nil {
			return err
		}
		counts[msg]++
		switch msg {
		case MsgCarPos:
			if _, seen := per[obj]; !seen {
				order = append(order, obj)
			}
			per[obj] = append(per[obj], pos(t, p))
		case MsgCarKeyframe:
			keys[obj] = append(keys[obj], Keyframe{t, f32(p, 0), f32(p, 4), f32(p, 8), f32(p, 16), f32(p, 20), f32(p, 24)})
		case MsgCarCollision:
			hits = append(hits, t)
		}
		return nil
	}
	for o < len(s) {
		if o+5 > len(s) {
			return nil, fail("truncated record")
		}
		t := f32(s, o)
		if !(t >= 0 && t < 86400) {
			return nil, fail("record time out of range")
		}
		if t < lastT {
			return nil, fail("record times run backwards")
		}
		lastT = t
		start := o + 4
		end, err := packetEnd(s, start, 0)
		if err != nil {
			return nil, err
		}
		if end+4 > len(s) || int(le32(s, end)) != end-start-1 {
			return nil, fail("record size does not match")
		}
		switch s[start] & 7 {
		case 2:
			if err := take(t, start); err != nil {
				return nil, err
			}
		case 1:
			at := start + 5
			for i, n := 0, int(le32(s, start+1)); i < n; i++ {
				if s[at]&7 == 2 {
					if err := take(t, at); err != nil {
						return nil, err
					}
				}
				if at, err = packetEnd(s, at, 0); err != nil {
					return nil, err
				}
			}
		}
		o = end + 4
	}
	if len(per) == 0 {
		return nil, fail("no car frames")
	}
	car := order[0]
	for _, k := range order {
		if len(per[k]) > len(per[car]) {
			car = k
		}
	}
	return &Ghost{header, per[car], keys[car], lastT, counts, hits}, nil
}
