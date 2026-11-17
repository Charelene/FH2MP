package lsp

import (
	"compress/gzip"
	enchex "encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"sort"
	"strings"
	"testing"

	"fh2mp/internal/dbtest"
	"fh2mp/internal/pyjson"
)

// reviewFixture is a database with one run of every kind the review queue meets.
type reviewFixture struct {
	o     *Ops
	route string // the Festival Route name of run "reported3"
	ids   map[string]int64
}

func newReviewFixture(t *testing.T) *reviewFixture {
	t.Helper()
	t.Setenv("FH2MP_ENV", "dev")
	c := New()
	c.Out = func(string) {}
	if err := c.Setup(dbtest.New(t, "fh2mp_cat")); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.DB.Close() })
	names := must(c.Riv.Names())
	var track int64
	var route string
	keys := make([]int64, 0, len(names.Track))
	for id := range names.Track {
		keys = append(keys, id)
	}
	sort.Slice(keys, func(i, j int) bool { return keys[i] < keys[j] })
	for _, id := range keys {
		if n := names.Track[id][0]; n != "" {
			track, route = id, n
			break
		}
	}
	if route == "" {
		t.Skip("the catalog has no named routes")
	}
	exec := func(q string, args ...any) {
		t.Helper()
		if _, err := c.DB.Exec(q, args...); err != nil {
			t.Fatalf("%s: %v", q, err)
		}
	}
	exec("INSERT INTO rival_events (id, month, reroll, idx, track, laps, length, kind, attrs, name, trackname, description, created) VALUES (65000, 10, 0, 0, ?, 1, 5000, 'race', '{}', 'Night Sprint', 'Castelletto', '', 1)", track)
	f := &reviewFixture{o: NewOps(c, nil), route: route, ids: map[string]int64{}}
	run := func(name string, sb, st, gsid int64) int64 {
		t.Helper()
		r := must(c.DB.One("INSERT INTO score_submissions (xuid, sb, st, gsid, track, car, cls, cpi, score, clean, stm, tcs, abs, submitted) "+
			"VALUES (?, ?, ?, ?, ?, 0, 3, 600, 95.5, 1, 0, 0, 0, ?) RETURNING id", 0x9000+int64(len(f.ids)), sb, st, gsid, track, 1.7e9+float64(len(f.ids))))
		f.ids[name] = r.Int("id")
		return f.ids[name]
	}
	report := func(id int64, n int, dismissed int) {
		for i := 0; i < n; i++ {
			exec("INSERT INTO score_reports (submission_id, browser, ip, created, dismissed) VALUES (?, ?, ?, ?, ?)", id, fmt.Sprintf("b%d-%d", id, i), fmt.Sprintf("ip%d-%d", id, i), 1.7e9+float64(i), dismissed)
		}
	}
	verdict := func(id int64, kind, reason string, reviewed int) {
		exec("INSERT INTO score_verdicts (submission_id, version, verdict, reasons, measurements, decided_at, reviewed) VALUES (?, 1, ?, ?, '{}', 1.7e9, ?)",
			id, kind, `["`+reason+`"]`, reviewed)
	}
	hide := func(id int64, source, reason string) {
		exec("INSERT INTO hidden_scores (submission_id, hidden_at, source, reason) VALUES (?, ?, ?, ?)", id, 1.7e9+float64(id), source, reason)
	}
	report(run("reported3", 1, 1, 3), 3, 0)
	verdict(run("flagged", 3, 1, 65000), "flag", "speed 125.3 is above 120", 0)
	id := run("hiddenByChecker", 1, 1, 3)
	verdict(id, "hard", "speed 130.0 is above 127.4", 0)
	hide(id, "auto", "speed 130.0 is above 127.4")
	hide(run("hiddenCar", 9, 5, 12), "auto", "car not in the game catalog")
	id = run("hiddenByAdmin", 1, 1, 3)
	hide(id, "operator", "")
	report(id, 1, 0)
	verdict(run("flaggedThenCleared", 1, 1, 3), "flag", "acceleration 21.0 is above 20", 1)
	hide(run("waitingForGhost", 1, 1, 3), "pending", "no ghost yet")
	report(run("reportsDismissed", 1, 1, 3), 2, 1)
	id = run("reportedAndFlagged", 6, 1, 77)
	report(id, 1, 0)
	verdict(id, "flag", "power 750.0 is above 700", 0)
	return f
}

func (f *reviewFixture) queue(t *testing.T, query string) (ids []string, rows map[string]pyjson.Obj, counts pyjson.Obj) {
	t.Helper()
	st, obj, _ := f.o.route(nil, "GET", "/ops/v1/reports"+query, nil)
	if st != 200 {
		t.Fatalf("GET %s: %d", query, st)
	}
	out := obj.(pyjson.Obj)
	byID := map[int64]string{}
	for name, id := range f.ids {
		byID[id] = name
	}
	rows = map[string]pyjson.Obj{}
	entries, _ := out.Get("entries")
	for _, e := range entries.([]any) {
		row := e.(pyjson.Obj)
		sid, _ := row.Get("submissionId")
		name := byID[sid.(int64)]
		ids = append(ids, name)
		rows[name] = row
	}
	c, _ := out.Get("counts")
	return ids, rows, c.(pyjson.Obj)
}

func field(o pyjson.Obj, k string) any { v, _ := o.Get(k); return v }

func dump(v any) string { s, _ := pyjson.Dumps(v, pyjson.Opts{}); return s }

// Each run is in exactly one view (reported, flagged, hidden) or in none, and its row says where it came from in plain words: the board by
// name, the report count, the checker's reason in km/h, who hid it.
func TestReviewQueueSortsEveryRunIntoOneViewAndSaysWhy(t *testing.T) {
	f := newReviewFixture(t)

	reported, rows, counts := f.queue(t, "?view=reported")
	if strings.Join(reported, ",") != "reported3,reportedAndFlagged" {
		t.Fatalf("reported view: %v, want the most reported first and only runs still on the boards", reported)
	}
	if want := `{"reported": 2, "flagged": 1, "hidden": 3, "hiddenBy": {"admin": 1, "checker": 1, "car": 1}}`; dump(counts) != want {
		t.Fatalf("counts %s, want %s", dump(counts), want)
	}
	r := rows["reported3"]
	if field(r, "board") != "Festival Route: "+f.route || field(r, "source") != "Reported 3 times on the website" || field(r, "reports") != int64(3) {
		t.Fatalf("reported row says %s", dump(r))
	}
	r = rows["reportedAndFlagged"]
	if field(r, "board") != "Showcase event 77" || field(r, "source") != "Reported once on the website" ||
		dump(field(r, "why")) != `["Power 1006 hp per tonne, over the 939 hp per tonne limit"]` {
		t.Fatalf("a reported run the checker also flagged says %s", dump(r))
	}

	flagged, rows, _ := f.queue(t, "?view=flagged")
	if strings.Join(flagged, ",") != "flagged" {
		t.Fatalf("flagged view: %v (a reviewed flag, a run waiting for its ghost and a reported run are not in it)", flagged)
	}
	r = rows["flagged"]
	if field(r, "board") != "Monthly Rivals: Night Sprint" || field(r, "source") != "Flagged by the replay checker" ||
		dump(field(r, "why")) != `["Speed 451 km/h, over the 432 km/h limit"]` {
		t.Fatalf("flagged row says %s", dump(r))
	}

	hidden, rows, _ := f.queue(t, "?view=hidden")
	sort.Strings(hidden)
	if strings.Join(hidden, ",") != "hiddenByAdmin,hiddenByChecker,hiddenCar" {
		t.Fatalf("hidden view: %v", hidden)
	}
	for name, want := range map[string]string{"hiddenByAdmin": "Hidden by an admin", "hiddenByChecker": "Hidden by the replay checker", "hiddenCar": "Hidden automatically: the car is not in the game"} {
		if field(rows[name], "source") != want {
			t.Errorf("%s says %q, want %q", name, field(rows[name], "source"), want)
		}
	}
	if dump(field(rows["hiddenByChecker"], "why")) != `["Speed 468 km/h, over the 459 km/h limit"]` || dump(field(rows["hiddenByAdmin"], "why")) != `["Reported once on the website"]` {
		t.Fatalf("hidden rows' reasons: %s / %s", dump(field(rows["hiddenByChecker"], "why")), dump(field(rows["hiddenByAdmin"], "why")))
	}
	if field(rows["hiddenCar"], "board") != "Speed trap 12" {
		t.Errorf("a speed trap run is on %q", field(rows["hiddenCar"], "board"))
	}
	if cars, _, _ := f.queue(t, "?view=hidden&by=car"); strings.Join(cars, ",") != "hiddenCar" {
		t.Fatalf("hidden by car: %v", cars)
	}

	// The panel's buttons move a run between views: hiding a reported run, clearing a flag, putting a hidden run back.
	for _, step := range []struct{ run, action string }{{"reported3", "hide"}, {"flagged", "dismiss"}, {"hiddenByChecker", "unhide"}} {
		if st, out := opsCall(t, f.o, "POST", fmt.Sprintf("/ops/v1/reports/%d/%s", f.ids[step.run], step.action), ""); st != 200 {
			t.Fatalf("%s %s: %d %s", step.action, step.run, st, out)
		}
	}
	_, _, counts = f.queue(t, "?view=reported")
	if want := `{"reported": 1, "flagged": 0, "hidden": 3, "hiddenBy": {"admin": 2, "checker": 0, "car": 1}}`; dump(counts) != want {
		t.Fatalf("after hide, dismiss and unhide the counts are %s, want %s", dump(counts), want)
	}

	if st, code := opsCall(t, f.o, "GET", "/ops/v1/reports?view=everything", ""); st != 400 || code != "bad_request" {
		t.Fatalf("an unknown view answered %d %s", st, code)
	}
	if st, _ := opsCall(t, f.o, "GET", "/ops/v1/reports?view=reported&by=car", ""); st != 400 {
		t.Fatalf("by= outside the hidden view answered %d", st)
	}
}

// The checker's reasons, as ghost.Check words them, read in km/h, g, hp per tonne and lap time.
func TestPlainReasonUsesDriversUnits(t *testing.T) {
	for raw, want := range map[string]string{
		"speed 125.3 is above 120":                        "Speed 451 km/h, over the 432 km/h limit",
		"acceleration 22.6 is above 20":                   "Acceleration 2.3 g, over the 2.0 g limit",
		"climb 31.0 is above 30":                          "Climbs at 112 km/h straight up, over the 108 km/h limit",
		"power 1200.0 is above 1000":                      "Power 1609 hp per tonne, over the 1341 hp per tonne limit",
		"ghost ends at 83.450 s, the score says 80.000 s": "The replay ends at 1:23.45, the submitted time is 1:20.00",
		"the score is not in the ghost's header":          "The submitted time is not in the replay file",
		"2.5 s missing between frames":                    "The replay has a 2.5 s hole in it",
		"7 jumps across the map":                          "The car jumps across the map 7 times",
		"unreadable ghost: short header":                  "The replay file cannot be read (short header)",
		"car not in the game catalog":                     "The car is not in the game",
	} {
		if got := plainReason(raw); got != want {
			t.Errorf("%q -> %q, want %q", raw, got, want)
		}
	}
}

// The replay and Monthly Rivals review routes exist on this build (prod lost them once when the LSP was rebuilt without them), the replay
// of a stored ghost comes back decoded with its board by name and the checker's own limits, and a run without a ghost says so.
func TestReplayAndRivalsReviewRoutes(t *testing.T) {
	f := newReviewFixture(t)
	fx, err := os.Open("../../testdata/ghost.json.gz")
	if err != nil {
		t.Skip(err)
	}
	defer fx.Close()
	zr, err := gzip.NewReader(fx)
	if err != nil {
		t.Fatal(err)
	}
	var cases struct {
		Cases []struct {
			Blob  string  `json:"blob"`
			Score float64 `json:"score"`
			Parse struct {
				Error *string `json:"error"`
			} `json:"parse"`
		} `json:"cases"`
	}
	if err := json.NewDecoder(zr).Decode(&cases); err != nil {
		t.Fatal(err)
	}
	var blob []byte
	for _, c := range cases.Cases {
		if c.Parse.Error == nil && len(c.Blob) > 0 {
			blob, _ = enchex.DecodeString(c.Blob)
			break
		}
	}
	if blob == nil {
		t.Skip("no readable ghost in the fixture")
	}
	id := f.ids["flagged"]
	if _, err := f.o.C.DB.Exec("INSERT INTO score_ghosts (submission_id, data, uploaded) VALUES (?, ?, 1.7e9)", id, blob); err != nil {
		t.Fatal(err)
	}
	st, obj, _ := f.o.route(nil, "GET", fmt.Sprintf("/ops/v1/runs/%d/ghost", id), nil)
	out := obj.(pyjson.Obj)
	samples, _ := field(out, "samples").(pyjson.Obj).Get("t")
	limits := field(out, "limits").(pyjson.Obj)
	if st != 200 || len(samples.([]any)) == 0 || field(out, "board") != "Monthly Rivals: Night Sprint" || field(out, "source") != "Flagged by the replay checker" ||
		field(limits, "speedHard") != 127.4 || field(limits, "speedFlag") != 120.0 {
		t.Fatalf("replay: %d, board %v, source %v, %d samples, limits %s", st, field(out, "board"), field(out, "source"), len(samples.([]any)), dump(limits))
	}
	// The comparison: other drivers' best visible runs on the same board in the same class, as traces along the distance driven.
	run := must(f.o.C.DB.One("SELECT * FROM score_submissions WHERE id = ?", id))
	for i, x := range []int64{0x7001, 0x7002, 0x7002, 0x7003} {
		cls := run.Int("cls")
		if i == 3 {
			cls++ // another class: not compared
		}
		r := must(f.o.C.DB.One("INSERT INTO score_submissions (xuid, sb, st, gsid, track, car, cls, cpi, score, clean, stm, tcs, abs, submitted) "+
			"VALUES (?, ?, ?, ?, ?, 0, ?, 600, ?, 1, 0, 0, 0, 1.7e9) RETURNING id", x, run.Int("sb"), run.Int("st"), run.Int("gsid"), run.Int("track"), cls, 90.0+float64(i)))
		if _, err := f.o.C.DB.Exec("INSERT INTO score_ghosts (submission_id, data, uploaded) VALUES (?, ?, 1.7e9)", r.Int("id"), blob); err != nil {
			t.Fatal(err)
		}
		if i == 1 { // the same driver's better run is hidden: the comparison takes their best run on the boards
			if _, err := f.o.C.DB.Exec("INSERT INTO hidden_scores (submission_id, hidden_at, source) VALUES (?, 1, 'operator')", r.Int("id")); err != nil {
				t.Fatal(err)
			}
		}
	}
	_, obj, _ = f.o.route(nil, "GET", fmt.Sprintf("/ops/v1/runs/%d/ghost", id), nil)
	fld := field(obj.(pyjson.Obj), "field").(pyjson.Obj)
	runs := field(fld, "runs").([]any)
	if len(runs) != 2 || field(fld, "step") != 20.0 || field(runs[0].(pyjson.Obj), "score") != 90.0 || field(runs[1].(pyjson.Obj), "score") != 92.0 {
		t.Fatalf("comparison: %s", dump(pyjson.Obj{{K: "class", V: field(fld, "class")}, {K: "n", V: len(runs)}}))
	}
	speeds := field(runs[0].(pyjson.Obj), "speed").([]any)
	d, _ := field(obj.(pyjson.Obj), "samples").(pyjson.Obj).Get("d")
	dist := d.([]any)
	if want := int(dist[len(dist)-1].(float64)/20) + 1; len(speeds) != want {
		t.Fatalf("a comparison trace has %d points, want one every 20 m of the %v m driven (%d)", len(speeds), dist[len(dist)-1], want)
	}

	if st, code := opsCall(t, f.o, "GET", fmt.Sprintf("/ops/v1/runs/%d/ghost", f.ids["reported3"]), ""); st != 404 || code != "no_ghost" {
		t.Fatalf("a run without a ghost answered %d %s", st, code)
	}
	if st, out := opsCall(t, f.o, "GET", "/ops/v1/rivals/events", ""); st != 200 || !strings.Contains(out, `"Night Sprint"`) {
		t.Fatalf("rivals events: %d %s", st, out)
	}
	if st, out := opsCall(t, f.o, "GET", "/ops/v1/rivals/events/65000/runs", ""); st != 200 || !strings.Contains(out, `"why": ["Speed 451 km/h, over the 432 km/h limit"]`) {
		t.Fatalf("rivals runs: %d %s", st, out)
	}
}
