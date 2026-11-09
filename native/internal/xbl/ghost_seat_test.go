package xbl

import "testing"

// Prod 2026-11-09 20:20 UTC, worlds of 12: lobby c4d3a5ae had 11 connected players, its session listed 12 active members (two had signed out 4 min
// and 19 s before, their games gone without leaving the session). A player matched into it as "fullest with room (11 of 12)" read the full session,
// waited 22 s and left without an OpenConnection; the next player was sent there too.
func lobbyOf11PlusAGhost(t *testing.T) (*heldSeatRig, string) {
	t.Helper()
	r := newHeldSeatRig(t)
	a := ""
	for _, x := range []string{"1001", "1002", "1003", "1004", "1005", "1006", "1007", "1008", "1009", "1010", "1011", "1012"} {
		got := r.match(x, true)
		if a == "" {
			a = got
		} else if got != a {
			t.Fatalf("player %s went to %s, want the lobby that fills first, %s", x, got, a)
		}
		r.put(a, joinBody, x)
	}
	r.disconnect("1012") // its game ended (closed window, crash): no leave in the session
	r.clock += 60        // its match reservation (ReserveSeconds) is long over; an active member stays for ActiveMemberIdleSeconds
	return r, a
}

func TestALobbyWhoseSessionIsFullIsNotPicked(t *testing.T) {
	r, a := lobbyOf11PlusAGhost(t)
	if n := len(r.hub.pops[a]); n != 11 {
		t.Fatalf("the game server has %d players in the lobby, want 11", n)
	}
	if b := r.match("2001", false); b == a {
		t.Fatal("matched into the lobby whose session already lists 12 members: the game reads it as full and gives up")
	}
}

func TestTheOldCountSentPlayersIntoTheFullSession(t *testing.T) { // the switch is what the test above depends on
	CountSessionMembers = false
	defer func() { CountSessionMembers = true }()
	r, a := lobbyOf11PlusAGhost(t)
	if b := r.match("2001", false); b != a {
		t.Fatalf("with connected players alone the lobby counts 11 of 12 and is picked: got %s, want %s", b, a)
	}
}

func TestDroppingTheGhostFreesItsSeat(t *testing.T) { // what the launcher's sign-out after the game does (lsp.Core.OnLogout)
	r, a := lobbyOf11PlusAGhost(t)
	r.s.DropPlayer("1012")
	if b := r.match("2001", false); b != a {
		t.Fatalf("after the ghost's seat is dropped the lobby has room again (11 of 12): got %s, want %s", b, a)
	}
	if b := r.match("2002", false); b == a {
		t.Fatal("the lobby now holds 11 connected players and one on its way: it is full for the next player")
	}
}

func TestAMemberOnItsWayIsCountedOnce(t *testing.T) { // matched, joined the session, not connected yet: one seat, not two
	r := newHeldSeatRig(t)
	a := r.match("1001", true)
	r.put(a, joinBody, "1001")
	if got := r.match("1002", false); got != a {
		t.Fatalf("the second player went to %s, want %s", got, a)
	}
	r.put(a, joinBody, "1002") // reserved by its match and a session member, the game server has not seen it yet
	r.s.mu.Lock()
	n := r.s.lobbyOccupancy(a, r.s.lookup("s", a), r.hub.pops, true)
	r.s.mu.Unlock()
	if n != 2 {
		t.Fatalf("the lobby counts %d players, want 2 (one connected, one on its way)", n)
	}
}
