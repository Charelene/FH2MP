package lsp

import (
	"errors"
	"fmt"
	"testing"

	"fh2mp/internal/auth"
	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
	"fh2mp/internal/mt/tl"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/social"
	"fh2mp/internal/xbl"
)

// lobbyHub answers the hub's lobby query with one Free Roam lobby holding the given players.
type lobbyHub struct{ members []any }

func (h *lobbyHub) Populations() (map[string][]string, bool) { return map[string][]string{}, true }
func (h *lobbyHub) Control(string)                           {}
func (h *lobbyHub) Tools(string) string                      { return "" }
func (h *lobbyHub) Query(q string) pyjson.Obj {
	if q != "lobbies" {
		return nil
	}
	return pyjson.Obj{{K: "lobbies", V: []any{pyjson.Obj{{K: "id", V: "L1"}, {K: "kind", V: "freeroam"}, {K: "memberXuids", V: h.members}}}}}
}

func authCode(err error) string {
	var ae *auth.Error
	if errors.As(err, &ae) {
		return ae.Code
	}
	return fmt.Sprint(err)
}

// The chat's friends and club API (chatCommunity) on a real database: the same rules as the launcher's HTTP API.
func TestChatCommunity(t *testing.T) {
	d, err := db.Open(dbtest.New(t, ""))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(d.Close)
	const a, b, c = 0x9000000000301, 0x9000000000302, 0x9000000000303
	now := nowF()
	err = d.Do(func(tx *db.Tx) error {
		for i, x := range []int64{a, b, c} {
			tag := []string{"Alpha", "Bravo", "Charlie"}[i]
			if _, err := tx.Exec("INSERT INTO accounts (xuid, pubkey, gamertag, gamertag_key, created, created_ip) VALUES (?,?,?,?,?,?)",
				x, []byte{byte(i), 1, 2, 3}, tag, auth.FoldTag(tag), now, "127.0.0.1"); err != nil {
				return err
			}
			if _, err := tx.Exec("INSERT INTO players (xuid, gamertag, career_level, first_seen, last_seen) VALUES (?,?,0,?,?)", x, tag, now, now); err != nil {
				return err
			}
		}
		if _, err := tx.Exec("INSERT INTO friendships (a, b, created) VALUES (?,?,?)", a, b, now); err != nil {
			return err
		}
		if _, err := tx.Exec("INSERT INTO friend_requests (sender, recipient, created) VALUES (?,?,?)", c, a, now); err != nil {
			return err
		}
		if _, err := tx.Exec("INSERT INTO clubs (id, tag, name, created) VALUES (1, 'FH', 'Horizon Club', ?)", now); err != nil {
			return err
		}
		for x, role := range map[int64]int{a: 2, b: 0} {
			if _, err := tx.Exec("INSERT INTO club_members (xuid, club_id, role, joined, status_changed) VALUES (?,1,?,?,?)", x, role, now, now); err != nil {
				return err
			}
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	core := &Core{DB: d, Social: social.New(d), Xbl: xbl.New(&lobbyHub{members: []any{fmt.Sprintf("%x", b)}}), tags: map[int64]string{a: "Alpha", b: "Bravo", c: "Charlie"}}
	l := NewLauncher(core)
	core.Launcher = l
	cc := chatCommunity{l}

	fl, err := cc.Friends(a)
	if err != nil {
		t.Fatal(err)
	}
	if cl, ok := fl.Me.GetClub(); !ok || cl.ID != 1 || cl.Role != "leader" || cl.Members != 2 || fl.Me.User.Gamertag != "Alpha" {
		t.Fatalf("me: %+v", fl.Me)
	}
	if len(fl.Friends) != 1 || fl.Friends[0].User.ID != b {
		t.Fatalf("friends: %+v", fl.Friends)
	}
	if p, ok := fl.Friends[0].Presence.(*tl.PresencePlaying); !ok || p.LobbyKind != "freeroam" {
		t.Fatalf("Bravo is in a Free Roam lobby: %#v", fl.Friends[0].Presence)
	}
	if len(fl.Incoming) != 1 || fl.Incoming[0].User.ID != c || len(fl.Outgoing) != 0 {
		t.Fatalf("requests: in %+v out %+v", fl.Incoming, fl.Outgoing)
	}

	m, err := cc.SendClub(a, 1, "  hello club \n")
	if err != nil {
		t.Fatal(err)
	}
	if m.ID == 0 || m.Text != "hello club" || m.Sender.Gamertag != "Alpha" || m.ClubID != 1 {
		t.Fatalf("sent %+v", m)
	}
	h, err := cc.ClubHistory(b, 1, 0, 50)
	if err != nil || len(h.Messages) != 1 || h.Messages[0].ID != m.ID || h.Messages[0].Sender.Gamertag != "Alpha" || h.ReadMaxID != 0 {
		t.Fatalf("history for Bravo: %+v %v", h, err)
	}
	if err := cc.ReadClub(b, 1, m.ID); err != nil {
		t.Fatal(err)
	}
	if h, _ = cc.ClubHistory(b, 1, 0, 50); h.ReadMaxID != m.ID {
		t.Fatalf("read mark %d, %d expected", h.ReadMaxID, m.ID)
	}
	if h, _ = cc.ClubHistory(b, 1, m.ID, 50); len(h.Messages) != 0 {
		t.Fatalf("history after the last id: %+v", h.Messages)
	}
	for _, tc := range []struct {
		who, club  int64
		text, code string
	}{{c, 1, "let me in", "chat_unavailable"}, {a, 2, "wrong club", "chat_unavailable"}, {a, 1, "   ", "bad_message"}, {a, 1, "bell\a", "bad_message"}} {
		if _, err := cc.SendClub(tc.who, tc.club, tc.text); authCode(err) != tc.code {
			t.Errorf("SendClub(%#x, %d, %q): %v, %s expected", tc.who, tc.club, tc.text, err, tc.code)
		}
	}
	if _, err := cc.ClubHistory(c, 1, 0, 10); authCode(err) != "chat_unavailable" {
		t.Errorf("outsider history: %v", err)
	}

	if err := cc.AddFriend(a, "Nobody Here"); authCode(err) != "player_not_found" {
		t.Errorf("add an unknown player: %v", err)
	}
	if err := cc.AddFriend(a, "bravo"); authCode(err) != "already_friends" {
		t.Errorf("add a friend again: %v", err)
	}
	if err := cc.Respond(a, c, true); err != nil {
		t.Fatal(err)
	}
	if fl, _ = cc.Friends(a); len(fl.Friends) != 2 || len(fl.Incoming) != 0 {
		t.Fatalf("after accepting Charlie: %+v", fl)
	}
	if err := cc.RemoveFriend(a, c); err != nil {
		t.Fatal(err)
	}
	if fl, _ = cc.Friends(c); len(fl.Friends) != 0 {
		t.Fatalf("Charlie after removal: %+v", fl.Friends)
	}
	g, err := cc.Gamerpic(a)
	if err != nil || len(g.Png) < 8 || string(g.Png[1:4]) != "PNG" {
		t.Fatalf("gamerpic: %d bytes %v", len(g.Png), err)
	}
}
