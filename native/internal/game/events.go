package game

import (
	"fmt"
	"math"
	"sort"
	"strings"

	"fh2mp/internal/bits"
	"fh2mp/internal/capacity"
	"fh2mp/internal/diag"
	"fh2mp/internal/gameopts"
	"fh2mp/internal/netv2"
	"fh2mp/internal/omap"
	"fh2mp/internal/playersetup"
	"fh2mp/internal/pyfmt"
	"fh2mp/internal/pyjson"
)

func kv(k string, v any) pyjson.Pair { return pyjson.Pair{K: k, V: v} }

func floatRepr(f float64) string { return pyfmt.ReprFloat(f) }

var playersetupFields = playersetup.Fields

func owners(l []*Peer) string {
	parts := make([]string, len(l))
	for i, q := range l {
		parts[i] = fmt.Sprint(q.Owner)
	}
	return "[" + strings.Join(parts, ", ") + "]"
}

func (e *Event) stage() string {
	switch {
	case e.Racing:
		return "racing"
	case e.Over:
		return "results"
	case e.Begun:
		return "grid"
	}
	return "loading"
}

// EndEvent goes back to the idle lobby: clear the SessionHost race flags so the next event starts clean.
func (w *World) EndEvent(why string, keepLeader bool) {
	ev := w.Event
	now := w.Mono()
	participants, finished := 0, 0
	begun := false
	var seconds any
	if ev != nil {
		participants, begun = len(ev.People), ev.Begun
		finished = ev.Finished.Len()
		if ev.Started != 0 {
			seconds = math.RoundToEven((now-ev.Started)*10) / 10
		}
	}
	w.ev("event_end", kv("why", why), kv("participants", participants), kv("begun", begun), kv("finished", finished), kv("seconds", seconds))
	stage := "no event was running"
	if ev != nil {
		stage = ev.stage()
	}
	msg := fmt.Sprintf("event end: %s; %s", why, stage)
	if ev != nil {
		msg += fmt.Sprintf(", %d participant(s) %s, %d finished, %.0f s after the start", len(ev.People), owners(ev.People), ev.Finished.Len(), now-ev.Started)
	}
	if w.PendingRace != nil && ev == nil {
		msg += fmt.Sprintf("; pending race request of owner %d cleared", w.PendingRace.Leader.Owner)
	}
	if ev == nil || ev.Begun {
		w.Log(msg)
	} else {
		w.Warn(msg)
	}
	if w.Event != nil {
		for _, q := range w.Event.People {
			if q.Connected {
				w.SetConsoleBool(q, ConsoleVarParticipatingInRace, 0, why)
				q.ConsoleVars[ConsoleVarParticipatingInRace] = false
			}
		}
	}
	w.Event = nil
	w.PendingRace = nil
	if !keepLeader {
		former := w.Leader
		if former == nil {
			former = w.GameOptionsOwner
		}
		w.SetLeader(nil, "event over: "+why)
		w.ReleaseOptions(former, "event over: "+why)
	}
	w.RefreshCarPhasing()
	for _, v := range []int{SHInRace, SHRaceParticipantsSelected, SHRaceReady} {
		w.SetSessionHostBool(v, 0, "event over: "+why)
	}
	w.SetRaceClock(0, "event over: "+why)
	for _, q := range w.Peers.Values() {
		delete(q.ConsoleVars, 4)
	}
	if w.Roadtrip {
		w.RtNextEvent()
	}
}

// ---- tag modes ----

func (w *World) tagPeople() []*Peer {
	var out []*Peer
	if w.Event == nil {
		return out
	}
	for _, q := range w.Event.People {
		if q.Connected && q.CarID != nil {
			out = append(out, q)
		}
	}
	return out
}

func (t *Tag) bucket(p *Peer) int {
	if b, ok := t.Buckets.Get(p); ok {
		return b
	}
	return TagBucketNotIt
}

// TagSend sends MsgTagBuckets to every participant.
func (w *World) TagSend(why string, quiet bool) {
	tag := w.Event.Tag
	now := w.Mono()
	tag.SentAt = &now
	people := w.tagPeople()
	for _, q := range people {
		entries := make([]netv2.TagEntry, len(people))
		for i, p := range people {
			entries[i] = netv2.TagEntry{Relay: p != q, ID: uint64(*p.CarID), Owner: uint64(p.Owner), Bucket: uint64(tag.bucket(p))}
		}
		w.Send(q, false, netv2.TagBuckets(entries))
	}
	if !quiet {
		parts := make([]string, len(people))
		for i, p := range people {
			parts[i] = fmt.Sprintf("owner%d=%d", p.Owner, tag.bucket(p))
		}
		w.Logf("tag: buckets sent (%s): %s", why, strings.Join(parts, " "))
	}
}

// TagAssign draws the first It car(s).
func (w *World) TagAssign() {
	tag := w.Event.Tag
	people := w.tagPeople()
	if len(people) == 0 {
		return
	}
	perIt := tag.Mode.PerIt
	nIt := 1
	if perIt != 0 {
		nIt = maxInt(1, (len(people)+perIt-1)/perIt)
	}
	w.TagAccount(tag, w.Mono())
	tag.Buckets = omap.New[*Peer, int]()
	for _, p := range people {
		tag.Buckets.Set(p, TagBucketNotIt)
	}
	firsts := sample(w.Rng, people, minInt(nIt, len(people)))
	tag.First = nil
	for _, p := range firsts {
		tag.First = append(tag.First, p.Xuid)
		tag.Buckets.Set(p, TagBucketIt)
	}
	tag.Assigned = true
	w.Logf("tag: %s starts, %d player(s), It: %s", tag.Mode.Name, len(people), owners(firsts))
	w.TagSend("initial assignment", false)
}

// TagTick runs the tag mode's timers.
func (w *World) TagTick(now float64) {
	ev := w.Event
	tag := ev.Tag
	if tag.AssignAt == nil || ev.Over {
		return
	}
	if !tag.Assigned {
		if now >= *tag.AssignAt {
			w.TagAssign()
			tag.Repeats = nil
			for _, x := range TagAssignRepeats[1:] {
				tag.Repeats = append(tag.Repeats, now+x)
			}
		}
		return
	}
	people := w.tagPeople()
	var its []*Peer
	for _, p := range people {
		if tag.bucket(p) == TagBucketIt {
			its = append(its, p)
		}
	}
	switch {
	case len(people) > 0 && len(its) == 0:
		p := people[w.Rng.Intn(len(people))]
		w.TagAccount(tag, now)
		tag.Buckets.Set(p, TagBucketIt)
		w.Logf("tag: no It car left, owner %d is It now", p.Owner)
		w.TagSend("new It", false)
	case len(tag.Repeats) > 0 && now >= tag.Repeats[0]:
		tag.Repeats = tag.Repeats[1:]
		w.TagSend("repeat", false)
	case now-sentAtOr(tag, now) >= TagResyncS:
		w.TagSend("resync", true)
	}
	if tag.Mode.Name == "Infection" && len(people) > 0 && len(its) == len(people) && len(people) > 1 {
		w.EndRace("everybody is infected")
	} else if tag.EndAt != nil && now >= *tag.EndAt {
		w.EndRace("time limit")
	}
}

func sentAtOr(t *Tag, d float64) float64 {
	if t.SentAt != nil {
		return *t.SentAt
	}
	return d
}

// TagCollision handles MsgTagCollision.
func (w *World) TagCollision(peer *Peer, k1, k2 carKey, oldA, newA, oldB, newB int) {
	ev := w.Event
	if ev == nil || ev.Tag == nil || !ev.Tag.Assigned {
		return
	}
	tag := ev.Tag
	if ev.Over {
		secs := tag.Seconds
		w.LogEvery([2]any{"tag-over", fmt.Sprintf("%p", ev)}, 60.0, fmt.Sprintf("tag: collision from owner %d ignored: the server ended the event (time limit %s s or all infected) but this client still plays it", peer.Owner, pyfmt.FmtG(secs)), diag.Info)
		return
	}
	car := func(k carKey) *Peer {
		switch k.kind {
		case "own":
			return peer
		case "relay":
			for _, q := range w.Peers.Values() {
				if q.Connected && uint64(q.Owner) == k.owner {
					return q
				}
			}
		}
		return nil
	}
	a, b := car(k1), car(k2)
	if a == nil || b == nil || a == b || !inList(ev.People, a) || !inList(ev.People, b) {
		w.LogEvery([2]any{"tag-unknown", peer.Owner}, 10.0, fmt.Sprintf("tag: collision from %d names unknown cars %s %s", peer.Addr.Port, k1.repr(), k2.repr()), diag.Info)
		return
	}
	haveA, haveB := tag.bucket(a), tag.bucket(b)
	if haveA != oldA || haveB != oldB {
		if haveA == newA && haveB == newB {
			return
		}
		w.LogEvery([2]any{"tag-stale", peer.Owner}, 5.0, fmt.Sprintf("tag: collision from owner %d dropped: it reports owner %d %d->%d, owner %d %d->%d, the server has %d and %d (this client missed an update, or two hits crossed)",
			peer.Owner, a.Owner, oldA, newA, b.Owner, oldB, newB, haveA, haveB), diag.Info)
		return
	}
	if (oldA == TagBucketIt) == (oldB == TagBucketIt) || maxInt(newA, newB) > TagBucketIt {
		w.Logf("tag: ignored collision %d->%d / %d->%d", oldA, newA, oldB, newB)
		return
	}
	if tag.Guessed {
		tag.Guessed = false
		itAfter := newB
		if oldA == TagBucketIt {
			itAfter = newA
		}
		if itAfter == TagBucketIt {
			tag.Mode = TagModes[RaceModeInfection]
			w.Log("tag: the It car stayed It: this is Infection, not King")
		}
	}
	w.TagAccount(tag, w.Mono())
	tag.Buckets.Set(a, newA)
	tag.Buckets.Set(b, newB)
	itWas, other := b, a
	if oldA == TagBucketIt {
		itWas, other = a, b
	}
	scorer := itWas
	if tag.Mode.Name == "King" {
		scorer = other
	}
	if scorer.Xuid != 0 {
		if tag.Hits == nil {
			tag.Hits = omap.New[uint64, int]()
		}
		n, _ := tag.Hits.Get(scorer.Xuid)
		tag.Hits.Set(scorer.Xuid, n+1)
	}
	w.Logf("tag: owner %d %d->%d, owner %d %d->%d (reported by %d)", a.Owner, oldA, newA, b.Owner, oldB, newB, peer.Owner)
	w.TagSend("collision", false)
	tag.Repeats = nil
	for _, x := range TagAssignRepeats[1:3] {
		tag.Repeats = append(tag.Repeats, w.Mono()+x)
	}
}

// TagAccount adds the time since the last bucket change to every It car's crown time (only from GO on).
func (w *World) TagAccount(tag *Tag, now float64) {
	if tag.GoAt != nil && tag.Marked != nil && now > math.Max(*tag.GoAt, *tag.Marked) {
		if tag.Held == nil {
			tag.Held = omap.New[uint64, float64]()
		}
		start := math.Max(*tag.GoAt, *tag.Marked)
		for _, p := range tag.Buckets.Keys() {
			b, _ := tag.Buckets.Get(p)
			if b == TagBucketIt && p.Xuid != 0 {
				h, _ := tag.Held.Get(p.Xuid)
				tag.Held.Set(p.Xuid, h+now-start)
			}
		}
	}
	tag.Marked = &now
}

// ReportEvent posts the result of an online event (two or more players) for the website.
func (w *World) ReportEvent(why string) {
	ev := w.Event
	var people []*Peer
	for _, q := range ev.People {
		if q.Xuid != 0 {
			people = append(people, q)
		}
	}
	if len(people) < 2 {
		return
	}
	now := w.Mono()
	var order []*Peer
	for _, q := range people {
		if _, ok := ev.Finished.Get(q); ok && !ev.Quit[q] {
			order = append(order, q)
		}
	}
	sort.SliceStable(order, func(i, j int) bool {
		a, _ := ev.Finished.Get(order[i])
		b, _ := ev.Finished.Get(order[j])
		return a < b
	})
	var players []any
	for _, q := range people {
		var team any
		if ev.Team != nil {
			if t, ok := ev.Team.Get(q); ok {
				team = t
			}
		}
		var finish any
		for i, o := range order {
			if o == q {
				finish = i + 1
			}
		}
		var car any
		if q.ModelID != nil {
			car = *q.ModelID
		}
		players = append(players, pyjson.Obj{{K: "xuid", V: q.Xuid}, {K: "car", V: car}, {K: "rental", V: q.RentalCar != nil}, {K: "team", V: team}, {K: "finish", V: finish}})
	}
	var seconds any
	if ev.RacingSince != 0 {
		seconds = math.RoundToEven((now-ev.RacingSince)*10) / 10
	}
	detail := pyjson.Obj{{K: "players", V: players}, {K: "end", V: why}, {K: "roadtrip", V: w.Roadtrip}, {K: "seconds", V: seconds}}
	mode := ""
	if tag := ev.Tag; tag != nil {
		w.TagAccount(tag, now)
		mode = "king"
		if tag.Mode.Name == "Infection" {
			mode = "infection"
		}
		var hits, held []any
		if tag.Hits != nil {
			for _, x := range tag.Hits.Keys() {
				n, _ := tag.Hits.Get(x)
				hits = append(hits, []any{x, n})
			}
		}
		var winner any = 0
		if tag.Held != nil {
			var best uint64
			bestV := math.Inf(-1)
			for _, x := range tag.Held.Keys() {
				t, _ := tag.Held.Get(x)
				held = append(held, []any{x, math.RoundToEven(t*10) / 10})
				if t > bestV {
					best, bestV = x, t
				}
			}
			if mode == "king" && len(held) > 0 {
				winner = best
			}
		}
		first := []any{}
		for _, x := range tag.First {
			first = append(first, x)
		}
		survivors := []any{}
		if mode == "infection" {
			for _, q := range people {
				if tag.bucket(q) != TagBucketIt {
					survivors = append(survivors, q.Xuid)
				}
			}
		}
		if hits == nil {
			hits = []any{}
		}
		if held == nil {
			held = []any{}
		}
		detail = append(detail, pyjson.Pair{K: "hits", V: hits}, pyjson.Pair{K: "first", V: first}, pyjson.Pair{K: "held", V: held},
			pyjson.Pair{K: "everyone", V: why == "everybody is infected"}, pyjson.Pair{K: "survivors", V: survivors}, pyjson.Pair{K: "winner", V: winner})
	} else {
		mode = "race"
		if ev.Team != nil && ev.Team.Len() > 0 {
			mode = "team"
		}
		var winner any = 0
		if len(order) > 0 {
			winner = order[0].Xuid
		}
		detail = append(detail, pyjson.Pair{K: "winner", V: winner})
	}
	if w.Sinks != nil && w.Sinks.Result != nil {
		go w.Sinks.Result(mode, detail, w.Log)
	}
}

// RetryRefusedReady: an invitee whose Ready was refused on its old car and who has since changed to one that fits is ready after all.
func (w *World) RetryRefusedReady(peer *Peer) {
	if peer.ReadyRetry == nil {
		return
	}
	if w.PendingRace == nil || w.Event != nil || peer == w.PendingRace.Leader || *peer.ReadyRetry != w.RaceRequestedAt {
		peer.ReadyRetry = nil
		return
	}
	rule, err := gameopts.CarRule(w.GameOptions)
	if err != nil {
		w.LogEvery("ready-rule", 60.0, fmt.Sprintf("event.rule_unreadable %s: the refused Ready stays refused after this car change",
			diag.KV(diag.KVP{K: "owner", V: peer.Owner}, diag.KVP{K: "error", V: err.Error()})), diag.Info)
		return
	}
	if !gameopts.AcceptsCar(rule, optsChoices(peer.Choices)) {
		return
	}
	peer.ReadyRetry = nil
	w.Logf("%d car changed to one that meets the event restrictions: the refused Ready counts now", peer.Addr.Port)
	w.SetConsoleBool(peer, 4, 1, "ready request accepted after a car change")
	peer.ConsoleVars[4] = true
	w.CheckInviteesReady()
}

func optsChoices(c *Choices) *gameopts.Choices {
	if c == nil {
		return nil
	}
	return &gameopts.Choices{HaveClass: c.HasClass, Class: uint64(c.Class), PI: c.PI}
}

// CheckInviteesReady shortens the invite window when everybody invited has accepted.
func (w *World) CheckInviteesReady() {
	waiting := 0
	for _, q := range w.Others(w.PendingRace.Leader) {
		if q.Connected && q.Entered && !q.ConsoleVars[4] {
			waiting++
		}
	}
	if waiting == 0 {
		w.PendingRace.Deadline = math.Min(w.PendingRace.Deadline, w.Mono()+1.5)
		w.Log("all invited clients ready -> starting the event now")
	}
}

// RequestRaceBlocked: one event at a time per Free Roam lobby.
func (w *World) RequestRaceBlocked(peer *Peer) bool {
	if w.Roadtrip {
		return false
	}
	ev := w.Event
	if ev != nil && !ev.Over {
		any := false
		for _, q := range ev.People {
			any = any || q.Connected
		}
		if any && w.Mono()-ev.Started < EventBlockMaxSeconds {
			return true
		}
	}
	pr := w.PendingRace
	return pr != nil && pr.Leader != peer && pr.Leader.Connected
}

// AcceptRequestedOptions: a former native owner can start another event without requesting ownership again.
func (w *World) AcceptRequestedOptions(peer *Peer) {
	pending := peer.GoptsPending
	if w.Roadtrip || pending == nil {
		return
	}
	size, payload, laps, err := gameopts.BoundedLaps(pending.Size, pending.Payload, w.FreeRoamLaps, nil, w.FreeRoamEndCondition)
	if err != nil {
		panic(err)
	}
	w.GameOptionsOwner = peer
	w.GameOptions = &gameopts.Options{Size: size, Payload: payload}
	w.FreeRoamLaps = laps
	if et, err := gameopts.EndConditionType(size, payload); err == nil && et != nil {
		w.FreeRoamEndCondition = int64(*et)
	}
	peer.GoptsPending = nil
	rule, _ := gameopts.CarRule(w.GameOptions)
	w.Logf("%d GameOptions accepted from race requester: class/PI (%d, %d, %d)", peer.Addr.Port, rule.Class, rule.Min, rule.Max)
	for _, q := range w.Peers.Values() {
		if q.Connected {
			w.SendGameOptions(q, nil)
		}
	}
}

// CloseFinishedEvent ends an event that only sits on its results screen; returns the peers still on it.
func (w *World) CloseFinishedEvent() map[*Peer]bool {
	ev := w.Event
	if ev == nil || w.Roadtrip {
		return map[*Peer]bool{}
	}
	busy := map[*Peer]bool{}
	for _, q := range ev.People {
		if q.Connected && q.ConsoleVars[3] {
			busy[q] = true
		}
	}
	w.EndEvent("a new race was requested", true)
	return busy
}

// ParticipantLeft: a participant's game reported RaceInProgress=0.
func (w *World) ParticipantLeft(peer *Peer) {
	ev := w.Event
	_, finished := ev.Finished.Get(peer)
	leftEarly := !finished
	ev.People = removeFrom(ev.People, peer)
	when := "before finishing"
	if !leftEarly {
		when = "after finishing"
	}
	stage := "loading"
	switch {
	case ev.Racing:
		stage = "racing"
	case ev.Over:
		stage = "results screen"
	case ev.Begun:
		stage = "grid"
	}
	w.Logf("%d owner %d left the event (RaceInProgress=0): %s, %d participant(s) remain, %s", peer.Addr.Port, peer.Owner, when, len(ev.People), stage)
	w.RefreshCarPhasing()
	if leftEarly && peer.Connected {
		w.SetConsoleBool(peer, ConsoleVarParticipatingInRace, 0, "left the event")
	}
	if len(ev.People) == 0 {
		w.EndEvent("every participant left the event", false)
	}
}

func (w *World) loadState(q *Peer, now float64) string {
	s := fmt.Sprintf("owner%d xuid %#x ", q.Owner, q.Xuid)
	if q.Connected {
		s += fmt.Sprintf("last packet %.1f s ago, ", now-q.LastRx)
	} else {
		s += "not connected, "
	}
	loaded := "None"
	if v, ok := q.ConsoleVars[ConsoleVarHasLoadedCars]; ok {
		loaded = fmt.Sprint(boolInt(v))
	}
	s += fmt.Sprintf("HasLoadedCars=%s, car %s", loaded, modelStr(q))
	if q.CarDoubtAt != 0 {
		s += fmt.Sprintf(" (failed a check %.0f s ago)", now-q.CarDoubtAt)
	}
	return s
}

func boolInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

// DropUnloaded removes participants that never reported HasLoadedCarsForAllParticipants.
func (w *World) DropUnloaded(now float64) {
	ev := w.Event
	var stuck []*Peer
	for _, q := range ev.People {
		if !(q.Connected && q.ConsoleVars[ConsoleVarHasLoadedCars]) {
			stuck = append(stuck, q)
		}
	}
	if len(stuck) == 0 {
		ev.LoadChecked = true
		return
	}
	var drop []*Peer
	var why string
	loadedAfter := func() string {
		var parts []string
		if ev.LoadedAfter != nil {
			for _, q := range ev.LoadedAfter.Keys() {
				t, _ := ev.LoadedAfter.Get(q)
				parts = append(parts, fmt.Sprintf("owner%d+%.1fs", q.Owner, t))
			}
		}
		if len(parts) == 0 {
			return "none"
		}
		return strings.Join(parts, " ")
	}
	states := func() string {
		parts := make([]string, len(stuck))
		for i, q := range stuck {
			parts[i] = w.loadState(q, now)
		}
		return strings.Join(parts, "; ")
	}
	if len(stuck) < len(ev.People) || len(stuck) == 1 {
		ev.LoadChecked = true
		drop, why = stuck, "did not load in time"
		w.Warnf("event load timeout: dropping %s from the event, %d participant(s) remain; %.0f s after the start (limit %.0f s), loaded: %s; dropped: %s",
			owners(stuck), len(ev.People)-len(stuck), now-ev.Started, EventLoadTimeout, loadedAfter(), states())
	} else {
		leader := ev.People[0]
		suspect := stuck[0]
		better := func(a, b *Peer) bool { // key = (not connected, a is not leader, car_doubt_at, car_seen_at, connected_at); max wins
			ka := [5]float64{float64(boolInt(!a.Connected)), float64(boolInt(a != leader)), a.CarDoubtAt, a.CarSeenAt, a.ConnectedAt}
			kb := [5]float64{float64(boolInt(!b.Connected)), float64(boolInt(b != leader)), b.CarDoubtAt, b.CarSeenAt, b.ConnectedAt}
			for i := range ka {
				if ka[i] != kb[i] {
					return ka[i] > kb[i]
				}
			}
			return false
		}
		for _, q := range stuck[1:] {
			if better(q, suspect) {
				suspect = q
			}
		}
		drop, why = []*Peer{suspect}, "the others could not load its car"
		ev.LoadNext = now + LoadSuspectStep
		failed := ""
		if suspect.CarDoubtAt != 0 {
			failed = ", failed a check"
		}
		w.Warnf("event load timeout: nobody loaded; dropping owner %d as the likeliest blocker (car checked %.0f s ago%s), %d participant(s) remain; %.0f s after the start (limit %.0f s), next suspect in %.0f s if still nobody; waiting: %s",
			suspect.Owner, now-suspect.CarSeenAt, failed, len(ev.People)-1, now-ev.Started, EventLoadTimeout, LoadSuspectStep, states())
	}
	for _, q := range drop {
		ev.People = removeFrom(ev.People, q)
		if q.Connected {
			w.SetConsoleBool(q, ConsoleVarParticipatingInRace, 0, why)
		}
	}
	w.RefreshCarPhasing()
	any := false
	for _, q := range ev.People {
		any = any || q.Connected
	}
	if !any {
		w.EndEvent("nobody loaded the event in time", false)
	} else {
		w.MaybeBeginRace()
		w.MaybeGoRace()
	}
}

// EligibleForEvent says whether a peer's car meets the event's class/PI rule.
func (w *World) EligibleForEvent(peer *Peer, rule *gameopts.Rule) bool {
	if w.Roadtrip {
		return true
	}
	var r gameopts.Rule
	if rule == nil {
		var err error
		r, err = gameopts.CarRule(w.GameOptions)
		if err != nil {
			stored := "none stored"
			if w.GameOptions != nil {
				stored = fmt.Sprintf("%d bits stored", w.GameOptions.Size)
			}
			w.Warnf("event admission refused for owner %d: invalid GameOptions (%v; %s)", peer.Owner, err, stored)
			return false
		}
	} else {
		r = *rule
	}
	if gameopts.AcceptsCar(r, optsChoices(peer.Choices)) {
		return true
	}
	cls := "None"
	pi := math.NaN()
	if peer.Choices != nil {
		if peer.Choices.HasClass {
			cls = fmt.Sprint(peer.Choices.Class)
		}
		pi = peer.Choices.PI
	}
	var clsV any
	if peer.Choices != nil && peer.Choices.HasClass {
		clsV = peer.Choices.Class
	}
	var piV any
	if !math.IsNaN(pi) {
		piV = math.RoundToEven(pi * 1000)
	}
	var car any
	if peer.ModelID != nil {
		car = *peer.ModelID
	}
	w.ev("not_eligible", kv("xuid", fmt.Sprintf("%#x", peer.Xuid)), kv("rule_class", r.Class), kv("rule_min", r.Min), kv("rule_max", r.Max), kv("car", car),
		kv("class", clsV), kv("pi", piV), kv("rentable", peer.RentalCar != nil), kv("repaired", peer.PartsRepairCar != nil))
	msg := fmt.Sprintf("%d not eligible: rule class %d PI %d-%d, owner %d car %s class %s PI %.0f", peer.Addr.Port, r.Class, r.Min, r.Max, peer.Owner, modelStr(peer), cls, pi*1000)
	if peer.RentalCar != nil {
		msg += ", rentable model"
	}
	if peer.PartsRepairCar != nil {
		msg += fmt.Sprintf(", repaired to stock car %d", *peer.PartsRepairCar)
	}
	w.Log(msg)
	return false
}

// ValidateEventCars: a participant whose car does not meet the event's rule leaves the EVENT; false only when no participant is left.
func (w *World) ValidateEventCars() bool {
	ev := w.Event
	if ev == nil || ev.CarRule == nil {
		return true
	}
	var wrong []*Peer
	for _, q := range ev.People {
		if !w.EligibleForEvent(q, ev.CarRule) {
			wrong = append(wrong, q)
		}
	}
	if len(wrong) == 0 {
		return true
	}
	for _, q := range wrong {
		ev.People = removeFrom(ev.People, q)
		if q.Connected {
			w.SetConsoleBool(q, ConsoleVarParticipatingInRace, 0, "car does not meet the event restrictions")
			q.ConsoleVars[ConsoleVarParticipatingInRace] = false
		}
	}
	w.Warnf("event: %s out of the event (car does not meet the restrictions, see the 'not eligible' line above); %d participant(s) remain %s", owners(wrong), len(ev.People), owners(ev.People))
	w.RefreshCarPhasing()
	any := false
	for _, q := range ev.People {
		any = any || q.Connected
	}
	if !any {
		w.EndEvent("no participant's car meets the event restrictions", false)
		return false
	}
	return true
}

// StartEvent: participants = the leader plus every client that reported Console.Ready.
func (w *World) StartEvent(leader *Peer) {
	if !w.Roadtrip {
		opt := w.GameOptions
		if opt == nil {
			opt = &gameopts.Options{}
		}
		size, payload, laps, err := gameopts.BoundedLaps(opt.Size, opt.Payload, w.FreeRoamLaps, nil, w.FreeRoamEndCondition)
		if err != nil {
			w.Warnf("event options: lap field unavailable (%v); keeping the last accepted count %d", err, w.FreeRoamLaps)
		} else {
			w.GameOptions = &gameopts.Options{Size: size, Payload: payload}
			w.FreeRoamLaps = laps
			if et, err := gameopts.EndConditionType(size, payload); err == nil && et != nil {
				w.FreeRoamEndCondition = int64(*et)
			}
			for _, q := range w.Peers.Values() {
				if q.Connected {
					w.SendGameOptions(q, nil)
				}
			}
		}
	}
	if !w.EligibleForEvent(leader, nil) {
		w.Logf("event start: leader owner %d's car does not meet the restrictions yet; it is selected and gets the event car select, its car is checked again when every participant has loaded", leader.Owner)
	}
	people := []*Peer{leader}
	excluded := map[*Peer]string{}
	for _, q := range w.Peers.Values() {
		if q == leader || !q.Connected || !q.ConsoleVars[4] {
			if q != leader {
				if !q.Connected {
					excluded[q] = "handshake not finished"
				} else {
					excluded[q] = "never answered the invite with Ready"
				}
			}
			continue
		}
		if !w.EligibleForEvent(q, nil) {
			excluded[q] = "car does not meet the event restrictions (see the 'not eligible' line above)"
			w.SetConsoleBool(q, 4, 0, "car does not meet the event restrictions")
			q.ConsoleVars[4] = false
			w.SetConsoleBool(q, ConsoleVarParticipatingInRace, 0, "ineligible car")
			q.ConsoleVars[ConsoleVarParticipatingInRace] = false
		} else if len(people) >= capacity.ClientLimit {
			excluded[q] = fmt.Sprintf("the event holds %d players", capacity.ClientLimit)
			w.SetConsoleBool(q, 4, 0, "event full")
			q.ConsoleVars[4] = false
			w.SetConsoleBool(q, ConsoleVarParticipatingInRace, 0, "event full")
			q.ConsoleVars[ConsoleVarParticipatingInRace] = false
		} else {
			people = append(people, q)
		}
	}
	invite := ""
	if w.RaceRequestedAt != 0 && !w.Roadtrip {
		invite = fmt.Sprintf(", invite sent %.0f s ago", w.Mono()-w.RaceRequestedAt)
	}
	w.Logf("event start: %d participant(s): %s; leader owner %d, %d of %d other client(s) excluded%s", len(people), owners(people), leader.Owner, len(excluded), w.Peers.Len()-1, invite)
	for _, q := range w.Peers.Values() {
		if q != leader {
			tail := "in the event"
			if why, ok := excluded[q]; ok {
				tail = "excluded: " + why
			}
			w.Logf("  other client owner %d: connected=%s Console vars=%s; %s", q.Owner, pyBool(q.Connected), consoleVarsRepr(q.ConsoleVars), tail)
		}
	}
	var laps *int64
	if !w.Roadtrip && w.FreeRoamEndCondition == 1 {
		l := w.FreeRoamLaps
		laps = &l
	}
	if w.Roadtrip && w.RtWinner >= 0 && w.RtGamedb != "" {
		l := w.rtNumLaps(w.rtRace())
		laps = &l
	}
	now := w.Mono()
	ev := &Event{People: people, Started: now, NumLaps: laps, Finished: omap.New[*Peer, float64](), Quit: map[*Peer]bool{}}
	w.Event = ev
	if !w.Roadtrip {
		rule, _ := gameopts.CarRule(w.GameOptions)
		ev.CarRule = &rule
	}
	mode := -1
	if w.Roadtrip && w.RtWinner >= 0 && w.RtGamedb != "" {
		if m, ok := w.rtRaceMode(w.rtRace()); ok {
			mode = m
		}
	}
	if w.Roadtrip && w.RtWinner >= 0 && w.RtCandidates != nil {
		race := w.rtRace()
		r := race
		w.RtLastRace = &r
		loc := w.RtCandidates.Locs[w.RtWinner]
		w.RtLastLoc = &loc
		if isTeamEvent(race) {
			w.AssignTeams(people, fmt.Sprintf("%d (event %d as a team race)", race, baseEventID(race)))
		}
	}
	guessed := false
	if mode < 0 && !w.Roadtrip && w.GameOptions != nil {
		size, payload := w.GameOptions.Size, w.GameOptions.Payload
		mode = gameOptionsTagMode(w.GameOptions)
		guessed = mode == RaceModeKing
		desc := "not a tag mode"
		if mode == RaceModeInfection {
			desc = "Infection (header ..600c2000)"
		} else if mode >= 0 {
			desc = "tag mode (King assumed until the first hit shows otherwise)"
		}
		w.Logf("event options: %d bits, header %010x: %s", size, payload.Field(0, 40), desc)
	}
	if !w.Roadtrip && TagModes[mode] == nil && gameopts.TeamsOn(w.GameOptions) {
		w.AssignTeams(people, "free roam, Teams: On")
	}
	if ev.Team == nil {
		for _, q := range people {
			if q.TeamID != 0 {
				w.SetPlayerTeam(q, 0)
			}
		}
	}
	if tm := TagModes[mode]; tm != nil {
		seconds := tm.Seconds
		if !w.Roadtrip && w.GameOptions != nil {
			if chosen, ok, err := gameopts.MaxTime(w.GameOptions.Size, w.GameOptions.Payload); err == nil && ok {
				seconds = chosen
			}
		}
		ev.Tag = &Tag{Mode: tm, Buckets: omap.New[*Peer, int](), Guessed: guessed, Seconds: seconds}
		ev.NumLaps = nil
		where := "(free roam)"
		if w.Roadtrip {
			where = fmt.Sprint(w.rtRace())
		}
		def := ""
		if seconds == tm.Seconds {
			def = " (the mode's default)"
		}
		w.Logf("tag: event %s is %s, time limit %s s%s", where, tm.Name, pyfmt.FmtG(seconds), def)
	}
	for _, q := range people {
		for _, stale := range []int{ConsoleVarHasLoadedCars, ConsoleVarHasGrid, ConsoleVarRenderReady} {
			delete(q.ConsoleVars, stale)
		}
		w.SetConsoleBool(q, ConsoleVarParticipatingInRace, 1, "participant selected")
	}
	w.SetSessionHostBool(SHRaceParticipantsSelected, 1, "participants selected")
	w.RefreshCarPhasing()
}

func consoleVarsRepr(m map[int]bool) string {
	keys := make([]int, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Ints(keys) // Python prints a dict in insertion order; Go keeps no order, ids are written in ascending order in practice
	parts := make([]string, len(keys))
	for i, k := range keys {
		parts[i] = fmt.Sprintf("%d: %d", k, boolInt(m[k]))
	}
	return "{" + strings.Join(parts, ", ") + "}"
}

// AssignTeams: two teams, red (0) and blue (1), alternating over a shuffled order.
func (w *World) AssignTeams(people []*Peer, what string) {
	order := append([]*Peer(nil), people...)
	shuffle(w.Rng, order)
	team := omap.New[*Peer, int]()
	for i, q := range order {
		team.Set(q, i%2)
	}
	w.Event.Team = team
	w.Event.Order = order
	for _, q := range order {
		t, _ := team.Get(q)
		w.SetPlayerTeam(q, t)
	}
	var parts []string
	for _, q := range team.Keys() {
		t, _ := team.Get(q)
		c := "red"
		if t != 0 {
			c = "blue"
		}
		parts = append(parts, fmt.Sprintf("owner%d=%s", q.Owner, c))
	}
	w.Logf("team event: %s, %d player(s): %s", what, len(people), strings.Join(parts, " "))
}

// NoteLoaded notes how long after the event start a participant's HasLoadedCarsForAllParticipants went to 1.
func (w *World) NoteLoaded(peer *Peer) {
	ev := w.Event
	if ev != nil && inList(ev.People, peer) && !ev.Begun {
		if ev.LoadedAfter == nil {
			ev.LoadedAfter = omap.New[*Peer, float64]()
		}
		if !ev.LoadedAfter.Has(peer) {
			ev.LoadedAfter.Set(peer, w.Mono()-ev.Started)
		}
	}
}

// MaybeBeginRace sends the starting grid once every participant loaded its cars.
func (w *World) MaybeBeginRace() {
	ev := w.Event
	if ev == nil || ev.Begun {
		return
	}
	for _, q := range ev.People {
		if !(q.Connected && q.ConsoleVars[5]) {
			return
		}
	}
	if !w.ValidateEventCars() {
		return
	}
	ev.Begun = true
	var loaded []string
	if ev.LoadedAfter != nil {
		for _, q := range ev.LoadedAfter.Keys() {
			t, _ := ev.LoadedAfter.Get(q)
			loaded = append(loaded, fmt.Sprintf("owner%d+%.1fs", q.Owner, t))
		}
	}
	la := "?"
	if len(loaded) > 0 {
		la = strings.Join(loaded, " ")
	}
	w.Logf("all %d participant(s) loaded cars -> starting grid; loaded after %s", len(ev.People), la)
	var order []*Peer
	for _, q := range ev.Order {
		if inList(ev.People, q) {
			order = append(order, q)
		}
	}
	if len(order) != len(ev.People) {
		order = append([]*Peer(nil), ev.People...)
		shuffle(w.Rng, order)
	}
	for _, q := range ev.People {
		entries := make([]netv2.GridEntry, len(order))
		for i, p := range order {
			if p == q {
				entries[i] = netv2.GridEntry{Local: true}
			} else {
				entries[i] = netv2.GridEntry{Owner: uint64(p.Owner)}
			}
		}
		w.Send(q, false, netv2.StartingGrid(entries))
	}
	for _, q := range ev.People {
		delete(q.ConsoleVars, 0)
	}
	now := w.Mono()
	if ev.Tag != nil {
		t := now + TagGridAssignDelay
		ev.Tag.AssignAt = &t
	}
	ev.GridAt, ev.GridReportAt, ev.GridReadyAfter = now, now+RaceGridReportSecs, omap.New[*Peer, float64]()
	g := now + RaceGridBackstopSecs
	ev.GoAt = &g
	slots := make([]string, len(order))
	for i, q := range order {
		slots[i] = fmt.Sprintf("%d=owner%d", i+1, q.Owner)
	}
	w.Logf("grid: sent to %d participant(s), slots %s; the race starts when every one reports RenderReady (backstop %.0f s)", len(ev.People), strings.Join(slots, " "), RaceGridBackstopSecs)
	ids := make([]any, len(ev.People))
	for i, q := range ev.People {
		ids[i] = q.Owner
	}
	w.ev("grid_sent", kv("participants", ids))
}

// MaybeGoRace releases the race once everyone's starting-grid screen is up.
func (w *World) MaybeGoRace() {
	ev := w.Event
	if ev != nil && ev.GoAt != nil {
		for _, q := range ev.People {
			if !(q.Connected && q.ConsoleVars[0]) {
				return
			}
		}
		w.GoRace("all ready")
	}
}

// NoteRenderReady notes a participant's Console.RenderReady change.
func (w *World) NoteRenderReady(peer *Peer, value int) {
	ev, now := w.Event, w.Mono()
	if ev == nil || !inList(ev.People, peer) || ev.GridAt == 0 {
		return
	}
	if ev.GoAt != nil {
		ready := ev.GridReadyAfter
		if value != 0 && !ready.Has(peer) {
			ready.Set(peer, now-ev.GridAt)
			var missing []string
			for _, q := range ev.People {
				if q.Connected && !q.ConsoleVars[0] {
					missing = append(missing, fmt.Sprintf("owner%d", q.Owner))
				}
			}
			tail := ""
			if len(missing) > 0 {
				tail = ", waiting for " + strings.Join(missing, " ")
			}
			t, _ := ready.Get(peer)
			w.Logf("grid: owner%d RenderReady %.1f s after the grid, %d/%d ready%s", peer.Owner, t, ready.Len(), len(ev.People), tail)
			w.ev("grid_ready", kv("owner", peer.Owner), kv("seconds", math.RoundToEven(t*10)/10), kv("ready", ready.Len()), kv("of", len(ev.People)))
		} else if value == 0 && ready.Has(peer) {
			t, _ := ready.Get(peer)
			ready.Delete(peer)
			w.Logf("grid: owner%d cleared RenderReady %.1f s after the grid (it was ready after %.1f s): its client restarted its pre-race flow", peer.Owner, now-ev.GridAt, t)
		}
	} else if value != 0 && ev.LeftBehind[peer] {
		delete(ev.LeftBehind, peer)
		w.Logf("grid: owner%d RenderReady %.1f s after the grid, %.1f s AFTER the race started without it: that client missed the start and stays on the grid screen", peer.Owner, now-ev.GridAt, now-ev.GoTime)
		w.ev("grid_ready_late", kv("owner", peer.Owner), kv("seconds", math.RoundToEven((now-ev.GridAt)*10)/10), kv("after_go", math.RoundToEven((now-ev.GoTime)*10)/10))
	}
}

// GridWaitReport says who the grid waits for and what that client has been doing.
func (w *World) GridWaitReport(now float64) {
	ev := w.Event
	if now < ev.GridReportAt {
		return
	}
	ev.GridReportAt = now + RaceGridReportSecs
	var waiting []*Peer
	for _, q := range ev.People {
		if !q.ConsoleVars[0] {
			waiting = append(waiting, q)
		}
	}
	if len(waiting) == 0 {
		return
	}
	rows := make([]string, len(waiting))
	for i, q := range waiting {
		row := fmt.Sprintf("owner%d xuid %#x ", q.Owner, q.Xuid)
		if q.Connected {
			keys := make([]int, 0)
			for k := range q.ConsoleVars {
				if _, ok := ConsoleBoolVarNames[k]; ok {
					keys = append(keys, k)
				}
			}
			sort.Ints(keys)
			vars := "none reported"
			if len(keys) > 0 {
				ps := make([]string, len(keys))
				for j, k := range keys {
					ps[j] = fmt.Sprintf("%s=%d", ConsoleBoolVarNames[k], boolInt(q.ConsoleVars[k]))
				}
				vars = strings.Join(ps, " ")
			}
			row += fmt.Sprintf("last packet %.1f s ago, vars %s", now-q.LastRx, vars)
		} else {
			row += "not connected"
		}
		rows[i] = row
	}
	w.Logf("grid: %.0f s after the grid, waiting for %s", now-ev.GridAt, strings.Join(rows, "; "))
}

// NoteLapScore handles a LapScore: elapsed time, lap timings and lap number.
func (w *World) NoteLapScore(peer *Peer, lap int, blobs []uint64) {
	ev := w.Event
	if ev != nil && ev.Racing && inList(ev.People, peer) {
		if ev.Laps == nil {
			ev.Laps = omap.New[*Peer, int]()
		}
		cur, _ := ev.Laps.Get(peer)
		ev.Laps.Set(peer, maxInt(cur, lap))
		w.PublishProgress()
	}
	if ev == nil || !ev.Racing || !inList(ev.People, peer) || ev.Finished.Has(peer) {
		return
	}
	totalT := math.Float64frombits(blobs[0])
	lapT := math.Float64frombits(blobs[2])
	if ev.NumLaps != nil && *ev.NumLaps != 0 {
		if int64(lap) < *ev.NumLaps {
			return
		}
	} else if lap < 1 || math.Abs(lapT-totalT) > 1e-6 {
		return
	}
	ev.Finished.Set(peer, w.Mono())
	w.Logf("%d finished in %.3f s (%d/%d finished)", peer.Addr.Port, totalT, ev.Finished.Len(), len(ev.People))
}

// NoteScoreKeyframe handles a player's final score keyframe.
func (w *World) NoteScoreKeyframe(peer *Peer) {
	ev := w.Event
	if ev == nil || !ev.Racing || (ev.NumLaps != nil && *ev.NumLaps != 0) || !inList(ev.People, peer) || ev.Finished.Has(peer) {
		return
	}
	if ev.Tag != nil && w.Mono()-orNow(ev.RacingSince, w.Mono()) < TagKeyframeMinSecs {
		return
	}
	ev.Finished.Set(peer, w.Mono())
	laps := 0
	if ev.Laps != nil {
		laps, _ = ev.Laps.Get(peer)
	}
	if ev.Tag == nil && laps < 1 {
		ev.Quit[peer] = true
		w.Logf("%d final score keyframe without a completed lap: out of the race, not a finisher (%d/%d done)", peer.Addr.Port, ev.Finished.Len(), len(ev.People))
		return
	}
	w.Logf("%d final score keyframe: done (%d/%d finished)", peer.Addr.Port, ev.Finished.Len(), len(ev.People))
}

func orNow(v, now float64) float64 {
	if v == 0 {
		return now
	}
	return v
}

// CheckRaceEnd runs the race-end timers.
func (w *World) CheckRaceEnd(now float64) {
	ev := w.Event
	if ev != nil && ev.Over {
		any := false
		for _, q := range ev.People {
			any = any || q.Connected
		}
		if !any {
			w.EndEvent("every participant is back in free roam or gone", false)
			return
		}
	}
	if ev != nil && ev.ClearAt != nil && now >= *ev.ClearAt {
		ev.ClearAt = nil
		for _, v := range []int{SHRaceParticipantsSelected, SHRaceReady} {
			w.SetSessionHostBool(v, 0, "race over: lobby flags cleared")
		}
		w.SetRaceClock(0, "race over: lobby flags cleared")
	}
	if ev != nil && ev.Tag != nil && !ev.Racing && !ev.Over && ev.Tag.AssignAt != nil {
		w.TagTick(now)
		return
	}
	if ev == nil || !ev.Racing || ev.Over {
		return
	}
	if ev.Tag != nil {
		var people []*Peer
		for _, q := range ev.People {
			if q.Connected {
				people = append(people, q)
			}
		}
		all := ev.Finished.Len() > 0 && len(people) > 0
		for _, q := range people {
			if !ev.Finished.Has(q) {
				all = false
			}
		}
		if all {
			w.EndRace("everybody finished")
			return
		}
		w.TagTick(now)
		return
	}
	if ev.Finished.Len() == 0 {
		return
	}
	first := math.Inf(1)
	have := false
	for _, q := range ev.Finished.Keys() {
		t, _ := ev.Finished.Get(q)
		if !ev.Quit[q] {
			have = true
			first = math.Min(first, t)
		}
	}
	all := true
	for _, q := range ev.People {
		if q.Connected && !ev.Finished.Has(q) {
			all = false
		}
	}
	if all {
		w.EndRace("everybody finished")
	} else if have && now >= first+RaceLastFinisherSecs {
		w.EndRace("time limit after the first finisher")
	}
}

// EndRace sets SessionHost.InRace=0, which the clients' NetworkWaitForRaceEnd waits for.
func (w *World) EndRace(why string) {
	func() {
		defer func() {
			if r := recover(); r != nil {
				w.Exc("event result failed (the website statistics lose this event; the race still ends)", r, diag.Warn)
			}
		}()
		w.ReportEvent(why)
	}()
	ev := w.Event
	t0 := ev.RacingSince
	type fin struct {
		q *Peer
		t float64
	}
	var done []fin
	for _, q := range ev.Finished.Keys() {
		t, _ := ev.Finished.Get(q)
		done = append(done, fin{q, t})
	}
	sort.SliceStable(done, func(i, j int) bool { return done[i].t < done[j].t })
	var rows []string
	if t0 != 0 {
		for _, d := range done {
			r := fmt.Sprintf("owner%d+%.1fs", d.q.Owner, d.t-t0)
			if ev.Quit[d.q] {
				r += "(quit)"
			}
			rows = append(rows, r)
		}
	}
	msg := fmt.Sprintf("race end: %s; finished %d/%d %s", why, len(done), len(ev.People), strings.Join(rows, " "))
	if len(done) < len(ev.People) {
		var nf []string
		for _, q := range ev.People {
			if !ev.Finished.Has(q) {
				s := fmt.Sprintf("owner%d", q.Owner)
				if !q.Connected {
					s += "(gone)"
				}
				nf = append(nf, s)
			}
		}
		msg += "; not finished: " + strings.Join(nf, " ")
	}
	if t0 != 0 {
		msg += fmt.Sprintf("; %.0f s after GO", w.Mono()-t0)
	}
	w.Log(msg)
	ev.Over = true
	ev.Racing = false
	c := w.Mono() + RaceFlagsClearSecs
	ev.ClearAt = &c
	w.SetSessionHostBool(SHInRace, 0, "race over: "+why)
	if w.Roadtrip {
		w.RtState = RTFinaliseEvent
		w.RtEndMs = w.NowMs() + RTFinaliseMs
		w.RtBroadcast(rtv("CompletedEventCount", int64(minInt(w.RtIdx+1, 4))), rtv("State", RTFinaliseEvent), rtv("StateEndTime", w.RtEndMs))
	}
}

// GoRace releases InRace and sends MsgStartRace.
func (w *World) GoRace(why string) {
	ev := w.Event
	if !w.ValidateEventCars() {
		return
	}
	now := w.Mono()
	var late []*Peer
	for _, q := range ev.People {
		if !q.ConsoleVars[0] {
			late = append(late, q)
		}
	}
	if ev.GridAt != 0 {
		ready := ev.GridReadyAfter
		var rr []string
		if ready != nil {
			for _, q := range ready.Keys() {
				t, _ := ready.Get(q)
				rr = append(rr, fmt.Sprintf("owner%d+%.1fs", q.Owner, t))
			}
		}
		rs := "none"
		if len(rr) > 0 {
			rs = strings.Join(rr, " ")
		}
		lb := ""
		if len(late) > 0 {
			ps := make([]string, len(late))
			for i, q := range late {
				ps[i] = fmt.Sprintf("owner%d xuid %#x (last packet %.1f s ago)", q.Owner, q.Xuid, now-q.LastRx)
			}
			lb = "; LEFT BEHIND " + strings.Join(ps, " ")
		}
		w.Logf("grid: race starts (%s) %.1f s after the grid; ready %s%s", why, now-ev.GridAt, rs, lb)
		lbs := []any{}
		for _, q := range late {
			lbs = append(lbs, q.Owner)
		}
		nready := 0
		if ready != nil {
			nready = ready.Len()
		}
		w.ev("race_go", kv("why", why), kv("seconds", math.RoundToEven((now-ev.GridAt)*10)/10), kv("ready", nready), kv("left_behind", lbs))
		ev.GoTime = now
		ev.LeftBehind = map[*Peer]bool{}
		for _, q := range late {
			ev.LeftBehind[q] = true
		}
	}
	ev.GoAt = nil
	ev.Racing = true
	if w.Roadtrip && w.RtState != RTInEvent {
		w.RtState = RTInEvent
		w.RtBroadcast(rtv("State", RTInEvent), rtv("StateEndTime", 0))
	}
	ev.Finished = omap.New[*Peer, float64]()
	ev.RacingSince = w.Mono()
	w.SetSessionHostBool(SHInRace, 1, "race begun")
	if ev.Tag != nil {
		goAt := w.Mono() + RaceCountdownMs/1000.0
		if !ev.Tag.Assigned && ev.Tag.AssignAt == nil {
			ev.Tag.AssignAt = &goAt
		}
		end := goAt + ev.Tag.Seconds
		ev.Tag.EndAt = &end
		g := goAt
		ev.Tag.GoAt = &g
		ev.Tag.Repeats = []float64{w.Mono() + 0.5, w.Mono() + 2.0}
	}
	goMs := (w.NowMs() + RaceCountdownMs) & 0xFFFFFFFF
	w.Logf("MsgStartRace start time %d (server clock now %d)", goMs, w.NowMs())
	if goMs == 0 {
		goMs = 1
	}
	w.SetRaceClock(goMs, "race begun")
	for _, q := range ev.People {
		w.Send(q, false, netv2.StartRace(uint64(goMs)))
	}
}

// Tag-mode detection from the leader's GameOptions.
const (
	gameOptionsTagMinBits      = 400
	gameOptionsInfectionHeader = 0x600C2000
)

var gameOptionsTagBitsMask = bits.FromUint64(0b111).Lsh(10)

// gameOptionsTagMode returns RaceModeKing or RaceModeInfection when the options look like a tag event, else -1.
func gameOptionsTagMode(o *gameopts.Options) int {
	if o == nil {
		return -1
	}
	size, payload := o.Size, o.Payload
	if size >= gameOptionsTagMinBits && payload.Field(0, 32) == gameOptionsInfectionHeader {
		return RaceModeInfection
	}
	if size < gameOptionsTagMinBits || payload.Field(0, 8) != 0 || payload.Field(10, 3) != 0b111 {
		return -1
	}
	return RaceModeKing
}

type carKey struct {
	kind  string
	id    uint64
	owner uint64
}

func (k carKey) repr() string { return fmt.Sprintf("('%s', %d, %d)", k.kind, k.id, k.owner) }
