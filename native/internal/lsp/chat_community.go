package lsp

import (
	"fmt"
	"strconv"
	"sync"
	"time"

	"fh2mp/internal/auth"
	"fh2mp/internal/diag"
	"fh2mp/internal/mt/tl"
	"fh2mp/internal/pyjson"
)

// chatCommunity is mt.Community on the launcher API's own functions: the chat core sees the friends, requests, presence and club chat the
// launcher shows, under the same rules (friendship, club membership, message rules, rate of requests).
type chatCommunity struct{ l *Launcher }

func num(v any) int64 {
	switch x := v.(type) {
	case int64:
		return x
	case int:
		return int64(x)
	case float64:
		return int64(x)
	case string:
		n, _ := strconv.ParseInt(x, 10, 64)
		return n
	}
	return 0
}

var clubRoles = map[int64]string{0: "member", 1: "coleader", 2: "leader"}

// contactOf turns a launcher card into the chat's Contact.
func contactOf(cd card) tl.Contact {
	ct := tl.Contact{User: tl.User{ID: num(objAny(cd, "xuid")), Gamertag: objStr(cd, "gamertag")}, Level: int(num(objAny(cd, "level"))),
		GamerpicVersion: num(objAny(cd, "gamerpicVersion"))}
	switch objStr(cd, "state") {
	case "playing":
		kind := ""
		if lo, ok := objAny(cd, "lobby").(pyjson.Obj); ok {
			if k, ok := objAny(lo, "kind").(string); ok {
				kind = k
			}
		}
		ct.Presence = &tl.PresencePlaying{LobbyKind: kind}
	case "online":
		ct.Presence = &tl.PresenceOnline{}
	default:
		ct.Presence = &tl.PresenceOffline{}
	}
	if cl, ok := objAny(cd, "club").(pyjson.Obj); ok {
		role, known := clubRoles[num(objAny(cl, "role"))]
		if !known {
			role = fmt.Sprint(objAny(cl, "role"))
		}
		ct.SetClub(tl.Club{ID: num(objAny(cl, "id")), Tag: objStr(cl, "tag"), Name: objStr(cl, "name"), Role: role, Members: int(num(objAny(cl, "members")))})
	}
	return ct
}

func (l *Launcher) lobbyList() []any {
	if r := l.hubLobbies(); r != nil {
		lv, _ := r.Get("lobbies")
		return anyList(lv)
	}
	return nil
}

func (c chatCommunity) Friends(me int64) (*tl.FriendsList, error) {
	l := c.l
	people, err := l.C.Social.Friends(me)
	if err != nil {
		return nil, err
	}
	reqs, err := l.C.DB.Query("SELECT sender, recipient FROM friend_requests WHERE sender=? OR recipient=? ORDER BY created", me, me)
	if err != nil {
		return nil, err
	}
	all := append([]int64{me}, people...)
	for _, r := range reqs {
		all = append(all, r.IntAt(0), r.IntAt(1))
	}
	cards, err := l.profiles(all, l.lobbyList())
	if err != nil {
		return nil, err
	}
	out := &tl.FriendsList{Me: contactOf(cards[me]), Friends: []tl.Contact{}, Incoming: []tl.Contact{}, Outgoing: []tl.Contact{}}
	for _, f := range people {
		out.Friends = append(out.Friends, contactOf(cards[f]))
	}
	for _, r := range reqs {
		if r.IntAt(1) == me {
			out.Incoming = append(out.Incoming, contactOf(cards[r.IntAt(0)]))
		} else {
			out.Outgoing = append(out.Outgoing, contactOf(cards[r.IntAt(1)]))
		}
	}
	return out, nil
}

func (c chatCommunity) AddFriend(me int64, query string) error {
	_, err := c.l.request(me, query)
	return err
}

func (c chatCommunity) Respond(me, other int64, accept bool) error {
	_, err := c.l.respond(me, other, accept)
	return err
}

func (c chatCommunity) RemoveFriend(me, other int64) error {
	_, err := c.l.remove(me, other)
	return err
}

// myClub checks that club is the caller's club.
func (c chatCommunity) myClub(me, club int64) error {
	row, err := c.l.C.DB.One("SELECT club_id FROM club_members WHERE xuid=?", me)
	if err != nil {
		return err
	}
	if row == nil || row.IntAt(0) != club {
		why := "not in a club"
		if row != nil {
			why = fmt.Sprintf("in club %d, not %d", row.IntAt(0), club)
		}
		return auth.NewError(403, "chat_unavailable", "Club chat requires current membership.").WithXuid(me).WithDetail(why)
	}
	return nil
}

func clubMessage(club int64, r pyjson.Obj) tl.ClubMessage {
	return tl.ClubMessage{ID: num(objAny(r, "id")), ClubID: club, Sender: tl.User{ID: num(objAny(r, "sender")), Gamertag: objStr(r, "gamertag")},
		Text: objStr(r, "body"), Date: int(num(objAny(r, "created")))}
}

func (c chatCommunity) ClubHistory(me, club, after int64, limit int) (*tl.ClubsHistory, error) {
	if err := c.myClub(me, club); err != nil {
		return nil, err
	}
	res, err := c.l.messages(me, "club", after)
	if err != nil {
		return nil, err
	}
	rows := anyList(objAny(res.(pyjson.Obj), "messages"))
	// messages pages 100 at a time: the newest page when after = 0, else forwards from after.
	if len(rows) > limit {
		if after == 0 {
			rows = rows[len(rows)-limit:]
		} else {
			rows = rows[:limit]
		}
	}
	out := &tl.ClubsHistory{Messages: make([]tl.ClubMessage, 0, len(rows))}
	for _, r := range rows {
		out.Messages = append(out.Messages, clubMessage(club, r.(pyjson.Obj)))
	}
	read, err := c.l.C.DB.One("SELECT last_id FROM launcher_chat_reads WHERE xuid=? AND channel=?", me, fmt.Sprintf("club:%d", club))
	if err != nil {
		return nil, err
	}
	if read != nil {
		out.ReadMaxID = read.IntAt(0)
	}
	return out, nil
}

func (c chatCommunity) SendClub(me, club int64, text string) (*tl.ClubMessage, error) {
	if err := c.myClub(me, club); err != nil {
		return nil, err
	}
	t, err := checkMessage(me, text)
	if err != nil {
		return nil, err
	}
	p, err := c.l.post(me, "club", t, me)
	if err != nil {
		return nil, err
	}
	m := tl.ClubMessage{ID: p.id, ClubID: club, Sender: tl.User{ID: me, Gamertag: c.l.C.Tag(me)}, Text: p.text, Date: int(p.created)}
	return &m, nil
}

func (c chatCommunity) ReadClub(me, club, maxID int64) error {
	if err := c.myClub(me, club); err != nil {
		return err
	}
	_, err := c.l.readMark(me, "club", maxID)
	return err
}

func (c chatCommunity) Gamerpic(user int64) (*tl.UsersGamerpic, error) {
	png, err := c.l.C.Social.Gamerpic(user)
	if err != nil {
		return nil, err
	}
	var version int64
	if r, err := c.l.C.DB.One("SELECT COALESCE(gamerpic_at, 0) FROM player_profiles WHERE xuid=?", user); err != nil {
		return nil, err
	} else if r != nil {
		version = num(r.At(0))
	}
	return &tl.UsersGamerpic{UserID: user, Version: version, Png: png}, nil
}

// pushClubMessage hands a stored club message to the members' chat sessions (skip = the sender that gets it as its answer).
func (l *Launcher) pushClubMessage(me int64, p *posted, skip int64) {
	var club int64
	if _, err := fmt.Sscanf(p.channel, "club:%d", &club); err != nil {
		return
	}
	rows, err := l.C.DB.Query("SELECT xuid FROM club_members WHERE club_id=?", club)
	if err != nil {
		communityLog.Exc(fmt.Sprintf("club message %d stored but not pushed to the chat: members of club %d not read", p.id, club), err, diag.Warn)
		return
	}
	to := make([]int64, 0, len(rows))
	for _, r := range rows {
		if x := r.IntAt(0); x != skip {
			to = append(to, x)
		}
	}
	l.C.Chat.PushShort(to, &tl.UpdateClubMessage{Message: tl.ClubMessage{ID: p.id, ClubID: club, Sender: tl.User{ID: me, Gamertag: l.C.Tag(me)},
		Text: p.text, Date: int(p.created)}})
}

// friendsChanged tells both players' chat sessions that their friend list or requests changed.
func (l *Launcher) friendsChanged(a, b int64) {
	if l.C.Chat != nil {
		l.C.Chat.PushShort([]int64{a, b}, &tl.UpdateFriends{})
	}
}

// presenceWatch pushes friends' presence changes to signed-in chat sessions. Every tick it looks at the friends of everyone signed in to
// the chat; a player whose state changed since the last tick is pushed to those of their friends who are signed in. The first sight of a
// player is not pushed (friends.get carries it).
type presenceWatch struct {
	l       *Launcher
	mu      sync.Mutex
	last    map[int64]string
	friends map[int64]friendsAt
}

type friendsAt struct {
	list []int64
	at   time.Time
}

func (w *presenceWatch) friendsOf(x int64, now time.Time) []int64 {
	if f, ok := w.friends[x]; ok && now.Sub(f.at) < time.Minute {
		return f.list
	}
	list, err := w.l.C.Social.Friends(x)
	if err != nil {
		return w.friends[x].list
	}
	w.friends[x] = friendsAt{list, now}
	return list
}

func presenceOf(state string, lobby pyjson.Obj) tl.PresenceClass {
	switch state {
	case "playing":
		kind := ""
		if lobby != nil {
			kind, _ = objAny(lobby, "kind").(string)
		}
		return &tl.PresencePlaying{LobbyKind: kind}
	case "online":
		return &tl.PresenceOnline{}
	}
	return &tl.PresenceOffline{}
}

func (w *presenceWatch) tick(now time.Time) {
	chat := w.l.C.Chat
	online := chat.SignedIn()
	w.mu.Lock()
	defer w.mu.Unlock()
	watchers := map[int64][]int64{} // player -> signed-in friends who see them
	for _, s := range online {
		for _, f := range w.friendsOf(s, now) {
			watchers[f] = append(watchers[f], s)
		}
	}
	for x := range w.friends {
		if _, still := watchers[x]; !still && !chat.Online(x) {
			delete(w.friends, x)
		}
	}
	lobbies := w.l.lobbyList()
	for x, to := range watchers {
		state, lobby := w.l.presence(x, lobbies)
		p := presenceOf(state, lobby)
		key := state
		if pp, ok := p.(*tl.PresencePlaying); ok {
			key += ":" + pp.LobbyKind
		}
		prev, seen := w.last[x]
		w.last[x] = key
		if seen && prev != key {
			chat.PushShort(to, &tl.UpdateUserPresence{UserID: x, Presence: p})
		}
	}
	for x := range w.last {
		if _, still := watchers[x]; !still {
			delete(w.last, x)
		}
	}
}

func (w *presenceWatch) run(every time.Duration) {
	for range time.Tick(every) {
		w.tick(time.Now())
	}
}
