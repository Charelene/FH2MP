package game

import (
	"encoding/binary"
	"encoding/hex"
	"os"
	"strings"
	"testing"

	"fh2mp/internal/catalog"
	"fh2mp/internal/pyrand"
)

// elimVectorRequest is the request shim/tests/test_eliminator.c builds with el_request: key 0..31, xuid
// 0x0102030405060708, seq 5, epoch a0..af, car 3, a claim of drop 7.
func elimVectorRequest() (payload, key []byte) {
	key = make([]byte, 32)
	for i := range key {
		key[i] = byte(i)
	}
	p := make([]byte, elimBase)
	copy(p, sideHeader)
	binary.LittleEndian.PutUint16(p[9:], elimRequest)
	copy(p[11:], elimMagic)
	p[19] = 1
	binary.LittleEndian.PutUint64(p[20:], 0x0102030405060708)
	binary.LittleEndian.PutUint64(p[28:], 5)
	for i := 0; i < 16; i++ {
		p[36+i] = byte(0xa0 + i)
	}
	p[52], p[53], p[54] = 3, ElimCmdDropClaim, 7
	return p, key
}

// The wire both ways against the frozen vectors the shim's own test reads (shim/tests/test_eliminator.sh):
// the request the shim builds, and the reply the shim must accept. A reply that is longer than a request
// (any with drops) was refused by the shim while its length field was the request's.
func TestEliminatorWireVectors(t *testing.T) {
	payload, key := elimVectorRequest()
	e := NewEliminator(&elimRigRand{})
	e.Phase, e.Area = ElimPhaseActive, 2
	e.Players[1] = &EliminatorPlayer{Owner: 1, Tier: 3, ChallengedBy: 2}
	e.Players[2] = &EliminatorPlayer{Owner: 2}
	e.Drops = []EliminatorDrop{{ID: 1, Class: 4, X: 100.5, Z: -200.25}, {ID: 2, Class: 0, X: -1, Z: 2}}
	reply := signed(e.snapshot(payload, 1), key)
	if got := int(binary.LittleEndian.Uint16(reply[9:])); got != len(reply) {
		t.Fatalf("reply length field %d of %d bytes", got, len(reply))
	}
	if reply[56] != elimFlagShrinking|elimFlagJoined || reply[elimBase] != 3 || reply[elimBase+1] != 2 {
		t.Fatalf("reply flags %d, tier %d, challenger %d", reply[56], reply[elimBase], reply[elimBase+1])
	}
	for name, got := range map[string][]byte{"eliminator_request.hex": signed(payload, key), "eliminator_reply.hex": reply} {
		want, err := os.ReadFile("../../../shim/tests/vectors/" + name)
		if err != nil {
			t.Fatalf("%v (expected content: %s)", err, hex.EncodeToString(got))
		}
		if strings.TrimSpace(string(want)) != hex.EncodeToString(got) {
			t.Errorf("%s is not what the server builds: %s", name, hex.EncodeToString(got))
		}
	}
}

// Which worlds host the mode: lobby type 4 always, Free Roam only with the test switch FH2MP_ELIMINATOR=1, a Car Meet never.
func TestEliminatorHostWorlds(t *testing.T) {
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
	if hub.WorldFor(LobbyTypeFreeRoam, "off").El != nil {
		t.Fatal("a Free Roam world hosts the mode without the switch")
	}
	if hub.WorldFor(LobbyTypeEliminator, "el").El == nil {
		t.Fatal("a world of lobby type 4 has no Eliminator")
	}
	t.Setenv("FH2MP_ELIMINATOR", "1")
	if w := hub.WorldFor(LobbyTypeFreeRoam, "on"); w.El == nil || !w.Eliminator {
		t.Fatal("a Free Roam world does not host the mode with FH2MP_ELIMINATOR=1")
	}
	if hub.WorldFor(LobbyTypeCarMeet, "meet").El != nil || hub.WorldFor(LobbyTypeFreeRoam, "off").El != nil {
		t.Fatal("the switch reached a Car Meet, or a Free Roam world made before it")
	}
}

// A player who goes (Leave, or the connection drops) frees the owner id and takes their challenge along.
func TestEliminatorForget(t *testing.T) {
	e := NewEliminator(&elimRigRand{})
	a, b := &Peer{Owner: 1}, &Peer{Owner: 2}
	e.Handle(ElimCmdJoin, 0, 0, a, 0)
	e.Handle(ElimCmdJoin, 0, 0, b, 0)
	e.Handle(ElimCmdChallenge, 2, 0, a, 0)
	e.Handle(ElimCmdChallenge, 1, 0, a, 0) // not oneself
	if e.Players[2].ChallengedBy != 1 || e.Players[1].ChallengedBy != 0 {
		t.Fatalf("challenge: %d %d", e.Players[2].ChallengedBy, e.Players[1].ChallengedBy)
	}
	e.Forget(1)
	if len(e.Players) != 1 || e.Players[2].ChallengedBy != 0 {
		t.Fatalf("after forget: players %d, challenger %d", len(e.Players), e.Players[2].ChallengedBy)
	}
}

// elimRigRand is a deterministic Rand for the Eliminator tests: Float64 always 0.5, Intn cycles 0,1,2,…
type elimRigRand struct{ i int }

func (r *elimRigRand) Intn(n int) int   { v := r.i % n; r.i++; return v }
func (r *elimRigRand) Float64() float64 { r.i++; return 0.5 }

func TestEliminatorLifecycle(t *testing.T) {
	e := NewEliminator(&elimRigRand{})
	if e.Phase != ElimPhaseWaiting || e.remaining() != 0 {
		t.Fatalf("new: phase %d remaining %d", e.Phase, e.remaining())
	}
	a, b := &Peer{Owner: 1}, &Peer{Owner: 2}
	e.Handle(ElimCmdJoin, 0, 0, a, 0)
	e.Handle(ElimCmdJoin, 0, 0, b, 0)
	if len(e.Players) != 2 || e.remaining() != 2 {
		t.Fatalf("join: players %d remaining %d", len(e.Players), e.remaining())
	}

	// Waiting -> Dropping once enough players joined, and the drops are placed.
	e.Tick(1)
	if e.Phase != ElimPhaseDropping || len(e.Drops) != ElimDropCount {
		t.Fatalf("dropping: phase %d drops %d", e.Phase, len(e.Drops))
	}
	// Dropping -> Active after ElimDropDelayS.
	e.Tick(1 + ElimDropDelayS + 1)
	if e.Phase != ElimPhaseActive {
		t.Fatalf("active: phase %d", e.Phase)
	}

	// A car-drop claim upgrades the tier and removes the drop.
	p := e.Players[1]
	id := e.Drops[0].ID
	e.Drops[0].Class = 3 // force a class upgrade
	e.Handle(ElimCmdDropClaim, uint32(id), 0, a, 100)
	if p.Tier != 3 {
		t.Fatalf("tier after claim %d", p.Tier)
	}
	if len(e.Drops) != ElimDropCount-1 {
		t.Fatalf("drop not removed: %d", len(e.Drops))
	}

	// Challenge -> accept clears the challenge bookkeeping.
	e.Handle(ElimCmdChallenge, 2, 0, a, 100)
	if e.Players[2].ChallengedBy != 1 {
		t.Fatalf("challenge not recorded")
	}
	e.Handle(ElimCmdAccept, 1, 0, b, 100)
	if e.Players[2].ChallengedBy != 0 {
		t.Fatalf("challenge not cleared")
	}

	// Eliminate b -> last player standing finishes the mode.
	e.Players[2].Eliminated = true
	e.Tick(200)
	if e.Phase != ElimPhaseFinished {
		t.Fatalf("finished: phase %d", e.Phase)
	}
}
