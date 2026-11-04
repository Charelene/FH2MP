package lsp

import (
	"fmt"
	"math"
	"net/url"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"fh2mp/internal/db"
	"fh2mp/internal/diag"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/rivals"
	"fh2mp/internal/scoreboard"
	"fh2mp/internal/scorereports"
	"fh2mp/internal/ugc"
)

// The website's statistics beyond the plain boards (/ops/v1/records, hall-of-fame, traps, cars, garage, history, totals): records and their history, the
// Monthly Rivals hall of fame, speed traps, car and make usage, the shared-design storefront, population history and online event results. Everything is read
// from stored rows; the writes are sample() (players online, once a minute) and AddResult() (an event result the game server posts to /admin/result).

const (
	historyRecheck = 15 * time.Second // between "has a run been stored?" checks of the cached record history
	historyRescan  = 60 * time.Second // and at most one full rescan in this long: at a busy hour a run arrives every few seconds
	sampleSeconds  = 60
	keepSamples    = 90 * 86400
	resultMax      = 16384 // bytes of one posted result
	weeks          = 12
	days           = 30 // per-day series length
	runFilter      = "sb IN (1, 3, 9)"
)

var (
	festival   = [2]int64{1, 1}
	monthly    = [2]int64{3, 1}
	speedKinds = []struct {
		st   int64
		kind string
	}{{5, "trap"}, {6, "zone"}}
	garageKinds = []string{"livery", "tune", "photo"}
	allClasses  = []string{"D", "C", "B", "A", "S1", "S2", "X", "R"}
	modeNames   = []string{"race", "team", "king", "infection"}
	garageSorts = map[string]string{"likes": "f.likes DESC, f.downloads DESC", "downloads": "f.downloads DESC, f.likes DESC", "new": "f.created DESC"}
	sortOrder   = []string{"likes", "downloads", "new"}
	ranges      = []struct {
		name       string
		span, step int64
	}{{"24h", 86400, 600}, {"7d", 7 * 86400, 3600}, {"30d", 30 * 86400, 4 * 3600}}
)

type siteCache struct {
	mu      sync.Mutex
	routes  []*destination
	cars    map[int64]*carFacts
	history *historyCache
	hmu     sync.Mutex
}

func (s *siteCache) resetHistory() {
	s.hmu.Lock()
	s.history = nil
	s.hmu.Unlock()
}

func letter(cls int64) string {
	if cls >= 0 && int(cls) < len(allClasses) {
		return allClasses[cls]
	}
	return ""
}

func (o *Ops) who(xuid int64, tag string) string { return o.gamertag(tag, xuid) }

// ---------------------------------------------------------------- catalog

type carFacts struct {
	ID      int64
	Name    string
	Make    string
	Year    any
	Class   string
	Country string
	Drive   string
	Body    string
	Top     any
}

func (f *carFacts) obj() pyjson.Obj {
	return pyjson.Obj{{K: "id", V: f.ID}, {K: "name", V: f.Name}, {K: "make", V: f.Make}, {K: "year", V: f.Year}, {K: "class", V: f.Class}, {K: "country", V: f.Country},
		{K: "drive", V: f.Drive}, {K: "body", V: f.Body}, {K: "topSpeed", V: f.Top}}
}

func (o *Ops) catalog() map[int64]*carFacts {
	o.site.mu.Lock()
	defer o.site.mu.Unlock()
	if o.site.cars != nil {
		return o.site.cars
	}
	out := map[int64]*carFacts{}
	if o.C.Cars != nil {
		for _, r := range must(o.C.Cars.DB.Query("SELECT id, name, make, year, class_id, country, drive, body, top_speed_ms FROM v_cars")) {
			body := ""
			if bv := r.Any("body"); bv != nil && r.Str("body") != "None" {
				body = r.Str("body")
			}
			var cls int64 = -1
			if r.Any("class_id") != nil {
				cls = r.Int("class_id")
			}
			out[r.Int("id")] = &carFacts{ID: r.Int("id"), Name: strings.TrimSpace(r.Str("make") + " " + r.Str("name")), Make: r.Str("make"), Year: r.Any("year"), Class: letter(cls),
				Country: r.Str("country"), Drive: r.Str("drive"), Body: body, Top: r.Any("top_speed_ms")}
		}
	}
	o.site.cars = out
	return out
}

func (o *Ops) carName(id int64) string {
	if c, ok := o.catalog()[id]; ok {
		return c.Name
	}
	return must(o.C.Riv.Names()).Car[id]
}

func (o *Ops) carFactsObj(id int64) pyjson.Obj {
	if c, ok := o.catalog()[id]; ok {
		return c.obj()
	}
	return (&carFacts{ID: id, Name: o.carName(id)}).obj()
}

// ---------------------------------------------------------------- record history

type boardKey struct {
	kind string
	id   int64
	cls  int64
}

type runRec struct {
	scorereports.Flags
	Xuid  int64
	Tag   string
	Score float64
	Car   int64
	Cls   int64
	At    float64
}

type recRec struct {
	runRec
	Key   boardKey
	Prev  *recRec
	Until *float64
}

type boardState struct {
	best    map[int64]runRec
	bestOrd []int64
	record  *recRec
}

type history struct {
	boards  *boardsInfo
	records []*recRec
	per     map[boardKey]*boardState
	order   []boardKey
}

type boardsInfo struct {
	routeOf map[int64]struct {
		dest string
		r    *route
	}
	routes map[int64]struct {
		dest string
		r    *route
	}
	events map[int64]*eventRow
}

type eventRow struct {
	ID, Month int64
	Name      string
	TrackName string
	Cls       int64
	hasCls    bool
}

func (o *Ops) newBoards() *boardsInfo {
	b := &boardsInfo{routeOf: map[int64]struct {
		dest string
		r    *route
	}{}, routes: map[int64]struct {
		dest string
		r    *route
	}{}, events: map[int64]*eventRow{}}
	for _, d := range o.routeCatalog() {
		for _, r := range d.Routes {
			for _, t := range r.Tracks {
				b.routeOf[t] = struct {
					dest string
					r    *route
				}{d.Name, r}
			}
		}
	}
	for _, v := range b.routeOf {
		b.routes[v.r.ID] = v
	}
	for _, e := range must(o.C.DB.Query("SELECT id, month, name, trackname, cls FROM rival_events")) {
		er := &eventRow{ID: e.Int("id"), Month: e.Int("month"), Name: e.Str("name"), TrackName: e.Str("trackname")}
		if e.Any("cls") != nil {
			er.Cls, er.hasCls = e.Int("cls"), true
		}
		b.events[er.ID] = er
	}
	return b
}

func (b *boardsInfo) key(sb, st, track, gsid, cls int64, score float64) (boardKey, bool) {
	switch {
	case sb == festival[0] && st == festival[1]:
		if f, ok := b.routeOf[track]; ok && cls >= 0 && int(cls) < len(rivals.ClassLetters) {
			return boardKey{"route", f.r.ID, cls}, true
		}
	case sb == monthly[0] && st == monthly[1]:
		if _, ok := b.events[gsid]; ok {
			maxed := false
			for _, m := range scoreboard.MaxedClasses {
				maxed = maxed || m == cls
			}
			if !maxed {
				return boardKey{"event", gsid, -1}, true
			}
		}
	case scoreboard.SpeedBoards[sb] && gsid > 0 && score <= scoreboard.SpeedCap:
		for _, k := range speedKinds {
			if k.st == st {
				return boardKey{k.kind, gsid, -1}, true
			}
		}
	}
	return boardKey{}, false
}

func (b *boardsInfo) info(k boardKey) pyjson.Obj {
	switch k.kind {
	case "route":
		f := b.routes[k.id]
		return pyjson.Obj{{K: "kind", V: k.kind}, {K: "id", V: k.id}, {K: "name", V: f.dest + " " + f.r.Name}, {K: "class", V: letter(k.cls)}, {K: "unit", V: "s"}}
	case "event":
		e := b.events[k.id]
		cl := ""
		if e.hasCls {
			cl = letter(e.Cls)
		}
		return pyjson.Obj{{K: "kind", V: k.kind}, {K: "id", V: k.id}, {K: "name", V: e.Name}, {K: "class", V: cl}, {K: "unit", V: "s"}, {K: "month", V: e.Month}}
	}
	w := "Zone"
	if k.kind == "trap" {
		w = "Trap"
	}
	return pyjson.Obj{{K: "kind", V: k.kind}, {K: "id", V: k.id}, {K: "name", V: fmt.Sprintf("Speed %s %d", w, k.id)}, {K: "class", V: ""}, {K: "unit", V: "m/s"}}
}

func isTimeKind(kind string) bool { return kind == "route" || kind == "event" }

func (o *Ops) scan() *history {
	h := &history{boards: o.newBoards(), per: map[boardKey]*boardState{}}
	rows := must(o.C.DB.Query(fmt.Sprintf("SELECT s.id, %s AS report_count, %s AS ghost_flag, s.xuid, s.sb, s.st, s.gsid, s.track, s.car, s.cls, s.score, s.submitted, COALESCE(p.gamertag, '') AS tag "+
		"FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid WHERE %s AND ((s.sb = ? AND s.st = ?) OR (s.sb = ? AND s.st = ?) OR s.sb = 9) ORDER BY s.submitted, s.id",
		scorereports.Counts, scorereports.GhostFlag, scorereports.Visible), festival[0], festival[1], monthly[0], monthly[1]))
	for i := range rows {
		r := &rows[i]
		key, ok := h.boards.key(r.Int("sb"), r.Int("st"), r.Int("track"), r.Int("gsid"), r.Int("cls"), r.Float("score"))
		if !ok {
			continue
		}
		lower := isTimeKind(key.kind)
		run := runRec{Flags: scorereports.FieldsOf(r), Xuid: r.Int("xuid"), Tag: r.Str("tag"), Score: r.Float("score"), Car: r.Int("car"), Cls: r.Int("cls"), At: r.Float("submitted")}
		b, ok := h.per[key]
		if !ok {
			b = &boardState{best: map[int64]runRec{}}
			h.per[key] = b
			h.order = append(h.order, key)
		}
		better := func(a, c float64) bool {
			if lower {
				return a < c
			}
			return a > c
		}
		mine, has := b.best[run.Xuid]
		if !has {
			b.bestOrd = append(b.bestOrd, run.Xuid)
		}
		if !has || better(run.Score, mine.Score) {
			b.best[run.Xuid] = run
		}
		if cur := b.record; cur == nil || better(run.Score, cur.Score) {
			rec := &recRec{runRec: run, Key: key, Prev: cur}
			if cur != nil {
				at := run.At
				cur.Until = &at
			}
			b.record = rec
			h.records = append(h.records, rec)
		}
	}
	return h
}

type historyCache struct {
	checked, scanned time.Time
	stamp            [3]int64
	data             *history
}

func (o *Ops) recordHistory() *history {
	s := o.site
	s.hmu.Lock()
	defer s.hmu.Unlock()
	now := time.Now()
	cache := s.history
	if cache != nil && now.Sub(cache.checked) < historyRecheck {
		return cache.data
	}
	row := must(o.C.DB.One("SELECT COUNT(*), MAX(id), (SELECT COUNT(*) FROM hidden_scores) FROM score_submissions"))
	stamp := [3]int64{row.IntAt(0), row.IntAt(1), row.IntAt(2)}
	if cache == nil || (cache.stamp != stamp && now.Sub(cache.scanned) >= historyRescan) {
		cache = &historyCache{stamp: stamp, data: o.scan(), scanned: now}
	}
	cache.checked = now
	s.history = cache
	return cache.data
}

func (o *Ops) recordOut(h *history, rec *recRec, now float64) pyjson.Obj {
	out := pyjson.Obj{{K: "submissionId", V: rec.SubmissionID}, {K: "reports", V: rec.Reports}, {K: "suspicious", V: rec.Suspicious}, {K: "board", V: h.boards.info(rec.Key)},
		{K: "xuid", V: rec.Xuid}, {K: "gamertag", V: o.who(rec.Xuid, rec.Tag)}, {K: "score", V: rec.Score}, {K: "car", V: o.carName(rec.Car)}, {K: "carId", V: rec.Car},
		{K: "class", V: letter(rec.Cls)}, {K: "at", V: int64(rec.At)}}
	var prev, margin, until any
	if rec.Prev != nil {
		prev = pyjson.Obj{{K: "xuid", V: rec.Prev.Xuid}, {K: "gamertag", V: o.who(rec.Prev.Xuid, rec.Prev.Tag)}, {K: "score", V: rec.Prev.Score}}
		margin = math.Abs(rec.Prev.Score - rec.Score)
	}
	end := now
	if rec.Until != nil {
		until, end = int64(*rec.Until), *rec.Until
	}
	return append(out, pyjson.Obj{{K: "previous", V: prev}, {K: "margin", V: margin}, {K: "until", V: until}, {K: "held", V: int64(end - rec.At)}}...)
}

func (o *Ops) records(limit, top int) pyjson.Obj {
	h := o.recordHistory()
	now := nowF()
	var current []*recRec
	for _, k := range h.order {
		if r := h.per[k].record; r != nil {
			current = append(current, r)
		}
	}
	held, total := map[int64]int{}, map[int64]int{}
	var holderOrd []int64
	for _, rec := range current {
		if _, ok := held[rec.Xuid]; !ok {
			holderOrd = append(holderOrd, rec.Xuid)
		}
		held[rec.Xuid]++
	}
	tags := map[int64]string{}
	for _, rec := range h.records {
		total[rec.Xuid]++
		tags[rec.Xuid] = rec.Tag
	}
	holders := append([]int64(nil), holderOrd...)
	sort.SliceStable(holders, func(i, j int) bool {
		a, b := holders[i], holders[j]
		if held[a] != held[b] {
			return held[a] > held[b]
		}
		if total[a] != total[b] {
			return total[a] > total[b]
		}
		return strings.ToLower(o.who(a, tags[a])) < strings.ToLower(o.who(b, tags[b]))
	})
	if len(holders) > top {
		holders = holders[:top]
	}
	type close struct {
		gap           float64
		key           boardKey
		first, second runRec
	}
	var closest []close
	for _, k := range h.order {
		b := h.per[k]
		if isTimeKind(k.kind) && len(b.best) >= 2 {
			runs := make([]runRec, 0, len(b.bestOrd))
			for _, x := range b.bestOrd {
				runs = append(runs, b.best[x])
			}
			sort.SliceStable(runs, func(i, j int) bool {
				if runs[i].Score != runs[j].Score {
					return runs[i].Score < runs[j].Score
				}
				return runs[i].At < runs[j].At
			})
			closest = append(closest, close{runs[1].Score - runs[0].Score, k, runs[0], runs[1]})
		}
	}
	sort.SliceStable(closest, func(i, j int) bool {
		if closest[i].gap != closest[j].gap {
			return closest[i].gap < closest[j].gap
		}
		return -math.Max(closest[i].first.At, closest[i].second.At) < -math.Max(closest[j].first.At, closest[j].second.At)
	})
	runObj := func(r runRec) pyjson.Obj {
		return pyjson.Obj{{K: "xuid", V: r.Xuid}, {K: "gamertag", V: o.who(r.Xuid, r.Tag)}, {K: "score", V: r.Score}, {K: "car", V: o.carName(r.Car)}, {K: "carId", V: r.Car}}
	}
	feed := []any{}
	if limit > 0 {
		recs := h.records
		if len(recs) > limit {
			recs = recs[len(recs)-limit:]
		}
		for i := len(recs) - 1; i >= 0; i-- {
			feed = append(feed, o.recordOut(h, recs[i], now))
		}
	}
	sorted := append([]*recRec(nil), current...)
	sort.SliceStable(sorted, func(i, j int) bool { return sorted[i].At < sorted[j].At })
	longest := []any{}
	for i, rec := range sorted {
		if i >= top {
			break
		}
		longest = append(longest, o.recordOut(h, rec, now))
	}
	hold := []any{}
	for _, x := range holders {
		hold = append(hold, pyjson.Obj{{K: "xuid", V: x}, {K: "gamertag", V: o.who(x, tags[x])}, {K: "current", V: held[x]}, {K: "total", V: total[x]}})
	}
	cl := []any{}
	for i, c := range closest {
		if i >= top {
			break
		}
		cl = append(cl, pyjson.Obj{{K: "board", V: h.boards.info(c.key)}, {K: "gap", V: c.gap}, {K: "first", V: runObj(c.first)}, {K: "second", V: runObj(c.second)}})
	}
	return pyjson.Obj{{K: "feed", V: feed}, {K: "longest", V: longest}, {K: "holders", V: hold}, {K: "closest", V: cl},
		{K: "totals", V: pyjson.Obj{{K: "records", V: len(h.records)}, {K: "boards", V: len(current)}, {K: "holders", V: len(held)}}}}
}

// ---------------------------------------------------------------- hall of fame

func (o *Ops) hallOfFame(podium int) pyjson.Obj {
	c := o.C
	nowMonth, nowReroll := must3(c.Riv.Serial(nil))
	names := must(c.Riv.Names()).Car
	type month struct {
		m       int64
		label   string
		current bool
		events  []pyjson.Obj
		rows    [][]pyjson.Obj
	}
	months := map[int64]*month{}
	var order []int64
	for _, e := range must(c.DB.Query("SELECT id, month, reroll, name, trackname, laps, length, cls FROM rival_events ORDER BY month DESC, reroll DESC, idx")) {
		top := must(c.Sb.Top(monthly[0], monthly[1], -1, e.Int("id"), podium))
		live := e.Int("month") == nowMonth && e.Int("reroll") == nowReroll
		if len(top) == 0 && !live {
			continue // a re-rolled set nobody raced
		}
		m, ok := months[e.Int("month")]
		if !ok {
			m = &month{m: e.Int("month"), label: monthLabel(e.Int("month")), current: e.Int("month") == nowMonth}
			months[m.m] = m
			order = append(order, m.m)
		}
		cl := ""
		if e.Any("cls") != nil {
			cl = letter(e.Int("cls"))
		}
		rows := []pyjson.Obj{}
		pl := []any{}
		for _, r := range top {
			row := o.boardRow(r, names)
			rows = append(rows, row)
			pl = append(pl, row)
		}
		m.rows = append(m.rows, rows)
		m.events = append(m.events, pyjson.Obj{{K: "id", V: e.Int("id")}, {K: "name", V: e.Str("name")}, {K: "track", V: e.Str("trackname")}, {K: "laps", V: e.Int("laps")},
			{K: "km", V: pyRound(e.Float("length")/1000.0, 2)}, {K: "class", V: cl}, {K: "podium", V: pl}})
	}
	sort.Slice(order, func(i, j int) bool { return order[i] > order[j] })
	out := []any{}
	for _, k := range order {
		m := months[k]
		type sc struct {
			p   [3]int
			tag string
		}
		score := map[int64]*sc{}
		var ord []int64
		for _, rows := range m.rows {
			for _, r := range rows {
				x := toInt(objAny(r, "xuid"))
				s, ok := score[x]
				if !ok {
					s = &sc{tag: pyjson.PyStr(objAny(r, "gamertag"))}
					score[x] = s
					ord = append(ord, x)
				}
				pos := int(toInt(objAny(r, "position")))
				if pos >= 1 && pos <= 3 {
					s.p[pos-1]++
				}
			}
		}
		sort.SliceStable(ord, func(i, j int) bool {
			a, b := score[ord[i]], score[ord[j]]
			if a.p[0] != b.p[0] {
				return a.p[0] > b.p[0]
			}
			if a.p[0]+a.p[1]+a.p[2] != b.p[0]+b.p[1]+b.p[2] {
				return a.p[0]+a.p[1]+a.p[2] > b.p[0]+b.p[1]+b.p[2]
			}
			if a.p[1] != b.p[1] {
				return a.p[1] > b.p[1]
			}
			return strings.ToLower(a.tag) < strings.ToLower(b.tag)
		})
		var champ any
		if len(ord) > 0 && score[ord[0]].p[0] > 0 {
			s := score[ord[0]]
			champ = pyjson.Obj{{K: "xuid", V: ord[0]}, {K: "gamertag", V: s.tag}, {K: "wins", V: s.p[0]}, {K: "podiums", V: s.p[0] + s.p[1] + s.p[2]}}
		}
		evs := make([]any, len(m.events))
		for i, e := range m.events {
			evs[i] = e
		}
		out = append(out, pyjson.Obj{{K: "month", V: m.m}, {K: "label", V: m.label}, {K: "current", V: m.current}, {K: "events", V: evs}, {K: "champion", V: champ}})
	}
	return pyjson.Obj{{K: "months", V: out}}
}

// ---------------------------------------------------------------- speed traps and zones

func (o *Ops) traps(limit int) pyjson.Obj {
	names := must(o.C.Riv.Names()).Car
	out := pyjson.Obj{}
	for _, k := range speedKinds {
		list := []any{}
		for _, r := range must(o.C.DB.Query("SELECT DISTINCT gsid FROM score_submissions WHERE sb = 9 AND st = ? AND gsid > 0 AND score <= ? ORDER BY gsid", k.st, scoreboard.SpeedCap)) {
			g := r.Int("gsid")
			w := "Zone"
			if k.kind == "trap" {
				w = "Trap"
			}
			list = append(list, pyjson.Obj{{K: "id", V: g}, {K: "name", V: fmt.Sprintf("Speed %s %d", w, g)}, {K: "rows", V: o.boardRows(must(o.C.Sb.Top(9, k.st, -1, g, limit)), names)}})
		}
		out = append(out, pyjson.Pair{K: k.kind + "s", V: list})
	}
	return out
}

// ---------------------------------------------------------------- cars and makes

func (o *Ops) cars(days int64, cls string, limit int) pyjson.Obj {
	since := 0.0
	if days > 0 {
		since = nowF() - float64(days*86400)
	}
	where, args := "submitted >= ? AND "+runFilter, []any{since}
	lt := ""
	for i, l := range rivals.ClassLetters {
		if l == cls {
			lt = cls
			where += " AND cls = ?"
			args = append(args, int64(i))
		}
	}
	cat := o.catalog()
	type perCar struct{ runs, drivers int64 }
	type perMake struct {
		runs    int64
		drivers map[int64]bool
		cars    map[int64]bool
	}
	pc, pm := map[int64]*perCar{}, map[string]*perMake{}
	var carOrd []int64
	for _, r := range must(o.C.DB.Query("SELECT car, xuid, COUNT(*) AS n FROM score_submissions WHERE "+where+" GROUP BY car, xuid", args...)) {
		car := r.Int("car")
		if o.carName(car) == "" {
			continue // a car id the catalog does not know (car pack): nothing to call it
		}
		c, ok := pc[car]
		if !ok {
			c = &perCar{}
			pc[car] = c
			carOrd = append(carOrd, car)
		}
		c.runs += r.Int("n")
		c.drivers++
		if f, ok := cat[car]; ok && f.Make != "" {
			m, ok := pm[f.Make]
			if !ok {
				m = &perMake{drivers: map[int64]bool{}, cars: map[int64]bool{}}
				pm[f.Make] = m
			}
			m.runs += r.Int("n")
			m.drivers[r.Int("xuid")] = true
			m.cars[car] = true
		}
	}
	heldCar, heldMake := map[int64]int{}, map[string]int{}
	h := o.recordHistory()
	for _, k := range h.order {
		rec := h.per[k].record
		if rec != nil && (lt == "" || letter(rec.Cls) == lt) {
			heldCar[rec.Car]++
			mk := ""
			if f, ok := cat[rec.Car]; ok {
				mk = f.Make
			}
			heldMake[mk]++
		}
	}
	top := append([]int64(nil), carOrd...)
	sort.SliceStable(top, func(i, j int) bool {
		a, b := pc[top[i]], pc[top[j]]
		if a.runs != b.runs {
			return a.runs > b.runs
		}
		if a.drivers != b.drivers {
			return a.drivers > b.drivers
		}
		return o.carName(top[i]) < o.carName(top[j])
	})
	if len(top) > limit {
		top = top[:limit]
	}
	country := map[string]string{}
	var catIDs []int64
	for id := range cat {
		catIDs = append(catIDs, id)
	}
	sort.Slice(catIDs, func(i, j int) bool { return catIDs[i] < catIDs[j] })
	for _, id := range catIDs {
		country[cat[id].Make] = cat[id].Country
	}
	classes := map[int64]int64{}
	for _, r := range must(o.C.DB.Query("SELECT cls, COUNT(*) AS n FROM score_submissions WHERE submitted >= ? AND "+runFilter+" GROUP BY cls", since)) {
		classes[r.Int("cls")] = r.Int("n")
	}
	total := int64(0)
	for _, c := range pc {
		total += c.runs
	}
	cars := []any{}
	for _, id := range top {
		cars = append(cars, append(o.carFactsObj(id), pyjson.Pair{K: "runs", V: pc[id].runs}, pyjson.Pair{K: "drivers", V: pc[id].drivers}, pyjson.Pair{K: "records", V: heldCar[id]}))
	}
	var makes []string
	for k := range pm {
		makes = append(makes, k)
	}
	sort.Slice(makes, func(i, j int) bool {
		a, b := pm[makes[i]], pm[makes[j]]
		if a.runs != b.runs {
			return a.runs > b.runs
		}
		return makes[i] < makes[j]
	})
	mk := []any{}
	for _, k := range makes {
		v := pm[k]
		mk = append(mk, pyjson.Obj{{K: "make", V: k}, {K: "country", V: country[k]}, {K: "runs", V: v.runs}, {K: "drivers", V: len(v.drivers)}, {K: "cars", V: len(v.cars)}, {K: "records", V: heldMake[k]}})
	}
	cl := []any{}
	for i, l := range rivals.ClassLetters {
		cl = append(cl, pyjson.Obj{{K: "class", V: l}, {K: "runs", V: classes[int64(i)]}})
	}
	return pyjson.Obj{{K: "days", V: days}, {K: "class", V: lt}, {K: "total", V: total}, {K: "cars", V: cars}, {K: "makes", V: mk}, {K: "classes", V: cl}}
}

func (o *Ops) car(idText string) pyjson.Obj {
	id, ok := pyInt(idText, 10)
	if !ok {
		panic(opsErr(404, "not_found", "unknown car"))
	}
	stats := must(o.C.DB.One("SELECT COUNT(*) AS runs, COUNT(DISTINCT xuid) AS drivers, MIN(submitted) AS first FROM score_submissions WHERE car = ? AND "+runFilter, id))
	if _, known := o.catalog()[id]; !known && stats.Int("runs") == 0 {
		panic(opsErr(404, "not_found", "unknown car"))
	}
	h := o.recordHistory()
	type runRow struct {
		flags scorereports.Flags
		xuid  int64
		tag   string
		score float64
		cls   int64
		car   int64
		at    float64
	}
	best := map[boardKey]runRow{}
	var keys []boardKey
	rows := must(o.C.DB.Query(fmt.Sprintf("SELECT s.id, %s AS report_count, %s AS ghost_flag, s.xuid, s.sb, s.st, s.gsid, s.track, s.car, s.cls, s.score, s.submitted, COALESCE(p.gamertag, '') AS tag "+
		"FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid WHERE s.car = ? AND %s AND s.%s", scorereports.Counts, scorereports.GhostFlag, scorereports.Visible, runFilter), id))
	for i := range rows {
		r := &rows[i]
		key, ok := h.boards.key(r.Int("sb"), r.Int("st"), r.Int("track"), r.Int("gsid"), r.Int("cls"), r.Float("score"))
		if !ok {
			continue
		}
		cur, has := best[key]
		lower := isTimeKind(key.kind)
		sc := r.Float("score")
		better := (lower && sc < cur.score) || (!lower && sc > cur.score)
		if !has || better || (sc == cur.score && r.Float("submitted") < cur.at) {
			if !has {
				keys = append(keys, key)
			}
			best[key] = runRow{scorereports.FieldsOf(r), r.Int("xuid"), r.Str("tag"), sc, r.Int("cls"), r.Int("car"), r.Float("submitted")}
		}
	}
	order := map[string]int{"route": 0, "event": 1, "trap": 2, "zone": 3}
	sort.SliceStable(keys, func(i, j int) bool {
		a, b := keys[i], keys[j]
		if order[a.kind] != order[b.kind] {
			return order[a.kind] < order[b.kind]
		}
		an, bn := pyjson.PyStr(objAny(h.boards.info(a), "name")), pyjson.PyStr(objAny(h.boards.info(b), "name"))
		if an != bn {
			return an < bn
		}
		return a.cls < b.cls
	})
	now := nowF()
	var heldRecs []*recRec
	for _, k := range h.order {
		if rec := h.per[k].record; rec != nil && rec.Car == id {
			heldRecs = append(heldRecs, rec)
		}
	}
	sort.SliceStable(heldRecs, func(i, j int) bool { return heldRecs[i].At > heldRecs[j].At })
	start := int64(now) - weeks*7*86400
	wk := map[int64]int64{}
	for _, r := range must(o.C.DB.Query("SELECT (CAST(submitted AS BIGINT) - ?) / 604800 AS w, COUNT(*) AS n FROM score_submissions WHERE car = ? AND submitted >= ? AND "+runFilter+" GROUP BY w", start, id, start)) {
		wk[r.Int("w")] = r.Int("n")
	}
	drivers := must(o.C.DB.Query("SELECT s.xuid, COALESCE(p.gamertag, '') AS tag, COUNT(*) AS n FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid WHERE s.car = ? AND s."+runFilter+
		" GROUP BY s.xuid, p.gamertag ORDER BY n DESC, s.xuid LIMIT 10", id))
	bestOut := []any{}
	for _, k := range keys {
		r := best[k]
		rec := h.per[k]
		isRec := false
		if rec != nil && rec.record != nil {
			isRec = rec.record.Car == r.car && rec.record.Xuid == r.xuid && rec.record.Score == r.score
		}
		bestOut = append(bestOut, append(flagsObj(r.flags), pyjson.Obj{{K: "board", V: h.boards.info(k)}, {K: "xuid", V: r.xuid}, {K: "gamertag", V: o.who(r.xuid, r.tag)}, {K: "score", V: r.score}, {K: "class", V: letter(r.cls)},
			{K: "at", V: int64(r.at)}, {K: "record", V: isRec}}...))
	}
	recOut := []any{}
	for _, rec := range heldRecs {
		recOut = append(recOut, o.recordOut(h, rec, now))
	}
	top := []any{}
	for _, r := range drivers {
		top = append(top, pyjson.Obj{{K: "xuid", V: r.Int("xuid")}, {K: "gamertag", V: o.who(r.Int("xuid"), r.Str("tag"))}, {K: "runs", V: r.Int("n")}})
	}
	wkOut := []any{}
	for i := int64(0); i < weeks; i++ {
		wkOut = append(wkOut, pyjson.Obj{{K: "start", V: start + i*604800}, {K: "runs", V: wk[i]}})
	}
	var first any
	if stats.Any("first") != nil && stats.Float("first") != 0 {
		first = int64(stats.Float("first"))
	}
	return pyjson.Obj{{K: "car", V: o.carFactsObj(id)}, {K: "runs", V: stats.Int("runs")}, {K: "drivers", V: stats.Int("drivers")}, {K: "firstRun", V: first},
		{K: "best", V: bestOut}, {K: "records", V: recOut}, {K: "topDrivers", V: top}, {K: "weeks", V: wkOut}}
}

// ---------------------------------------------------------------- shared designs (storefront)

func (o *Ops) garage(kind, sort_ string, limit int, offset int64) pyjson.Obj {
	const top = 5
	if !inList(garageKinds, kind) {
		panic(opsErr(400, "bad_request", "kind must be one of "+strings.Join(garageKinds, ", ")))
	}
	orderBy, ok := garageSorts[sort_]
	if !ok {
		panic(opsErr(400, "bad_request", "sort must be one of "+strings.Join(sortOrder, ", ")))
	}
	rows := must(o.C.DB.Query("SELECT f.id, f.owner, f.title, f.description, f.created, f.car_id, f.class_id, f.downloads, f.likes, f.uses, "+
		"COALESCE(p.gamertag, '') AS tag, CASE WHEN f.preview IS NOT NULL AND LENGTH(f.preview) > 0 THEN 1 "+
		"WHEN f.kind = 'photo' AND b.id IS NOT NULL AND b.received >= b.total_size THEN 1 ELSE 0 END AS has_preview "+
		"FROM ugc_files f LEFT JOIN players p ON p.xuid = f.owner LEFT JOIN ugc_blobs b ON b.id = f.blob_id WHERE f.removed = 0 AND f.kind = ? "+
		"ORDER BY "+orderBy+", f.id DESC LIMIT ? OFFSET ?", kind, limit, offset))
	counts := map[string]int64{}
	for _, r := range must(o.C.DB.Query("SELECT kind, COUNT(*) AS n FROM ugc_files WHERE removed = 0 GROUP BY kind")) {
		counts[r.Str("kind")] = r.Int("n")
	}
	creators := must(o.C.DB.Query("SELECT f.owner, COALESCE(p.gamertag, '') AS tag, COUNT(*) AS n, SUM(f.downloads) AS downloads, SUM(f.likes) AS likes "+
		"FROM ugc_files f LEFT JOIN players p ON p.xuid = f.owner WHERE f.removed = 0 AND f.kind IN ('livery', 'tune') "+
		"GROUP BY f.owner, p.gamertag ORDER BY downloads DESC, likes DESC, n DESC, f.owner LIMIT ?", top))
	cnt := pyjson.Obj{}
	for _, k := range garageKinds {
		cnt = append(cnt, pyjson.Pair{K: k, V: counts[k]})
	}
	items := []any{}
	for i := range rows {
		r := &rows[i]
		car := ""
		var carID any
		var cls int64 = -1
		if r.Any("car_id") != nil {
			carID = r.Any("car_id")
			if r.Int("car_id") != 0 {
				car = o.carName(r.Int("car_id"))
			}
		}
		if r.Any("class_id") != nil {
			cls = r.Int("class_id")
		}
		items = append(items, pyjson.Obj{{K: "id", V: r.Int("id")}, {K: "title", V: r.Str("title")}, {K: "description", V: r.Str("description")}, {K: "xuid", V: r.Int("owner")},
			{K: "gamertag", V: o.who(r.Int("owner"), r.Str("tag"))}, {K: "car", V: car}, {K: "carId", V: carID}, {K: "class", V: letter(cls)}, {K: "downloads", V: r.Int("downloads")},
			{K: "likes", V: r.Int("likes")}, {K: "uses", V: r.Int("uses")}, {K: "created", V: int64(r.Float("created"))}, {K: "preview", V: r.Int("has_preview") != 0}})
	}
	cr := []any{}
	for i := range creators {
		r := &creators[i]
		dl, lk := int64(r.Float("downloads")), int64(r.Float("likes"))
		if dl != 0 || lk != 0 {
			cr = append(cr, pyjson.Obj{{K: "xuid", V: r.Int("owner")}, {K: "gamertag", V: o.who(r.Int("owner"), r.Str("tag"))}, {K: "designs", V: r.Int("n")}, {K: "downloads", V: dl}, {K: "likes", V: lk}})
		}
	}
	return pyjson.Obj{{K: "kind", V: kind}, {K: "sort", V: sort_}, {K: "total", V: counts[kind]}, {K: "counts", V: cnt}, {K: "items", V: items}, {K: "creators", V: cr}}
}

func (o *Ops) garagePreview(idText string) []byte {
	id, ok := pyInt(idText, 10)
	if !ok {
		panic(opsErr(404, "not_found", "unknown design"))
	}
	r := must(o.C.DB.One("SELECT f.kind, f.preview, b.data AS original, b.received, b.total_size FROM ugc_files f LEFT JOIN ugc_blobs b ON b.id = f.blob_id WHERE f.id = ? AND f.removed = 0", id))
	var data []byte
	if r != nil && r.Str("kind") == "photo" && r.Any("original") != nil && r.Int("received") >= r.Int("total_size") {
		orig := must(ugc.Unpack(r.Bytes("original")))
		if hasPrefix(orig, "\xff\xd8\xff") || hasPrefix(orig, "\x89PNG\r\n\x1a\n") {
			data = orig
		}
	}
	if len(data) == 0 && r != nil {
		data = must(ugc.Unpack(r.Bytes("preview")))
	}
	if !(hasPrefix(data, "\x89PNG\r\n\x1a\n") || hasPrefix(data, "\xff\xd8\xff")) {
		panic(opsErr(404, "not_found", "no preview"))
	}
	return data
}

func hasPrefix(b []byte, p string) bool { return len(b) >= len(p) && string(b[:len(p)]) == p }

// ---------------------------------------------------------------- population

// Sample writes one stats_samples row (called once a minute), the all-time peak in meta, and drops old samples once an hour.
func (c *Core) Sample(now float64) (int, int) {
	online, lobbies := c.OnlineCount(), c.Xbl.Count()
	mustOK(c.DB.Do(func(t *db.Tx) error {
		if _, err := t.Exec("INSERT INTO stats_samples (at, online, lobbies) VALUES (?,?,?) ON CONFLICT DO NOTHING", now, online, lobbies); err != nil {
			return err
		}
		peak, err := t.One("SELECT value FROM meta WHERE key = 'peak_online'")
		if err != nil {
			return err
		}
		prev := int64(-1)
		if peak != nil {
			head, _, _ := strings.Cut(peak.Str("value"), ":")
			prev, _ = strconv.ParseInt(head, 10, 64)
		}
		if online > 0 && (peak == nil || int64(online) > prev) {
			if _, err := t.Exec("INSERT INTO meta (key, value) VALUES ('peak_online', ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value", fmt.Sprintf("%d:%d", online, int64(now))); err != nil {
				return err
			}
		}
		if int64(now)/sampleSeconds%60 == 0 {
			if _, err := t.Exec("DELETE FROM stats_samples WHERE at < ?", now-keepSamples); err != nil {
				return err
			}
		}
		return nil
	}))
	return online, lobbies
}

// StartSampler runs Sample once a minute.
func (c *Core) StartSampler() {
	go func() {
		for {
			now := nowF()
			time.Sleep(time.Duration((sampleSeconds - math.Mod(now, sampleSeconds)) * float64(time.Second)))
			func() {
				defer func() {
					if p := recover(); p != nil {
						c.Log(nil, fmt.Sprintf("stats sampler: %v", p))
					}
				}()
				c.Sample(nowF())
			}()
		}
	}()
}

func (o *Ops) peak() any {
	r := must(o.C.DB.One("SELECT value FROM meta WHERE key = 'peak_online'"))
	if r == nil {
		return nil
	}
	n, at, _ := strings.Cut(r.Str("value"), ":")
	nv, _ := strconv.ParseInt(n, 10, 64)
	av, _ := strconv.ParseInt(at, 10, 64)
	return pyjson.Obj{{K: "online", V: nv}, {K: "at", V: av}}
}

func (o *Ops) perDay(sql string, start int64) map[int64]int64 {
	out := map[int64]int64{}
	for _, r := range must(o.C.DB.Query(sql, start, start)) {
		out[r.Int("d")] = r.Int("n")
	}
	return out
}

func (o *Ops) history(rng string) pyjson.Obj {
	var span, step int64
	found := false
	var names []string
	for _, r := range ranges {
		names = append(names, r.name)
		if r.name == rng {
			span, step, found = r.span, r.step, true
		}
	}
	if !found {
		panic(opsErr(400, "bad_request", "range must be one of "+strings.Join(names, ", ")))
	}
	now := time.Now().Unix()
	start := (now - span) / step * step
	points := []any{}
	var peakB, peakOnline int64 = 0, -1
	have := false
	for _, r := range must(o.C.DB.Query("SELECT (CAST(at AS BIGINT) - ?) / ? AS b, MAX(online) AS online, MAX(lobbies) AS lobbies FROM stats_samples WHERE at >= ? GROUP BY b ORDER BY b", start, step, start)) {
		t, on := start+r.Int("b")*step, r.Int("online")
		points = append(points, []any{t, on, r.Int("lobbies")})
		if !have || on > peakOnline {
			peakB, peakOnline, have = t, on, true
		}
	}
	day0 := (now/86400 - (days - 1)) * 86400
	signups := o.perDay("SELECT (CAST(created AS BIGINT) - ?) / 86400 AS d, COUNT(*) AS n FROM accounts WHERE created >= ? GROUP BY d", day0)
	runs := o.perDay("SELECT (CAST(submitted AS BIGINT) - ?) / 86400 AS d, COUNT(*) AS n FROM score_submissions WHERE submitted >= ? GROUP BY d", day0)
	racers := o.perDay("SELECT (CAST(submitted AS BIGINT) - ?) / 86400 AS d, COUNT(DISTINCT xuid) AS n FROM score_submissions WHERE submitted >= ? GROUP BY d", day0)
	var peak any
	if have && peakOnline != 0 {
		peak = pyjson.Obj{{K: "online", V: peakOnline}, {K: "at", V: peakB}}
	}
	ds := []any{}
	for i := int64(0); i < days; i++ {
		ds = append(ds, pyjson.Obj{{K: "day", V: day0 + i*86400}, {K: "signups", V: signups[i]}, {K: "runs", V: runs[i]}, {K: "racers", V: racers[i]}})
	}
	return pyjson.Obj{{K: "range", V: rng}, {K: "step", V: step}, {K: "start", V: start}, {K: "end", V: now}, {K: "points", V: points}, {K: "peak", V: peak},
		{K: "allTimePeak", V: o.peak()}, {K: "days", V: ds}}
}

// AddResult is /admin/result: {"mode": race|team|king|infection, "detail": {...}} from the game server (loopback only) -> the stored id.
func (c *Core) AddResultBody(body []byte) (int64, error) {
	if len(body) > resultMax {
		return 0, &valErr{"result too large"}
	}
	var d pyjson.Obj
	if len(body) > 0 {
		v, err := pyjson.Parse(string(body))
		if err != nil {
			return 0, &valErr{err.Error()}
		}
		d, _ = v.(pyjson.Obj)
		if d == nil {
			return 0, &valErr{`expected {"mode": race|team|king|infection, "detail": {...}}`}
		}
	}
	mode, _ := d.Get("mode")
	detail, _ := d.Get("detail")
	ms, isStr := mode.(string)
	_, isObj := detail.(pyjson.Obj)
	if !isStr || !inList(modeNames, ms) || !isObj {
		return 0, &valErr{`expected {"mode": race|team|king|infection, "detail": {...}}`}
	}
	text, err := pyjson.Dumps(detail, pyjson.Opts{Compact: true})
	if err != nil {
		return 0, err
	}
	var id int64
	err = c.DB.Do(func(t *db.Tx) error {
		r, err := t.One("INSERT INTO mode_results (at, mode, detail) VALUES (?,?,?) RETURNING id", nowF(), ms, text)
		if err != nil {
			return err
		}
		id = r.IntAt(0)
		return nil
	})
	return id, err
}

func (o *Ops) ladder(counts map[int64]int64) []any {
	var xs []int64
	for x := range counts {
		xs = append(xs, x)
	}
	sort.Slice(xs, func(i, j int) bool {
		if counts[xs[i]] != counts[xs[j]] {
			return counts[xs[i]] > counts[xs[j]]
		}
		return xs[i] < xs[j]
	})
	out := []any{}
	for i, x := range xs {
		if i >= 5 {
			break
		}
		out = append(out, pyjson.Obj{{K: "xuid", V: x}, {K: "gamertag", V: o.who(x, "")}, {K: "count", V: counts[x]}})
	}
	return out
}

func jint(v any) (int64, bool) {
	switch x := v.(type) {
	case int64:
		return x, true
	case uint64:
		return int64(x), true
	case float64:
		return int64(x), true
	case bool:
		if x {
			return 1, true
		}
		return 0, true
	case string:
		n, err := strconv.ParseInt(strings.TrimSpace(x), 10, 64)
		return n, err == nil
	}
	return 0, false
}

// modes tallies the online events: races and their winners, red vs blue, King winners and crowns, Infection rounds, infections and survivors, the rentals raced.
func (o *Ops) modes() pyjson.Obj {
	var raceN, racePlayers, teamN, red, blue, kingN, infN, infEveryone int64
	raceWins, kingSteals, kingWins, infHits, infSurvived := map[int64]int64{}, map[int64]int64{}, map[int64]int64{}, map[int64]int64{}, map[int64]int64{}
	rentals := map[int64]int64{}
	var longestX int64
	var longestT float64
	haveLongest := false
	var fastest float64
	haveFastest := false
	for _, r := range must(o.C.DB.Query("SELECT mode, detail FROM mode_results ORDER BY at")) {
		v, err := pyjson.Parse(r.Str("detail"))
		if err != nil {
			continue
		}
		d, _ := v.(pyjson.Obj)
		var people []pyjson.Obj
		for _, p := range anyList(objAny(d, "players")) {
			if po, ok := p.(pyjson.Obj); ok {
				people = append(people, po)
			}
		}
		for _, p := range people {
			if truthy(objAny(p, "rental")) && truthy(objAny(p, "car")) {
				car, _ := jint(objAny(p, "car"))
				rentals[car]++
			}
		}
		mode := r.Str("mode")
		winner := int64(0)
		if wv := objAny(d, "winner"); truthy(wv) {
			winner, _ = jint(wv)
		}
		if mode == "race" || mode == "team" {
			raceN++
			racePlayers += int64(len(people))
			if winner != 0 {
				raceWins[winner]++
			}
			if mode == "team" {
				teamN++
				for _, p := range people {
					if x, _ := jint(objAny(p, "xuid")); x == winner {
						if t, ok := jint(objAny(p, "team")); ok && (t == 0 || t == 1) {
							if t == 1 {
								blue++
							} else {
								red++
							}
						}
						break
					}
				}
			}
			continue
		}
		hits := map[int64]int64{}
		for _, hv := range anyList(objAny(d, "hits")) {
			pair := anyList(hv)
			if len(pair) == 2 {
				x, _ := jint(pair[0])
				n, _ := jint(pair[1])
				hits[x] = n
			}
		}
		if mode == "king" {
			kingN++
			for x, n := range hits {
				kingSteals[x] += n
			}
			if winner != 0 {
				kingWins[winner]++
			}
			for _, hv := range anyList(objAny(d, "held")) {
				pair := anyList(hv)
				if len(pair) == 2 {
					x, _ := jint(pair[0])
					t, _ := toFloat(pair[1])
					if !haveLongest || t > longestT {
						longestX, longestT, haveLongest = x, t, true
					}
				}
			}
		} else {
			infN++
			for x, n := range hits {
				infHits[x] += n
			}
			for _, sv := range anyList(objAny(d, "survivors")) {
				x, _ := jint(sv)
				infSurvived[x]++
			}
			if truthy(objAny(d, "everyone")) {
				infEveryone++
				if s, ok := toFloat(objAny(d, "seconds")); ok && s != 0 && (!haveFastest || s < fastest) {
					fastest, haveFastest = s, true
				}
			}
		}
	}
	var avg any
	if raceN > 0 {
		avg = pyRound(float64(racePlayers)/float64(raceN), 1)
	}
	var longest, fast any
	if haveLongest {
		longest = pyjson.Obj{{K: "xuid", V: longestX}, {K: "gamertag", V: o.who(longestX, "")}, {K: "seconds", V: longestT}}
	}
	if haveFastest {
		fast = fastest
	}
	var cars []int64
	for c := range rentals {
		cars = append(cars, c)
	}
	sort.Slice(cars, func(i, j int) bool {
		if rentals[cars[i]] != rentals[cars[j]] {
			return rentals[cars[i]] > rentals[cars[j]]
		}
		return cars[i] < cars[j]
	})
	rl := []any{}
	for i, c := range cars {
		if i >= 5 {
			break
		}
		rl = append(rl, pyjson.Obj{{K: "carId", V: c}, {K: "car", V: o.carName(c)}, {K: "count", V: rentals[c]}})
	}
	return pyjson.Obj{{K: "races", V: pyjson.Obj{{K: "count", V: raceN}, {K: "avgPlayers", V: avg}, {K: "winners", V: o.ladder(raceWins)}}},
		{K: "teams", V: pyjson.Obj{{K: "count", V: teamN}, {K: "red", V: red}, {K: "blue", V: blue}}},
		{K: "king", V: pyjson.Obj{{K: "count", V: kingN}, {K: "winners", V: o.ladder(kingWins)}, {K: "stealers", V: o.ladder(kingSteals)}, {K: "longestCrown", V: longest}}},
		{K: "infection", V: pyjson.Obj{{K: "count", V: infN}, {K: "everyone", V: infEveryone}, {K: "fastest", V: fast}, {K: "infectors", V: o.ladder(infHits)}, {K: "survivors", V: o.ladder(infSurvived)}}},
		{K: "rentals", V: rl}}
}

func (o *Ops) totals() pyjson.Obj {
	d := o.C.DB
	kmOf := map[int64]float64{}
	for _, dd := range o.routeCatalog() {
		for _, r := range dd.Routes {
			for _, t := range r.Tracks {
				kmOf[t] = r.Km
			}
		}
	}
	km := 0.0
	for _, r := range must(d.Query("SELECT track, COUNT(*) AS n FROM score_submissions WHERE sb = ? AND st = ? GROUP BY track", festival[0], festival[1])) {
		km += kmOf[r.Int("track")] * float64(r.Int("n"))
	}
	for _, r := range must(d.Query("SELECT e.length, e.laps, COUNT(*) AS n FROM score_submissions s JOIN rival_events e ON e.id = s.gsid WHERE s.sb = ? AND s.st = ? GROUP BY e.id, e.length, e.laps", monthly[0], monthly[1])) {
		laps := r.Float("laps")
		if laps == 0 {
			laps = 1
		}
		km += r.Float("length") * laps / 1000.0 * float64(r.Int("n"))
	}
	seconds := must(d.One("SELECT COALESCE(SUM(s.score), 0) FROM score_submissions s WHERE s.st = 1 AND s.sb IN (1, 3) AND " + scorereports.Visible)).FloatAt(0)
	fast := must(d.One("SELECT s.xuid, s.score, s.car, s.gsid, COALESCE(p.gamertag, '') AS tag FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid WHERE s.sb = 9 AND "+
		scorereports.Visible+" AND s.score <= ? ORDER BY s.score DESC, s.id LIMIT 1", scoreboard.SpeedCap))
	designs := map[string]int64{}
	for _, r := range must(d.Query("SELECT kind, COUNT(*) AS n FROM ugc_files WHERE removed = 0 GROUP BY kind")) {
		designs[r.Str("kind")] = r.Int("n")
	}
	var topSpeed any
	if fast != nil {
		topSpeed = pyjson.Obj{{K: "xuid", V: fast.Int("xuid")}, {K: "gamertag", V: o.who(fast.Int("xuid"), fast.Str("tag"))}, {K: "score", V: fast.Float("score")},
			{K: "car", V: o.carName(fast.Int("car"))}, {K: "carId", V: fast.Int("car")}}
	}
	dg := pyjson.Obj{}
	for _, k := range garageKinds {
		dg = append(dg, pyjson.Pair{K: k, V: designs[k]})
	}
	return pyjson.Obj{{K: "runs", V: must(o.C.Sb.Count())}, {K: "km", V: int64(math.RoundToEven(km))}, {K: "hours", V: pyRound(seconds/3600.0, 1)},
		{K: "drivers", V: must(d.One("SELECT COUNT(*) FROM players WHERE gamertag <> ''")).IntAt(0)}, {K: "racers", V: must(d.One("SELECT COUNT(DISTINCT xuid) FROM score_submissions")).IntAt(0)},
		{K: "designs", V: dg}, {K: "downloads", V: int64(must(d.One("SELECT COALESCE(SUM(downloads), 0) FROM ugc_files WHERE removed = 0")).FloatAt(0))},
		{K: "topSpeed", V: topSpeed}, {K: "records", V: len(o.recordHistory().records)}, {K: "modes", V: o.modes()}}
}

// siteRoute routes the website statistics: the status and object (or PNG/JPEG bytes), or found=false when `what` is not one of these endpoints.
func (o *Ops) siteRoute(what string, rest []string, q url.Values) (int, any, bool) {
	first := func(key, dflt string) string {
		if v, ok := qfirst(q, key); ok {
			return v
		}
		return dflt
	}
	switch {
	case what == "records" && len(rest) == 0:
		return 200, o.records(int(qInt(q, "limit", 50, 0, 200)), 10), true
	case what == "hall-of-fame" && len(rest) == 0:
		return 200, o.hallOfFame(3), true
	case what == "traps" && len(rest) == 0:
		return 200, o.traps(int(qInt(q, "limit", 10, 1, 50))), true
	case what == "cars":
		if len(rest) == 0 {
			return 200, o.cars(qInt(q, "days", 0, 0, 3650), strings.ToUpper(first("class", "")), int(qInt(q, "limit", 25, 1, 200))), true
		}
		if len(rest) == 1 {
			return 200, o.car(rest[0]), true
		}
	case what == "garage":
		if len(rest) == 0 {
			return 200, o.garage(first("kind", "livery"), first("sort", "likes"), int(qInt(q, "limit", 24, 1, 100)), qInt(q, "offset", 0, 0, 1e6)), true
		}
		if len(rest) == 2 && rest[1] == "preview.png" {
			return 200, o.garagePreview(rest[0]), true
		}
	case what == "history" && len(rest) == 0:
		return 200, o.history(first("range", "24h")), true
	case what == "totals" && len(rest) == 0:
		return 200, o.totals(), true
	}
	return 0, nil, false
}

var _ = diag.Get
