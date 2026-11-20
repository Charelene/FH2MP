package game

import (
	"testing"

	"fh2mp/internal/netv2"
)

// raceFlagRig is a Free Roam world of n entered players, each with its own Console object, and no event.
func raceFlagRig(t *testing.T, n int) (*stateRig, []*Peer) {
	t.Helper()
	prev := RaceFlagClear
	RaceFlagClear = true
	t.Cleanup(func() { RaceFlagClear = prev })
	r := newStateRig(t)
	var peers []*Peer
	for i := 0; i < n; i++ {
		p := r.halfOpen(41001 + i)
		oid := 10 + i
		p.Owner, p.ConsoleOid = i+1, &oid
		p.Connected, p.Entered, p.State = true, true, PeerEntered
		peers = append(peers, p)
	}
	return r, peers
}

// rememberedRaceFlag is the last Console.ParticipatingInRace the world keeps for p's console: what the other games were sent and what a player who
// joins later is replayed. -1 = never set.
func rememberedRaceFlag(p *Peer) int {
	hist, _ := p.Objs.Get(ObjKey{3, *p.ConsoleOid})
	v := -1
	for _, e := range hist {
		if e.Type == netv2.MsgObjectUpdate && e.Size == netv2.ConsoleVarIDBits+1 && int(e.Payload.Field(0, netv2.ConsoleVarIDBits)) == ConsoleVarParticipatingInRace {
			v = int(e.Payload.Field(netv2.ConsoleVarIDBits, 1))
		}
	}
	return v
}

// invite marks the players who answered the race invite with Ready (Console var 4), as their games do.
func invite(peers ...*Peer) {
	for _, p := range peers {
		p.ConsoleVars[4] = true
	}
}

// finishAll takes the running event to its results screen with every participant finished.
func finishAll(r *stateRig) {
	ev := r.w.Event
	for _, s := range []EventStage{StageGrid, StageRacing, StageResults} {
		r.w.advanceEvent(ev, s, "test")
	}
	for i, p := range ev.People {
		ev.Finished.Set(p, 100+float64(i))
	}
}

// Prod 2026-11-19 20:30-20:33 (freeroam:0bc34872): four players finished a race and left its results screen one by one, so the event ended
// "every participant left the event" with nobody left in it. Their ParticipatingInRace stayed 1, two of them skipped the next invite, and both
// players of the next event waited for those two as racers ("still loading 2 of 3 racers [4:c 5:c]") until the load timeout ended it.
// A finisher keeps its flag while the results screen is still up (it is part of the result); the end of the event clears it for every finisher.
func TestRaceFlagOfFinishersClearedWhenTheEventEnds(t *testing.T) {
	r, p := raceFlagRig(t, 3)
	invite(p[1], p[2])
	r.w.StartEvent(p[0])
	for _, q := range p {
		if got := rememberedRaceFlag(q); got != 1 {
			t.Fatalf("owner %d at the event start: ParticipatingInRace = %d, want 1 (participant selected)", q.Owner, got)
		}
	}
	finishAll(r)

	r.w.ParticipantLeft(p[0])
	if got := rememberedRaceFlag(p[0]); got != 1 {
		t.Errorf("owner 1 left the results screen while two others are still on it: ParticipatingInRace = %d, want 1 (still part of the result)", got)
	}
	r.w.ParticipantLeft(p[1])
	r.w.ParticipantLeft(p[2])

	if r.w.Event != nil {
		t.Fatalf("the event did not end after its last participant left")
	}
	for _, q := range p {
		if got := rememberedRaceFlag(q); got != 0 {
			t.Errorf("owner %d after the event ended: ParticipatingInRace = %d, want 0", q.Owner, got)
		}
	}
}

// The next event's participants must not see a player outside it as a racer: at the start every connected non-participant whose flag is still 1
// gets 0, whatever left it set (here a finisher of the event before, as above, before the end-of-event clear existed).
func TestRaceFlagOfNonParticipantsClearedAtTheStart(t *testing.T) {
	r, p := raceFlagRig(t, 4)
	r.w.SetConsoleBool(p[2], ConsoleVarParticipatingInRace, 1, "test: left over from an earlier event")
	r.w.SetConsoleBool(p[3], ConsoleVarParticipatingInRace, 1, "test: left over from an earlier event")
	invite(p[1])
	r.w.StartEvent(p[0])

	for _, q := range p[:2] {
		if got := rememberedRaceFlag(q); got != 1 {
			t.Errorf("participant owner %d: ParticipatingInRace = %d, want 1", q.Owner, got)
		}
	}
	for _, q := range p[2:] {
		if got := rememberedRaceFlag(q); got != 0 {
			t.Errorf("owner %d did not answer the invite: ParticipatingInRace = %d, want 0", q.Owner, got)
		}
	}
}
