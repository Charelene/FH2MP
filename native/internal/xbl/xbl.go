// Package xbl is the stand-in for the Xbox Live Multiplayer Session Directory (MPSD) and matchmaking, enough for the game's embedded XSAPI
// (= server/xbl.py and server/launcher_lobbies.py). Sessions live in memory and are lost when the server restarts. Nothing here talks to Microsoft.
//
// The game writes sessions with PUT /serviceconfigs/{scid}/sessionTemplates/{template}/sessions/{name} and reads them back with GET; the reply must
// be a full session document (XSAPI's multiplayer_session::_Deserialize requires constants.system and properties.system, and each member needs
// constants.system and properties.system). Matchmaking tickets are matched at once to a lobby session of this server (momatch.xboxlive.com).
package xbl

import (
	"encoding/base64"
	"fmt"
	"math"
	"net"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"fh2mp/internal/auth"
	"fh2mp/internal/capacity"
	"fh2mp/internal/diag"
	"fh2mp/internal/pyjson"
)

// Limits and timings.
const (
	MaxSessionsPerOwner     = 32 // live sessions one player may have created
	MaxSessionBytes         = 256 * 1024
	SessionIdleSeconds      = 6 * 3600.0 // a session nobody read, wrote or matched into for this long is forgotten
	EmptySessionIdleSeconds = 900.0      // one without members goes sooner
	TicketSeconds           = 900.0
	MaxTicketsPerPlayer     = 8
	RefusalLogSeconds       = 30.0
	ControlLogSeconds       = 60.0
	TargetTemplate          = "defaultTemplate"
	LeftAvoidSeconds        = 120.0 // a search started in, or right after leaving, a lobby is never matched back into it
	HeldRejoinSeconds       = 20.0  // a seat reserved on purpose beats that rule once the leave is this old (prod 2026-11-06, see heldSeatVsAvoid)
	ReserveSeconds          = 30.0  // a matched player that has not connected after this long no longer holds a seat
	MemberIdleSeconds       = 60.0  // a matched player that never joins the session drops out of it
	// ActiveMemberIdleSeconds: a player that joined (active) is in the game world and does not poll the session there.
	ActiveMemberIdleSeconds = 1800.0
	DefaultScid             = "788f0100-a4bc-46c3-b19b-d1001a96545b"
)

var (
	mpsdLog  = diag.Get("mpsd")
	matchLog = diag.Get("match")
	hubLog   = diag.Get("hubctl")
	lobbyLog = diag.Get("lobbies")
)

var (
	sessionPath = regexp.MustCompile(`(?i)^/serviceconfigs/([^/]+)/sessiontemplates/([^/]+)/sessions/([^/?]+)`)
	momatchPath = regexp.MustCompile(`(?i)^/serviceconfigs/([^/]+)/hoppers/([^/?]+)(?:/tickets/([^/?]*))?`)
	guidRe      = regexp.MustCompile(`^[0-9a-fA-F]{8}(-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}$`)
	digitsRe    = regexp.MustCompile(`^[0-9]+$`)
)

func isDigits(s string) bool { return digitsRe.MatchString(s) }

// Hub is the control channel to the game server.
type Hub interface {
	// Populations is {lobby name: [hex xuid, ...]} of the players the game server has in each lobby; ok=false when it does not answer.
	Populations() (map[string][]string, bool)
	// Control sends one text datagram.
	Control(text string)
	// Query asks for a JSON summary ("lobbies", "positions"); nil when there is no usable answer.
	Query(command string) pyjson.Obj
	// Tools reports tools used by a player and returns the hub's reply ("" = no answer).
	Tools(text string) string
}

// Session is the stored record of a session.
type Session struct {
	Scid, Template, Name string
	Corr, Branch, Start  string
	Change               int64
	Owner                string // "" = none
	Constants            *Dict
	Properties           *Dict
	Members              *Dict // member key -> *Dict
	Servers              *Dict
	Used                 float64
	HasUsed              bool
	Seen                 map[string]float64
	HasSeen              bool
}

func (s *Session) obj() pyjson.Obj {
	var owner any
	if s.Owner != "" {
		owner = s.Owner
	}
	o := pyjson.Obj{{K: "scid", V: s.Scid}, {K: "template", V: s.Template}, {K: "name", V: s.Name}, {K: "corr", V: s.Corr}, {K: "branch", V: s.Branch},
		{K: "start", V: s.Start}, {K: "change", V: s.Change}, {K: "owner", V: owner}, {K: "constants", V: ToJSON(s.Constants)},
		{K: "properties", V: ToJSON(s.Properties)}, {K: "members", V: ToJSON(s.Members)}, {K: "servers", V: ToJSON(s.Servers)}}
	if s.HasUsed {
		o = append(o, pyjson.Pair{K: "used", V: s.Used})
	}
	if s.HasSeen {
		keys := make([]string, 0, len(s.Seen))
		for k := range s.Seen {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		seen := pyjson.Obj{}
		for _, k := range keys {
			seen = append(seen, pyjson.Pair{K: k, V: s.Seen[k]})
		}
		o = append(o, pyjson.Pair{K: "seen", V: seen})
	}
	return o
}

// Ticket is a matchmaking ticket.
type Ticket struct {
	Scid, Hopper string
	Ref          *Dict // ticketSessionRef or nil
	Attrs        *Dict
	Preserve     any
	Xuid         string
	At           float64
	Lobby        string
}

// LobbyKey identifies the set of lobbies players share: the same hopper, game-server image set and location.
type LobbyKey struct{ Scid, Hopper, Gsi, Location string }

// Hold is a launcher seat lease.
type Hold struct {
	ID        string
	Kind      string
	ExpiresAt int64
	Key       *LobbyKey
}

type left struct {
	name string
	at   float64
}

// Server is the MPSD / matchmaking state.
type Server struct {
	mu        sync.Mutex
	sessions  map[[3]string]*Session
	tickets   map[string]*Ticket
	lobbies   map[LobbyKey][]string
	lobbyKeys []LobbyKey
	reserved  map[string]map[string]float64
	left      map[string]left
	holds     map[string]*Hold
	lastSweep float64

	rate         *diag.RateLimit
	popDownUntil float64
	hubDownSince float64
	hubWasDown   bool

	Hub        Hub
	NameLookup func(xuid int64) string // xuid -> gamertag; the real MPSD fills a member's gamertag itself, the game never sends it
	Now        func() float64
	Monotonic  func() float64
	NewUUID    func() string
	// MaxSessions and LobbyCapacity are limits tests change.
	MaxSessions   int
	LobbyCapacity int
}

// New makes a server on a hub.
func New(hub Hub) *Server {
	start := time.Now()
	return &Server{
		sessions: map[[3]string]*Session{}, tickets: map[string]*Ticket{}, lobbies: map[LobbyKey][]string{}, reserved: map[string]map[string]float64{},
		left: map[string]left{}, holds: map[string]*Hold{}, rate: diag.NewRateLimit(nil, 0), Hub: hub,
		Now:       func() float64 { return float64(time.Now().UnixNano()) / 1e9 },
		Monotonic: func() float64 { return time.Since(start).Seconds() },
		NewUUID:   randomUUID, MaxSessions: 20000, LobbyCapacity: capacity.FreeRoam(),
	}
}

func (s *Server) every(l *diag.Logger, level, key string, interval float64, msg string) {
	if ok, skipped := s.rate.Allow(key, interval); ok {
		if skipped > 0 {
			msg += fmt.Sprintf(" (+%d like it since the last line)", skipped)
		}
		switch level {
		case "warn":
			l.Warn(msg)
		case "info":
			l.Info(msg)
		default:
			l.Debug(msg)
		}
	}
}

func kv(fields ...diag.KVP) string { return diag.KV(fields...) }

func f(k string, v any) diag.KVP { return diag.KVP{K: k, V: v} }

func (s *Server) refuse(status int, key *[3]string, xuid, why string, extra ...diag.KVP) (int, pyjson.Obj) {
	var tmpl, sess any
	if key != nil {
		tmpl, sess = key[1], trunc(key[2], 8)
	}
	fields := append([]diag.KVP{f("xuid", nilIfEmpty(xuid)), f("template", tmpl), f("session", sess)}, extra...)
	s.every(mpsdLog, "warn", fmt.Sprintf("refuse/%d/%s/%s", status, why, xuid), RefusalLogSeconds, fmt.Sprintf("PUT refused %d: %s ", status, why)+kv(fields...))
	return status, nil
}

func nilIfEmpty(s string) any {
	if s == "" {
		return nil
	}
	return s
}

func trunc(s string, n int) string {
	r := []rune(s)
	if len(r) > n {
		return string(r[:n])
	}
	return s
}

func (s *Server) nowISO() string {
	return time.Unix(int64(s.Now()), 0).UTC().Format("2006-01-02T15:04:05.000Z")
}

// ParsePath splits a session path into (scid, template, name).
func ParsePath(path string) (key [3]string, ok bool) {
	p, _, _ := strings.Cut(path, "?")
	m := sessionPath.FindStringSubmatch(p)
	if m == nil {
		return key, false
	}
	return [3]string{m[1], m[2], m[3]}, true
}

func lowerKey(k [3]string) [3]string {
	return [3]string{strings.ToLower(k[0]), strings.ToLower(k[1]), strings.ToLower(k[2])}
}

func (s *Server) memberName(key string) string {
	if !isDigits(key) || s.NameLookup == nil {
		return ""
	}
	n, err := strconv.ParseInt(key, 10, 64)
	if err != nil {
		return ""
	}
	return s.NameLookup(n)
}

func truthy(v any) bool {
	switch x := v.(type) {
	case nil:
		return false
	case bool:
		return x
	case string:
		return x != ""
	case int64:
		return x != 0
	case uint64:
		return x != 0
	case float64:
		return x != 0
	case []any:
		return len(x) > 0
	case *Dict:
		return x != nil && x.Len() > 0
	}
	return true
}

func memberDoc(s *Server, index int, m *Dict, now, key string) *Dict {
	constants := NewDict()
	if c := m.SubOrNil("constants"); c != nil {
		constants = c.copyDict()
	}
	system, _ := constants.Sub("system")
	if isDigits(key) {
		system.Set("xuid", key) // the entry's key is the identity the server vouched for
	} else if !system.Has("xuid") {
		system.Set("xuid", "0")
	}
	if !system.Has("initialize") {
		system.Set("initialize", false)
	}
	system.Set("index", int64(index))
	constants.Sub("custom")
	props := NewDict()
	if p := m.SubOrNil("properties"); p != nil {
		props = p.copyDict()
	}
	psys, _ := props.Sub("system")
	if !psys.Has("active") {
		psys.Set("active", true)
	}
	if !psys.Has("ready") {
		psys.Set("ready", false)
	}
	if !psys.Has("secureDeviceAddress") {
		psys.Set("secureDeviceAddress", "")
	}
	props.Sub("custom")
	gt := any(s.memberName(key))
	if !truthy(gt) {
		gt, _ = m.Get("gamertag")
	}
	if !truthy(gt) {
		gt = "Player 1" // the registered name wins over anything a client sends
	}
	d := NewDict()
	d.Set("constants", constants)
	d.Set("properties", props)
	d.Set("gamertag", gt)
	d.Set("reserved", false)
	d.Set("joinTime", now)
	d.Set("nat", "open")
	d.Set("turn", false)
	d.Set("next", int64(index+1))
	return d
}

func setDefault(d *Dict, k string, v any) {
	if !d.Has(k) {
		d.Set(k, v)
	}
}

// SessionDocument builds the JSON document XSAPI expects from a stored record.
func (s *Server) SessionDocument(rec *Session) *Dict {
	now := rec.Start
	system := NewDict()
	if c := rec.Constants.SubOrNil("system"); c != nil {
		system = c.copyDict()
	}
	setDefault(system, "version", int64(1))
	setDefault(system, "visibility", "open")
	setDefault(system, "maxMembersCount", int64(maxPlayers(rec, 100)))
	setDefault(system, "capabilities", NewDict())
	psys := NewDict()
	if p := rec.Properties.SubOrNil("system"); p != nil {
		psys = p.copyDict()
	}
	setDefault(psys, "joinRestriction", "none")
	setDefault(psys, "readRestriction", "none")
	setDefault(psys, "closed", false)
	setDefault(psys, "locked", false)
	setDefault(psys, "allocateCloudCompute", false)
	members := NewDict()
	keys := rec.Members.Keys()
	sort.Strings(keys)
	for i, key := range keys {
		m, _ := rec.Members.m[key].(*Dict)
		if m == nil {
			m = NewDict()
		}
		members.Set(strconv.Itoa(i), memberDoc(s, i, m, now, key))
	}
	custom := func(d *Dict) any {
		if c := d.SubOrNil("custom"); c != nil {
			return c.copyDict()
		}
		return NewDict()
	}
	cons := NewDict()
	cons.Set("system", system)
	cons.Set("custom", custom(rec.Constants))
	props := NewDict()
	props.Set("system", psys)
	props.Set("custom", custom(rec.Properties))
	info := NewDict()
	info.Set("first", int64(0))
	info.Set("next", int64(members.Len()))
	info.Set("count", int64(members.Len()))
	info.Set("accepted", int64(members.Len()))
	doc := NewDict()
	doc.Set("correlationId", rec.Corr)
	doc.Set("startTime", now)
	doc.Set("branch", rec.Branch)
	doc.Set("changeNumber", rec.Change)
	doc.Set("contractVersion", int64(105))
	doc.Set("constants", cons)
	doc.Set("properties", props)
	doc.Set("membersInfo", info)
	doc.Set("members", members)
	doc.Set("servers", rec.Servers.copyDict())
	return doc
}

// intOrNone is an int from a JSON number or a numeric string ("12" is what the GSDS reply hands the game), else ok=false.
func intOrNone(v any) (int64, bool) {
	switch x := v.(type) {
	case bool:
		return 0, false
	case int64:
		return x, true
	case uint64:
		return int64(x), true
	case int:
		return int64(x), true
	case string:
		t := strings.TrimSpace(x)
		if isDigits(t) {
			n, err := strconv.ParseInt(t, 10, 64)
			if err == nil {
				return n, true
			}
		}
	}
	return 0, false
}

// maxPlayers is constants.custom.gameServerPlatform.maxAllowedPlayers of a record, or def when it is missing or not a sane number.
func maxPlayers(rec *Session, def int) int {
	gsp := rec.Constants.SubOrNil("custom").SubOrNil("gameServerPlatform")
	if gsp == nil {
		return def
	}
	v, _ := gsp.Get("maxAllowedPlayers")
	if n, ok := intOrNone(v); ok && n >= 1 && n <= 100 {
		return int(n)
	}
	return def
}

// shapeProblem says why the request does not have the types the document builder relies on, or "" when it does.
func shapeProblem(req *Dict) string {
	for _, part := range []string{"constants", "properties", "servers"} {
		v, has := req.Get(part)
		if !has {
			continue
		}
		d, ok := v.(*Dict)
		if !ok {
			return fmt.Sprintf("%s is a %s, not an object", part, TypeName(v))
		}
		if part != "servers" {
			for _, sub := range []string{"system", "custom"} {
				if sv, has := d.Get(sub); has {
					if _, ok := sv.(*Dict); !ok {
						return fmt.Sprintf("%s.%s is a %s, not an object", part, sub, TypeName(sv))
					}
				}
			}
		}
	}
	gsp := req.SubOrNil("constants").SubOrNil("custom")
	if gsp != nil {
		if g, has := gsp.Get("gameServerPlatform"); has {
			gd, ok := g.(*Dict)
			if !ok {
				return fmt.Sprintf("constants.custom.gameServerPlatform is a %s, not an object", TypeName(g))
			}
			if mv, has := gd.Get("maxAllowedPlayers"); has {
				if n, ok := intOrNone(mv); !ok || n < 1 || n > 100 {
					return fmt.Sprintf("maxAllowedPlayers %s is not a number from 1 to 100", Repr(mv))
				}
			}
		}
	}
	return ""
}

// Sweep forgets sessions nobody used for SessionIdleSeconds (EmptySessionIdleSeconds without members) and matchmaking tickets older than
// TicketSeconds. Called from the session calls at most once a minute.
func (s *Server) Sweep() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.sweepLocked()
}

func (s *Server) sweepLocked() {
	now := s.Now()
	if now-s.lastSweep < 60.0 {
		return
	}
	s.lastSweep = now
	forgotten, occupied := 0, 0
	for k, rec := range s.sessions {
		used := now
		if rec.HasUsed {
			used = rec.Used
		}
		limit := EmptySessionIdleSeconds
		if rec.Members.Len() > 0 {
			limit = SessionIdleSeconds
		}
		if now-used > limit {
			if rec.Members.Len() > 0 {
				occupied++
			}
			forgotten++
			delete(s.sessions, k)
		}
	}
	old := 0
	for id, t := range s.tickets {
		if now-t.At > TicketSeconds {
			delete(s.tickets, id)
			old++
		}
	}
	if forgotten > 0 || old > 0 {
		msg := fmt.Sprintf("sweep forgot %d idle session(s) (%d still listed members, idle over %s h) and %d ticket(s) older than %s s; %d session(s) remain",
			forgotten, occupied, fmtG(SessionIdleSeconds/3600), old, fmtG(TicketSeconds), len(s.sessions))
		if occupied > 0 {
			mpsdLog.Info(msg)
		} else {
			mpsdLog.Debug(msg)
		}
	}
}

func fmtG(x float64) string { return strconv.FormatFloat(x, 'g', -1, 64) }

func ownMemberKey(name, xuid string) bool { return name == "me" || name == xuid }

func serversBlock() *Dict {
	host := os.Getenv("FH2MP_PUBLIC_HOST")
	if host == "" {
		host = "127.0.0.1"
	}
	custom := NewDict()
	custom.Set("status", "Active")
	custom.Set("datacenter", "North Central US")
	custom.Set("secureDeviceAddress", base64.StdEncoding.EncodeToString([]byte("FH2MP-SDA-0000000")))
	custom.Set("hostName", host)
	props := NewDict()
	props.Set("custom", custom)
	gsp := NewDict()
	gsp.Set("properties", props)
	out := NewDict()
	out.Set("gameServerPlatform", gsp)
	return out
}

// ServersBlock is the allocated game server, as the game reads it: servers.gameServerPlatform.properties.custom.
func ServersBlock() pyjson.Obj { return ToJSON(serversBlock()).(pyjson.Obj) }

// AllocateServer answers a GSMS AllocateCluster for the session the request names (sessionPath is the "sessionId" of the request body): the allocated
// game server goes into that session's servers, the place the game reads it from. After the cluster answer Network::Live::Session polls the session
// and ends the request with "Unable to allocate a game server" when the block never shows up (win1 -> dev 2026-11-08: the private Free Roam the game
// creates itself allocates into its QoS session, template defaultTemplate_qos, not into a lobby session). The change number moves so a poller sees a
// change. It reports false when the path is not a session path or the session is unknown.
func (s *Server) AllocateServer(sessionPath string) bool {
	key, ok := ParsePath(sessionPath)
	if !ok {
		return false
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	rec := s.sessions[lowerKey(key)]
	if rec == nil {
		return false
	}
	rec.Servers = serversBlock()
	rec.Change++
	return true
}

// PutSession creates or updates a session and returns (http status, document or nil). xuid is the authenticated caller; the server's own calls
// leave it "" and are not restricted. A caller may create a session (and becomes its owner), join it, and write, update or remove only its own member
// entry. Only a member or the owner may change the session's properties or servers.
func (s *Server) PutSession(path string, body []byte, xuid string) (int, pyjson.Obj) {
	s.mu.Lock()
	status, doc, announce := s.putSession(path, body, xuid)
	s.mu.Unlock()
	for _, a := range announce {
		s.AnnouncePlayer(a[0], a[1])
	}
	return status, doc
}

// putSession does PutSession's work; the caller holds the lock.
func (s *Server) putSession(path string, body []byte, xuid string) (int, pyjson.Obj, [][2]string) {
	key, ok := ParsePath(path)
	if !ok {
		p, _, _ := strings.Cut(path, "?")
		s.every(mpsdLog, "warn", "refuse/400/path", RefusalLogSeconds, fmt.Sprintf("PUT refused 400: not a session path %s ", pyRepr(trunc(p, 120)))+kv(f("xuid", nilIfEmpty(xuid))))
		return 400, nil, nil
	}
	var reqv any = NewDict()
	if len(body) > 0 {
		v, err := pyjson.Parse(string(body))
		if err != nil {
			st, _ := s.refuse(400, &key, xuid, fmt.Sprintf("body is not JSON (%v)", err), f("size", len(body)))
			return st, nil, nil
		}
		reqv = FromJSON(v)
	}
	req, isDict := reqv.(*Dict)
	if !isDict {
		st, _ := s.refuse(400, &key, xuid, fmt.Sprintf("body is a %s, not an object", TypeName(reqv)))
		return st, nil, nil
	}
	if problem := shapeProblem(req); problem != "" {
		st, _ := s.refuse(400, &key, xuid, problem)
		return st, nil, nil
	}
	membersReq := NewDict()
	if mv, has := req.Get("members"); has && truthy(mv) {
		md, ok := mv.(*Dict)
		if ok {
			for _, n := range md.Keys() {
				if m := md.m[n]; m != nil {
					if _, ok := m.(*Dict); !ok {
						ok = false
						st, _ := s.refuse(400, &key, xuid, "members is not an object of objects")
						return st, nil, nil
					}
				}
			}
			membersReq = md
		} else {
			st, _ := s.refuse(400, &key, xuid, "members is not an object of objects")
			return st, nil, nil
		}
	}
	for _, name := range membersReq.Keys() {
		m, _ := membersReq.m[name].(*Dict)
		if m == nil {
			continue
		}
		for _, part := range []string{"constants", "properties"} {
			if pv, has := m.Get(part); has {
				pd, ok := pv.(*Dict)
				if !ok {
					st, _ := s.refuse(400, &key, xuid, "a member's constants / properties (or their system part) is not an object", f("member", trunc(name, 20)))
					return st, nil, nil
				}
				if sv, has := pd.Get("system"); has {
					if _, ok := sv.(*Dict); !ok {
						st, _ := s.refuse(400, &key, xuid, "a member's constants / properties (or their system part) is not an object", f("member", trunc(name, 20)))
						return st, nil, nil
					}
				}
			}
		}
	}
	s.sweepLocked()
	lk := lowerKey(key)
	cur := s.sessions[lk]
	created := cur == nil
	if xuid != "" {
		// decide everything before changing anything, so a refused request leaves the session as it was
		var foreign []string
		for _, name := range membersReq.Keys() {
			if !ownMemberKey(name, xuid) {
				foreign = append(foreign, name)
			}
		}
		if len(foreign) > 0 {
			var ents []string
			for _, n := range foreign[:min(3, len(foreign))] {
				ents = append(ents, trunc(n, 20))
			}
			st, _ := s.refuse(403, &key, xuid, "writes the member entry of another player", f("entries", strings.Join(ents, ",")))
			return st, nil, nil
		}
		if !created {
			_, isMember := cur.Members.Get(xuid)
			isMember = isMember || cur.Owner == xuid
			joining := false
			for _, name := range membersReq.Keys() {
				if ownMemberKey(name, xuid) && membersReq.m[name] != nil {
					joining = true
				}
			}
			if !isMember && !joining {
				st, _ := s.refuse(403, &key, xuid, "not a member of this session and not joining it", f("owner", nilIfEmpty(cur.Owner)), f("members", cur.Members.Len()))
				return st, nil, nil
			}
			if !isMember && (req.Has("constants") || req.Has("properties") || req.Has("servers")) {
				var parts []string
				for _, p := range []string{"constants", "properties", "servers"} {
					if req.Has(p) {
						parts = append(parts, p)
					}
				}
				st, _ := s.refuse(403, &key, xuid, "joins and rewrites the session in one request", f("parts", strings.Join(parts, ",")))
				return st, nil, nil
			}
		} else {
			owned := 0
			for _, r := range s.sessions {
				if r.Owner == xuid {
					owned++
				}
			}
			if owned >= MaxSessionsPerOwner {
				st, _ := s.refuse(429, &key, xuid, "owns too many live sessions", f("limit", MaxSessionsPerOwner))
				return st, nil, nil
			}
		}
	}
	if created && len(s.sessions) >= s.MaxSessions {
		st, _ := s.refuse(503, &key, xuid, "session table is full", f("sessions", len(s.sessions)), f("limit", s.MaxSessions))
		return st, nil, nil
	}
	var rec *Session
	if created {
		rec = &Session{Scid: key[0], Template: key[1], Name: key[2], Corr: s.NewUUID(), Branch: s.NewUUID(), Start: s.nowISO(), Owner: xuid,
			Constants: NewDict(), Properties: NewDict(), Members: NewDict(), Servers: NewDict()}
	} else {
		rec = cur.clone() // the update is built on a copy and only stored when it is valid
	}
	if c, ok := req.Get("constants"); ok {
		Merge(rec.Constants, c.(*Dict))
	}
	if p, ok := req.Get("properties"); ok {
		Merge(rec.Properties, p.(*Dict))
	}
	qos := strings.Contains(strings.ToLower(key[1]), "qos")
	for _, name := range membersReq.Keys() {
		m, _ := membersReq.m[name].(*Dict)
		if name == "me" && xuid != "" { // "me" is the caller: key members by xuid so several clients can share a session
			name = xuid
		}
		if m == nil {
			if _, had := rec.Members.Delete(name); had && xuid != "" && name == xuid && !qos {
				s.left[xuid] = left{strings.ToLower(key[2]), s.Now()}
			}
			continue
		}
		m = m.copyDict()
		if xuid != "" {
			// the advertised identity comes from the authenticated caller, for "me" and for an explicit own entry alike
			c, _ := m.Sub("constants")
			sys, _ := c.Sub("system")
			sys.Set("xuid", xuid)
		}
		dst, ok := rec.Members.Sub(name)
		if !ok {
			dst = NewDict()
			rec.Members.Set(name, dst)
		}
		Merge(dst, m)
	}
	rec.Change++
	rec.Used, rec.HasUsed = s.Now(), true
	s.touch(rec, xuid)
	if !qos && rec.Servers.Len() == 0 {
		// A game-mode session the game created itself (Car Meet -> Free Roam / Road Trip): it asks the game-server platform for a server and
		// reads the address from the session, so list ours; and put its members into this session's world.
		rec.Servers = serversBlock()
	}
	doc := s.SessionDocument(rec)
	text, _ := pyjson.Dumps(rec.obj(), pyjson.Opts{})
	if size := len([]rune(text)); size > MaxSessionBytes {
		st, _ := s.refuse(413, &key, xuid, "stored record too large", f("size", size), f("limit", MaxSessionBytes))
		return st, nil, nil
	}
	s.sessions[lk] = rec
	var announce [][2]string
	if !qos {
		for _, mk := range rec.Members.Keys() {
			if isDigits(mk) && mk == xuid {
				announce = append(announce, [2]string{mk, key[2]})
			}
		}
	}
	// The real service answers 201 for a new session; XSAPI's REST layer in this build returns a null session for it, so create and update
	// both answer 200.
	return 200, ToJSON(doc).(pyjson.Obj), announce
}

func (r *Session) clone() *Session {
	n := *r
	n.Constants, n.Properties, n.Members, n.Servers = r.Constants.copyDict(), r.Properties.copyDict(), r.Members.copyDict(), r.Servers.copyDict()
	if r.Seen != nil {
		n.Seen = map[string]float64{}
		for k, v := range r.Seen {
			n.Seen[k] = v
		}
	}
	return &n
}

func pyRepr(s string) string { return pyjson.PyRepr(s) }

func (s *Server) touch(rec *Session, xuid string) {
	if xuid != "" && rec.Members.Has(xuid) {
		if rec.Seen == nil {
			rec.Seen = map[string]float64{}
		}
		rec.HasSeen = true
		rec.Seen[xuid] = s.Now()
	}
}

func isActive(m any) bool {
	d, _ := m.(*Dict)
	sys := d.SubOrNil("properties").SubOrNil("system")
	if sys == nil {
		return false
	}
	v, _ := sys.Get("active")
	return truthy(v)
}

// prune forgets members that have not touched the session for a while (so a crashed client does not hold a seat).
func (s *Server) prune(rec *Session) {
	now := s.Now()
	rec.HasSeen = true
	if rec.Seen == nil {
		rec.Seen = map[string]float64{}
	}
	for _, k := range rec.Members.Keys() {
		limit := MemberIdleSeconds
		if isActive(rec.Members.m[k]) {
			limit = ActiveMemberIdleSeconds
		}
		seen, has := rec.Seen[k]
		if !has {
			seen = now
		}
		if isDigits(k) && now-seen > limit {
			m, _ := rec.Members.Delete(k)
			wasActive := isActive(m)
			rec.Change++
			mpsdLog.Info("member removed after no contact " + kv(f("xuid", k), f("session", trunc(rec.Name, 8)), f("idle_s", int(now-seen)), f("limit_s", int(limit)), f("active", wasActive)))
		}
	}
}

// GetSession reads a session: (status, document or nil).
func (s *Server) GetSession(path, xuid string) (int, pyjson.Obj) {
	key, ok := ParsePath(path)
	if !ok {
		return 404, nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.sweepLocked()
	rec := s.sessions[lowerKey(key)]
	if rec == nil {
		return 404, nil
	}
	rec.Used, rec.HasUsed = s.Now(), true
	s.touch(rec, xuid)
	s.prune(rec)
	return 200, ToJSON(s.SessionDocument(rec)).(pyjson.Obj)
}

// DeleteSession returns the http status. The owner deletes the session; another member of it only leaves; anybody else is refused. The server's own
// calls (xuid "") are not restricted.
func (s *Server) DeleteSession(path, xuid string) int {
	key, ok := ParsePath(path)
	if !ok {
		return 204
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	lk := lowerKey(key)
	rec := s.sessions[lk]
	if rec == nil {
		return 204
	}
	if xuid != "" && rec.Owner != xuid {
		if !rec.Members.Has(xuid) {
			s.every(mpsdLog, "warn", "refuse/delete/"+xuid, RefusalLogSeconds, "DELETE refused 403: neither the owner nor a member "+kv(f("xuid", xuid), f("session", trunc(key[2], 8)), f("owner", nilIfEmpty(rec.Owner))))
			return 403
		}
		rec.Members.Delete(xuid)
		rec.Change++
		return 204
	}
	delete(s.sessions, lk)
	return 204
}

// ETag is the strong ETag for a session document (the real service sends one with every session).
func ETag(doc pyjson.Obj) string {
	v, _ := doc.Get("changeNumber")
	if v == nil {
		v = int64(0)
	}
	return `"` + pyjson.PyStr(v) + `"`
}

// Count is the number of live sessions.
func (s *Server) Count() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.sessions)
}

// ForTunnel edits a session document in place as a client sees it when its game talks to the launcher's local UDP-over-TCP proxy: the game server is
// on 127.0.0.1.
func ForTunnel(doc pyjson.Obj) pyjson.Obj {
	get := func(o pyjson.Obj, k string) pyjson.Obj { v, _ := o.Get(k); r, _ := v.(pyjson.Obj); return r }
	custom := get(get(get(get(doc, "servers"), "gameServerPlatform"), "properties"), "custom")
	if custom != nil {
		if _, ok := custom.Get("hostName"); ok {
			custom.Set("hostName", "127.0.0.1")
		}
	}
	return doc
}

// ---------------------------------------------------------------- hub control datagrams

// AnnouncePlayer tells the game server which lobby (= world) this player was matched into.
func (s *Server) AnnouncePlayer(xuid, lobby string) {
	if xuid == "" {
		return
	}
	n, err := strconv.ParseInt(xuid, 10, 64)
	if err != nil {
		s.every(hubLog, "warn", "send-assign", ControlLogSeconds, fmt.Sprintf("could not tell the game server that xuid=%s was matched into lobby %s, the player will not be placed there: ValueError: %v", xuid, trunc(lobby, 8), err))
		return
	}
	s.Hub.Control(fmt.Sprintf("assign %x %s", n, lobby))
}

// AnnounceRun tells the game server the run id of the game this player started: it writes it on its lines about them.
func (s *Server) AnnounceRun(xuid int64, run string) {
	s.Hub.Control(fmt.Sprintf("run %x %s", xuid, run))
}

// AnnounceAuth tells the game server that xuid is signed in from ip (the UDP OpenConnection must come from there) with a session whose UDP ticket
// key is key (hex).
func (s *Server) AnnounceAuth(xuid int64, ip, key string) {
	if xuid == 0 || ip == "" {
		return
	}
	if key != "" {
		s.Hub.Control(fmt.Sprintf("auth %x %s %s", xuid, ip, key))
	} else {
		s.Hub.Control(fmt.Sprintf("auth %x %s", xuid, ip))
	}
}

// AnnounceLogout: a session signed out; the game server forgets its ticket key and ends the connections that key opened (not a ban).
func (s *Server) AnnounceLogout(xuid int64, key string) {
	if xuid != 0 && key != "" {
		s.Hub.Control(fmt.Sprintf("logout %x %s", xuid, key))
	}
}

// AnnounceStandins: the stock cars a car-pack player's custom cars are shown as ("<custom>:<candidate>/...,...", checked by the caller); "-"
// forgets the player's list (its game runs the server's database).
func (s *Server) AnnounceStandins(xuid int64, field string) {
	if xuid != 0 && field != "" {
		s.Hub.Control(fmt.Sprintf("standins %x %s", xuid, field))
	}
}

// AnnounceAllow: a fresh sign-in lifts an earlier revoke of this player on the game server.
func (s *Server) AnnounceAllow(xuid int64) {
	if xuid != 0 {
		s.Hub.Control(fmt.Sprintf("allow %x", xuid))
	}
}

// AnnounceRevoke: ban or kick; the game server forgets the sign-in and ends the player's live connections.
func (s *Server) AnnounceRevoke(xuid int64) {
	if xuid != 0 {
		s.Hub.Control(fmt.Sprintf("revoke %x", xuid))
	}
}

// QuarantineTools flags the authenticated sender and waits for the hub before matchmaking can run; "" when the hub does not answer.
func (s *Server) QuarantineTools(xuid int64, tools []string) string {
	set := map[string]bool{}
	for _, t := range tools {
		set[t] = true
	}
	var list []string
	for t := range set {
		list = append(list, t)
	}
	sort.Strings(list)
	if xuid == 0 || len(list) == 0 {
		return ""
	}
	for _, t := range list {
		if t != "redline" && t != "nitrosystem" {
			return ""
		}
	}
	reply := s.Hub.Tools(fmt.Sprintf("tools %x %s", xuid, strings.Join(list, ",")))
	if reply != "ok" && reply != "rejoin" {
		return ""
	}
	return reply
}

// DropPlayer takes a player out of every session and ticket (ban / kick).
func (s *Server) DropPlayer(xuid string) {
	s.CancelHold(xuid)
	s.mu.Lock()
	defer s.mu.Unlock()
	seats, sessions := 0, 0
	for _, res := range s.reserved {
		if _, ok := res[xuid]; ok {
			delete(res, xuid)
			seats++
		}
	}
	for _, rec := range s.sessions {
		if _, ok := rec.Members.Delete(xuid); ok {
			rec.Change++
			sessions++
		}
	}
	n := 0
	for id, t := range s.tickets {
		if t.Xuid == xuid {
			delete(s.tickets, id)
			n++
		}
	}
	if seats > 0 || sessions > 0 || n > 0 {
		matchLog.Info("player dropped " + kv(f("xuid", xuid), f("sessions", sessions), f("tickets", n), f("reserved_seats", seats)))
	}
}

// ParseMomatch splits a matchmaking path into (scid, hopper, ticket id).
func ParseMomatch(path string) (scid, hopper, id string, ok bool) {
	p, _, _ := strings.Cut(path, "?")
	m := momatchPath.FindStringSubmatch(p)
	if m == nil {
		return "", "", "", false
	}
	return m[1], m[2], m[3], true
}

// LobbyKeyOf: players share a lobby when they asked for the same hopper, game-server image set and location.
func LobbyKeyOf(scid, hopper string, attrs *Dict) LobbyKey {
	str := func(k string) string {
		if attrs == nil {
			return ""
		}
		v, ok := attrs.Get(k)
		if !ok {
			return ""
		}
		return pyjson.PyStr(ToJSON(v))
	}
	return LobbyKey{scid, hopper, str("gsiSetId"), str("Location")}
}

// ---------------------------------------------------------------- population queries

// Populations asks the hub which players it has in each lobby; ok=false when it does not answer (the caller then counts MPSD members).
func (s *Server) Populations() (map[string][]string, bool) {
	s.mu.Lock()
	down := s.Monotonic() < s.popDownUntil
	s.mu.Unlock()
	if down {
		return nil, false
	}
	pops, ok := s.Hub.Populations()
	s.mu.Lock()
	defer s.mu.Unlock()
	if !ok {
		s.popDownUntil = s.Monotonic() + 5.0
		if !s.hubWasDown {
			s.hubWasDown, s.hubDownSince = true, s.Monotonic()
			hubLog.Warn("game server did not answer the population query within 0.3 s; lobbies are filled by counting MPSD members and the kicked-player pool is not enforced until it answers")
		}
		return nil, false
	}
	if s.hubWasDown {
		hubLog.Info(fmt.Sprintf("game server answers the population query again after %.0f s", s.Monotonic()-s.hubDownSince))
		s.hubWasDown = false
	}
	return pops, true
}

func lstrip0(s string) string { return strings.TrimLeft(s, "0") }

// lobbyOccupancy counts the players in a lobby, including the ones matched into it that are still on their way. With the game server's report that
// is who is connected plus the recent matches it has not seen yet; without it, the session's members.
func (s *Server) lobbyOccupancy(name string, rec *Session, pops map[string][]string, havePops bool) int {
	now := s.Now()
	if !havePops {
		members := map[string]bool{}
		for _, k := range rec.Members.Keys() {
			if isDigits(k) {
				members[k] = true
			}
		}
		for x := range s.seatsLocked(name, nil, now) {
			members[x] = true
		}
		return len(members)
	}
	present := map[string]bool{}
	for _, x := range pops[name] {
		present[lstrip0(x)] = true
	}
	res := s.reserved[name]
	for x, t := range res {
		n, _ := strconv.ParseInt(x, 10, 64)
		if now-t > ReserveSeconds || present[lstrip0(fmt.Sprintf("%x", n))] {
			delete(res, x) // it connected (the report counts it from now on) or never came
		}
	}
	held := s.seatsLocked(name, nil, now)
	presentDec := map[string]bool{}
	for _, x := range pops[name] {
		if x != "" {
			n, _ := strconv.ParseInt(x, 16, 64)
			presentDec[strconv.FormatInt(n, 10)] = true
		}
	}
	extra := map[string]bool{}
	for x := range res {
		extra[x] = true
	}
	for x := range held {
		extra[x] = true
	}
	n := len(pops[name])
	for x := range extra {
		if !presentDec[x] {
			n++
		}
	}
	return n
}

// FlaggedSet is the kicked players the game server reports (its "!flagged" entry); empty when it is not reachable.
func (s *Server) FlaggedSet(pops map[string][]string) map[int64]bool {
	out := map[int64]bool{}
	for _, x := range pops["!flagged"] {
		n, err := strconv.ParseInt(x, 16, 64)
		if err != nil {
			s.every(hubLog, "warn", "flagged-entry", ControlLogSeconds, fmt.Sprintf("the game server's !flagged list has an entry that is not hex: %s, ignored", pyRepr(trunc(x, 20))))
			continue
		}
		out[n] = true
	}
	return out
}

// poolFits says whether a player may be matched into this lobby: a flagged player only into a lobby whose players are all flagged, anybody else
// only into one with no flagged player in it. Players still on their way count. An empty lobby fits both; the first to be matched into it decides.
func (s *Server) poolFits(name string, pops map[string][]string, flagged map[int64]bool, mine bool) bool {
	members := map[int64]bool{}
	for _, x := range pops[name] {
		if x != "" {
			n, _ := strconv.ParseInt(x, 16, 64)
			members[n] = true
		}
	}
	for x := range s.reserved[name] {
		if isDigits(x) {
			n, _ := strconv.ParseInt(x, 10, 64)
			members[n] = true
		}
	}
	for x := range s.seatsLocked(name, nil, s.Now()) {
		n, _ := strconv.ParseInt(x, 10, 64)
		members[n] = true
	}
	delete(members, 0)
	for m := range members {
		if mine && !flagged[m] {
			return false
		}
		if !mine && flagged[m] {
			return false
		}
	}
	return true
}

// avoided are the lobbies a ticket of this player must not be matched into: the one the game server has it connected to (it is searching from
// inside a session, "find a new session"), and the one it left through MPSD within LeftAvoidSeconds. Offered either, the game leaves that session
// and then tries to join it as the "new" one.
func (s *Server) avoided(xuid string, pops map[string][]string) map[string]bool {
	out := map[string]bool{}
	if xuid == "" || !isDigits(xuid) {
		return out
	}
	n, _ := strconv.ParseInt(xuid, 10, 64)
	me := fmt.Sprintf("%x", n)
	for name, xs := range pops {
		if strings.HasPrefix(name, "!") {
			continue
		}
		for _, x := range xs {
			if lstrip0(x) == me {
				out[strings.ToLower(name)] = true
				break
			}
		}
	}
	if l, ok := s.left[xuid]; ok {
		if s.Now()-l.at <= LeftAvoidSeconds {
			out[l.name] = true
		} else {
			delete(s.left, xuid)
		}
	}
	return out
}

// heldSeatVsAvoid: a seat the player reserved on purpose (launcher "join a friend") must not be thrown away by avoided. Prod 2026-11-06 22:20-22:22 UTC:
// the game left the friend's lobby, searched again a second later, and the held seat was used up and dropped by the left-lobby rule, twice (the second
// time 100 s after the leave): the player landed in a random lobby. Now: still connected to the held lobby, or left it less than HeldRejoinSeconds ago
// (the immediate re-join that stalled the client on dev 2026-11-01) -> the hold stays unused and this ticket gets another lobby; an older leave -> the
// held seat wins and the lobby is taken off avoid. Returns true when the hold must stay unused for this ticket. Called under the matchmaking lock.
func (s *Server) heldSeatVsAvoid(xuid string, avoid map[string]bool, pops map[string][]string) bool {
	if xuid == "" || !isDigits(xuid) {
		return false
	}
	s.pruneHolds(s.Now())
	h := s.holds[xuid]
	if h == nil || !avoid[strings.ToLower(h.ID)] {
		return false
	}
	name := strings.ToLower(h.ID)
	n, _ := strconv.ParseInt(xuid, 10, 64)
	me := fmt.Sprintf("%x", n)
	inside := false
	for pn, xs := range pops {
		if strings.ToLower(pn) != name {
			continue
		}
		for _, x := range xs {
			if lstrip0(x) == me {
				inside = true
			}
		}
	}
	var leftS any
	age, hasAge := 0.0, false
	if l, ok := s.left[xuid]; ok && l.name == name {
		age, hasAge = s.Now()-l.at, true
		leftS = math.Round(age*10) / 10
	}
	if inside || (hasAge && age < HeldRejoinSeconds) {
		why := "it left that lobby a moment ago"
		if inside {
			why = "the player is connected to it"
		}
		s.pickLog("held-wait/"+xuid, "reservation kept for a later ticket: this ticket gets another lobby "+kv(f("xuid", xuid), f("lobby", trunc(name, 8)), f("why", why), f("left_s", leftS), f("rejoin_after_s", HeldRejoinSeconds)))
		return true
	}
	delete(avoid, name)
	s.pickLog("held-beats-left/"+xuid, "reservation beats the left-lobby rule "+kv(f("xuid", xuid), f("lobby", trunc(name, 8)), f("left_s", leftS)))
	return false
}

func (s *Server) lookup(scid, name string) *Session {
	return s.sessions[[3]string{strings.ToLower(scid), strings.ToLower(TargetTemplate), strings.ToLower(name)}]
}

func (s *Server) refSession(ref *Dict) *Session {
	if ref == nil {
		return nil
	}
	str := func(k string) string {
		v, ok := ref.Get(k)
		if !ok {
			return ""
		}
		return strings.ToLower(pyjson.PyStr(ToJSON(v)))
	}
	return s.sessions[[3]string{str("scid"), str("templateName"), str("name")}]
}

// lobbyFor is the lobby a ticket is matched into: of the hopper's lobbies the one with the most players that still has room, so that players arriving
// one after another fill lobbies up (12, 12, 1) instead of leaving every lobby partly empty (8, 10, 6, 1). Players leave lobbies that were filled
// earlier, so the newest lobby is not the only one with seats.
func (s *Server) lobbyFor(scid, hopper string, attrs *Dict, ref *Dict, xuid string) (string, error) {
	key := LobbyKeyOf(scid, hopper, attrs)
	pops, havePops := s.Populations() // up to 0.3 s when the game server is busy: tickets must not queue behind it
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.lobbies[key]; !ok {
		s.lobbies[key] = nil
		s.lobbyKeys = append(s.lobbyKeys, key)
	}
	names := s.lobbies[key]
	avoid := s.avoided(xuid, pops)
	var preferred string
	if xuid != "" && !s.heldSeatVsAvoid(xuid, avoid, pops) {
		var err error
		if preferred, err = s.pick(xuid, key, attrs, ref, pops, havePops); err != nil {
			return "", err
		}
	}
	names = s.lobbies[key] // pick may have added the held lobby
	if preferred != "" && avoid[strings.ToLower(preferred)] {
		preferred = ""
	}
	lobbyMax := capacity.ForHopper(hopper)
	best, bestN := preferred, -1
	if preferred != "" {
		bestN = lobbyMax
	}
	flagged := s.FlaggedSet(pops)
	mine := false
	if xuid != "" && isDigits(xuid) {
		n, _ := strconv.ParseInt(xuid, 10, 64)
		mine = flagged[n]
	}
	full, pooled, avoidedN := 0, 0, 0
	for _, name := range append([]string(nil), names...) {
		rec := s.lookup(scid, name)
		if rec == nil {
			names = removeName(names, name)
			delete(s.reserved, name)
			continue
		}
		if avoid[strings.ToLower(name)] {
			avoidedN++
			continue
		}
		s.prune(rec)
		cp := min(maxPlayers(rec, lobbyMax), lobbyMax)
		n := s.lobbyOccupancy(name, rec, pops, havePops)
		if len(flagged) > 0 && !s.poolFits(name, pops, flagged, mine) {
			pooled++
			continue
		}
		if n >= cp {
			full++
		}
		if n < cp && n > bestN {
			best, bestN = name, n
		}
	}
	name := best
	created := name == ""
	if created {
		name = s.newLobby(scid, attrs, ref, "", hopper)
		names = append(names, name)
	}
	s.lobbies[key] = names
	if rec := s.lookup(scid, name); rec != nil {
		rec.Used, rec.HasUsed = s.Now(), true
	}
	released := 0
	oldNames := make([]string, 0, len(avoid))
	for old := range avoid {
		oldNames = append(oldNames, old)
	}
	sort.Strings(oldNames)
	for _, old := range oldNames { // the lobby it searched from / left: its MPSD seat goes now, not when its game gets round to it
		oldRec := s.lookup(scid, old)
		if oldRec != nil && old != strings.ToLower(name) {
			if _, ok := oldRec.Members.Delete(xuid); ok {
				oldRec.Change++
				released++
			}
		}
	}
	if xuid != "" {
		for _, res := range s.reserved {
			delete(res, xuid) // one seat per player: a new match replaces the one it made before
		}
		if s.reserved[name] == nil {
			s.reserved[name] = map[string]float64{}
		}
		s.reserved[name][xuid] = s.Now()
	}
	how := fmt.Sprintf("fullest with room (%d of %d)", bestN, lobbyMax)
	switch {
	case created:
		how = "new lobby"
	case name == preferred:
		how = "held seat"
	}
	loc, _ := attrs.Get("Location")
	gsi := lastN(pyjson.PyStr(ToJSON(getOr(attrs, "gsiSetId", ""))), 3)
	src := "MPSD members (game server silent)"
	if havePops {
		src = "game server"
	}
	var mineFlag any
	if mine {
		mineFlag = true
	}
	matchLog.Info("ticket matched " + kv(f("xuid", nilIfEmpty(xuid)), f("lobby", trunc(name, 8)), f("how", how), f("hopper", hopper), f("location", ToJSON(loc)), f("gsi", nilIfEmpty(gsi)),
		f("lobbies", len(names)), f("full", nz(full)), f("pool_skipped", nz(pooled)), f("left_or_inside_skipped", nz(avoidedN)), f("released", nz(released)), f("flagged_player", mineFlag),
		f("occupancy_source", src)))
	return name, nil
}

func lastN(s string, n int) string {
	r := []rune(s)
	if len(r) > n {
		return string(r[len(r)-n:])
	}
	return s
}

func nz(n int) any {
	if n == 0 {
		return nil
	}
	return n
}

func getOr(d *Dict, k string, def any) any {
	if d == nil {
		return def
	}
	if v, ok := d.Get(k); ok {
		return v
	}
	return def
}

func removeName(l []string, name string) []string {
	for i, x := range l {
		if x == name {
			return append(l[:i:i], l[i+1:]...)
		}
	}
	return l
}

// lobbyCustom is constants.custom of a new lobby. Only what the game reads, each checked: one player's ticket session seeds the document every later
// joiner parses, and that parser throws on a bad key. The lobby size is the server's.
func (s *Server) lobbyCustom(ticket *Dict, attrs *Dict, maxPlayers int) *Dict {
	guid := func(v any, def string) string {
		if str, ok := v.(string); ok && guidRe.MatchString(str) {
			return str
		}
		return def
	}
	gsp := ticket.SubOrNil("gameServerPlatform")
	get := func(k string) any {
		if gsp == nil {
			return nil
		}
		v, _ := gsp.Get(k)
		return v
	}
	out := NewDict()
	out.Set("gameVariantId", guid(get("gameVariantId"), "00000000-0000-0000-0000-000000000001"))
	attrGsi, _ := attrs.Get("gsiSetId")
	out.Set("gsiSetId", guid(get("gsiSetId"), guid(attrGsi, "00000000-0000-0000-0000-000000000102")))
	if maxPlayers == 0 {
		maxPlayers = s.LobbyCapacity
	}
	out.Set("maxAllowedPlayers", int64(maxPlayers))
	switch loc := get("location").(type) {
	case string:
		if len([]rune(loc)) <= 64 {
			out.Set("location", loc)
		}
	case int64:
		out.Set("location", loc)
	}
	custom := NewDict()
	custom.Set("gameServerPlatform", out)
	hv := any(nil)
	if ticket != nil {
		hv, _ = ticket.Get("hopperId")
	}
	if h, ok := hv.(int64); ok {
		custom.Set("hopperId", h)
	} else {
		custom.Set("hopperId", getOr(attrs, "hopperId", int64(0)))
	}
	if ticket != nil {
		if b, ok := getOr(ticket, "EnableFullArbitration", nil).(bool); ok {
			custom.Set("EnableFullArbitration", b)
		}
	}
	return custom
}

// newLobby creates a lobby session on the server's behalf and returns its name.
func (s *Server) newLobby(scid string, attrs *Dict, ref *Dict, name, hopper string) string {
	if name == "" {
		name = s.NewUUID()
	}
	// The game reads constants.custom.gameServerPlatform.{gameVariantId,gsiSetId[,location]} from the target session and throws on a missing key, so
	// take them from the ticket session it wrote itself.
	src := s.refSession(ref)
	var ticketCustom *Dict
	if src != nil {
		ticketCustom = src.Constants.SubOrNil("custom")
	}
	mp := 0
	if hopper != "" {
		mp = capacity.ForHopper(hopper)
	}
	custom := s.lobbyCustom(ticketCustom, attrs, mp)
	sys := NewDict()
	sys.Set("version", int64(1))
	sys.Set("visibility", "open")
	cons := NewDict()
	cons.Set("system", sys)
	cons.Set("custom", custom)
	psys := NewDict()
	psys.Set("joinRestriction", "none")
	psys.Set("readRestriction", "none")
	props := NewDict()
	props.Set("system", psys)
	props.Set("custom", NewDict())
	body := NewDict()
	body.Set("constants", cons)
	body.Set("properties", props)
	text, _ := pyjson.Dumps(ToJSON(body), pyjson.Opts{})
	s.putSession(fmt.Sprintf("/serviceconfigs/%s/sessionTemplates/%s/sessions/%s", scid, TargetTemplate, name), []byte(text), "")
	// The allocated game server, as the game reads it: servers.gameServerPlatform.properties.custom.
	if rec := s.lookup(scid, name); rec != nil {
		rec.Servers = serversBlock()
	}
	return name
}

// CreateTicket answers a matchmaking POST: (status, body or nil).
func (s *Server) CreateTicket(path string, body []byte, xuid string) (int, pyjson.Obj) {
	scid, hopper, _, ok := ParseMomatch(path)
	if !ok {
		p, _, _ := strings.Cut(path, "?")
		s.every(matchLog, "warn", "ticket-refuse/path", RefusalLogSeconds, fmt.Sprintf("ticket refused 400: not a hopper path %s ", pyRepr(trunc(p, 120)))+kv(f("xuid", nilIfEmpty(xuid))))
		return 400, nil
	}
	var reqv any = NewDict()
	if len(body) > 0 {
		v, err := pyjson.Parse(string(body))
		if err != nil {
			s.every(matchLog, "warn", "ticket-refuse/json/"+xuid, RefusalLogSeconds, fmt.Sprintf("ticket refused 400: body is not JSON (%v) ", err)+kv(f("xuid", nilIfEmpty(xuid)), f("hopper", hopper), f("size", len(body))))
			return 400, nil
		}
		reqv = FromJSON(v)
	}
	req, ok := reqv.(*Dict)
	if !ok {
		return 400, nil // Python would raise on .get of a non-object; the LSP answers 400/500 either way
	}
	refv, hasRef := req.Get("ticketSessionRef")
	s.mu.Lock()
	defer s.mu.Unlock()
	if xuid != "" && truthy(refv) {
		// the ticket may only name a session the caller is in: it is the caller's own request to be matched
		refd, isDict := refv.(*Dict)
		var src *Session
		if isDict {
			src = s.refSession(refd)
		}
		if src == nil || (!src.Members.Has(xuid) && src.Owner != xuid) {
			why := "the caller is neither a member nor the owner of the ticket's session"
			if !isDict {
				why = "ticketSessionRef is not an object"
			} else if src == nil {
				why = "the ticket's session does not exist on this server (restart, or expired)"
			}
			var sess, owner any
			if isDict {
				sess = trunc(pyjson.PyStr(ToJSON(getOr(refd, "name", ""))), 8)
			}
			if src != nil {
				owner = nilIfEmpty(src.Owner)
			}
			s.every(matchLog, "warn", "ticket-refuse/"+why+"/"+xuid, RefusalLogSeconds, "ticket refused 403: "+why+" "+kv(f("xuid", nilIfEmpty(xuid)), f("hopper", hopper), f("session", sess), f("owner", owner)))
			return 403, nil
		}
	}
	attrs := NewDict()
	av, hasAttrs := req.Get("ticketAttributes")
	if hasAttrs && truthy(av) {
		ad, ok := av.(*Dict)
		if !ok {
			s.every(matchLog, "warn", "ticket-refuse/shape/"+xuid, RefusalLogSeconds, "ticket refused 400: ticketAttributes is not an object "+kv(f("xuid", nilIfEmpty(xuid)), f("hopper", hopper)))
			return 400, nil
		}
		attrs = ad
	}
	var ref *Dict
	if hasRef && refv != nil {
		rd, ok := refv.(*Dict)
		if !ok {
			s.every(matchLog, "warn", "ticket-refuse/shape/"+xuid, RefusalLogSeconds, "ticket refused 400: ticketSessionRef is not an object "+kv(f("xuid", nilIfEmpty(xuid)), f("hopper", hopper)))
			return 400, nil
		}
		ref = rd
	}
	s.sweepLocked()
	if xuid != "" {
		type mineT struct {
			at float64
			id string
		}
		var mine []mineT
		for id, t := range s.tickets {
			if t.Xuid == xuid {
				mine = append(mine, mineT{t.At, id})
			}
		}
		sort.Slice(mine, func(i, j int) bool {
			if mine[i].at != mine[j].at {
				return mine[i].at < mine[j].at
			}
			return mine[i].id < mine[j].id
		})
		for _, m := range mine[:min(len(mine), max(0, len(mine)-MaxTicketsPerPlayer+1))] {
			delete(s.tickets, m.id) // the caller's own oldest tickets make room; nobody else's
		}
	}
	tid := s.NewUUID()
	var preserve any = "never"
	if pv, ok := req.Get("preserveSession"); ok {
		preserve = pv
	}
	s.tickets[tid] = &Ticket{Scid: scid, Hopper: hopper, Ref: ref, Attrs: attrs, Preserve: preserve, Xuid: xuid, At: s.Now()}
	// The game polls a ticket every 5000 ms whatever waitTime says; it is telemetry only.
	return 200, pyjson.Obj{{K: "ticketId", V: tid}, {K: "waitTime", V: int64(0)}}
}

// GetTicket answers a matchmaking GET: (status, body or nil).
func (s *Server) GetTicket(path, xuid string) (int, pyjson.Obj) {
	_, _, id, ok := ParseMomatch(path)
	s.mu.Lock()
	t := s.tickets[id]
	if !ok {
		t = nil
	}
	if t == nil || (xuid != "" && t.Xuid != "" && t.Xuid != xuid) {
		s.mu.Unlock()
		return 404, nil
	}
	name := t.Lobby
	needMatch := name == "" || s.lookup(t.Scid, name) == nil
	scid, hopper, attrs, ref, txuid := t.Scid, t.Hopper, t.Attrs, t.Ref, t.Xuid
	s.mu.Unlock()
	// A ticket keeps the lobby it was first matched into: the game polls it every 5 s and the lobby it is sent to must not change
	if needMatch {
		n, err := s.lobbyFor(scid, hopper, attrs, ref, txuid)
		if err != nil {
			if ae, ok := err.(*auth.Error); ok {
				s.every(matchLog, "warn", fmt.Sprintf("ticket-auth/%s/%s", ae.Code, txuid), RefusalLogSeconds,
					fmt.Sprintf("ticket not matched, answered %d %s: %s ", ae.Status, ae.Code, ae.Message)+kv(f("xuid", nilIfEmpty(txuid)), f("hopper", hopper)))
				return ae.Status, pyjson.Obj{{K: "error", V: ae.Code}, {K: "message", V: ae.Message}}
			}
			return 500, nil
		}
		name = n
		s.mu.Lock()
		if cur := s.tickets[id]; cur != nil {
			cur.Lobby = name
		}
		s.mu.Unlock()
	}
	s.mu.Lock()
	// The matchmaker puts the ticket session's members into the target session; the game checks that it is in there.
	src := s.refSession(ref)
	dst := s.lookup(scid, name)
	if src != nil && dst != nil {
		for _, k := range src.Members.Keys() {
			m, _ := src.Members.m[k].(*Dict)
			if m != nil && (txuid == "" || k == txuid) { // a ticket matches its creator, not whoever else sits in the session
				if !dst.Members.Has(k) {
					mm := m.copyDict()
					// matched members start inactive; the game then PUTs properties.system.active = true
					p, _ := mm.Sub("properties")
					sys, _ := p.Sub("system")
					sys.Set("active", false)
					dst.Members.Set(k, mm)
					dst.Change++
					s.touch(dst, k)
				}
			}
		}
	}
	// the momatch calls carry no caller identity, but the ticket session's member is the player that asked
	var who []string
	if src != nil {
		for _, k := range src.Members.Keys() {
			if isDigits(k) && (txuid == "" || k == txuid) {
				who = append(who, k)
			}
		}
	}
	sort.Strings(who)
	if len(who) == 0 {
		who = []string{txuid}
	}
	var refOut any
	if ref != nil {
		refOut = ToJSON(ref)
	}
	preserve := t.Preserve
	s.mu.Unlock()
	for _, k := range who {
		s.AnnouncePlayer(k, name)
	}
	return 200, pyjson.Obj{{K: "ticketStatus", V: "Found"}, {K: "waitTime", V: int64(0)}, {K: "preserveSession", V: ToJSON(preserve)},
		{K: "ticketSessionRef", V: refOut},
		{K: "targetSessionRef", V: pyjson.Obj{{K: "scid", V: scid}, {K: "templateName", V: TargetTemplate}, {K: "name", V: name}}},
		{K: "ticketAttributes", V: ToJSON(attrs)}}
}

// DeleteTicket removes a ticket; always 204.
func (s *Server) DeleteTicket(path, xuid string) int {
	_, _, id, ok := ParseMomatch(path)
	s.mu.Lock()
	defer s.mu.Unlock()
	if ok && id != "" {
		if t := s.tickets[id]; t != nil && !(xuid != "" && t.Xuid != "" && t.Xuid != xuid) {
			delete(s.tickets, id)
		}
	}
	return 204
}

// CreateGameSession makes a Free Roam (mode 1) or Road Trip (mode 2) session on the server's request (Car Meet game-mode invite). Same shape as a
// matched lobby: gameServerPlatform constants, our game server in servers. Returns (name, template).
func (s *Server) CreateGameSession(mode int, xuids []string) (string, string) {
	scid := DefaultScid
	name := s.NewUUID()
	gsi := "00000000-0000-0000-0000-00000000010" + map[bool]string{true: "0", false: "2"}[mode == 1]
	gsp := NewDict()
	gsp.Set("gameVariantId", "00000000-0000-0000-0000-000000000001")
	gsp.Set("gsiSetId", gsi)
	gsp.Set("maxAllowedPlayers", int64(12))
	custom := NewDict()
	custom.Set("hopperId", int64(0))
	custom.Set("EnableFullArbitration", true)
	custom.Set("gameServerPlatform", gsp)
	sys := NewDict()
	sys.Set("version", int64(1))
	sys.Set("visibility", "open")
	cons := NewDict()
	cons.Set("system", sys)
	cons.Set("custom", custom)
	psys := NewDict()
	psys.Set("joinRestriction", "none")
	psys.Set("readRestriction", "none")
	props := NewDict()
	props.Set("system", psys)
	props.Set("custom", NewDict())
	body := NewDict()
	body.Set("constants", cons)
	body.Set("properties", props)
	text, _ := pyjson.Dumps(ToJSON(body), pyjson.Opts{})
	s.PutSession(fmt.Sprintf("/serviceconfigs/%s/sessionTemplates/%s/sessions/%s", scid, TargetTemplate, name), []byte(text), "")
	s.mu.Lock()
	defer s.mu.Unlock()
	if rec := s.lookup(scid, name); rec != nil {
		rec.Servers = serversBlock()
		// like the matchmaker does for a match, put the invited players into the session as inactive members: the game looks for itself there and
		// then PUTs properties.system.active = true (without it, it never joins and leaves again)
		for _, x := range xuids {
			m := NewDict()
			c := NewDict()
			cs := NewDict()
			cs.Set("xuid", x)
			c.Set("system", cs)
			p := NewDict()
			ps := NewDict()
			ps.Set("active", false)
			p.Set("system", ps)
			m.Set("constants", c)
			m.Set("properties", p)
			rec.Members.Set(x, m)
			rec.Change++
			s.touch(rec, x)
		}
	}
	return name, TargetTemplate
}

// HubLobbies is the live public summaries from the UDP hub, or nil when it cannot be reached.
func (s *Server) HubLobbies() pyjson.Obj { return s.Hub.Query("lobbies") }

// HubPositions is HubLobbies plus where each car is in those lobbies, or nil when the hub cannot be reached.
func (s *Server) HubPositions() pyjson.Obj { return s.Hub.Query("positions") }

// ---------------------------------------------------------------- the hub over UDP

// UDPHub talks to the game server's control port (env FH2MP_SERVER_CONTROL, default 127.0.0.1:5090).
type UDPHub struct {
	rate *diag.RateLimit
}

// NewUDPHub makes the production hub.
func NewUDPHub() *UDPHub { return &UDPHub{rate: diag.NewRateLimit(nil, 0)} }

func controlTarget() string {
	if v := os.Getenv("FH2MP_SERVER_CONTROL"); v != "" {
		return v
	}
	return "127.0.0.1:5090"
}

func (h *UDPHub) every(key string, msg string) {
	if ok, skipped := h.rate.Allow(key, ControlLogSeconds); ok {
		if skipped > 0 {
			msg += fmt.Sprintf(" (+%d like it since the last line)", skipped)
		}
		hubLog.Warn(msg)
	}
}

// Control sends one text datagram to the game server's control port.
func (h *UDPHub) Control(text string) {
	c, err := net.Dial("udp", controlTarget())
	if err == nil {
		defer c.Close()
		_, err = c.Write([]byte(text))
	}
	if err != nil {
		verb, _, _ := strings.Cut(text, " ") // the rest of an auth line is an address and a key
		h.every("send/"+verb, fmt.Sprintf("control datagram %s not delivered to the game server (FH2MP_SERVER_CONTROL=%s): %v", pyRepr(verb), controlTarget(), err))
	}
}

func (h *UDPHub) ask(request string, timeout time.Duration, max int) ([]byte, error) {
	c, err := net.Dial("udp", controlTarget())
	if err != nil {
		return nil, err
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(timeout))
	if _, err := c.Write([]byte(request)); err != nil {
		return nil, err
	}
	buf := make([]byte, max)
	n, err := c.Read(buf)
	return buf[:n], err
}

// Populations asks for the "pop" report.
func (h *UDPHub) Populations() (map[string][]string, bool) {
	data, err := h.ask("pop", 300*time.Millisecond, 65535)
	if err != nil {
		return nil, false
	}
	parts := strings.Fields(string(data))
	if len(parts) == 0 || parts[0] != "pop" {
		h.every("pop-reply", fmt.Sprintf("population query got an unexpected reply %s, expected 'pop ...'; treated as no answer", pyRepr(trunc(string(data), 40))))
		return nil, false
	}
	out := map[string][]string{}
	for _, item := range parts[1:] {
		name, xs, _ := strings.Cut(item, "=")
		var l []string
		for _, x := range strings.Split(xs, ",") {
			if x != "" {
				l = append(l, x)
			}
		}
		if l == nil {
			l = []string{}
		}
		out[name] = l
	}
	return out, true
}

// Query asks for a JSON summary.
func (h *UDPHub) Query(command string) pyjson.Obj {
	data, err := h.ask(command, 300*time.Millisecond, 65535)
	if err != nil {
		h.every("query/"+command, fmt.Sprintf("%s query got no usable answer within 0.3 s: %v", pyRepr(command), err))
		return nil
	}
	v, err := pyjson.Parse(string(data))
	o, ok := v.(pyjson.Obj)
	if err != nil || !ok {
		h.every("query-shape/"+command, fmt.Sprintf("%s query: reply has no 'lobbies' list, treated as no answer", pyRepr(command)))
		return nil
	}
	if l, ok := o.Get("lobbies"); !ok {
		h.every("query-shape/"+command, fmt.Sprintf("%s query: reply has no 'lobbies' list, treated as no answer", pyRepr(command)))
		return nil
	} else if _, isList := l.([]any); !isList {
		h.every("query-shape/"+command, fmt.Sprintf("%s query: reply has no 'lobbies' list, treated as no answer", pyRepr(command)))
		return nil
	}
	return o
}

// Tools sends a tools report and waits up to 1 s for the reply.
func (h *UDPHub) Tools(text string) string {
	data, err := h.ask(text, time.Second, 64)
	if err != nil {
		h.every("tools-send", fmt.Sprintf("tools report (%s) got no answer from the game server in 1 s, the LSP answers 503: %v", pyRepr(text), err))
		return ""
	}
	reply := string(data)
	if reply != "ok" && reply != "rejoin" {
		h.every("tools-reply", fmt.Sprintf("tools report: unexpected reply %s, expected 'ok' or 'rejoin'", pyRepr(trunc(reply, 20))))
	}
	return reply
}
