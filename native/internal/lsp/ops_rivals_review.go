package lsp

// The admin panel's Monthly Rivals review (/ops/v1/rivals/events and /ops/v1/rivals/events/<id>/runs): every Monthly Rivals event with its run counts, and
// for one event the best run of every player with the ghost decoded to traces, so a reviewer sees all of them side by side and the outliers stand out.

import (
	"math"

	"fh2mp/internal/ghost"
	"fh2mp/internal/pyjson"
)

const traceStep = 0.5 // s between trace points

func opt(v float64, ok bool) any {
	if !ok {
		return nil
	}
	return math.Round(v*10) / 10
}

// rivalsEvents lists the events of every rotation, newest first, with how many runs and players they have and how many of those runs kept a ghost.
func (o *Ops) rivalsEvents() pyjson.Obj {
	rows := must(o.C.DB.Query(`SELECT e.id, e.month, e.reroll, e.idx, e.name, e.trackname, e.laps, e.cls,
        COUNT(s.id) AS runs, COUNT(DISTINCT s.xuid) AS players, COUNT(g.submission_id) AS ghosts
        FROM rival_events e LEFT JOIN score_submissions s ON s.sb = 3 AND s.st = 1 AND s.gsid = e.id
        LEFT JOIN score_ghosts g ON g.submission_id = s.id
        GROUP BY e.id ORDER BY e.month DESC, e.reroll DESC, e.idx`))
	out := []any{}
	for i := range rows {
		r := &rows[i]
		cls := ""
		if r.Any("cls") != nil {
			cls = classLetter(r.Int("cls"))
		}
		out = append(out, pyjson.Obj{{K: "id", V: r.Int("id")}, {K: "month", V: monthLabel(r.Int("month"))}, {K: "reroll", V: r.Int("reroll")},
			{K: "name", V: r.Str("name")}, {K: "track", V: r.Str("trackname")}, {K: "laps", V: r.Int("laps")}, {K: "class", V: cls},
			{K: "runs", V: r.Int("runs")}, {K: "players", V: r.Int("players")}, {K: "ghosts", V: r.Int("ghosts")}})
	}
	return pyjson.Obj{{K: "events", V: out}}
}

// rivalsRuns gives each player's best run on an event, with the ghost's summary numbers and traces (every traceStep seconds).
func (o *Ops) rivalsRuns(eventID int64) pyjson.Obj {
	c := o.C
	ev := must(c.DB.One("SELECT id, name, trackname, laps, cls, month FROM rival_events WHERE id = ?", eventID))
	if ev == nil {
		panic(opsErr(404, "not_found", "unknown event"))
	}
	rows := must(c.DB.Query(`SELECT s.id, s.xuid, s.car, s.score, s.submitted, s.clean, COALESCE(p.gamertag, '') AS tag, g.data AS blob,
        v.verdict, v.reasons, (SELECT hs.source FROM hidden_scores hs WHERE hs.submission_id = s.id) AS hidden_by,
        (SELECT COUNT(*) FROM score_submissions x WHERE x.sb = 3 AND x.st = 1 AND x.gsid = s.gsid AND x.xuid = s.xuid) AS tries
        FROM score_submissions s JOIN (SELECT xuid, MIN(score) AS best FROM score_submissions WHERE sb = 3 AND st = 1 AND gsid = ? GROUP BY xuid) b
        ON b.xuid = s.xuid AND b.best = s.score
        LEFT JOIN players p ON p.xuid = s.xuid LEFT JOIN score_ghosts g ON g.submission_id = s.id LEFT JOIN score_verdicts v ON v.submission_id = s.id
        WHERE s.sb = 3 AND s.st = 1 AND s.gsid = ? ORDER BY s.score, s.id`, eventID, eventID))
	names := must(c.Riv.Names()).Car
	seen := map[int64]bool{}
	runs := []any{}
	for i := range rows {
		r := &rows[i]
		x := r.Int("xuid")
		if seen[x] {
			continue
		}
		seen[x] = true
		reasons := any([]any{})
		if rs := r.Str("reasons"); rs != "" {
			reasons = must(pyjson.Parse(rs))
		}
		var hiddenBy any
		if hb := r.Str("hidden_by"); hb != "" {
			hiddenBy = hb
		}
		run := pyjson.Obj{{K: "submissionId", V: r.Int("id")}, {K: "xuid", V: x}, {K: "gamertag", V: o.gamertag(r.Str("tag"), x)}, {K: "carId", V: r.Int("car")},
			{K: "car", V: names[r.Int("car")]}, {K: "score", V: r.Float("score")}, {K: "at", V: int64(r.Float("submitted"))}, {K: "tries", V: r.Int("tries")},
			{K: "clean", V: r.Int("clean") != 0}, {K: "verdict", V: r.Any("verdict")}, {K: "reasons", V: reasons}, {K: "why", V: plainReasons(reasons)}, {K: "hiddenBy", V: hiddenBy}}
		blob := r.Bytes("blob")
		var g *ghost.Ghost
		if len(blob) > 0 {
			g, _ = ghost.Parse(blob)
		}
		if g == nil {
			run = append(run, pyjson.Pair{K: "ghost", V: false})
			runs = append(runs, run)
			continue
		}
		var m any
		if mm := ghost.Measure(g); mm != nil {
			m = pyjson.Obj{{K: "speed", V: opt(mm.Speed, true)}, {K: "acceleration", V: opt(mm.Accel, true)}, {K: "power", V: opt(mm.Power, mm.Power != 0)},
				{K: "climb", V: opt(mm.Climb, true)}, {K: "teleports", V: mm.Teleports}}
		}
		tr := traceOf(ghost.Series(g))
		run = append(run, pyjson.Pair{K: "ghost", V: true}, pyjson.Pair{K: "measures", V: m}, pyjson.Pair{K: "trace", V: tr})
		runs = append(runs, run)
	}
	cls := ""
	if ev.Any("cls") != nil {
		cls = classLetter(ev.Int("cls"))
	}
	return pyjson.Obj{{K: "event", V: pyjson.Obj{{K: "id", V: eventID}, {K: "name", V: ev.Str("name")}, {K: "track", V: ev.Str("trackname")}, {K: "laps", V: ev.Int("laps")},
		{K: "class", V: cls}, {K: "month", V: monthLabel(ev.Int("month"))}}},
		{K: "limits", V: checkerLimits()}, {K: "runs", V: runs}}
}

// traceOf resamples a ghost's series every traceStep seconds: time, speed, acceleration, power and the line driven.
func traceOf(series []ghost.Sample) pyjson.Obj {
	var t, sp, ac, pw, px, pz []any
	k := 0
	for next := 0.0; len(series) > 0 && next <= series[len(series)-1].T; next += traceStep {
		for k < len(series)-1 && series[k].T < next {
			k++
		}
		pick := &series[k]
		t = append(t, math.Round(next*10)/10)
		sp = append(sp, opt(pick.Speed, true))
		ac = append(ac, opt(pick.Accel, pick.Counted))
		pw = append(pw, opt(pick.Power, pick.Counted))
		px = append(px, math.Round(pick.X))
		pz = append(pz, math.Round(pick.Z))
	}
	return pyjson.Obj{{K: "t", V: t}, {K: "speed", V: sp}, {K: "accel", V: ac}, {K: "power", V: pw}, {K: "x", V: px}, {K: "z", V: pz}}
}
