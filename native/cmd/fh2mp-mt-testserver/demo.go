package main

// -demo: a chat server to try the chat window against on one PC, never a real one. Any account key that signs correctly is let in (its id is
// made from the key), it has three made-up friends and a made-up club with a few messages. stdin commands drive the made-up side:
//
//	say <friend> <text>            the friend posts in the club (pushed to everyone signed in)
//	presence <friend> <state>      offline | online | freeroam | roadtrip
//	request <friend>               the friend sends a friend request

import (
	"crypto/ed25519"
	"encoding/binary"
	"fmt"
	"strings"
	"sync"
	"time"

	"fh2mp/internal/auth"
	"fh2mp/internal/mt"
	"fh2mp/internal/mt/tl"
	"fh2mp/internal/social"
)

type demoFriend struct {
	id    int64
	tag   string
	state string
}

type demo struct {
	mu       sync.Mutex
	s        *mt.Server
	realm    string
	chals    map[string]bool
	tags     map[int64]string
	friends  []*demoFriend
	incoming map[int64]bool
	club     tl.Club
	posts    []tl.ClubMessage
	reads    map[int64]int64
}

func newDemo(realm string) *demo {
	d := &demo{realm: realm, chals: map[string]bool{}, tags: map[int64]string{}, incoming: map[int64]bool{}, reads: map[int64]int64{},
		club: tl.Club{ID: 42, Tag: "FHT", Name: "FH2MP Testers", Role: "member", Members: 4}}
	for i, f := range []struct{ tag, state string }{{"Maplefox21378", "freeroam"}, {"Pixelwolf21378", "online"}, {"DebugFox", "offline"}} {
		d.friends = append(d.friends, &demoFriend{id: 0x9000000000d01 + int64(i), tag: f.tag, state: f.state})
	}
	now := int(time.Now().Unix())
	for i, p := range []struct {
		who  int
		text string
	}{{0, "anyone up for a convoy around Nice?"}, {1, "give me 5 min, tuning the Zonda"}, {0, "ok, meet at the festival hub"}} {
		f := d.friends[p.who]
		d.posts = append(d.posts, tl.ClubMessage{ID: int64(i + 1), ClubID: 42, Sender: tl.User{ID: f.id, Gamertag: f.tag}, Text: p.text, Date: now - 600 + i*60})
	}
	return d
}

func (d *demo) Challenge(ip string) ([]byte, float64, error) {
	b := make([]byte, 41)
	for i := range b {
		b[i] = byte(time.Now().UnixNano() >> (i % 8))
	}
	d.mu.Lock()
	d.chals[string(b)] = true
	d.mu.Unlock()
	return b, 120, nil
}

func (d *demo) SignIn(ip string, chal, pub, sig []byte, fields ...[]byte) (int64, string, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if !d.chals[string(chal)] {
		return 0, "", auth.NewError(400, "challenge_used", "that challenge was already used")
	}
	delete(d.chals, string(chal))
	if len(pub) != 32 || !ed25519.Verify(pub, auth.SignMessage(d.realm, mt.Purpose, chal, append([][]byte{pub}, fields...)...), sig) {
		return 0, "", auth.NewError(401, "bad_signature", "")
	}
	x := int64(0x9000000000000 | uint64(binary.BigEndian.Uint32(pub)|0x10000))
	if d.tags[x] == "" {
		d.tags[x] = "You"
	}
	return x, d.tags[x], nil
}

func presence(state string) tl.PresenceClass {
	switch state {
	case "online":
		return &tl.PresenceOnline{}
	case "freeroam", "roadtrip":
		return &tl.PresencePlaying{LobbyKind: state}
	}
	return &tl.PresenceOffline{}
}

func (d *demo) Friends(me int64) (*tl.FriendsList, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	m := tl.Contact{User: tl.User{ID: me, Gamertag: d.tags[me]}, Level: 12, Presence: &tl.PresenceOnline{}}
	m.SetClub(d.club)
	l := &tl.FriendsList{Me: m, Friends: []tl.Contact{}, Incoming: []tl.Contact{}, Outgoing: []tl.Contact{}}
	for _, f := range d.friends {
		c := tl.Contact{User: tl.User{ID: f.id, Gamertag: f.tag}, Level: 30, Presence: presence(f.state)}
		if d.incoming[f.id] {
			l.Incoming = append(l.Incoming, c)
			continue
		}
		c.SetClub(d.club)
		l.Friends = append(l.Friends, c)
	}
	return l, nil
}

func (d *demo) friend(tag string) *demoFriend {
	for _, f := range d.friends {
		if strings.EqualFold(f.tag, tag) {
			return f
		}
	}
	return nil
}

func (d *demo) AddFriend(me int64, q string) error {
	return auth.NewError(404, "player_not_found", "No player has that gamertag or ID.")
}

func (d *demo) Respond(me, other int64, accept bool) error {
	d.mu.Lock()
	delete(d.incoming, other)
	d.mu.Unlock()
	d.s.PushShort([]int64{me}, &tl.UpdateFriends{})
	return nil
}

func (d *demo) RemoveFriend(me, other int64) error {
	return auth.NewError(403, "demo", "the demo keeps its friends")
}

func (d *demo) ClubHistory(me, club, after int64, limit int) (*tl.ClubsHistory, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	h := &tl.ClubsHistory{Messages: []tl.ClubMessage{}, ReadMaxID: d.reads[me]}
	for _, m := range d.posts {
		if m.ID > after {
			h.Messages = append(h.Messages, m)
		}
	}
	return h, nil
}

func (d *demo) post(sender tl.User, text string) tl.ClubMessage {
	d.mu.Lock()
	m := tl.ClubMessage{ID: int64(len(d.posts) + 1), ClubID: d.club.ID, Sender: sender, Text: text, Date: int(time.Now().Unix())}
	d.posts = append(d.posts, m)
	d.mu.Unlock()
	return m
}

func (d *demo) SendClub(me, club int64, text string) (*tl.ClubMessage, error) {
	if club != d.club.ID {
		return nil, auth.NewError(403, "chat_unavailable", "Club chat requires current membership.")
	}
	text = strings.TrimSpace(text)
	if text == "" || len([]rune(text)) > 2000 {
		return nil, auth.NewError(400, "bad_message", "Messages must contain 1–2000 characters.")
	}
	d.mu.Lock()
	tag := d.tags[me]
	d.mu.Unlock()
	m := d.post(tl.User{ID: me, Gamertag: tag}, text)
	var others []int64
	for _, x := range d.s.SignedIn() {
		if x != me {
			others = append(others, x)
		}
	}
	d.s.PushShort(others, &tl.UpdateClubMessage{Message: m})
	return &m, nil
}

func (d *demo) ReadClub(me, club, maxID int64) error {
	d.mu.Lock()
	d.reads[me] = max(d.reads[me], maxID)
	d.mu.Unlock()
	return nil
}

func (d *demo) Gamerpic(user int64) (*tl.UsersGamerpic, error) {
	return &tl.UsersGamerpic{UserID: user, Version: 1, Png: social.Identicon(user, 96)}, nil
}

// command runs one stdin line; false = not a demo command.
func (d *demo) command(line string) (string, bool) {
	f := strings.SplitN(strings.TrimSpace(line), " ", 3)
	if len(f) < 2 {
		return "", false
	}
	fr := d.friend(f[1])
	switch f[0] {
	case "say":
		if fr == nil || len(f) < 3 {
			return "usage: say <friend> <text>", true
		}
		m := d.post(tl.User{ID: fr.id, Gamertag: fr.tag}, f[2])
		d.s.PushShort(d.s.SignedIn(), &tl.UpdateClubMessage{Message: m})
		return fmt.Sprintf("%s said it in the club", fr.tag), true
	case "presence":
		if fr == nil || len(f) < 3 {
			return "usage: presence <friend> offline|online|freeroam|roadtrip", true
		}
		d.mu.Lock()
		fr.state = f[2]
		d.mu.Unlock()
		d.s.PushShort(d.s.SignedIn(), &tl.UpdateUserPresence{UserID: fr.id, Presence: presence(f[2])})
		return fmt.Sprintf("%s is %s", fr.tag, f[2]), true
	case "request":
		if fr == nil {
			return "usage: request <friend>", true
		}
		d.mu.Lock()
		d.incoming[fr.id] = true
		d.mu.Unlock()
		d.s.PushShort(d.s.SignedIn(), &tl.UpdateFriends{})
		return fmt.Sprintf("%s sent a friend request", fr.tag), true
	}
	return "", false
}
