package game

import (
	"reflect"
	"testing"

	"fh2mp/internal/netv2"
	"fh2mp/internal/omap"
	"github.com/Charelene/encore-paddock/bits"
)

// A client that keeps hitting in a King or Infection the server has ended did not get the race-over flag (it goes out once, unreliably): its hits make
// the server send it that flag again, at most every raceOverResendS, and to it only. World 14062880 on prod (2026-11-17): one client hit for 2 minutes
// after a 240 s King had ended.
func TestTagHitAfterTheEndSendsTheRaceOverFlagAgain(t *testing.T) {
	r := newModsRig(t, 2, false)
	w, late, other := r.w, r.peers[1], r.peers[0]
	w.Event = &Event{Over: true, Tag: &Tag{Assigned: true, Seconds: 240}, Finished: omap.New[*Peer, float64](), Quit: map[*Peer]bool{}}
	flag := func(seq int) netv2.Baked {
		return netv2.Bake(netv2.SessionHostUpdate(uint64(seq), func(b *bits.Writer) {
			b.Bits(uint64(SHInRace), netv2.SessionHostVarBits)
			b.Bits(0, 1)
		}))
	}
	hit := func() { w.TagCollision(late, carKey{kind: "own"}, carKey{kind: "own"}, 0, 1, 1, 0) }
	before, seq, otherBefore := len(late.Outbox), late.HostSeq, len(other.Outbox)

	hit()
	if len(late.Outbox) != before+1 || !reflect.DeepEqual(late.Outbox[len(late.Outbox)-1], flag(seq)) || late.HostSeq != seq+1 {
		t.Fatalf("the first late hit did not send InRace = 0 with seq %d: outbox %d -> %d", seq, before, len(late.Outbox))
	}
	hit()
	r.clk.t += raceOverResendS - 0.5
	hit()
	if len(late.Outbox) != before+1 {
		t.Fatalf("the flag went out again within %g s: outbox %d", raceOverResendS, len(late.Outbox))
	}
	r.clk.t += 0.5
	hit()
	if len(late.Outbox) != before+2 || !reflect.DeepEqual(late.Outbox[len(late.Outbox)-1], flag(seq+1)) {
		t.Fatalf("the flag was not sent again after %g s: outbox %d", raceOverResendS, len(late.Outbox))
	}
	if len(other.Outbox) != otherBefore {
		t.Fatal("the other player was sent the flag too")
	}
	// an event still running: hits are the game's, nothing is sent again
	w.Event.Over = false
	n := len(late.Outbox)
	r.clk.t += 10
	w.TagCollision(late, carKey{kind: "own"}, carKey{kind: "own"}, 0, 1, 1, 0)
	if len(late.Outbox) > n && reflect.DeepEqual(late.Outbox[len(late.Outbox)-1], flag(late.HostSeq-1)) {
		t.Fatal("a hit in a running event sent the race-over flag")
	}
}
