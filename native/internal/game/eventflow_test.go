package game

import (
	"strings"
	"testing"

	"fh2mp/internal/gameopts"
)

// flowRig: an A-class event (rule class 3) with two connected participants, started 0 s ago, the new flow on.
func flowRig(t *testing.T) *loadRig {
	t.Helper()
	prev := EventFlowV2
	EventFlowV2 = true
	t.Cleanup(func() { EventFlowV2 = prev })
	r := newLoadRig(t, 2)
	r.w.Event.CarRule = &gameopts.Rule{Class: 3}
	for _, p := range r.peers {
		p.Choices = &Choices{HasClass: true, Class: 3, PI: 0.745} // both arrive in an A car
	}
	return r
}

func (r *loadRig) logHas(s string) bool {
	return strings.Contains(strings.Join(r.log, "\n"), s)
}

// User report 2026-11-20 (dev): a player whose car did not fit stayed in the event until everybody had loaded and held the others. Now the car a
// participant picked is judged EventPickJudgeSeconds after it left the car select: an S1 car in an A event is out then, its game is told
// (ParticipatingInRace 0), and the other participant stays.
func TestEventFlowPickedCarThatDoesNotFitIsOutAtOnce(t *testing.T) {
	r := flowRig(t)
	wrong, right := r.peers[0], r.peers[1]
	wrong.Choices = &Choices{HasClass: true, Class: 4, PI: 0.807} // S1, as its game reports the picked car

	r.w.notePicked(r.w.Event, wrong)
	r.w.notePicked(r.w.Event, right)
	r.at(EventPickJudgeSeconds - 0.5)
	if !r.w.Event.has(wrong) {
		t.Fatalf("owner %d was taken out before its picked car's numbers could arrive (%s s after the pick)", wrong.Owner, fmtG(EventPickJudgeSeconds-0.5))
	}
	r.at(EventPickJudgeSeconds + 0.1)
	if r.w.Event == nil || r.w.Event.has(wrong) {
		t.Fatalf("owner %d with an S1 car is still in the A event %s s after its pick: %q", wrong.Owner, fmtG(EventPickJudgeSeconds+0.1), r.log)
	}
	if !r.w.Event.has(right) {
		t.Fatalf("owner %d with an A car was taken out too", right.Owner)
	}
	if !r.logHas("out of the event 3 s after leaving the car select") {
		t.Fatalf("no line says when and why it was taken out: %q", r.log)
	}
}

// A participant whose first car was refused and who picks one that fits stays: the judgement uses the numbers of the picked car, not the
// refused one.
func TestEventFlowPickedCarThatFitsStays(t *testing.T) {
	r := flowRig(t)
	p := r.peers[0]
	p.Choices = &Choices{HasClass: true, Class: 4, PI: 0.841} // the refused S1 car it arrived with
	r.w.notePicked(r.w.Event, p)
	p.Choices = &Choices{HasClass: true, Class: 3, PI: 0.748} // the A car it picked, arriving after its status
	r.at(EventPickJudgeSeconds + 0.1)
	if r.w.Event == nil || !r.w.Event.has(p) {
		t.Fatalf("owner %d picked a car that fits and was taken out: %q", p.Owner, r.log)
	}
}

// With the old flow nothing is judged at the pick (FH2MP_EVENT_FLOW=1).
func TestEventFlowOldFlowJudgesOnlyAfterLoading(t *testing.T) {
	r := flowRig(t)
	EventFlowV2 = false
	p := r.peers[0]
	p.Choices = &Choices{HasClass: true, Class: 4, PI: 0.807}
	r.w.notePicked(r.w.Event, p)
	r.at(EventPickJudgeSeconds + 0.1)
	if r.w.Event == nil || !r.w.Event.has(p) {
		t.Fatalf("the old flow took owner %d out at the pick", p.Owner)
	}
}

// The join wait: a race request opens EventJoinSeconds, and at its end the event starts without holding for an invitee whose Ready was refused
// on its car; that invitee is a participant (it gets the event car select), nobody waits RaceJoinGraceSeconds.
func TestEventFlowJoinWaitDoesNotHoldForARefusedInvitee(t *testing.T) {
	r := flowRig(t)
	r.w.Event = nil
	leader, refused := r.peers[0], r.peers[1]
	for _, p := range r.peers {
		p.Entered = true
	}
	r.w.RaceRequestedAt = r.clk.t
	r.w.RaceHardDeadline = r.clk.t + RaceJoinGraceSeconds
	r.w.PendingRace = &PendingRace{r.clk.t + EventJoinSeconds, leader}
	leader.ConsoleVars[4] = true
	refused.ConsoleVars[3] = true // accepted the invite
	at := r.w.RaceRequestedAt
	refused.ReadyRetry = &at // its Ready was refused on its car
	refused.GoptsWrittenAt = at - 10

	r.at(EventJoinSeconds - 1) // the request came at the rig's 0 s
	if r.w.Event != nil {
		t.Fatal("the event started before the join wait ended")
	}
	r.at(EventJoinSeconds + 0.1)
	if r.w.Event == nil {
		t.Fatalf("the event did not start when the %s s join wait ended: %q", fmtG(EventJoinSeconds), r.log)
	}
	if !r.w.Event.has(refused) {
		t.Fatalf("the invitee refused on its car is not a participant (it should get the event car select): %q", r.log)
	}
	if r.logHas("event start held for owner") {
		t.Fatalf("the start was held for the refused invitee: %q", r.log)
	}
}
