// Package gameopts reads and edits the lobby's GameOptions transfer object (= server/gameopts.py). Values are [variable id: 10 bits][value]
// pairs in ascending id order; writes are deltas.
package gameopts

import (
	"errors"
	"math"
	"sort"

	"fh2mp/internal/bits"
)

// Variable ids and limits.
const (
	NumTeamsID     = 428
	TeamScoringID  = 429
	IDBits         = 10
	NumLapsID      = 419
	EndConditionID = 418
	MaxLaps        = 10
	MaxTimeID      = 420
	MaxTimeLimit   = 86400.0
)

var bucketWidths = []int{
	-24, 32, 1, 8, 8, 32, 32, 32, 8, 16, 16, 32, 32, 32, 32, 32, 32,
	2, 8, 2, 8, 2, 8, 2, 8, 2, 16, 2, 16, 2, 16, 2, 8, 2, 8, 2, 8,
	2, 8, 2, 8, 2, 8, 1, 32, 800, 800,
}

var lapPrefixWidths = func() []int {
	w := []int{8, 8, 3, 4, 32, 32, 8, 32, 32, 8, 32, 32, 4, 4, 8, 3, 2, 3, 1, 3, 8, 8}
	for i := 0; i < 8; i++ {
		w = append(w, bucketWidths...)
	}
	return append(w, 32, 32, 2, 3, 1, 1, 1, 1, 8, 8, 32, 32, 32, 1, 8, 8, 8, 32, 32, 32, 3, 32)
}()

var optionWidths = append(append([]int{}, lapPrefixWidths...), []int{
	32, 8, 32, 32, 4, 1, 1, 1, 2, 3, 8, 1, 1, 1, 1, 8, 96, 1, 1,
	32, 32, 8, 8, 8, 8, 8, 1, 1, 1, 1, 1, 2, 2, 2, 3, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 3, 32, 16, 16, 32, 32, 32, 32, 1, 1, 1,
	1, 32, 4, 1, 1, 32, 32, 32, 32, 1, 1, 4, 1, 1, 1, 1, 1, 1,
}...)

// Options is the server's (size, payload) of a lobby's current options.
type Options struct {
	Size    int
	Payload bits.Int
}

// Field is one decoded variable: its native width and bits.
type Field struct {
	Width int
	Value bits.Int
}

// ErrInvalid is any ValueError of the Python codec.
type ErrInvalid struct{ Msg string }

func (e *ErrInvalid) Error() string { return e.Msg }

func bad(msg string) error { return &ErrInvalid{msg} }

func field(p bits.Int, off, width int) bits.Int { return p.Rsh(off).Low(width) }

// Fields decodes a delta into id -> (width, bits). Unknown or unordered ids are refused.
func Fields(size int, payload bits.Int) (map[int]Field, error) {
	if size < 0 || size >= 1<<14 {
		return nil, bad("invalid GameOptions size")
	}
	pos, previous := 0, -1
	out := map[int]Field{}
	for pos < size {
		if pos+IDBits > size {
			return nil, bad("truncated GameOptions id")
		}
		vid := int(payload.Field(pos, IDBits))
		pos += IDBits
		if vid <= previous || vid >= len(optionWidths) {
			return nil, bad("unknown or unordered GameOptions variable")
		}
		width := optionWidths[vid]
		if width < 0 {
			count := int(payload.Field(pos, 5))
			if count > -width || pos+5 > size {
				return nil, bad("invalid GameOptions string")
			}
			width = 5 + 16*count
		}
		if pos+width > size {
			return nil, bad("truncated GameOptions value")
		}
		out[vid] = Field{width, field(payload, pos, width)}
		pos += width
		previous = vid
	}
	return out, nil
}

// EncodeFields is encode_fields.
func EncodeFields(values map[int]Field) (int, bits.Int, error) {
	ids := make([]int, 0, len(values))
	for id := range values {
		ids = append(ids, id)
	}
	sort.Ints(ids)
	size := 0
	var payload bits.Int
	for _, id := range ids {
		f := values[id]
		entry := bits.FromUint64(uint64(id)).Or(f.Value.Lsh(IDBits))
		payload = payload.Or(entry.Lsh(size))
		size += IDBits + f.Width
	}
	if size >= 1<<14 {
		return 0, nil, bad("GameOptions snapshot too large")
	}
	return size, payload, nil
}

// Merge accumulates a transfer-object delta on the previous options, preserving unchanged race restrictions.
func Merge(previous *Options, size int, payload bits.Int) (int, bits.Int, error) {
	values := map[int]Field{}
	if previous != nil {
		v, err := Fields(previous.Size, previous.Payload)
		if err != nil {
			return 0, nil, err
		}
		values = v
	}
	delta, err := Fields(size, payload)
	if err != nil {
		return 0, nil, err
	}
	for k, v := range delta {
		values[k] = v
	}
	return EncodeFields(values)
}

// Rule is the primary race bucket: CarClassId (255 = unrestricted), minimum and maximum PI.
type Rule struct{ Class, Min, Max uint64 }

// CarRule is car_rule.
func CarRule(options *Options) (Rule, error) {
	values := map[int]Field{}
	if options != nil {
		v, err := Fields(options.Size, options.Payload)
		if err != nil {
			return Rule{}, err
		}
		values = v
	}
	get := func(id int, def uint64) uint64 {
		if f, ok := values[id]; ok {
			return f.Value.Uint64()
		}
		return def
	}
	return Rule{get(30, 255), get(31, 0), get(32, 0)}, nil
}

// Choices is the (class, PI) a player's game last sent; HaveClass false = class unknown.
type Choices struct {
	HaveClass bool
	Class     uint64
	PI        float64
}

// AcceptsCar is accepts_car; choices nil = the game never sent them.
func AcceptsCar(rule Rule, choices *Choices) bool {
	if rule.Class == 255 && rule.Min == 0 && rule.Max == 0 {
		return true
	}
	if choices == nil {
		return false
	}
	if !choices.HaveClass || math.IsNaN(choices.PI) || math.IsInf(choices.PI, 0) {
		return false
	}
	// Python: int(pi * 1000 + 0.5) truncates toward zero.
	display := int64(choices.PI*1000 + 0.5)
	if !(rule.Class == 255 || choices.Class == rule.Class) {
		return false
	}
	if rule.Min != 0 && display < int64(rule.Min) {
		return false
	}
	if rule.Max != 0 && display > int64(rule.Max) {
		return false
	}
	return true
}

type lapInfo struct {
	offset     int
	value      *uint64
	endOffset  int
	hasEndOff  bool
	endType    *uint64
	hasEndType bool
}

func lapFields(size int, payload bits.Int) (lapInfo, error) {
	pos, previous := 0, -1
	var li lapInfo
	for pos < size {
		start := pos
		if pos+IDBits > size {
			return li, bad("truncated GameOptions variable id")
		}
		vid := int(payload.Field(pos, IDBits))
		pos += IDBits
		if vid <= previous || vid >= 670 {
			return li, bad("invalid GameOptions variable order")
		}
		if vid > EndConditionID && !li.hasEndOff {
			li.endOffset, li.hasEndOff = start, true
		}
		if vid > NumLapsID {
			li.offset = start
			return li, nil
		}
		width := lapPrefixWidths[vid]
		if width < 0 {
			if pos+5 > size {
				return li, bad("truncated GameOptions bucket name")
			}
			count := int(payload.Field(pos, 5))
			if count > -width {
				return li, bad("oversize GameOptions bucket name")
			}
			width = 5 + 16*count
		}
		if pos+width > size {
			return li, bad("truncated GameOptions variable")
		}
		if vid == EndConditionID {
			t := payload.Field(pos, 3)
			li.endOffset, li.hasEndOff, li.endType = start, true, &t
		}
		if vid == NumLapsID {
			v := payload.Field(pos, 32)
			li.offset, li.value = start, &v
			return li, nil
		}
		pos += width
		previous = vid
	}
	li.offset = pos
	if !li.hasEndOff {
		li.endOffset, li.hasEndOff = pos, true
	}
	return li, nil
}

// MaxTime is max_time: the event time limit in seconds the leader set, if usable.
func MaxTime(size int, payload bits.Int) (float64, bool, error) {
	f, err := Fields(size, payload)
	if err != nil {
		return 0, false, err
	}
	e, ok := f[MaxTimeID]
	if !ok || e.Width != 32 {
		return 0, false, nil
	}
	sec := float64(math.Float32frombits(uint32(e.Value.Uint64())))
	if sec > 0.0 && sec <= MaxTimeLimit {
		return sec, true, nil
	}
	return 0, false, nil
}

// LapField is lap_field: (entry bit offset, value or nil when omitted).
func LapField(size int, payload bits.Int) (int, *uint64, error) {
	li, err := lapFields(size, payload)
	return li.offset, li.value, err
}

// EndConditionType is end_condition_type (nil = not present).
func EndConditionType(size int, payload bits.Int) (*uint64, error) {
	li, err := lapFields(size, payload)
	return li.endType, err
}

func writeField(size int, payload bits.Int, offset, vid, width int, value uint64, present bool) (int, bits.Int, error) {
	if !present {
		if size+IDBits+width >= 1<<14 {
			return 0, nil, bad("GameOptions too large to insert lap settings")
		}
		low := payload.Low(offset)
		entry := bits.FromUint64(uint64(vid)).Or(bits.FromUint64(value).Lsh(IDBits))
		payload = low.Or(entry.Lsh(offset)).Or(payload.Rsh(offset).Lsh(offset + IDBits + width))
		return size + IDBits + width, payload, nil
	}
	shift := offset + IDBits
	mask := bits.FromUint64(1).Lsh(width)
	mask = subOne(mask).Lsh(shift)
	return size, andNot(payload, mask).Or(bits.FromUint64(value).Lsh(shift)), nil
}

func subOne(x bits.Int) bits.Int {
	out := make([]uint64, len(x))
	copy(out, x)
	for i := range out {
		if out[i] != 0 {
			out[i]--
			break
		}
		out[i] = ^uint64(0)
	}
	n := len(out)
	for n > 0 && out[n-1] == 0 {
		n--
	}
	return bits.Int(out[:n])
}

func andNot(x, mask bits.Int) bits.Int {
	out := make([]uint64, len(x))
	copy(out, x)
	for i := range out {
		if i < len(mask) {
			out[i] &^= mask[i]
		}
	}
	n := len(out)
	for n > 0 && out[n-1] == 0 {
		n--
	}
	return bits.Int(out[:n])
}

// BoundedLaps is bounded_laps: explicitly writes a lap count in 1..10 and an end condition. fixed nil = none.
func BoundedLaps(size int, payload bits.Int, def int64, fixed *int64, defaultEnd int64) (int, bits.Int, int64, error) {
	li, err := lapFields(size, payload)
	if err != nil {
		return 0, nil, 0, err
	}
	var selected int64
	switch {
	case fixed != nil:
		selected = *fixed
	case li.value == nil:
		selected = def
	default:
		selected = int64(*li.value)
	}
	laps := selected
	if laps < 1 {
		laps = 1
	}
	if laps > MaxLaps {
		laps = MaxLaps
	}
	condition := defaultEnd
	if li.endType != nil {
		condition = int64(*li.endType)
	}
	if condition == 0 || fixed != nil {
		condition = 1
	}
	if condition < 1 || condition > 6 {
		return 0, nil, 0, bad("invalid GameOptions end condition")
	}
	size, payload, err = writeField(size, payload, li.offset, NumLapsID, 32, uint64(laps), li.value != nil)
	if err != nil {
		return 0, nil, 0, err
	}
	size, payload, err = writeField(size, payload, li.endOffset, EndConditionID, 3, uint64(condition), li.endType != nil)
	if err != nil {
		return 0, nil, 0, err
	}
	return size, payload, laps, nil
}

// NumTeams is num_teams: NumTeams the leader chose, or ok=false when the options do not carry it.
func NumTeams(size int, payload bits.Int) (uint64, bool) {
	for o := 0; o <= size-(2*IDBits+2); o++ {
		if payload.Field(o, IDBits) == NumTeamsID && payload.Field(o+IDBits+2, IDBits) == TeamScoringID {
			return payload.Field(o+IDBits, 2), true
		}
	}
	return 0, false
}

// TeamsOn is teams_on.
func TeamsOn(o *Options) bool {
	if o == nil {
		return false
	}
	n, ok := NumTeams(o.Size, o.Payload)
	return ok && n == 2
}

var _ = errors.New
