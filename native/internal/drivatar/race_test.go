package drivatar

import (
	"crypto/rand"
	"fmt"
	"os"
	"testing"

	"fh2mp/internal/catalog"
	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/pyrand"
)

// playersStore is a store on the test catalog's shipped Drivatars in the players-only mode (FH2MP_DRIVATAR_BOTS off), the switches restored after.
func playersStore(t *testing.T) (*Store, *db.DB, map[int64]bool) {
	t.Helper()
	d, err := db.Open(dbtest.New(t, "fh2mp_cat"))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { d.Close() })
	oldBots, oldPlayers, oldCars := Bots, CachePlayers, Cars
	t.Cleanup(func() { Bots, CachePlayers, Cars = oldBots, oldPlayers, oldCars })
	Bots, CachePlayers, Cars = false, true, false
	rows, err := d.Query("SELECT record FROM game.drivatar_shipped ORDER BY idx")
	if err != nil {
		t.Fatal(err)
	}
	var shipped []Record
	bot := map[int64]bool{}
	for i := range rows {
		v, err := pyjson.Parse(rows[i].Str("record"))
		if err != nil {
			t.Fatal(err)
		}
		shipped = append(shipped, v.(pyjson.Obj))
		bot[asInt(get(v.(pyjson.Obj), "Xuid"))] = true
	}
	if len(shipped) == 0 {
		t.Skip("no shipped Drivatars in the test catalog")
	}
	st := New(shipped, d)
	st.SetRng(pyrand.New(42))
	return st, d, bot
}

func addPlayers(t *testing.T, st *Store, from int64, n int) []int64 {
	t.Helper()
	var out []int64
	for i := 0; i < n; i++ {
		x := from + int64(i)
		if _, err := st.ForPlayer(x, fmt.Sprintf("P%x", x)); err != nil {
			t.Fatal(err)
		}
		out = append(out, x)
	}
	return out
}

func xuidsOf(recs []Record) []int64 {
	var out []int64
	for _, r := range recs {
		out = append(out, asInt(get(r, "Xuid")))
	}
	return out
}

// A race grid is exactly as long as asked, drawn from every named player (not the oldest rows), with bad rows left out before the count.
func TestRaceFullGridAndVariety(t *testing.T) {
	st, _, bot := playersStore(t)
	// the 12 oldest rows are unusable: 6 nameless, 6 with a number the client would choke on
	for _, x := range addPlayers(t, st, 0x8000, 12) {
		if x < 0x8006 {
			if _, err := st.DB.Exec("UPDATE drivatars SET gamertag='' WHERE xuid=?", x); err != nil {
				t.Fatal(err)
			}
		} else {
			rec, _ := st.ForPlayer(x, "")
			ui, _ := get(rec, "UserInfo").(pyjson.Obj)
			if _, err := st.Edit(x, []Set{{K: firstFloat(ui), V: 1e9}}, nil); err != nil {
				t.Fatal(err)
			}
		}
	}
	good := addPlayers(t, st, 0x8100, 30)
	me, cached := good[0], []int64{good[1], good[2]}
	skill := int64(2)
	seen := map[int64]bool{}
	for round := 0; round < 12; round++ {
		recs, err := st.Pool(11, &skill, nil, me, cached, true)
		if err != nil {
			t.Fatal(err)
		}
		if len(recs) != 11 {
			t.Fatalf("round %d: %d records, want 11", round, len(recs))
		}
		dup := map[int64]bool{}
		for _, r := range recs {
			x := asInt(get(r, "Xuid"))
			switch {
			case bot[x]:
				t.Errorf("a shipped Drivatar %#x in a store of 30 players", x)
			case x < 0x8100:
				t.Errorf("unusable row %#x served", x)
			case x == me || x == cached[0] || x == cached[1]:
				t.Errorf("excluded %#x served", x)
			case dup[x]:
				t.Errorf("%#x twice in one grid", x)
			}
			dup[x] = true
			seen[x] = true
			if asInt(get(r, "UserSkillLevel")) != skill {
				t.Errorf("%#x has skill %v, want %d", x, get(r, "UserSkillLevel"), skill)
			}
			if f, _ := get(r, "IsFriend").(bool); f {
				t.Errorf("%#x marked a friend with no friends", x)
			}
		}
	}
	if len(seen) <= 11 {
		t.Errorf("12 grids drew from only %d players: always the same ones", len(seen))
	}
	if recs, err := st.Pool(0, &skill, nil, me, nil, true); err != nil || len(recs) != 0 {
		t.Errorf("limit 0: %d records, err %v", len(recs), err)
	}
}

// Fewer players than the grid: the players first, then shipped Drivatars of the skill, still exactly as many as asked, nobody twice.
func TestRaceTopsUpWithShipped(t *testing.T) {
	st, _, bot := playersStore(t)
	players := addPlayers(t, st, 0x9000, 4)
	for skill := int64(0); skill < NumSkills; skill++ {
		recs, err := st.Pool(11, &skill, nil, players[0], nil, true)
		if err != nil {
			t.Fatal(err)
		}
		if len(recs) != 11 {
			t.Fatalf("skill %d: %d records, want 11", skill, len(recs))
		}
		nPlayers, dup := 0, map[string]bool{} // the shipped Drivatars all have Xuid 0: they differ by name
		for _, r := range recs {
			x := asInt(get(r, "Xuid"))
			key := fmt.Sprintf("%d/%v", x, get(r, "GamerTag"))
			if dup[key] {
				t.Errorf("%s twice", key)
			}
			dup[key] = true
			if !bot[x] {
				nPlayers++
			}
			if asInt(get(r, "UserSkillLevel")) != skill {
				t.Errorf("skill %d grid has %#x at skill %v", skill, x, get(r, "UserSkillLevel"))
			}
		}
		if nPlayers != 3 {
			t.Errorf("skill %d: %d players in the grid, want the 3 others", skill, nPlayers)
		}
	}
}

// Friends come first and are marked; a friend without a stored Drivatar is skipped; the cache puts them in every level.
func TestFriendsFirst(t *testing.T) {
	st, _, _ := playersStore(t)
	all := addPlayers(t, st, 0xa000, 40)
	me := all[0]
	friends := map[int64]bool{all[7]: true, all[21]: true, all[33]: true}
	st.FriendsOf = func(x int64) ([]int64, error) {
		if x != me {
			t.Errorf("friends asked for %#x", x)
		}
		return []int64{all[7], 0xdead, all[21], all[33], me}, nil
	}
	recs, err := st.Pool(11, nil, nil, me, []int64{all[33]}, true)
	if err != nil {
		t.Fatal(err)
	}
	if len(recs) != 11 {
		t.Fatalf("%d records, want 11", len(recs))
	}
	for i, r := range recs {
		x := asInt(get(r, "Xuid"))
		f, _ := get(r, "IsFriend").(bool)
		if i < 2 && (!friends[x] || !f) {
			t.Errorf("place %d is %#x (IsFriend %v), want one of the two friends not cached", i, x, f)
		}
		if i >= 2 && (friends[x] || f) {
			t.Errorf("place %d is %#x (IsFriend %v): a friend after the others, or a cached one", i, x, f)
		}
	}
	cache, err := st.ForCache(120, me)
	if err != nil {
		t.Fatal(err)
	}
	firstAt := map[int64]int{}
	for i, r := range cache {
		x, skill := asInt(get(r, "Xuid")), asInt(get(r, "UserSkillLevel"))
		if f, _ := get(r, "IsFriend").(bool); f != friends[x] {
			t.Errorf("cache: %#x IsFriend %v", x, f)
		}
		if x == me {
			t.Errorf("cache serves the requesting player")
		}
		if friends[x] {
			if _, ok := firstAt[skill]; !ok {
				firstAt[skill] = i
			}
		}
	}
	for skill := int64(0); skill < NumSkills; skill++ {
		if want := int(skill) * CacheCapacity / NumSkills; firstAt[skill] != want {
			t.Errorf("skill %d: first friend at %d, want the start of the level (%d)", skill, firstAt[skill], want)
		}
	}
}

// Every difficulty gets a cache level, with few players and with many; many players spread over the levels without repeats.
func TestCacheEverySkill(t *testing.T) {
	st, _, bot := playersStore(t)
	addPlayers(t, st, 0xb000, 7)
	cache, err := st.ForCache(120, 0)
	if err != nil {
		t.Fatal(err)
	}
	per := map[int64]int{}
	for _, r := range cache {
		if bot[asInt(get(r, "Xuid"))] {
			t.Errorf("shipped Drivatar in a players' cache")
		}
		per[asInt(get(r, "UserSkillLevel"))]++
	}
	for skill := int64(0); skill < NumSkills; skill++ {
		if per[skill] != 7 {
			t.Errorf("7 players: skill %d has %d records, want all 7", skill, per[skill])
		}
	}
	addPlayers(t, st, 0xc000, 200)
	if cache, err = st.ForCache(120, 0); err != nil {
		t.Fatal(err)
	}
	distinct := map[int64]bool{}
	per = map[int64]int{}
	for _, r := range cache {
		distinct[asInt(get(r, "Xuid"))] = true
		per[asInt(get(r, "UserSkillLevel"))]++
	}
	if len(cache) != 120 || len(distinct) != 120 {
		t.Errorf("207 players: %d records, %d distinct, want 120 and 120", len(cache), len(distinct))
	}
	for skill := int64(0); skill < NumSkills; skill++ {
		if per[skill] != 24 {
			t.Errorf("207 players: skill %d has %d records, want 24", skill, per[skill])
		}
	}
}

// With the game's car list (the embedded catalog, as the LSP sets it) a player's Drivatar drives their latest known car with their own shared
// livery and tune for it; a car pack car (not in the list) and a withdrawn livery are left out.
func TestCarsFromTheCatalog(t *testing.T) {
	if os.Getenv("FH2MP_DRIVATAR_CARS") == "" && !Cars {
		t.Errorf("FH2MP_DRIVATAR_CARS unset: cars are off, want on by default")
	}
	st, d, _ := playersStore(t)
	Cars = true
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	st.KnownCars = cat.KnownCars
	known := cat.SortedKnown()
	carA, carB := int64(known[0]), int64(known[1])
	if _, ok := cat.KnownCars[9024]; ok {
		t.Fatal("test car pack id 9024 is a known car")
	}
	ps := addPlayers(t, st, 0xd000, 4)
	submit := func(x, car int64, at float64) {
		id := make([]byte, 16)
		rand.Read(id)
		if _, err := d.Exec("INSERT INTO score_submissions (xuid, sb, st, gsid, track, car, cls, cpi, score, clean, stm, tcs, abs, autobrake, autoshift, clutch, submitted, score_id) "+
			"VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", x, 1, 1, 0, 1001, car, 3, 0.7, 100.0, 1, 0, 0, 0, 0, 0, 0, at, id); err != nil {
			t.Fatal(err)
		}
	}
	share := func(x, car int64, kind string, refs int64, at float64) int64 {
		r, err := d.One("INSERT INTO ugc_versioned (kind, car_id, name, creator, layers, data, thumb, thumb_small, refs, created) VALUES (?,?,?,?,?,?,?,?,?,?) RETURNING id",
			kind, car, "x", x, 0, []byte{}, []byte{}, []byte{}, refs, at)
		if err != nil {
			t.Fatal(err)
		}
		return r.IntAt(0)
	}
	submit(ps[1], carB, 1)
	submit(ps[1], carA, 2) // the latest
	livery := share(ps[1], carA, "livery", 1, 3)
	share(ps[1], carA, "livery", 0, 4) // withdrawn: not named even though newer
	tune := share(ps[1], carA, "tune", 2, 5)
	submit(ps[2], 9024, 3) // a car pack car
	recs, err := st.Pool(3, nil, nil, ps[0], nil, true)
	if err != nil {
		t.Fatal(err)
	}
	for _, r := range recs {
		switch asInt(get(r, "Xuid")) {
		case ps[1]:
			if asInt(get(r, "CarId")) != carA || asInt(get(r, "LiveryId")) != livery || asInt(get(r, "TuneId")) != tune {
				t.Errorf("owner's car: CarId %v LiveryId %v TuneId %v, want %d %d %d", get(r, "CarId"), get(r, "LiveryId"), get(r, "TuneId"), carA, livery, tune)
			}
		case ps[2]:
			if asInt(get(r, "CarId")) == 9024 {
				t.Errorf("a car pack car reached another player's grid")
			}
		}
	}
}

// paceOf is a record's UserInfo.base.timeRatioMean.
func paceOf(r Record) float64 {
	ui, _ := get(r, "UserInfo").(pyjson.Obj)
	base, _ := get(ui, "base").(pyjson.Obj)
	return asFloat(get(base, "timeRatioMean"))
}

// A player's record served at a skill level drives at that level's pace (the band of the game's own Drivatars), at the player's place in it; the
// stored record keeps the player's own pace, and the same player in two levels gets two paces.
func TestPaceFollowsTheLevel(t *testing.T) {
	st, _, _ := playersStore(t)
	paces := map[int64]float64{0xd000: 0, 0xd001: 0.661, 0xd002: 1, 0xd003: 0.25}
	for x, p := range paces {
		addPlayers(t, st, x, 1)
		if _, err := st.Edit(x, []Set{{K: "timeRatioMean", V: p}}, nil); err != nil {
			t.Fatal(err)
		}
	}
	inBand := func(where string, r Record) {
		t.Helper()
		x, skill := asInt(get(r, "Xuid")), asInt(get(r, "UserSkillLevel"))
		own, ok := paces[x]
		if !ok {
			return
		}
		want := skillBand*float64(skill) + skillBandSpan*own
		if got := paceOf(r); got < want-1e-6 || got > want+1e-6 || got < skillBand*float64(skill)-1e-6 || got > skillBand*float64(skill)+skillBandSpan+1e-6 {
			t.Errorf("%s: %#x (own pace %.3f) at skill %d has pace %.4f, want %.4f", where, x, own, skill, got, want)
		}
	}
	cache, err := st.ForCache(120, 0)
	if err != nil {
		t.Fatal(err)
	}
	levels := map[int64]map[int64]bool{}
	for _, r := range cache {
		inBand("cache", r)
		x := asInt(get(r, "Xuid"))
		if levels[x] == nil {
			levels[x] = map[int64]bool{}
		}
		levels[x][asInt(get(r, "UserSkillLevel"))] = true
	}
	if len(levels[0xd001]) != NumSkills {
		t.Errorf("0xd001 is in %d levels of the cache, want %d", len(levels[0xd001]), NumSkills)
	}
	for skill := int64(0); skill < NumSkills; skill++ {
		recs, err := st.Pool(11, &skill, nil, 0x1, nil, true)
		if err != nil {
			t.Fatal(err)
		}
		for _, r := range recs {
			inBand(fmt.Sprintf("grid %d", skill), r)
		}
	}
	for x, p := range paces {
		rec, err := st.ForPlayer(x, "")
		if err != nil {
			t.Fatal(err)
		}
		if got := paceOf(rec); got < p-1e-6 || got > p+1e-6 {
			t.Errorf("stored %#x has pace %.4f after serving, want its own %.4f", x, got, p)
		}
	}
}
