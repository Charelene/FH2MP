// Package clubs is Car Clubs: clubs, members and roles, invitations, join requests, the noticeboard and each player's club history
// (= server/clubs.py). A player is in at most one club. The rules come from the game's own club texts: a tag is 3 or 4 characters and unique, a club has
// one Leader and at most 3 Co-Leaders, a club has a maximum size, only Leader / Co-Leaders invite, approve, kick and post notices, and joining another
// club leaves the current one (a Leader who leaves disbands the club).
package clubs

import (
	"math"
	"strings"
	"time"
	"unicode/utf8"

	"fh2mp/internal/db"
	"fh2mp/internal/pyjson"
)

// Now is the clock in seconds (tests replace it).
var Now = func() float64 { return float64(time.Now().UnixNano()) / 1e9 }

// Roles are stored (and compared) by power: member 1 < co-leader 2 < leader 3. The wire numbers run the other way, confirmed in game: 1 = LEADER,
// 2 = CO-LEADER, 3 = MEMBER, 0 = none.
const (
	RoleMember   = 1
	RoleCoLeader = 2
	RoleLeader   = 3
)

// WireRole converts a stored role to the wire number.
func WireRole(power int64) int64 {
	if power != 0 {
		return 4 - power
	}
	return 0
}

// PowerRole converts a wire role to the stored one.
func PowerRole(wire int64) int64 {
	if 1 <= wire && wire <= 3 {
		return 4 - wire
	}
	return 0
}

// Privacy and limits. Open = 1 confirmed in game; Invite Only = 2 and Private = 3 are [guess].
const (
	PrivacyOpen       = 1
	PrivacyInviteOnly = 2
	PrivacyPrivate    = 3
	MaxMembers        = 100
	MaxGarage         = 20 // cars in one Club Garage [guess]
	MaxCoLeaders      = 3
)

// HistoryActions are the history codes [guess].
var HistoryActions = map[string]int64{"created": 0, "joined": 1, "quit": 2, "kicked": 3, "promoted": 4, "demoted": 5, "disbanded": 6}

// Keyword is one entry of the club keyword lists (Category 0 region, 1 attitude, 2 interest [guess]).
type Keyword struct {
	Keyword   string
	KeywordID int
	Category  int
}

// KeywordList is the three keyword lists with their ids.
func KeywordList() []Keyword {
	cats := []struct {
		cat   int
		words []string
	}{{0, []string{"Global", "North America", "South America", "Europe", "Asia", "Oceania", "Africa"}},
		{1, []string{"Casual", "Friendly", "Competitive", "Hardcore", "Relaxed", "Serious"}},
		{2, []string{"Racing", "Drifting", "Off-Road", "Cruising", "Car Collecting", "Tuning", "Design", "Photography"}}}
	var out []Keyword
	id := 1
	for _, c := range cats {
		for _, w := range c.words {
			out = append(out, Keyword{w, id, c.cat})
			id++
		}
	}
	return out
}

// Codes map a rule violation to the Xls ErrorCode (the low 16 bits of the HRESULT 0x8051xxxx); which code is which text is [guess].
var Codes = map[string]int{"not_found": 0x06, "tag_exists": 0x07, "no_invites": 0x0e, "already_invited": 0x0f, "in_club": 0x10, "club_full": 0x11,
	"co_leader_limit": 0x12, "no_such_invite": 0x13, "no_privilege": 0x14, "already_recommended": 0x15, "not_in_club": 0x16,
	"bad_tag": 0x17, "bad_word": 0x18, "keywords": 0x19, "unknown": 0x1a, "garage_full": 0x1b}

// Error is a rule violation; Code is the Xls ErrorCode the client maps to one of its club messages.
type Error struct {
	Text string
	Code int
}

func (e *Error) Error() string { return e.Text }

func fail(text, code string) error { return &Error{text, Codes[code]} }

// Club is a row of clubs; Members is only filled by Search and Recommended.
type Club struct {
	ID                                  int64
	Tag, Name, Description              string
	Privacy, Region, Attitude, Interest int64
	Created                             float64
	Members                             int64
}

func clubOf(r *db.Row) *Club {
	c := &Club{ID: r.Int("id"), Tag: r.Str("tag"), Name: r.Str("name"), Description: r.Str("description"), Privacy: r.Int("privacy"), Region: r.Int("region"),
		Attitude: r.Int("attitude"), Interest: r.Int("interest"), Created: r.Float("created")}
	if r.Has("members") {
		c.Members = r.Int("members")
	}
	return c
}

// Member is a row of the member list.
type Member struct {
	Xuid          int64
	Role          int64
	StatusChanged float64
}

// Notice is a noticeboard post.
type Notice struct {
	ID, ClubID, Sender, Role int64
	Message                  string
	Created                  float64
	NotificationIDs          string // JSON
}

// Info is (clubId, clubTag, role) for the UserInfo struct of a player.
type Info struct {
	ClubID int64
	Tag    string
	Role   int64
}

type querier interface {
	One(sql string, args ...any) (*db.Row, error)
	Query(sql string, args ...any) ([]db.Row, error)
	Exec(sql string, args ...any) (db.Result, error)
}

// Store is the club store.
type Store struct{ DB *db.DB }

// New makes a store.
func New(d *db.DB) *Store { return &Store{d} }

// ---------------------------------------------------------------- lookups

func (s *Store) q(t *db.Tx) querier {
	if t != nil {
		return t
	}
	return s.DB
}

// Club returns a club by id (nil = none).
func (s *Store) Club(id int64) (*Club, error) { return s.club(nil, id) }

func (s *Store) club(t *db.Tx, id int64) (*Club, error) {
	r, err := s.q(t).One("SELECT * FROM clubs WHERE id=?", id)
	if err != nil || r == nil {
		return nil, err
	}
	return clubOf(r), nil
}

// ByTag returns a club by its tag, ignoring case (nil = none).
func (s *Store) ByTag(tag string) (*Club, error) { return s.byTag(nil, tag) }

func (s *Store) byTag(t *db.Tx, tag string) (*Club, error) {
	r, err := s.q(t).One("SELECT * FROM clubs WHERE lower(tag)=lower(?)", tag)
	if err != nil || r == nil {
		return nil, err
	}
	return clubOf(r), nil
}

// Membership gives the club and role of the player's club ((nil, 0) when in none).
func (s *Store) Membership(xuid int64) (*Club, int64, error) { return s.membership(nil, xuid) }

func (s *Store) membership(t *db.Tx, xuid int64) (*Club, int64, error) {
	r, err := s.q(t).One("SELECT m.role, c.* FROM club_members m JOIN clubs c ON c.id = m.club_id WHERE m.xuid=?", xuid)
	if err != nil || r == nil {
		return nil, 0, err
	}
	return clubOf(r), r.Int("role"), nil
}

// MemberInfo is (clubId, clubTag, role) of a player; zero values without a club.
func (s *Store) MemberInfo(xuid int64) (Info, error) {
	c, role, err := s.Membership(xuid)
	if err != nil || c == nil {
		return Info{}, err
	}
	return Info{c.ID, c.Tag, role}, nil
}

// MemberInfos gives the info of the players that are in a club, in one query for a whole leaderboard page.
func (s *Store) MemberInfos(xuids []int64) (map[int64]Info, error) {
	seen := map[int64]bool{}
	var uniq []any
	for _, x := range xuids {
		if !seen[x] {
			seen[x] = true
			uniq = append(uniq, x)
		}
	}
	out := map[int64]Info{}
	if len(uniq) == 0 {
		return out, nil
	}
	rows, err := s.DB.Query("SELECT m.xuid, m.role, c.id, c.tag FROM club_members m JOIN clubs c ON c.id = m.club_id WHERE m.xuid IN ("+
		strings.TrimSuffix(strings.Repeat("?,", len(uniq)), ",")+")", uniq...)
	if err != nil {
		return nil, err
	}
	for i := range rows {
		out[rows[i].Int("xuid")] = Info{rows[i].Int("id"), rows[i].Str("tag"), rows[i].Int("role")}
	}
	return out, nil
}

// MemberCount is the number of members.
func (s *Store) MemberCount(clubID int64) (int64, error) {
	r, err := s.DB.One("SELECT COUNT(*) FROM club_members WHERE club_id=?", clubID)
	if err != nil {
		return 0, err
	}
	return r.IntAt(0), nil
}

// Leaders lists the leader and co-leaders, leader first.
func (s *Store) Leaders(clubID int64, limit int) ([]int64, error) {
	rows, err := s.DB.Query("SELECT xuid FROM club_members WHERE club_id=? AND role>=? ORDER BY role DESC, joined LIMIT ?", clubID, RoleCoLeader, limit)
	if err != nil {
		return nil, err
	}
	out := make([]int64, len(rows))
	for i := range rows {
		out[i] = rows[i].Int("xuid")
	}
	return out, nil
}

// Leader is the club's leader (0 = none).
func (s *Store) Leader(clubID int64) (int64, error) {
	r, err := s.DB.One("SELECT xuid FROM club_members WHERE club_id=? AND role=? ", clubID, RoleLeader)
	if err != nil || r == nil {
		return 0, err
	}
	return r.Int("xuid"), nil
}

// Champion is the member with the most XP.
func (s *Store) Champion(clubID int64) (int64, error) {
	r, err := s.DB.One("SELECT m.xuid FROM club_members m LEFT JOIN player_xp x ON x.xuid = m.xuid WHERE m.club_id=? "+
		"ORDER BY GREATEST(0, COALESCE(x.xp, 0) - m.xp_base) DESC, m.joined LIMIT 1", clubID)
	if err != nil || r == nil {
		return 0, err
	}
	return r.Int("xuid"), nil
}

// Gain is the club XP of a member: what they earned since they joined; the client shows level = XP / 100 + 1.
const Gain = "GREATEST(0, x.xp - m.xp_base)"

// XP is the club's XP.
func (s *Store) XP(clubID int64) (int64, error) {
	r, err := s.DB.One("SELECT COALESCE(SUM("+Gain+"), 0) FROM club_members m JOIN player_xp x ON x.xuid = m.xuid WHERE m.club_id=?", clubID)
	if err != nil {
		return 0, err
	}
	return int64(r.FloatAt(0)), nil
}

// XPRank is the club's place on the XP ladder.
func (s *Store) XPRank(clubID int64) (int64, error) {
	r, err := s.DB.One("SELECT COUNT(*) + 1 FROM (SELECT m.club_id, SUM("+Gain+") AS t FROM club_members m JOIN player_xp x ON x.xuid = m.xuid "+
		"GROUP BY m.club_id) s WHERE s.t > (SELECT COALESCE(SUM("+Gain+"), 0) FROM club_members m JOIN player_xp x "+
		"ON x.xuid = m.xuid WHERE m.club_id=?)", clubID)
	if err != nil {
		return 0, err
	}
	return r.IntAt(0), nil
}

// Members lists the members, leader first.
func (s *Store) Members(clubID int64, limit int) ([]Member, error) {
	rows, err := s.DB.Query("SELECT xuid, role, status_changed FROM club_members WHERE club_id=? ORDER BY role DESC, joined LIMIT ?", clubID, limit)
	if err != nil {
		return nil, err
	}
	out := make([]Member, len(rows))
	for i := range rows {
		out[i] = Member{rows[i].Int("xuid"), rows[i].Int("role"), rows[i].Float("status_changed")}
	}
	return out, nil
}

// AcceptsInvites says whether a player takes club invites (default yes).
func (s *Store) AcceptsInvites(xuid int64) (bool, error) {
	r, err := s.DB.One("SELECT accepts FROM club_prefs WHERE xuid=?", xuid)
	if err != nil {
		return false, err
	}
	if r == nil {
		return true, nil
	}
	return r.Int("accepts") != 0, nil
}

// SetAcceptsInvites changes the preference.
func (s *Store) SetAcceptsInvites(xuid int64, accepts bool) error {
	v := int64(0)
	if accepts {
		v = 1
	}
	return s.DB.Do(func(t *db.Tx) error {
		_, err := t.Exec("INSERT INTO club_prefs (xuid, accepts) VALUES (?,?) ON CONFLICT (xuid) DO UPDATE SET accepts=excluded.accepts", xuid, v)
		return err
	})
}

// ---------------------------------------------------------------- search

// SearchSpec is the filter of Search; 0 means "any" (privacy: <= 0).
type SearchSpec struct {
	Privacy, Region, Attitude, Interest int64
	Name                                string
	Limit                               int
}

// Search lists clubs, biggest first.
func (s *Store) Search(q SearchSpec) ([]*Club, error) {
	where, args := []string{"1=1"}, []any{}
	if q.Privacy > 0 { // 0 = any
		where = append(where, "privacy=?")
		args = append(args, q.Privacy)
	}
	for _, c := range []struct {
		col string
		v   int64
	}{{"region", q.Region}, {"attitude", q.Attitude}, {"interest", q.Interest}} {
		if c.v > 0 {
			where = append(where, c.col+"=?")
			args = append(args, c.v)
		}
	}
	if q.Name != "" {
		where = append(where, "name ILIKE ?")
		args = append(args, "%"+q.Name+"%")
	}
	rows, err := s.DB.Query("SELECT *, (SELECT COUNT(*) FROM club_members m WHERE m.club_id = clubs.id) AS members FROM clubs WHERE "+strings.Join(where, " AND ")+
		" ORDER BY members DESC, id LIMIT ?", append(args, max(1, q.Limit))...)
	return clubsOf(rows), err
}

func clubsOf(rows []db.Row) []*Club {
	out := make([]*Club, len(rows))
	for i := range rows {
		out[i] = clubOf(&rows[i])
	}
	return out
}

// Recommended lists open and invite-only clubs the player is not in, biggest first.
func (s *Store) Recommended(xuid int64, limit int) ([]*Club, error) {
	club, _, err := s.Membership(xuid)
	if err != nil {
		return nil, err
	}
	var own int64
	if club != nil {
		own = club.ID
	}
	rows, err := s.DB.Query("SELECT *, (SELECT COUNT(*) FROM club_members m WHERE m.club_id = clubs.id) AS members FROM clubs WHERE privacy <> ? AND id <> ? "+
		"ORDER BY members DESC, id LIMIT ?", PrivacyPrivate, own, max(1, limit))
	return clubsOf(rows), err
}

// Total is the number of clubs.
func (s *Store) Total() (int64, error) {
	r, err := s.DB.One("SELECT COUNT(*) FROM clubs")
	if err != nil {
		return 0, err
	}
	return r.IntAt(0), nil
}

// Tier is (weekly XP threshold, credit reward). The client sorts the rows into tiers by walking this list in order (Tier 1 has to be the highest); the
// lowest tab (Tier 6) takes everyone under the last threshold. The values are mine [guess].
type Tier struct{ XP, Credits int64 }

// Tiers are the ladder tiers, Tier 1 first.
var Tiers = []Tier{{50000, 250000}, {25000, 100000}, {10000, 50000}, {2500, 25000}, {500, 10000}}

// WeekStart is the start of the ladder week: Monday 00:00 UTC [guess; the original reset time is unknown]. now nil = the clock.
func WeekStart(now *float64) float64 {
	t := Now()
	if now != nil {
		t = *now
	}
	r := math.Mod(t+3*86400, 7*86400)
	if r < 0 {
		r += 7 * 86400
	}
	return t - r
}

// Standing is a club or member with its XP.
type Standing struct {
	ID int64
	XP float64
}

// Ladder lists every club with its XP, best first.
func (s *Store) Ladder() ([]Standing, error) {
	rows, err := s.DB.Query("SELECT c.id, COALESCE(SUM(" + Gain + "), 0) AS xp FROM clubs c LEFT JOIN club_members m ON m.club_id = c.id " +
		"LEFT JOIN player_xp x ON x.xuid = m.xuid GROUP BY c.id ORDER BY xp DESC, c.id")
	if err != nil {
		return nil, err
	}
	out := make([]Standing, len(rows))
	for i := range rows {
		out[i] = Standing{rows[i].Int("id"), rows[i].Float("xp")}
	}
	return out, nil
}

// MemberLadder ranks the club's members by the XP they earned this week (never from before they joined). It uses the accepted awards of the XP
// ledger, so the tiers start over every week instead of filling up for good. now nil = the clock.
func (s *Store) MemberLadder(clubID int64, now *float64) ([]Standing, error) {
	rows, err := s.DB.Query("SELECT m.xuid, COALESCE((SELECT SUM(a.xp) FROM xp_awards a WHERE a.xuid = m.xuid AND a.verdict = 'ok' "+
		"AND a.at >= GREATEST(m.joined, ?)), 0) AS xp FROM club_members m WHERE m.club_id=? ORDER BY xp DESC, m.joined", WeekStart(now), clubID)
	if err != nil {
		return nil, err
	}
	out := make([]Standing, len(rows))
	for i := range rows {
		out[i] = Standing{rows[i].Int("xuid"), rows[i].Float("xp")}
	}
	return out, nil
}

// ---------------------------------------------------------------- history

func (s *Store) history(t *db.Tx, xuid, clubID int64, clubName, action string, role int64) error {
	_, err := t.Exec("INSERT INTO club_history (xuid, club_id, club_name, action, role, created) VALUES (?,?,?,?,?,?)", xuid, clubID, clubName, HistoryActions[action], role, Now())
	return err
}

func trunc(s string, n int) string {
	if utf8.RuneCountInString(s) <= n {
		return s
	}
	return string([]rune(s)[:n])
}

// ---------------------------------------------------------------- create / change / disband

// Create makes a club with the player as its Leader and returns the club id.
func (s *Store) Create(xuid int64, tag, name, description string, privacy, region, attitude, interest int64) (int64, error) {
	tag, name = strings.TrimSpace(tag), strings.TrimSpace(name)
	if n := utf8.RuneCountInString(tag); n < 3 || n > 4 {
		return 0, fail("club tags must be 3 or 4 characters long", "bad_tag")
	}
	if name == "" {
		return 0, fail("a club needs a name", "unknown")
	}
	var cid int64
	err := s.DB.Do(func(t *db.Tx) error {
		if c, _, err := s.membership(t, xuid); err != nil || c != nil {
			if err != nil {
				return err
			}
			return fail("already in a club", "in_club")
		}
		if c, err := s.byTag(t, tag); err != nil || c != nil {
			if err != nil {
				return err
			}
			return fail("the club tag already exists", "tag_exists")
		}
		res, err := t.Exec("INSERT INTO clubs (tag, name, description, privacy, region, attitude, interest, created) VALUES (?,?,?,?,?,?,?,?)",
			tag, trunc(name, 50), trunc(description, 400), privacy, region, attitude, interest, Now())
		if err != nil {
			return err
		}
		cid = res.LastID
		if _, err := t.Exec("INSERT INTO club_members (xuid, club_id, role, joined, status_changed, xp_base) VALUES "+
			"(?,?,?,?,?, COALESCE((SELECT xp FROM player_xp WHERE xuid=?), 0))", xuid, cid, RoleLeader, Now(), Now(), xuid); err != nil {
			return err
		}
		return s.history(t, xuid, cid, name, "created", RoleLeader)
	})
	return cid, err
}

// ChangeDetails lets the Leader change the club and returns its id.
func (s *Store) ChangeDetails(xuid int64, name, description string, privacy, region, attitude, interest int64) (int64, error) {
	var id int64
	err := s.DB.Do(func(t *db.Tx) error {
		club, role, err := s.membership(t, xuid)
		if err != nil {
			return err
		}
		if club == nil {
			return fail("not in a club", "not_in_club")
		}
		if role != RoleLeader {
			return fail("only the leader changes the club", "no_privilege")
		}
		if name == "" {
			name = club.Name
		}
		_, err = t.Exec("UPDATE clubs SET name=?, description=?, privacy=?, region=?, attitude=?, interest=? WHERE id=?", trunc(name, 50), trunc(description, 400), privacy, region, attitude, interest, club.ID)
		id = club.ID
		return err
	})
	return id, err
}

func (s *Store) remove(t *db.Tx, xuid int64, club *Club, action string) error {
	if _, err := t.Exec("DELETE FROM shared_cars WHERE owner=? AND club_id=?", xuid, club.ID); err != nil { // a member's shared cars leave with them
		return err
	}
	if _, err := t.Exec("DELETE FROM club_members WHERE xuid=?", xuid); err != nil {
		return err
	}
	return s.history(t, xuid, club.ID, club.Name, action, 0)
}

// Disband removes the club inside a transaction and returns the xuids of the members it had.
func (s *Store) Disband(t *db.Tx, club *Club) ([]int64, error) {
	rows, err := t.Query("SELECT xuid FROM club_members WHERE club_id=?", club.ID)
	if err != nil {
		return nil, err
	}
	members := make([]int64, len(rows))
	for i := range rows {
		members[i] = rows[i].Int("xuid")
	}
	for _, x := range members {
		if err := s.history(t, x, club.ID, club.Name, "disbanded", 0); err != nil {
			return nil, err
		}
	}
	// members, invites, requests, notices cascade
	_, err = t.Exec("DELETE FROM clubs WHERE id=?", club.ID)
	return members, err
}

func without(l []int64, x int64) []int64 {
	out := []int64{}
	for _, v := range l {
		if v != x {
			out = append(out, v)
		}
	}
	return out
}

// ---------------------------------------------------------------- joining

// join adds xuid to club, leaving a previous club first (a Leader's club is disbanded). Returns the xuids disbanded out of the old club.
func (s *Store) join(t *db.Tx, xuid int64, club *Club, role int64) ([]int64, error) {
	old, oldRole, err := s.membership(t, xuid)
	if err != nil {
		return nil, err
	}
	left := []int64{}
	if old != nil && old.ID != club.ID {
		if oldRole == RoleLeader {
			ds, err := s.Disband(t, old)
			if err != nil {
				return nil, err
			}
			left = without(ds, xuid)
		} else if err := s.remove(t, xuid, old, "quit"); err != nil {
			return nil, err
		}
	}
	r, err := t.One("SELECT COUNT(*) FROM club_members WHERE club_id=?", club.ID)
	if err != nil {
		return nil, err
	}
	if r.IntAt(0) >= MaxMembers {
		return nil, fail("the club has reached its maximum size", "club_full")
	}
	if _, err := t.Exec("INSERT INTO club_members (xuid, club_id, role, joined, status_changed, xp_base) VALUES "+
		"(?,?,?,?,?, COALESCE((SELECT xp FROM player_xp WHERE xuid=?), 0)) ON CONFLICT (xuid) DO NOTHING", xuid, club.ID, role, Now(), Now(), xuid); err != nil {
		return nil, err
	}
	if _, err := t.Exec("DELETE FROM club_requests WHERE xuid=?", xuid); err != nil {
		return nil, err
	}
	if _, err := t.Exec("DELETE FROM club_invites WHERE invitee=? AND club_id=?", xuid, club.ID); err != nil {
		return nil, err
	}
	return left, s.history(t, xuid, club.ID, club.Name, "joined", role)
}

// RequestJoin: an open club takes the player at once; others queue a request. Looks the club up by id, or by tag when clubID is nil. Returns (club, approved).
func (s *Store) RequestJoin(xuid int64, clubID *int64, tag string) (*Club, bool, error) {
	var club *Club
	approved := false
	err := s.DB.Do(func(t *db.Tx) error {
		var err error
		if clubID != nil {
			club, err = s.club(t, *clubID)
		} else {
			club, err = s.byTag(t, tag)
		}
		if err != nil {
			return err
		}
		if club == nil {
			return fail("club not found", "not_found")
		}
		cur, _, err := s.membership(t, xuid)
		if err != nil {
			return err
		}
		if cur != nil && cur.ID == club.ID {
			return fail("already in this club", "in_club")
		}
		if club.Privacy == PrivacyOpen {
			_, err := s.join(t, xuid, club, RoleMember)
			approved = err == nil
			return err
		}
		if club.Privacy == PrivacyPrivate {
			return fail("the club is private", "no_privilege")
		}
		_, err = t.Exec("INSERT INTO club_requests (club_id, xuid, created) VALUES (?,?,?) ON CONFLICT DO NOTHING", club.ID, xuid, Now())
		return err
	})
	if err != nil {
		return nil, false, err
	}
	return club, approved, nil
}

// Approve lets a Leader / Co-Leader accept a join request.
func (s *Store) Approve(leaderXuid, userXuid int64) (*Club, error) {
	var club *Club
	err := s.DB.Do(func(t *db.Tx) error {
		c, role, err := s.membership(t, leaderXuid)
		if err != nil {
			return err
		}
		if c == nil || role < RoleCoLeader {
			return fail("insufficient privileges", "no_privilege")
		}
		club = c
		if uc, _, err := s.membership(t, userXuid); err != nil {
			return err
		} else if uc != nil && uc.ID == c.ID {
			return nil // already in (joined meanwhile): accepting a stale request is a no-op
		}
		r, err := t.One("SELECT 1 FROM club_requests WHERE club_id=? AND xuid=?", c.ID, userXuid)
		if err != nil {
			return err
		}
		if r == nil {
			return fail("no such request", "no_such_invite")
		}
		_, err = s.join(t, userXuid, c, RoleMember)
		return err
	})
	if err != nil {
		return nil, err
	}
	return club, nil
}

// Invite lets a Leader / Co-Leader invite a player; returns the club and the invite id.
func (s *Store) Invite(sponsor, invitee int64) (*Club, int64, error) {
	var club *Club
	var iid int64
	err := s.DB.Do(func(t *db.Tx) error {
		c, role, err := s.membership(t, sponsor)
		if err != nil {
			return err
		}
		if c == nil {
			return fail("not in a club", "not_in_club")
		}
		if role < RoleCoLeader {
			return fail("insufficient privileges", "no_privilege")
		}
		if ok, err := s.AcceptsInvites(invitee); err != nil {
			return err
		} else if !ok {
			return fail("the user does not accept club invites", "no_invites")
		}
		if ic, _, err := s.membership(t, invitee); err != nil {
			return err
		} else if ic != nil {
			return fail("the player is already in a club", "in_club")
		}
		r, err := t.One("SELECT 1 FROM club_invites WHERE club_id=? AND invitee=?", c.ID, invitee)
		if err != nil {
			return err
		}
		if r != nil {
			return fail("already invited", "already_invited")
		}
		res, err := t.Exec("INSERT INTO club_invites (club_id, sponsor, invitee, created) VALUES (?,?,?,?)", c.ID, sponsor, invitee, Now())
		club, iid = c, res.LastID
		return err
	})
	if err != nil {
		return nil, 0, err
	}
	return club, iid, nil
}

// AcceptInvite joins the club of an invite. The client's invitation id is the id of the Message Center notification, which is not the row id here:
// the invite is matched on club + invitee.
func (s *Store) AcceptInvite(xuid, clubID, invitationID int64) (*Club, error) {
	var club *Club
	err := s.DB.Do(func(t *db.Tx) error {
		inv, err := t.One("SELECT * FROM club_invites WHERE club_id=? AND invitee=?", clubID, xuid)
		if err != nil {
			return err
		}
		c, err := s.club(t, clubID)
		if err != nil {
			return err
		}
		if inv == nil || c == nil {
			return fail("the invite does not exist", "no_such_invite")
		}
		club = c
		_, err = s.join(t, xuid, c, RoleMember)
		return err
	})
	if err != nil {
		return nil, err
	}
	return club, nil
}

// ---------------------------------------------------------------- members

// ChangeRole returns (club, old role). Promoting someone to Leader hands the leadership over (the actor becomes a Co-Leader).
func (s *Store) ChangeRole(actor, target, newRole int64) (*Club, int64, error) {
	var club *Club
	var trole int64
	err := s.DB.Do(func(t *db.Tx) error {
		c, role, err := s.membership(t, actor)
		if err != nil {
			return err
		}
		tc, tr, err := s.membership(t, target)
		if err != nil {
			return err
		}
		if c == nil {
			return fail("not in a club", "not_in_club")
		}
		if tc == nil || tc.ID != c.ID {
			return fail("not in your club", "not_in_club")
		}
		if actor != target && role != RoleLeader {
			return fail("insufficient privileges", "no_privilege")
		}
		if actor == target && newRole > role {
			return fail("insufficient privileges", "no_privilege")
		}
		if newRole == RoleCoLeader && tr != RoleCoLeader {
			r, err := t.One("SELECT COUNT(*) FROM club_members WHERE club_id=? AND role=?", c.ID, RoleCoLeader)
			if err != nil {
				return err
			}
			if r.IntAt(0) >= MaxCoLeaders {
				return fail("only 3 Co-Leaders are allowed", "co_leader_limit")
			}
		}
		if newRole == RoleLeader && actor != target {
			if _, err := t.Exec("UPDATE club_members SET role=?, status_changed=? WHERE xuid=?", RoleCoLeader, Now(), actor); err != nil {
				return err
			}
		}
		if _, err := t.Exec("UPDATE club_members SET role=?, status_changed=? WHERE xuid=?", newRole, Now(), target); err != nil {
			return err
		}
		action := "demoted"
		if newRole > tr {
			action = "promoted"
		}
		club, trole = c, tr
		return s.history(t, target, c.ID, c.Name, action, newRole)
	})
	if err != nil {
		return nil, 0, err
	}
	return club, trole, nil
}

// Remove kicks (or quits when actor == target). Returns (club, kind, disbanded members): kind "quit" | "kicked" | "disbanded".
func (s *Store) Remove(actor, target int64) (*Club, string, []int64, error) {
	var club *Club
	kind := ""
	disbanded := []int64{}
	err := s.DB.Do(func(t *db.Tx) error {
		c, role, err := s.membership(t, actor)
		if err != nil {
			return err
		}
		if c == nil {
			return fail("not in a club", "not_in_club")
		}
		tc, trole, err := s.membership(t, target)
		if err != nil {
			return err
		}
		if tc == nil || tc.ID != c.ID {
			return fail("not in your club", "not_in_club")
		}
		club = c
		if actor == target {
			if role == RoleLeader {
				ds, err := s.Disband(t, c)
				kind, disbanded = "disbanded", without(ds, actor)
				return err
			}
			kind = "quit"
			return s.remove(t, target, c, "quit")
		}
		if role < RoleCoLeader || trole >= role {
			return fail("insufficient privileges", "no_privilege")
		}
		kind = "kicked"
		return s.remove(t, target, c, "kicked")
	})
	if err != nil {
		return nil, "", nil, err
	}
	return club, kind, disbanded, nil
}

// ---------------------------------------------------------------- noticeboard

// PostNotice records a notice; notificationIDs are the per-member Message Center rows created for it. Returns the notice id.
func (s *Store) PostNotice(xuid int64, message string, notificationIDs []int64) (int64, error) {
	ids, err := pyjson.Dumps(notificationIDs, pyjson.Opts{})
	if err != nil {
		return 0, err
	}
	var nid int64
	err = s.DB.Do(func(t *db.Tx) error {
		club, role, err := s.membership(t, xuid)
		if err != nil {
			return err
		}
		if club == nil || role < RoleCoLeader {
			return fail("insufficient privileges", "no_privilege")
		}
		res, err := t.Exec("INSERT INTO club_notices (club_id, sender, role, message, created, notification_ids) VALUES (?,?,?,?,?,?)", club.ID, xuid, role, message, Now(), ids)
		nid = res.LastID
		return err
	})
	return nid, err
}

func noticeOf(r *db.Row) *Notice {
	return &Notice{r.Int("id"), r.Int("club_id"), r.Int("sender"), r.Int("role"), r.Str("message"), r.Float("created"), r.Str("notification_ids")}
}

// Notice returns a notice (nil = none).
func (s *Store) Notice(id int64) (*Notice, error) {
	r, err := s.DB.One("SELECT * FROM club_notices WHERE id=?", id)
	if err != nil || r == nil {
		return nil, err
	}
	return noticeOf(r), nil
}

// DeleteNotice removes a notice and returns the Message Center ids to delete.
func (s *Store) DeleteNotice(xuid, noticeID int64) ([]int64, error) {
	var ids []int64
	err := s.DB.Do(func(t *db.Tx) error {
		club, role, err := s.membership(t, xuid)
		if err != nil {
			return err
		}
		r, err := t.One("SELECT * FROM club_notices WHERE id=?", noticeID)
		if err != nil {
			return err
		}
		if club == nil || role < RoleCoLeader || r == nil || r.Int("club_id") != club.ID {
			return fail("insufficient privileges", "no_privilege")
		}
		if _, err := t.Exec("DELETE FROM club_notices WHERE id=?", noticeID); err != nil {
			return err
		}
		v, err := pyjson.Parse(r.Str("notification_ids"))
		if err != nil {
			return err
		}
		ids = []int64{}
		if l, ok := v.([]any); ok {
			for _, x := range l {
				switch n := x.(type) {
				case int64:
					ids = append(ids, n)
				case float64:
					ids = append(ids, int64(n))
				}
			}
		}
		return nil
	})
	return ids, err
}
