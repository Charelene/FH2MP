package game

import (
	"database/sql"
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"

	"fh2mp/internal/catalog"
	"fh2mp/internal/diag"
	"fh2mp/internal/netv2"
	"fh2mp/internal/omap"
)

// RoadTripServerState.
const (
	RTInitialising = iota
	RTDestinationVote
	RTCarSelect
	RTState3
	RTDriveToEvent
	RTArrivedAtDestination
	RTPrepareForEvent
	RTInEvent
	RTFinaliseEvent
)

// Road Trip timing (milliseconds unless named).
const (
	RTCarClasses     = 6
	TeamEventOffset  = 10000
	RTVoteMs         = 60 * 1000
	RTAllVotedMs     = 10 * 1000
	RTAllPickedMs    = 3 * 1000
	RTCarSelectMs    = 30 * 1000
	RTDriveMs        = 180 * 1000
	RTDriveSpeedMs   = 20.0
	RTDriveDetour    = 1.5
	RTDriveSlackS    = 45.0
	RTRegionLegM     = 3500.0
	RTPrepareMs      = 2 * 60 * 1000
	RTFinaliseMs     = 240 * 1000
	RTFirstArrivedMs = 30 * 1000
)

var (
	rtInitSeconds      = envFloat("FH2MP_RT_INIT", 3.0)
	rtDriveMinMs       = int64(envInt("FH2MP_RT_DRIVE_MIN", 150)) * 1000
	rtDriveMaxMs       = int64(envInt("FH2MP_RT_DRIVE_MAX", 600)) * 1000
	rtTeamMinPlayers   = envInt("FH2MP_TEAM_MIN_PLAYERS", 4)
	rtTeamEventsPerLoc = envInt("FH2MP_TEAM_EVENTS", 1)
	rtTeamOnly         = os.Getenv("FH2MP_RT_TEAM_ONLY") == "1"
	rtSprintOnly       = os.Getenv("FH2MP_RT_SPRINT_ONLY") == "1"
	rtFirstRace        = os.Getenv("FH2MP_RT_FIRST_RACE")
	rtExcludedLocs     = map[int64]bool{9: true}
	rtTeamModes        = []int{0, 1, 2, 4, 5, 31}
	rtHubXZ            = map[int64][2]float64{3: {-3532.0, -2308.0}, 4: {-821.0, -6061.0}, 5: {2241.0, -3100.0}, 6: {9941.0, -7896.0}, 7: {8831.0, -3573.0}, 8: {5111.0, -3528.0}}
	rtFestivalXZ       = [2]float64{0.0, 0.0}
	roadTripDBDefault  = DataDir + "/roadtrip.sqlite"
)

func rtv(name string, v int64) netv2.RTValue { return netv2.RTValue{Name: name, Value: v} }

func isTeamEvent(id int64) bool { return id >= TeamEventOffset }

func baseEventID(id int64) int64 {
	if isTeamEvent(id) {
		return id - TeamEventOffset
	}
	return id
}

// RoadTripDriveMs: how long a group gets for a drive; from nil = a fresh lobby (the festival).
func RoadTripDriveMs(from, to *int64) int64 {
	var straight float64
	if from != nil && to != nil && *from == *to {
		straight = RTRegionLegM
	} else {
		a, okA := rtFestivalXZ, true
		if from != nil {
			a, okA = rtHubXZ[*from]
		}
		var b [2]float64
		okB := false
		if to != nil {
			b, okB = rtHubXZ[*to]
		}
		if !okA || !okB {
			return RTDriveMs
		}
		straight = math.Max(math.Hypot(a[0]-b[0], a[1]-b[1]), RTRegionLegM)
	}
	seconds := straight*RTDriveDetour/RTDriveSpeedMs + RTDriveSlackS
	v := int64(seconds * 1000)
	if v < rtDriveMinMs {
		v = rtDriveMinMs
	}
	if v > rtDriveMaxMs {
		v = rtDriveMaxMs
	}
	return v
}

func openRoadTripDB(path string) (*sql.DB, error) {
	return sql.Open("sqlite", "file:"+strings.ReplaceAll(path, "\\", "/")+"?mode=ro&immutable=1")
}

var _ = catalog.PartNone

// PickRoadTripCandidates draws 3 locations with 4 events each from the game's own event table.
func PickRoadTripCandidates(rng Rand, gamedbPath string, players int) (*RtCand, error) {
	db, err := openRoadTripDB(gamedbPath)
	if err != nil {
		return nil, err
	}
	defer db.Close()
	q := "SELECT e.AnthemOnlineEventId, e.RoadTripLocationId, e.RaceModeId, m.MinimumPlayers FROM AnthemOnlineEvents e JOIN RaceModes m ON m.id = e.RaceModeId " +
		"WHERE e.RoadTripLocationId > 0 AND m.MinimumPlayers <= ? AND e.RaceModeId NOT IN (-1)"
	if rtTeamOnly {
		modes := append([]int(nil), rtTeamModes...)
		sort.Ints(modes)
		ss := make([]string, len(modes))
		for i, m := range modes {
			ss[i] = strconv.Itoa(m)
		}
		q += " AND e.RaceModeId IN (" + strings.Join(ss, ",") + ")"
	}
	rows, err := db.Query(q, maxInt(1, players))
	if err != nil {
		return nil, err
	}
	type row struct{ ev, loc, mode, minp int64 }
	var rs []row
	for rows.Next() {
		var r row
		if err := rows.Scan(&r.ev, &r.loc, &r.mode, &r.minp); err != nil {
			rows.Close()
			return nil, err
		}
		rs = append(rs, r)
	}
	rows.Close()
	byLoc := omap.New[int64, *omap.Map[int64, []int64]]()
	minPlayers := map[int64]int64{}
	eventMode := map[int64]int64{}
	for _, r := range rs {
		if rtSprintOnly && r.mode != 1 && r.mode != 31 {
			continue
		}
		modes, ok := byLoc.Get(r.loc)
		if !ok {
			modes = omap.New[int64, []int64]()
			byLoc.Set(r.loc, modes)
		}
		l, _ := modes.Get(r.mode)
		modes.Set(r.mode, append(l, r.ev))
		minPlayers[r.ev] = r.minp
		eventMode[r.ev] = r.mode
	}
	need := 4
	if rtSprintOnly {
		need = 1
	}
	var eligible []int64
	for _, l := range byLoc.Keys() {
		modes, _ := byLoc.Get(l)
		n := 0
		for _, m := range modes.Keys() {
			v, _ := modes.Get(m)
			n += len(v)
		}
		if !rtExcludedLocs[l] && n >= need {
			eligible = append(eligible, l)
		}
	}
	if len(eligible) < 3 {
		return nil, fmt.Errorf("sample larger than population or is negative")
	}
	locs := sample(rng, eligible, 3)
	out := &RtCand{}
	for li, l := range locs {
		out.Locs[li] = l
		modes, _ := byLoc.Get(l)
		pools := omap.New[int64, []int64]()
		fill := func() {
			pools = omap.New[int64, []int64]()
			for _, m := range modes.Keys() {
				v, _ := modes.Get(m)
				pools.Set(m, sample(rng, v, len(v)))
			}
		}
		fill()
		var picked []int64
		for len(picked) < 4 {
			if rtSprintOnly {
				any := false
				for _, m := range pools.Keys() {
					v, _ := pools.Get(m)
					any = any || len(v) > 0
				}
				if !any {
					fill()
				}
			}
			for _, m := range sample(rng, pools.Keys(), pools.Len()) {
				v, _ := pools.Get(m)
				if len(v) > 0 && len(picked) < 4 {
					picked = append(picked, v[len(v)-1])
					pools.Set(m, v[:len(v)-1])
				}
			}
		}
		if players >= rtTeamMinPlayers || rtTeamOnly {
			var teamable []int
			for i, e := range picked {
				for _, tm := range rtTeamModes {
					if eventMode[e] == int64(tm) {
						teamable = append(teamable, i)
						break
					}
				}
			}
			n := minInt(rtTeamEventsPerLoc, len(teamable))
			if rtTeamOnly {
				n = len(teamable)
			}
			for _, i := range sample(rng, teamable, n) {
				team := picked[i] + TeamEventOffset
				mp := minPlayers[picked[i]]
				if int64(rtTeamMinPlayers) > mp {
					mp = int64(rtTeamMinPlayers)
				}
				minPlayers[team] = mp
				picked[i] = team
			}
		}
		for k := 0; k < 4; k++ {
			out.Events[li][k] = picked[k]
		}
	}
	classes := sample(rng, []int{0, 1, 2, 3, 4, 5}, 3)
	for i := 0; i < 3; i++ {
		out.Classes[i] = classes[i]
		for k := 0; k < 4; k++ {
			out.Mins[i][k] = int(minPlayers[out.Events[i][k]])
		}
	}
	return out, nil
}

func (c *RtCand) selection(votes [3]int) *netv2.EventSelection {
	mins := c.Mins
	return &netv2.EventSelection{Locations: c.Locs, Events: c.Events, Classes: c.Classes, Mins: &mins, Votes: votes}
}

func (w *World) rtRace() int64 { return w.RtCandidates.Events[w.RtWinner][w.RtIdx] }

func (w *World) rtNumLaps(race int64) int64 {
	db, err := openRoadTripDB(w.RtGamedb)
	if err == nil {
		defer db.Close()
		var n int64
		err = db.QueryRow("SELECT NumLaps FROM AnthemOnlineEvents WHERE AnthemOnlineEventId = ?", baseEventID(race)).Scan(&n)
		if err == nil {
			return maxI64(1, n)
		}
		if err == sql.ErrNoRows {
			w.LogEvery([3]any{"rt-no-row", "laps", race}, 300.0, fmt.Sprintf("road trip: event %d (row %d) is not in AnthemOnlineEvents: 1 lap assumed", race, baseEventID(race)), diag.Warn)
			return 1
		}
	}
	w.ExcEvery([2]any{"rt-db", "laps"}, 60.0, fmt.Sprintf("road trip: lap count of event %d unreadable from %s; 1 lap assumed", race, w.RtGamedb), err, diag.Warn)
	return 1
}

func (w *World) rtRaceMode(race int64) (int, bool) {
	db, err := openRoadTripDB(w.RtGamedb)
	if err == nil {
		defer db.Close()
		var n int
		err = db.QueryRow("SELECT RaceModeId FROM AnthemOnlineEvents WHERE AnthemOnlineEventId = ?", baseEventID(race)).Scan(&n)
		if err == nil {
			return n, true
		}
		if err == sql.ErrNoRows {
			w.LogEvery([3]any{"rt-no-row", "mode", race}, 300.0, fmt.Sprintf("road trip: event %d (row %d) is not in AnthemOnlineEvents: no race mode", race, baseEventID(race)), diag.Warn)
			return 0, false
		}
	}
	w.ExcEvery([2]any{"rt-db", "mode"}, 60.0, fmt.Sprintf("road trip: race mode of event %d unreadable from %s; treated as no tag mode", race, w.RtGamedb), err, diag.Warn)
	return 0, false
}

// RtNextEvent: a destination is a block of four events: vote -> drive -> event 1 -> drive -> event 2 ... then vote again.
func (w *World) RtNextEvent() {
	if w.RtWinner < 0 || w.RtIdx+1 >= 4 {
		w.RtNewRound()
		return
	}
	w.RtIdx++
	race := w.rtRace()
	w.Logf("road trip: event %d/%d of the destination is %d", w.RtIdx+1, 4, race)
	w.RtPendingDrive = true
	w.RtEndMs = 0
	w.RtSendConfigure()
	w.RtCfgDeadline = w.Mono() + 12.0
	w.RtDriveNotBefore = w.Mono() + 6.0
	w.RtBroadcast(rtv("CompletedEventCount", int64(w.RtIdx)), rtv("NextRaceId", race))
}

// RtNewRound: a fresh destination vote, like the game's own loop.
func (w *World) RtNewRound() {
	if w.RtGamedb != "" {
		here := 0
		for _, c := range w.Peers.Values() {
			if c.Connected {
				here++
			}
		}
		if cand, err := PickRoadTripCandidates(w.Rng, w.RtGamedb, here); err == nil {
			w.RtCandidates = cand
		} else {
			panic(err)
		}
		w.RtPickPlayers = here
	}
	w.RtVotes = omap.New[*Peer, int]()
	w.RtWinner = -1
	w.RtIdx = 0
	w.RtPendingDrive = false
	w.RtState = RTDestinationVote
	w.RtEndMs = w.NowMs() + RTVoteMs
	if w.RtCandidates != nil {
		w.Logf("road trip: new round, candidates %s", locsRepr(w.RtCandidates))
	} else {
		w.Log("road trip: new round, candidates None")
	}
	for _, q := range w.Peers.Values() {
		if !(q.Connected && q.RtStarted) {
			continue
		}
		var msgs []netv2.Msg
		if w.RtCandidates != nil {
			msgs = append(msgs, netv2.RoadTripEventSelectionUpdate(uint64(q.EsSeq), w.RtCandidates.selection(w.rtCounts())))
			q.EsSeq = (q.EsSeq + 1) % 0xFF
		}
		q.RtSeq = (q.RtSeq + 1) % 0xFF
		msgs = append(msgs, netv2.RoadTripUpdate(uint64(q.RtSeq), rtv("CompletedEventCount", 0), rtv("State", int64(w.RtState)), rtv("StateEndTime", w.RtEndMs)))
		w.Send(q, false, msgs...)
	}
}

func locsRepr(c *RtCand) string { return intList(c.Locs[:]) }

// RtBroadcast sends road trip variables to every started client.
func (w *World) RtBroadcast(values ...netv2.RTValue) {
	for _, q := range w.Peers.Values() {
		if q.Connected && q.RtStarted {
			q.RtSeq = (q.RtSeq + 1) % 0xFF
			w.Send(q, false, netv2.RoadTripUpdate(uint64(q.RtSeq), values...))
		}
	}
}

func (w *World) rtCounts() [3]int {
	var counts [3]int
	for _, idx := range w.RtVotes.Values() {
		if 0 <= idx && idx < 3 {
			counts[idx]++
		}
	}
	return counts
}

// RtVote tallies a vote.
func (w *World) RtVote(peer *Peer, idx int) {
	if w.RtState != RTDestinationVote || !(0 <= idx && idx < 3) {
		why := "index outside 0..2"
		if w.RtState != RTDestinationVote {
			why = fmt.Sprintf("the vote is closed, road trip state %d", w.RtState)
		}
		w.LogEvery([2]any{"vote-ignored", peer.Addr}, 10.0, fmt.Sprintf("%d vote for location index %d ignored (owner %d): %s", peer.Addr.Port, idx, peer.Owner, why), diag.Info)
		return
	}
	w.RtVotes.Set(peer, idx)
	counts := w.rtCounts()
	w.Logf("%d voted for location index %d (%d); tally %s", peer.Addr.Port, idx, w.RtCandidates.Locs[idx], intList(counts[:]))
	for _, q := range w.Peers.Values() {
		if q.Connected && q.RtStarted {
			w.Send(q, false, netv2.RoadTripVoteCountsUpdate(uint64(q.EsSeq), counts))
			q.EsSeq = (q.EsSeq + 1) % 0xFF
		}
	}
	voters := w.rtVoters()
	all := len(voters) > 0
	for _, q := range voters {
		if !w.RtVotes.Has(q) {
			all = false
		}
	}
	if all {
		w.RtEndMs = minI64(w.RtEndMs, w.NowMs()+RTAllVotedMs)
		w.RtBroadcast(rtv("StateEndTime", w.RtEndMs))
		w.Log("everybody voted: closing the vote shortly")
	}
}

func minI64(a, b int64) int64 {
	if a < b {
		return a
	}
	return b
}

func (w *World) rtVoters() []*Peer {
	var out []*Peer
	for _, q := range w.Peers.Values() {
		if q.Connected && q.RtStarted {
			out = append(out, q)
		}
	}
	return out
}

// RtCarPicked: a client's car pick during CarSelect shows up as a large update of its player object (router 1).
func (w *World) RtCarPicked(peer *Peer) {
	if w.RtState != RTCarSelect {
		return
	}
	w.RtPicked[peer] = true
	voters := w.rtVoters()
	all := len(voters) > 0
	for _, q := range voters {
		if !w.RtPicked[q] {
			all = false
		}
	}
	if all {
		w.RtEndMs = minI64(w.RtEndMs, w.NowMs()+RTAllPickedMs)
		w.RtBroadcast(rtv("StateEndTime", w.RtEndMs))
		w.Log("everybody picked a car: ending car select shortly")
	}
}

// RtFinishVote closes the vote.
func (w *World) RtFinishVote() {
	counts := w.rtCounts()
	best := counts[0]
	for _, c := range counts {
		if c > best {
			best = c
		}
	}
	winner := 0
	for i, c := range counts {
		if c == best {
			winner = i
			break
		}
	}
	w.RtWinner = winner
	w.RtState = RTCarSelect
	w.RtEndMs = w.NowMs() + RTCarSelectMs
	loc := w.RtCandidates.Locs[winner]
	w.Logf("vote closed %s: location %d wins; State -> CarSelect", intList(counts[:]), loc)
	race := w.RtCandidates.Events[winner][0]
	w.RtPicked = map[*Peer]bool{}
	w.RtIdx = 0
	w.RtBroadcast(rtv("CurrentLocationId", loc), rtv("NextRaceId", race), rtv("State", RTCarSelect), rtv("StateEndTime", w.RtEndMs))
	w.RtSendConfigure()
}

// RtSendConfigure asks the leader's client to build the GameOptions of the round's event.
func (w *World) RtSendConfigure() {
	race := w.rtRace()
	w.RtConfigured = false
	w.RtCfgDeadline = w.Mono() + RTCarSelectMs/1000 + 5.0
	var host *Peer
	for _, c := range w.Peers.Values() {
		if c.Connected && c.RtStarted && (host == nil || c.Owner < host.Owner) {
			host = c
		}
	}
	if host != nil {
		w.Send(host, false, netv2.RoadTripConfigureGameOptions(uint64(race), 1))
		w.Logf("%d -> ConfigureGameOptionsForRoadTripEvent(%d, 1)", host.Addr.Port, race)
	}
}

// RtCheckReady: PrepareForEvent: once every player is ready (or the phase ran out) the event starts.
func (w *World) RtCheckReady(force bool) {
	var peers, ready []*Peer
	for _, q := range w.Peers.Values() {
		if q.Connected && q.RtStarted {
			peers = append(peers, q)
			if q.ConsoleVars[4] {
				ready = append(ready, q)
			}
		}
	}
	if len(ready) == 0 || !(force || len(ready) == len(peers)) {
		return
	}
	leader := ready[0]
	for _, q := range ready {
		if q.Owner < leader.Owner {
			leader = q
		}
	}
	w.RtEndMs = 0
	w.Logf("road trip: %d/%d players ready: event starts (State stays PrepareForEvent)", len(ready), len(peers))
	w.RtBroadcast(rtv("StateEndTime", 0))
	w.StartEvent(leader)
}

// RtStartDrive: car select is over; everybody drives to the first event of the winning location.
func (w *World) RtStartDrive() {
	loc := w.RtCandidates.Locs[w.RtWinner]
	race := w.rtRace()
	w.RtState = RTDriveToEvent
	for _, q := range w.Peers.Values() {
		q.RtPstate = 0
	}
	prev := race
	if w.RtLastRace != nil {
		prev = *w.RtLastRace
	} else if rtFirstRace != "" {
		if v, err := strconv.ParseInt(rtFirstRace, 0, 64); err == nil {
			prev = v
		}
	}
	var from *int64
	if w.RtLastRace != nil {
		from = w.RtLastLoc
	}
	w.RtDriveMs = RoadTripDriveMs(from, &loc)
	w.RtEndMs = w.NowMs() + w.RtDriveMs
	src := "none: the festival"
	if w.RtLastRace != nil {
		src = fmt.Sprint(prev)
	}
	w.Logf("car select over: State -> DriveToEvent (location %d, event %d, from the end of event %s, %d s)", loc, race, src, w.RtDriveMs/1000)
	w.SetSessionHostBool(SHRaceReady, 1, "road trip: the next event is chosen")
	w.RtBroadcast(rtv("CurrentLocationId", loc), rtv("CompletedEventCount", int64(w.RtIdx)), rtv("EventRaceId", prev), rtv("NextRaceId", race),
		rtv("State", RTDriveToEvent), rtv("StateEndTime", 0))
	if w.RtIdx == 0 {
		w.RtBroadcast(rtv("StateEndTime", w.RtEndMs))
	} else {
		w.RtEndMs = 0
		w.RtDriveWait = true
		w.RtDriveReadyAt = 0
		w.RtDriveDeadline = w.Mono() + 180.0
	}
}

// RoadTripTick drives the Road Trip state machine.
func (w *World) RoadTripTick(now float64) {
	host := -1
	for _, c := range w.Peers.Values() {
		if c.Connected && (host < 0 || c.Owner < host) {
			host = c.Owner
		}
	}
	for _, q := range w.Peers.Values() {
		if !q.Connected || q.RtStarted || now-q.ConnectedAt < rtInitSeconds {
			continue
		}
		q.RtStarted = true
		here := 0
		for _, c := range w.Peers.Values() {
			if c.Connected {
				here++
			}
		}
		if w.RtState == RTInitialising {
			if w.RtGamedb != "" {
				cand, err := PickRoadTripCandidates(w.Rng, w.RtGamedb, here)
				if err != nil {
					panic(err)
				}
				w.RtCandidates = cand
				w.RtPickPlayers = here
				w.RtVotes = omap.New[*Peer, int]()
				w.RtWinner = -1
				w.Logf("road trip candidates: locations %s events %s classes %s", locsRepr(cand), eventsRepr(cand), intList(cand.Classes[:]))
			}
			w.RtState = RTDestinationVote
			w.RtEndMs = w.NowMs() + RTVoteMs
		} else if w.RtState == RTDestinationVote && w.RtGamedb != "" && w.RtVotes.Len() == 0 {
			shown := false
			for _, o := range w.Peers.Values() {
				if o != q && o.Connected && o.RtStarted {
					shown = true
				}
			}
			if here > w.RtPickPlayers && !shown {
				cand, err := PickRoadTripCandidates(w.Rng, w.RtGamedb, here)
				if err != nil {
					panic(err)
				}
				w.RtCandidates = cand
				w.RtPickPlayers = here
				w.Logf("road trip: %d players, vote drawn again: locations %s events %s", here, locsRepr(cand), eventsRepr(cand))
				for _, o := range w.Peers.Values() {
					if o != q && o.Connected && o.RtStarted {
						w.Send(o, false, netv2.RoadTripEventSelectionUpdate(uint64(o.EsSeq), w.RtCandidates.selection(w.rtCounts())))
						o.EsSeq = (o.EsSeq + 1) % 0xFF
					}
				}
			}
		}
		var msgs []netv2.Msg
		if q.Owner == host {
			msgs = append(msgs, netv2.ServerUpdate(1, 0, netv2.RouterBits[1][0], uint64(q.PlayerSeq), hostFlag))
			q.PlayerSeq = (q.PlayerSeq + 1) % 0xFF
		}
		if w.RtCandidates != nil {
			msgs = append(msgs, netv2.RoadTripEventSelectionUpdate(uint64(q.EsSeq), w.RtCandidates.selection(w.rtCounts())))
			q.EsSeq = (q.EsSeq + 1) % 0xFF
		}
		q.RtSeq = (q.RtSeq + 1) % 0xFF
		values := []netv2.RTValue{rtv("State", int64(w.RtState)), rtv("StateEndTime", w.RtEndMs)}
		if w.RtWinner >= 0 {
			values = append(values, rtv("CurrentLocationId", w.RtCandidates.Locs[w.RtWinner]), rtv("NextRaceId", w.rtRace()))
		}
		msgs = append(msgs, netv2.RoadTripUpdate(uint64(q.RtSeq), values...))
		w.Send(q, false, msgs...)
		w.Logf("%d Road Trip session state sent: State=%d", q.Addr.Port, w.RtState)
	}
	if w.RtDriveWait && w.RtState == RTDriveToEvent {
		voters := w.rtVoters()
		allDrive := len(voters) > 0
		for _, q := range voters {
			if q.RtPstate != RTDriveToEvent {
				allDrive = false
			}
		}
		if w.RtDriveReadyAt == 0.0 && allDrive {
			w.RtDriveReadyAt = now + 1.5
		}
		if (w.RtDriveReadyAt != 0 && now >= w.RtDriveReadyAt) || now >= w.RtDriveDeadline {
			w.RtDriveWait = false
			w.RtEndMs = w.NowMs() + w.RtDriveMs
			w.RtBroadcast(rtv("StateEndTime", w.RtEndMs))
			w.Log("clients are waiting: drive countdown started")
		}
	}
	if w.RtPendingDrive && now >= w.RtDriveNotBefore && (w.RtConfigured || now >= w.RtCfgDeadline) {
		w.RtPendingDrive = false
		w.RtStartDrive()
	}
	if w.RtState == RTFinaliseEvent && w.Event != nil && !w.RtPendingDrive {
		all := true
		for _, q := range w.Event.People {
			if q.Connected && q.RtPstate < RTFinaliseEvent {
				all = false
			}
		}
		if all {
			w.EndEvent("event finished", false)
		}
	}
	if w.RtEndMs != 0 && w.NowMs() >= w.RtEndMs {
		switch w.RtState {
		case RTDestinationVote:
			w.RtFinishVote()
		case RTCarSelect:
			if w.RtConfigured || now >= w.RtCfgDeadline {
				w.RtStartDrive()
			}
		case RTPrepareForEvent:
			w.RtCheckReady(true)
		case RTFinaliseEvent:
			w.EndEvent("event finished", false)
		case RTDriveToEvent:
			w.RtState = RTPrepareForEvent
			w.RtEndMs = w.NowMs() + RTPrepareMs
			w.Log("drive over: State -> PrepareForEvent")
			w.RtBroadcast(rtv("State", RTPrepareForEvent), rtv("StateEndTime", w.RtEndMs))
			w.RtCheckReady(false)
		}
	}
}

func hostFlag(b *bitsWriter) {
	b.Bits(LobbyPlayerPlayerHost, 8)
	b.Bits(1, 1)
}

func eventsRepr(c *RtCand) string {
	rows := make([]string, 3)
	for i := range rows {
		rows[i] = intList(c.Events[i][:])
	}
	return "[" + strings.Join(rows, ", ") + "]"
}
