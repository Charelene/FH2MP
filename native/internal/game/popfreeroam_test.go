package game

import (
	"strings"
	"testing"

	"fh2mp/internal/catalog"
	"fh2mp/internal/pyrand"
)

// The pop report names the lobbies with a Free Roam world that has a connected player (the LSP keeps a Car Meet that went to Free Roam
// out of matchmaking with it); a Car Meet world, an empty Free Roam world and the switch off leave it out.
func TestPopReportNamesFreeRoamLobbies(t *testing.T) {
	clk := &fakeClock{t: 1000.0}
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	hub, err := NewHub(HubConfig{Clock: clk, Rng: pyrand.New(1), Sock: &fakeSock{clk: clk}, Out: NewLogOut(func(string) error { return nil }), Catalog: cat,
		CtlSend: func([]byte, Addr) {}, Sinks: &Sinks{}})
	if err != nil {
		t.Fatal(err)
	}
	prev := PopFreeRoam
	t.Cleanup(func() { PopFreeRoam = prev })
	PopFreeRoam = true

	join := func(lobbyType int, name string, addr Addr, connected bool) {
		w := hub.WorldFor(lobbyType, name)
		p := NewPeer(addr, clk.t)
		p.Connected = connected
		w.Peers.Set(addr, p)
	}
	join(LobbyTypeCarMeet, "meet-a", Addr{"10.0.0.1", 1}, true)
	join(LobbyTypeFreeRoam, "meet-b", Addr{"10.0.0.2", 2}, true)
	join(LobbyTypeFreeRoam, "empty-c", Addr{"10.0.0.3", 3}, false)

	got := hub.populationsText()
	if !strings.HasSuffix(got, " !freeroam=meet-b") {
		t.Fatalf("pop report %q: want it to end with the one Free Roam lobby that has a player", got)
	}
	PopFreeRoam = false
	if got := hub.populationsText(); strings.Contains(got, "!freeroam") {
		t.Fatalf("switched off, the report still lists Free Roam lobbies: %q", got)
	}
}
