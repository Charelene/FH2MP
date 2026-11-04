// Package parts reads the car's installed part ids out of a LobbyPlayer object and checks them against the catalog (= server/parts.py).
package parts

import (
	"fmt"
	"math"
	"sort"

	"fh2mp/internal/bits"
	"fh2mp/internal/catalog"
	"fh2mp/internal/pyfmt"
)

// None is the "no part" word.
const (
	None    = catalog.PartNone
	Slots   = catalog.PartSlots
	MinBits = 32 * Slots
)

// Problem is why a word cannot be in a slot ("" when it can).
func Problem(t *catalog.Tables, slot int, word uint32) string {
	if word == None {
		if slot == 8 {
			return "missing tire compound"
		}
		return ""
	}
	table, ok := t.SlotTable[slot]
	if !ok {
		return "this slot has no part type"
	}
	if _, in := t.TableIDs[table][word]; in {
		return ""
	}
	if _, in := t.AllIDs[word]; in {
		return fmt.Sprintf("not in %s (an id of another part table)", table)
	}
	return fmt.Sprintf("not in %s (no such id)", table)
}

// MustReplace: an id no client can build the car with.
func MustReplace(t *catalog.Tables, slot int, word uint32) bool {
	text := Problem(t, slot, word)
	_, hasTable := t.SlotTable[slot]
	return text != "" && hasTable && slot != 2 && (contains(text, "no such id") || slot == 8 || slot == 12)
}

func contains(s, sub string) bool {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return true
		}
	}
	return false
}

// expectedOwner: who a row in `table` must belong to for this car (ok=false when it cannot be told).
func expectedOwner(t *catalog.Tables, table string, words []uint32) (int64, bool) {
	kind := t.OwnerKind[table]
	body := words[2]
	switch kind {
	case "car":
		if _, ok := t.TableIDs["CarBody"][body]; ok {
			return int64(body) / 1000, true
		}
	case "engine":
		v, ok := t.Installs["Engine"][words[0]]
		return v, ok
	case "drivetrain":
		v, ok := t.Installs["Drivetrain"][words[1]]
		return v, ok
	}
	return 0, false
}

// Change is one replaced word.
type Change struct {
	Slot     int
	Old, New uint32
	Why      string
}

// Odd is a reported-only violation.
type Odd struct {
	Slot int
	Word uint32
	Text string
}

// Neutralize returns a copy of the part list that every client can build, and what was changed.
func Neutralize(t *catalog.Tables, words []uint32) ([]uint32, []Change, []Odd) {
	out := append([]uint32(nil), words...)
	var changes []Change
	var odd []Odd
	if _, ok := t.TableIDs["CarBody"][words[2]]; !ok {
		return out, changes, odd
	}
	order := make([]int, len(out))
	for i := range order {
		order[i] = i
	}
	sort.SliceStable(order, func(i, j int) bool {
		a, b := order[i], order[j]
		ka, kb := a > 2, b > 2
		if ka != kb {
			return !ka
		}
		return a < b
	})
	for _, k := range order {
		w := out[k]
		table, hasTable := t.SlotTable[k]
		if !hasTable && w != None {
			out[k] = None
			changes = append(changes, Change{k, w, None, "slot has no part type"})
			continue
		}
		if MustReplace(t, k, w) {
			nw := uint32(None)
			if owner, ok := expectedOwner(t, table, out); ok {
				if s, ok := t.Stock[table][owner]; ok {
					nw = s
				}
			}
			out[k] = nw
			why := "an id of another part table"
			if _, in := t.AllIDs[w]; !in {
				why = "no such id"
			}
			if k == 8 && w == None {
				why = "missing tire compound"
			}
			changes = append(changes, Change{k, w, nw, why})
			continue
		}
		if text := Problem(t, k, w); text != "" {
			odd = append(odd, Odd{k, w, text})
			continue
		}
		if w == None || !hasTable {
			continue
		}
		owner, okOwner := expectedOwner(t, table, out)
		have, okHave := t.RowOwner[table][w]
		if okOwner && okHave && have != owner {
			if t.OwnerKind[table] == "engine" {
				nw := uint32(None)
				if s, ok := t.Stock[table][owner]; ok {
					nw = s
				}
				out[k] = nw
				changes = append(changes, Change{k, w, nw, fmt.Sprintf("%s belongs to engine %d, installed engine is %d", table, have, owner)})
				continue
			}
			odd = append(odd, Odd{k, w, fmt.Sprintf("%s row of %s %d, this car's is %d", table, t.OwnerKind[table], have, owner)})
		}
	}
	return out, changes, odd
}

// StockList is the part list of `car` as it comes from the dealer; ok=false when the catalog has no body row for it.
func StockList(t *catalog.Tables, car int64) ([]uint32, bool) {
	words := make([]uint32, Slots)
	for i := range words {
		words[i] = None
	}
	for k := 0; k <= 2; k++ {
		if s, ok := t.Stock[t.SlotTable[k]][car]; ok {
			words[k] = s
		}
	}
	if words[2] == None {
		return nil, false
	}
	slots := make([]int, 0, len(t.SlotTable))
	for k := range t.SlotTable {
		slots = append(slots, k)
	}
	sort.Ints(slots) // dict order of SLOT_TABLE is ascending in the generated file
	for _, k := range slots {
		if k > 2 {
			tbl := t.SlotTable[k]
			if owner, ok := expectedOwner(t, tbl, words); ok {
				if s, ok := t.Stock[tbl][owner]; ok {
					words[k] = s
				} else {
					words[k] = None
				}
			} else {
				words[k] = None
			}
		}
	}
	return words, true
}

// IntegrityError is one (slot, word, text) of IntegrityErrors; Slot -1 = "invalid parts count".
type IntegrityError struct {
	Slot int
	Word uint32
	Text string
}

// IntegrityErrors checks decoded values, without using their validity to locate the field.
func IntegrityErrors(t *catalog.Tables, words []uint32) []IntegrityError {
	if len(words) != Slots {
		return []IntegrityError{{-1, 0, "invalid parts count"}}
	}
	var errs []IntegrityError
	car, haveCar := int64(0), false
	if _, ok := t.TableIDs["CarBody"][words[2]]; ok {
		car, haveCar = int64(words[2])/1000, true
	}
	motor, haveMotor := t.Installs["Motor"][words[0]]
	electric := false
	if haveMotor && haveCar {
		if o, ok := t.RowOwner["Motor"][words[0]]; ok && o == car {
			electric = true
		}
	}
	for slot, word := range words {
		if slot == 0 && electric {
			continue
		}
		text := Problem(t, slot, word)
		table, hasTable := t.SlotTable[slot]
		if word == None && (slot == 0 || slot == 1 || slot == 2 || slot == 12) && !electric && haveCar {
			if stock, ok := StockList(t, car); ok && stock[slot] != None {
				text = "missing required part"
			}
		}
		if text == "" && word != None && hasTable {
			var expected int64
			var haveExp bool
			if t.OwnerKind[table] == "motor" {
				expected, haveExp = motor, haveMotor
			} else {
				expected, haveExp = expectedOwner(t, table, words)
			}
			actual, haveAct := t.RowOwner[table][word]
			if haveExp && haveAct && expected != actual {
				text = fmt.Sprintf("%s belongs to %d, expected %d", table, actual, expected)
			}
		}
		if text != "" {
			errs = append(errs, IntegrityError{slot, word, text})
		}
	}
	return errs
}

// PlayerChoices variable ids.
var choiceVars = [6]uint64{82, 83, 84, 85, 86, 87}

const (
	ChoiceClassVar = 81
	PIMin, PIMax   = 0.0, 1.5
	RatingMin      = 0.0
	RatingMax      = 12.0
)

// Choices locates the class and PI in a payload: PIAt and ClassAt are bit offsets of the values (ClassAt -1 = not carried; PIAt -1 = only class).
type Choices struct{ PIAt, ClassAt int }

// FindChoices is find_choices.
func FindChoices(size int, payload bits.Int) (Choices, bool) {
	for o := 0; o <= size-6*40; o++ {
		match := true
		for k, v := range choiceVars {
			if payload.Field(o+40*k, 8) != v {
				match = false
				break
			}
		}
		if match {
			cls := -1
			if o >= 40 && payload.Field(o-40, 8) == ChoiceClassVar {
				cls = o - 40 + 8
			}
			return Choices{o + 8, cls}, true
		}
	}
	return Choices{}, false
}

// FindShortChoices is find_short_choices: a short [81 class][82 PI] update from bit 0.
func FindShortChoices(size int, payload bits.Int) (Choices, bool) {
	if size != 40 && size != 80 {
		return Choices{}, false
	}
	ids := make([]uint64, size/40)
	for k := range ids {
		ids[k] = payload.Field(40*k, 8)
	}
	ok := false
	switch len(ids) {
	case 2:
		ok = ids[0] == ChoiceClassVar && ids[1] == 82
	case 1:
		ok = ids[0] == ChoiceClassVar || ids[0] == 82
	}
	if !ok {
		return Choices{}, false
	}
	c := Choices{-1, -1}
	if ids[0] == ChoiceClassVar {
		c.ClassAt = 8
	}
	for i, id := range ids {
		if id == 82 {
			c.PIAt = 8 + 40*i
			break
		}
	}
	return c, true
}

func f32of(payload bits.Int, at int) float64 {
	return float64(math.Float32frombits(uint32(payload.Field(at, 32))))
}

func replace32(payload bits.Int, at int, v uint32) bits.Int {
	mask := bits.FromUint64(0xFFFFFFFF).Lsh(at)
	return andNot(payload, mask).Or(bits.FromUint64(uint64(v)).Lsh(at))
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

// ReadShortChoices is read_short_choices: (class or nil, PI or nil).
func ReadShortChoices(payload bits.Int, c Choices) (cls *uint64, pi *float64) {
	if c.PIAt >= 0 {
		v := f32of(payload, c.PIAt)
		pi = &v
	}
	if c.ClassAt >= 0 {
		v := payload.Field(c.ClassAt, 32)
		cls = &v
	}
	return
}

// WriteShortChoices is write_short_choices.
func WriteShortChoices(payload bits.Int, c Choices, cls uint32, pi float32) bits.Int {
	if c.PIAt >= 0 {
		payload = replace32(payload, c.PIAt, math.Float32bits(pi))
	}
	if c.ClassAt >= 0 {
		payload = replace32(payload, c.ClassAt, cls)
	}
	return payload
}

// ReadChoices is read_choices: (class or nil as signed 32-bit, PI).
func ReadChoices(payload bits.Int, c Choices) (cls *int64, pi float64) {
	pi = f32of(payload, c.PIAt)
	if c.ClassAt >= 0 {
		v := int64(int32(uint32(payload.Field(c.ClassAt, 32))))
		cls = &v
	}
	return
}

// WriteChoices is write_choices: the payload with the class (when carried) and the PI replaced.
func WriteChoices(payload bits.Int, c Choices, cls uint32, pi float32) bits.Int {
	payload = replace32(payload, c.PIAt, math.Float32bits(pi))
	if c.ClassAt >= 0 {
		payload = replace32(payload, c.ClassAt, cls)
	}
	return payload
}

// SanitizeChoices replaces a NaN or absurd PerfIndex / rating by the nearest sane value; notes say what changed.
func SanitizeChoices(payload bits.Int, c Choices) (bits.Int, []string) {
	var notes []string
	type spec struct {
		name   string
		lo, hi float64
	}
	specs := []spec{{"PerfIndex", PIMin, PIMax}}
	for i := 1; i <= 5; i++ {
		specs = append(specs, spec{fmt.Sprintf("rating %d", i), RatingMin, RatingMax})
	}
	for k, s := range specs {
		at := c.PIAt + 40*k
		v := f32of(payload, at)
		if !math.IsNaN(v) && !math.IsInf(v, 0) && s.lo <= v && v <= s.hi {
			continue
		}
		var fixed float64
		if math.IsNaN(v) || math.IsInf(v, 0) {
			fixed = (s.lo + s.hi) / 2
		} else {
			fixed = math.Min(math.Max(v, s.lo), s.hi)
		}
		notes = append(notes, fmt.Sprintf("%s %s -> %s", s.name, pyfmt.ReprFloat(v), pyfmt.FmtG(fixed)))
		payload = replace32(payload, at, math.Float32bits(float32(fixed)))
	}
	return payload, notes
}

// ChoicesUpdate is a server-written PlayerChoices update [81 class][82 PI]: (bits, payload).
func ChoicesUpdate(cls uint32, pi float32) (int, bits.Int) {
	v := bits.FromUint64(ChoiceClassVar).
		Or(bits.FromUint64(uint64(cls)).Lsh(8)).
		Or(bits.FromUint64(82).Lsh(40)).
		Or(bits.FromUint64(uint64(math.Float32bits(pi))).Lsh(48))
	return 80, v
}

// WriteParts replaces the 43 words at offset.
func WriteParts(payload bits.Int, offset int, words []uint32) bits.Int {
	for k, w := range words {
		payload = replace32(payload, offset+32*k, w)
	}
	return payload
}
