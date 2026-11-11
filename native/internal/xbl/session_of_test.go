package xbl

import "testing"

// The session a player is in, for a join through the game's own session-ready flow. Dev 2026-11-11 (win1 invited win2 from a private Free
// Roam): the invitee's game was handed the Car Meet lobby the world is named after; the host had left that session for the one the game
// makes for a private Free Roam (template defaultTemplate_qos), so the game read a session without members and said "The session you were
// trying to join no longer exists."

func put(t *testing.T, s *Server, template, name, body, xuid string) {
	t.Helper()
	if st, _ := s.PutSession("/serviceconfigs/s/sessionTemplates/"+template+"/sessions/"+name, []byte(body), xuid); st != 200 {
		t.Fatalf("PUT %s/%s as %s: status %d", template, name, xuid, st)
	}
}

func TestSessionOfFollowsThePlayerFromLobbyToPrivateSession(t *testing.T) {
	r := newHeldSeatRig(t)
	s := r.s
	if _, _, _, ok := s.SessionOf("2001"); ok {
		t.Fatal("a player who wrote no session is in one")
	}
	put(t, s, "defaultTemplate_qos", "ticket1", joinBody, "2001") // the matchmaking ticket's session
	put(t, s, "defaultTemplate", "meet1", joinBody, "2001")       // the Car Meet lobby
	if _, tmpl, name, ok := s.SessionOf("2001"); !ok || tmpl != "defaultTemplate" || name != "meet1" {
		t.Fatalf("in the lobby: got %s/%s ok=%v", tmpl, name, ok)
	}
	put(t, s, "defaultTemplate", "meet1", leaveBody, "2001") // Car Meet -> private Free Roam: out of the lobby's session ...
	if _, _, name, ok := s.SessionOf("2001"); ok {
		t.Fatalf("after leaving the lobby the player is still in %s", name)
	}
	put(t, s, "defaultTemplate_qos", "private1", joinBody, "2001") // ... into the one the game makes
	if _, tmpl, name, ok := s.SessionOf("2001"); !ok || tmpl != "defaultTemplate_qos" || name != "private1" {
		t.Fatalf("in the private Free Roam: got %s/%s ok=%v", tmpl, name, ok)
	}
}

func TestSessionOfIgnoresAnotherPlayersLeave(t *testing.T) {
	r := newHeldSeatRig(t)
	s := r.s
	put(t, s, "defaultTemplate", "lobby1", joinBody, "2001")
	put(t, s, "defaultTemplate", "lobby1", joinBody, "2002")
	put(t, s, "defaultTemplate", "lobby1", leaveBody, "2002")
	if _, _, name, ok := s.SessionOf("2001"); !ok || name != "lobby1" {
		t.Fatalf("2001 stays in lobby1 when 2002 leaves: got %s ok=%v", name, ok)
	}
	if _, _, name, ok := s.SessionOf("2002"); ok {
		t.Fatalf("2002 left and is still in %s", name)
	}
}
