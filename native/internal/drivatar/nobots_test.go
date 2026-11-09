package drivatar

import (
	"fmt"
	"testing"

	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
	"fh2mp/internal/pyjson"
)

// Without FH2MP_DRIVATAR_BOTS no shipped Drivatar (named after a real racing driver) reaches a race or the cache once players exist; every skill
// level of the cache is still filled, from players' records; a nameless player does not inherit a template's driver name.
func TestNoBotsOnceThereArePlayers(t *testing.T) {
	d, err := db.Open(dbtest.New(t, "fh2mp_cat"))
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	oldBots, oldPlayers := Bots, CachePlayers
	defer func() { Bots, CachePlayers = oldBots, oldPlayers }()
	Bots, CachePlayers = false, true
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
	if len(shipped) == 0 {
		t.Skip("no shipped Drivatars in the test catalog")
	}
	st := New(shipped, d)
	bot := map[int64]bool{}
	for _, s := range shipped {
		bot[asInt(get(s, "Xuid"))] = true
	}

	// a store with no player still serves the shipped records (a fresh server is not empty)
	if recs, err := st.ForCache(120, 0); err != nil || len(recs) == 0 {
		t.Fatalf("empty store: %d cache records, err %v", len(recs), err)
	}

	for i := 0; i < 80; i++ {
		if _, err := st.ForPlayer(int64(0x7000+i), fmt.Sprintf("Player%d", i)); err != nil {
			t.Fatal(err)
		}
	}
	nameless, err := st.ForPlayer(0x7fff, "")
	if err != nil {
		t.Fatal(err)
	}
	if tag, _ := get(nameless, "GamerTag").(string); tag != "" {
		t.Errorf("a nameless player's record carries the template name %q", tag)
	}

	cache, err := st.ForCache(120, 0)
	if err != nil {
		t.Fatal(err)
	}
	if len(cache) != 80 {
		t.Errorf("cache has %d records, want the 80 named players", len(cache))
	}
	perSkill := map[int64]int{}
	for _, r := range cache {
		x := asInt(get(r, "Xuid"))
		if bot[x] || x == 0x7fff {
			t.Errorf("cache serves %v (%v)", x, get(r, "GamerTag"))
		}
		perSkill[asInt(get(r, "UserSkillLevel"))]++
	}
	for skill := int64(0); skill < NumSkills; skill++ {
		if perSkill[skill] < CacheShippedPerSkill {
			t.Errorf("skill %d has %d records in the cache, want at least %d", skill, perSkill[skill], CacheShippedPerSkill)
		}
	}

	pool, err := st.Pool(40, nil, nil, 0x7000, nil, true)
	if err != nil {
		t.Fatal(err)
	}
	if len(pool) != 40 {
		t.Errorf("pool has %d records, want 40", len(pool))
	}
	for _, r := range pool {
		if x := asInt(get(r, "Xuid")); bot[x] || x == 0x7fff || x == 0x7000 {
			t.Errorf("pool serves %v (%v)", x, get(r, "GamerTag"))
		}
	}

	Bots = true
	if cache, _ = st.ForCache(120, 0); len(cache) != 120 {
		t.Errorf("with bots the cache has %d records, want 120", len(cache))
	}
}
