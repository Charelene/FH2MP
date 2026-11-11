package mt

import (
	"sync"
	"testing"

	"github.com/gotd/td/bin"

	"fh2mp/internal/auth"
	"fh2mp/internal/mt/tl"
)

// fakeCommunity is one club and fixed friends, in memory.
type fakeCommunity struct {
	mu    sync.Mutex
	s     *Server
	posts []tl.ClubMessage
	club  int64
	in    map[int64]bool // club members
}

func (f *fakeCommunity) Friends(me int64) (*tl.FriendsList, error) {
	c := tl.Contact{User: tl.User{ID: me, Gamertag: "me"}, Presence: &tl.PresenceOnline{}}
	c.SetClub(tl.Club{ID: f.club, Tag: "FH", Name: "Club", Role: "leader", Members: len(f.in)})
	return &tl.FriendsList{Me: c, Friends: []tl.Contact{{User: tl.User{ID: 7, Gamertag: "seven"}, Presence: &tl.PresencePlaying{LobbyKind: "freeroam"}}}}, nil
}
func (f *fakeCommunity) AddFriend(me int64, q string) error {
	return auth.NewError(404, "player_not_found", "No player has that gamertag or ID.")
}
func (f *fakeCommunity) Respond(me, o int64, a bool) error { return nil }
func (f *fakeCommunity) RemoveFriend(me, o int64) error    { return nil }
func (f *fakeCommunity) ClubHistory(me, club, after int64, limit int) (*tl.ClubsHistory, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	h := &tl.ClubsHistory{}
	for _, m := range f.posts {
		if m.ID > after {
			h.Messages = append(h.Messages, m)
		}
	}
	return h, nil
}
func (f *fakeCommunity) SendClub(me, club int64, text string) (*tl.ClubMessage, error) {
	if !f.in[me] || club != f.club {
		return nil, auth.NewError(403, "chat_unavailable", "Club chat requires current membership.")
	}
	f.mu.Lock()
	m := tl.ClubMessage{ID: int64(len(f.posts) + 1), ClubID: club, Sender: tl.User{ID: me}, Text: text, Date: 1}
	f.posts = append(f.posts, m)
	f.mu.Unlock()
	var to []int64
	for x := range f.in {
		if x != me {
			to = append(to, x)
		}
	}
	f.s.PushShort(to, &tl.UpdateClubMessage{Message: m})
	return &m, nil
}
func (f *fakeCommunity) ReadClub(me, club, maxID int64) error { return nil }
func (f *fakeCommunity) Gamerpic(user int64) (*tl.UsersGamerpic, error) {
	return &tl.UsersGamerpic{UserID: user, Version: 3, Png: []byte{0x89, 'P', 'N', 'G'}}, nil
}

func TestCommunityOverChat(t *testing.T) {
	ts := startServer(t, nil)
	const a, b, outsider = 0x9000000000201, 0x9000000000202, 0x9000000000203
	pa, pb, po := ts.accs.add(a, "A"), ts.accs.add(b, "B"), ts.accs.add(outsider, "O")
	ca := dialRaw(t, ts.addr, false).mustKey().init()
	ca.signIn(pa, 1)
	// Without a community the methods say so.
	if e := ca.callErr(&tl.FriendsGetRequest{}); e.ErrorCode != 501 {
		t.Fatalf("friends.get without a community: %+v", e)
	}
	fc := &fakeCommunity{s: ts.s, club: 5, in: map[int64]bool{a: true, b: true}}
	ts.s.SetCommunity(fc)
	var fl tl.FriendsList
	ca.call(&tl.FriendsGetRequest{}, &fl)
	if cl, ok := fl.Me.GetClub(); !ok || cl.ID != 5 || len(fl.Friends) != 1 || fl.Friends[0].Presence.(*tl.PresencePlaying).LobbyKind != "freeroam" {
		t.Fatalf("friends.get: %+v", fl)
	}
	if e := ca.callErr(&tl.FriendsAddRequest{Query: "nobody"}); e.ErrorCode != 404 || e.ErrorMessage != "PLAYER_NOT_FOUND" {
		t.Fatalf("friends.add of nobody: %+v", e)
	}
	cb := dialRaw(t, ts.addr, false).mustKey().init()
	cb.signIn(pb, 2)
	if !ts.s.Online(b) || len(ts.s.SignedIn()) != 2 {
		t.Fatalf("online %v signed in %v", ts.s.Online(b), ts.s.SignedIn())
	}
	// A posts; B gets the push, A gets the message as its answer; the same random_id again posts nothing new.
	var m tl.ClubMessage
	ca.call(&tl.ClubsSendMessageRequest{ClubID: 5, RandomID: 77, Text: "hi club"}, &m)
	var us tl.UpdateShort
	if err := us.Decode(&bin.Buffer{Buf: cb.next(tl.UpdateShortTypeID).body}); err != nil {
		t.Fatal(err)
	}
	if u, ok := us.Update.(*tl.UpdateClubMessage); !ok || u.Message.Text != "hi club" || u.Message.ID != m.ID {
		t.Fatalf("pushed %#v", us.Update)
	}
	var again tl.ClubMessage
	ca.call(&tl.ClubsSendMessageRequest{ClubID: 5, RandomID: 77, Text: "hi club"}, &again)
	if again.ID != m.ID || len(fc.posts) != 1 {
		t.Fatalf("resend with the same random_id posted again: %d posts, answer %d", len(fc.posts), again.ID)
	}
	var h tl.ClubsHistory
	cb.call(&tl.ClubsGetHistoryRequest{ClubID: 5, AfterID: 0, Limit: 50}, &h)
	if len(h.Messages) != 1 {
		t.Fatalf("history %+v", h)
	}
	// Someone outside the club is refused with the community's own error.
	co := dialRaw(t, ts.addr, false).mustKey().init()
	co.signIn(po, 3)
	if e := co.callErr(&tl.ClubsSendMessageRequest{ClubID: 5, RandomID: 1, Text: "let me in"}); e.ErrorCode != 403 || e.ErrorMessage != "CHAT_UNAVAILABLE" {
		t.Fatalf("outsider post: %+v", e)
	}
	var g tl.UsersGamerpic
	cb.call(&tl.UsersGetGamerpicRequest{UserID: a}, &g)
	if g.Version != 3 || string(g.Png[1:4]) != "PNG" {
		t.Fatalf("gamerpic %+v", g)
	}
}
