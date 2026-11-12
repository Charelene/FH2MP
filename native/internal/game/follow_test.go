package game

import (
	"testing"

	"fh2mp/internal/catalog"
	"fh2mp/internal/pyrand"
)

// Dev 2026-11-12: win1 accepted win2's invite while in a Car Meet; its game joined win2's private Free Roam as a Car Meet (lobby type 3)
// and the hub made it a Car Meet world of its own under the same name. A private game's member ("assign ... follow") goes to the world
// of that name that has players.
func TestAPrivateGamesMemberFollowsIntoThePopulatedWorld(t *testing.T) {
	t.Setenv("FH2MP_UDP_AUTH", "off")
	clk := &fakeClock{t: 1000.0}
	cat, err := catalog.Load()
	if err != nil {
		t.Skip("catalog not available:", err)
	}
	hub, err := NewHub(HubConfig{Clock: clk, Rng: pyrand.New(1), Sock: &fakeSock{clk: clk}, Out: NewLogOut(func(string) error { return nil }),
		Catalog: cat, CtlSend: func([]byte, Addr) {}, Sinks: &Sinks{}})
	if err != nil {
		t.Fatal(err)
	}
	name := "33fe4922-5f61-4353-8106-6fd85869ac14"
	host, joiner := Addr{"10.0.0.1", 5000}, Addr{"10.0.0.2", 5000}
	hub.Route.Set(host, hub.WorldFor(LobbyTypeFreeRoam, name)) // the host in its private Free Roam

	hub.ControlText("assign 90000abd7df93 "+name, nil)
	if got := hub.followType(0x90000abd7df93, LobbyTypeCarMeet, name, joiner); got != LobbyTypeCarMeet {
		t.Fatalf("a plain assignment changed the lobby type to %d", got)
	}
	hub.ControlText("assign 90000abd7df93 "+name+" follow", nil)
	if !hub.Follow[0x90000abd7df93] {
		t.Fatal("the follow assignment was not taken")
	}
	if got := hub.followType(0x90000abd7df93, LobbyTypeCarMeet, name, joiner); got != LobbyTypeFreeRoam {
		t.Fatalf("a private game's member asking for a Car Meet went to lobby type %d, want the host's Free Roam", got)
	}
	// a Car Meet of that name with players of its own keeps a Car Meet joiner
	hub.Route.Set(Addr{"10.0.0.3", 5000}, hub.WorldFor(LobbyTypeCarMeet, name))
	if got := hub.followType(0x90000abd7df93, LobbyTypeCarMeet, name, joiner); got != LobbyTypeCarMeet {
		t.Fatalf("with players in the Car Meet the joiner went to lobby type %d", got)
	}
	hub.ControlText("assign 90000abd7df93 "+name, nil)
	if hub.Follow[0x90000abd7df93] {
		t.Fatal("a plain assignment kept the follow mark")
	}
}
