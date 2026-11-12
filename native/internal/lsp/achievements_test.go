package lsp

import (
	"testing"

	"fh2mp/internal/dbtest"
	"fh2mp/internal/pyjson"
)

// The Guide reports what the Achievements mod's unlock file holds, again and again (every change of the file, every start). An unlock is
// kept once with the time it first came with, ids the game does not have are not kept, and only a player who reported has a Gamerscore.
func TestReportedAchievementsAreKeptOnceAndGiveAGamerscore(t *testing.T) {
	t.Setenv("FH2MP_ENV", "dev")
	c := New()
	c.Out = func(string) {}
	if err := c.Setup(dbtest.New(t, "fh2mp_cat")); err != nil {
		t.Fatal(err)
	}
	defer c.DB.Close()
	l := &Launcher{C: c}
	report := func(unlocked pyjson.Obj) (count, score int64) {
		t.Helper()
		v, err := l.reportAchievements(0x10, pyjson.Obj{{K: "unlocked", V: unlocked}})
		if err != nil {
			t.Fatal(err)
		}
		o := v.(pyjson.Obj)
		cv, _ := o.Get("count")
		gv, _ := o.Get("gamerscore")
		return cv.(int64), gv.(int64)
	}
	if n, g := report(pyjson.Obj{{K: "welcome_festival", V: "2026-10-20 18:02:11 UTC"}, {K: "made_up", V: "2026-10-20 18:02:11 UTC"}}); n != 1 || g != 10 {
		t.Fatalf("first report: %d unlocked for %dG, want 1 for 10G", n, g)
	}
	// the same achievement with a later time, and a new one
	if n, g := report(pyjson.Obj{{K: "welcome_festival", V: "2026-11-01 00:00:00 UTC"}, {K: "welcome_sisteron", V: "2026-10-21 19:00:00 UTC"}}); n != 2 || g != 35 {
		t.Fatalf("second report: %d unlocked for %dG, want 2 for 35G", n, g)
	}
	ids, when, err := l.achievementsOf(0x10)
	if err != nil || len(ids) != 2 || ids[0] != "welcome_festival" {
		t.Fatalf("kept %v, %v", ids, err)
	}
	if first, _ := when.Get("welcome_festival"); first != "2026-10-20 18:02:11 UTC" {
		t.Fatalf("the first unlock's time became %v", first)
	}
	scores, err := l.gamerscores([]int64{0x10, 0x20})
	if err != nil || scores[0x10] != 35 {
		t.Fatalf("Gamerscore of the reporter: %v, %v", scores, err)
	}
	if _, has := scores[0x20]; has {
		t.Fatalf("a player who reported nothing has a Gamerscore: %v", scores)
	}
	if _, err := l.reportAchievements(0x10, pyjson.Obj{{K: "unlocked", V: "everything"}}); errCode(err) != "bad_request" {
		t.Fatalf("a report that is not a list of unlocks passed: %v", err)
	}
}

// The share of players who have an achievement: of the players who report achievements; the dev stack's phantom players only widen
// the base (so two test accounts can see a rare toast).
func TestRarityIsTheShareOfReportingPlayers(t *testing.T) {
	share := func(o pyjson.Obj, id string) float64 {
		pv, _ := o.Get("percent")
		v, _ := pv.(pyjson.Obj).Get(id)
		f, _ := v.(float64)
		return f
	}
	r := rarityOf(map[string]int64{"welcome_festival": 40, "horizon_leader": 3}, 40, 0)
	if n, _ := r.Get("players"); n != int64(40) {
		t.Fatalf("players %v", n)
	}
	if got := share(r, "welcome_festival"); got != 100 {
		t.Errorf("everybody has it: %v%%", got)
	}
	if got := share(r, "horizon_leader"); got != 7.5 {
		t.Errorf("3 of 40: %v%%, want 7.5", got)
	}
	if got := share(r, "welcome_nice"); got != 0 {
		t.Errorf("nobody has it: %v%%", got)
	}
	if got := share(rarityOf(map[string]int64{"horizon_leader": 2}, 2, 100), "horizon_leader"); got != 2 {
		t.Errorf("2 of 2 with 100 phantom players: %v%%, want 2", got)
	}
	if got := share(rarityOf(nil, 0, 0), "horizon_leader"); got != 0 {
		t.Errorf("no players: %v%%", got)
	}
}
