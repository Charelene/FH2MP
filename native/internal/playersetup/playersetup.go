// Package playersetup is the LobbyPlayer transfer codec (= server/player_setup.py): [8-bit variable id][value] pairs in ascending order.
package playersetup

import (
	"math"
	"math/bits"
	"sort"

	b "fh2mp/internal/bits"
)

// Variable ids.
const (
	IDBits     = 8
	Model      = 103
	WheelStyle = 106
	Parts      = 107
	UseStock   = 144
)

// TuneIDs are variables 108..143.
var TuneIDs = func() []int {
	out := make([]int, 0, 36)
	for i := 108; i < 144; i++ {
		out = append(out, i)
	}
	return out
}()

var widths = []int{
	64, 1, 1, -16, 64, 1, 32, 1, 32, 32, 32, 32, 1, 1, 1, 1, 1, 1,
	1, 4, 3, 16, 16, 32, 32, 32, 32, 32, 32, 1, 7, 8, 1, 1, 1, 1,
	1, 32, 2, -4, 5, 64, 1, 1, 64, 1, 1, 64, 1, 1, 64, 1, 1, 64,
	1, 1, 64, 1, 1, 64, 1, 1, 64, 1, 1, 64, 1, 1, 64, 1, 1, 64,
	1, 1, 64, 1, 1, 3, 17, 16, 16, 32, 32, 32, 32, 32, 32, 32, 32, 1,
	1, 3, 32, 64, 64, 2, 2, 2, 1, 1, 2, 1, 3, 32, 32, 32, 32, 1376,
	32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 288, 32, 32, 32, 32,
	32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
	1, 32, 32, 32, 32, 1, 1, 1, 1, 1, 2, 2, 2, 2, 32, 32, 32, 32,
}

// Field is a decoded variable: where its value starts, its width and the raw bits.
type Field struct {
	Offset, Width int
	Value         b.Int
}

// ErrInvalid is any ValueError of the Python codec.
type ErrInvalid struct{ Msg string }

func (e *ErrInvalid) Error() string { return e.Msg }

func bad(m string) error { return &ErrInvalid{m} }

// Fields returns id -> field and rejects malformed deltas.
func Fields(size int, payload b.Int) (map[int]Field, error) {
	if size < 0 || size >= 1<<14 || payload.BitLen() > size {
		return nil, bad("invalid player object size")
	}
	pos, previous := 0, -1
	out := map[int]Field{}
	for pos < size {
		if size-pos < IDBits {
			return nil, bad("truncated player variable id")
		}
		vid := int(payload.Field(pos, 8))
		pos += IDBits
		if vid <= previous || vid >= len(widths) {
			return nil, bad("unknown or unordered player variable")
		}
		width := widths[vid]
		if width < 0 {
			prefix := bits.Len(uint(-width))
			if pos+prefix > size {
				return nil, bad("truncated player string length")
			}
			count := int(payload.Field(pos, prefix))
			if count > -width {
				return nil, bad("oversize player string")
			}
			width = prefix + 16*count
		}
		if pos+width > size {
			return nil, bad("truncated player variable value")
		}
		out[vid] = Field{pos, width, payload.Rsh(pos).Low(width)}
		previous, pos = vid, pos+width
	}
	return out, nil
}

// Encode encodes id -> raw value with the schema widths; strings include their prefix.
func Encode(values map[int]b.Int) (int, b.Int, error) {
	ids := make([]int, 0, len(values))
	for id := range values {
		ids = append(ids, id)
	}
	sort.Ints(ids)
	size := 0
	var payload b.Int
	for _, vid := range ids {
		value := values[vid]
		if vid < 0 || vid >= len(widths) {
			return 0, nil, bad("invalid player variable")
		}
		width := widths[vid]
		if width < 0 {
			prefix := bits.Len(uint(-width))
			count := int(value.Low(prefix).Uint64())
			if count > -width {
				return 0, nil, bad("oversize player string")
			}
			width = prefix + 16*count
		}
		if value.BitLen() > width {
			return 0, nil, bad("player value does not fit")
		}
		entry := b.FromUint64(uint64(vid)).Or(value.Lsh(IDBits))
		payload = payload.Or(entry.Lsh(size))
		size += IDBits + width
	}
	if size >= 1<<14 {
		return 0, nil, bad("player object too large")
	}
	return size, payload, nil
}

// PartWords splits the 43-word installed-parts value.
func PartWords(value b.Int) []uint32 {
	out := make([]uint32, 43)
	for k := range out {
		out[k] = uint32(value.Field(32*k, 32))
	}
	return out
}

// PartsValue joins 43 words back into one value.
func PartsValue(words []uint32) (b.Int, error) {
	if len(words) != 43 {
		return nil, bad("invalid installed parts array")
	}
	var v b.Int
	for k, w := range words {
		v = v.Or(b.FromUint64(uint64(w)).Lsh(32 * k))
	}
	return v, nil
}

// StockCorrection is stock_correction: a complete stock setup delta. choices (class, PI) may be nil.
func StockCorrection(words []uint32, choices *struct {
	Class uint64
	PI    float64
}) (map[int]b.Int, error) {
	pv, err := PartsValue(words)
	if err != nil {
		return nil, err
	}
	values := map[int]b.Int{Parts: pv, UseStock: b.FromUint64(1), WheelStyle: nil, 94: b.FromUint64(^uint64(0))}
	for _, id := range TuneIDs {
		values[id] = b.FromUint64(0xBF800000)
	}
	values[121] = nil
	if choices != nil {
		values[81] = b.FromUint64(choices.Class)
		values[82] = b.FromUint64(uint64(math.Float32bits(float32(choices.PI))))
	}
	return values, nil
}
