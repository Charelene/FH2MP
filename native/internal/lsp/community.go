package lsp

import (
	"fmt"
	"math"
	"net/url"
	"slices"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
	"unicode"
	"unicode/utf8"

	"fh2mp/internal/auth"
	"fh2mp/internal/clubs"
	"fh2mp/internal/db"
	"fh2mp/internal/diag"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/social"
	"fh2mp/internal/xbl"
)

// Authenticated launcher friends, presence, profiles, conversations and lobby leases (/v1/community/*).

const (
	messageKeep     = 7 * 86400 // chat messages older than a week are deleted
	hubCacheSeconds = 2.5
)

var communityLog = diag.Get("community")

var inviteErrors = map[string]string{ // club rule violations, in the launcher's words
	"not_in_club": "You are not in a club.", "no_privilege": "Only the club leader and co-leaders can invite players.",
	"no_invites": "That player does not accept club invites.", "in_club": "That player is already in a club.",
	"already_invited": "That player is already invited.", "club_full": "The club is full.",
}

type communityState struct {
	mu       sync.Mutex
	seen     map[int64]float64 // launcher presence, independent of the game's presence poll
	pruneAt  float64
	hubAt    time.Time
	hubOK    bool
	hubValue pyjson.Obj
	rate     *diag.RateLimit
	invites  *inviteBook
}

func (l *Launcher) cstate() *communityState {
	l.boardsMu.Lock()
	defer l.boardsMu.Unlock()
	if l.comm == nil {
		l.comm = &communityState{seen: map[int64]float64{}, rate: diag.NewRateLimit(nil, 0), invites: newInviteBook()}
	}
	return l.comm
}

func nowF() float64 { return float64(time.Now().UnixNano()) / 1e9 }

// pyInt is int(text, base) for base 10 or 16: surrounding whitespace, a sign, an 0x prefix in base 16, underscores between digits.
func pyInt(text string, base int) (int64, bool) {
	t := strings.TrimSpace(text)
	neg := false
	if t != "" && (t[0] == '+' || t[0] == '-') {
		neg, t = t[0] == '-', t[1:]
	}
	if base == 16 && len(t) > 1 && (t[:2] == "0x" || t[:2] == "0X") {
		t = t[2:]
	}
	if t == "" || t[0] == '_' || t[len(t)-1] == '_' || strings.Contains(t, "__") {
		return 0, false
	}
	n, err := strconv.ParseInt(strings.ReplaceAll(t, "_", ""), base, 64)
	if err != nil {
		if ne, ok := err.(*strconv.NumError); ok && ne.Err == strconv.ErrRange {
			return 1 << 62, true // a number, too big for any xuid
		}
		return 0, false
	}
	if neg {
		n = -n
	}
	return n, true
}

// pyStrip is str.strip(): Unicode white space and the separators \x1c-\x1f.
func pyStrip(s string) string {
	return strings.TrimFunc(s, func(r rune) bool { return unicode.IsSpace(r) || (r >= 0x1c && r <= 0x1f) })
}

func (l *Launcher) resolve(value string) (int64, error) {
	value = pyStrip(value)
	base := 10
	if strings.HasPrefix(strings.ToLower(value), "0x") {
		base = 16
	}
	xuid, parsed := pyInt(value, base)
	inRange := parsed && xuid > 0
	var row *db.Row
	var err error
	if inRange {
		row, err = l.C.DB.One("SELECT xuid FROM accounts WHERE xuid=? AND (banned_at IS NULL OR ban_until <= ?)", xuid, nowF())
	} else {
		row, err = l.C.DB.One("SELECT xuid FROM accounts WHERE gamertag_key=? AND (banned_at IS NULL OR ban_until <= ?)", auth.FoldTag(value), nowF())
	}
	if err != nil {
		return 0, err
	}
	if row == nil {
		as := "as a gamertag"
		if inRange {
			as = "as an id"
		}
		return 0, auth.NewError(404, "player_not_found", "No player has that gamertag or ID.").WithDetail(fmt.Sprintf("no unbanned account matches %s %s", pyjson.PyRepr(cut(value, 40)), as))
	}
	return row.IntAt(0), nil
}

// card is one player's card as the launcher shows it.
type card = pyjson.Obj

func (l *Launcher) profile(xuid int64, lobbies []any) (card, error) {
	m, err := l.profiles([]int64{xuid}, lobbies)
	if err != nil {
		return nil, err
	}
	return m[xuid], nil
}

func marks(n int) string { return strings.TrimSuffix(strings.Repeat("?,", n), ",") }

func int64Args(xs []int64) []any {
	out := make([]any, len(xs))
	for i, x := range xs {
		out[i] = x
	}
	return out
}

func dedupe(xs []int64) []int64 {
	seen := map[int64]bool{}
	var out []int64
	for _, x := range xs {
		if !seen[x] {
			seen[x] = true
			out = append(out, x)
		}
	}
	return out
}

// profiles are the cards of many players in three queries.
func (l *Launcher) profiles(xuids []int64, lobbies []any) (map[int64]card, error) {
	xuids = dedupe(xuids)
	out := map[int64]card{}
	if len(xuids) == 0 {
		return out, nil
	}
	in := marks(len(xuids))
	rows, err := l.C.DB.Query("SELECT a.xuid, a.gamertag, p.career_level, COALESCE(g.gamerpic_at,0) AS gamerpic_version, "+
		"GREATEST(a.last_login, COALESCE(p.last_seen,0)) AS last_seen, COALESCE(g.motto,'') AS motto, COALESCE(g.real_name,'') AS real_name, "+
		"COALESCE(g.location,'') AS location, COALESCE(g.bio,'') AS bio, COALESCE(g.theme,'') AS theme FROM accounts a LEFT JOIN players p ON p.xuid=a.xuid "+
		"LEFT JOIN player_profiles g ON g.xuid=a.xuid WHERE a.xuid IN ("+in+")", int64Args(xuids)...)
	if err != nil {
		return nil, err
	}
	byXuid := map[int64]*db.Row{}
	for i := range rows {
		byXuid[rows[i].IntAt(0)] = &rows[i]
	}
	crows, err := l.C.DB.Query("SELECT m.xuid, c.id, c.tag, c.name, m.role, (SELECT COUNT(*) FROM club_members n WHERE n.club_id=c.id) AS members FROM club_members m "+
		"JOIN clubs c ON c.id=m.club_id WHERE m.xuid IN ("+in+")", int64Args(xuids)...)
	if err != nil {
		return nil, err
	}
	clubOf := map[int64]*db.Row{}
	for i := range crows {
		clubOf[crows[i].Int("xuid")] = &crows[i]
	}
	scores, err := l.gamerscores(xuids)
	if err != nil {
		return nil, err
	}
	for _, x := range xuids {
		c, err := l.card(x, byXuid[x], clubOf[x], lobbies)
		if err != nil {
			return nil, err
		}
		if g, reported := scores[x]; reported {
			c = append(c, pyjson.Pair{K: "gamerscore", V: g}) // from the achievements their Guide reported
		}
		out[x] = c
	}
	return out, nil
}

func (l *Launcher) card(xuid int64, row, club *db.Row, lobbies []any) (card, error) {
	var tag string
	var level int64
	var version any = int64(0)
	if row == nil {
		// Dev/optional-auth worlds may contain a legitimate game player without a launcher account. They are visible but cannot be added until they register.
		prof, err := l.C.Social.Profile(xuid)
		if err != nil {
			return nil, err
		}
		tag = prof.Gamertag
	} else {
		tag, level, version = row.Str("gamertag"), row.Int("career_level"), row.Any("gamerpic_version")
	}
	state, lobby := l.presence(xuid, lobbies)
	var clubV any
	if club != nil {
		clubV = pyjson.Obj{{K: "id", V: club.Any("id")}, {K: "tag", V: club.Any("tag")}, {K: "name", V: club.Any("name")}, {K: "role", V: club.Any("role")}, {K: "members", V: club.Any("members")}}
	}
	var lobbyV any
	if lobby != nil {
		o := pyjson.Obj{}
		for _, p := range lobby {
			if p.K != "memberXuids" {
				o = append(o, p)
			}
		}
		lobbyV = o
	}
	out := pyjson.Obj{{K: "xuid", V: xuid}, {K: "gamertag", V: tag}, {K: "level", V: db.DisplayLevel(level)}, {K: "gamerpicVersion", V: version}, {K: "state", V: state},
		{K: "club", V: clubV}, {K: "lobby", V: lobbyV}}
	if row != nil {
		// the Guide's profile: the four texts, and for a player who is offline how long ago they were last here
		out = append(out, pyjson.Pair{K: "theme", V: row.Str("theme")})
		out = append(out, pyjson.Pair{K: "motto", V: row.Str("motto")}, pyjson.Pair{K: "name", V: row.Str("real_name")},
			pyjson.Pair{K: "location", V: row.Str("location")}, pyjson.Pair{K: "bio", V: row.Str("bio")})
		if last := row.Float("last_seen"); state == "offline" && last > 0 {
			out = append(out, pyjson.Pair{K: "lastSeenAgo", V: int64(math.Max(0, nowF()-last))})
		}
	}
	return out, nil
}

// presence is what a player is doing as friends see it: playing (in a hub lobby, or a game polled lately), online (the launcher polled in the
// last 40 s, or the chat core is signed in) or offline; lobby is the hub lobby the player is in (nil = none).
func (l *Launcher) presence(xuid int64, lobbies []any) (state string, lobby pyjson.Obj) {
	hex := fmt.Sprintf("%x", xuid)
	for _, lv := range lobbies {
		lo, _ := lv.(pyjson.Obj)
		mv, _ := lo.Get("memberXuids")
		for _, m := range anyList(mv) {
			if s, _ := m.(string); s == hex {
				lobby = lo
				break
			}
		}
		if lobby != nil {
			break
		}
	}
	cs := l.cstate()
	cs.mu.Lock()
	seen := cs.seen[xuid]
	cs.mu.Unlock()
	switch {
	case lobby != nil || social.Online(xuid):
		return "playing", lobby
	case nowF()-seen < 40 || (l.C.Chat != nil && l.C.Chat.Online(xuid)):
		return "online", lobby
	}
	return "offline", lobby
}

// hubLobbies is the hub's answer, shared for a moment: a launcher open means one UDP query every 5 s, up to 0.3 s each.
func (l *Launcher) hubLobbies() pyjson.Obj {
	cs := l.cstate()
	cs.mu.Lock()
	defer cs.mu.Unlock()
	if cs.hubOK && time.Since(cs.hubAt).Seconds() < hubCacheSeconds {
		return cs.hubValue
	}
	r := l.C.Xbl.HubLobbies()
	cs.hubAt, cs.hubOK, cs.hubValue = time.Now(), true, r
	return r
}

func publicHold(h *xbl.Hold) any {
	if h == nil {
		return nil
	}
	out := pyjson.Obj{{K: "id", V: h.ID}, {K: "kind", V: h.Kind}, {K: "expiresAt", V: h.ExpiresAt}}
	if h.Key != nil {
		// the lobby as the game joins it: its session in the directory (the Guide hands this to the game's own session-ready flow)
		out = append(out, pyjson.Pair{K: "session", V: pyjson.Obj{{K: "scid", V: h.Key.Scid}, {K: "templateName", V: xbl.TargetTemplate}, {K: "name", V: h.ID}}})
	}
	return out
}

// seatFor is the answer to a seat held to join another player (an accepted invite, Join Session In Progress): the hold, with the session the
// other player is in (for the host of a private Free Roam that is the game's own session, not the lobby the world is named after), and
// the game server told that the joiner belongs in that lobby's world (a session the game made itself is not announced when it is joined).
func (l *Launcher) seatFor(me int64, h *xbl.Hold, host int64) any {
	out, _ := publicHold(h).(pyjson.Obj)
	if h == nil {
		return nil
	}
	if scid, tmpl, name, ok := l.C.Xbl.SessionOf(fmt.Sprint(host)); ok {
		ses := pyjson.Obj{{K: "scid", V: scid}, {K: "templateName", V: tmpl}, {K: "name", V: name}}
		replaced := false
		for i := range out {
			if out[i].K == "session" {
				out[i].V, replaced = ses, true
			}
		}
		if !replaced {
			out = append(out, pyjson.Pair{K: "session", V: ses})
		}
		reserved := l.C.Xbl.ReserveMember(scid, tmpl, name, fmt.Sprint(me))
		communityLog.Info(fmt.Sprintf("seat to join a player: joiner=%#x host=%#x lobby=%.8s session=%s/%.8s (the joiner is a reserved member there: %s), the game server is told the joiner's world",
			me, host, h.ID, tmpl, name, pyBool(reserved)))
	} else {
		reserved := h.Key != nil && l.C.Xbl.ReserveMember(h.Key.Scid, xbl.TargetTemplate, h.ID, fmt.Sprint(me))
		communityLog.Info(fmt.Sprintf("seat to join a player: joiner=%#x host=%#x lobby=%.8s, the host is in no session of the directory: the lobby's own session is named (the joiner is a reserved member there: %s)",
			me, host, h.ID, pyBool(reserved)))
	}
	if scid, tmpl, name, ok := l.C.Xbl.SessionOf(fmt.Sprint(host)); ok && l.C.Xbl.PrivateWorld(scid, tmpl, name) != "" {
		l.C.Xbl.AnnounceFollower(fmt.Sprint(me), l.C.Xbl.PrivateWorld(scid, tmpl, name))
	} else {
		l.C.Xbl.AnnouncePlayer(fmt.Sprint(me), h.ID)
	}
	return out
}

// playersHere are the players in the caller's game right now, the caller left out (the Guide's Players tab): the other members of the
// game server world the caller is in, from the hub's lobby list (memberXuids, hex).
func playersHere(me int64, lobbies []any) []int64 {
	var here []int64
	meHex := fmt.Sprintf("%x", me)
	for _, lv := range lobbies {
		lo, _ := lv.(pyjson.Obj)
		mv, _ := lo.Get("memberXuids")
		members := anyList(mv)
		if !slices.Contains(members, any(meHex)) {
			continue
		}
		for _, m := range members {
			if s, _ := m.(string); s != meHex {
				if x, err := strconv.ParseInt(s, 16, 64); err == nil && x != 0 {
					here = append(here, x)
				}
			}
		}
		break
	}
	return here
}

func (l *Launcher) snapshot(me int64) (any, error) {
	result := l.hubLobbies()
	var lobbies []any
	if result != nil {
		lv, _ := result.Get("lobbies")
		lobbies = anyList(lv)
	}
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
		all = append(all, r.IntAt(0))
	}
	for _, r := range reqs {
		all = append(all, r.IntAt(1))
	}
	here := playersHere(me, lobbies)
	all = append(all, here...)
	cards, err := l.profiles(all, lobbies)
	if err != nil {
		return nil, err
	}
	players := []any{}
	for _, x := range here {
		players = append(players, cards[x])
	}
	friends, incoming, outgoing := []any{}, []any{}, []any{}
	for _, f := range people {
		friends = append(friends, cards[f])
	}
	for _, r := range reqs {
		if r.IntAt(1) == me {
			incoming = append(incoming, cards[r.IntAt(0)])
		}
	}
	for _, r := range reqs {
		if r.IntAt(0) == me {
			outgoing = append(outgoing, cards[r.IntAt(1)])
		}
	}
	chats, err := l.channels(me)
	if err != nil {
		return nil, err
	}
	var counts any
	if result != nil {
		cnt := pyjson.Obj{}
		for _, kind := range []string{"freeroam", "roadtrip"} {
			n := int64(0)
			for _, lv := range lobbies {
				lo, _ := lv.(pyjson.Obj)
				if k, _ := lo.Get("kind"); k == kind {
					n++
				}
			}
			cnt = append(cnt, pyjson.Pair{K: kind, V: n})
		}
		counts = cnt
	}
	return pyjson.Obj{{K: "friends", V: friends}, {K: "incoming", V: incoming}, {K: "outgoing", V: outgoing}, {K: "me", V: cards[me]},
		{K: "reservation", V: publicHold(l.C.Xbl.CurrentHold(fmt.Sprint(me)))}, {K: "chats", V: chats}, {K: "lobbyCounts", V: counts}, {K: "players", V: players}}, nil
}

func ok1() pyjson.Obj { return pyjson.Obj{{K: "ok", V: true}} }

// player is GET /v1/community/player/<xuid>: any player's card and what they are to the caller ("friend", "incoming": they asked the caller,
// "outgoing": the caller asked them, "none", or "self"). The Guide shows it when the game asks for a player's gamercard (a lobby's player
// list, a leaderboard row), where the player need not be a friend.
func (l *Launcher) player(me, target int64) (any, error) {
	row, err := l.C.DB.One("SELECT 1 FROM accounts WHERE xuid=? AND (banned_at IS NULL OR ban_until <= ?)", target, nowF())
	if err != nil {
		return nil, err
	}
	if row == nil {
		return nil, auth.NewError(404, "player_not_found", "No player has that gamertag or ID.").WithXuid(me).WithDetail(fmt.Sprintf("no unbanned account %#x", target))
	}
	var lobbies []any
	if r := l.hubLobbies(); r != nil {
		lv, _ := r.Get("lobbies")
		lobbies = anyList(lv)
	}
	c, err := l.profile(target, lobbies)
	if err != nil {
		return nil, err
	}
	relation := "none"
	if target == me {
		relation = "self"
	} else if r, err := l.C.DB.One("SELECT 1 FROM friendships WHERE a=? AND b=?", min(me, target), max(me, target)); err != nil {
		return nil, err
	} else if r != nil {
		relation = "friend"
	} else if r, err := l.C.DB.One("SELECT sender FROM friend_requests WHERE (sender=? AND recipient=?) OR (sender=? AND recipient=?)", target, me, me, target); err != nil {
		return nil, err
	} else if r != nil {
		relation = "outgoing"
		if r.IntAt(0) == target {
			relation = "incoming"
		}
	}
	return pyjson.Obj{{K: "player", V: c}, {K: "relation", V: relation}}, nil
}

func (l *Launcher) request(me int64, value string) (any, error) {
	target, err := l.resolve(value)
	if err != nil {
		return nil, err
	}
	if target == me {
		return nil, auth.NewError(400, "self_request", "That is your own account.")
	}
	err = l.C.DB.Do(func(t *db.Tx) error {
		a, b := min(me, target), max(me, target)
		r, err := t.One("SELECT 1 FROM friendships WHERE a=? AND b=?", a, b)
		if err != nil {
			return err
		}
		if r != nil {
			return auth.NewError(409, "already_friends", "You are already friends.").WithXuid(me).WithDetail(fmt.Sprintf("with %#x", target))
		}
		r, err = t.One("SELECT COUNT(*) FROM friend_requests WHERE sender=?", me)
		if err != nil {
			return err
		}
		if r.IntAt(0) >= 100 {
			return auth.NewError(409, "request_limit", "Cancel an outgoing request before sending another.").WithXuid(me).WithDetail("100 outgoing friend requests are pending")
		}
		_, err = t.Exec("INSERT INTO friend_requests(sender,recipient,created) VALUES (?,?,?) ON CONFLICT DO NOTHING", me, target, nowF())
		return err
	})
	if err != nil {
		return nil, err
	}
	l.friendsChanged(me, target)
	return ok1(), nil
}

func (l *Launcher) respond(me, target int64, accept bool) (any, error) {
	err := l.C.DB.Do(func(t *db.Tx) error {
		if accept {
			r, err := t.One("SELECT 1 FROM friend_requests WHERE sender=? AND recipient=?", target, me)
			if err != nil {
				return err
			}
			if r == nil {
				return auth.NewError(404, "request_gone", "This friend request is no longer available.").WithXuid(me).WithDetail(fmt.Sprintf("no request from %#x", target))
			}
			if _, err := t.Exec("INSERT INTO friendships(a,b,created) VALUES (?,?,?) ON CONFLICT DO NOTHING", min(me, target), max(me, target), nowF()); err != nil {
				return err
			}
		}
		_, err := t.Exec("DELETE FROM friend_requests WHERE (sender=? AND recipient=?) OR (sender=? AND recipient=?)", target, me, me, target)
		return err
	})
	if err != nil {
		return nil, err
	}
	l.friendsChanged(me, target)
	return ok1(), nil
}

func (l *Launcher) remove(me, target int64) (any, error) {
	err := l.C.DB.Do(func(t *db.Tx) error {
		if _, err := t.Exec("DELETE FROM friendships WHERE a=? AND b=?", min(me, target), max(me, target)); err != nil {
			return err
		}
		_, err := t.Exec("DELETE FROM friend_requests WHERE (sender=? AND recipient=?) OR (sender=? AND recipient=?)", target, me, me, target)
		return err
	})
	if err != nil {
		return nil, err
	}
	l.friendsChanged(me, target)
	return ok1(), nil
}

type oneFn func(sql string, args ...any) (*db.Row, error)

func isDigits(s string) bool {
	if s == "" {
		return false
	}
	for _, r := range s {
		if !unicode.IsDigit(r) {
			return false
		}
	}
	return true
}

// channel is the chat channel name for a reference ("club" or a friend's xuid), checked against membership and friendship.
func (l *Launcher) channel(me int64, value string, one oneFn) (string, error) {
	if one == nil {
		one = l.C.DB.One
	}
	if value == "club" {
		row, err := one("SELECT club_id FROM club_members WHERE xuid=?", me)
		if err != nil {
			return "", err
		}
		if row != nil {
			return fmt.Sprintf("club:%d", row.IntAt(0)), nil
		}
	} else if isDigits(value) {
		if other, err := strconv.ParseInt(value, 10, 64); err == nil && other > 0 {
			row, err := one("SELECT 1 FROM friendships WHERE a=? AND b=?", min(me, other), max(me, other))
			if err != nil {
				return "", err
			}
			if row != nil {
				return fmt.Sprintf("dm:%d:%d", min(me, other), max(me, other)), nil
			}
		}
	}
	why := "unknown channel"
	switch {
	case value == "club":
		why = "not in a club"
	case isDigits(value):
		why = "not friends"
	}
	return "", auth.NewError(403, "chat_unavailable", "Direct messages require friendship; club chat requires current membership.").WithXuid(me).
		WithDetail(fmt.Sprintf("channel %s: %s", pyjson.PyRepr(cut(value, 20)), why))
}

func (l *Launcher) channels(me int64) ([]any, error) {
	friends, err := l.C.Social.Friends(me)
	if err != nil {
		return nil, err
	}
	cards, err := l.profiles(friends, nil)
	if err != nil {
		return nil, err
	}
	type ref struct{ id, title string }
	var refs []ref
	for _, f := range friends {
		refs = append(refs, ref{fmt.Sprint(f), objStr(cards[f], "gamertag")})
	}
	club, err := l.C.DB.One("SELECT c.tag, c.name FROM clubs c JOIN club_members m ON m.club_id=c.id WHERE m.xuid=?", me)
	if err != nil {
		return nil, err
	}
	if club != nil {
		refs = append([]ref{{"club", fmt.Sprintf("[%s] %s", club.Str("tag"), club.Str("name"))}}, refs...)
	}
	if len(refs) == 0 {
		return []any{}, nil
	}
	named := map[string]string{}
	var channels []string
	for _, r := range refs {
		ch, err := l.channel(me, r.id, nil)
		if err != nil {
			return nil, err
		}
		if _, dup := named[r.id]; !dup {
			channels = append(channels, ch)
		}
		named[r.id] = ch
	}
	in := marks(len(channels))
	chArgs := make([]any, len(channels))
	for i, c := range channels {
		chArgs[i] = c
	}
	rows, err := l.C.DB.Query("SELECT channel, MAX(id) FROM launcher_messages WHERE channel IN ("+in+") AND created>? GROUP BY channel", append(append([]any{}, chArgs...), nowF()-messageKeep)...)
	if err != nil {
		return nil, err
	}
	lastIDs := map[string]int64{}
	var ids []any
	for _, r := range rows {
		lastIDs[r.StrAt(0)] = r.IntAt(1)
		ids = append(ids, r.IntAt(1))
	}
	bodies := map[int64]string{}
	if len(ids) > 0 {
		br, err := l.C.DB.Query("SELECT id, body FROM launcher_messages WHERE id IN ("+marks(len(ids))+")", ids...)
		if err != nil {
			return nil, err
		}
		for _, r := range br {
			bodies[r.IntAt(0)] = r.StrAt(1)
		}
	}
	rr, err := l.C.DB.Query("SELECT channel, last_id FROM launcher_chat_reads WHERE xuid=? AND channel IN ("+in+")", append([]any{me}, chArgs...)...)
	if err != nil {
		return nil, err
	}
	read := map[string]int64{}
	for _, r := range rr {
		read[r.StrAt(0)] = r.IntAt(1)
	}
	mr, err := l.C.DB.Query("SELECT channel, id FROM launcher_messages WHERE channel IN ("+in+") AND sender<>?", append(append([]any{}, chArgs...), me)...)
	if err != nil {
		return nil, err
	}
	incoming, unread := map[string]int64{}, map[string]int64{}
	for _, r := range mr {
		ch, id := r.StrAt(0), r.IntAt(1)
		incoming[ch] = max(incoming[ch], id)
		if id > read[ch] {
			unread[ch]++
		}
	}
	out := []any{}
	for _, r := range refs {
		ch := named[r.id]
		out = append(out, pyjson.Obj{{K: "id", V: r.id}, {K: "title", V: r.title}, {K: "last", V: bodies[lastIDs[ch]]}, {K: "unread", V: unread[ch]}, {K: "lastIncomingId", V: incoming[ch]}})
	}
	return out, nil
}

func (l *Launcher) messages(me int64, ref string, after int64) (any, error) {
	ch, err := l.channel(me, ref, nil)
	if err != nil {
		return nil, err
	}
	// On first open show the most recent page; then incrementally fetch forwards.
	dir := "DESC"
	if after != 0 {
		dir = "ASC"
	}
	rows, err := l.C.DB.Query("SELECT m.id,m.sender,a.gamertag,m.body,m.created FROM launcher_messages m JOIN accounts a ON a.xuid=m.sender WHERE m.channel=? AND m.id>? AND m.created>? ORDER BY m.id "+dir+" LIMIT 100",
		ch, after, nowF()-messageKeep)
	if err != nil {
		return nil, err
	}
	sort.SliceStable(rows, func(i, j int) bool { return rows[i].IntAt(0) < rows[j].IntAt(0) })
	list := []any{}
	for _, r := range rows {
		list = append(list, pyjson.Obj{{K: "id", V: r.Any("id")}, {K: "sender", V: r.Any("sender")}, {K: "gamertag", V: r.Any("gamertag")}, {K: "body", V: r.Any("body")}, {K: "created", V: r.Any("created")}})
	}
	return pyjson.Obj{{K: "messages", V: list}}, nil
}

func (l *Launcher) send(me int64, ref string, body any) (any, error) {
	text, err := checkMessage(me, body)
	if err != nil {
		return nil, err
	}
	if _, err := l.post(me, ref, text, 0); err != nil {
		return nil, err
	}
	return ok1(), nil
}

// checkMessage is the message rule of both chat paths: a string of 1-2000 characters after stripping, no control characters but newline and
// tab. Returns the stripped text.
func checkMessage(me int64, body any) (string, error) {
	s, isStr := body.(string)
	bad := !isStr
	if isStr {
		st := pyStrip(s)
		bad = st == "" || utf8.RuneCountInString(st) > 2000
		for _, ch := range s {
			if ch < 32 && ch != '\n' && ch != '\t' {
				bad = true
			}
		}
	}
	if bad {
		detail := pyjson.TypeName(body)
		if isStr {
			detail += fmt.Sprintf(" of %d characters", utf8.RuneCountInString(pyStrip(s)))
		}
		return "", auth.NewError(400, "bad_message", "Messages must contain 1–2000 characters.").WithXuid(me).WithDetail(detail)
	}
	return pyStrip(s), nil
}

// posted is a stored chat message.
type posted struct {
	id      int64
	channel string
	created float64
	text    string
}

// post stores a checked message in the channel of ref and pushes a club message to the members' chat sessions (all but skip, the sender
// that gets it as its answer; 0 = everyone).
func (l *Launcher) post(me int64, ref, text string, skip int64) (*posted, error) {
	var p posted
	err := l.C.DB.Do(func(t *db.Tx) error {
		ch, err := l.channel(me, ref, t.One)
		if err != nil {
			return err
		}
		p.channel, p.created, p.text = ch, nowF(), text
		row, err := t.One("INSERT INTO launcher_messages(sender,channel,body,created) VALUES (?,?,?,?) RETURNING id", me, ch, text, p.created)
		if err != nil {
			return err
		}
		p.id = row.IntAt(0)
		return nil
	})
	if err != nil {
		return nil, err
	}
	if strings.HasPrefix(p.channel, "club:") && l.C.Chat != nil {
		l.pushClubMessage(me, &p, skip)
	}
	return &p, nil
}

func (l *Launcher) readMark(me int64, ref string, last int64) (any, error) {
	err := l.C.DB.Do(func(t *db.Tx) error {
		ch, err := l.channel(me, ref, t.One)
		if err != nil {
			return err
		}
		row, err := t.One("SELECT MAX(id) FROM launcher_messages WHERE channel=?", ch)
		if err != nil {
			return err
		}
		var top int64
		if !row.IsNullAt(0) {
			top = row.IntAt(0)
		}
		last = max(0, min(last, top))
		_, err = t.Exec("INSERT INTO launcher_chat_reads(xuid,channel,last_id) VALUES (?,?,?) ON CONFLICT (xuid,channel) DO UPDATE SET last_id=GREATEST(launcher_chat_reads.last_id,excluded.last_id)", me, ch, last)
		return err
	})
	if err != nil {
		return nil, err
	}
	return ok1(), nil
}

// pruneMessages deletes chat messages past messageKeep (at most once an hour per process).
func (l *Launcher) pruneMessages() error {
	cs := l.cstate()
	now := nowF()
	cs.mu.Lock()
	if now-cs.pruneAt < 3600 {
		cs.mu.Unlock()
		return nil
	}
	cs.pruneAt = now
	cs.mu.Unlock()
	_, err := l.C.DB.Exec("DELETE FROM launcher_messages WHERE created<?", now-messageKeep)
	return err
}

// clubSuggestions are the friends added most recently who are not in a club yet: the shortcut list of the club invite screen.
func (l *Launcher) clubSuggestions(me int64) (any, error) {
	const limit = 5
	rows, err := l.C.DB.Query("SELECT CASE WHEN a=? THEN b ELSE a END AS xuid FROM friendships WHERE a=? OR b=? ORDER BY created DESC LIMIT 50", me, me, me)
	if err != nil {
		return nil, err
	}
	out := []any{}
	for _, r := range rows {
		person, err := l.profile(r.IntAt(0), nil)
		if err != nil {
			return nil, err
		}
		if objAny(person, "club") == nil {
			out = append(out, person)
		}
		if len(out) >= limit {
			break
		}
	}
	return pyjson.Obj{{K: "players", V: out}}, nil
}

func (l *Launcher) clubInvite(me int64, value string) (any, error) {
	target, err := l.resolve(value)
	if err != nil {
		return nil, err
	}
	if target == me {
		return nil, auth.NewError(400, "self_invite", "That is your own account.")
	}
	if l.ClubInv == nil {
		return nil, auth.NewError(503, "unavailable", "Club invites are not available on this server.").WithXuid(me).WithDetail("Ctx.club_invite is not set")
	}
	if err := l.ClubInv(me, target); err != nil {
		ce, ok := err.(*clubs.Error)
		if !ok {
			return nil, err
		}
		code := "unknown"
		for k, v := range clubs.Codes {
			if v == ce.Code {
				code = k
			}
		}
		msg, known := inviteErrors[code]
		if !known {
			msg = ce.Text
		}
		return nil, auth.NewError(409, code, msg).WithXuid(me).WithDetail(fmt.Sprintf("club rule: %s (target %#x)", ce.Text, target))
	}
	p, err := l.profile(target, nil)
	if err != nil {
		return nil, err
	}
	return pyjson.Obj{{K: "ok", V: true}, {K: "gamertag", V: objAny(p, "gamertag")}}, nil
}

func (l *Launcher) lobbies(me int64) (any, error) {
	result := l.C.Xbl.HubLobbies()
	if result == nil {
		return nil, auth.NewError(503, "unavailable", "The game server did not answer. Try again shortly.").WithXuid(me).WithDetail("lobby list query got no answer")
	}
	pops, ok := l.C.Xbl.Populations()
	if !ok {
		return nil, auth.NewError(503, "unavailable", "").WithXuid(me).WithDetail("population query got no answer")
	}
	flags := l.C.Xbl.FlaggedSet(pops)
	lv, _ := result.Get("lobbies")
	rows := l.C.Xbl.LauncherRows(me, anyList(lv), pops, flags)
	// Database lookups do not hold the matchmaking mutex.
	out := []any{}
	for _, r := range rows {
		row := r.Row
		if r.IDs != nil {
			members := []any{}
			for _, x := range r.IDs {
				s, _ := x.(string)
				n, _ := strconv.ParseInt(s, 16, 64)
				if n != 0 {
					p, err := l.profile(n, nil)
					if err != nil {
						return nil, err
					}
					members = append(members, p)
				}
			}
			row.Set("members", members)
		}
		out = append(out, row)
	}
	return pyjson.Obj{{K: "lobbies", V: out}, {K: "reservation", V: publicHold(l.C.Xbl.CurrentHold(fmt.Sprint(me)))}, {K: "time", V: time.Now().Unix()}}, nil
}

// handleCommunity answers /v1/community[/...].
func (l *Launcher) handleCommunity(r *Req, method, p string, u *url.URL, body pyjson.Obj, acct *auth.Account) (any, error) {
	me := acct.Xuid
	cs := l.cstate()
	cs.mu.Lock()
	cs.seen[me] = nowF()
	cs.mu.Unlock()
	if err := l.pruneMessages(); err != nil { // housekeeping must never fail a request
		if ok, skipped := cs.rate.Allow("prune-messages", 3600.0); ok {
			msg := "old chat messages not deleted, the request carries on"
			if skipped > 0 {
				msg += fmt.Sprintf(" (%d more failed since)", skipped)
			}
			communityLog.Exc(msg, err, diag.Warn)
		}
	}
	cs.mu.Lock() // expired presence entries carry no durable meaning
	for x, seen := range cs.seen {
		if nowF()-seen > 120 {
			delete(cs.seen, x)
		}
	}
	cs.mu.Unlock()
	if p == "/v1/community" && method == "GET" {
		return l.snapshot(me)
	}
	if p == "/v1/community/friends" && method == "POST" {
		v, _ := body.Get("player")
		if v == nil {
			if _, has := body.Get("player"); !has {
				return l.request(me, "")
			}
		}
		return l.request(me, pyjson.PyStr(v))
	}
	parts := strings.Split(p, "/")
	if (len(parts) == 5 || len(parts) == 6) && strings.Join(parts[:4], "/") == "/v1/community/friends" && isDigits(parts[4]) {
		target, err := strconv.ParseInt(parts[4], 10, 64)
		if err != nil || target <= 0 {
			return nil, auth.NewError(400, "bad_request", "")
		}
		switch {
		case len(parts) == 6 && parts[5] == "accept" && method == "POST":
			return l.respond(me, target, true)
		case len(parts) == 6 && parts[5] == "request" && method == "DELETE":
			return l.respond(me, target, false)
		case len(parts) == 5 && method == "DELETE":
			return l.remove(me, target)
		}
	}
	if p == "/v1/community/invites" {
		switch method {
		case "GET":
			return l.listInvites(me)
		case "POST":
			v, _ := body.Get("player")
			t, _ := body.Get("text")
			return l.sendInvite(me, pyjson.PyStr(v), t)
		}
	}
	if (len(parts) == 5 || len(parts) == 6) && strings.Join(parts[:4], "/") == "/v1/community/invites" && isDigits(parts[4]) {
		id, err := strconv.ParseInt(parts[4], 10, 64)
		if err != nil || id <= 0 {
			return nil, auth.NewError(400, "bad_request", "")
		}
		switch {
		case len(parts) == 6 && parts[5] == "accept" && method == "POST":
			return l.acceptInvite(me, id)
		case len(parts) == 5 && method == "DELETE":
			cs.invites.Take(me, id)
			return ok1(), nil
		}
	}
	if p == "/v1/community/lobbies" && method == "GET" {
		return l.lobbies(me)
	}
	if len(parts) == 5 && strings.Join(parts[:4], "/") == "/v1/community/player" && isDigits(parts[4]) && method == "GET" {
		target, err := strconv.ParseInt(parts[4], 10, 64)
		if err != nil || target <= 0 {
			return nil, auth.NewError(400, "bad_request", "")
		}
		return l.player(me, target)
	}
	if len(parts) == 6 && strings.Join(parts[:4], "/") == "/v1/community/player" && isDigits(parts[4]) && parts[5] == "achievements" && method == "GET" {
		target, err := strconv.ParseInt(parts[4], 10, 64)
		if err != nil || target <= 0 {
			return nil, auth.NewError(400, "bad_request", "")
		}
		return l.playerAchievements(me, target)
	}
	if p == "/v1/community/achievements" && method == "POST" {
		return l.reportAchievements(me, body)
	}
	if p == "/v1/community/profile" {
		switch method {
		case "GET":
			return l.ownProfile(me)
		case "POST":
			return l.setProfile(me, body)
		}
	}
	if p == "/v1/community/reservation" {
		switch method {
		case "POST":
			lobby, kind := "", ""
			if v, ok := body.Get("lobby"); ok && v != nil {
				lobby = pyjson.PyStr(v)
			}
			if v, ok := body.Get("kind"); ok && v != nil {
				kind = pyjson.PyStr(v)
			}
			h, err := l.C.Xbl.Reserve(fmt.Sprint(me), lobby, kind)
			if err != nil {
				return nil, err
			}
			// "player": the friend whose game is joined (the Guide's Join Session In Progress)
			if v, ok := body.Get("player"); ok && v != nil && lobby != "" {
				if host, perr := strconv.ParseInt(pyjson.PyStr(v), 10, 64); perr == nil && host > 0 && host != me {
					return pyjson.Obj{{K: "reservation", V: l.seatFor(me, h, host)}}, nil
				}
			}
			return pyjson.Obj{{K: "reservation", V: publicHold(h)}}, nil
		case "DELETE":
			l.C.Xbl.CancelHold(fmt.Sprint(me))
			return ok1(), nil
		}
	}
	if p == "/v1/community/club/suggestions" && method == "GET" {
		return l.clubSuggestions(me)
	}
	if p == "/v1/community/club/invite" && method == "POST" {
		v, _ := body.Get("player")
		return l.clubInvite(me, pyjson.PyStr(v))
	}
	if len(parts) == 5 && strings.Join(parts[:4], "/") == "/v1/community/chat" {
		ref := parts[4]
		switch method {
		case "GET":
			after := int64(0)
			if vals := u.Query()["after"]; len(vals) > 0 && vals[0] != "" {
				n, ok := pyInt(vals[0], 10)
				if !ok {
					return nil, auth.NewError(400, "bad_request", "")
				}
				after = max(0, n)
			}
			return l.messages(me, ref, after)
		case "POST":
			v, _ := body.Get("text")
			return l.send(me, ref, v)
		case "PATCH":
			v, has := body.Get("lastId")
			if !has {
				v = int64(0)
			}
			n, isInt := v.(int64)
			if !isInt {
				return nil, auth.NewError(400, "bad_request", "")
			}
			return l.readMark(me, ref, n)
		}
	}
	return nil, auth.NewError(404, "not_found", "")
}
