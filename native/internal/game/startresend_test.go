package game

import (
	"reflect"
	"testing"

	"fh2mp/internal/netv2"
)

// startRig is a 3-participant race that has just begun (GoRace at t=0), with resends on.
func startRig(t *testing.T, on bool) (*loadRig, uint64) {
	r := newLoadRig(t, 3)
	prev := StartResend
	StartResend = on
	t.Cleanup(func() { StartResend = prev })
	r.at(0)
	r.w.advanceEvent(r.w.Event, StageGrid, "test")
	r.w.GoRace("test")
	ev := r.w.Event
	if !ev.Racing || ev.GoMs == 0 || ev.RaceHeard == nil {
		t.Fatalf("race not begun: racing=%v goMs=%d; log %q", ev.Racing, ev.GoMs, r.log)
	}
	return r, ev.GoMs
}

// resendRound runs StartResendTick at sec seconds and returns what each peer got queued: nil, or the one message it got.
func resendRound(r *loadRig, sec float64) [][]netv2.Baked {
	r.clk.t = 1000 + sec
	before := make([]int, len(r.peers))
	for i, p := range r.peers {
		before[i] = len(p.Outbox)
	}
	r.w.StartResendTick(r.w.Mono())
	out := make([][]netv2.Baked, len(r.peers))
	for i, p := range r.peers {
		out[i] = p.Outbox[before[i]:]
	}
	return out
}

// prod 2026-11-11 lobby 7741dc1d: the start went out once; the leader's copy was lost and it stayed on the grid while seven others raced.
// A participant with no score message 10 s and 17 s after the start gets the same start again; one that sent scores never gets a second copy.
func TestStartResentToSilentRacers(t *testing.T) {
	r, goMs := startRig(t, true)
	start := netv2.Bake(netv2.StartRace(goMs))
	r.at(3)
	r.w.NoteRaceHeard(r.peers[0])
	r.at(9.5)
	if r.count("race start resent") != 0 {
		t.Fatalf("resent before the first round: %q", r.log)
	}
	got := resendRound(r, 10.5)
	if len(got[0]) != 0 || len(got[1]) != 1 || len(got[2]) != 1 || !reflect.DeepEqual(got[1][0], start) || !reflect.DeepEqual(got[2][0], start) {
		t.Fatalf("round 1 queued %v, want the same start for owners 2 and 3 only", got)
	}
	if r.count("race start resent (round 1") != 1 || r.count("to owner(s) [2, 3]") != 1 {
		t.Fatalf("round 1 log: %q", r.log)
	}
	r.w.NoteRaceHeard(r.peers[1])
	got = resendRound(r, 17.5)
	if len(got[0]) != 0 || len(got[1]) != 0 || len(got[2]) != 1 || !reflect.DeepEqual(got[2][0], start) {
		t.Fatalf("round 2 queued %v, want the start for owner 3 only", got)
	}
	r.at(40)
	if r.count("race start resent") != 2 || r.w.Event.StartResends != 2 {
		t.Fatalf("more than two rounds: %d; %q", r.w.Event.StartResends, r.log)
	}
}

// The resend runs from the world tick.
func TestStartResendRunsInTheTick(t *testing.T) {
	r, _ := startRig(t, true)
	r.at(10.5)
	if r.count("race start resent (round 1") != 1 {
		t.Fatalf("no resend from the tick: %q", r.log)
	}
}

// FH2MP_START_RESEND=0 keeps the single start (the frozen replay traces are recorded that way).
func TestStartResendOff(t *testing.T) {
	r, _ := startRig(t, false)
	r.at(30)
	if r.count("race start resent") != 0 {
		t.Fatalf("resent with the switch off: %q", r.log)
	}
}
