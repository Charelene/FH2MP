package game

import (
	"reflect"
	"strings"
	"testing"

	"fh2mp/internal/bits"
	"fh2mp/internal/gameopts"
	"fh2mp/internal/netv2"
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

// The join wait as the game shows it (user design 2026-11-20): the leader stays on "Waiting for Players" until the event starts. Its game leaves
// that screen on the first remote Console.Ready, so an invitee's Ready reaches every game but the leader's during the wait, and the leader's at
// the start; the launch countdown the request started is ended by the start.
func TestEventFlowJoinWaitKeepsTheInviteesReadyFromTheLeader(t *testing.T) {
	r := flowRig(t)
	bystander := NewPeer(Addr{"127.0.0.1", 40009}, 1000)
	bystander.Owner, bystander.Xuid, bystander.Connected, bystander.ConnectedAt = 9, 109, true, 900
	bystander.State, bystander.OpenedAt = PeerConnected, 900
	r.w.Peers.Set(bystander.Addr, bystander)
	r.peers = append(r.peers, bystander)
	r.w.Event = nil
	leader, invitee := r.peers[0], r.peers[1]
	for i, p := range r.peers {
		oid := i + 1
		p.ConsoleOid, p.Entered = &oid, true
	}
	r.w.RaceRequestedAt = r.clk.t
	r.w.RaceHardDeadline = r.clk.t + RaceJoinGraceSeconds
	r.w.PendingRace = &PendingRace{r.clk.t + EventJoinSeconds, leader}
	r.w.SetLaunchCountdown(r.w.NowMs()+int64(EventJoinSeconds*1000), "test request")
	leader.ConsoleVars[4] = true
	if !r.logHas("CountdownType = 1 (launch)") {
		t.Fatalf("the request started no launch countdown: %q", r.log)
	}

	leader.Outbox, bystander.Outbox = nil, nil
	r.w.SetConsoleBool(invitee, 4, 1, "ready request accepted")
	invitee.ConsoleVars[4] = true
	if len(leader.Outbox) != 1 || len(bystander.Outbox) != 1 {
		t.Fatalf("the invitee's Console update was not relayed once to each: leader %d, bystander %d", len(leader.Outbox), len(bystander.Outbox))
	}
	if sameBaked(leader.Outbox[0], bystander.Outbox[0]) {
		t.Fatal("the leader got the invitee's Ready = 1 during the join wait (its game would leave the wait screen at once)")
	}
	if !invitee.ReadyHeld {
		t.Fatal("the held Ready is not remembered for the start")
	}

	leader.Outbox = nil
	r.at(EventJoinSeconds + 0.1)
	if r.w.Event == nil || !r.w.Event.has(invitee) {
		t.Fatalf("the event did not start with the invitee: %q", r.log)
	}
	if !r.logHas("join wait: the Ready of owner [2] reached the leader's game (owner 1) only now, at the start") {
		t.Fatalf("the held Ready was not sent to the leader at the start: %q", r.log)
	}
	if !r.logHas("SessionHost CountdownType = 0, RaceStartTime = 0") {
		t.Fatalf("the start did not end the launch countdown: %q", r.log)
	}
	if invitee.ReadyHeld {
		t.Fatal("the held flag outlived the start")
	}
	// after the start nothing is held any more
	leader.Outbox, bystander.Outbox = nil, nil
	r.w.SetConsoleBool(invitee, 4, 1, "again")
	if len(leader.Outbox) != 1 || len(bystander.Outbox) != 1 || !sameBaked(leader.Outbox[0], bystander.Outbox[0]) {
		t.Fatal("a Ready after the start is still kept from the leader")
	}
}

// consoleReadyCleared turns only Ready = 1 into Ready = 0 and leaves the other variables of the update as they are.
func TestConsoleReadyCleared(t *testing.T) {
	pair := func(vid, val uint64) (uint64, int) { return vid | val<<4, 5 }
	build := func(ps ...[2]uint64) (bits.Int, int) {
		v, size := uint64(0), 0
		for _, p := range ps {
			x, n := pair(p[0], p[1])
			v |= x << uint(size)
			size += n
		}
		return bits.FromUint64(v), size
	}
	in, size := build([2]uint64{2, 1}, [2]uint64{4, 1}, [2]uint64{0, 0})
	want, _ := build([2]uint64{2, 1}, [2]uint64{4, 0}, [2]uint64{0, 0})
	out, ok := consoleReadyCleared(in, size)
	if !ok || !reflect.DeepEqual(out, want) {
		t.Fatalf("ParticipatingInRace=1 Ready=1 RenderReady=0 -> %x ok=%v, want %x", out.Uint64(), ok, want.Uint64())
	}
	in, size = build([2]uint64{4, 0}, [2]uint64{6, 1})
	if _, ok := consoleReadyCleared(in, size); ok {
		t.Fatal("an update without Ready = 1 was changed")
	}
}

func sameBaked(a, b netv2.Baked) bool { return a.N == b.N && reflect.DeepEqual(a.V, b.V) }
