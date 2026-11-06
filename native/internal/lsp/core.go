// Package lsp is the FH2 Turn 10 LSP stand-in (= server/fh2mp_lsp.py): the Xls web services the game's shim redirects to this server (Content-Type
// bin/xtsw), the Xbox Live host answers, the notification admin API and the HTTP front that also serves the launcher (/v1/) and operator (/ops/) APIs.
//
// Request: headers X-ClassName / X-Method, body = "<Call>_Parameters" struct, big endian. Response: 12-byte header (Result u32, ErrorCode u32,
// DataLength u32; big endian) followed by the "<Call>_Results" struct. Result 0 = OK; 2 = error (ErrorCode | 0x80510000), DataLength bytes of message
// text follow the header. Schemas come from the game catalog (package xls).
package lsp

import (
	"encoding/binary"
	"fmt"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"fh2mp/internal/auth"
	"fh2mp/internal/cardb"
	"fh2mp/internal/catalog"
	"fh2mp/internal/clubs"
	"fh2mp/internal/db"
	"fh2mp/internal/diag"
	"fh2mp/internal/drivatar"
	"fh2mp/internal/notifstore"
	"fh2mp/internal/pyfmt"
	"fh2mp/internal/rivals"
	"fh2mp/internal/scoreboard"
	"fh2mp/internal/social"
	"fh2mp/internal/ugc"
	"fh2mp/internal/xbl"
	"fh2mp/internal/xls"
)

// Core is the LSP's state: every store, the schema, and what the game announced about players.
type Core struct {
	DB     *db.DB
	SC     *xls.Schema
	Cars   *cardb.CarDb
	Notif  *notifstore.Store
	Clubs  *clubs.Store
	Social *social.Store
	Ugc    *ugc.Store
	Drv    *drivatar.Store
	Sb     *scoreboard.Store
	Riv    *rivals.Catalog
	Auth   *auth.Service
	Xbl    *xbl.Server
	Tables *catalog.Tables // part ids of the game data (nil: tune checks are skipped)

	Launcher  LauncherAPI                      // /v1/* (nil: not served)
	Ops       OpsAPI                           // /ops/* (nil: not served)
	AddResult func(body []byte) (int64, error) // the game server's event result (site_stats.add_result); a *valErr is a 400
	front     frontState

	setupLauncher func(c *Core) error // builds the launcher API and the ops API once the identity service exists

	ClubXPDivisor int64
	Verbosity     diag.Verbosity
	Out           func(line string) // where log lines go (default stdout)
	Now           func() float64

	rate   *diag.RateLimit
	seq    atomic.Int64
	mu     sync.RWMutex
	tags   map[int64]string // xuid -> gamertag, learned at Logon
	levels map[int64]int64  // xuid -> currentCareerLevel as sent at Logon (career XP, NOT display level)
	runs   map[string]int64 // run id -> xuid, the runs already introduced to the log and the game server

	authShare *shareCounter
	pollShare *pollCounter

	elemMu sync.Mutex
	elems  map[[2]string]*xls.Struct
}

// New makes a Core around ready stores.
func New() *Core {
	c := &Core{ClubXPDivisor: 10000, Out: func(l string) { fmt.Println(l) }, Now: func() float64 { return float64(time.Now().UnixNano()) / 1e9 },
		rate: diag.NewRateLimit(nil, 0), tags: map[int64]string{}, levels: map[int64]int64{}, runs: map[string]int64{}, elems: map[[2]string]*xls.Struct{}}
	if v := os.Getenv("FH2MP_CLUB_XP_DIVISOR"); v != "" {
		var n int64
		if _, err := fmt.Sscan(v, &n); err == nil && n > 0 {
			c.ClubXPDivisor = n
		}
	}
	c.Verbosity = diag.NewVerbosity(nil, nil)
	c.authShare = newShareCounter(3600)
	c.pollShare = newPollCounter(600)
	return c
}

// Req is what is known about the request being served (Python's thread-local CTX).
type Req struct {
	Seq     int64
	Xuid    int64
	Authed  bool
	Tag     string
	Run     string
	Shim    string
	OwnCars bool
	Nitro   bool      // the game reports NitroSystem in X-FH2MP-Tools (set by identify)
	GameDB  [2]string // client's and server's database hash, for the lines that refuse a request later
}

func (c *Core) nextSeq() int64 { return c.seq.Add(1) }

// ---------------------------------------------------------------- tags and levels

// Tag is the gamertag learned for an xuid ("" = unknown).
func (c *Core) Tag(xuid int64) string {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.tags[xuid]
}

// SetTag remembers a gamertag.
func (c *Core) SetTag(xuid int64, tag string) {
	c.mu.Lock()
	c.tags[xuid] = tag
	c.mu.Unlock()
}

// SetTagDefault remembers a gamertag unless one is known.
func (c *Core) SetTagDefault(xuid int64, tag string) {
	c.mu.Lock()
	if _, ok := c.tags[xuid]; !ok {
		c.tags[xuid] = tag
	}
	c.mu.Unlock()
}

// Level is the career level sent at Logon (0 = unknown).
func (c *Core) Level(xuid int64) int64 {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.levels[xuid]
}

// SetLevel remembers the career level.
func (c *Core) SetLevel(xuid, lvl int64) {
	c.mu.Lock()
	c.levels[xuid] = lvl
	c.mu.Unlock()
}

// SetLevelDefault remembers the career level unless one is known.
func (c *Core) SetLevelDefault(xuid, lvl int64) {
	c.mu.Lock()
	if _, ok := c.levels[xuid]; !ok {
		c.levels[xuid] = lvl
	}
	c.mu.Unlock()
}

// tagFromDB is the gamertag stored in players ("" = none).
func (c *Core) tagFromDB(xuid int64) string {
	r, err := c.DB.One("SELECT gamertag FROM players WHERE xuid=?", xuid)
	if err != nil || r == nil {
		return ""
	}
	return r.Str("gamertag")
}

// ---------------------------------------------------------------- logging

// Log writes one INFO line. A line the handler of a request writes below its "#N POST ..." line (indented two spaces) carries that request's number
// too, because the server handles requests in parallel. INFO carries no level word (other tools parse these lines).
func (c *Core) Log(r *Req, msg string) { c.LogAt(r, diag.Info, "", msg) }

// Warn writes a WARN line.
func (c *Core) Warn(r *Req, msg string) { c.LogAt(r, diag.Warn, "", msg) }

// Debugf writes a DEBUG line of a subsystem.
func (c *Core) Debug(r *Req, sub, msg string) { c.LogAt(r, diag.Debug, sub, msg) }

// LogAt writes a line at a level for a subsystem.
func (c *Core) LogAt(r *Req, level int, sub, msg string) {
	if !c.Verbosity.Enabled(level, sub) {
		return
	}
	if r != nil {
		if r.Seq != 0 && strings.HasPrefix(msg, "  ") && !strings.HasPrefix(msg, "  #") {
			msg = fmt.Sprintf("  #%d%s", r.Seq, msg[1:])
		}
		if r.Run != "" && (level == diag.Warn || level == diag.Error) && !strings.Contains(msg, " run=") {
			msg += " run=" + r.Run // a refusal or failure names its game without a search for the "#N POST" line
		}
	}
	word := ""
	if level != diag.Info {
		word = diag.LevelName(level) + " "
	}
	line := fmt.Sprintf("%s %s%s", time.Now().Format("15:04:05"), word, diag.Scrub(msg))
	diag.WriteLine(func(l string) error { c.Out(l); return nil }, line)
}

// LogExc logs a caught error: what was being done, then the error.
func (c *Core) LogExc(r *Req, msg string, err error, level int) {
	c.LogAt(r, level, "", fmt.Sprintf("%s: %v", msg, err))
}

// LogEvery logs a repeating condition at most once per interval seconds per key, saying how many lines it stood in for.
func (c *Core) LogEvery(r *Req, key string, interval float64, msg string, level int, sub string) {
	if ok, skipped := c.rate.Allow(key, interval); ok {
		if skipped > 0 {
			msg += fmt.Sprintf(" (and %d more in the last %.0f s)", skipped, interval)
		}
		c.LogAt(r, level, sub, msg)
	}
}

// ---------------------------------------------------------------- counters that replace per-request lines

type shareCounter struct {
	mu      sync.Mutex
	period  float64
	start   time.Time
	counts  [2]int
	players [2]map[int64]bool
}

func newShareCounter(period float64) *shareCounter {
	return &shareCounter{period: period, start: time.Now(), players: [2]map[int64]bool{{}, {}}}
}

// note counts a request; once per period it returns the summary line of the last period.
func (s *shareCounter) note(xuid int64, authed bool) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	line := ""
	if time.Since(s.start).Seconds() >= s.period {
		line = fmt.Sprintf("auth share: last hour %d requests with a token from %d players, %d without from %d", s.counts[1], len(s.players[1]), s.counts[0], len(s.players[0]))
		s.start, s.counts, s.players = time.Now(), [2]int{}, [2]map[int64]bool{{}, {}}
	}
	i := 0
	if authed {
		i = 1
	}
	s.counts[i]++
	s.players[i][xuid] = true
	return line
}

type pollCounter struct {
	mu      sync.Mutex
	period  float64
	start   time.Time
	counts  map[string]int
	players map[string]map[int64]bool
}

func newPollCounter(period float64) *pollCounter {
	return &pollCounter{period: period, start: time.Now(), counts: map[string]int{}, players: map[string]map[int64]bool{}}
}

func (p *pollCounter) note(call string, xuid int64) string {
	p.mu.Lock()
	defer p.mu.Unlock()
	line := ""
	if el := time.Since(p.start).Seconds(); el >= p.period {
		if len(p.counts) > 0 {
			var names []string
			for k := range p.counts {
				names = append(names, k)
			}
			sortStrings(names)
			parts := make([]string, len(names))
			for i, k := range names {
				parts[i] = fmt.Sprintf("%s %d requests from %d players", k, p.counts[k], len(p.players[k]))
			}
			line = fmt.Sprintf("polls: last %.0f s: %s", el, strings.Join(parts, ", "))
		}
		p.start, p.counts, p.players = time.Now(), map[string]int{}, map[string]map[int64]bool{}
	}
	p.counts[call]++
	if p.players[call] == nil {
		p.players[call] = map[int64]bool{}
	}
	p.players[call][xuid] = true
	return line
}

func sortStrings(l []string) {
	for i := 1; i < len(l); i++ {
		for j := i; j > 0 && l[j] < l[j-1]; j-- {
			l[j], l[j-1] = l[j-1], l[j]
		}
	}
}

// ---------------------------------------------------------------- run ids

// noteRun is the first signed-in request of a game: one line that ties its run id to the player, and the same for the game server, whose lines cannot
// see the header.
func (c *Core) noteRun(r *Req, run string, xuid int64) {
	base, _, _ := strings.Cut(run, ".")
	c.mu.Lock()
	if base == "" || xuid == 0 || c.runs[base] == xuid {
		c.mu.Unlock()
		return
	}
	if len(c.runs) >= 20000 {
		c.runs = map[string]int64{}
	}
	c.runs[base] = xuid
	c.mu.Unlock()
	var shim any
	if r.Shim != "" {
		shim = r.Shim
	}
	c.Log(r, "  run.seen "+diag.KV(diag.KVP{K: "run", V: base}, diag.KVP{K: "xuid", V: fmt.Sprintf("%#x", xuid)}, diag.KVP{K: "shim", V: shim}))
	c.Xbl.AnnounceRun(xuid, base)
}

// ---------------------------------------------------------------- the wire

// XlsError is an error result the game maps to a message.
type XlsError struct {
	Text string
	Code int
}

func (e *XlsError) Error() string { return e.Text }

func xerr(f string, a ...any) *XlsError { return &XlsError{fmt.Sprintf(f, a...), 1} }

// frame is the 12-byte response header + text + payload.
func frame(payload []byte, result, errCode uint32, text []byte) []byte {
	out := make([]byte, 12, 12+len(text)+len(payload))
	binary.BigEndian.PutUint32(out, result)
	binary.BigEndian.PutUint32(out[4:], errCode)
	binary.BigEndian.PutUint32(out[8:], uint32(len(text)))
	return append(append(out, text...), payload...)
}

// candidates are the possible descriptor-entry names for a call, e.g. ForzaUserLogon_Results.
func candidates(cls, method, suffix string) []string {
	cls, _, _ = strings.Cut(cls, ",")
	cls = strings.TrimSpace(cls)
	parts := strings.Split(cls, ".")
	join := func(ps []string, skipWS bool) string {
		var b strings.Builder
		for _, p := range ps {
			if skipWS && p == "WebServices" {
				continue
			}
			b.WriteString(p)
		}
		return b.String()
	}
	var rest []string
	if len(parts) > 1 {
		rest = parts[1:]
	}
	names := []string{join(parts, false) + method, join(parts, true) + method, join(rest, false) + method, join(rest, true) + method}
	for i := range names {
		names[i] += suffix
	}
	return names
}

// findStruct looks a call struct up under the names the exe could have used.
func (c *Core) findStruct(cls, method, suffix string) (string, []*xls.Field) {
	for _, n := range candidates(cls, method, suffix) {
		if f, err := c.SC.CallStruct(n); err == nil {
			return n, f
		}
	}
	return "", nil
}

func (c *Core) callStruct(name string) []*xls.Field {
	f, err := c.SC.CallStruct(name)
	if err != nil {
		panic(fmt.Sprintf("no call struct %s: %v", name, err))
	}
	return f
}

// def is a struct with every field at its default value.
func (c *Core) def(fields []*xls.Field) *xls.Struct {
	s, err := c.SC.DefaultStruct(fields)
	if err != nil {
		panic(err)
	}
	return s
}

// defCall is the default result of a call struct, by name.
func (c *Core) defCall(name string) *xls.Struct { return c.def(c.callStruct(name)) }

// elemTemplate is a default element of an array field of a result struct (every field present, ready to fill).
func (c *Core) elemTemplate(resultStruct, field string) *xls.Struct {
	key := [2]string{resultStruct, field}
	c.elemMu.Lock()
	t, ok := c.elems[key]
	if !ok {
		for _, f := range c.callStruct(resultStruct) {
			if f.Name == field {
				el := c.SC.Field(f.Elem)
				t = c.def(c.SC.Fields(el.Nested))
			}
		}
		if t == nil {
			c.elemMu.Unlock()
			panic("no field " + field + " in " + resultStruct)
		}
		c.elems[key] = t
	}
	c.elemMu.Unlock()
	return t.Clone()
}

// subFields is the field list of a nested struct field of a call struct's root (the "responseData"-style wrapper).
func (c *Core) subFields(f *xls.Field) []*xls.Field { return c.SC.Sub(f) }

// owner is the UserInfo struct of a player (creator of a design, club member, ...), with the club tag shown in front of the gamertag.
func (c *Core) owner(tmpl *xls.Struct, xuid int64) *xls.Struct {
	o := tmpl.Clone()
	var clubID, role int64
	var clubTag string
	if c.Clubs != nil {
		if info, err := c.Clubs.MemberInfo(xuid); err == nil {
			clubID, clubTag, role = info.ClubID, info.Tag, info.Role
		}
	}
	tag := c.Tag(xuid)
	if tag == "" {
		tag = c.tagFromDB(xuid)
	}
	o.Set("qwXuid", xuid)
	o.Set("wzGamerTag", cut(tag, 31))
	o.Set("currentCareerLevel", cleanLevel(c.Level(xuid)))
	o.Set("clubId", clubID)
	o.Set("clubTag", cut(clubTag, 9))
	o.Set("roleInClub", clubs.WireRole(role))
	return o
}

// cut is s[:n] by characters.
func cut(s string, n int) string {
	if r := []rune(s); len(r) > n {
		return string(r[:n])
	}
	return s
}

func fmtG(x float64) string { return pyfmt.FmtG(x) }

// cleanLevel is db.clean_level for a value that is given.
func cleanLevel(x int64) int64 { v, _ := db.CleanLevel(&x); return v }
