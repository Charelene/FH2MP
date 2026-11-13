package game

import (
	"testing"

	"fh2mp/internal/bits"
	"fh2mp/internal/capacity"
	"fh2mp/internal/catalog"
	"fh2mp/internal/netv2"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/pyrand"
)

// Storm Island sessions (types 4..6, game+0x80e7d0) used to be folded into the mainland default at the handshake: an island player got a
// mainland world and SessionHostSynced.LobbyType kept its default 2 (mainland Free Roam).

func newIslandHub(t *testing.T) *Hub {
	t.Helper()
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
	return hub
}

// withLobbyType is a real client's OpenConnection datagram with its lobby type field (a, 32 bits after the player id) set to lt.
func withLobbyType(t *testing.T, data []byte, lt uint32) []byte {
	t.Helper()
	out := append([]byte(nil), data...)
	r := bits.NewReader(out)
	if _, err := netv2.ParseHeader(r); err != nil {
		t.Fatal(err)
	}
	if _, err := r.Bits(7); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 2; i++ {
		if _, err := r.String(); err != nil {
			t.Fatal(err)
		}
	}
	if err := r.Skip(2 + 64); err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 32; i++ {
		pos := r.Pos + i
		if lt>>uint(i)&1 == 1 {
			out[pos>>3] |= 1 << uint(pos&7)
		} else {
			out[pos>>3] &^= 1 << uint(pos&7)
		}
	}
	return out
}

func TestIslandOpenConnectionKeepsItsType(t *testing.T) {
	hub := newIslandHub(t)
	data, pid := openConnectionFromTrace(t, hub, "join_relay")
	for _, c := range []struct {
		lt                       uint32
		island, roadtrip, carmet bool
	}{{5, true, false, false}, {6, true, false, true}, {4, true, true, false}, {2, false, false, false}} {
		addr := Addr{"10.0.0.1", 6000 + int(c.lt)}
		dg := withLobbyType(t, data, c.lt)
		if got, lt, ok := hub.OpenConnectionFields(dg); !ok || got != pid || lt != int(c.lt) {
			t.Fatalf("type %d: OpenConnectionFields = (%#x, %d, %v)", c.lt, got, lt, ok)
		}
		hub.Handle(dg, addr)
		w, ok := hub.Route.Get(addr)
		if !ok || w == nil || w.HubKey == nil {
			t.Fatalf("type %d: not routed to a world", c.lt)
		}
		if w.HubKey.Type != int(c.lt) || w.Island != c.island || w.Roadtrip != c.roadtrip || w.Carmeet != c.carmet {
			t.Fatalf("type %d: world %s type %d island %v roadtrip %v carmeet %v", c.lt, w.Tag, w.HubKey.Type, w.Island, w.Roadtrip, w.Carmeet)
		}
		peer, ok := w.Peers.Get(addr)
		if !ok || peer.LobbyType != int(c.lt) {
			t.Fatalf("type %d: peer lobby type %v", c.lt, peer)
		}
		// the next client of the same player id comes from another address; drop this one so that it is not replaced
		hub.Route.Delete(addr)
		w.Peers.Delete(addr)
	}
}

// sessionHostLobbyType reads the LobbyType a SessionHostSynced create would carry (-1 = none, the game keeps its default 2).
func sessionHostLobbyType(t *testing.T, vw netv2.VarsWriter) int {
	t.Helper()
	if vw == nil {
		return -1
	}
	var b bits.Writer
	vw(&b)
	r := bits.NewReader(b.Bytes())
	id, err := r.Bits(netv2.SessionHostVarBits)
	if err != nil {
		t.Fatal(err)
	}
	if id != SHLobbyType {
		return -1
	}
	v, err := r.Bits(SHLobbyTypeBits)
	if err != nil {
		t.Fatal(err)
	}
	return int(v)
}

func TestIslandSessionHostTellsItsType(t *testing.T) {
	hub := newIslandHub(t)
	for _, c := range []struct{ world, peer, want int }{
		{LobbyTypeIslandFreeRoam, LobbyTypeIslandFreeRoam, LobbyTypeIslandFreeRoam},
		{LobbyTypeIslandCarMeet, LobbyTypeIslandCarMeet, LobbyTypeIslandCarMeet},
		{LobbyTypeIslandRoadTrip, LobbyTypeIslandRoadTrip, LobbyTypeIslandRoadTrip},
		{LobbyTypeFreeRoam, LobbyTypeFreeRoam, -1},
		{LobbyTypeCarMeet, LobbyTypeCarMeet, LobbyTypeCarMeet},
		{LobbyTypeRoadTrip, LobbyTypeRoadTrip, LobbyTypeRoadTrip},
	} {
		w := hub.WorldFor(c.world, "sh-test")
		p := NewPeer(Addr{"10.0.0.9", 7000 + c.world}, 1000)
		p.LobbyType = c.peer
		if got := sessionHostLobbyType(t, w.sessionHostCreateVars(p)); got != c.want {
			t.Errorf("world type %d, peer type %d: SessionHost LobbyType %d, want %d", c.world, c.peer, got, c.want)
		}
	}
}

// A private game's member follows into the populated world of its lobby name only on its own side: a mainland Car Meet never joins an
// island Free Roam (the game would load the mainland and be told it is on the island).
func TestFollowNeverCrossesSides(t *testing.T) {
	hub := newIslandHub(t)
	name := "5b1e0c8e-2f1a-4c55-9a43-6a4c2d6e0f11"
	hub.Route.Set(Addr{"10.0.0.1", 5000}, hub.WorldFor(LobbyTypeIslandFreeRoam, name))
	hub.ControlText("assign 90000abd7df93 "+name+" follow", nil)
	joiner := Addr{"10.0.0.2", 5000}
	if got := hub.followType(0x90000abd7df93, LobbyTypeIslandCarMeet, name, joiner); got != LobbyTypeIslandFreeRoam {
		t.Fatalf("an island Car Meet member went to lobby type %d, want the host's island Free Roam", got)
	}
	if got := hub.followType(0x90000abd7df93, LobbyTypeCarMeet, name, joiner); got != LobbyTypeCarMeet {
		t.Fatalf("a mainland Car Meet member went to lobby type %d, want its own type", got)
	}
}

func TestIslandLobbiesAreListed(t *testing.T) {
	hub := newIslandHub(t)
	w := hub.WorldFor(LobbyTypeIslandFreeRoam, "isl-1")
	p := NewPeer(Addr{"10.0.0.3", 5000}, 1000)
	p.LobbyType, p.Xuid = LobbyTypeIslandFreeRoam, 0x9000000000000123
	w.Peers.Set(p.Addr, p)
	if !w.setPeerState(p, PeerConnected, "test") {
		t.Fatal("peer not connected")
	}
	rows := hub.lobbyStatus()[0].V.([]any)
	if len(rows) != 1 {
		t.Fatalf("%d lobby rows, want 1", len(rows))
	}
	row := rows[0].(pyjson.Obj)
	if k, _ := row.Get("kind"); k != "islandfreeroam" {
		t.Fatalf("kind %v, want islandfreeroam", k)
	}
	if st, _ := row.Get("status"); st != "Free roaming on Storm Island" {
		t.Fatalf("status %v", st)
	}
	if c, _ := row.Get("capacity"); c != capacity.FreeRoam() {
		t.Fatalf("capacity %v, want the Free Roam capacity %d", c, capacity.FreeRoam())
	}
	if fr := hub.freeRoamLobbies(); len(fr) != 1 || fr[0] != "isl-1" {
		t.Fatalf("Free Roam lobbies %v, want the island one", fr)
	}
}
