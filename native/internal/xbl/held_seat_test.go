package xbl

import (
	"fmt"
	"strconv"
	"testing"
)

// Prod 2026-11-06 22:20-22:22 UTC: a player reserved a seat in a friend's lobby, the game left that lobby and searched again, and the left-lobby rule ate
// the held seat, twice (the second time 100 s after the leave): a random lobby. The seat now survives the rule and wins once the leave is old.
// Same cases as HeldSeatAgainstTheLeftLobbyRule in server/tests/test_lobby_packing.py.
type heldSeatRig struct {
	t     *testing.T
	s     *Server
	hub   *fakeHub
	clock float64
}

func newHeldSeatRig(t *testing.T) *heldSeatRig {
	r := &heldSeatRig{t: t, hub: &fakeHub{have: true, pops: map[string][]string{}}, clock: 1_700_000_000.0}
	r.s = New(r.hub)
	r.s.Now = func() float64 { return r.clock }
	r.s.Monotonic = func() float64 { return 1000.0 }
	n := 0
	r.s.NewUUID = func() string { n++; return fmt.Sprintf("00000000-0000-4000-8000-%012x", n) }
	return r
}

func hexOf(x string) string { n, _ := strconv.ParseInt(x, 10, 64); return fmt.Sprintf("%x", n) }

func (r *heldSeatRig) match(xuid string, connect bool) string {
	r.t.Helper()
	name, err := r.s.lobbyFor("s", "FreeRoam_qos", NewDict(), nil, xuid)
	if err != nil {
		r.t.Fatal(err)
	}
	if connect {
		r.disconnect(xuid)
		r.hub.pops[name] = append(r.hub.pops[name], hexOf(xuid))
	}
	return name
}

func (r *heldSeatRig) disconnect(xuid string) {
	for name, xs := range r.hub.pops {
		kept := xs[:0:0]
		for _, x := range xs {
			if x != hexOf(xuid) {
				kept = append(kept, x)
			}
		}
		r.hub.pops[name] = kept
	}
}

func (r *heldSeatRig) path(name string) string {
	return "/serviceconfigs/s/sessionTemplates/" + TargetTemplate + "/sessions/" + name
}

func (r *heldSeatRig) put(name, body, xuid string) {
	r.t.Helper()
	if st, _ := r.s.PutSession(r.path(name), []byte(body), xuid); st != 200 {
		r.t.Fatalf("PUT %s as %s: status %d", body, xuid, st)
	}
}

const (
	joinBody  = `{"members": {"me": {"properties": {"system": {"active": true}}}}}`
	leaveBody = `{"members": {"me": null}}`
)

func (r *heldSeatRig) friendLobbyAndAPlayerWhoLeftIt() string {
	r.t.Helper()
	a := r.match("1001", true) // the friend's lobby
	if got := r.match("1002", true); got != a {
		r.t.Fatalf("the first ticket of the player went to %s, want the friend's lobby %s", got, a)
	}
	r.put(a, joinBody, "1002")
	if _, err := r.s.Reserve("1002", a, ""); err != nil { // "join" in the launcher
		r.t.Fatal(err)
	}
	r.put(a, leaveBody, "1002")
	r.disconnect("1002") // the game left the lobby and searches again
	return a
}

func TestHeldSeatSearchASecondAfterLeavingGetsAnotherLobbyAndKeepsTheSeat(t *testing.T) {
	r := newHeldSeatRig(t)
	a := r.friendLobbyAndAPlayerWhoLeftIt()
	if b := r.match("1002", false); b == a {
		t.Fatalf("matched back into the lobby it just left: %s", b)
	}
	if h := r.s.CurrentHold("1002"); h == nil || h.ID != a {
		t.Fatalf("the held seat is used up: %+v", h)
	}
}

func TestHeldSeatWinsOnceTheLeaveIsOld(t *testing.T) {
	r := newHeldSeatRig(t)
	a := r.friendLobbyAndAPlayerWhoLeftIt()
	if b := r.match("1002", false); b == a {
		t.Fatalf("matched back into the lobby it just left: %s", b)
	}
	r.clock += 100 // 22:22:35 in the log: 100 s after the leave, inside LeftAvoidSeconds
	if got := r.match("1002", false); got != a {
		t.Fatalf("100 s after the leave the held seat should win: got %s, want %s", got, a)
	}
	if h := r.s.CurrentHold("1002"); h != nil {
		t.Fatalf("the seat should be used now: %+v", h)
	}
}

func TestHeldSeatPlayerStillConnectedToTheHeldLobbyIsNotMatchedIntoIt(t *testing.T) {
	r := newHeldSeatRig(t)
	a := r.match("1001", true)
	r.match("1002", false)
	if _, err := r.s.Reserve("1002", a, ""); err != nil {
		t.Fatal(err)
	}
	r.hub.pops[a] = append(r.hub.pops[a], hexOf("1002")) // connected to a and searching from inside it
	if b := r.match("1002", false); b == a {
		t.Fatalf("matched into the lobby it is connected to: %s", b)
	}
	if r.s.CurrentHold("1002") == nil {
		t.Fatal("the held seat is used up")
	}
}

func TestHeldSeatWithoutAReservationTheRuleIsUnchanged(t *testing.T) {
	r := newHeldSeatRig(t)
	a := r.match("1001", true)
	r.match("1002", true)
	r.put(a, joinBody, "1002")
	r.put(a, leaveBody, "1002")
	r.disconnect("1002")
	r.clock += 100
	if b := r.match("1002", false); b == a {
		t.Fatalf("without a reservation a lobby left 100 s ago is still avoided: got %s", b)
	}
}
