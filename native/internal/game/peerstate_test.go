package game

import (
	"strings"
	"testing"

	"fh2mp/internal/catalog"
	"fh2mp/internal/netv2"
	"fh2mp/internal/pyrand"
)

// stateRig is a world with a fake clock and a captured log, no network.
type stateRig struct {
	w   *World
	clk *fakeClock
	log []string
}

func newStateRig(t *testing.T) *stateRig {
	t.Helper()
	t.Setenv("FH2MP_UDP_AUTH", "off")
	r := &stateRig{clk: &fakeClock{t: 1000}}
	cat, err := catalog.Load()
	if err != nil {
		t.Skip("catalog not available:", err)
	}
	out := NewLogOut(func(chunk string) error {
		r.log = append(r.log, strings.Split(strings.TrimRight(chunk, "\n"), "\n")...)
		return nil
	})
	w, err := NewWorld("[test]", out, r.clk, pyrand.New(1), &fakeSock{clk: r.clk}, nil, &Sinks{}, func() *catalog.Catalog { return cat }, false, false, 0)
	if err != nil {
		t.Fatal(err)
	}
	r.w = w
	return r
}

func (r *stateRig) halfOpen(port int) *Peer {
	p := NewPeer(Addr{"127.0.0.1", port}, r.clk.t)
	p.Xuid, p.OpenedAt = 0x9000000000000000+uint64(port), r.clk.t
	res := ResultOK
	p.OpenResult = &res
	r.w.Peers.Set(p.Addr, p)
	return p
}

func (r *stateRig) logHas(s string) bool {
	for _, l := range r.log {
		if strings.Contains(l, s) {
			return true
		}
	}
	return false
}

// A half-open connection may only finish the handshake, keep time, ping, close and send the objects that are held for it; everything else
// needs the ack. A connected or entered peer is admitted everywhere; a removed one nowhere.
func TestPeerStateAdmission(t *testing.T) {
	halfOpen := map[int]bool{netv2.MsgOpenConnection: true, netv2.MsgOpenConnectionResponse: true, netv2.MsgCloseConnection: true, netv2.MsgPing: true,
		netv2.MsgTimeSync: true, netv2.MsgConnectionSyncDone: true, netv2.MsgObjectCreate: true, netv2.MsgObjectUpdate: true}
	for mtype := 0; mtype < 128; mtype++ {
		if got := PeerHalfOpen.Admits(mtype); got != halfOpen[mtype] {
			t.Errorf("half-open admits type %d: %v, want %v", mtype, got, halfOpen[mtype])
		}
		for _, s := range []PeerState{PeerConnected, PeerEntered, PeerLeaving} {
			if !s.Admits(mtype) {
				t.Errorf("%s does not admit type %d", s, mtype)
			}
		}
		if PeerRemoved.Admits(mtype) {
			t.Errorf("removed admits type %d", mtype)
		}
	}
}

// The transition table: forward moves go through and keep the mirror booleans the old code wrote; a move the table forbids is refused and said.
func TestPeerStateMoves(t *testing.T) {
	r := newStateRig(t)
	p := r.halfOpen(40001)
	if p.State != PeerHalfOpen || p.Connected || p.Entered {
		t.Fatalf("a new peer is %s connected=%v entered=%v", p.State, p.Connected, p.Entered)
	}
	if !r.w.admit(p, netv2.MsgObjectCreate) || r.w.admit(p, netv2.MsgRequestRace) || r.w.admit(p, netv2.MsgFinaliseSessionEntry) {
		t.Fatal("half-open admission wrong")
	}
	if p.Refused[netv2.MsgRequestRace] != 1 || p.Refused[netv2.MsgFinaliseSessionEntry] != 1 || refusedTotal(p) != 2 {
		t.Fatalf("refusals not counted: %v", p.Refused)
	}
	if !r.logHas("message type 32 not handled") && !r.logHas("not handled: the connection is half-open") {
		t.Fatalf("no refusal line: %q", r.log)
	}
	if !r.w.setPeerState(p, PeerConnected, "test") || p.State != PeerConnected || !p.Connected || p.Entered {
		t.Fatalf("ack move: %s connected=%v entered=%v", p.State, p.Connected, p.Entered)
	}
	if r.w.setPeerState(p, PeerHalfOpen, "test") || p.State != PeerConnected {
		t.Fatal("a move back to half-open went through")
	}
	if !r.logHas("peer state connected -> half-open refused") {
		t.Fatalf("no refusal line for the backward move: %q", r.log)
	}
	if !r.w.setPeerState(p, PeerEntered, "test") || !p.Connected || !p.Entered {
		t.Fatalf("finalise move: %s connected=%v entered=%v", p.State, p.Connected, p.Entered)
	}
	if !r.w.admit(p, netv2.MsgRequestRace) {
		t.Fatal("an entered peer is refused")
	}
	reason := "test kick"
	p.leaving(&reason)
	if p.State != PeerLeaving || !p.Connected || !p.Entered || p.KickReason == nil {
		t.Fatalf("pending kick: %s connected=%v entered=%v", p.State, p.Connected, p.Entered)
	}
	n := len(r.log)
	r.w.DropPeer(p, "test")
	if p.State != PeerRemoved || p.Connected || !p.Entered || r.w.Peers.Has(p.Addr) {
		t.Fatalf("removal: %s connected=%v entered=%v in peers=%v", p.State, p.Connected, p.Entered, r.w.Peers.Has(p.Addr))
	}
	if !strings.Contains(strings.Join(r.log[n:], "\n"), "removed (test)") || !strings.Contains(strings.Join(r.log[n:], "\n"), "2 message(s) not handled before the handshake ended") {
		t.Fatalf("removal line: %q", r.log[n:])
	}
	if r.w.admit(p, netv2.MsgPing) {
		t.Fatal("a removed peer admits a message")
	}
}

// An abandoned handshake goes through the same removal as every other, so the pending race it led ends and nothing of it stays; it gets the
// abandonment line, not a removal line. A half-open connection that keeps sending without the ack is dropped at the deadline.
func TestAbandonedHandshakeIsARemoval(t *testing.T) {
	r := newStateRig(t)
	p := r.halfOpen(40002)
	r.w.PendingRace = &PendingRace{Deadline: r.clk.t + RaceInviteSeconds, Leader: p}
	r.w.RaceRequestedAt = r.clk.t
	r.clk.t += PendingSeconds + 1
	r.w.Tick()
	if r.w.Peers.Has(p.Addr) || p.State != PeerRemoved {
		t.Fatalf("abandoned peer still here: %s", p.State)
	}
	if r.w.PendingRace != nil {
		t.Fatal("the pending race of the abandoned leader was not cleared")
	}
	joined := strings.Join(r.log, "\n")
	if !strings.Contains(joined, "handshake abandoned") || !strings.Contains(joined, "pending race request of owner 0 cleared") {
		t.Fatalf("log: %q", r.log)
	}
	if strings.Contains(joined, "removed (") || strings.Contains(joined, "session summary") {
		t.Fatalf("an abandoned handshake got a removal line: %q", r.log)
	}

	q := r.halfOpen(40003)
	q.OpenedAt = r.clk.t - HalfOpenMaxSeconds - 1
	q.LastRx = r.clk.t // still sending, never the ack
	r.w.Tick()
	if r.w.Peers.Has(q.Addr) || q.State != PeerRemoved {
		t.Fatalf("half-open peer past the deadline still here: %s", q.State)
	}
	if !strings.Contains(strings.Join(r.log, "\n"), "handshake not finished within") {
		t.Fatalf("no deadline line: %q", r.log)
	}
}
