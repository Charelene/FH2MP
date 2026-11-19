package lsp

import (
	"compress/gzip"
	enchex "encoding/hex"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"strings"
	"testing"

	"fh2mp/internal/pyjson"
)

// ghostBlob is the first readable stored ghost of the ghost package's fixture.
func ghostBlob(t *testing.T) []byte {
	t.Helper()
	f, err := os.Open("../../testdata/ghost.json.gz")
	if err != nil {
		t.Skip(err)
	}
	defer f.Close()
	zr, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	var fx struct {
		Cases []struct {
			Blob  string `json:"blob"`
			Parse struct {
				Error *string `json:"error"`
			} `json:"parse"`
		} `json:"cases"`
	}
	if err := json.NewDecoder(zr).Decode(&fx); err != nil {
		t.Fatal(err)
	}
	for _, c := range fx.Cases {
		if c.Parse.Error == nil && c.Blob != "" {
			b, _ := enchex.DecodeString(c.Blob)
			return b
		}
	}
	t.Skip("no readable ghost in the fixture")
	return nil
}

func flatGrid(n int, speed, accel float64) *grid {
	g := &grid{}
	for k := 0; k < n; k++ {
		g.t = append(g.t, float64(k)*0.5)
		g.speed = append(g.speed, speed+0.1*float64(k%3))
		g.accel = append(g.accel, accel)
	}
	return g
}

// A stretch is road where the run is above every other driver for about 60 m or more: a surge of acceleration and a burst of speed are found
// where they are, a one-point spike is not, and nothing is judged against fewer than two other drivers.
func TestStretchesMarkWhereARunBeatsEveryoneOnTheRoad(t *testing.T) {
	others := []*grid{flatGrid(50, 40, 1.0), flatGrid(50, 40.5, 1.2), flatGrid(50, 39.5, 0.8)}
	run := flatGrid(50, 40, 1.0)
	for k := 20; k <= 24; k++ {
		run.accel[k] = 4.0 // a surge, 100 m long
	}
	run.accel[10] = 5.0 // one point: a bump, not a stretch
	for k := 30; k <= 33; k++ {
		run.speed[k] = 50 // 10 m/s over everyone, 80 m long
	}
	got := stretchesOf(run, others)
	if len(got) != 2 {
		t.Fatalf("found %d stretches, want the surge and the burst: %+v", len(got), got)
	}
	a, s := got[0], got[1]
	if a.kind != "accel" || a.from != 400 || a.to != 480 || a.value != 4.0 || a.top != 1.2 || a.t != 10 {
		t.Errorf("surge: %+v", a)
	}
	if s.kind != "speed" || s.from != 600 || s.to != 660 || s.value != 50 || math.Abs(s.med-40.1) > 0.2 {
		t.Errorf("burst: %+v", s)
	}
	if got := stretchesOf(run, others[:1]); len(got) != 0 {
		t.Errorf("judged against one other driver: %+v", got)
	}
}

// The review of one event: the open runs, suspects first (here the run the checker flagged), the others in finishing order; runs already
// hidden or cleared apart; the comparison is the same car at the same PI when at least two others share it.
func TestRivalsReviewSplitsOpenAndDecided(t *testing.T) {
	f := newReviewFixture(t)
	fx := ghostBlob(t)
	flagged := f.ids["flagged"]
	exec := func(q string, a ...any) {
		t.Helper()
		if _, err := f.o.C.DB.Exec(q, a...); err != nil {
			t.Fatalf("%s: %v", q, err)
		}
	}
	exec("INSERT INTO score_ghosts (submission_id, data, uploaded) VALUES (?, ?, 1.7e9)", flagged, fx)
	exec("UPDATE rival_events SET length = 0 WHERE id = 65000") // the fixture's ghost is short: the route length comes from the others
	add := func(xuid int64, score float64) int64 {
		r := must(f.o.C.DB.One("INSERT INTO score_submissions (xuid, sb, st, gsid, track, car, cls, cpi, score, clean, stm, tcs, abs, submitted) "+
			"VALUES (?, 3, 1, 65000, 0, 0, 3, 600, ?, 1, 0, 0, 0, 1.7e9) RETURNING id", xuid, score))
		exec("INSERT INTO score_ghosts (submission_id, data, uploaded) VALUES (?, ?, 1.7e9)", r.Int("id"), fx)
		exec("INSERT INTO score_verdicts (submission_id, version, verdict, reasons, measurements, decided_at) VALUES (?, 1, 'pass', '[]', '{}', 1)", r.Int("id"))
		return r.Int("id")
	}
	fast, mid, slow := add(0x7101, 90), add(0x7102, 91), add(0x7103, 99)
	hidden, cleared := add(0x7104, 80), add(0x7105, 92)
	exec("INSERT INTO hidden_scores (submission_id, hidden_at, source) VALUES (?, 1, 'operator')", hidden)
	exec("UPDATE score_verdicts SET reviewed = 1 WHERE submission_id = ?", cleared)

	st, obj, _ := f.o.route(nil, "GET", "/ops/v1/rivals/events/65000/review", nil)
	out := obj.(pyjson.Obj)
	if st != 200 || dump(field(out, "counts")) != `{"open": 4, "decided": 2}` {
		t.Fatalf("review: %d %s", st, dump(field(out, "counts")))
	}
	var order []string
	for _, c := range field(out, "open").([]any) {
		o := c.(pyjson.Obj)
		order = append(order, fmt.Sprint(field(o, "submissionId"), "/", field(o, "suspect")))
		if l := field(field(o, "compared").(pyjson.Obj), "label"); l != "same car and PI" {
			t.Errorf("run %v compared with %v", field(o, "submissionId"), l)
		}
	}
	if want := fmt.Sprintf("%d/true %d/false %d/false %d/false", flagged, fast, mid, slow); strings.Join(order, " ") != want {
		t.Fatalf("open order %v, want %s (the flagged run first, then finishing order)", order, want)
	}
	decided := map[int64]string{}
	for _, c := range field(out, "decided").([]any) {
		o := c.(pyjson.Obj)
		decided[field(o, "submissionId").(int64)] = field(o, "decided").(string)
	}
	if decided[hidden] != "Hidden by an admin" || decided[cleared] != "Cleared by an admin" {
		t.Fatalf("decided: %v", decided)
	}
	if st, code := opsCall(t, f.o, "GET", "/ops/v1/rivals/events/123/review", ""); st != 404 || code != "not_found" {
		t.Fatalf("an unknown event answered %d %s", st, code)
	}
}
