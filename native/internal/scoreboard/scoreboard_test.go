package scoreboard

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"reflect"
	"strconv"
	"testing"

	"fh2mp/internal/cardb"
	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
	"fh2mp/internal/ghost"
)

type fixture struct {
	Hash     [][]json.RawMessage  `json:"hash"`
	FakeXuid [][2]json.RawMessage `json:"fake_xuid"`
	Bits     [][2]json.RawMessage `json:"bits"`
	Iso      [][2]json.RawMessage `json:"iso"`
	Steps    []struct {
		S string          `json:"s"`
		R json.RawMessage `json:"r"`
	} `json:"steps"`
}

func load(t *testing.T) *fixture {
	raw, err := os.ReadFile("../../testdata/scoreboard.json")
	if err != nil {
		t.Skip(err)
	}
	var f fixture
	if err := json.Unmarshal(raw, &f); err != nil {
		t.Fatal(err)
	}
	return &f
}

func TestHashAndBits(t *testing.T) {
	f := load(t)
	for _, h := range f.Hash {
		var parts []any
		if err := json.Unmarshal(h[0], &parts); err != nil {
			t.Fatal(err)
		}
		for i, p := range parts {
			if x, ok := p.(float64); ok {
				parts[i] = int64(x)
			}
		}
		var wantI int64
		var wantF float64
		json.Unmarshal(h[1], &wantI)
		json.Unmarshal(h[2], &wantF)
		if got := hashInt(parts...); got != wantI {
			t.Errorf("hashInt(%v) = %d, python %d", parts, got, wantI)
		}
		if got := hashFrac(parts...); got != wantF {
			t.Errorf("hashFrac(%v) = %v, python %v", parts, got, wantF)
		}
	}
	for _, c := range f.FakeXuid {
		var name string
		var want int64
		json.Unmarshal(c[0], &name)
		json.Unmarshal(c[1], &want)
		if got := FakeXuid(name); got != want {
			t.Errorf("FakeXuid(%q) = %d, python %d", name, got, want)
		}
	}
	for _, c := range f.Bits {
		var x float64
		json.Unmarshal(c[0], &x)
		want, _ := strconv.ParseUint(string(c[1]), 10, 64)
		if got := DblToU64(x); got != want {
			t.Errorf("DblToU64(%v) = %d, python %d", x, got, want)
		}
		if U64ToDbl(want) != x {
			t.Errorf("U64ToDbl(%d) != %v", want, x)
		}
	}
	for _, c := range f.Iso {
		var x float64
		var want string
		json.Unmarshal(c[0], &x)
		json.Unmarshal(c[1], &want)
		if got := Iso(x); got != want {
			t.Errorf("Iso(%v) = %q, python %q", x, got, want)
		}
	}
}

// ---- the scenario

type run struct {
	t *testing.T
	f *fixture
	n int
}

func (r *run) check(name string, got any) {
	r.t.Helper()
	if r.n >= len(r.f.Steps) {
		r.t.Fatalf("step %s: no more steps in the fixture", name)
	}
	want := r.f.Steps[r.n]
	r.n++
	if want.S != name {
		r.t.Fatalf("step %d is %s in Python and %s in Go", r.n, want.S, name)
	}
	b, err := json.Marshal(got)
	if err != nil {
		r.t.Fatalf("step %s: %v", name, err)
	}
	var g, w any
	json.Unmarshal(b, &g)
	json.Unmarshal(want.R, &w)
	if !reflect.DeepEqual(g, w) {
		r.t.Fatalf("step %s differs\n got %s\nwant %s", name, b, want.R)
	}
}

func rowMap(r *Row) any {
	if r == nil {
		return nil
	}
	m := map[string]any{"xuid": r.Xuid, "tag": r.Tag, "sb": r.Sb, "st": r.St, "track": r.Track, "car": r.Car, "cls": r.Cls, "cpi": r.Cpi, "score": r.Score,
		"clean": r.Clean, "stm": r.Stm, "tcs": r.Tcs, "abs": r.Abs, "autoshift": r.Autoshift, "t": r.T}
	if r.Fake {
		m["fake"] = true
		if r.Xuid == TargetXuid {
			delete(m, "autoshift") // the world-record row of Python has no such key; the zero value says the same
		}
	} else {
		m["submissionId"], m["reports"], m["suspicious"], m["flaggedBy"] = r.SubmissionID, r.Reports, r.Suspicious, r.FlaggedBy
		m["gsid"], m["autobrake"], m["clutch"], m["ghost"] = r.Gsid, r.Autobrake, r.Clutch, r.Ghost
	}
	if r.Position > 0 {
		m["position"] = r.Position
	}
	return m
}

func rowsMap(rs []*Row) any {
	out := []any{}
	for _, r := range rs {
		out = append(out, rowMap(r))
	}
	return out
}

func storedMap(s *Stored) any {
	if s == nil {
		return nil
	}
	return map[string]any{"id": s.ID, "score_id": hex.EncodeToString(s.ScoreID), "xuid": s.Xuid, "tag": s.Tag, "sb": s.Sb, "st": s.St, "score": s.Score}
}

func round4(x float64) float64 {
	v, _ := strconv.ParseFloat(strconv.FormatFloat(x, 'f', 4, 64), 64)
	return v
}

func i64(v int64) *int64 { return &v }

type mk struct {
	sb, gsid, track, car, cls int64
	cpi                       float64
	clean                     bool
	autobrake                 bool
}

func sub(sb int64, score float64, o ...func(*mk)) *Submission {
	m := &mk{sb: sb, track: 1001, car: 1010, cls: 3, cpi: 0.7, clean: true}
	for _, f := range o {
		f(m)
	}
	return &Submission{Score: DblToU64(score), ScoreboardType: m.sb, GameScoreboardID: m.gsid, TrackID: m.track, CarID: m.car, CarClassID: m.cls, CarPerformanceIndex: m.cpi,
		IsClean: m.clean, UsedTCS: true, UsedAutoBrake: m.autobrake}
}

func gsid(v int64) func(*mk)  { return func(m *mk) { m.gsid = v } }
func track(v int64) func(*mk) { return func(m *mk) { m.track = v } }
func car(v int64) func(*mk)   { return func(m *mk) { m.car = v } }
func cls(v int64) func(*mk)   { return func(m *mk) { m.cls = v } }
func cpi(v float64) func(*mk) { return func(m *mk) { m.cpi = v } }
func unclean() func(*mk)      { return func(m *mk) { m.clean = false } }
func autobrake() func(*mk)    { return func(m *mk) { m.autobrake = true } }

// ghost builder, as in the Python script

func chunks(raw []byte) []byte {
	var out []byte
	for i := 0; i < len(raw); i += 4096 {
		part := raw[i:min(i+4096, len(raw))]
		z := zlibCompress(part)
		var h [8]byte
		binary.LittleEndian.PutUint32(h[:], uint32(len(z)))
		binary.LittleEndian.PutUint32(h[4:], uint32(len(part)))
		out = append(append(out, h[:]...), z...)
	}
	return out
}

func makeGhost(seconds, speed, score float64, carID uint32, accel float64) []byte {
	const hz = 5
	v, x := 0.0, 100.0
	var recs []byte
	q := func(v, lo, span, top float64) uint64 { return uint64(math.RoundToEven((v - lo) / span * top)) }
	for i := 0; i <= int(seconds*hz); i++ {
		bits := q(x, -16000, 32000, (1<<22)-1) | q(50, -1000, 2020, (1<<20)-1)<<22 | q(200, -16000, 32000, (1<<22)-1)<<42
		pkt := []byte{2 | ghost.MsgCarPos<<3, 0x90, 0}
		var b8 [8]byte
		binary.LittleEndian.PutUint64(b8[:], bits)
		pkt = append(pkt, b8[:]...)
		pkt = append(pkt, make([]byte, 6)...)
		pkt = append(pkt, 0, 3<<4, 0)
		var h2 [2]byte
		binary.LittleEndian.PutUint16(h2[:], 127)
		pkt = append(pkt, h2[:]...)
		binary.LittleEndian.PutUint16(h2[:], 30000)
		pkt = append(pkt, h2[:]...)
		var rec [4]byte
		binary.LittleEndian.PutUint32(rec[:], math.Float32bits(float32(float64(i)/hz)))
		recs = append(recs, rec[:]...)
		recs = append(recs, pkt...)
		binary.LittleEndian.PutUint32(rec[:], uint32(len(pkt)-1))
		recs = append(recs, rec[:]...)
		v = math.Min(speed, v+accel/hz)
		x += v / hz
	}
	h := make([]byte, 3600)
	binary.LittleEndian.PutUint64(h[2082:], math.Float64bits(score))
	binary.LittleEndian.PutUint32(h[3000:], carID)
	a, b := chunks(h), chunks(recs)
	var hd [8]byte
	binary.LittleEndian.PutUint32(hd[:], uint32(len(a)))
	binary.LittleEndian.PutUint32(hd[4:], uint32(len(b)))
	return append(append(hd[:], a...), b...)
}

func TestMatchesPython(t *testing.T) {
	f := load(t)
	d, err := db.Open(dbtest.New(t, ""))
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	clock := 1_000_000.0
	oldNow, oldRand := Now, Urandom
	Now = func() float64 { return clock }
	n := 0
	Urandom = func(k int) []byte {
		n++
		b := make([]byte, k)
		for i := range b {
			b[i] = byte(n*7 + i)
		}
		return b
	}
	defer func() { Now, Urandom, FakeRows = oldNow, oldRand, 0 }()
	for _, p := range []struct {
		x   int64
		tag string
	}{{0x100, "Alice"}, {0x200, "Bob"}, {0x300, "Carol"}, {0x400, "Dave"}, {0x500, ""}} {
		if _, err := d.Exec("INSERT INTO players (xuid, gamertag, first_seen, last_seen) VALUES (?,?,?,?)", p.x, p.tag, 1.0, 1.0); err != nil {
			t.Fatal(err)
		}
	}
	routes := map[int64]float64{1001: 5000, 1002: 3000, 1003: 46000}
	events := map[int64]*EventInfo{500: {1001, 2, i64(3), i64(3)}, 501: {1002, 1, nil, nil}, 502: {1003, 3, i64(5), i64(5)}}
	var cars []cardb.PoolCar
	for i := 0; i < 40; i++ {
		cars = append(cars, cardb.PoolCar{ID: int64(1000 + i), Class: int64(i % 6), PI: round4(0.25 + float64(i)*0.0237), Top: 50.0 + math.Mod(float64(i)*3.1, 80)})
	}
	eventCars := map[int64][]cardb.PoolCar{500: cars[5:15], 501: cars[:3], 600: cars[20:25]}
	eventIDs := map[int64]map[int64]bool{500: {}}
	for _, c := range cars[5:12] {
		eventIDs[500][c.ID] = true
	}
	records := map[[2]int]float64{{1001, 2}: 100, {1001, 4}: 80, {1002, 3}: 60, {1002, 5}: 55, {1003, 0}: 900}
	names := []string{"Alice", "", "Bob", "Alice", "A very long driver name", "Ünï Cödé", "Zed", "Mallory", "Trent", "Peggy", "Victor", "Walter"}
	st := New(d, names, cars)
	st.RouteLength = func(tr int64) (float64, bool) { l, ok := routes[tr]; return l, ok }
	st.EventInfo = func(e int64) *EventInfo { return events[e] }
	st.EventCars = func(e int64) []cardb.PoolCar { return eventCars[e] }
	st.EventCarIDs = func(e int64) map[int64]bool { return eventIDs[e] }
	st.Records = records
	st.GhostPolicy = "log"
	r := &run{t: t, f: f}
	must := func(err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
	}
	put := func(name string, xuid int64, tag string, scoreType int64, d *Submission) *Stored {
		clock++
		s, err := st.Submit(xuid, tag, scoreType, d)
		must(err)
		r.check(name, storedMap(s))
		return s
	}

	put("class_a1", 0x100, "Alice", 1, sub(1, 120.5))
	put("class_a2", 0x100, "Alice", 1, sub(1, 118.25, autobrake()))
	put("class_b1", 0x200, "Bob", 1, sub(1, 121.0, cpi(0.71)))
	put("class_c1", 0x300, "Carol", 1, sub(1, 119.0, cpi(0.69), unclean()))
	put("class_d1", 0x400, "Dave", 1, sub(1, 119.0, cpi(0.65)))
	put("class_other", 0x100, "Alice", 1, sub(1, 150.0, cls(2), cpi(0.55), track(1002)))
	put("event_a", 0x100, "Alice", 1, sub(3, 230.0, gsid(500), car(1007)))
	put("event_b", 0x200, "Bob", 1, sub(3, 228.5, gsid(500)))
	put("event_open", 0x300, "Carol", 1, sub(3, 90.0, gsid(501), track(1002), cls(1), car(1001)))
	put("speed_a", 0x100, "Alice", 5, sub(9, 117.23, gsid(3), car(1030)))
	put("speed_b", 0x200, "Bob", 5, sub(9, 90.0, gsid(3), car(1031)))
	put("speed_other", 0x200, "Bob", 5, sub(9, 60.0, gsid(4), car(1031)))
	put("progress_1", 0x100, "Alice", 9, sub(4, 40.0, track(0)))
	put("progress_2", 0x100, "Alice", 9, sub(4, 38.0, track(0)))
	put("progress_b", 0x200, "Bob", 9, sub(4, 12.0, track(0)))
	put("xp_board", 0x100, "Alice", 3, sub(2, 123456.0, track(0)))
	put("xp_board_b", 0x200, "Bob", 3, sub(2, 5000.0, track(0)))
	put("refused_nan", 0x300, "Carol", 1, sub(1, math.NaN()))
	put("refused_inf", 0x300, "Carol", 1, sub(1, math.Inf(1)))
	put("refused_negative", 0x300, "Carol", 1, sub(1, -1.0))
	put("refused_zero_time", 0x300, "Carol", 1, sub(1, 0.0))
	put("refused_speed_cap", 0x300, "Carol", 5, sub(9, 130.0, gsid(3)))
	put("refused_progress_frac", 0x300, "Carol", 9, sub(4, 12.5, track(0)))
	put("refused_progress_big", 0x300, "Carol", 9, sub(4, 151.0, track(0)))
	put("refused_progress_neg", 0x300, "Carol", 9, sub(4, -2.0, track(0)))
	put("refused_maxed_class", 0x300, "Carol", 1, sub(3, 100.0, gsid(500), cls(6)))
	put("refused_wrong_class", 0x300, "Carol", 1, sub(3, 100.0, gsid(500), cls(2)))
	put("refused_class_r_festival", 0x300, "Carol", 1, sub(1, 100.0, cls(7)))
	put("refused_zero_score_ok", 0x300, "Carol", 3, sub(2, 0.0, track(0)))

	type board struct {
		nm                         string
		x, sb, stype, track, pivot int64
	}
	for _, fr := range []int{0, 6} {
		FakeRows = fr
		for _, b := range []board{{"class3", 0x100, 1, 1, 1001, 3}, {"class3_me_missing", 0x500, 1, 1, 1001, 3}, {"class_any_track", 0x200, 1, 1, -1, 3},
			{"event500", 0x100, 3, 1, 1001, 500}, {"event501", 0x300, 3, 1, 1002, 501}, {"event_unknown", 0x100, 3, 1, 1001, 999},
			{"speed3", 0x100, 9, 5, 65535, 3}, {"speed_all", 0x200, 9, 5, -1, -1}, {"progress", 0x100, 4, 9, 0, -1},
			{"xp", 0x200, 2, 3, 0, -1}, {"empty_class", 0x100, 1, 1, 1003, 0}, {"empty_other", 0x100, 7, 2, 5, -1}} {
			for _, sc := range [][2]int{{1, 25}, {2, 2}} {
				page, mine, nr, nf, err := st.Rows(b.x, b.sb, b.stype, b.track, sc[0], sc[1], b.pivot)
				must(err)
				r.check(fmt.Sprintf("rows_fr%d_%s_%d", fr, b.nm, sc[0]), map[string]any{"page": rowsMap(page), "mine": rowMap(mine), "real": nr, "fake": nf})
			}
		}
	}
	FakeRows = 0
	for _, b := range []board{{"class3", 0x100, 1, 1, 1001, 3}, {"event500", 0x200, 3, 1, 1001, 500}, {"speed3", 0x100, 9, 5, 65535, 3}, {"none", 0x100, 1, 1, 1001, 5}} {
		row, err := st.BestOf(b.x, b.sb, b.stype, b.track, b.pivot)
		must(err)
		r.check("best_"+b.nm, rowMap(row))
		top, err := st.Top(b.sb, b.stype, b.track, b.pivot, 10)
		must(err)
		r.check("top_"+b.nm, rowsMap(top))
	}
	top, err := st.Top(1, 1, 1001, 3, 2)
	must(err)
	r.check("top_limit", rowsMap(top))
	top, err = st.Top(1, 1, 1001, 3, 0)
	must(err)
	r.check("top_zero", rowsMap(top))

	type rv struct {
		nm                               string
		x, sb, stype, track, pivot, pick int64
	}
	for _, b := range []rv{{"newcomer", 0x500, 1, 1, 1001, 3, 0}, {"middle", 0x300, 1, 1, 1001, 3, 0}, {"first", 0x100, 1, 1, 1001, 3, 0}, {"last", 0x200, 1, 1, 1001, 3, 0},
		{"picked", 0x200, 1, 1, 1001, 3, 0x300}, {"self_pick", 0x100, 1, 1, 1001, 3, 0x100}, {"picked_missing", 0x100, 1, 1, 1001, 3, 0x999},
		{"empty_record", 0x100, 1, 1, 1001, 4, 0}, {"empty_record_faster", 0x100, 1, 1, 1001, 1, 0}, {"empty_no_record", 0x100, 1, 1, 1002, 1, 0},
		{"empty_fake_class", 0x100, 1, 1, 1002, 0, 0}, {"empty_event", 0x100, 3, 1, 1002, 501, 0}, {"empty_event2", 0x100, 3, 1, 1003, 502, 0},
		{"empty_speed", 0x100, 9, 5, 65535, 40, 0}, {"empty_count", 0x100, 4, 5, 0, -1, 0}, {"event_mid", 0x500, 3, 1, 1001, 500, 0}} {
		row, err := st.RivalFor(b.x, b.sb, b.stype, b.track, b.pivot, b.pick)
		must(err)
		r.check("rival_"+b.nm, rowMap(row))
	}
	for _, b := range [][4]int64{{1, 1, 1001, 3}, {1, 1, 1001, 1}, {1, 1, 1001, 5}, {1, 1, 1002, 0}, {1, 1, 1003, 2}, {1, 2, 1001, 3}, {3, 1, 0, 500}, {3, 1, 0, 501}, {3, 1, 0, 999},
		{6, 1, 1002, 77}, {9, 5, 1001, 3}, {1, 1, 1001, -1}, {1, 1, 1001, 6}} {
		secs, c, ok := st.RecordTarget(b[0], b[1], b[2], b[3])
		var got any
		if ok {
			got = []any{secs, c}
		}
		r.check(fmt.Sprintf("target_%d_%d_%d_%d", b[0], b[1], b[2], b[3]), got)
	}
	for _, b := range [][2]int64{{1001, 1}, {1001, 2}, {1001, 5}, {1003, 1}, {0, 1}, {-1, 3}, {65535, 1}, {4242, 1}, {1002, 0}} {
		r.check(fmt.Sprintf("bounty_%d_%d", b[0], b[1]), st.BountyFor(b[0], int(b[1])))
	}
	var fn []string
	for _, nm := range names {
		fn = append(fn, st.FakeName(FakeXuid(nm)))
	}
	fn = append(fn, st.FakeName(1), st.FakeName(TargetXuid), st.FakeName(FakeBase+0xFFFFFF+1))
	r.check("fake_names", fn)

	// ghosts and the verdict policy
	policy := func(name, p string) { st.GhostPolicy = p; r.check("policy_"+name, p) }
	policy("enforce", "enforce")
	clock = 2000
	runs := map[string]*Stored{}
	order := []string{"ok", "fast", "wrongpi", "shortroute"}
	for _, c := range []struct {
		nm  string
		x   int64
		tag string
		cpi float64
	}{{"ok", 0x100, "Alice", 0.70}, {"fast", 0x200, "Bob", 0.71}, {"wrongpi", 0x300, "Carol", 0.2}, {"shortroute", 0x400, "Dave", 0.70}} {
		clock += 5
		s, err := st.Submit(c.x, c.tag, 1, sub(1, 110.0, cpi(c.cpi)))
		must(err)
		runs[c.nm] = s
		r.check("enforce_submit_"+c.nm, storedMap(s))
	}
	vis := func() []bool {
		var out []bool
		for _, k := range order {
			v, err := st.IsVisible(runs[k].ID)
			must(err)
			out = append(out, v)
		}
		return out
	}
	r.check("enforce_pending_viewer", vis())
	top, err = st.Top(1, 1, 1001, 3, 10)
	must(err)
	r.check("enforce_top_hides_pending", rowsMap(top))
	page, mine, _, _, err := st.Rows(0x100, 1, 1, 1001, 1, 25, 3)
	must(err)
	r.check("enforce_rows_owner_sees", []any{rowsMap(page), rowMap(mine)})
	page, mine, _, _, err = st.Rows(0x300, 1, 1, 1001, 1, 25, 3)
	must(err)
	r.check("enforce_rows_other", []any{rowsMap(page), rowMap(mine)})
	gOk := makeGhost(110, 60, 110, 1010, 5)
	gFast := makeGhost(110, 140, 110, 1010, 40)
	gPi := makeGhost(110, 60, 110, 1010, 5)
	attach := func(name string, sid []byte, data []byte, xuid int64) {
		id, public, err := st.AttachGhost(sid, data, xuid)
		must(err)
		var idv any
		if id != 0 {
			idv = id
		}
		r.check(name, []any{idv, public})
	}
	attach("attach_unknown", make([]byte, 16), gOk, 0x100)
	attach("attach_wrong_owner", runs["ok"].ScoreID, gOk, 0x200)
	attach("attach_garbage", runs["ok"].ScoreID, []byte("garbage"), 0x100)
	attach("attach_too_big", runs["ok"].ScoreID, bytes.Repeat([]byte("x"), ghost.MaxBlob+1), 0x100)
	clock = 2100
	attach("attach_ok", runs["ok"].ScoreID, gOk, 0x100)
	attach("attach_ok_again", runs["ok"].ScoreID, gOk, 0x100)
	attach("attach_other_bytes", runs["ok"].ScoreID, makeGhost(110, 61, 110, 1010, 5), 0x100)
	attach("attach_fast", runs["fast"].ScoreID, gFast, 0x200)
	attach("attach_wrongpi", runs["wrongpi"].ScoreID, gPi, 0x300)
	attach("attach_shortroute", runs["shortroute"].ScoreID, makeGhost(110, 60, 110, 1010, 5), 0x400)
	r.check("after_visible", vis())
	top, err = st.Top(1, 1, 1001, 3, 10)
	must(err)
	r.check("after_top", rowsMap(top))
	g1, err := st.Ghost(runs["ok"].ID)
	must(err)
	g2, err := st.Ghost(99999)
	must(err)
	g3, err := st.PublicGhost(runs["ok"].ID)
	must(err)
	g4, err := st.PublicGhost(runs["fast"].ID)
	must(err)
	var n2, n4 any
	if g2 != nil {
		n2 = hex.EncodeToString(g2)
	}
	if g4 != nil {
		n4 = hex.EncodeToString(g4)
	}
	r.check("ghost_bytes", []any{bytes.Equal(g1, gOk), n2, bytes.Equal(g3, gOk), n4})
	b1, err := st.ByScoreID(runs["ok"].ScoreID)
	must(err)
	b2, err := st.ByScoreID(bytes.Repeat([]byte{1}, 16))
	must(err)
	withID := func(row *Row) any {
		if row == nil {
			return nil
		}
		m := rowMap(row).(map[string]any)
		m["id"] = row.ID
		return m
	}
	r.check("by_score_id", []any{withID(b1), withID(b2)})
	vrows, err := d.Query("SELECT submission_id, version, verdict, reasons, measurements FROM score_verdicts ORDER BY submission_id")
	must(err)
	var vr []any
	for i := range vrows {
		vr = append(vr, []any{vrows[i].Int("submission_id"), vrows[i].Int("version"), vrows[i].Str("verdict"), vrows[i].Str("reasons"), vrows[i].Str("measurements")})
	}
	r.check("verdict_rows", vr)
	hrows, err := d.Query("SELECT submission_id, hidden_at, source, reason FROM hidden_scores ORDER BY submission_id")
	must(err)
	var hr []any
	for i := range hrows {
		hr = append(hr, []any{hrows[i].Int("submission_id"), hrows[i].Float("hidden_at"), hrows[i].Str("source"), hrows[i].Str("reason")})
	}
	r.check("hidden_rows", hr)
	clock = 2200
	sp, err := st.Submit(0x300, "Carol", 5, sub(9, 80.0, gsid(3)))
	must(err)
	attach("attach_not_judged", sp.ScoreID, makeGhost(10, 60, 10, 1010, 5), 0x300)
	policy("log", "log")
	clock = 2300
	lg, err := st.Submit(0x500, "", 1, sub(1, 110.0, cpi(0.2)))
	must(err)
	v, err := st.IsVisible(lg.ID)
	must(err)
	r.check("log_submit_visible", v)
	attach("log_attach_hard", lg.ScoreID, makeGhost(110, 60, 110, 1010, 5), 0x500)
	v, err = st.IsVisible(lg.ID)
	must(err)
	r.check("log_still_visible", v)

	for i := 0; i < 10; i++ {
		_, err := d.Exec("INSERT INTO score_reports (submission_id, browser, ip, created) VALUES (?,?,?,?)", 3, fmt.Sprintf("b%d", i), fmt.Sprintf("i%d", i), 1.0)
		must(err)
	}
	_, err = d.Exec("INSERT INTO score_reports (submission_id, browser, ip, created) VALUES (?,?,?,?)", 4, "x", "y", 1.0)
	must(err)
	_, err = d.Exec("INSERT INTO hidden_scores (submission_id, hidden_at, source) VALUES (?,?,'operator')", 1, 5.0)
	must(err)
	top, err = st.Top(1, 1, 1001, 3, 10)
	must(err)
	r.check("top_with_reports", rowsMap(top))
	bo, err := st.BestOf(0x100, 1, 1, 1001, 3)
	must(err)
	v1, _ := st.IsVisible(1)
	v2, _ := st.IsVisible(2)
	r.check("hidden_best", []any{rowMap(bo), v1, v2})
	cnt, err := st.Count()
	must(err)
	r.check("count", cnt)

	clock = 5000
	for _, c := range []struct {
		nm string
		xp float64
	}{{"ok", 150}, {"zero", 0}, {"nan", math.NaN()}, {"neg", -5}, {"huge", 1e9}, {"inf", math.Inf(1)}, {"big_ok", 500000}, {"budget_1", 560000}, {"budget_2", 560000},
		{"budget_3", 560000}, {"budget_4", 100000}} {
		clock += 10
		total, verdict, reason, err := st.AddXP(0x700, c.xp)
		must(err)
		r.check("xp_"+c.nm, []any{total, verdict, reason})
	}
	a, _ := st.XPOf(0x700)
	b, _ := st.XPOf(0x999)
	r.check("xp_of", []float64{a, b})
	total, verdict, reason, err := st.AddXP(0x701, 42.5)
	must(err)
	r.check("xp_other", []any{total, verdict, reason})
	xt := func(name string, limit int) {
		es, err := st.XPTop(limit)
		must(err)
		out := []any{}
		for _, e := range es {
			out = append(out, map[string]any{"position": e.Position, "xuid": e.Xuid, "tag": e.Tag, "score": e.Score})
		}
		r.check(name, out)
	}
	xt("xp_top", 25)
	xt("xp_top_1", 1)
	xt("xp_top_neg", -3)
	arows, err := d.Query("SELECT xuid, xp, verdict, reason FROM xp_awards ORDER BY id")
	must(err)
	var ar []any
	for i := range arows {
		ar = append(ar, []any{arows[i].Int("xuid"), arows[i].Float("xp"), arows[i].Str("verdict"), arows[i].Str("reason")})
	}
	r.check("xp_awards", ar)
	if r.n != len(f.Steps) {
		t.Fatalf("Go ran %d steps, Python %d", r.n, len(f.Steps))
	}
}

func zlibCompress(b []byte) []byte {
	var buf bytes.Buffer
	w := zlib.NewWriter(&buf)
	w.Write(b)
	w.Close()
	return buf.Bytes()
}

// A car outside the game catalog never reaches a car board: refused on submit, hidden when already stored (reversibly), counters unaffected.
func TestCarOutsideCatalog(t *testing.T) {
	d, err := db.Open(dbtest.New(t, ""))
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	oldNow := Now
	Now = func() float64 { return 1000 }
	defer func() { Now = oldNow }()
	st := New(d, nil, nil)
	st.GhostPolicy = "log"
	must := func(err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
	}
	// stored before there was a catalog check
	old, err := st.Submit(10, "old", 1, sub(3, 70.0, car(999999)))
	must(err)
	ok, err := st.Submit(10, "ok", 1, sub(3, 71.0, car(5)))
	must(err)
	if old == nil || ok == nil {
		t.Fatal("without a catalog nothing is refused")
	}
	if n, err := st.HideUnknownCars(); n != 0 || err != nil {
		t.Fatalf("HideUnknownCars without a catalog = %d, %v", n, err)
	}
	st.KnownCars = func() map[int64]bool { return map[int64]bool{5: true, 6: true} }
	for _, sb := range []int64{1, 3, 6, 9} {
		for _, c := range []int64{999999, 0} {
			s, err := st.Submit(11, "pack", 1, sub(sb, 80.0, car(c)))
			must(err)
			if s != nil {
				t.Errorf("board %d car %d was stored", sb, c)
			}
		}
		s, err := st.Submit(11, "stock", 1, sub(sb, 80.0, car(6)))
		must(err)
		if s == nil {
			t.Errorf("board %d car 6 (in the catalog) was refused", sb)
		}
	}
	if s, err := st.Submit(11, "count", 2, sub(4, 5.0, car(0))); err != nil || s == nil {
		t.Errorf("a progress counter (no car) was refused: %v %v", s, err)
	}
	if why := st.Refusal(1, sub(1, 80.0, car(0)), 80.0); why == "" {
		t.Error("Refusal is empty for car 0")
	}
	// an operator restore stays: run 3 below is restored before the pass
	extra, err := st.Submit(12, "restored", 1, sub(6, 90.0, car(5)))
	must(err)
	_, err = d.Exec("UPDATE score_submissions SET car=424242 WHERE id=?", extra.ID)
	must(err)
	_, err = d.Exec("INSERT INTO score_overrides (submission_id, at) VALUES (?,?)", extra.ID, 1.0)
	must(err)
	if n, err := st.HideUnknownCars(); n != 1 || err != nil {
		t.Fatalf("HideUnknownCars = %d, %v; want 1", n, err)
	}
	if n, err := st.HideUnknownCars(); n != 0 || err != nil {
		t.Fatalf("second HideUnknownCars = %d, %v; want 0 (idempotent)", n, err)
	}
	h, err := d.Query("SELECT submission_id, source, reason FROM hidden_scores")
	must(err)
	if len(h) != 1 || h[0].Int("submission_id") != old.ID || h[0].Str("source") != "auto" || h[0].Str("reason") != "car not in the game catalog" {
		t.Fatalf("hidden_scores = %+v", h)
	}
	top, err := st.Top(3, 1, 1001, 0, 10)
	must(err)
	if len(top) != 2 || top[0].Car+top[1].Car != 11 {
		t.Errorf("board shows %d rows, want cars 5 and 6 only", len(top))
	}
	v, err := st.IsVisible(extra.ID)
	must(err)
	if !v {
		t.Error("a restored run was hidden again")
	}
	cnt, err := st.Count()
	must(err)
	if cnt != 2+4+1+1 {
		t.Errorf("rows deleted? count = %d", cnt)
	}
}
