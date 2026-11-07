package game

import "fmt"

// EventStage is where an online event stands. Until 2026-11-07 the booleans Begun / Racing / Over and the GoAt / GridAt / ClearAt sentinels stood
// in for it and the stage was derived in five places. The booleans stay as mirrors, written only by World.advanceEvent.
type EventStage uint8

const (
	StageLoading  EventStage = iota // the event exists: cars load, car select (today's !Begun)
	StageGrid                       // the starting grid went out (Begun && !Racing && !Over)
	StageRacing                     // GO (Racing)
	StageResults                    // the race ended; the lobby flags are still set (Over, ClearAt set)
	StageClearing                   // the lobby flags were cleared; waiting for everyone to be back in free roam (Over, ClearAt nil)
)

// String gives the words the log always used for the stage (results and clearing read the same there).
func (s EventStage) String() string {
	switch s {
	case StageLoading:
		return "loading"
	case StageGrid:
		return "grid"
	case StageRacing:
		return "racing"
	case StageResults, StageClearing:
		return "results"
	}
	return fmt.Sprintf("stage %d", uint8(s))
}

// stageMoves is the transition table: an event only moves forward, one step at a time; EndEvent (w.Event = nil) ends it from any stage.
var stageMoves = map[EventStage]EventStage{StageLoading: StageGrid, StageGrid: StageRacing, StageRacing: StageResults, StageResults: StageClearing}

// advanceEvent moves the event to the next stage and writes the mirror booleans the way the old code did. A move the table does not allow is
// said with one WARN line and still applied (the detector of a programming error; nothing in the traces trips it).
func (w *World) advanceEvent(ev *Event, to EventStage, trigger string) {
	from := ev.Stage
	if from == to {
		return
	}
	if stageMoves[from] != to {
		w.Warnf("event stage %s -> %s is not a forward step (%s): applied anyway; %d participant(s) %s", from, to, trigger, len(ev.People), owners(ev.People))
	}
	ev.Stage = to
	switch to {
	case StageGrid:
		ev.Begun = true
	case StageRacing:
		ev.Begun, ev.Racing = true, true
	case StageResults:
		ev.Begun, ev.Racing, ev.Over = true, false, true
	case StageClearing:
		ev.Over = true
	}
	w.Debugf("state", "event %s -> %s (%s; %d participant(s) %s)", from, to, trigger, len(ev.People), owners(ev.People))
}

func (e *Event) stage() string { return e.Stage.String() }

// membersOf is the set behind People, so membership tests in the relay path do not scan the list per receiver.
func membersOf(people []*Peer) map[*Peer]struct{} {
	m := make(map[*Peer]struct{}, len(people))
	for _, p := range people {
		m[p] = struct{}{}
	}
	return m
}

// has says whether p takes part in the event (an Event made without Members, as the tests do, falls back to the list).
func (e *Event) has(p *Peer) bool {
	if e.Members == nil {
		return inList(e.People, p)
	}
	_, ok := e.Members[p]
	return ok
}

// remove takes a participant out of the event (People keeps the grid order; Members the set).
func (e *Event) remove(p *Peer) {
	e.People = removeFrom(e.People, p)
	if e.Members != nil {
		delete(e.Members, p)
	}
}

// inEvent says whether p is a participant of the running event.
func (w *World) inEvent(p *Peer) bool { return w.Event != nil && w.Event.has(p) }

// noneConnected says whether every listed peer is gone.
func noneConnected(people []*Peer) bool {
	for _, q := range people {
		if q.Connected {
			return false
		}
	}
	return true
}
