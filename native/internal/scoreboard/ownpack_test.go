package scoreboard

import (
	"strings"
	"testing"

	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
)

// A game with its own car database (a player's car packs): the server cannot see its parts or car data, so it takes a time run of a catalog car and
// judges the ghost strictly, whatever the ghost policy. Everything else from such a game stays refused.
func TestOwnCarsRuns(t *testing.T) {
	d, err := db.Open(dbtest.New(t, ""))
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	oldNow := Now
	clock := 5000.0
	Now = func() float64 { clock++; return clock }
	defer func() { Now = oldNow }()
	st := New(d, nil, nil)
	st.GhostPolicy = "log" // the strict check does not depend on it
	st.KnownCars = func() map[int64]bool { return map[int64]bool{1010: true, 6: true} }
	must := func(err error) {
		t.Helper()
		if err != nil {
			t.Fatal(err)
		}
	}
	own := func(s *Submission) *Submission { s.OwnCars = true; return s }
	visible := func(id int64) bool {
		t.Helper()
		v, err := st.IsVisible(id)
		must(err)
		return v
	}
	hiddenRow := func(id int64) (string, string) {
		t.Helper()
		rows, err := d.Query("SELECT source, reason FROM hidden_scores WHERE submission_id=?", id)
		must(err)
		if len(rows) == 0 {
			return "", ""
		}
		return rows[0].Str("source"), rows[0].Str("reason")
	}

	// a time run of a catalog car is taken, but waits for its ghost, hidden from everybody except its owner
	ok, err := st.Submit(0x100, "Sara", 1, own(sub(1, 110.0)))
	must(err)
	if ok == nil {
		t.Fatal("a time run of a catalog car from a game with its own car database was refused")
	}
	if src, _ := hiddenRow(ok.ID); src != "pending" || visible(ok.ID) {
		t.Fatalf("before its ghost the run is pending and hidden, got source %q visible %v", src, visible(ok.ID))
	}
	mine, err := st.BestOf(0x100, 1, 1, 1001, 3)
	must(err)
	if mine == nil || mine.Score != 110.0 {
		t.Fatalf("the owner does not see the run that waits for its ghost: %+v", mine)
	}
	top, err := st.Top(1, 1, 1001, 3, 10)
	must(err)
	if len(top) != 0 {
		t.Fatalf("a run without its ghost is on the public board: %d rows", len(top))
	}

	// a clean ghost of that car makes it public, although the policy is only "log"
	id, public, err := st.AttachGhostAs(ok.ScoreID, makeGhost(110, 60, 110, 1010, 5), 0x100, true)
	must(err)
	if id != ok.ID || !public || !visible(ok.ID) {
		t.Fatalf("a clean ghost left the run hidden: id %d public %v visible %v", id, public, visible(ok.ID))
	}
	top, err = st.Top(1, 1, 1001, 3, 10)
	must(err)
	if len(top) != 1 || top[0].Xuid != 0x100 {
		t.Fatalf("the run is not on the public board: %d rows", len(top))
	}

	// a ghost that is only flagged (here: another car in its header) hides the run of such a game, but not that of a game with the server's database
	flagged := makeGhost(110, 60, 110, 999, 5)
	pack, err := st.Submit(0x200, "Pack", 1, own(sub(1, 110.0)))
	must(err)
	_, public, err = st.AttachGhostAs(pack.ScoreID, flagged, 0x200, true)
	must(err)
	if src, why := hiddenRow(pack.ID); public || visible(pack.ID) || src != "auto" || !strings.HasPrefix(why, ownCarsFlagReason) {
		t.Fatalf("a flagged ghost of a game with its own car database must hide the run: public %v source %q reason %q", public, src, why)
	}
	st.GhostPolicy = "enforce"
	plain, err := st.Submit(0x300, "Plain", 1, sub(1, 110.0))
	must(err)
	_, public, err = st.AttachGhostAs(plain.ScoreID, flagged, 0x300, false)
	must(err)
	if !public || !visible(plain.ID) {
		t.Fatalf("the same flagged ghost from a game with the server's database stays public (flags go to the report queue): public %v", public)
	}
	st.GhostPolicy = "log"

	// an impossible ghost is hidden even though the policy is "log"
	fast, err := st.Submit(0x400, "Fast", 1, own(sub(1, 110.0)))
	must(err)
	_, public, err = st.AttachGhostAs(fast.ScoreID, makeGhost(110, 140, 110, 1010, 40), 0x400, true)
	must(err)
	if public || visible(fast.ID) {
		t.Fatalf("an impossible ghost of a game with its own car database was shown")
	}

	// what has no ghost to judge, or a car the catalog lacks, is refused
	for name, s := range map[string]*Submission{
		"speed board":   own(sub(9, 80.0, gsid(3), car(1010))),
		"counter board": own(sub(4, 5.0, track(0))),
		"experience":    own(sub(2, 5000.0, track(0))),
		"pack car":      own(sub(1, 110.0, car(5000))),
		"event, pack":   own(sub(3, 110.0, gsid(500), car(5000))),
		"no car":        own(sub(1, 110.0, car(0))),
	} {
		typ := int64(1)
		if s.ScoreboardType == 9 {
			typ = 5
		} else if s.ScoreboardType == 4 {
			typ = 9
		} else if s.ScoreboardType == 2 {
			typ = 3
		}
		got, err := st.Submit(0x500, "Refused", typ, s)
		must(err)
		if got != nil {
			t.Errorf("%s: a score from a game with its own car database was stored", name)
		}
		if why := st.Refusal(typ, s, U64ToDbl(s.Score)); why == "" {
			t.Errorf("%s: Refusal is empty", name)
		}
	}

	// without a catalog the car cannot be checked: refused, while a game with the server's database is not affected
	st.KnownCars = nil
	if got, err := st.Submit(0x600, "NoCatalog", 1, own(sub(1, 110.0))); err != nil || got != nil {
		t.Errorf("without a loaded catalog a game with its own car database was taken: %v %v", got, err)
	}
	if got, err := st.Submit(0x600, "NoCatalog", 1, sub(1, 110.0)); err != nil || got == nil {
		t.Errorf("without a loaded catalog a game with the server's database was refused: %v %v", got, err)
	}
}
