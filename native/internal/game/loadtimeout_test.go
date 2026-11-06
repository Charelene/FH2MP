package game

import (
	"strings"
	"testing"

	"fh2mp/internal/catalog"
	"fh2mp/internal/omap"
	"fh2mp/internal/pyrand"
)

// loadRig is a world with n connected participants in an event that started at t=1000 (no network, a fake clock).
type loadRig struct {
	t     *testing.T
	w     *World
	clk   *fakeClock
	peers []*Peer
	log   []string
}

func newLoadRig(t *testing.T, n int) *loadRig {
	t.Helper()
	t.Setenv("FH2MP_UDP_AUTH", "off")
	r := &loadRig{t: t, clk: &fakeClock{t: 1000}}
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
	ev := &Event{Started: 1000, Finished: omap.New[*Peer, float64](), Quit: map[*Peer]bool{}}
	for i := 0; i < n; i++ {
		p := NewPeer(Addr{"127.0.0.1", 40000 + i}, 1000)
		p.Owner, p.Xuid, p.Connected, p.ConnectedAt = i+1, uint64(100+i), true, 900
		w.Peers.Set(p.Addr, p)
		ev.People = append(ev.People, p)
		r.peers = append(r.peers, p)
	}
	w.Event = ev
	return r
}

// at moves the clock to t seconds after the event start and ticks the world.
func (r *loadRig) at(sec float64) {
	r.clk.t = 1000 + sec
	for _, p := range r.peers { // the clients are alive: only the load is in question
		p.LastRx, p.LastKeepalive, p.LastTimeSync = r.clk.t, r.clk.t, r.clk.t
	}
	r.w.Tick()
}

// picked: the participant left its car select (PreRace) sec seconds after the start.
func (r *loadRig) picked(p *Peer, sec float64) {
	ev := r.w.Event
	if ev.PickedAfter == nil {
		ev.PickedAfter = omap.New[*Peer, float64]()
	}
	ev.PickedAfter.Set(p, sec)
	if ev.SelectEnteredAt == nil {
		v := 1000 + 1.0
		ev.SelectEnteredAt = &v
	}
}

func (r *loadRig) loaded(p *Peer) { p.ConsoleVars[ConsoleVarHasLoadedCars] = true }

func (r *loadRig) in(p *Peer) bool {
	if r.w.Event == nil {
		r.t.Fatalf("the event ended; log: %q", r.log)
	}
	return inList(r.w.Event.People, p)
}

func (r *loadRig) count(sub string) int {
	n := 0
	for _, l := range r.log {
		if strings.Contains(l, sub) {
			n++
		}
	}
	return n
}

func TestLoadTimeoutDefault(t *testing.T) {
	if EventLoadTimeout != 30 {
		t.Skipf("FH2MP_EVENT_LOAD_TIMEOUT overrides the default (%v)", EventLoadTimeout)
	}
}

// Everyone is in the car select for ~20 s; that time is not part of the 30 s, so a normal load that finishes 25 s after the select is never dropped.
func TestLoadTimeoutCarSelectTimeDoesNotCount(t *testing.T) {
	r := newLoadRig(t, 3)
	for _, p := range r.peers {
		r.picked(p, 21)
	}
	r.at(40) // 40 s after the event start, only 19 s after the car select ended
	r.at(50) // 29 s after the car select
	for _, p := range r.peers {
		if !r.in(p) {
			t.Fatalf("owner%d was dropped while still within its own load time", p.Owner)
		}
	}
	if n := r.count("event load timeout"); n != 0 {
		t.Fatalf("%d load timeout line(s) before anybody's own time ran out", n)
	}
	r.loaded(r.peers[0])
	r.loaded(r.peers[1])
	r.at(53) // 32 s after the select: only the third one is over its limit
	if r.in(r.peers[2]) || !r.in(r.peers[0]) || !r.in(r.peers[1]) {
		t.Fatalf("expected only owner3 to be dropped, people now: %s", owners(r.w.Event.People))
	}
}

// One slow player is dropped alone; the lobby carries on and starts the grid when the rest have loaded.
func TestLoadTimeoutSlowPlayerAloneIsDropped(t *testing.T) {
	r := newLoadRig(t, 4)
	for _, p := range r.peers {
		r.picked(p, 20)
	}
	r.loaded(r.peers[0])
	r.loaded(r.peers[1])
	r.loaded(r.peers[2])
	r.at(49) // 29 s after the select
	if len(r.w.Event.People) != 4 {
		t.Fatalf("dropped before the limit: %s", owners(r.w.Event.People))
	}
	r.at(51)
	if r.in(r.peers[3]) {
		t.Fatal("the slow player was not dropped")
	}
	for _, p := range r.peers[:3] {
		if !r.in(p) {
			t.Fatalf("owner%d fell off with the slow player", p.Owner)
		}
	}
	if r.w.Event == nil || !r.w.Event.Begun {
		t.Fatal("the remaining players that all loaded did not get the starting grid")
	}
	if n := r.count("event load timeout: dropping [4]"); n != 1 {
		t.Fatalf("want one drop line naming [4], got %d: %q", n, r.log)
	}
	r.at(60)
	r.at(70)
	if n := r.count("event load timeout"); n != 1 {
		t.Fatalf("the drop was logged %d times", n)
	}
}

// Nobody loaded: one suspect is dropped, the rest get a fresh window and are not dropped one after another; the event ends once, later.
func TestLoadTimeoutNobodyLoadedDoesNotCascade(t *testing.T) {
	r := newLoadRig(t, 4)
	for _, p := range r.peers {
		r.picked(p, 20)
	}
	r.at(49)
	if len(r.w.Event.People) != 4 {
		t.Fatalf("dropped before the limit: %s", owners(r.w.Event.People))
	}
	r.at(51)
	if len(r.w.Event.People) != 3 {
		t.Fatalf("want exactly one suspect dropped, people: %s", owners(r.w.Event.People))
	}
	for s := 52.0; s < 80; s += 1 { // 29 s after the restart: nobody else is touched, not even at the old 10 s suspect step
		r.at(s)
		if len(r.w.Event.People) != 3 {
			t.Fatalf("at +%.0f s the lobby lost another player: %s", s, owners(r.w.Event.People))
		}
	}
	r.at(82) // 31 s after the restart: still nobody loaded, but no second suspect
	if r.w.Event == nil || len(r.w.Event.People) != 3 {
		t.Fatal("a second player was dropped one by one")
	}
	r.at(112) // 2 x limit after the restart
	if r.w.Event != nil {
		t.Fatalf("the dead event was not ended, people: %s", owners(r.w.Event.People))
	}
	if n := r.count("event load timeout"); n != 2 {
		t.Fatalf("want two timeout lines (suspect, then end), got %d: %q", n, r.log)
	}
}

// Nobody loaded and only the last player is left: it is never dropped on its own, the event ends once after the long limit.
func TestLoadTimeoutNeverRemovesLastPlayer(t *testing.T) {
	r := newLoadRig(t, 1)
	r.picked(r.peers[0], 20)
	r.at(55)
	r.at(75)
	if r.w.Event == nil || len(r.w.Event.People) != 1 {
		t.Fatal("the only player was dropped by the timeout")
	}
	r.at(85) // 2 x 30 s after the select
	if r.w.Event != nil {
		t.Fatal("the stuck event was not ended")
	}
	if n := r.count("event load timeout"); n != 1 {
		t.Fatalf("want one line, got %d", n)
	}
}

// A participant that (re)connected late has its own 30 s from the connect, not from the event start.
func TestLoadTimeoutLateJoinerGetsOwn30s(t *testing.T) {
	r := newLoadRig(t, 3)
	for _, p := range r.peers {
		r.picked(p, 20)
	}
	r.loaded(r.peers[0])
	r.loaded(r.peers[1])
	r.peers[2].ConnectedAt = 1000 + 45 // joined 45 s after the start
	r.at(60)
	r.at(74) // 29 s after the join
	if !r.in(r.peers[2]) {
		t.Fatal("the late joiner was dropped inside its own 30 s")
	}
	r.at(76)
	if r.in(r.peers[2]) || !r.in(r.peers[0]) || !r.in(r.peers[1]) {
		t.Fatalf("expected only the late joiner to be dropped, people: %s", owners(r.w.Event.People))
	}
}

// Without any car select data the group's hold (deadline + grace) still bounds the wait, so a player that never reports is dropped.
func TestLoadTimeoutNoCarSelectInfoStillBounded(t *testing.T) {
	r := newLoadRig(t, 2)
	r.loaded(r.peers[0])
	r.at(40) // deadline 3 + 20 + 3 = 26 s, plus 30 s: not yet
	if len(r.w.Event.People) != 2 {
		t.Fatalf("dropped too early: %s", owners(r.w.Event.People))
	}
	r.at(57)
	if r.in(r.peers[1]) || !r.in(r.peers[0]) {
		t.Fatalf("people: %s", owners(r.w.Event.People))
	}
}
