package lsp

// The admin panel's Monthly Rivals review (/ops/v1/rivals/events/<id>/review): every driver's run on one event that still needs a decision,
// each measure set against the drivers it is fairly compared with (the same car at the same PI when at least two others share it, else every
// other driver on the event), the stretches of road where it beats all of them, and how much of the route it drove. Runs already decided
// (hidden, or cleared by an admin) come separately. The approach is the 2026-10-31 Monthly Rivals scan's: a number means something only next
// to what the other drivers did on the same road.

import (
	"math"
	"sort"
	"strings"

	"fh2mp/internal/db"
	"fh2mp/internal/ghost"
	"fh2mp/internal/pyjson"
)

const (
	sameSetupMin  = 2      // other drivers in the same car at the same PI before they are the comparison
	samePI        = 0.0005 // PI difference (score_submissions.cpi) still counted as the same setup
	stretchWindow = 2      // grid points either side a driver's line may sit at the same place on the road (2 x 20 m)
	stretchMinLen = 3      // grid points a stretch needs (about 60 m)
	shortRoute    = 0.85   // a run that drove less than this share of the route cut the course
	above         = 1.05   // a measure this far over the highest of the others is above them (the fastest by a hair is not)
	farAbove      = 1.25   // and this far over it is far above
)

// grid is a ghost resampled every fieldStep metres along the line driven; NaN where the checker does not read the sample.
type grid struct{ t, speed, accel []float64 }

func gridOf(series []ghost.Sample) *grid {
	dist := distances(series)
	g := &grid{}
	j := 0
	for k := 0; len(series) > 0 && float64(k)*fieldStep <= dist[len(dist)-1]; k++ {
		for j < len(series)-1 && dist[j] < float64(k)*fieldStep {
			j++
		}
		s := &series[j]
		g.t = append(g.t, s.T)
		g.speed = append(g.speed, s.Speed)
		if s.Counted && !s.Jump {
			g.accel = append(g.accel, s.Accel)
		} else {
			g.accel = append(g.accel, math.NaN())
		}
	}
	return g
}

// stretch is a piece of road where a run beats every one of the others: metres from the start, the run's time there, its peak and what the
// others did there (median, highest), in m/s or m/s^2.
type stretch struct {
	kind            string // speed | accel
	from, to        float64
	t               float64
	value, med, top float64
}

// stretchesOf finds where a run's speed or acceleration is above every other driver's at the same place on the road (each other driver's
// highest within stretchWindow grid points, so a slightly different line does not count). Speed must beat them by 5 % and 2 m/s, acceleration
// by 0.5 m/s^2 and be at least 1 m/s^2; a stretch is at least stretchMinLen points, with one point of gap allowed.
func stretchesOf(run *grid, others []*grid) []stretch {
	var out []stretch
	for _, kind := range []string{"speed", "accel"} {
		col := func(g *grid) []float64 {
			if kind == "speed" {
				return g.speed
			}
			return g.accel
		}
		mine := col(run)
		type mark struct {
			k        int
			top, med float64
		}
		var marks []mark
		for k := range mine {
			v := mine[k]
			if math.IsNaN(v) {
				continue
			}
			top, n := math.Inf(-1), 0
			var here []float64
			for _, o := range others {
				oc := col(o)
				for w := k - stretchWindow; w <= k+stretchWindow; w++ {
					if w >= 0 && w < len(oc) && !math.IsNaN(oc[w]) && oc[w] > top {
						top = oc[w]
					}
				}
				if k < len(oc) && !math.IsNaN(oc[k]) {
					here = append(here, oc[k])
					n++
				}
			}
			if n < sameSetupMin {
				continue
			}
			above := kind == "speed" && v > top*1.05 && v > top+2 || kind == "accel" && v > top+0.5 && v > 1
			if above {
				sort.Float64s(here)
				marks = append(marks, mark{k, top, here[len(here)/2]})
			}
		}
		for i := 0; i < len(marks); {
			j := i
			for j+1 < len(marks) && marks[j+1].k-marks[j].k <= 2 {
				j++
			}
			if marks[j].k-marks[i].k+1 >= stretchMinLen {
				s := stretch{kind: kind, from: float64(marks[i].k) * fieldStep, to: float64(marks[j].k) * fieldStep, t: run.t[marks[i].k], value: math.Inf(-1), top: math.Inf(-1)}
				sum := 0.0
				for _, m := range marks[i : j+1] {
					s.value = math.Max(s.value, mine[m.k])
					s.top = math.Max(s.top, m.top)
					sum += m.med
				}
				s.med = sum / float64(j-i+1)
				out = append(out, s)
			}
			i = j + 1
		}
	}
	sort.Slice(out, func(a, b int) bool { return out[a].from < out[b].from })
	return out
}

// caseRun is one run in the review with what its ghost says.
type caseRun struct {
	row        *db.Row
	id, xuid   int64
	car        int64
	cpi, score float64
	hidden     bool
	reviewed   bool
	grid       *grid
	m          *ghost.Measures
	driven     float64
}

func median(v []float64) float64 {
	s := append([]float64(nil), v...)
	sort.Float64s(s)
	return s[len(s)/2]
}

// measureVs is one measure of a run against its comparison group: value, the others' median and highest, and 0 (within them), 1 (above all of
// them) or 2 (farAbove the highest).
func measureVs(v float64, others []float64) any {
	if len(others) == 0 {
		return pyjson.Obj{{K: "value", V: round2(v)}, {K: "median", V: nil}, {K: "top", V: nil}, {K: "level", V: 0}}
	}
	top := others[0]
	for _, o := range others {
		top = math.Max(top, o)
	}
	level := 0
	if v > top*above {
		level = 1
		if v > top*farAbove {
			level = 2
		}
	}
	return pyjson.Obj{{K: "value", V: round2(v)}, {K: "median", V: round2(median(others))}, {K: "top", V: round2(top)}, {K: "level", V: level}}
}

// rivalsReview is the review of one Monthly Rivals event.
func (o *Ops) rivalsReview(eventID int64) pyjson.Obj {
	c := o.C
	ev := must(c.DB.One("SELECT id, name, trackname, laps, length, cls, month FROM rival_events WHERE id = ?", eventID))
	if ev == nil {
		panic(opsErr(404, "not_found", "unknown event"))
	}
	rows := must(c.DB.Query(`SELECT s.id, s.xuid, s.car, s.score, s.cpi, s.submitted, COALESCE(p.gamertag, '') AS tag, v.verdict, v.reasons,
        COALESCE(v.reviewed, 0) AS reviewed, hs.source AS hidden_source, `+hiddenKindSQL+` AS hidden_kind,
        (SELECT COUNT(*) FROM score_submissions x WHERE x.sb = 3 AND x.st = 1 AND x.gsid = s.gsid AND x.xuid = s.xuid) AS tries
        FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid LEFT JOIN score_verdicts v ON v.submission_id = s.id
        LEFT JOIN hidden_scores hs ON hs.submission_id = s.id AND hs.source <> 'pending'
        WHERE s.sb = 3 AND s.st = 1 AND s.gsid = ? ORDER BY s.score, s.id`, eventID))
	// per driver: the best run on the boards, and the best hidden run when it is better than that (or the driver has nothing on the boards)
	var runs []*caseRun
	visible, hidden := map[int64]bool{}, map[int64]bool{}
	for i := range rows {
		r := &rows[i]
		x := r.Int("xuid")
		isHidden := r.Any("hidden_source") != nil
		if isHidden && (hidden[x] || visible[x]) || !isHidden && visible[x] {
			continue
		}
		if isHidden {
			hidden[x] = true
		} else {
			visible[x] = true
		}
		runs = append(runs, &caseRun{row: r, id: r.Int("id"), xuid: x, car: r.Int("car"), cpi: r.Float("cpi"), score: r.Float("score"), hidden: isHidden,
			reviewed: r.Int("reviewed") != 0})
	}
	for _, cr := range runs {
		g := must(c.DB.One("SELECT data FROM score_ghosts WHERE submission_id = ?", cr.id))
		if g == nil {
			continue
		}
		gh, err := ghost.Parse(g.Bytes("data"))
		if err != nil {
			continue
		}
		series := ghost.Series(gh)
		if len(series) == 0 {
			continue
		}
		cr.grid = gridOf(series)
		cr.m = ghost.Measure(gh)
		d := distances(series)
		cr.driven = d[len(d)-1]
	}
	names := must(c.Riv.Names())
	expected := ev.Float("length") * float64(max(1, ev.Int("laps")))
	// Two passes: the first finds the suspects against everyone; the second compares every run only with the others that are not suspects, so
	// one cheat does not raise the bar ("others' top") for the honest runs.
	var open, decided []any
	suspects := map[*caseRun]bool{}
	for pass := 0; pass < 2; pass++ {
		skip := suspects
		if pass == 0 {
			skip = map[*caseRun]bool{}
		}
		found := map[*caseRun]bool{}
		position := int64(0)
		open, decided = []any{}, []any{}
		for _, cr := range runs {
			var pos any
			if !cr.hidden {
				position++
				pos = position
			}
			// the comparison: the others on the boards with a readable ghost, narrowed to the same car at the same PI when enough share it
			var all, same []*caseRun
			for _, o := range runs {
				if o == cr || o.hidden || o.m == nil || skip[o] {
					continue
				}
				all = append(all, o)
				if o.car == cr.car && math.Abs(o.cpi-cr.cpi) < samePI {
					same = append(same, o)
				}
			}
			group, label := all, "everyone on this event"
			if len(same) >= sameSetupMin {
				group, label = same, "same car and PI"
			}
			item := pyjson.Obj{{K: "submissionId", V: cr.id}, {K: "xuid", V: cr.xuid}, {K: "gamertag", V: o.gamertag(cr.row.Str("tag"), cr.xuid)},
				{K: "carId", V: cr.car}, {K: "car", V: names.Car[cr.car]}, {K: "score", V: cr.score}, {K: "at", V: int64(cr.row.Float("submitted"))},
				{K: "tries", V: cr.row.Int("tries")}, {K: "position", V: pos}, {K: "verdict", V: cr.row.Any("verdict")},
				{K: "checker", V: plainReasons(parsedList(cr.row.Str("reasons")))}, {K: "ghost", V: cr.m != nil},
				{K: "compared", V: pyjson.Obj{{K: "label", V: label}, {K: "size", V: len(group)}}}}
			unusual := -1.0
			if cr.m != nil {
				var sp, ac, pw []float64
				for _, o := range group {
					sp = append(sp, o.m.Speed)
					ac = append(ac, o.m.Accel)
					if o.m.Power != 0 {
						pw = append(pw, o.m.Power)
					}
				}
				unusual = 0
				ratio := func(v float64, others []float64) {
					if len(others) == 0 {
						return
					}
					top := others[0]
					for _, x := range others {
						top = math.Max(top, x)
					}
					if top > 0 {
						unusual = math.Max(unusual, v/top)
					}
				}
				item = append(item, pyjson.Pair{K: "speed", V: measureVs(cr.m.Speed, sp)}, pyjson.Pair{K: "accel", V: measureVs(cr.m.Accel, ac)})
				ratio(cr.m.Speed, sp)
				ratio(cr.m.Accel, ac)
				if cr.m.Power != 0 {
					item = append(item, pyjson.Pair{K: "power", V: measureVs(cr.m.Power, pw)})
					ratio(cr.m.Power, pw)
				} else {
					item = append(item, pyjson.Pair{K: "power", V: nil})
				}
				// a run far shorter than the route (or than the others, when the route length is unknown) cut the course
				want := expected
				if want <= 0 && len(group) > 0 {
					var ds []float64
					for _, o := range group {
						ds = append(ds, o.driven)
					}
					want = median(ds)
				}
				short := want > 0 && cr.driven < want*shortRoute
				if short {
					unusual = math.Max(unusual, 2)
				}
				item = append(item, pyjson.Pair{K: "distance", V: pyjson.Obj{{K: "driven", V: math.Round(cr.driven)}, {K: "expected", V: math.Round(want)}, {K: "short", V: short}}})
				var grids []*grid
				for _, o := range group {
					grids = append(grids, o.grid)
				}
				sts := []any{}
				for _, s := range stretchesOf(cr.grid, grids) {
					sts = append(sts, pyjson.Obj{{K: "kind", V: s.kind}, {K: "from", V: s.from}, {K: "to", V: s.to}, {K: "t", V: round2(s.t)},
						{K: "value", V: round2(s.value)}, {K: "median", V: round2(s.med)}, {K: "top", V: round2(s.top)}})
				}
				item = append(item, pyjson.Pair{K: "stretches", V: sts})
				if len(sts) > 0 {
					unusual = math.Max(unusual, farAbove+0.01*float64(len(sts)))
				}
			}
			v := cr.row.Str("verdict")
			suspect := unusual >= farAbove || v == "flag" || v == "hard"
			if suspect || cr.hidden {
				found[cr] = true
			}
			item = append(item, pyjson.Pair{K: "unusual", V: round2(unusual)}, pyjson.Pair{K: "suspect", V: suspect})
			switch {
			case cr.hidden:
				kind := cr.row.Str("hidden_kind")
				item = append(item, pyjson.Pair{K: "decided", V: map[string]string{"admin": "Hidden by an admin", "checker": "Hidden by the replay checker", "car": "Hidden: car not in the game"}[kind]})
				decided = append(decided, item)
			case cr.reviewed:
				item = append(item, pyjson.Pair{K: "decided", V: "Cleared by an admin"})
				decided = append(decided, item)
			default:
				item = append(item, pyjson.Pair{K: "decided", V: ""})
				open = append(open, item)
			}
		}
		suspects = found
	}
	// open cases: the suspects first, the most unusual of them first; everyone else in finishing order
	sort.SliceStable(open, func(a, b int) bool {
		sa, _ := open[a].(pyjson.Obj).Get("suspect")
		sb, _ := open[b].(pyjson.Obj).Get("suspect")
		if sa != sb {
			return sa.(bool)
		}
		if !sa.(bool) {
			return false
		}
		ua, _ := open[a].(pyjson.Obj).Get("unusual")
		ub, _ := open[b].(pyjson.Obj).Get("unusual")
		return ua.(float64) > ub.(float64)
	})
	cls := ""
	if ev.Any("cls") != nil {
		cls = classLetter(ev.Int("cls"))
	}
	return pyjson.Obj{{K: "event", V: pyjson.Obj{{K: "id", V: eventID}, {K: "name", V: ev.Str("name")}, {K: "track", V: ev.Str("trackname")},
		{K: "laps", V: ev.Int("laps")}, {K: "length", V: math.Round(expected)}, {K: "class", V: cls}, {K: "month", V: monthLabel(ev.Int("month"))}}},
		{K: "counts", V: pyjson.Obj{{K: "open", V: len(open)}, {K: "decided", V: len(decided)}}},
		{K: "open", V: open}, {K: "decided", V: decided}}
}

// parsedList is a stored JSON list of reasons ("" = none).
func parsedList(s string) any {
	if strings.TrimSpace(s) == "" {
		return []any{}
	}
	return must(pyjson.Parse(s))
}
