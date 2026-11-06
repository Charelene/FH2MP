package game

import (
	"math"
	"sort"
	"testing"

	"fh2mp/internal/bits"
	"fh2mp/internal/parts"
	"fh2mp/internal/playersetup"
)

// A car with a part id the catalog does not know is repaired to stock (PartPolicy fix). From then on the class and PI that every later update of that
// object carries must be the stock car's, whatever the modified game sends: the others see it, and the event rule is checked against peer.Choices.
func TestRepairedCarKeepsStockChoices(t *testing.T) {
	r := newLoadRig(t, 1)
	w, p := r.w, r.peers[0]
	c := w.cat()
	if PartPolicy != "fix" {
		t.Skip("part policy", PartPolicy)
	}
	ids := make([]int, 0, len(c.Choices))
	for id := range c.Choices {
		ids = append(ids, id)
	}
	sort.Ints(ids)
	var car int
	var stock []uint32
	for _, id := range ids {
		if _, rental := c.Rentals[id]; rental {
			continue
		}
		s, ok := parts.StockList(c.Parts, int64(id))
		if ok && len(parts.IntegrityErrors(c.Parts, s)) == 0 && c.Choices[id].PI > 0.2 && c.Choices[id].PI < 0.6 {
			car, stock = id, s
			break
		}
	}
	if stock == nil {
		t.Skip("no stock car in the catalog")
	}
	row := c.Choices[car]
	bad := append([]uint32(nil), stock...)
	bad[12] = 987654321 // a camshaft id no database has
	if len(parts.IntegrityErrors(c.Parts, bad)) == 0 {
		t.Fatal("the test part list is not flagged by the integrity check")
	}
	pv, err := playersetup.PartsValue(bad)
	if err != nil {
		t.Fatal(err)
	}
	inflated := func(cls uint64, pi float32) map[int]bits.Int {
		return map[int]bits.Int{81: bits.FromUint64(cls), 82: bits.FromUint64(uint64(math.Float32bits(pi)))}
	}
	send := func(vals map[int]bits.Int) (bool, bool, int, bits.Int) {
		size, payload, err := playersetup.Encode(vals)
		if err != nil {
			t.Fatal(err)
		}
		viol, hide, size, payload := w.CheckParts(p, size, payload, 7)
		if !hide {
			if size >= 240 {
				payload = w.NoteChoices(p, size, payload)
			} else if size == 40 || size == 80 {
				payload = w.ShortRentalChoices(p, size, payload)
			}
			payload = w.NoteTypedChoices(p, size, payload)
		}
		return viol, hide, size, payload
	}
	checkStock := func(what string) {
		t.Helper()
		if p.Choices == nil || !p.Choices.HasClass || int(p.Choices.Class) != row.Class || math.Abs(p.Choices.PI-row.PI) > 1e-6 {
			t.Fatalf("%s: server-side choices %+v, want class %d PI %.6f", what, p.Choices, row.Class, row.PI)
		}
	}

	// the first object: modified parts with the class and PI the modified game computed
	vals := inflated(7, 1.05)
	vals[playersetup.Parts] = pv
	viol, hide, size, payload := send(vals)
	if !viol || hide {
		t.Fatalf("modified setup: violation=%v withheld=%v, want repaired and relayed", viol, hide)
	}
	if p.PartsRepairCar == nil || *p.PartsRepairCar != int64(car) {
		t.Fatalf("PartsRepairCar = %v, want %d", p.PartsRepairCar, car)
	}
	checkStock("after the repair")
	f, err := playersetup.Fields(size, payload)
	if err != nil {
		t.Fatal(err)
	}
	if got := f[81].Value.Uint64(); int(got) != row.Class {
		t.Fatalf("relayed class %d, want %d", got, row.Class)
	}
	if got := math.Float32frombits(uint32(f[82].Value.Uint64())); math.Abs(float64(got)-row.PI) > 1e-6 {
		t.Fatalf("relayed PI %v, want %v", got, row.PI)
	}

	// later updates that carry only the game's own class and PI (no parts): the full pair, then each field alone
	for _, vals := range []map[int]bits.Int{
		inflated(7, 1.05),
		{81: bits.FromUint64(7)},
		{82: bits.FromUint64(uint64(math.Float32bits(1.05)))},
	} {
		_, _, size, payload := send(vals)
		f, err := playersetup.Fields(size, payload)
		if err != nil {
			t.Fatal(err)
		}
		if v, ok := f[81]; ok && int(v.Value.Uint64()) != row.Class {
			t.Fatalf("relayed class %d after the repair, want %d", v.Value.Uint64(), row.Class)
		}
		if v, ok := f[82]; ok && math.Abs(float64(math.Float32frombits(uint32(v.Value.Uint64())))-row.PI) > 1e-6 {
			t.Fatalf("relayed PI %v after the repair, want %v", math.Float32frombits(uint32(v.Value.Uint64())), row.PI)
		}
		checkStock("after a choices update")
	}
}
