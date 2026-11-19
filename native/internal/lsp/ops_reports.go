package lsp

// The admin panel's review queue (/ops/v1/reports): runs players reported on the website, runs the ghost checker flagged, and runs taken off the
// boards (by an admin, by the checker, or because the car is not in the game). Every run sits in exactly one view, and every row says in plain
// words why it is there.

import (
	"fmt"
	"math"
	"regexp"
	"strconv"
	"strings"

	"fh2mp/internal/db"
	"fh2mp/internal/ghost"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/rivals"
	"fh2mp/internal/scorereports"
)

// The review views. "" is all of them together (what the panel asked for before the views existed).
var reviewViews = map[string]string{
	"reported": "hs.submission_id IS NULL AND COALESCE(r.n, 0) > 0",
	"flagged":  "hs.submission_id IS NULL AND COALESCE(r.n, 0) = 0 AND v.verdict IN ('flag', 'hard') AND v.reviewed = 0",
	"hidden":   "hs.submission_id IS NOT NULL",
	"":         "(hs.submission_id IS NOT NULL OR COALESCE(r.n, 0) > 0 OR (v.verdict IN ('flag', 'hard') AND v.reviewed = 0))",
}

var reviewOrder = map[string]string{
	"reported": "r.n DESC, r.last_at DESC, s.id DESC",
	"flagged":  "s.id DESC",
	"hidden":   "hs.hidden_at DESC, s.id DESC",
	"":         "COALESCE(r.n, 0) DESC, s.id DESC",
}

// Who took a hidden run off the boards: admin (an operator), car (the car is not in the game catalog) or checker (a hard ghost verdict).
const hiddenKindSQL = "CASE WHEN hs.source = 'operator' THEN 'admin' WHEN hs.reason = '" + scorereports.CarOutsideCatalog + "' THEN 'car' ELSE 'checker' END"

var hiddenKinds = []string{"admin", "checker", "car"}

const reviewFrom = `FROM score_submissions s
        LEFT JOIN players p ON p.xuid = s.xuid
        LEFT JOIN (SELECT submission_id, COUNT(*) AS n, MIN(created) AS first_at, MAX(created) AS last_at FROM score_reports
                   WHERE dismissed = 0 GROUP BY submission_id) r ON r.submission_id = s.id
        LEFT JOIN hidden_scores hs ON hs.submission_id = s.id AND hs.source <> 'pending'
        LEFT JOIN score_verdicts v ON v.submission_id = s.id
        LEFT JOIN rival_events e ON s.sb = 3 AND e.id = s.gsid
        WHERE s.sb IN ` + scorereports.ReviewBoards

const reviewSelect = `SELECT s.id, s.xuid, s.sb, s.st, s.gsid, s.track, s.car, s.cls, s.score, s.submitted, COALESCE(p.gamertag, '') AS tag,
        COALESCE(r.n, 0) AS report_count, r.first_at, r.last_at, hs.hidden_at, hs.source AS hidden_source, hs.reason AS hidden_reason,
        ` + hiddenKindSQL + ` AS hidden_kind, v.verdict, v.reasons, v.reviewed, e.name AS event_name,
        CASE WHEN EXISTS (SELECT 1 FROM score_ghosts g WHERE g.submission_id = s.id) THEN 1 ELSE 0 END AS has_ghost
        ` + reviewFrom

// reviewOne is one run's review row whatever view it is in (or none), nil for a run on a board nobody reviews.
func (o *Ops) reviewOne(id int64) *db.Row {
	return must(o.C.DB.One(reviewSelect+" AND s.id = ?", id))
}

// reportsQueue lists one view of the review queue, newest decision first; by narrows the hidden view to one kind of hide. A run still waiting
// for its ghost is not a decision and is in no view.
func (o *Ops) reportsQueue(view, by string, limit int, offset int64) pyjson.Obj {
	cond, ok := reviewViews[view]
	if !ok {
		reportsLog.Warn(fmt.Sprintf("review queue refused: unknown view %s, expected reported, flagged or hidden", pyjson.PyRepr(cut(view, 20))))
		panic(opsErr(400, "bad_request", "unknown view"))
	}
	if by != "" {
		if view != "hidden" || !contains(hiddenKinds, by) {
			reportsLog.Warn(fmt.Sprintf("review queue refused: by=%s on view %s, expected admin, checker or car on the hidden view", pyjson.PyRepr(cut(by, 20)), pyjson.PyRepr(view)))
			panic(opsErr(400, "bad_request", "unknown filter"))
		}
		cond += " AND " + hiddenKindSQL + " = '" + by + "'"
	}
	rows := must(o.C.DB.Query(reviewSelect+` AND `+cond+` ORDER BY `+reviewOrder[view]+` LIMIT ? OFFSET ?`, limit+1, offset))
	names := must(o.C.Riv.Names())
	out := []any{}
	for i := range rows {
		if i >= limit {
			break
		}
		out = append(out, o.reviewRow(&rows[i], names))
	}
	return pyjson.Obj{{K: "view", V: view}, {K: "by", V: by}, {K: "counts", V: o.reviewCounts()}, {K: "entries", V: out}, {K: "hasNext", V: len(rows) > limit}}
}

// reviewCounts is how many runs each view holds, and the hidden view by kind.
func (o *Ops) reviewCounts() pyjson.Obj {
	sum := func(cond string) string { return "COALESCE(SUM(CASE WHEN " + cond + " THEN 1 ELSE 0 END), 0)" }
	q := "SELECT " + sum(reviewViews["reported"]) + " AS reported, " + sum(reviewViews["flagged"]) + " AS flagged, " + sum(reviewViews["hidden"]) + " AS hidden"
	for _, k := range hiddenKinds {
		q += ", " + sum("hs.submission_id IS NOT NULL AND "+hiddenKindSQL+" = '"+k+"'") + " AS hidden_" + k
	}
	r := must(o.C.DB.One(q + " " + reviewFrom + " AND " + reviewViews[""]))
	by := pyjson.Obj{}
	for _, k := range hiddenKinds {
		by = append(by, pyjson.Pair{K: k, V: r.Int("hidden_" + k)})
	}
	return pyjson.Obj{{K: "reported", V: r.Int("reported")}, {K: "flagged", V: r.Int("flagged")}, {K: "hidden", V: r.Int("hidden")}, {K: "hiddenBy", V: by}}
}

func (o *Ops) reviewRow(r *db.Row, names *rivals.Names) pyjson.Obj {
	unit := "m/s"
	if r.Int("st") == 1 {
		unit = "s"
	}
	reports := r.Int("report_count")
	hidden := r.Any("hidden_at") != nil
	var reasons []string
	if rs := r.Str("reasons"); rs != "" {
		for _, v := range must(pyjson.Parse(rs)).([]any) {
			if s, ok := v.(string); ok {
				reasons = append(reasons, s)
			}
		}
	}
	verdict := r.Any("verdict")
	reviewed := r.Int("reviewed") != 0
	suspect := (verdict == "flag" || verdict == "hard") && !reviewed
	var source string
	var hiddenKind any
	why := []any{}
	switch {
	case hidden:
		kind := r.Str("hidden_kind")
		hiddenKind = kind
		switch kind {
		case "admin":
			source = "Hidden by an admin"
		case "car":
			source = "Hidden automatically: the car is not in the game"
		default:
			source = "Hidden by the replay checker"
			if len(reasons) == 0 && r.Str("hidden_reason") != "" {
				reasons = strings.Split(r.Str("hidden_reason"), "; ")
			}
		}
	case reports > 0:
		source = "Reported " + times(reports) + " on the website"
	case suspect:
		source = "Flagged by the replay checker"
	}
	if reports > 0 && hidden {
		why = append(why, "Reported "+times(reports)+" on the website")
	}
	if suspect || hiddenKind == "checker" {
		for _, s := range reasons {
			why = append(why, plainReason(s))
		}
	}
	var hiddenBy any
	if hidden {
		hiddenBy = r.Str("hidden_source")
	}
	return pyjson.Obj{{K: "submissionId", V: r.Int("id")}, {K: "gamertag", V: o.gamertag(r.Str("tag"), r.Int("xuid"))}, {K: "xuid", V: r.Int("xuid")},
		{K: "score", V: r.Float("score")}, {K: "unit", V: unit}, {K: "car", V: names.Car[r.Int("car")]}, {K: "class", V: classText(r.Int("cls"))},
		{K: "boardType", V: r.Int("sb")}, {K: "track", V: r.Int("track")}, {K: "event", V: r.Int("gsid")},
		{K: "board", V: boardLabel(r.Int("sb"), r.Int("st"), r.Int("track"), r.Int("gsid"), r.Str("event_name"), names)},
		{K: "at", V: int64(r.Float("submitted"))}, {K: "reports", V: reports}, {K: "firstReport", V: optInt(r, "first_at")}, {K: "lastReport", V: optInt(r, "last_at")},
		{K: "hidden", V: hidden}, {K: "hiddenBy", V: hiddenBy}, {K: "hiddenKind", V: hiddenKind}, {K: "hiddenAt", V: optInt(r, "hidden_at")},
		{K: "verdict", V: verdict}, {K: "reviewed", V: reviewed}, {K: "reasons", V: stringsAny(reasons)},
		{K: "source", V: source}, {K: "why", V: why}, {K: "hasGhost", V: r.Int("has_ghost") != 0}}
}

func optInt(r *db.Row, name string) any {
	if r.IsNull(name) {
		return nil
	}
	return int64(r.Float(name))
}

// classText is the class letter of a run, X and R included (the classes above S2 a maxed-out car reports).
func classText(cls int64) string {
	if cls >= 0 && int(cls) < len(allClasses) {
		return allClasses[cls]
	}
	return strconv.FormatInt(cls, 10)
}

func times(n int64) string {
	switch n {
	case 1:
		return "once"
	case 2:
		return "twice"
	}
	return fmt.Sprintf("%d times", n)
}

func contains(l []string, s string) bool {
	for _, v := range l {
		if v == s {
			return true
		}
	}
	return false
}

func stringsAny(l []string) []any {
	out := make([]any, len(l))
	for i, s := range l {
		out[i] = s
	}
	return out
}

// boardLabel names the board a run is on: the route or event by name, the speed trap or zone by number.
func boardLabel(sb, st, track, gsid int64, eventName string, names *rivals.Names) string {
	switch sb {
	case 1:
		if n := names.Track[track][0]; n != "" {
			return "Festival Route: " + n
		}
		return fmt.Sprintf("Festival Route %d", track)
	case 3:
		if eventName != "" {
			return "Monthly Rivals: " + eventName
		}
		return fmt.Sprintf("Monthly Rivals event %d", gsid)
	case 6:
		return fmt.Sprintf("Showcase event %d", gsid)
	case 9:
		if st == 6 {
			return fmt.Sprintf("Speed zone %d", gsid)
		}
		return fmt.Sprintf("Speed trap %d", gsid)
	}
	return fmt.Sprintf("Board %d", sb)
}

var (
	reLimit   = regexp.MustCompile(`^(speed|acceleration|climb|power) (-?[0-9.]+) is above ([0-9.]+)$`)
	reEnds    = regexp.MustCompile(`^ghost ends at ([0-9.]+) s, the score says ([0-9.]+) s$`)
	reGap     = regexp.MustCompile(`^([0-9.]+) s missing between frames$`)
	reJumps   = regexp.MustCompile(`^([0-9]+) jumps across the map$`)
	hpPerWkg  = 1.341 // hp per tonne in one W/kg
	gravity   = 9.81
	fixedText = map[string]string{
		"the score is not in the ghost's header": "The submitted time is not in the replay file",
		"the car is not in the ghost's header":   "The replay file does not name this car (also seen on real runs, not proof on its own)",
		"too little driving in the ghost":        "The replay holds too little driving to check",
		scorereports.CarOutsideCatalog:           "The car is not in the game",
	}
)

// plainReason says a ghost checker's reason (ghost.Check) in the words and units a driver uses: km/h, g, hp per tonne, lap clock.
func plainReason(s string) string {
	if t, ok := fixedText[s]; ok {
		return t
	}
	if m := reLimit.FindStringSubmatch(s); m != nil {
		v, _ := strconv.ParseFloat(m[2], 64)
		l, _ := strconv.ParseFloat(m[3], 64)
		switch m[1] {
		case "speed":
			return fmt.Sprintf("Speed %.0f km/h, over the %.0f km/h limit", v*3.6, l*3.6)
		case "acceleration":
			return fmt.Sprintf("Acceleration %.1f g, over the %.1f g limit", v/gravity, l/gravity)
		case "climb":
			return fmt.Sprintf("Climbs at %.0f km/h straight up, over the %.0f km/h limit", v*3.6, l*3.6)
		default:
			return fmt.Sprintf("Power %.0f hp per tonne, over the %.0f hp per tonne limit", v*hpPerWkg, l*hpPerWkg)
		}
	}
	if m := reEnds.FindStringSubmatch(s); m != nil {
		a, _ := strconv.ParseFloat(m[1], 64)
		b, _ := strconv.ParseFloat(m[2], 64)
		return "The replay ends at " + lapClock(a) + ", the submitted time is " + lapClock(b)
	}
	if m := reGap.FindStringSubmatch(s); m != nil {
		return "The replay has a " + m[1] + " s hole in it"
	}
	if m := reJumps.FindStringSubmatch(s); m != nil {
		return "The car jumps across the map " + m[1] + " times"
	}
	if rest, ok := strings.CutPrefix(s, "unreadable ghost: "); ok {
		return "The replay file cannot be read (" + rest + ")"
	}
	if s == "" {
		return s
	}
	return strings.ToUpper(s[:1]) + s[1:]
}

// checkerLimits are the replay checker's limits for the panel's charts (speedFlag, speedHard, accelFlag, ... in m/s, m/s^2 and W/kg), read from
// the checker itself so a line on a chart is the line the checker uses.
func checkerLimits() pyjson.Obj {
	out := pyjson.Obj{}
	for _, l := range []struct{ rule, key string }{{"speed", "speed"}, {"acceleration", "accel"}, {"power", "power"}, {"climb", "climb"}} {
		flag, hard := ghost.Limits(l.rule)
		if flag > 0 {
			out = append(out, pyjson.Pair{K: l.key + "Flag", V: flag})
		}
		out = append(out, pyjson.Pair{K: l.key + "Hard", V: hard})
	}
	return append(out, pyjson.Pair{K: "highSpeed", V: ghost.HighSpeed})
}

// plainReasons is plainReason over a stored reasons list.
func plainReasons(reasons any) []any {
	out := []any{}
	l, _ := reasons.([]any)
	for _, v := range l {
		if s, ok := v.(string); ok {
			out = append(out, plainReason(s))
		}
	}
	return out
}

// distances is how far along the line driven each sample of a ghost is, in metres; a step that jumps across the map adds nothing.
func distances(series []ghost.Sample) []float64 {
	out := make([]float64, len(series))
	for i := 1; i < len(series); i++ {
		d := 0.0
		if !series[i].Jump && !series[i-1].Jump {
			d = math.Hypot(series[i].X-series[i-1].X, series[i].Z-series[i-1].Z)
		}
		out[i] = out[i-1] + d
	}
	return out
}

const (
	fieldStep = 20.0 // metres between the points of a comparison trace
	fieldMax  = 30   // other drivers in the comparison
)

func round2(v float64) float64 { return math.Round(v*100) / 100 }

// fieldTrace resamples a ghost every fieldStep metres along the line driven: speed, acceleration, power (above the checker's high-speed floor
// only) and climb, nil where the checker does not read the sample. Point k is k*fieldStep metres from the start.
func fieldTrace(series []ghost.Sample) (speed, accel, power, climb []any) {
	dist := distances(series)
	j := 0
	for k := 0; len(series) > 0 && float64(k)*fieldStep <= dist[len(dist)-1]; k++ {
		for j < len(series)-1 && dist[j] < float64(k)*fieldStep {
			j++
		}
		s := &series[j]
		speed = append(speed, round2(s.Speed))
		if s.Counted && !s.Jump {
			accel = append(accel, round2(s.Accel))
		} else {
			accel = append(accel, nil)
		}
		if s.Counted && !s.Jump && s.Speed > ghost.HighSpeed {
			power = append(power, round2(s.Power))
		} else {
			power = append(power, nil)
		}
		climb = append(climb, round2(s.Climb))
	}
	return
}

// field is the comparison for a run's replay: the best run of every other driver on the same board in the same class that is on the boards
// and has a ghost, fastest first, each as a trace along the distance driven.
func (o *Ops) field(row *db.Row, names *rivals.Names) pyjson.Obj {
	c := o.C
	ids := must(c.DB.Query(`SELECT id FROM (SELECT DISTINCT ON (s.xuid) s.id, s.score FROM score_submissions s
        WHERE s.sb = ? AND s.st = ? AND s.gsid = ? AND s.track = ? AND s.cls = ? AND s.xuid <> ?
        AND EXISTS (SELECT 1 FROM score_ghosts g WHERE g.submission_id = s.id) AND `+scorereports.Visible+`
        ORDER BY s.xuid, s.score, s.id) best ORDER BY score, id LIMIT ?`,
		row.Int("sb"), row.Int("st"), row.Int("gsid"), row.Int("track"), row.Int("cls"), row.Int("xuid"), fieldMax))
	runs := []any{}
	for _, r := range ids {
		f := must(c.DB.One(`SELECT s.id, s.xuid, s.score, s.car, COALESCE(p.gamertag, '') AS tag, v.verdict, g.data AS blob FROM score_submissions s
            JOIN score_ghosts g ON g.submission_id = s.id LEFT JOIN players p ON p.xuid = s.xuid LEFT JOIN score_verdicts v ON v.submission_id = s.id
            WHERE s.id = ?`, r.Int("id")))
		if f == nil {
			continue
		}
		g, err := ghost.Parse(f.Bytes("blob"))
		if err != nil {
			continue
		}
		speed, accel, power, climb := fieldTrace(ghost.Series(g))
		runs = append(runs, pyjson.Obj{{K: "submissionId", V: f.Int("id")}, {K: "gamertag", V: o.gamertag(f.Str("tag"), f.Int("xuid"))},
			{K: "score", V: f.Float("score")}, {K: "car", V: names.Car[f.Int("car")]}, {K: "verdict", V: f.Any("verdict")},
			{K: "speed", V: speed}, {K: "accel", V: accel}, {K: "power", V: power}, {K: "climb", V: climb}})
	}
	return pyjson.Obj{{K: "class", V: classText(row.Int("cls"))}, {K: "step", V: fieldStep}, {K: "runs", V: runs}}
}

// lapClock is m:ss.ss.
func lapClock(t float64) string {
	m := math.Floor(t / 60)
	return fmt.Sprintf("%d:%05.2f", int64(m), t-m*60)
}
