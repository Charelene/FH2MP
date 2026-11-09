package drivatar

import (
	"compress/gzip"
	"crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"reflect"
	"sort"
	"strings"
	"testing"

	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/pyrand"
)

type fixture struct {
	Steps []struct {
		S        string          `json:"s"`
		R        json.RawMessage `json:"r"`
		KeyError *string         `json:"key_error"`
	} `json:"steps"`
}

func load(t *testing.T) *fixture {
	f, err := os.Open("../../testdata/drivatar.json.gz")
	if err != nil {
		t.Skip(err)
	}
	defer f.Close()
	zr, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	var fx fixture
	if err := json.NewDecoder(zr).Decode(&fx); err != nil {
		t.Fatal(err)
	}
	return &fx
}

func J(r Record) any {
	if r == nil {
		return nil
	}
	s, err := pyjson.Dumps(r, pyjson.Opts{})
	if err != nil {
		panic(err)
	}
	return s
}

func Js(rs []Record) []any {
	out := []any{}
	for _, r := range rs {
		out = append(out, J(r))
	}
	return out
}

func i64(v int64) *int64 { return &v }

func firstFloat(o pyjson.Obj) string {
	for _, p := range o {
		if _, ok := p.V.(float64); ok {
			return p.K
		}
	}
	return ""
}

func TestMatchesPython(t *testing.T) {
	f := load(t)
	d, err := db.Open(dbtest.New(t, "fh2mp_cat"))
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	clock := 1_700_000_000.0
	oldNow := Now
	Now = func() float64 { return clock }
	oldPlayers, oldCars, oldBots := CachePlayers, Cars, Bots
	defer func() { Now, CachePlayers, Cars, Bots = oldNow, oldPlayers, oldCars, oldBots }()
	Bots = true  // the Python store served the shipped Drivatars; FH2MP_DRIVATAR_BOTS=1 keeps that behaviour
	Cars = false // Python's default; the steps below switch it on where Python did (the Go default is on since 2026-11-09)
	rows, err := d.Query("SELECT record FROM game.drivatar_shipped ORDER BY idx")
	if err != nil {
		t.Fatal(err)
	}
	var shipped []Record
	for i := range rows {
		v, err := pyjson.Parse(rows[i].Str("record"))
		if err != nil {
			t.Fatal(err)
		}
		shipped = append(shipped, v.(pyjson.Obj))
	}
	st := New(shipped, d)
	st.KnownCars = map[int]struct{}{1005: {}, 1010: {}, 1020: {}}
	n := 0
	check := func(name string, got any, err error) {
		t.Helper()
		if n >= len(f.Steps) {
			t.Fatalf("%s: no more steps", name)
		}
		want := f.Steps[n]
		n++
		if want.S != name {
			t.Fatalf("step %d is %s in Python and %s in Go", n, want.S, name)
		}
		var ke *KeyError
		if errors.As(err, &ke) {
			if want.KeyError == nil || *want.KeyError != ke.Error() {
				t.Fatalf("%s: key error %q, python %v", name, ke.Error(), want.KeyError)
			}
			return
		}
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if want.KeyError != nil {
			t.Fatalf("%s: python raised KeyError %s", name, *want.KeyError)
		}
		b, _ := json.Marshal(got)
		var g, w any
		json.Unmarshal(b, &g)
		json.Unmarshal(want.R, &w)
		if !reflect.DeepEqual(g, w) {
			bs, ws := string(b), string(want.R)
			if len(bs) > 1500 {
				bs = bs[:1500]
			}
			if len(ws) > 1500 {
				ws = ws[:1500]
			}
			t.Fatalf("step %s differs\n got %s\nwant %s", name, bs, ws)
		}
	}
	must := func(err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
	}
	stored := func(xuid int64) any {
		q := "SELECT xuid, gamertag, skill, wristband, locked, record, updated FROM drivatars"
		var rs []db.Row
		var err error
		if xuid != 0 {
			rs, err = d.Query(q+" WHERE xuid=? ORDER BY seq", xuid)
		} else {
			rs, err = d.Query(q + " ORDER BY seq")
		}
		must(err)
		out := []any{}
		for i := range rs {
			row := []any{}
			for _, v := range rs[i].Vals {
				row = append(row, v)
			}
			out = append(out, row)
		}
		return out
	}

	tpl0 := shipped[0]
	ui0 := get(tpl0, "UserInfo").(pyjson.Obj)
	uiFloat := firstFloat(ui0)
	baseFloat := firstFloat(get(ui0, "base").(pyjson.Obj))
	skills, wbs := map[int64]bool{}, map[int64]bool{}
	for _, t := range shipped {
		skills[asInt(get(t, "UserSkillLevel"))] = true
		wbs[asInt(get(t, "wristbandLevel"))] = true
	}
	sortedKeys := func(m map[int64]bool) []int64 {
		out := []int64{}
		for k := range m {
			out = append(out, k)
		}
		sort.Slice(out, func(i, j int) bool { return out[i] < out[j] })
		return out
	}
	keys := []string{}
	for _, p := range tpl0 {
		keys = append(keys, p.K)
	}
	check("template_shape", []any{len(shipped), sortedKeys(skills), sortedKeys(wbs), []string{uiFloat, baseFloat}, keys}, nil)

	names := []any{}
	for _, t := range shipped[:4] {
		names = append(names, st.BotName(asInt(get(t, "Xuid"))))
	}
	names = append(names, st.BotName(12345))
	check("bot_names", names, nil)
	a, _ := st.TagForXuid(12345)
	b, _ := st.TagForXuid(asInt(get(shipped[1], "Xuid")))
	check("tag_for_unknown", []string{a, b}, nil)
	var bw []int64
	for _, x := range []int64{-3, 0, 1, 5, 24, 25, 99} {
		bw = append(bw, st.BucketWristband(x))
	}
	check("bucket_wristband", bw, nil)
	check("templates", []any{J(st.Template(nil, nil)), J(st.Template(i64(0), i64(0))), J(st.Template(i64(4), i64(1_000_000_000))), J(st.Template(i64(2), i64(100000))),
		J(st.Template(i64(1), nil)), J(st.Template(nil, i64(50000)))}, nil)
	c0, err := st.Count()
	check("count_empty", c0, err)

	clock = 1_700_000_100
	r, err := st.ForPlayer(0x100, "Alice")
	check("for_player_new", J(r), err)
	check("stored_alice", stored(0x100), nil)
	clock = 1_700_000_200
	r, err = st.ForPlayer(0x100, "Alice")
	check("for_player_same", J(r), err)
	check("stored_alice_untouched", stored(0x100), nil)
	r, err = st.ForPlayer(0x100, "")
	check("for_player_no_tag", J(r), err)
	clock = 1_700_000_300
	r, err = st.ForPlayer(0x100, "AliceNew")
	check("for_player_renamed", J(r), err)
	st.CarOverride = map[string]int64{"256": 777}
	clock = 1_700_000_400
	r, err = st.ForPlayer(0x100, "AliceNew")
	check("for_player_car", J(r), err)
	st.CarOverride = map[string]int64{}
	clock = 1_700_000_500
	r, err = st.AddPlayer(0x200, "Bob the very long name", i64(1), i64(100000))
	check("add_player", J(r), err)
	r, err = st.AddPlayer(0x300, "Carol", nil, nil)
	check("add_player_default", J(r), err)
	r1, _ := st.Remove(0x300)
	r2, _ := st.Remove(0x300)
	check("remove", []bool{r1, r2}, nil)
	cnt, err := st.Count()
	check("count", cnt, err)
	t1, _ := st.TagForXuid(0x100)
	t2, _ := st.TagForXuid(0x200)
	t3, _ := st.TagForXuid(0x300)
	check("tag_stored", []string{t1, t2, t3}, nil)

	tr, fl := true, false
	r, err = st.Edit(0x999, []Set{{uiFloat, 1.5}}, nil)
	check("edit_none", J(r), err)
	r, err = st.Edit(0x100, []Set{{uiFloat, 1.5}, {baseFloat, 2.25}, {"UserSkillLevel", 2.9}, {"wristbandLevel", 123456}, {"CarId", 5}, {"BucketId", 3}}, nil)
	check("edit_ok", J(r), err)
	r, err = st.Edit(0x100, []Set{{uiFloat, 99.0}, {"NoSuchField", 1}}, nil)
	check("edit_unknown_key", J(r), err)
	check("after_failed_edit", stored(0x100), nil)
	r, err = st.Edit(0x100, nil, &tr)
	check("edit_lock", J(r), err)
	r, err = st.Edit(0x100, []Set{{uiFloat, 0.5}}, &fl)
	check("edit_unlock", J(r), err)
	r, err = st.Edit(0x200, nil, &tr)
	check("edit_lock_only", J(r), err)
	check("stored_after_edit", stored(0), nil)

	base7, err := copyRecord(shipped[7])
	must(err)
	info, curves, segments := get(base7, "UserInfo"), get(base7, "Curves"), get(base7, "Segments")
	baseObj := get(info.(pyjson.Obj), "base").(pyjson.Obj)
	baseObj.Set(baseFloat, 123.456)
	upload := func(name string, xuid int64, tag string, i, c, s any) {
		stored, refused, err := st.SaveUpload(xuid, tag, i, c, s)
		var got any
		switch {
		case refused:
			got = false
		case stored:
			got = true
		}
		check(name, got, err)
	}
	clock = 1_700_000_600
	upload("upload_locked", 0x200, "Bob", info, curves, segments)
	r, err = st.Edit(0x200, nil, &fl)
	check("unlock_bob", J(r), err)
	clock = 1_700_000_700
	upload("upload_ok", 0x200, "Bob", info, curves, segments)
	check("after_upload", stored(0x200), nil)
	bi, err := copyRecord(pyjson.Obj{{K: "i", V: info}})
	must(err)
	badInfo := get(bi, "i").(pyjson.Obj)
	badInfo.Set(uiFloat, 1e7)
	upload("upload_insane_info", 0x200, "Bob", badInfo, curves, segments)
	ctext, _ := pyjson.Dumps(curves, pyjson.Opts{})
	badCurves := curves
	if strings.Contains(ctext, "0.0") {
		badCurves, err = pyjson.Parse(strings.Replace(ctext, "0.0", "-2000000.0", 1))
		must(err)
	}
	upload("upload_insane_curves", 0x200, "Bob", info, badCurves, segments)
	clock = 1_700_000_800
	upload("upload_new_player", 0x500, "Newbie", info, curves, segments)
	check("after_new_player", stored(0x500), nil)
	w1, w2, w3 := st.SetWristband(0x500, 150000), st.SetWristband(0x500, 150000), st.SetWristband(0x9999, 5)
	must(w1)
	must(w2)
	must(w3)
	check("set_wristband", []any{nil, nil, nil}, nil)
	check("after_wristband", stored(0x500), nil)

	clock = 1_700_001_000
	for i := int64(0); i < 12; i++ {
		x := 0x600 + i
		clock = 1_700_001_000 + float64(i)*10
		_, err := st.AddPlayer(x, fmt.Sprintf("Crowd%d", i), i64(i%5), i64([]int64{46000, 80000, 120000, 240000, 400000}[i%5]))
		must(err)
		if i%4 == 3 {
			must(st.SetWristband(x, 46000))
		}
	}
	row, err := d.One("SELECT record FROM drivatars WHERE xuid=?", 0x601)
	must(err)
	v, _ := pyjson.Parse(row.Str("record"))
	bad := v.(pyjson.Obj)
	badBase := get(get(bad, "UserInfo").(pyjson.Obj), "base").(pyjson.Obj)
	badBase.Set(baseFloat, 5e9)
	txt, _ := pyjson.Dumps(bad, pyjson.Opts{})
	_, err = d.Exec("UPDATE drivatars SET record=? WHERE xuid=?", txt, 0x601)
	must(err)
	_, err = d.Exec("UPDATE drivatars SET locked=1 WHERE xuid=?", 0x603)
	must(err)
	row, err = d.One("SELECT record FROM drivatars WHERE xuid=?", 0x604)
	must(err)
	v, _ = pyjson.Parse(row.Str("record"))
	r4 := v.(pyjson.Obj)
	r4.Set("GamerTag", "")
	txt, _ = pyjson.Dumps(r4, pyjson.Opts{})
	_, err = d.Exec("UPDATE drivatars SET gamertag='', record=? WHERE xuid=?", txt, 0x604)
	must(err)
	tags, err := st.Gamertags()
	check("gamertags", tags, err)
	check("stored_crowd", stored(0), nil)

	type pk struct {
		limit       int
		skill, near *int64
		ex          int64
		exs         []int64
		anySkill    bool
	}
	s0, s1 := shipped[0], shipped[1]
	pools := []pk{{10, nil, nil, 0, nil, true}, {5, i64(3), i64(100000), 0, nil, true}, {40, i64(2), nil, 0x602, []int64{0x603, 0x605}, true},
		{30, i64(1), nil, 0, nil, false}, {0, nil, nil, 0, nil, true}, {200, nil, i64(240000), 0, nil, true}, {3, i64(4), i64(46000), 0, nil, true},
		{16, i64(0), nil, 0, []int64{asInt(get(s0, "Xuid")), asInt(get(s1, "Xuid"))}, true}}
	for i, p := range pools {
		for seed := 1; seed <= 2; seed++ {
			st.SetRng(pyrand.New(uint64(seed*100 + i)))
			recs, err := st.Pool(p.limit, p.skill, p.near, p.ex, p.exs, p.anySkill)
			check(fmt.Sprintf("pool_%d_%d", i, seed), Js(recs), err)
		}
	}
	for i, c := range []struct {
		limit int
		ex    int64
	}{{120, 0}, {120, 0x600}, {20, 0}, {5, 0}, {0, 0}, {50, 0x602}} {
		st.SetRng(pyrand.New(uint64(900 + i)))
		recs, err := st.ForCache(c.limit, c.ex)
		check(fmt.Sprintf("cache_%d", i), Js(recs), err)
	}
	os.Setenv("FH2MP_DRIVATAR_CACHE_LIMIT", "30")
	st.SetRng(pyrand.New(5))
	recs, err := st.ForCache(120, 0)
	check("cache_env_limit", Js(recs), err)
	os.Setenv("FH2MP_DRIVATAR_CACHE_LIMIT", "garbage")
	st.SetRng(pyrand.New(6))
	recs, err = st.ForCache(120, 0)
	check("cache_env_garbage", len(recs), err)
	os.Setenv("FH2MP_DRIVATAR_CACHE_LIMIT", "0")
	st.SetRng(pyrand.New(7))
	recs, err = st.ForCache(120, 0)
	check("cache_env_zero", len(recs), err)
	os.Unsetenv("FH2MP_DRIVATAR_CACHE_LIMIT")
	CachePlayers = false
	st.SetRng(pyrand.New(8))
	recs, err = st.ForCache(120, 0)
	check("cache_no_players", Js(recs), err)
	CachePlayers = true

	for _, q := range []struct {
		x   int64
		car int64
		t   float64
	}{{0x100, 1010, 5}, {0x100, 1005, 4}, {0x200, 999, 9}, {0x600, 1020, 3}, {0x600, 1010, 2}, {0x602, 1005, 7}, {0x605, 0, 8}} {
		id := make([]byte, 16)
		rand.Read(id)
		_, err := d.Exec("INSERT INTO score_submissions (xuid, sb, st, gsid, track, car, cls, cpi, score, clean, stm, tcs, abs, autobrake, autoshift, clutch, submitted, score_id) "+
			"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", q.x, 1, 1, 0, 1001, q.car, 3, 0.7, 100.0, 1, 0, 0, 0, 0, 0, 0, q.t, id)
		must(err)
	}
	for _, q := range []struct {
		creator, car int64
		kind         string
		refs         int64
		t            float64
	}{{0x100, 1010, "livery", 1, 1}, {0x100, 1010, "livery", 1, 2}, {0x100, 1010, "tune", 0, 3}, {0x100, 1005, "tune", 1, 3}, {0x600, 1020, "livery", 2, 4}, {0x602, 1005, "tune", 1, 5}} {
		_, err := d.Exec("INSERT INTO ugc_versioned (kind, car_id, name, creator, layers, data, thumb, thumb_small, refs, created) VALUES (?,?,?,?,?,?,?,?,?,?)",
			q.kind, q.car, "x", q.creator, 0, []byte{}, []byte{}, []byte{}, q.refs, q.t)
		must(err)
	}
	Cars = true
	st.SetRng(pyrand.New(11))
	recs, err = st.Pool(40, nil, nil, 0, nil, true)
	check("cars_pool", Js(recs), err)
	st.SetRng(pyrand.New(12))
	recs, err = st.ForCache(120, 0)
	check("cars_cache", Js(recs), err)
	Cars = false
	st.SetRng(pyrand.New(11))
	recs, err = st.Pool(40, nil, nil, 0, nil, true)
	check("cars_off", Js(recs), err)
	check("final", stored(0), nil)
	if n != len(f.Steps) {
		t.Fatalf("Go ran %d steps, Python %d", n, len(f.Steps))
	}
}
