package rivals

import (
	"encoding/json"
	"os"
	"reflect"
	"sort"
	"testing"
	"time"

	"fh2mp/internal/cardb"
	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
)

type fixture struct {
	Names   map[string][][2]json.RawMessage `json:"names"`
	Themes  []json.RawMessage               `json:"themes"`
	Usable  []int                           `json:"usable"`
	Rentals []int64                         `json:"rentals"`
	CarsFor []struct {
		Attrs [][2]any     `json:"attrs"`
		Cars  [][4]float64 `json:"cars"`
	} `json:"cars_for"`
	WeatherWord [][3]json.RawMessage `json:"weather_word"`
	Tod         [][3]json.RawMessage `json:"tod"`
	Conditions  [][3]json.RawMessage `json:"conditions"`
	Generate    []struct {
		Month, N int64
		Events   []json.RawMessage `json:"events"`
	} `json:"generate"`
	Describe []struct {
		Event     json.RawMessage `json:"event"`
		Text      string          `json:"text"`
		TextNamed string          `json:"text_named"`
	} `json:"describe"`
	Serial []struct {
		Date string   `json:"date"`
		Meta *string  `json:"meta"`
		R    [2]int64 `json:"r"`
	} `json:"serial"`
	Steps []struct {
		S string          `json:"s"`
		R json.RawMessage `json:"r"`
	} `json:"steps"`
}

func load(t *testing.T) *fixture {
	raw, err := os.ReadFile("../../testdata/rivals.json")
	if err != nil {
		t.Skip(err)
	}
	var f fixture
	if err := json.Unmarshal(raw, &f); err != nil {
		t.Fatal(err)
	}
	return &f
}

func jsonOf(v any) any {
	b, err := json.Marshal(v)
	if err != nil {
		panic(err)
	}
	var out any
	json.Unmarshal(b, &out)
	return out
}

func rawOf(r json.RawMessage) any {
	var out any
	json.Unmarshal(r, &out)
	return out
}

func pairs(a Attrs) [][]any {
	out := [][]any{}
	for _, p := range a {
		out = append(out, []any{p.K, p.V})
	}
	return out
}

func fp(p *float64) any {
	if p == nil {
		return nil
	}
	return *p
}

func eventMap(e *Event) any {
	if e == nil {
		return nil
	}
	return map[string]any{"id": e.ID, "track": e.Track, "laps": e.Laps, "length": e.Length, "kind": e.Kind, "attrs": pairs(e.Attrs), "cls": ptr(e.Cls), "name": e.Name,
		"trackname": e.TrackName, "description": e.Description, "tod": ptr(e.Tod), "weather": ptr(e.Weather), "wetness": fp(e.Wetness), "precip": fp(e.Precip)}
}

func eventsMap(es []*Event) any {
	out := []any{}
	for _, e := range es {
		out = append(out, eventMap(e))
	}
	return out
}

func themeMap(t *Theme) any {
	var base, avoid any
	if t.BasePhrase != "" {
		base = t.BasePhrase
	}
	if t.Avoid {
		avoid = true
	}
	return map[string]any{"kind": t.Kind, "attrs": pairs(t.Attrs), "title": t.Title, "phrase": t.Phrase, "cars": t.Cars, "cls": ptr(t.Cls), "base_phrase": base, "avoid": avoid}
}

func same(t *testing.T, what string, got, want any) {
	t.Helper()
	g, w := jsonOf(got), want
	if !reflect.DeepEqual(g, w) {
		gb, _ := json.Marshal(g)
		wb, _ := json.Marshal(w)
		if len(gb) > 1500 {
			gb = gb[:1500]
		}
		if len(wb) > 1500 {
			wb = wb[:1500]
		}
		t.Fatalf("%s differs\n got %s\nwant %s", what, gb, wb)
	}
}

func newCatalog(t *testing.T) (*Catalog, *db.DB) {
	d, err := db.Open(dbtest.New(t, "fh2mp_cat"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(d.Close)
	c := New(cardb.New(d), d)
	c.Today = func() time.Time { return time.Date(2026, 11, 15, 0, 0, 0, 0, time.Local) }
	return c, d
}

func TestCatalogMatchesPython(t *testing.T) {
	f := load(t)
	c, _ := newCatalog(t)
	n, err := c.Names()
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]any{}
	conv := func(order []int64, m map[int64]string) [][]any {
		out := [][]any{}
		for _, id := range order {
			out = append(out, []any{id, m[id]})
		}
		return out
	}
	tr := [][]any{}
	for _, id := range n.trackOrder {
		tr = append(tr, []any{id, []string{n.Track[id][0], n.Track[id][1]}})
	}
	got["track"], got["make"], got["body"], got["country"], got["drive"] = tr, conv(n.makeOrder, n.Make), conv(n.bodyOrder, n.Body), conv(n.countryOrder, n.Country), conv(n.driveOrder, n.Drive)
	var carIDs []int64
	for id := range n.Car {
		carIDs = append(carIDs, id)
	}
	for k, want := range f.Names {
		if k == "car" {
			if len(want) != len(n.Car) {
				t.Fatalf("%d car names, python %d", len(n.Car), len(want))
			}
			for _, p := range want {
				var id int64
				var name string
				json.Unmarshal(p[0], &id)
				json.Unmarshal(p[1], &name)
				if n.Car[id] != name {
					t.Fatalf("car %d is %q, python %q", id, n.Car[id], name)
				}
			}
			continue
		}
		w := make([]any, len(want))
		for i, p := range want {
			w[i] = []any{rawOf(p[0]), rawOf(p[1])}
		}
		same(t, "names "+k, got[k], any(w))
	}
	themes, err := c.Themes()
	if err != nil {
		t.Fatal(err)
	}
	if len(themes) != len(f.Themes) {
		t.Fatalf("%d themes, python %d", len(themes), len(f.Themes))
	}
	for i, th := range themes {
		same(t, "theme "+th.Kind, themeMap(th), rawOf(f.Themes[i]))
	}
	usable, err := c.UsableThemes()
	if err != nil {
		t.Fatal(err)
	}
	var idx []int
	for _, u := range usable {
		for i, th := range themes {
			if th == u {
				idx = append(idx, i)
			}
		}
	}
	if !reflect.DeepEqual(idx, f.Usable) {
		t.Fatalf("usable themes %v, python %v", idx, f.Usable)
	}
	rent := []int64{}
	for id := range c.Rentals() {
		rent = append(rent, id)
	}
	sort.Slice(rent, func(i, j int) bool { return rent[i] < rent[j] })
	if !reflect.DeepEqual(rent, f.Rentals) {
		t.Fatalf("rentals %v, python %v", rent, f.Rentals)
	}
	for _, cf := range f.CarsFor {
		var attrs Attrs
		for _, p := range cf.Attrs {
			attrs = append(attrs, struct {
				K string
				V any
			}{p[0].(string), int64(p[1].(float64))})
		}
		cars, err := c.CarsFor(attrs)
		if err != nil {
			t.Fatal(err)
		}
		if len(cars) != len(cf.Cars) {
			t.Fatalf("cars_for %v: %d cars, python %d", cf.Attrs, len(cars), len(cf.Cars))
		}
		for i, x := range cars {
			w := cf.Cars[i]
			if float64(x.ID) != w[0] || float64(x.Class) != w[1] || x.PI != w[2] || x.Top != w[3] {
				t.Fatalf("cars_for %v car %d: %+v python %v", cf.Attrs, i, x, w)
			}
		}
	}
}

func TestTextsMatchPython(t *testing.T) {
	f := load(t)
	c, _ := newCatalog(t)
	for _, w := range f.WeatherWord {
		var preset int64
		var precip *float64
		var want string
		json.Unmarshal(w[0], &preset)
		json.Unmarshal(w[1], &precip)
		json.Unmarshal(w[2], &want)
		got, err := c.WeatherWord(preset, precip)
		if err != nil || got != want {
			t.Errorf("WeatherWord(%d, %v) = %q %v, python %q", preset, precip, got, err, want)
		}
	}
	for _, w := range f.Tod {
		var h int64
		var word string
		var secs int64
		json.Unmarshal(w[0], &h)
		json.Unmarshal(w[1], &word)
		json.Unmarshal(w[2], &secs)
		if TodWord(h) != word || TodSeconds(h) != secs {
			t.Errorf("tod %d: %q %d, python %q %d", h, TodWord(h), TodSeconds(h), word, secs)
		}
	}
	for _, w := range f.Conditions {
		var tod *int64
		var adj *string
		var want *string
		json.Unmarshal(w[0], &tod)
		json.Unmarshal(w[1], &adj)
		json.Unmarshal(w[2], &want)
		got, ok := Conditions(tod, adj)
		if (want == nil) != !ok || (ok && got != *want) {
			t.Errorf("Conditions(%v, %v) = %q %v, python %v", tod, adj, got, ok, want)
		}
	}
	for _, g := range f.Generate {
		ev, err := c.Generate(g.Month, g.N)
		if err != nil {
			t.Fatal(err)
		}
		if len(ev) != len(g.Events) {
			t.Fatalf("generate(%d,%d): %d events, python %d", g.Month, g.N, len(ev), len(g.Events))
		}
		for i, e := range ev {
			same(t, "generate event", eventMap(e), rawOf(g.Events[i]))
		}
	}
	for _, d := range f.Describe {
		var e Event
		var m map[string]any
		json.Unmarshal(d.Event, &m)
		e.ID, e.Track, e.Laps, e.Length, e.Kind = int64(m["id"].(float64)), int64(m["track"].(float64)), int64(m["laps"].(float64)), m["length"].(float64), m["kind"].(string)
		for _, p := range m["attrs"].([]any) {
			pp := p.([]any)
			e.Attrs = append(e.Attrs, struct {
				K string
				V any
			}{pp[0].(string), int64(pp[1].(float64))})
		}
		if v, ok := m["cls"].(float64); ok {
			e.Cls = ip(int64(v))
		}
		if v, ok := m["tod"].(float64); ok {
			e.Tod = ip(int64(v))
		}
		if v, ok := m["weather"].(float64); ok {
			e.Weather = ip(int64(v))
		}
		if v, ok := m["precip"].(float64); ok {
			e.Precip = &v
		}
		got, err := c.Describe(&e, "")
		if err != nil || got != d.Text {
			t.Errorf("describe: %q %v, python %q", got, err, d.Text)
		}
		got, err = c.Describe(&e, "Some Track")
		if err != nil || got != d.TextNamed {
			t.Errorf("describe named: %q %v, python %q", got, err, d.TextNamed)
		}
	}
}

func TestSerialMatchesPython(t *testing.T) {
	f := load(t)
	c, d := newCatalog(t)
	for _, s := range f.Serial {
		if _, err := d.Exec("DELETE FROM meta WHERE key='rival_reroll'"); err != nil {
			t.Fatal(err)
		}
		if s.Meta != nil {
			if _, err := d.Exec("INSERT INTO meta (key, value) VALUES ('rival_reroll', ?)", *s.Meta); err != nil {
				t.Fatal(err)
			}
		}
		day, _ := time.Parse("2006-01-02", s.Date)
		cc := New(c.Cars, d)
		m, n, err := cc.Serial(&day)
		if err != nil || [2]int64{m, n} != s.R {
			t.Errorf("serial(%s, %v) = %d,%d %v, python %v", s.Date, s.Meta, m, n, err, s.R)
		}
	}
}

func TestStoredRotationMatchesPython(t *testing.T) {
	f := load(t)
	c, d := newCatalog(t)
	if _, err := d.Exec("DELETE FROM meta WHERE key='rival_reroll'"); err != nil {
		t.Fatal(err)
	}
	if _, err := d.Exec("DELETE FROM rival_events"); err != nil {
		t.Fatal(err)
	}
	n := 0
	check := func(name string, got any) {
		t.Helper()
		want := f.Steps[n]
		n++
		if want.S != name {
			t.Fatalf("step %d is %s in Python and %s in Go", n, want.S, name)
		}
		same(t, "step "+name, got, rawOf(want.R))
	}
	must := func(err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
	}
	first, err := c.Events()
	must(err)
	check("events", eventsMap(first))
	m, k, err := c.Serial(nil)
	must(err)
	check("serial", []int64{m, k})
	again, err := c.Events()
	must(err)
	check("events_cached", reflect.DeepEqual(eventsMap(again), eventsMap(first)))
	c2 := New(c.Cars, d)
	c2.Today = c.Today
	re, err := c2.Events()
	must(err)
	check("events_reloaded", eventsMap(re))
	rows, err := d.Query("SELECT id, month, reroll, idx, track, laps, length, kind, attrs, cls, name, trackname, description, tod, weather, wetness, precip FROM rival_events ORDER BY id")
	must(err)
	var rr []any
	for i := range rows {
		row := []any{}
		for _, v := range rows[i].Vals {
			row = append(row, v)
		}
		rr = append(rr, row)
	}
	check("rows", rr)
	rot, err := c2.Rotate()
	must(err)
	check("rotate1", eventsMap(rot))
	m, k, err = c2.Serial(nil)
	must(err)
	check("serial_after", []int64{m, k})
	for i := 2; i < 12; i++ {
		rot, err = c2.Rotate()
		must(err)
		ids := []int64{}
		for _, e := range rot {
			ids = append(ids, e.ID)
		}
		check("rotate"+itoa(i), ids)
	}
	m, k, err = c2.Serial(nil)
	must(err)
	check("serial_capped", []int64{m, k})
	e, err := c2.ByID(first[2].ID)
	must(err)
	check("by_id_old", eventMap(e))
	e, err = c2.ByID(rot[0].ID)
	must(err)
	check("by_id_current", eventMap(e))
	e, err = c2.ByID(12345)
	must(err)
	check("by_id_unknown", eventMap(e))
	for _, q := range []struct {
		id, idx, track int64
		length         float64
		kind, attrs    string
		cls            any
		name, tn       string
		tod, w         any
		wet, pr        any
	}{{7001, 0, first[0].Track, 2500, "make", `{"MakeId": 5}`, 2, "Old make event", "Track", nil, nil, nil, nil},
		{7002, 1, first[0].Track, 2500, "class", `{"CarClassId": 3}`, 3, "Old class event", "Track", nil, nil, nil, nil},
		{7003, 2, first[1].Track, 3000, "era", `{"YearMax": 1969}`, 4, "Old era event", "Track", 1500, 4, 0.5, 0.0},
		{7004, 3, 999999, 1000, "open", `{}`, nil, "Old open event", "", nil, nil, nil, nil}} {
		_, err := d.Exec("INSERT INTO rival_events (id, month, reroll, idx, track, laps, length, kind, attrs, cls, name, trackname, description, created, tod, weather, wetness, precip) VALUES "+
			"(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", q.id, 0, 0, q.idx, q.track, 1, q.length, q.kind, q.attrs, q.cls, q.name, q.tn, "old text", 1.0, q.tod, q.w, q.wet, q.pr)
		must(err)
	}
	cnt, err := c2.EnforceStoredClasses()
	must(err)
	check("enforce", cnt)
	cnt, err = c2.EnforceStoredClasses()
	must(err)
	check("enforce_again", cnt)
	cnt, err = c2.RefreshTexts()
	must(err)
	check("refresh", cnt)
	cnt, err = c2.RefreshTexts()
	must(err)
	check("refresh_again", cnt)
	lrows, err := d.Query("SELECT id, kind, attrs, cls, description FROM rival_events WHERE id >= 7001 ORDER BY id")
	must(err)
	var lr []any
	for i := range lrows {
		row := []any{}
		for _, v := range lrows[i].Vals {
			row = append(row, v)
		}
		lr = append(lr, row)
	}
	check("legacy_rows", lr)
	if n != len(f.Steps) {
		t.Fatalf("Go ran %d steps, Python %d", n, len(f.Steps))
	}
}

func itoa(i int) string { b, _ := json.Marshal(i); return string(b) }
