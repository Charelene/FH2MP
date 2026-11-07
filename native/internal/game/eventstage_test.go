package game

import (
	"strings"
	"testing"

	"fh2mp/internal/omap"
)

// The stage moves forward one step at a time and writes the mirror booleans the way the old code did; the words of the log stay.
func TestEventStageMoves(t *testing.T) {
	r := newStateRig(t)
	ev := &Event{Finished: omap.New[*Peer, float64](), Quit: map[*Peer]bool{}}
	if ev.stage() != "loading" || ev.Begun || ev.Racing || ev.Over {
		t.Fatalf("new event: %s %v %v %v", ev.stage(), ev.Begun, ev.Racing, ev.Over)
	}
	r.w.advanceEvent(ev, StageGrid, "test")
	if ev.stage() != "grid" || !ev.Begun || ev.Racing || ev.Over {
		t.Fatalf("grid: %s %v %v %v", ev.stage(), ev.Begun, ev.Racing, ev.Over)
	}
	r.w.advanceEvent(ev, StageRacing, "test")
	if ev.stage() != "racing" || !ev.Begun || !ev.Racing || ev.Over {
		t.Fatalf("racing: %s %v %v %v", ev.stage(), ev.Begun, ev.Racing, ev.Over)
	}
	r.w.advanceEvent(ev, StageResults, "test")
	if ev.stage() != "results" || !ev.Begun || ev.Racing || !ev.Over {
		t.Fatalf("results: %s %v %v %v", ev.stage(), ev.Begun, ev.Racing, ev.Over)
	}
	r.w.advanceEvent(ev, StageClearing, "test")
	if ev.stage() != "results" || ev.Racing || !ev.Over {
		t.Fatalf("clearing: %s %v %v", ev.stage(), ev.Racing, ev.Over)
	}
	if r.logHas("is not a forward step") {
		t.Fatalf("a forward walk was called out: %q", r.log)
	}
	ev2 := &Event{Finished: omap.New[*Peer, float64](), Quit: map[*Peer]bool{}}
	r.w.advanceEvent(ev2, StageRacing, "test")
	if !r.logHas("event stage loading -> racing is not a forward step") || !ev2.Racing || !ev2.Begun {
		t.Fatalf("a skipped stage was not called out, or not applied: %q", r.log)
	}
}

// Membership through the set matches the list, and removal keeps both in step.
func TestEventMembers(t *testing.T) {
	a, b, c := NewPeer(Addr{"a", 1}, 0), NewPeer(Addr{"b", 2}, 0), NewPeer(Addr{"c", 3}, 0)
	ev := &Event{People: []*Peer{a, b}, Members: membersOf([]*Peer{a, b})}
	if !ev.has(a) || !ev.has(b) || ev.has(c) {
		t.Fatal("membership")
	}
	ev.remove(a)
	if ev.has(a) || len(ev.People) != 1 || ev.People[0] != b {
		t.Fatal("removal")
	}
	legacy := &Event{People: []*Peer{c}} // made without the set, as the older tests do
	if !legacy.has(c) || legacy.has(a) {
		t.Fatal("list fallback")
	}
}

// A race whose every participant left before anyone finished ends instead of staying InRace forever.
func TestRaceEndsWhenEveryoneLeft(t *testing.T) {
	r := newLoadRig(t, 2)
	ev := r.w.Event
	r.w.advanceEvent(ev, StageGrid, "test")
	r.w.advanceEvent(ev, StageRacing, "test")
	ev.RacingSince = r.clk.t
	for _, p := range r.peers {
		r.w.setPeerState(p, PeerRemoved, "gone")
	}
	r.clk.t += 1
	r.w.CheckRaceEnd(r.clk.t)
	if ev.Stage != StageResults || !ev.Over {
		t.Fatalf("race did not end: stage %s", ev.Stage)
	}
	if !strings.Contains(strings.Join(r.log, "\n"), "every participant left during the race") {
		t.Fatalf("log: %q", r.log)
	}
}
