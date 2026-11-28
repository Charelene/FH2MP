package xbl

import (
	"os"
	"path/filepath"
	"testing"

	"fh2mp/internal/auth"
	"fh2mp/internal/flags"
)

// The operator's lobbies switch (internal/flags): off, nobody is matched into a lobby and nobody reserves a seat, each with a 503 the game and
// the launcher can show; on again, the same calls work. A player who is already in a world is not touched.
func TestNobodyIsMatchedOrSeatedWhileLobbiesAreSwitchedOff(t *testing.T) {
	r := newHeldSeatRig(t)
	path := filepath.Join(t.TempDir(), "features.json")
	if err := os.WriteFile(path, []byte(`{"lobbies": false}`), 0o644); err != nil {
		t.Fatal(err)
	}
	set := flags.New(path)
	defer flags.SetDefault(set)()

	if _, err := r.s.lobbyFor("s", "FreeRoam_qos", NewDict(), nil, "2001"); !isLobbiesOff(err) {
		t.Fatalf("matchmaking with lobbies off gave %v, want 503 lobbies_off", err)
	}
	if _, err := r.s.Reserve("2001", "", "freeroam"); !isLobbiesOff(err) {
		t.Fatalf("a reservation with lobbies off gave %v, want 503 lobbies_off", err)
	}
	if _, err := r.s.ReserveInvited("2001", "ffbb4ebb-0726-4ebc-9780-ceaf136507f9"); !isLobbiesOff(err) {
		t.Fatalf("an invited seat with lobbies off gave %v, want 503 lobbies_off", err)
	}
	if err := set.Update(map[string]bool{flags.Lobbies: true}); err != nil {
		t.Fatal(err)
	}
	if name := r.match("2001", true); name == "" {
		t.Fatal("with lobbies back on nobody was matched")
	}
	if h, err := r.s.Reserve("2002", "", "freeroam"); err != nil || h == nil {
		t.Fatalf("with lobbies back on a reservation gave %v, %v", h, err)
	}
}

func isLobbiesOff(err error) bool {
	ae, ok := err.(*auth.Error)
	return ok && ae.Status == 503 && ae.Code == "lobbies_off"
}
