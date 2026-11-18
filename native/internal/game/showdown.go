package game

import (
	"fmt"
	"math"
	"sort"
	"strings"

	"fh2mp/internal/capacity"
	"fh2mp/internal/gameopts"
	"fh2mp/internal/netv2"
	"fh2mp/internal/omap"
)

// The Car Meet Showdown (the game's wager race): a race of the players at one Car Meet, started from the meet's menu and run inside the
// meet's own session.
//
// From the game binary (1.0.0.64) and its state flow (media/stateflow.zip):
//   - "Start Showdown Race" sends message 26 InviteRequest with type 0 (game+0x7d8ca0; Free Roam is 1, Road Trip 2); "Join" and "Leave"
//     send message 27 with the first bit 0 / 1 and join type 1 (game+0x7d8d40 / +0x7d8b60). The starter's and each joiner's game marks
//     itself ready for the race (network_car_meet_states.xml set_wager_race_ready).
//   - The meet menu shows the Showdown from CarMeetSynced: JoinEndTimeWagerRace (network clock), InviteCountDownTime and the join types
//     (1 = in the Showdown) (view model game+0x830ad0). A Showdown and a Free Roam / Road Trip invite can be open at the same time
//     (NetworkCarMeetWaitForInviteState: WagerRace=1, OnlineFreeRoam=2, OnlineRoadTrip=4 and their sums).
//   - Message 30 to the starter (game+0x9c90d0) makes its game take the GameOptions, fill them from a random row of its own CarMeetEvents
//     table for this meet (a one-lap race) and, once those writes are sent, answer message 31 (game+0x7d4790).
//   - Message 28 to the racers (game+0x9c9140) sends their games to the race start (car_meet.xml launch_wager_race: fast travel to the
//     GameOptions' start, wait for ParticipatingInRace and the cars, route switch), from where the race runs like a Free Roam event's.
//
// One Showdown at a time per meet: the session has one SessionHost race state.

// ShowdownPhase is where a meet's Showdown is.
type ShowdownPhase string

const (
	ShowdownIdle    ShowdownPhase = "idle"
	ShowdownJoining ShowdownPhase = "joining" // the menus count down to JoinEndTimeWagerRace
	ShowdownSetup   ShowdownPhase = "setup"   // message 30 went to the starter; its message 31 is awaited
	ShowdownRacing  ShowdownPhase = "racing"  // message 28 went to the racers; the event engine runs the race

	// ShowdownSetupSeconds: how long the starter's game has after message 30 to fill the GameOptions and answer message 31.
	ShowdownSetupSeconds = 15.0
	// ShowdownEndCounterMarginSecs: after the first finisher the server waits for the game's own EndRaceCounter plus this much (the racers'
	// games end their race themselves when it runs out; the margin covers their packets on the way).
	ShowdownEndCounterMarginSecs = 3.0
)

// Showdown is a Car Meet world's Showdown.
type Showdown struct {
	Phase   ShowdownPhase
	Starter *Peer
	Members *omap.Map[*Peer, struct{}] // in join order
	End     int64                      // JoinEndTimeWagerRace, network clock
	Snap    int64                      // when the state was last sent again (network clock)
	SetupBy float64                    // mono: message 31 is due by then
	Racers  map[*Peer]struct{}         // the racers of the last launched Showdown: the meet's SessionHost race vars go to them only
}

// showdownOpen says whether the Showdown takes joins or is being set up: its members are marked in the meet's join types.
func (w *World) showdownOpen() bool {
	return w.Wager.Phase == ShowdownJoining || w.Wager.Phase == ShowdownSetup
}

// showdownMembers are the Showdown's members still at the meet, the starter first.
func (w *World) showdownMembers() []*Peer {
	s := &w.Wager
	var out []*Peer
	here := func(q *Peer) bool { return q.Connected && q.Entered && q.LobbyType == LobbyTypeCarMeet }
	if s.Starter != nil && s.Members.Has(s.Starter) && here(s.Starter) {
		out = append(out, s.Starter)
	}
	for _, q := range s.Members.Keys() {
		if q != s.Starter && here(q) {
			out = append(out, q)
		}
	}
	return out
}

// showdownSend sends the meet's Showdown vars to everybody at the meet.
func (w *World) showdownSend() {
	end, countdown := w.Wager.End, int64(CMJoinWindowMs)
	if !w.showdownOpen() {
		end = 0
	}
	w.cmBroadcast(netv2.CarMeetFields{Countdown: &countdown, WagerEnd: &end, Joins: w.cmJoins()}, nil)
}

// ShowdownInvite handles "Start Showdown Race" (message 26 with type 0).
func (w *World) ShowdownInvite(peer *Peer) {
	s := &w.Wager
	switch {
	case !w.Carmeet:
		w.Logf("%d Showdown from owner %d ignored: a Showdown starts at a Car Meet and this world is %s", peer.Addr.Port, peer.Owner, w.kindName())
		return
	case s.Phase == ShowdownJoining:
		w.Logf("%d Showdown from owner %d while owner %d's takes players: counted as a join", peer.Addr.Port, peer.Owner, s.Starter.Owner)
		w.ShowdownJoin(peer, false)
		return
	case s.Phase != ShowdownIdle || (w.Event != nil && !w.Event.Over):
		what := fmt.Sprintf("owner %d's Showdown is %s", ownerOf(s.Starter), s.Phase)
		if s.Phase == ShowdownIdle {
			what = fmt.Sprintf("a race of %d player(s) is %s", len(w.Event.People), w.Event.stage())
		}
		w.Logf("%d Showdown from owner %d refused: %s in this meet (one Showdown at a time: the session has one race state)", peer.Addr.Port, peer.Owner, what)
		return
	}
	now := w.NowMs()
	s.Phase, s.Starter, s.Members, s.Snap = ShowdownJoining, peer, omap.New[*Peer, struct{}](), now
	s.End = (now + CMJoinWindowMs) & 0xFFFFFFFF
	s.Members.Set(peer, struct{}{})
	w.showdownSend()
	w.Logf("CarMeet: Showdown started by owner %d, join window %d ms (JoinEndTimeWagerRace %d), %d player(s) at the meet", peer.Owner, CMJoinWindowMs, s.End, len(w.cmMembers()))
}

// ShowdownJoin handles Join (cancel false) and Leave (cancel true) of the Showdown (message 27 with join type 1).
func (w *World) ShowdownJoin(peer *Peer, cancel bool) {
	s := &w.Wager
	if !w.showdownOpen() {
		// a Leave is routine (the game sends one as it comes back to the meet after a race); a Join came too late
		if !cancel {
			w.Logf("%d Showdown join from owner %d ignored: no Showdown takes players in this meet (it is %s)", peer.Addr.Port, peer.Owner, s.Phase)
		}
		return
	}
	if !cancel {
		switch {
		case s.Members.Has(peer):
			return
		case s.Phase != ShowdownJoining:
			w.Logf("%d Showdown join from owner %d too late: owner %d's game is setting the race up", peer.Addr.Port, peer.Owner, ownerOf(s.Starter))
			return
		case len(w.showdownMembers()) >= capacity.ClientLimit:
			w.Logf("%d Showdown join from owner %d refused: the Showdown holds %d players", peer.Addr.Port, peer.Owner, capacity.ClientLimit)
			return
		}
		s.Members.Set(peer, struct{}{})
		w.showdownSend()
		w.Logf("CarMeet: owner %d joined owner %d's Showdown (%d in it)", peer.Owner, s.Starter.Owner, len(w.showdownMembers()))
		return
	}
	if !s.Members.Has(peer) {
		return
	}
	s.Members.Delete(peer)
	left := w.showdownMembers()
	switch {
	case peer == s.Starter && s.Phase == ShowdownSetup:
		w.showdownCallOff(fmt.Sprintf("its starter, owner %d, left while its game set the race up", peer.Owner))
		return
	case peer == s.Starter && len(left) == 0:
		w.showdownCallOff(fmt.Sprintf("its starter, owner %d, left and nobody else is in it", peer.Owner))
		return
	case peer == s.Starter:
		s.Starter = left[0]
		w.Logf("CarMeet: owner %d left its Showdown; owner %d, the next to join, is its starter now (%d in it)", peer.Owner, s.Starter.Owner, len(left))
	case s.Phase == ShowdownSetup && len(left) < 2:
		w.showdownCallOff(fmt.Sprintf("owner %d left while the race was set up and only owner %d is left", peer.Owner, ownerOf(s.Starter)))
		return
	default:
		w.Logf("CarMeet: owner %d left owner %d's Showdown (%d in it)", peer.Owner, s.Starter.Owner, len(left))
	}
	w.showdownSend()
}

// showdownTick runs the Showdown's join window and set-up deadline; nowMs is the world's network clock.
func (w *World) showdownTick(nowMs int64) {
	s := &w.Wager
	switch s.Phase {
	case ShowdownJoining:
		if ((nowMs - s.End) & 0xFFFFFFFF) >= 0x80000000 {
			if nowMs-s.Snap >= 1000 { // the updates are unreliable: the whole state again once a second, as for the Free Roam invite
				s.Snap = nowMs
				w.showdownSend()
			}
			return
		}
		members := w.showdownMembers()
		if len(members) < 2 {
			who := "nobody joined"
			if len(members) == 0 {
				who = "nobody in it is at the meet any more"
			}
			w.showdownCallOff(fmt.Sprintf("join window over and %s", who))
			return
		}
		s.Starter = members[0]
		s.Phase, s.SetupBy = ShowdownSetup, w.Mono()+ShowdownSetupSeconds
		w.Send(s.Starter, false, netv2.CarMeetConfigureWager())
		w.Logf("CarMeet: Showdown join window over, %d player(s) %s: owner %d's game sets the race up (ConfigureGameOptionsForWagerRace sent, %.0f s to answer)",
			len(members), owners(members), s.Starter.Owner, ShowdownSetupSeconds)
	case ShowdownSetup:
		switch {
		case !s.Starter.Connected:
			w.showdownCallOff(fmt.Sprintf("its starter, owner %d, left while its game set the race up", s.Starter.Owner))
		case w.Mono() > s.SetupBy:
			w.showdownCallOff(fmt.Sprintf("owner %d's game sent no ConfiguredGameOptionsForWagerRace within %.0f s of the request (GameOptions owner %s, options %s)",
				s.Starter.Owner, ShowdownSetupSeconds, ownerName(w.GameOptionsOwner), w.optionsState()))
		case len(w.showdownMembers()) < 2:
			w.showdownCallOff("only its starter is still at the meet")
		}
	}
}

// ShowdownConfigured handles message 31 ConfiguredGameOptionsForWagerRace: the starter's game has set the race up, the race starts.
func (w *World) ShowdownConfigured(peer *Peer, value uint64) {
	s := &w.Wager
	if s.Phase != ShowdownSetup || peer != s.Starter {
		why := fmt.Sprintf("the meet's Showdown is %s", s.Phase)
		if s.Phase == ShowdownSetup {
			why = fmt.Sprintf("owner %d's game sets the race up", s.Starter.Owner)
		}
		w.Logf("%d CarMeet ConfiguredGameOptionsForWagerRace value=%#x from owner %d ignored: %s", peer.Addr.Port, value, peer.Owner, why)
		return
	}
	if w.GameOptionsOwner != peer || w.GameOptions == nil {
		w.showdownCallOff(fmt.Sprintf("owner %d's game said the race is set up (value %#x) but the server holds no GameOptions from it (GameOptions owner %s, options %s)",
			peer.Owner, value, ownerName(w.GameOptionsOwner), w.optionsState()))
		return
	}
	racers := w.showdownMembers()
	if len(racers) < 2 {
		w.showdownCallOff("only its starter is still at the meet")
		return
	}
	if w.Event != nil {
		w.EndEvent("a new Showdown starts", true)
	}
	s.Phase, s.Racers = ShowdownRacing, membersOf(racers)
	w.SetLeader(peer, "set the Showdown up")
	w.SetSessionHostBool(SHRaceReady, 1, "Showdown launched")
	w.StartEventWith(peer, racers)
	for _, q := range racers {
		w.Send(q, false, netv2.CarMeetLaunchWagerRace())
	}
	w.showdownSend()
	w.Logf("CarMeet: Showdown launched (owner %d's game set it up, value %#x): LaunchWagerRace sent to %d racer(s) %s", peer.Owner, value, len(racers), owners(racers))
}

// showdownCarsLoaded: a Showdown racer's game cleared HasGrid during the race setup. The Car Meet flow sets no HasLoadedCarsForAllParticipants:
// after the cars-loaded wait it clears HasGrid and waits for the route switch (car_meet.xml car_loading_cutscene_for_wager_race; Free Roam and
// Road Trip set the flag first, network_freeroam_states.xml:468, network_roadtrip_states.xml:852). So that clear is its "cars loaded", and the
// grid (MaybeBeginRace) goes out once every racer has sent it. HasGrid set before the start is dropped at StartEvent, so only a clear after it
// counts.
func (w *World) showdownCarsLoaded(peer *Peer, addr Addr) {
	ev := w.Event
	if !w.Carmeet || w.Wager.Phase != ShowdownRacing || ev == nil || ev.Begun || !ev.has(peer) || peer.ConsoleVars[ConsoleVarHasLoadedCars] {
		return
	}
	peer.ConsoleVars[ConsoleVarHasLoadedCars] = true
	w.NoteLoaded(peer)
	w.Logf("%d Showdown racer owner %d cleared HasGrid after its cars-loaded wait: counted as HasLoadedCarsForAllParticipants (the Car Meet flow sends no such flag)", addr.Port, peer.Owner)
}

// showdownGridOrder puts a Showdown's grid in the order its GameOptions ask for. The starter's game leaves GridOrderingType at its default, PI
// (EGridOrderingType 1, game+0x112800), and sets Ascending, so the car with the lowest performance index takes pole: the only evening-out of
// different classes the game's Showdown has (its FirstCarHeadStartMeters and PerCarRollOffDelay stay 0). The grid order comes from the server
// (MsgStartingGrid); without this it was shuffled. A racer whose PI is not known goes after the known ones.
func (w *World) showdownGridOrder(ev *Event) {
	kind, asc, err := gameopts.GridOrder(w.GameOptions)
	if err != nil {
		w.Warnf("Showdown grid: the GameOptions cannot be read (%v); the grid order stays as it is", err)
		return
	}
	if kind != gameopts.GridOrderPI {
		w.Logf("Showdown grid: GridOrderingType %d is not PI; the grid order stays as it is", kind)
		return
	}
	pi := func(q *Peer) float64 {
		if q.Choices == nil || math.IsNaN(q.Choices.PI) {
			return math.NaN()
		}
		return q.Choices.PI
	}
	order := append([]*Peer(nil), ev.People...)
	sort.SliceStable(order, func(i, j int) bool {
		a, b := pi(order[i]), pi(order[j])
		switch {
		case math.IsNaN(a):
			return false
		case math.IsNaN(b):
			return true
		case asc:
			return a < b
		}
		return a > b
	})
	ev.Order = order
	parts := make([]string, len(order))
	for i, q := range order {
		parts[i] = fmt.Sprintf("owner%d PI %.0f", q.Owner, pi(q)*1000)
	}
	dir := "highest"
	if asc {
		dir = "lowest"
	}
	w.Logf("Showdown grid by PI, %s first (GameOptions GridOrderingType PI, Ascending %s): %s", dir, pyBool(asc), strings.Join(parts, ", "))
}

// showdownEndLimit is how long a Showdown's race runs on after its first finisher: the game's own EndRaceCounter from the options (32 s in the
// starter's options on dev 2026-11-18; each racer's game counts it down and ends its own race) plus ShowdownEndCounterMarginSecs, or the
// server's RaceLastFinisherSecs when the options set none. Said once per event.
func (w *World) showdownEndLimit(ev *Event) float64 {
	counter, ok, err := gameopts.EndRaceCounter(w.GameOptions)
	limit := RaceLastFinisherSecs
	why := "the options set no EndRaceCounter: the server's own limit"
	switch {
	case err != nil:
		why = fmt.Sprintf("the options cannot be read (%v): the server's own limit", err)
	case ok:
		limit = counter + ShowdownEndCounterMarginSecs
		why = fmt.Sprintf("the game's EndRaceCounter %.0f s plus %.0f s", counter, ShowdownEndCounterMarginSecs)
	}
	if !ev.EndLimitLogged {
		ev.EndLimitLogged = true
		w.Logf("Showdown race: it ends %.0f s after the first finisher (%s)", limit, why)
	}
	return limit
}

// showdownCallOff ends a Showdown that never raced and tells the meet's menus.
func (w *World) showdownCallOff(why string) {
	s := &w.Wager
	starter, wasSetup := s.Starter, s.Phase == ShowdownSetup
	s.Phase, s.Starter, s.Members, s.End = ShowdownIdle, nil, omap.New[*Peer, struct{}](), 0
	w.showdownSend()
	tail := ""
	if wasSetup && starter != nil && w.GameOptionsOwner == starter {
		w.SetLeader(nil, "Showdown called off")
		w.ReleaseOptions(starter, "Showdown called off")
		tail = "; GameOptions taken back from owner " + fmt.Sprint(starter.Owner)
	}
	w.Logf("CarMeet: Showdown called off: %s%s", why, tail)
}

// showdownRaceOver: the event engine ended the Showdown's race; the meet can start another.
func (w *World) showdownRaceOver(why string) {
	s := &w.Wager
	if s.Phase != ShowdownRacing {
		return
	}
	s.Phase, s.Starter, s.Members, s.End = ShowdownIdle, nil, omap.New[*Peer, struct{}](), 0
	w.Logf("CarMeet: Showdown over (%s); the meet takes a new Showdown", why)
}

func ownerOf(p *Peer) int {
	if p == nil {
		return 0
	}
	return p.Owner
}

func ownerName(p *Peer) string {
	if p == nil {
		return "nobody"
	}
	return fmt.Sprintf("owner %d", p.Owner)
}

func (w *World) optionsState() string {
	if w.GameOptions == nil {
		return "none stored"
	}
	return fmt.Sprintf("%d bits stored", w.GameOptions.Size)
}

func (w *World) kindName() string {
	switch {
	case w.Carmeet:
		return "a Car Meet"
	case w.Roadtrip:
		return "a Road Trip"
	}
	return "a Free Roam"
}
