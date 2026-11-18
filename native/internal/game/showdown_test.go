package game

import (
	"fmt"
	"math"
	"testing"

	"fh2mp/internal/bits"
	"fh2mp/internal/gameopts"
	"fh2mp/internal/netv2"
)

// The Car Meet Showdown (the game's wager race), as the game binary 1.0.0.64 runs it:
//   - "Start Showdown Race" sends message 26 InviteRequest with type 0 (game+0x7d8ca0; Free Roam is 1, Road Trip 2); "Join" / "Leave" send
//     message 27 with the first bit 0 / 1 and join type 1 (game+0x7d8d40 / +0x7d8b60).
//   - The meet menu reads CarMeetSynced.JoinEndTimeWagerRace (var 2, network clock), InviteCountDownTime (var 1) and JoinTypes (vars 4..15,
//     1 = in the Showdown) (view model game+0x830ad0).
//   - Message 30 ConfigureGameOptionsForWagerRace to the starter makes its game take the GameOptions, fill them from its own CarMeetEvents
//     table and answer message 31 (game+0x7d4790); message 28 LaunchWagerRace sends each racer's game to the start (car_meet.xml
//     launch_wager_race), where the race goes on like a Free Roam event's (participants, cars loaded, grid, start).

func cmMsg(write func(w *bits.Writer)) []byte {
	return netv2.PackMessages([]netv2.Baked{netv2.Bake(write)}, netv2.UnreliableID, 0, 0)
}

func cmInvite(kind uint64) []byte {
	return cmMsg(func(w *bits.Writer) { w.Bits(26, 7); w.Bits(kind, 2); w.Bits(0x1234, 32) })
}

// cmJoinMsg is message 27: cancel 0 = join, 1 = leave; joinType 1 = Showdown, 2 = the Free Roam / Road Trip invite.
func cmJoinMsg(cancel, joinType uint64) []byte {
	return cmMsg(func(w *bits.Writer) { w.Bits(27, 7); w.Bits(cancel, 1); w.Bits(joinType, 2); w.Bits(0x1234, 32) })
}

func cmConfigured(v uint64) []byte {
	return cmMsg(func(w *bits.Writer) { w.Bits(31, 7); w.Bits(v, 32) })
}

// gameOptionsOwnershipRequest is message 10 for the GameOptions object: router 6, type 1 (2 bits), key 0 0, id 0 (2 bits).
func gameOptionsOwnershipRequest() []byte {
	return cmMsg(func(w *bits.Writer) {
		w.Bits(10, 7)
		w.Bits(6, 4)
		w.Bits(1, 2)
		w.Bits(0, 1)
		w.Bits(0, 1)
		w.Bits(0, 2)
	})
}

// carMeetVarsQueued folds the CarMeetSynced messages queued for p into the last value of each var: router 11, key 0 0, id 1 bit, seq 8,
// size 14, then var id (5 bits) and value: var 0 two bits, vars 1-3 32 bits, vars 4-15 (the join types) two bits; strings end the read.
func carMeetVarsQueued(p *Peer) map[int]uint64 {
	out := map[int]uint64{}
	for _, m := range p.Outbox {
		if (m.Type() != netv2.MsgObjectUpdate && m.Type() != netv2.MsgObjectCreate) || m.V.Field(7, 4) != netv2.RouterCarMeet {
			continue
		}
		end := 36 + int(m.V.Field(22, 14))
		for o := 36; o+5 <= end; {
			id := int(m.V.Field(o, 5))
			o += 5
			if id >= 16 {
				break
			}
			width := 2
			if id >= 1 && id <= 3 {
				width = 32
			}
			out[id] = m.V.Field(o, width)
			o += width
		}
	}
	return out
}

func typesQueued(p *Peer, mtype int) int {
	n := 0
	for _, m := range p.Outbox {
		if m.Type() == mtype {
			n++
		}
	}
	return n
}

// sessionHostBoolsQueued lists (var, value) of the one-bool SessionHost updates queued for p.
func sessionHostBoolsQueued(p *Peer) [][2]uint64 {
	var out [][2]uint64
	for _, m := range p.Outbox {
		if m.Type() != netv2.MsgObjectUpdate || m.V.Field(7, 4) != netv2.RouterLobby || m.V.Field(11, 2) != 0 || m.V.Field(13, 2) != 0 || m.V.Field(23, 14) != 5 {
			continue
		}
		out = append(out, [2]uint64{m.V.Field(37, 4), m.V.Field(41, 1)})
	}
	return out
}

func carMeetRig(t *testing.T, n int) (*stateRig, []*Peer) {
	r := newStateRig(t)
	r.w.Carmeet = true
	key := &WorldKey{Type: LobbyTypeCarMeet, Name: "meet0001"}
	var peers []*Peer
	for i := 0; i < n; i++ {
		p := r.halfOpen(40001 + i)
		oid := 1
		p.Owner, p.LobbyType, p.World, p.ConsoleOid = i+1, LobbyTypeCarMeet, key, &oid
		p.Connected, p.Entered, p.State = true, true, PeerEntered
		peers = append(peers, p)
	}
	return r, peers
}

func clearQueues(peers ...*Peer) {
	for _, p := range peers {
		p.Outbox = nil
	}
}

func (r *stateRig) advance(seconds float64, peers ...*Peer) {
	r.clk.t += seconds
	for _, p := range peers {
		p.LastRx = r.clk.t
	}
	r.w.CmTick(r.w.NowMs())
}

// "Start Showdown Race" opens the Showdown: every player at the meet gets the join deadline 20 s ahead and the starter marked in it.
func TestShowdownInviteOpensTheJoinWindow(t *testing.T) {
	r, p := carMeetRig(t, 2)
	r.w.Handle(cmInvite(0), p[0].Addr)
	want := uint64(r.w.NowMs()+20000) & 0xFFFFFFFF
	for _, q := range p {
		v := carMeetVarsQueued(q)
		if v[2] != want || v[1] != 20000 || v[4] != 1 || v[5] != 0 {
			t.Fatalf("owner %d's CarMeetSynced: %v, want JoinEndTimeWagerRace %d, InviteCountDownTime 20000, slot 0 in the Showdown", q.Owner, v, want)
		}
	}
	if r.logHas("not served yet") || !r.logHas("Showdown started by owner 1") {
		t.Errorf("log: %q", r.log)
	}
}

// Join and Leave move a player in and out of the Showdown.
func TestShowdownJoinAndLeave(t *testing.T) {
	r, p := carMeetRig(t, 2)
	r.w.Handle(cmInvite(0), p[0].Addr)
	r.w.Handle(cmJoinMsg(0, 1), p[1].Addr)
	if v := carMeetVarsQueued(p[0]); v[4] != 1 || v[5] != 1 {
		t.Fatalf("after the join: %v, want slots 0 and 1 in the Showdown", v)
	}
	r.w.Handle(cmJoinMsg(1, 1), p[1].Addr)
	if v := carMeetVarsQueued(p[0]); v[4] != 1 || v[5] != 0 {
		t.Fatalf("after the leave: %v, want slot 0 only", v)
	}
}

// Nobody joined when the window closes: the Showdown is called off, the menus are told, nobody is asked to set up a race.
func TestShowdownAloneIsCalledOff(t *testing.T) {
	r, p := carMeetRig(t, 2)
	r.w.Handle(cmInvite(0), p[0].Addr)
	clearQueues(p...)
	r.advance(21, p...)
	if v := carMeetVarsQueued(p[1]); v[2] != 0 || v[4] != 0 {
		t.Fatalf("after the window: %v, want JoinEndTimeWagerRace 0 and slot 0 out", v)
	}
	if typesQueued(p[0], netv2.MsgCarMeetConfigureWager) != 0 {
		t.Error("the starter was asked to set up a race nobody joined")
	}
	if !r.logHas("Showdown called off") {
		t.Errorf("no line for the called-off Showdown in %q", r.log)
	}
}

// Two players: the starter alone is asked to set the race up; its GameOptions and message 31 launch the race for both, as an event of the
// two of them, and the meet's menus close the Showdown.
func TestShowdownLaunch(t *testing.T) {
	r, p := carMeetRig(t, 3)
	starter, guest, bystander := p[0], p[1], p[2]
	r.w.Handle(cmInvite(0), starter.Addr)
	r.w.Handle(cmJoinMsg(0, 1), guest.Addr)
	clearQueues(p...)
	r.advance(21, p...)
	if typesQueued(starter, netv2.MsgCarMeetConfigureWager) != 1 || typesQueued(guest, netv2.MsgCarMeetConfigureWager) != 0 {
		t.Fatalf("message 30: starter %d, guest %d, want 1 and 0", typesQueued(starter, netv2.MsgCarMeetConfigureWager), typesQueued(guest, netv2.MsgCarMeetConfigureWager))
	}
	r.w.Handle(gameOptionsOwnershipRequest(), starter.Addr)
	if r.w.GameOptionsOwner != starter {
		t.Fatalf("the GameOptions owner is %v, want the starter", r.w.GameOptionsOwner)
	}
	size, payload, err := gameopts.EncodeFields(map[int]gameopts.Field{30: {Width: 8, Value: bits.FromUint64(255)}})
	if err != nil {
		t.Fatal(err)
	}
	r.w.GameOptions = &gameopts.Options{Size: size, Payload: payload}
	clearQueues(p...)
	r.w.Handle(cmConfigured(1), starter.Addr)
	for _, q := range []*Peer{starter, guest} {
		if typesQueued(q, netv2.MsgCarMeetLaunchWagerRace) != 1 {
			t.Fatalf("owner %d got %d LaunchWagerRace, want 1", q.Owner, typesQueued(q, netv2.MsgCarMeetLaunchWagerRace))
		}
		if !r.logHas(fmt.Sprintf("%d Console.ParticipatingInRace = 1", q.Addr.Port)) {
			t.Errorf("owner %d was not made a participant: %q", q.Owner, r.log)
		}
		got := sessionHostBoolsQueued(q)
		if !containsPair(got, SHRaceParticipantsSelected, 1) {
			t.Errorf("owner %d's SessionHost bools %v lack RaceParticipantsSelected = 1", q.Owner, got)
		}
	}
	if typesQueued(bystander, netv2.MsgCarMeetLaunchWagerRace) != 0 || len(sessionHostBoolsQueued(bystander)) != 0 {
		t.Error("the bystander got the race")
	}
	if ev := r.w.Event; ev == nil || len(ev.People) != 2 || ev.People[0] != starter || ev.People[1] != guest {
		t.Fatalf("event: %+v, want the starter and the guest", r.w.Event)
	}
	if v := carMeetVarsQueued(bystander); v[2] != 0 || v[4] != 0 || v[5] != 0 {
		t.Errorf("the meet's menus still show the Showdown: %v", v)
	}
	// while it runs, another Showdown cannot start in this meet
	r.w.Handle(cmInvite(0), bystander.Addr)
	if !r.logHas("Showdown from owner 3 refused") {
		t.Errorf("no refusal line for a second Showdown in %q", r.log)
	}
	// the race over, the meet is free again
	r.w.EndEvent("test", false)
	r.w.Handle(cmInvite(0), bystander.Addr)
	if !r.logHas("Showdown started by owner 3") {
		t.Errorf("a new Showdown did not start after the race: %q", r.log)
	}
}

// The starter's game never answers message 31: after 15 s the Showdown is called off and the GameOptions taken back.
func TestShowdownStarterNeverSetsUp(t *testing.T) {
	r, p := carMeetRig(t, 2)
	r.w.Handle(cmInvite(0), p[0].Addr)
	r.w.Handle(cmJoinMsg(0, 1), p[1].Addr)
	r.advance(21, p...)
	r.w.Handle(gameOptionsOwnershipRequest(), p[0].Addr)
	clearQueues(p...)
	r.advance(16, p...)
	if r.w.Event != nil || typesQueued(p[1], netv2.MsgCarMeetLaunchWagerRace) != 0 {
		t.Fatal("a race started without the starter's set-up")
	}
	if typesQueued(p[0], netv2.MsgObjectTransferOwnership) != 1 || r.w.GameOptionsOwner == p[0] {
		t.Error("the GameOptions were not taken back from the starter")
	}
	if !r.logHas("Showdown called off") {
		t.Errorf("no line in %q", r.log)
	}
}

// The starter leaves while its game sets the race up: called off, the other player's menu is told.
func TestShowdownStarterLeaves(t *testing.T) {
	r, p := carMeetRig(t, 2)
	r.w.Handle(cmInvite(0), p[0].Addr)
	r.w.Handle(cmJoinMsg(0, 1), p[1].Addr)
	r.advance(21, p...)
	r.w.DropPeer(p[0], "test")
	clearQueues(p[1])
	r.advance(1, p[1])
	if v := carMeetVarsQueued(p[1]); v[2] != 0 || v[5] != 0 {
		t.Fatalf("after the starter left: %v", v)
	}
	r.w.Handle(cmConfigured(1), p[1].Addr)
	if r.w.Event != nil {
		t.Fatal("a race started after the starter left")
	}
}

// A Free Roam invite at the same meet works while a Showdown is open; the Showdown keeps its players.
func TestShowdownAlongsideAFreeRoamInvite(t *testing.T) {
	r, p := carMeetRig(t, 3)
	r.w.Handle(cmInvite(0), p[0].Addr)
	r.w.Handle(cmInvite(1), p[2].Addr)
	v := carMeetVarsQueued(p[1])
	if v[0] != 1 || v[4] != 1 || v[6] != 2 {
		t.Fatalf("CarMeetSynced %v: want the Free Roam invite (type 1) with slot 2 in it, and slot 0 in the Showdown", v)
	}
}

// consoleVarsMsg is a client's Console update (router 3, its own object 0): (var id 4 bits, value 1 bit) pairs.
func consoleVarsMsg(seq uint64, pairs ...[2]uint64) []byte {
	return cmMsg(func(w *bits.Writer) {
		var body bits.Writer
		for _, p := range pairs {
			body.Bits(p[0], netv2.ConsoleVarIDBits)
			body.Bits(p[1], 1)
		}
		w.Bits(netv2.MsgObjectUpdate, 7)
		w.Bits(3, 4)
		w.Bits(0, 1)
		w.Bits(0, 1)
		w.Bits(0, netv2.RouterBits[3][0])
		w.Bits(seq, 8)
		w.Bits(uint64(body.Pos), 14)
		w.Raw(body.Value(), body.Pos)
	})
}

// launchedShowdown runs a two-player Showdown up to message 28.
func launchedShowdown(t *testing.T) (*stateRig, []*Peer) {
	r, p := carMeetRig(t, 2)
	r.w.Handle(cmInvite(0), p[0].Addr)
	r.w.Handle(cmJoinMsg(0, 1), p[1].Addr)
	r.advance(21, p...)
	r.w.Handle(gameOptionsOwnershipRequest(), p[0].Addr)
	size, payload, err := gameopts.EncodeFields(map[int]gameopts.Field{30: {Width: 8, Value: bits.FromUint64(255)}})
	if err != nil {
		t.Fatal(err)
	}
	r.w.GameOptions = &gameopts.Options{Size: size, Payload: payload}
	r.w.Handle(cmConfigured(1), p[0].Addr)
	if r.w.Event == nil {
		t.Fatal("the Showdown did not start its event")
	}
	clearQueues(p...)
	return r, p
}

// The Car Meet's race flow has no car select (car_meet.xml launch_wager_race), so the shim's cars-loaded wait is never held for one: on dev
// 2026-11-18 the hold made both racers wait 19 s for each other's car select.
func TestShowdownHasNoCarSelectHold(t *testing.T) {
	r, p := launchedShowdown(t)
	for _, q := range p {
		if hold, _, _ := r.w.CarselAnswer(q); hold {
			t.Fatalf("owner %d's cars-loaded wait is held in a Showdown", q.Owner)
		}
	}
}

// The Car Meet flow never sets HasLoadedCarsForAllParticipants: after the cars-loaded wait it clears HasGrid and waits for the route switch
// (car_meet.xml car_loading_cutscene_for_wager_race; Free Roam sets the flag first, network_freeroam_states.xml:468). So HasGrid=0 after the
// start is a Showdown racer's "loaded", and the grid goes out once both have sent it (dev 2026-11-18: no grid, load timeout after 30 s).
func TestShowdownGridAfterHasGridCleared(t *testing.T) {
	r, p := launchedShowdown(t)
	r.w.Handle(consoleVarsMsg(5, [2]uint64{ConsoleVarHasGrid, 0}), p[0].Addr)
	if typesQueued(p[0], netv2.MsgStartingGrid) != 0 {
		t.Fatal("the grid went out before the second racer loaded")
	}
	r.w.Handle(consoleVarsMsg(5, [2]uint64{ConsoleVarHasGrid, 0}), p[1].Addr)
	for _, q := range p {
		if typesQueued(q, netv2.MsgStartingGrid) != 1 {
			t.Fatalf("owner %d got %d StartingGrid, want 1", q.Owner, typesQueued(q, netv2.MsgStartingGrid))
		}
	}
	if !r.logHas("all 2 participant(s) loaded cars -> starting grid") {
		t.Errorf("no grid line in %q", r.log)
	}
}

// gridQueued decodes the StartingGrid queued for p into relay owners in slot order (p's own entry is local, written as p.Owner): type 7 bits,
// lobby key 4, count 4, then per slot A 1, B 1, car id 7 and, for another player's car (B=1), its owner 8 bits.
func gridQueued(p *Peer) []int {
	for _, m := range p.Outbox {
		if m.Type() != netv2.MsgStartingGrid {
			continue
		}
		n := int(m.V.Field(11, 4))
		var out []int
		for o, i := 15, 0; i < n; i++ {
			b := m.V.Field(o+1, 1)
			o += 2 + netv2.CarKeyIDBits
			if b == 1 {
				out = append(out, int(m.V.Field(o, 8)))
				o += 8
			} else {
				out = append(out, p.Owner)
			}
		}
		return out
	}
	return nil
}

// A Showdown's GameOptions keep GridOrderingType at its default, PI (EGridOrderingType 1, enum registration game+0x112800), and set Ascending
// (var 402 = 1 in the starter's options on dev 2026-11-18): the slowest car takes pole, the game's only evening-out of a Showdown (its
// FirstCarHeadStartMeters and PerCarRollOffDelay stay 0). The server orders the grid it sends that way instead of shuffling it.
func TestShowdownGridByPIAscending(t *testing.T) {
	r, p := launchedShowdown(t)
	size, payload, err := gameopts.EncodeFields(map[int]gameopts.Field{30: {Width: 8, Value: bits.FromUint64(255)},
		gameopts.GridAscendingID: {Width: 1, Value: bits.FromUint64(1)}})
	if err != nil {
		t.Fatal(err)
	}
	r.w.GameOptions = &gameopts.Options{Size: size, Payload: payload}
	p[0].Choices = &Choices{HasClass: true, Class: 3, PI: 0.720} // the starter drives the faster car
	p[1].Choices = &Choices{HasClass: true, Class: 1, PI: 0.526}
	for _, q := range p {
		r.w.Handle(consoleVarsMsg(5, [2]uint64{ConsoleVarHasGrid, 0}), q.Addr)
	}
	for _, q := range p {
		if got := gridQueued(q); len(got) != 2 || got[0] != p[1].Owner || got[1] != p[0].Owner {
			t.Fatalf("owner %d's grid %v, want the slower car (owner %d) on pole", q.Owner, got, p[1].Owner)
		}
	}
	if !r.logHas("Showdown grid by PI, lowest first") {
		t.Errorf("no grid order line in %q", r.log)
	}
}

// A Showdown's options set EndConditionOptions.EndRaceCounter (var 423) to 32 s: each racer's game counts that long after the first finisher
// before it ends its own race. The server's own limit (30 s) ended the race 2 s before the game's countdown ran out (dev 2026-11-18 04:29:40):
// in a Showdown the server waits for the game's counter plus a margin instead.
func TestShowdownWaitsForTheGamesEndRaceCounter(t *testing.T) {
	r, p := launchedShowdown(t)
	size, payload, err := gameopts.EncodeFields(map[int]gameopts.Field{30: {Width: 8, Value: bits.FromUint64(255)},
		gameopts.EndRaceCounterID: {Width: 32, Value: bits.FromUint64(uint64(math.Float32bits(32)))}})
	if err != nil {
		t.Fatal(err)
	}
	r.w.GameOptions = &gameopts.Options{Size: size, Payload: payload}
	r.w.GoRace("test")
	ev := r.w.Event
	first := r.w.Mono()
	ev.Finished.Set(p[0], first)
	r.w.CheckRaceEnd(first + 31)
	if ev.Over {
		t.Fatal("the race ended before the game's own 32 s end-of-race counter")
	}
	r.w.CheckRaceEnd(first + 32 + ShowdownEndCounterMarginSecs + 0.1)
	if !ev.Over {
		t.Fatal("the race did not end after the game's end-of-race counter")
	}
}

// playerVarsQueued lists (var id, value) of the server updates of p's own player object (router 1, key 1 1, id 7 bits, seq 8, size 14, then
// var id 8 bits and the value) whose single var has the given width.
func playerVarsQueued(p *Peer, width int) [][2]uint64 {
	var out [][2]uint64
	for _, m := range p.Outbox {
		if m.Type() != netv2.MsgObjectUpdate || m.V.Field(7, 4) != 1 || m.V.Field(11, 1) != 1 || m.V.Field(12, 1) != 1 {
			continue
		}
		if int(m.V.Field(28, 14)) != 8+width {
			continue
		}
		out = append(out, [2]uint64{m.V.Field(42, 8), m.V.Field(50, width)})
	}
	return out
}

// The Showdown's staggered start, from the game: after the 3-2-1 a wager race (and only a wager race, race_flow.xml wait_for_rolloff_delay)
// holds each car until SessionHost.CountdownEndTime plus the RollOffDelay of the player's bucket and shows the seconds left
// (States::WaitForPlayerBucketRollOffDelay game+0x964dc0). So each racer gets its own bucket in grid order and the slower cars the head start;
// the size of it is ours (FH2MP_SHOWDOWN_SECS_PER_PI, 0.1 s per PI point by default: the original server's numbers are not in the game).
func TestShowdownHeadStartByPI(t *testing.T) {
	r, p := launchedShowdown(t)
	size, payload, err := gameopts.EncodeFields(map[int]gameopts.Field{30: {Width: 8, Value: bits.FromUint64(255)},
		gameopts.GridAscendingID: {Width: 1, Value: bits.FromUint64(1)}})
	if err != nil {
		t.Fatal(err)
	}
	r.w.GameOptions = &gameopts.Options{Size: size, Payload: payload}
	p[0].Choices = &Choices{HasClass: true, Class: 3, PI: 0.720}
	p[1].Choices = &Choices{HasClass: true, Class: 1, PI: 0.526}
	for _, q := range p {
		r.w.Handle(consoleVarsMsg(5, [2]uint64{ConsoleVarHasGrid, 0}), q.Addr)
	}
	// buckets: the slower car (pole) bucket 0, the faster one bucket 1
	for q, want := range map[*Peer]uint64{p[1]: 0, p[0]: 1} {
		if !containsPair(playerVarsQueued(q, 3), PlayerVarBucketID, want) {
			t.Errorf("owner %d: player bucket %v, want %d", q.Owner, playerVarsQueued(q, 3), want)
		}
	}
	fs, err := gameopts.Fields(r.w.GameOptions.Size, r.w.GameOptions.Payload)
	if err != nil {
		t.Fatal(err)
	}
	delay := func(bucket int) float64 {
		f, ok := fs[gameopts.BucketRollOffDelayID(bucket)]
		if !ok {
			return 0
		}
		return float64(math.Float32frombits(uint32(f.Value.Uint64())))
	}
	if d0, d1 := delay(0), delay(1); d0 != 0 || math.Abs(d1-19.4) > 0.01 {
		t.Fatalf("roll-off delays %.2f / %.2f s, want 0 for pole and 19.4 s for the car 194 PI faster", d0, d1)
	}
	// the race start carries CountdownEndTime (var 3), what the delays count from
	clearQueues(p...)
	r.w.GoRace("test")
	found := false
	for _, m := range p[0].Outbox {
		if m.Type() == netv2.MsgObjectUpdate && m.V.Field(7, 4) == netv2.RouterLobby {
			end := 37 + int(m.V.Field(23, 14))
			for o := 37; o+4 <= end; {
				id := int(m.V.Field(o, 4))
				o += 4
				width := map[int]int{SHCountdownType: SHCountdownTypeBits, SHRaceStartTime: 32, SHNumLapsCompleted: 8, SHFurthestProgress: 32, SHCountdownEndTime: 32, SHInRace: 1}[id]
				if width == 0 {
					break
				}
				if id == SHCountdownEndTime && m.V.Field(o, 32) == r.w.Event.GoMs {
					found = true
				}
				o += width
			}
		}
	}
	if !found {
		t.Error("the race start sent no CountdownEndTime equal to the start time")
	}
}

func containsPair(l [][2]uint64, id int, v uint64) bool {
	for _, x := range l {
		if x[0] == uint64(id) && x[1] == v {
			return true
		}
	}
	return false
}
