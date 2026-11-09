package game

import (
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"fh2mp/internal/catalog"
	"fh2mp/internal/omap"
	"fh2mp/internal/pyrand"
)

// openConnectionFromTrace takes the first OpenConnection datagram of a recorded trace (a real client's), with its player id.
func openConnectionFromTrace(t *testing.T, hub *Hub, name string) ([]byte, uint64) {
	raw, err := os.ReadFile(filepath.Join("..", "..", "testdata", "game_"+name+".json"))
	if err != nil {
		t.Skip("trace not recorded:", err)
	}
	var tr trace
	if err := json.Unmarshal(raw, &tr); err != nil {
		t.Fatal(err)
	}
	for _, ev := range tr.Events {
		if ev.Kind != "dgram" {
			continue
		}
		data, _ := hex.DecodeString(ev.Hex)
		if pid, _, opened := hub.OpenConnectionFields(data); opened {
			return data, pid
		}
	}
	t.Fatalf("trace %s holds no OpenConnection", name)
	return nil, 0
}

// Datagrams that fail authentication must not pause the workers: a forged ticket, a side-channel request nobody's key signed and an
// OpenConnection of a revoked player used to each stop every worker before any check (one global pause per packet, from anyone).
func TestUnauthenticatedDatagramsPauseNoWorker(t *testing.T) {
	t.Setenv("FH2MP_UDP_AUTH", "ticket")
	clk := &fakeClock{t: 1000.0}
	sock := &fakeSock{clk: clk}
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	hub, err := NewHub(HubConfig{Clock: clk, Rng: pyrand.New(1), Sock: sock, Out: NewLogOut(func(string) error { return nil }), Catalog: cat,
		CtlSend: func([]byte, Addr) {}, Sinks: &Sinks{}})
	if err != nil {
		t.Fatal(err)
	}
	hub.StartWorkers(2, true)
	t.Cleanup(hub.StopWorkers)
	before := hub.stopCalls.Load()

	// a ticket-shaped datagram with a made-up signature for a player who never signed in
	tk := make([]byte, TicketLen)
	copy(tk[11:19], TicketMagic)
	tk[19] = 1
	binary.LittleEndian.PutUint64(tk[20:], 0x9000000000000001)
	binary.LittleEndian.PutUint64(tk[28:], uint64(clk.Wall()))
	for i := 52; i < 84; i++ {
		tk[i] = byte(i)
	}
	hub.HandleDatagram(tk, Addr{"10.0.0.1", 5000})

	// a lighting request from a player who never signed in (no key can have signed it)
	lt := make([]byte, lightingRequest)
	copy(lt, sideHeader[:9])
	binary.LittleEndian.PutUint16(lt[9:], uint16(len(lt)))
	copy(lt[11:19], lightingMagic)
	lt[19] = 1
	binary.LittleEndian.PutUint64(lt[20:], 0x9000000000000002)
	binary.LittleEndian.PutUint64(lt[28:], 1)
	hub.HandleDatagram(lt, Addr{"10.0.0.2", 5000})

	// a real client's OpenConnection, for a player the account service revoked
	data, pid := openConnectionFromTrace(t, hub, "tickets")
	hub.Revoked[pid] = clk.t + 100
	hub.HandleDatagram(data, Addr{"10.0.0.3", 5000})

	if n := hub.stopCalls.Load() - before; n != 0 {
		t.Fatalf("%d worker pause(s) for datagrams that fail authentication; want 0", n)
	}
	if hub.Route.Len() != 0 || hub.Nonces.Len() != 0 {
		t.Fatalf("refused datagrams left state behind: %d route(s), %d nonce(s)", hub.Route.Len(), hub.Nonces.Len())
	}
}

// A ticket budget per player: one account cannot fill the replay table for everyone.
func TestTicketNonceBudgetPerPlayer(t *testing.T) {
	clk := &fakeClock{t: 1000.0}
	hub := &Hub{Clk: clk, Nonces: omap.New[nonceKey, float64](), NonceCount: map[uint64]int{}, Keys: map[uint64]map[string]float64{}, Revoked: map[uint64]float64{}, Bound: map[Addr]boundEnt{}, AuthLog: map[any]float64{}}
	hub.NonceCount[7] = MaxTicketNoncesPerXuid
	tk := make([]byte, TicketLen)
	tk[19] = 1
	binary.LittleEndian.PutUint64(tk[20:], 7)
	binary.LittleEndian.PutUint64(tk[28:], uint64(clk.Wall()))
	if _, why := hub.ticketCheck(tk, Addr{"10.0.0.1", 1}, clk.t); why != "replay protection full for this player" {
		t.Fatalf("ticket over the player's budget: %q", why)
	}
	hub.NonceCount[7] = 0
	if _, why := hub.ticketCheck(tk, Addr{"10.0.0.1", 1}, clk.t); why != "bad signature or no session" {
		t.Fatalf("ticket within the budget but unsigned: %q", why)
	}
}
