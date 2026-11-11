package mt

import (
	"context"
	"errors"
	"strings"
	"sync"
	"time"

	"fh2mp/internal/auth"
	"fh2mp/internal/mt/tl"
)

// Community is the friends, presence and club data behind the chat (the LSP implements it on the launcher API's own functions, so the
// HTTP and the chat paths keep one rule set). Errors are *auth.Error for what the caller did wrong.
type Community interface {
	Friends(me int64) (*tl.FriendsList, error)
	AddFriend(me int64, query string) error
	Respond(me, other int64, accept bool) error
	RemoveFriend(me, other int64) error
	ClubHistory(me, club, after int64, limit int) (*tl.ClubsHistory, error)
	// SendClub posts in the caller's club; the message reaches the other members through PushShort from the implementation.
	SendClub(me, club int64, text string) (*tl.ClubMessage, error)
	ReadClub(me, club, maxID int64) error
	Gamerpic(user int64) (*tl.UsersGamerpic, error)
}

// SetCommunity plugs the friends and club API in; without it those methods answer 501 NOT_IMPLEMENTED.
func (s *Server) SetCommunity(c Community) { s.community = c }

// sentClub remembers clubs.sendMessage answers by (account, random_id) for an hour, so a resend after a lost answer posts once.
type sentClub struct {
	mu   sync.Mutex
	byID map[[2]int64]sentEntry
}

type sentEntry struct {
	msg *tl.ClubMessage
	at  time.Time
}

func (c *sentClub) get(x, rid int64, now time.Time) *tl.ClubMessage {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.byID == nil {
		c.byID = map[[2]int64]sentEntry{}
	}
	for k, e := range c.byID {
		if now.Sub(e.at) > time.Hour {
			delete(c.byID, k)
		}
	}
	if e, ok := c.byID[[2]int64{x, rid}]; ok {
		return e.msg
	}
	return nil
}

func (c *sentClub) put(x, rid int64, m *tl.ClubMessage, now time.Time) {
	c.mu.Lock()
	c.byID[[2]int64{x, rid}] = sentEntry{msg: m, at: now}
	c.mu.Unlock()
}

// communityRPC maps a Community error to an rpc_error: auth.Error codes become UPPER_SNAKE messages with their status, the rest is 500.
func (s *Server) communityRPC(c *Call, method string, err error) *RPCError {
	var ae *auth.Error
	if !errors.As(err, &ae) {
		return s.internal(c, method, err)
	}
	code := ae.Status
	if code == 429 {
		code = 420
	}
	return &RPCError{Code: code, Message: strings.ToUpper(ae.Code), detail: firstNonEmpty(ae.Detail, ae.Message)}
}

var errNoCommunity = &RPCError{Code: 501, Message: "NOT_IMPLEMENTED"}

func (s *Server) registerCommunity() {
	d := s.disp
	need := func(ctx context.Context) (*Call, Community, error) {
		c := callOf(ctx)
		if s.community == nil {
			return c, nil, errNoCommunity
		}
		return c, s.community, nil
	}
	ok := func(c *Call, method string, err error) (bool, error) {
		if err != nil {
			return false, s.communityRPC(c, method, err)
		}
		return true, nil
	}
	d.OnFriendsGet(func(ctx context.Context) (*tl.FriendsList, error) {
		c, cm, err := need(ctx)
		if err != nil {
			return nil, err
		}
		l, err := cm.Friends(c.xuid())
		if err != nil {
			return nil, s.communityRPC(c, "friends.get", err)
		}
		return l, nil
	})
	d.OnFriendsAdd(func(ctx context.Context, query string) (bool, error) {
		c, cm, err := need(ctx)
		if err != nil {
			return false, err
		}
		return ok(c, "friends.add", cm.AddFriend(c.xuid(), clip(query, 64)))
	})
	d.OnFriendsAccept(func(ctx context.Context, user int64) (bool, error) {
		c, cm, err := need(ctx)
		if err != nil {
			return false, err
		}
		return ok(c, "friends.accept", cm.Respond(c.xuid(), user, true))
	})
	d.OnFriendsDecline(func(ctx context.Context, user int64) (bool, error) {
		c, cm, err := need(ctx)
		if err != nil {
			return false, err
		}
		return ok(c, "friends.decline", cm.Respond(c.xuid(), user, false))
	})
	d.OnFriendsRemove(func(ctx context.Context, user int64) (bool, error) {
		c, cm, err := need(ctx)
		if err != nil {
			return false, err
		}
		return ok(c, "friends.remove", cm.RemoveFriend(c.xuid(), user))
	})
	d.OnClubsGetHistory(func(ctx context.Context, r *tl.ClubsGetHistoryRequest) (*tl.ClubsHistory, error) {
		c, cm, err := need(ctx)
		if err != nil {
			return nil, err
		}
		limit := r.Limit
		if limit <= 0 || limit > 100 {
			limit = 100
		}
		h, err := cm.ClubHistory(c.xuid(), r.ClubID, max(0, r.AfterID), limit)
		if err != nil {
			return nil, s.communityRPC(c, "clubs.getHistory", err)
		}
		return h, nil
	})
	d.OnClubsSendMessage(func(ctx context.Context, r *tl.ClubsSendMessageRequest) (*tl.ClubMessage, error) {
		c, cm, err := need(ctx)
		if err != nil {
			return nil, err
		}
		x, now := c.xuid(), s.cfg.Now()
		if m := s.sent.get(x, r.RandomID, now); m != nil {
			return m, nil
		}
		m, err := cm.SendClub(x, r.ClubID, r.Text)
		if err != nil {
			return nil, s.communityRPC(c, "clubs.sendMessage", err)
		}
		s.sent.put(x, r.RandomID, m, now)
		return m, nil
	})
	d.OnClubsReadHistory(func(ctx context.Context, r *tl.ClubsReadHistoryRequest) (bool, error) {
		c, cm, err := need(ctx)
		if err != nil {
			return false, err
		}
		return ok(c, "clubs.readHistory", cm.ReadClub(c.xuid(), r.ClubID, r.MaxID))
	})
	d.OnUsersGetGamerpic(func(ctx context.Context, user int64) (*tl.UsersGamerpic, error) {
		c, cm, err := need(ctx)
		if err != nil {
			return nil, err
		}
		g, err := cm.Gamerpic(user)
		if err != nil {
			return nil, s.communityRPC(c, "users.getGamerpic", err)
		}
		return g, nil
	})
}

// PushShort sends one update without pts (updateShort) to the signed-in sessions of the accounts.
func (s *Server) PushShort(xuids []int64, u tl.UpdateClass) {
	msg := &tl.UpdateShort{Update: u, Date: int(s.cfg.Now().Unix())}
	for _, x := range xuids {
		s.mu.Lock()
		k := s.byXuid[x]
		s.mu.Unlock()
		if k == nil {
			continue
		}
		k.mu.Lock()
		sessions := make([]*session, 0, len(k.sessions))
		for _, ss := range k.sessions {
			sessions = append(sessions, ss)
		}
		k.mu.Unlock()
		for _, ss := range sessions {
			ss.mu.Lock()
			live := ss.layer > 0 && ss.conn != nil && !ss.conn.closed()
			ss.mu.Unlock()
			if live {
				s.send(ss, outMsg{obj: msg, content: true})
			}
		}
	}
}

// Online says whether the account has a signed-in chat session with a live connection.
func (s *Server) Online(xuid int64) bool {
	s.mu.Lock()
	k := s.byXuid[xuid]
	s.mu.Unlock()
	if k == nil {
		return false
	}
	k.mu.Lock()
	defer k.mu.Unlock()
	for _, ss := range k.sessions {
		ss.mu.Lock()
		live := ss.conn != nil && !ss.conn.closed()
		ss.mu.Unlock()
		if live {
			return true
		}
	}
	return false
}

// SignedIn lists the accounts with a chat session.
func (s *Server) SignedIn() []int64 {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := make([]int64, 0, len(s.byXuid))
	for x := range s.byXuid {
		out = append(out, x)
	}
	return out
}

// presenceName is a Presence for log lines.
func presenceName(p tl.PresenceClass) string {
	switch v := p.(type) {
	case *tl.PresencePlaying:
		if v.LobbyKind != "" {
			return "playing:" + v.LobbyKind
		}
		return "playing"
	case *tl.PresenceOnline:
		return "online"
	default:
		return "offline"
	}
}
