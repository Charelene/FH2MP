package lsp

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/json"
	"fmt"
	"math"
	"net"
	"net/http"
	"net/url"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"
	"unicode"

	"fh2mp/internal/auth"
	"fh2mp/internal/db"
	"fh2mp/internal/diag"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/rivals"
	"fh2mp/internal/scoreboard"
	"fh2mp/internal/scorereports"
	"fh2mp/internal/social"
)

// The operator API the forza.scuffed.af website talks to (/ops/v1/*): the public stats and the admin panel. It is off unless FH2MP_OPS_TOKEN is set. A request
// needs `Authorization: Bearer <token>`, must come straight from an address in FH2MP_OPS_ALLOW (comma separated CIDRs, default loopback only) and must not
// carry proxy headers, so nothing reachable through the public reverse proxy can ever get in. Wrong tokens are rate limited per address.

var (
	opsLog     = diag.Get("ops")
	opsStarted = time.Now()
)

const (
	opsRefusalSecs = 120.0
	blocklistMax   = 500
	newsMax        = 12
	inviteAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789" // no 0/O/1/I/L
)

var (
	monthlyBoard  = [2]int64{3, 1} // (scoreboardType, scoreType) of a Monthly Rivals event; the pivot is the event id
	festivalBoard = [2]int64{1, 1} // a Festival Route; the track id is the route, the pivot the car class
	destinations  = []string{"Castelletto", "Montellino", "Nice", "Saint-Martin", "San Giovanni", "Sisteron"}
	notRoutes     = []string{"Showcase", "Road Trip", "Finale", "Arena"}
	linkKeys      = []string{"website", "patreon", "discord"}
	regModes      = []string{"open", "invite", "closed"}
	newsFields    = []struct {
		k     string
		limit int
	}{{"tag", 24}, {"title", 80}, {"text", 200}, {"url", 300}, {"image", 300}}
)

// OpsError is a refusal of the operator API.
type OpsError struct {
	Status        int
	Code, Message string
	Detail        string
}

func (e *OpsError) Error() string { return e.Message }

func opsErr(status int, code, message string) *OpsError {
	if message == "" {
		message = code
	}
	return &OpsError{Status: status, Code: code, Message: message}
}

func (e *OpsError) with(detail string) *OpsError { e.Detail = detail; return e }

// OpsConfig is the access policy.
type OpsConfig struct {
	Token string
	Allow []*net.IPNet
}

// OpsConfigFromEnv reads FH2MP_OPS_TOKEN and FH2MP_OPS_ALLOW.
func OpsConfigFromEnv() *OpsConfig {
	raw := os.Getenv("FH2MP_OPS_ALLOW")
	allow := []string{"127.0.0.1/32", "::1/128"}
	if strings.TrimSpace(raw) != "" {
		allow = strings.Split(raw, ",")
	}
	cfg := &OpsConfig{Token: strings.TrimSpace(os.Getenv("FH2MP_OPS_TOKEN"))}
	for _, a := range allow {
		a = strings.TrimSpace(a)
		if a == "" {
			continue
		}
		if !strings.Contains(a, "/") {
			if ip := net.ParseIP(a); ip != nil && ip.To4() != nil {
				a += "/32"
			} else {
				a += "/128"
			}
		}
		if _, n, err := net.ParseCIDR(a); err == nil {
			cfg.Allow = append(cfg.Allow, n)
		}
	}
	return cfg
}

// Permits says whether the address is on the allow list.
func (cfg *OpsConfig) Permits(ip string) bool {
	addr := net.ParseIP(ip)
	if addr == nil {
		return false
	}
	if v4 := addr.To4(); v4 != nil {
		addr = v4
	}
	for _, n := range cfg.Allow {
		if n.Contains(addr) {
			return true
		}
	}
	return false
}

// Ops is the operator API.
type Ops struct {
	C    *Core
	Cfg  *OpsConfig
	rate *diag.RateLimit
	site *siteCache
}

// NewOps makes the API for a core.
func NewOps(c *Core, cfg *OpsConfig) *Ops {
	return &Ops{C: c, Cfg: cfg, rate: diag.NewRateLimit(nil, 0), site: &siteCache{}}
}

var opsIDRe = regexp.MustCompile(`/\d+`)

func opsRouteName(path string) string {
	p, _, _ := strings.Cut(path, "?")
	return cut(opsIDRe.ReplaceAllString(strings.TrimRight(p, "/"), "/N"), 80)
}

// Handle answers one /ops request; it never fails.
func (o *Ops) Handle(r *Req, method, path string, h http.Header, body []byte, peer string, proxied bool) (status int, obj any, dl *Download) {
	defer func() {
		if p := recover(); p != nil {
			switch e := p.(type) {
			case *OpsError:
				o.refused(method, path, e.Status, e.Code, e.Message, e.Detail)
				status, obj, dl = e.Status, pyjson.Obj{{K: "error", V: e.Code}, {K: "message", V: e.Message}}, nil
			case *auth.Error:
				o.refused(method, path, e.Status, e.Code, e.Message, e.Detail)
				status, obj, dl = e.Status, pyjson.Obj{{K: "error", V: e.Code}, {K: "message", V: e.Message}}, nil
			case *ApiError:
				o.refused(method, path, e.Status, "bad_request", e.Msg, "")
				status, obj, dl = e.Status, pyjson.Obj{{K: "error", V: "bad_request"}, {K: "message", V: e.Msg}}, nil
			default:
				err, _ := p.(error)
				if err == nil {
					err = fmt.Errorf("%v", p)
				}
				opsLog.Exc(fmt.Sprintf("unhandled error in %s %s, answered 500", method, opsRouteName(path)), err)
				status, obj, dl = 500, pyjson.Obj{{K: "error", V: "internal"}, {K: "message", V: fmt.Sprintf("%T", p)}}, nil
			}
		}
	}()
	o.authorize(h, peer, proxied)
	return o.route(r, method, path, body)
}

func (o *Ops) refused(method, path string, status int, code, message, detail string) {
	route := opsRouteName(path)
	ok, skipped := o.rate.Allow([4]any{"refused", status, code, route}, opsRefusalSecs)
	if !ok {
		return
	}
	var msg, det any
	if message != code {
		msg = message
	}
	if detail != "" {
		det = detail
	}
	line := fmt.Sprintf("refused %d %s: %s %s ", status, code, method, route) + diag.KV(diag.KVP{K: "message", V: msg}, diag.KVP{K: "detail", V: det})
	if skipped > 0 {
		line += fmt.Sprintf(" (and %d more in the last %.0f s)", skipped, opsRefusalSecs)
	}
	if status == 401 || status == 403 || status == 429 || status >= 500 {
		opsLog.Warn(line)
	} else {
		opsLog.Info(line)
	}
}

func (o *Ops) authorize(h http.Header, peer string, proxied bool) {
	if o.Cfg.Token == "" {
		panic(opsErr(404, "disabled", "the operator API is not enabled on this server").with("FH2MP_OPS_TOKEN is not set"))
	}
	if proxied || !o.Cfg.Permits(peer) {
		d := fmt.Sprintf("the sender is not in FH2MP_OPS_ALLOW (%d network(s) allowed)", len(o.Cfg.Allow))
		if proxied {
			d = "the request carries proxy headers (X-Real-IP / X-Forwarded-For): it came through the public proxy"
		}
		panic(opsErr(403, "forbidden", "").with(d))
	}
	given := bearer(h)
	if subtle.ConstantTimeCompare([]byte(given), []byte(o.Cfg.Token)) != 1 {
		if o.C.Auth != nil && !o.C.Auth.Hit([2]any{"ops-fail", peer}, 10, 60) {
			panic(opsErr(429, "rate_limited", "too many wrong tokens").with("over 10 wrong tokens per minute from this sender"))
		}
		d := "no bearer token sent"
		if given != "" {
			d = fmt.Sprintf("the bearer token does not match (%d characters sent)", len([]rune(given)))
		}
		panic(opsErr(401, "unauthorized", "").with(d))
	}
}

func opsJSON(body []byte) pyjson.Obj {
	if len(body) == 0 {
		return pyjson.Obj{}
	}
	v, err := pyjson.Parse(string(body))
	if err != nil {
		panic(opsErr(400, "bad_request", "invalid JSON"))
	}
	d, ok := v.(pyjson.Obj)
	if !ok {
		panic(opsErr(400, "bad_request", "expected a JSON object"))
	}
	return d
}

// qfirst is the first non-blank value of a query parameter (parse_qs drops blanks).
func qfirst(q url.Values, key string) (string, bool) {
	for _, v := range q[key] {
		if v != "" {
			return v, true
		}
	}
	return "", false
}

func qInt(q url.Values, key string, dflt, lo, hi int64) int64 {
	s, ok := qfirst(q, key)
	if !ok {
		return max(lo, min(hi, dflt))
	}
	n, okn := pyInt(s, 10)
	if !okn {
		panic(opsErr(400, "bad_request", key+" must be a number"))
	}
	return max(lo, min(hi, n))
}

func (o *Ops) nickname(hexXuid string) string {
	x, _ := strconv.ParseInt(hexXuid, 16, 64)
	name := o.C.Tag(x)
	if name == "" {
		if row, err := o.C.DB.One("SELECT gamertag FROM players WHERE xuid = ?", x); err == nil && row != nil {
			name = row.Str("gamertag")
		}
	}
	if name == "" {
		return "Connecting player"
	}
	return name
}

// flagsList is the game server's cheater pool with gamertags: {"flagged": [{xuid, gamertag, reason, until}]}.
func (o *Ops) flagsList() pyjson.Obj {
	result := o.C.Xbl.HubFlags()
	if result == nil {
		panic(opsErr(503, "unavailable", "the game server did not answer").with("flags query got no answer"))
	}
	fv, _ := result.Get("flagged")
	out := []any{}
	for _, f := range anyList(fv) {
		fo, _ := f.(pyjson.Obj)
		xv, _ := fo.Get("xuid")
		rv, _ := fo.Get("reason")
		uv, _ := fo.Get("until")
		xs := pyjson.PyStr(xv)
		x, _ := strconv.ParseInt(xs, 16, 64)
		out = append(out, pyjson.Obj{{K: "xuid", V: x}, {K: "gamertag", V: o.nickname(xs)}, {K: "reason", V: rv}, {K: "until", V: uv}})
	}
	return pyjson.Obj{{K: "flagged", V: out}}
}

// unflag releases a player from the cheater pool and answers with whether the game server still lists them afterwards.
func (o *Ops) unflag(decimalXuid string) pyjson.Obj {
	x, err := strconv.ParseInt(decimalXuid, 10, 64)
	if err != nil || x <= 0 {
		panic(opsErr(400, "bad_xuid", "that is not a player id"))
	}
	if o.C.Xbl.HubFlags() == nil {
		panic(opsErr(503, "unavailable", "the game server did not answer").with("flags query got no answer"))
	}
	o.C.Xbl.HubUnflag(x)
	time.Sleep(150 * time.Millisecond) // the control datagram and the query travel on separate sockets
	still := false
	if result := o.C.Xbl.HubFlags(); result != nil {
		fv, _ := result.Get("flagged")
		for _, f := range anyList(fv) {
			fo, _ := f.(pyjson.Obj)
			xv, _ := fo.Get("xuid")
			if v, err := strconv.ParseInt(pyjson.PyStr(xv), 16, 64); err == nil && v == x {
				still = true
			}
		}
	}
	opsLog.Info(fmt.Sprintf("operator released %#x from the cheater pool (still flagged afterwards: %v)", x, still))
	return pyjson.Obj{{K: "ok", V: !still}, {K: "xuid", V: x}}
}

func must[T any](v T, err error) T {
	if err != nil {
		panic(err)
	}
	return v
}

func mustOK(err error) {
	if err != nil {
		panic(err)
	}
}

func (o *Ops) route(r *Req, method, path string, body []byte) (int, any, *Download) {
	c := o.C
	u, _ := url.Parse(path)
	q := u.Query()
	parts := splitPath(u.Path)
	if len(parts) < 3 || parts[0] != "ops" || parts[1] != "v1" {
		panic(opsErr(404, "not_found", ""))
	}
	what, rest := parts[2], parts[3:]

	if what == "crash-reports" && len(rest) == 2 && method == "GET" {
		if c.Launcher == nil {
			panic(opsErr(404, "not_found", ""))
		}
		l, _ := c.Launcher.(*Launcher)
		if l == nil || l.Crashes == nil {
			panic(opsErr(404, "not_found", ""))
		}
		data, err := l.Crashes.Download(rest[0], rest[1])
		mustOK(err)
		return 200, nil, &Download{Data: data, Name: rest[1], ContentType: crashContentType(rest[1])}
	}
	if what == "reports" {
		if len(rest) == 0 && method == "GET" {
			return 200, o.reportsQueue(int(qInt(q, "limit", 50, 1, 100)), qInt(q, "offset", 0, 0, 1e9)), nil
		}
		if len(rest) > 0 && isDigits(rest[0]) {
			sid, _ := strconv.ParseInt(rest[0], 10, 64)
			if len(rest) == 1 && method == "POST" {
				return 200, o.reportRun(sid, opsJSON(body)), nil
			}
			if len(rest) == 2 && method == "POST" {
				return 200, o.moderate(sid, rest[1]), nil
			}
		}
	}
	if what == "lobbies" && len(rest) == 0 && method == "GET" {
		result := c.Xbl.HubLobbies()
		if result == nil {
			panic(opsErr(503, "unavailable", "the game server did not answer").with("lobby list query got no answer"))
		}
		// Resolve private protocol IDs to nicknames here; public summaries contain no player IDs.
		lv, _ := result.Get("lobbies")
		list := anyList(lv)
		for i := range list {
			lo, _ := list[i].(pyjson.Obj)
			var names []string
			mv, _ := lo.Get("memberXuids")
			for _, x := range anyList(mv) {
				s, _ := x.(string)
				names = append(names, o.nickname(s))
			}
			sort.SliceStable(names, func(i, j int) bool { return strings.ToLower(names[i]) < strings.ToLower(names[j]) })
			members := make([]any, len(names))
			for i, n := range names {
				members[i] = n
			}
			dropKey(&lo, "memberXuids")
			lo.Set("members", members)
			list[i] = lo
		}
		return 200, result, nil
	}
	if what == "flags" {
		if len(rest) == 0 && method == "GET" {
			return 200, o.flagsList(), nil
		}
		if len(rest) == 2 && rest[1] == "unflag" && method == "POST" && isDigits(rest[0]) {
			return 200, o.unflag(rest[0]), nil
		}
	}
	if what == "positions" && len(rest) == 0 && method == "GET" {
		result := c.Xbl.HubPositions()
		if result == nil {
			panic(opsErr(503, "unavailable", "the game server did not answer").with("positions query got no answer"))
		}
		names := must(c.Riv.Names()).Car
		lv, _ := result.Get("lobbies")
		list := anyList(lv)
		for i := range list {
			lo, _ := list[i].(pyjson.Obj)
			dropKey(&lo, "memberXuids")
			cv, _ := lo.Get("cars")
			cars := []any{}
			for _, cc := range anyList(cv) {
				co, _ := cc.(pyjson.Obj)
				id, _ := co.Get("id")
				xv, _ := co.Get("xuid")
				x, _ := co.Get("x")
				z, _ := co.Get("z")
				carv, _ := co.Get("car")
				cars = append(cars, pyjson.Obj{{K: "id", V: id}, {K: "name", V: o.nickname(pyjson.PyStr(xv))}, {K: "x", V: x}, {K: "z", V: z}, {K: "car", V: names[toInt(carv)]}})
			}
			lo.Set("cars", cars)
			list[i] = lo
		}
		return 200, result, nil
	}
	if what == "overview" && method == "GET" {
		return 200, o.overview(), nil
	}
	if what == "online" && method == "GET" {
		return 200, pyjson.Obj{{K: "players", V: o.online()}}, nil
	}
	if what == "challenges" && method == "GET" {
		return 200, o.challenges(int(qInt(q, "top", 3, 0, 25))), nil
	}
	if what == "leaderboards" && method == "GET" {
		return 200, o.leaderboards(int(qInt(q, "limit", 25, 1, 100))), nil
	}
	if what == "players" {
		if len(rest) == 0 && method == "GET" {
			text, _ := qfirst(q, "q")
			return 200, o.players(text, int(qInt(q, "limit", 50, 1, 200)), qInt(q, "offset", 0, 0, 1e9)), nil
		}
		if len(rest) == 2 && method == "POST" {
			return 200, o.playerAction(r, rest[0], rest[1], opsJSON(body)), nil
		}
	}
	if what == "notifications" || what == "notification-types" {
		return o.notifications(r, method, what, rest, body)
	}
	if what == "rivals" && len(rest) == 1 && rest[0] == "rotate" && method == "POST" {
		events := must(c.Riv.Rotate())
		mustOK(c.announceRotation(r))
		c.Log(r, fmt.Sprintf("  ops: rivals re-rolled -> %d events", len(events)))
		return 200, o.challenges(0), nil
	}
	if what == "routes" && method == "GET" {
		if len(rest) == 0 {
			return 200, o.routes(), nil
		}
		if len(rest) == 1 {
			return 200, o.routeBoards(rest[0], int(qInt(q, "limit", 50, 1, 100))), nil
		}
	}
	if what == "recent" && method == "GET" {
		return 200, o.recent(int(qInt(q, "limit", 20, 1, 100))), nil
	}
	if what == "config" {
		if method == "GET" {
			return 200, o.getConfig(), nil
		}
		if method == "PUT" {
			return 200, o.setConfig(r, opsJSON(body)), nil
		}
	}
	if what == "invites" && len(rest) == 0 && method == "POST" {
		return 201, o.makeInvite(r, opsJSON(body)), nil
	}
	if what == "auth-events" && method == "GET" {
		return 200, o.authEvents(int(qInt(q, "limit", 50, 1, 500))), nil
	}
	if method == "GET" {
		if st, obj, found := o.siteRoute(what, rest, q); found {
			return st, obj, nil
		}
	}
	panic(opsErr(404, "not_found", "unknown endpoint"))
}

func crashContentType(name string) string {
	if strings.HasSuffix(name, ".dmp") {
		return "application/octet-stream"
	}
	return "text/plain; charset=utf-8"
}

func dropKey(o *pyjson.Obj, k string) {
	var out pyjson.Obj
	for _, p := range *o {
		if p.K != k {
			out = append(out, p)
		}
	}
	*o = out
}

// ---------------------------------------------------------------- reads

func (o *Ops) overview() pyjson.Obj {
	c := o.C
	st := must(c.Auth.Stats())
	month, reroll := must3(c.Riv.Serial(nil))
	evs := must(c.Riv.Events())
	cfg := c.Launcher.(*Launcher).Config.Get()
	players := must(c.DB.One("SELECT COUNT(*) FROM players WHERE gamertag <> ''")).IntAt(0)
	clubs := must(c.DB.One("SELECT COUNT(*) FROM clubs")).IntAt(0)
	ugcN := must(c.Ugc.Count(""))
	nn := must(c.Notif.Count())
	scores := must(c.Sb.Count())
	now := time.Now()
	return pyjson.Obj{{K: "time", V: now.Unix()}, {K: "uptime", V: int64(now.Sub(opsStarted).Seconds())}, {K: "env", V: c.Auth.Env},
		{K: "auth", V: c.Auth.Mode}, {K: "registration", V: c.Auth.Registration}, {K: "maintenance", V: objAny(cfg, "maintenance")},
		{K: "online", V: c.OnlineCount()}, {K: "accounts", V: st["accounts"]}, {K: "sessions", V: st["sessions"]}, {K: "registered24h", V: st["registered_24h"]},
		{K: "players", V: players}, {K: "scores", V: scores}, {K: "notifications", V: nn}, {K: "userContent", V: ugcN},
		{K: "clubs", V: clubs}, {K: "lobbies", V: c.Xbl.Count()},
		{K: "rotation", V: pyjson.Obj{{K: "month", V: month}, {K: "reroll", V: reroll}, {K: "events", V: len(evs)}}}}
}

func must3(a, b int64, err error) (int64, int64) {
	if err != nil {
		panic(err)
	}
	return a, b
}

func (o *Ops) online() []any {
	now := social.Now()
	type ent struct {
		x int64
		t float64
		g string
	}
	var es []ent
	for x, t := range social.Polled() {
		if now-t < social.OnlineSeconds {
			es = append(es, ent{x, t, o.C.Tag(x)})
		}
	}
	sort.SliceStable(es, func(i, j int) bool { return strings.ToLower(es[i].g) < strings.ToLower(es[j].g) })
	out := []any{}
	for _, e := range es {
		out = append(out, pyjson.Obj{{K: "xuid", V: e.x}, {K: "gamertag", V: e.g}, {K: "lastSeen", V: e.t}})
	}
	return out
}

func (o *Ops) gamertag(tag string, xuid int64) string {
	if tag != "" {
		return tag
	}
	if t := o.C.Tag(xuid); t != "" {
		return t
	}
	return fmt.Sprintf("Player %04X", xuid&0xFFFF)
}

func classLetter(cls int64) string {
	if cls >= 0 && int(cls) < len(rivals.ClassLetters) {
		return rivals.ClassLetters[cls]
	}
	return ""
}

func (o *Ops) boardRow(r *scoreboard.Row, car map[int64]string) pyjson.Obj {
	row := pyjson.Obj{{K: "submissionId", V: r.SubmissionID}, {K: "reports", V: r.Reports}, {K: "suspicious", V: r.Suspicious}}
	return append(row, pyjson.Obj{{K: "position", V: r.Position}, {K: "xuid", V: r.Xuid}, {K: "gamertag", V: o.gamertag(r.Tag, r.Xuid)}, {K: "score", V: r.Score},
		{K: "car", V: car[r.Car]}, {K: "carId", V: r.Car}, {K: "class", V: classLetter(r.Cls)}, {K: "clean", V: r.Clean}, {K: "at", V: int64(r.T)}}...)
}

// pyRound is round(x, nd): correctly rounded decimal, ties to even.
func pyRound(x float64, nd int) float64 {
	f, _ := strconv.ParseFloat(strconv.FormatFloat(x, 'f', nd, 64), 64)
	return f
}

func (o *Ops) event(e *rivals.Event, rows []any) pyjson.Obj {
	cls := ""
	if e.Cls != nil {
		cls = classLetter(*e.Cls)
	}
	return pyjson.Obj{{K: "id", V: e.ID}, {K: "name", V: e.Name}, {K: "track", V: e.TrackName}, {K: "laps", V: e.Laps}, {K: "km", V: pyRound(e.Length/1000.0, 2)},
		{K: "kind", V: e.Kind}, {K: "description", V: e.Description}, {K: "class", V: cls}, {K: "rows", V: rows}}
}

func yearMonth(month int64) (int, int) {
	return 2026 + int((month)/12), int(month%12) + 1
}

func monthLabel(month int64) string {
	y, m := yearMonth(month)
	return fmt.Sprintf("%s %d", time.Month(m).String(), y)
}

func (o *Ops) rotation() pyjson.Obj {
	month, reroll := must3(o.C.Riv.Serial(nil))
	y, m := yearMonth(month)
	ny, nm := y, m+1
	if m == 12 {
		ny, nm = y+1, 1
	}
	end := time.Date(ny, time.Month(nm), 1, 0, 0, 0, 0, time.UTC).Unix()
	return pyjson.Obj{{K: "label", V: monthLabel(month)}, {K: "month", V: month}, {K: "reroll", V: reroll}, {K: "endsAt", V: end}}
}

func (o *Ops) boardRows(rows []*scoreboard.Row, names map[int64]string) []any {
	out := []any{}
	for _, r := range rows {
		out = append(out, o.boardRow(r, names))
	}
	return out
}

func (o *Ops) challenges(top int) pyjson.Obj {
	c := o.C
	names := must(c.Riv.Names()).Car
	events := []any{}
	for _, e := range must(c.Riv.Events()) {
		rows := []any{}
		if top > 0 {
			rows = o.boardRows(must(c.Sb.Top(monthlyBoard[0], monthlyBoard[1], -1, e.ID, top)), names)
		}
		events = append(events, o.event(e, rows))
	}
	return pyjson.Obj{{K: "rotation", V: o.rotation()}, {K: "events", V: events}}
}

func (o *Ops) leaderboards(limit int) pyjson.Obj {
	c := o.C
	names := must(c.Riv.Names()).Car
	xp := []any{}
	for i, r := range must(c.Sb.XPTop(limit)) {
		xp = append(xp, pyjson.Obj{{K: "position", V: i + 1}, {K: "xuid", V: r.Xuid}, {K: "gamertag", V: o.gamertag(r.Tag, r.Xuid)}, {K: "xp", V: int64(r.Score)}})
	}
	events := []any{}
	for _, e := range must(c.Riv.Events()) {
		events = append(events, o.event(e, o.boardRows(must(c.Sb.Top(monthlyBoard[0], monthlyBoard[1], -1, e.ID, limit)), names)))
	}
	return pyjson.Obj{{K: "rotation", V: o.rotation()}, {K: "xp", V: xp}, {K: "events", V: events}}
}

// Boards is the launcher's /v1/challenges, /v1/leaderboards and /v1/home source.
func (o *Ops) Boards(kind string, n int) (any, error) {
	var out any
	var err error
	func() {
		defer func() {
			if p := recover(); p != nil {
				if e, ok := p.(error); ok {
					err = e
					return
				}
				panic(p)
			}
		}()
		switch kind {
		case "home":
			out = o.C.buildHome()
		case "challenges":
			out = o.challenges(n)
		default:
			out = o.leaderboards(n)
		}
	}()
	return out, err
}

type route struct {
	ID      int64
	Name    string
	Kind    string
	Km      float64
	Circuit bool
	Dest    string
	Tracks  []int64
}

type destination struct {
	Name   string
	Routes []*route
}

func routeKind(name string, closed bool) string {
	low := strings.ToLower(name)
	for _, w := range [][2]string{{"cross country", "Cross Country"}, {"street race", "Street Race"}, {"scramble", "Scramble"}} {
		if strings.Contains(low, w[0]) {
			return w[1]
		}
	}
	if closed {
		return "Circuit"
	}
	return "Sprint"
}

// routeCatalog is Festival Routes grouped by destination, from the game catalog. The catalog has several track ids for one route (same name); they are one
// route here, and its id is the lowest. Circuits come first, then by length. Cached: the catalog never changes while the server runs.
func (o *Ops) routeCatalog() []*destination {
	o.site.mu.Lock()
	defer o.site.mu.Unlock()
	if o.site.routes != nil {
		return o.site.routes
	}
	rows := must(o.C.Cars.DB.Query("SELECT t.id, v.name, v.length_m, v.closed FROM Tracks t JOIN v_tracks v ON v.id = t.id " +
		"WHERE t.MediaName = 'Anthem' AND t.Active = 1 AND v.length_m IS NOT NULL AND v.name IS NOT NULL ORDER BY t.id"))
	byName := map[string]*route{}
	var order []string
	for i := range rows {
		r := &rows[i]
		name := strings.TrimSpace(r.Str("name"))
		dest := ""
		for _, d := range destinations {
			if strings.HasPrefix(name, d+" ") {
				dest = d
				break
			}
		}
		skip := dest == ""
		for _, w := range notRoutes {
			skip = skip || strings.Contains(name, w)
		}
		if skip {
			continue
		}
		rt, ok := byName[name]
		if !ok {
			closed := r.Int("closed") != 0
			rt = &route{ID: r.Int("id"), Name: name[len(dest)+1:], Kind: routeKind(name, closed), Km: pyRound(r.Float("length_m")/1000.0, 2), Circuit: closed, Dest: dest}
			byName[name] = rt
			order = append(order, name)
		}
		rt.Tracks = append(rt.Tracks, r.Int("id"))
	}
	var out []*destination
	for _, d := range destinations {
		var mine []*route
		for _, n := range order {
			if byName[n].Dest == d {
				mine = append(mine, byName[n])
			}
		}
		sort.SliceStable(mine, func(i, j int) bool {
			a, b := mine[i], mine[j]
			if a.Circuit != b.Circuit {
				return a.Circuit
			}
			if a.Km != b.Km {
				return a.Km < b.Km
			}
			return a.Name < b.Name
		})
		if len(mine) > 0 {
			out = append(out, &destination{d, mine})
		}
	}
	o.site.routes = out
	return out
}

func (o *Ops) routes() pyjson.Obj {
	entries := map[int64]map[int64]int64{}
	for _, r := range must(o.C.DB.Query("SELECT track, cls, COUNT(DISTINCT xuid) AS n FROM score_submissions WHERE sb = ? AND st = ? GROUP BY track, cls", festivalBoard[0], festivalBoard[1])) {
		t := r.Int("track")
		if entries[t] == nil {
			entries[t] = map[int64]int64{}
		}
		entries[t][r.Int("cls")] = r.Int("n")
	}
	classes := make([]any, len(rivals.ClassLetters))
	for i, l := range rivals.ClassLetters {
		classes[i] = l
	}
	dests := []any{}
	for _, d := range o.routeCatalog() {
		rs := []any{}
		for _, r := range d.Routes {
			per := map[int64]int64{}
			for _, t := range r.Tracks {
				for cls, n := range entries[t] {
					per[cls] += n
				}
			}
			var keys []int64
			for c := range per {
				keys = append(keys, c)
			}
			sort.Slice(keys, func(i, j int) bool { return keys[i] < keys[j] })
			ent := pyjson.Obj{}
			total := int64(0)
			for _, c := range keys {
				if c >= 0 && int(c) < len(rivals.ClassLetters) {
					ent = append(ent, pyjson.Pair{K: rivals.ClassLetters[c], V: per[c]})
				}
				total += per[c]
			}
			rs = append(rs, append(routeObj(r, true), pyjson.Pair{K: "entries", V: ent}, pyjson.Pair{K: "total", V: total}))
		}
		dests = append(dests, pyjson.Obj{{K: "name", V: d.Name}, {K: "routes", V: rs}})
	}
	return pyjson.Obj{{K: "classes", V: classes}, {K: "destinations", V: dests}}
}

func routeObj(r *route, withTracks bool) pyjson.Obj {
	tr := make([]any, len(r.Tracks))
	for i, t := range r.Tracks {
		tr[i] = t
	}
	o := pyjson.Obj{{K: "id", V: r.ID}, {K: "name", V: r.Name}, {K: "kind", V: r.Kind}, {K: "km", V: r.Km}, {K: "circuit", V: r.Circuit}}
	if withTracks {
		o = append(o, pyjson.Pair{K: "tracks", V: tr})
	}
	return o
}

func (o *Ops) routeBoards(routeID string, limit int) pyjson.Obj {
	id, ok := pyInt(routeID, 10)
	if !ok {
		panic(opsErr(404, "not_found", "unknown route"))
	}
	var dest string
	var rt *route
	for _, d := range o.routeCatalog() {
		for _, r := range d.Routes {
			if r.ID == id && rt == nil {
				dest, rt = d.Name, r
			}
		}
	}
	if rt == nil {
		panic(opsErr(404, "not_found", "unknown route"))
	}
	names := must(o.C.Riv.Names()).Car
	classes := []any{}
	for cls, letter := range rivals.ClassLetters {
		best := map[int64]*scoreboard.Row{}
		var order []int64
		for _, t := range rt.Tracks { // best time per player over all of the route's track ids
			for _, r := range must(o.C.Sb.Top(festivalBoard[0], festivalBoard[1], t, int64(cls), limit)) {
				cur, ok := best[r.Xuid]
				if !ok {
					order = append(order, r.Xuid)
				}
				if !ok || r.Score < cur.Score {
					best[r.Xuid] = r
				}
			}
		}
		ranked := make([]*scoreboard.Row, 0, len(best))
		for _, x := range order {
			ranked = append(ranked, best[x])
		}
		sort.SliceStable(ranked, func(i, j int) bool {
			if ranked[i].Score != ranked[j].Score {
				return ranked[i].Score < ranked[j].Score
			}
			return ranked[i].T < ranked[j].T
		})
		if len(ranked) > limit {
			ranked = ranked[:limit]
		}
		for i, r := range ranked {
			r.Position = i + 1
		}
		classes = append(classes, pyjson.Obj{{K: "class", V: letter}, {K: "rows", V: o.boardRows(ranked, names)}})
	}
	return pyjson.Obj{{K: "destination", V: dest}, {K: "route", V: routeObj(rt, false)}, {K: "classes", V: classes}}
}

func flagsObj(f scorereports.Flags) pyjson.Obj {
	var by any
	if f.FlaggedBy != nil {
		by = *f.FlaggedBy
	}
	return pyjson.Obj{{K: "submissionId", V: f.SubmissionID}, {K: "reports", V: f.Reports}, {K: "suspicious", V: f.Suspicious}, {K: "flaggedBy", V: by}}
}

func (o *Ops) recent(limit int) pyjson.Obj {
	c := o.C
	trackNames := map[int64]string{}
	for _, d := range o.routeCatalog() {
		for _, r := range d.Routes {
			for _, t := range r.Tracks {
				trackNames[t] = d.Name + " " + r.Name
			}
		}
	}
	events := map[int64]string{}
	for _, e := range must(c.Riv.Events()) {
		events[e.ID] = e.Name
	}
	names := must(c.Riv.Names()).Car
	rows := must(c.DB.Query(fmt.Sprintf("SELECT s.id, %s AS report_count, %s AS ghost_flag, s.xuid, s.sb, s.gsid, s.track, s.car, s.cls, s.score, s.clean, s.submitted, COALESCE(p.gamertag, '') AS tag "+
		"FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid WHERE s.st = ? AND s.sb IN (?, ?) AND %s ORDER BY s.submitted DESC, s.id DESC LIMIT ?",
		scorereports.Counts, scorereports.GhostFlag, scorereports.Visible), festivalBoard[1], festivalBoard[0], monthlyBoard[0], limit*3))
	out := []any{}
	for i := range rows {
		r := &rows[i]
		var where, kind string
		var ok bool
		if r.Int("sb") == monthlyBoard[0] {
			where, ok = events[r.Int("gsid")]
			kind = "event"
		} else {
			where, ok = trackNames[r.Int("track")]
			kind = "route"
		}
		if !ok {
			continue
		}
		row := flagsObj(scorereports.FieldsOf(r))
		row = append(row, pyjson.Obj{{K: "gamertag", V: o.gamertag(r.Str("tag"), r.Int("xuid"))}, {K: "xuid", V: r.Int("xuid")}, {K: "kind", V: kind}, {K: "where", V: where},
			{K: "score", V: r.Float("score")}, {K: "car", V: names[r.Int("car")]}, {K: "carId", V: r.Int("car")}, {K: "class", V: classLetter(r.Int("cls"))},
			{K: "clean", V: r.Int("clean") != 0}, {K: "at", V: int64(r.Float("submitted"))}}...)
		out = append(out, row)
		if len(out) >= limit {
			break
		}
	}
	return pyjson.Obj{{K: "runs", V: out}}
}

var likeEscRe = regexp.MustCompile(`([\\%_])`)

func (o *Ops) players(text string, limit int, offset int64) pyjson.Obj {
	c := o.C
	where, args := "", []any{}
	text = pyStrip(text)
	if text != "" {
		like := "%" + likeEscRe.ReplaceAllString(strings.ToLower(text), `\$1`) + "%"
		where, args = "WHERE lower(a.gamertag) LIKE ?", []any{like}
		if x, err := parseIntBase0(text); err == nil {
			where += " OR a.xuid = ?"
			args = append(args, x)
		}
	}
	total := must(c.DB.One("SELECT COUNT(*) FROM accounts a "+where, args...)).IntAt(0)
	now := nowF()
	rows := must(c.DB.Query("SELECT a.xuid, a.gamertag, a.created, a.last_login, a.banned_at, a.ban_until, a.ban_reason, COALESCE(p.career_level, 0) AS level, "+
		"COALESCE(p.last_seen, 0) AS last_seen, COALESCE(x.xp, 0) AS xp FROM accounts a LEFT JOIN players p ON p.xuid = a.xuid "+
		"LEFT JOIN player_xp x ON x.xuid = a.xuid "+where+" ORDER BY GREATEST(a.last_login, COALESCE(p.last_seen, 0)) DESC, a.xuid LIMIT ? OFFSET ?", append(append([]any{}, args...), limit, offset)...))
	list := []any{}
	for i := range rows {
		r := &rows[i]
		bannedAt, until := fptrOf(r, "banned_at"), fptrOf(r, "ban_until")
		var banUntil any
		if until != nil && *until != 0 {
			banUntil = int64(*until)
		}
		list = append(list, pyjson.Obj{{K: "xuid", V: r.Int("xuid")}, {K: "gamertag", V: r.Str("gamertag")}, {K: "created", V: int64(r.Float("created"))},
			{K: "lastLogin", V: int64(r.Float("last_login"))}, {K: "lastSeen", V: int64(r.Float("last_seen"))}, {K: "level", V: db.DisplayLevel(r.Int("level"))},
			{K: "xp", V: int64(r.Float("xp"))}, {K: "online", V: social.Online(r.Int("xuid"))}, {K: "banned", V: auth.BanActive(bannedAt, until, now)},
			{K: "banUntil", V: banUntil}, {K: "banReason", V: r.Any("ban_reason")}})
	}
	return pyjson.Obj{{K: "total", V: total}, {K: "players", V: list}}
}

func fptrOf(r *db.Row, k string) *float64 {
	v := r.Any(k)
	if v == nil {
		return nil
	}
	f := r.Float(k)
	return &f
}

func (o *Ops) authEvents(limit int) pyjson.Obj {
	list := []any{}
	for _, r := range must(o.C.DB.Query("SELECT at, kind, ip, xuid, detail FROM auth_events ORDER BY id DESC LIMIT ?", limit)) {
		list = append(list, pyjson.Obj{{K: "at", V: int64(r.Float("at"))}, {K: "kind", V: r.Any("kind")}, {K: "ip", V: r.Any("ip")}, {K: "xuid", V: r.Any("xuid")}, {K: "detail", V: r.Any("detail")}})
	}
	return pyjson.Obj{{K: "events", V: list}}
}

// ---------------------------------------------------------------- actions

func (o *Ops) playerAction(r *Req, xuidText, action string, d pyjson.Obj) pyjson.Obj {
	c := o.C
	xuid, err := parseIntBase0(xuidText)
	if err != nil {
		panic(opsErr(400, "bad_request", "xuid must be a number"))
	}
	row := must(c.DB.One("SELECT banned_at, ban_until FROM accounts WHERE xuid=?", xuid))
	if row == nil {
		panic(opsErr(404, "no_such_player", "no account with that xuid").with(fmt.Sprintf("%#x, action %s", xuid, pyjson.PyRepr(action))))
	}
	wasBanned := auth.BanActive(fptrOf(row, "banned_at"), fptrOf(row, "ban_until"), nowF())
	switch action {
	case "ban":
		reason := cut(pyStrip(pyjson.PyStr(objAnyOr(d, "reason", ""))), 200)
		if reason == "" {
			reason = "banned by an operator"
		}
		var until *float64
		if dv, has := d.Get("days"); has && dv != nil && dv != "" {
			days, ok := toFloat(dv)
			if !ok {
				panic(opsErr(400, "bad_request", "days must be a number"))
			}
			if !(0 < days && days <= 3650) {
				panic(opsErr(400, "bad_request", "days must be between 0 and 3650"))
			}
			u := nowF() + days*86400
			until = &u
		}
		must(c.Auth.SetBanned(xuid, &reason, until))
		suffix, again := "", ""
		var untilV any
		if until != nil {
			suffix = fmt.Sprintf(" until %d", int64(*until))
			untilV = int64(*until)
		}
		if wasBanned {
			again = " (was banned already, replaced)"
		}
		c.Log(r, fmt.Sprintf("  ops: banned %#x%s: %s%s", xuid, suffix, reason, again))
		return pyjson.Obj{{K: "banned", V: true}, {K: "until", V: untilV}}
	case "unban":
		must(c.Auth.SetBanned(xuid, nil, nil))
		note := ""
		if !wasBanned {
			note = " (was not banned)"
		}
		c.Log(r, fmt.Sprintf("  ops: unbanned %#x%s", xuid, note))
		return pyjson.Obj{{K: "banned", V: false}}
	case "kick":
		n := must(c.Auth.Revoke(xuid))
		c.Log(r, fmt.Sprintf("  ops: dropped %d session(s) of %#x", n, xuid))
		return pyjson.Obj{{K: "sessionsDropped", V: n}}
	}
	panic(opsErr(404, "not_found", "unknown player action"))
}

func objAnyOr(o pyjson.Obj, k string, d any) any {
	if v, ok := o.Get(k); ok {
		return v
	}
	return d
}

func toFloat(v any) (float64, bool) {
	switch x := v.(type) {
	case int64:
		return float64(x), true
	case uint64:
		return float64(x), true
	case float64:
		return x, true
	case bool:
		if x {
			return 1, true
		}
		return 0, true
	case string:
		f, err := strconv.ParseFloat(strings.TrimSpace(strings.ReplaceAll(x, "_", "")), 64)
		return f, err == nil
	}
	return 0, false
}

func (o *Ops) notifications(r *Req, method, what string, rest []string, body []byte) (int, any, *Download) {
	var path string
	if what == "notification-types" {
		if method != "GET" || len(rest) > 0 {
			panic(opsErr(404, "not_found", ""))
		}
		path = "/api/types"
	} else {
		path = "/api/notifications"
		for _, p := range rest {
			path += "/" + p
		}
	}
	status, obj, err := o.C.Api(r, method, path, body)
	if err != nil {
		panic(err)
	}
	if method == "POST" || method == "DELETE" {
		o.C.Log(r, fmt.Sprintf("  ops: %s %s -> %d", method, path, status))
	}
	return status, obj, nil
}

// ---------------------------------------------------------------- settings

func (o *Ops) savedConfig() pyjson.Obj {
	row := must(o.C.DB.One("SELECT value FROM meta WHERE key='ops_config'"))
	if row == nil {
		return pyjson.Obj{}
	}
	v := must(pyjson.Parse(row.Str("value")))
	obj, _ := v.(pyjson.Obj)
	return obj
}

func (o *Ops) savedBlocklist(key string) []any {
	v, _ := o.savedConfig().Get(key)
	if l, ok := v.([]any); ok {
		return l
	}
	return []any{}
}

func (o *Ops) getConfig() pyjson.Obj {
	c := o.C
	cf := c.Launcher.(*Launcher).Config.Get()
	hero, ok := cf.Get("heroImage")
	if !ok {
		hero = ""
	}
	news, ok := cf.Get("news")
	if !ok {
		news = []any{}
	}
	return pyjson.Obj{{K: "env", V: c.Auth.Env}, {K: "auth", V: c.Auth.Mode}, {K: "registration", V: c.Auth.Registration},
		{K: "motd", V: objAny(cf, "motd")}, {K: "maintenance", V: objAny(cf, "maintenance")}, {K: "minLauncher", V: objAny(cf, "minLauncher")}, {K: "minShim", V: objAny(cf, "minShim")},
		{K: "links", V: objAny(cf, "links")}, {K: "heroImage", V: hero}, {K: "news", V: news},
		{K: "blockedNames", V: o.savedBlocklist("blockedNames")}, {K: "blockedWords", V: o.savedBlocklist("blockedWords")}}
}

func isStr(v any) (string, bool) { s, ok := v.(string); return s, ok }

func opsWords(d pyjson.Obj, key string) []any {
	v, _ := d.Get(key)
	l, ok := v.([]any)
	bad := !ok || len(l) > blocklistMax
	if !bad {
		for _, x := range l {
			s, isS := isStr(x)
			if !isS || n(pyStrip(s)) < 2 || n(pyStrip(s)) > 40 {
				bad = true
			}
		}
	}
	if bad {
		panic(opsErr(400, "bad_request", fmt.Sprintf("%s must be a list of at most %d words of 2 to 40 characters", key, blocklistMax)))
	}
	set := map[string]bool{}
	for _, x := range l {
		set[strings.ToLower(pyStrip(x.(string)))] = true
	}
	var words []string
	for w := range set {
		words = append(words, w)
	}
	sort.Strings(words)
	out := make([]any, len(words))
	for i, w := range words {
		out[i] = w
	}
	return out
}

func n(s string) int { return len([]rune(s)) }

func opsText(d pyjson.Obj, key string, limit int) string {
	v, _ := d.Get(key)
	s, ok := isStr(v)
	bad := !ok || n(s) > limit
	for _, ch := range s {
		if ch < 32 && ch != '\n' {
			bad = true
		}
	}
	if bad {
		panic(opsErr(400, "bad_request", fmt.Sprintf("%s must be text of at most %d characters", key, limit)))
	}
	return pyStrip(s)
}

var (
	httpsRe  = regexp.MustCompile(`^https://[^\s]+$`)
	httpRe   = regexp.MustCompile(`^https?://[^\s]+$`)
	verOpsRe = regexp.MustCompile(`^\d{1,4}\.\d{1,4}\.\d{1,4}(-rc\d{1,3})?$`)
	inviteRe = regexp.MustCompile(`^[A-Z0-9-]{4,40}$`)
)

func opsURL(d pyjson.Obj, key string, limit int, httpsOnly bool) string {
	v, _ := d.Get(key)
	s, ok := isStr(v)
	pat, word := httpRe, "http(s)"
	if httpsOnly {
		pat, word = httpsRe, "https"
	}
	if !ok || n(s) > limit || (s != "" && !pat.MatchString(s)) {
		panic(opsErr(400, "bad_request", fmt.Sprintf("%s must be empty or an %s URL", key, word)))
	}
	return s
}

// validateConfig returns the settings to keep (unknown keys are errors, so a typo cannot silently do nothing).
func validateConfig(d pyjson.Obj) pyjson.Obj {
	out := pyjson.Obj{}
	known := map[string]bool{"motd": true, "maintenance": true, "minLauncher": true, "minShim": true, "links": true, "registration": true, "heroImage": true, "news": true,
		"blockedNames": true, "blockedWords": true}
	var unknown []string
	for _, p := range d {
		if !known[p.K] {
			unknown = append(unknown, p.K)
		}
	}
	if len(unknown) > 0 {
		sort.Strings(unknown)
		panic(opsErr(400, "bad_request", "unknown setting(s): "+strings.Join(unknown, ", ")))
	}
	for _, kl := range []struct {
		k string
		l int
	}{{"motd", 500}, {"maintenance", 300}} {
		if _, ok := d.Get(kl.k); ok {
			out.Set(kl.k, opsText(d, kl.k, kl.l))
		}
	}
	for _, k := range []string{"minLauncher", "minShim"} {
		if v, ok := d.Get(k); ok {
			s, isS := isStr(v)
			if !isS || !verOpsRe.MatchString(s) {
				panic(opsErr(400, "bad_request", k+" must look like 1.2.3 or 1.2.3-rc4"))
			}
			out.Set(k, s)
		}
	}
	if lv, ok := d.Get("links"); ok {
		lo, isObj := lv.(pyjson.Obj)
		bad := !isObj
		for _, p := range lo {
			if !inList(linkKeys, p.K) {
				bad = true
			}
		}
		if bad {
			panic(opsErr(400, "bad_request", "links may only have: "+strings.Join(linkKeys, ", ")))
		}
		links := pyjson.Obj{}
		for _, p := range lo {
			s, isS := isStr(p.V)
			if !isS || n(s) > 200 || (s != "" && !httpRe.MatchString(s)) {
				panic(opsErr(400, "bad_request", fmt.Sprintf("links.%s must be empty or an http(s) URL", p.K)))
			}
			links.Set(p.K, s)
		}
		out.Set("links", links)
	}
	if _, ok := d.Get("heroImage"); ok {
		out.Set("heroImage", opsURL(d, "heroImage", 300, true))
	}
	if nv, ok := d.Get("news"); ok {
		nl, isList := nv.([]any)
		if !isList || len(nl) > newsMax {
			panic(opsErr(400, "bad_request", fmt.Sprintf("news must be a list of at most %d cards", newsMax)))
		}
		cards := []any{}
		for i, cv := range nl {
			card, isObj := cv.(pyjson.Obj)
			bad := !isObj
			for _, p := range card {
				found := false
				for _, f := range newsFields {
					found = found || f.k == p.K
				}
				bad = bad || !found
			}
			if tv, _ := card.Get("title"); !truthy(tv) {
				bad = true
			}
			if bad {
				names := make([]string, len(newsFields))
				for j, f := range newsFields {
					names[j] = f.k
				}
				panic(opsErr(400, "bad_request", fmt.Sprintf("news[%d] needs a title and may only have: %s", i, strings.Join(names, ", "))))
			}
			full := pyjson.Obj{}
			for _, f := range newsFields {
				full.Set(f.k, objAnyOr(card, f.k, ""))
			}
			item := pyjson.Obj{}
			for _, f := range newsFields {
				if f.k == "url" || f.k == "image" {
					item.Set(f.k, opsURL(full, f.k, f.limit, f.k == "image"))
				} else {
					item.Set(f.k, opsText(full, f.k, f.limit))
				}
			}
			cards = append(cards, item)
		}
		out.Set("news", cards)
	}
	for _, k := range []string{"blockedNames", "blockedWords"} {
		if _, ok := d.Get(k); ok {
			out.Set(k, opsWords(d, k))
		}
	}
	if rv, ok := d.Get("registration"); ok {
		s, isS := isStr(rv)
		if !isS || !inList(regModes, s) {
			panic(opsErr(400, "bad_request", "registration must be one of "+strings.Join(regModes, ", ")))
		}
		out.Set("registration", s)
	}
	return out
}

func foldAll(l []any) []string {
	out := make([]string, 0, len(l))
	for _, x := range l {
		out = append(out, auth.FoldTag(pyjson.PyStr(x)))
	}
	return out
}

func (o *Ops) applyConfig(saved pyjson.Obj) {
	overlay := pyjson.Obj{}
	for _, p := range saved {
		if p.K != "registration" && p.K != "blockedNames" && p.K != "blockedWords" {
			overlay = append(overlay, p)
		}
	}
	l := o.C.Launcher.(*Launcher)
	l.Config.mu.Lock()
	l.Config.Overlay = overlay
	l.Config.mu.Unlock()
	if rv, ok := saved.Get("registration"); ok {
		o.C.Auth.Registration = pyjson.PyStr(rv)
	}
	names, _ := saved.Get("blockedNames")
	words, _ := saved.Get("blockedWords")
	o.C.Auth.Blocked = append(append([]string(nil), o.C.Auth.BlockedFile...), foldAll(anyList(names))...)
	o.C.Auth.BlockedWords = foldAll(anyList(words))
}

func (o *Ops) setConfig(r *Req, d pyjson.Obj) pyjson.Obj {
	newv := validateConfig(d)
	saved := o.savedConfig()
	before := o.getConfig()
	if lv, ok := newv.Get("links"); ok {
		links := pyjson.Obj{}
		if sv, ok := saved.Get("links"); ok {
			if so, ok := sv.(pyjson.Obj); ok {
				links = cloneObj(so)
			}
		}
		for _, p := range lv.(pyjson.Obj) {
			links.Set(p.K, p.V)
		}
		newv.Set("links", links)
	}
	saved = cloneObj(saved)
	for _, p := range newv {
		saved.Set(p.K, p.V)
	}
	text, err := pyjson.Dumps(saved, pyjson.Opts{})
	mustOK(err)
	mustOK(o.C.DB.Do(func(t *db.Tx) error {
		_, err := t.Exec("INSERT INTO meta (key, value) VALUES ('ops_config', ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", text)
		return err
	}))
	o.applyConfig(saved)
	var keys, moved []string
	for _, p := range newv {
		keys = append(keys, p.K)
	}
	sort.Strings(keys)
	for _, k := range []string{"minLauncher", "minShim", "maintenance", "registration"} { // short settings are written out; texts, links and lists are only named
		if nv, ok := newv.Get(k); ok {
			if bv, _ := before.Get(k); !pyEq(bv, nv) {
				moved = append(moved, fmt.Sprintf("%s %s -> %s", k, pyjson.PyRepr(bv), pyjson.PyRepr(nv)))
			}
		}
	}
	extra := ""
	if len(moved) > 0 {
		extra = " (" + strings.Join(moved, "; ") + ")"
	}
	o.C.Log(r, fmt.Sprintf("  ops: settings changed: %s%s", strings.Join(keys, ", "), extra))
	return o.getConfig()
}

func pyEq(a, b any) bool { return pyjson.PyRepr(a) == pyjson.PyRepr(b) }

// LoadSaved re-applies what the operator last saved at LSP start (the environment's registration mode is only the default).
func (o *Ops) LoadSaved() {
	row := must(o.C.DB.One("SELECT value FROM meta WHERE key='ops_config'"))
	if row != nil {
		v := must(pyjson.Parse(row.Str("value")))
		saved, _ := v.(pyjson.Obj)
		o.applyConfig(saved)
	}
}

func (o *Ops) makeInvite(r *Req, d pyjson.Obj) pyjson.Obj {
	uses := int64(1)
	if uv, ok := d.Get("uses"); ok {
		u, isInt := uv.(int64)
		if !isInt || u < 1 || u > 1000 {
			panic(opsErr(400, "bad_request", "uses must be a whole number from 1 to 1000"))
		}
		uses = u
	}
	note := cut(pyjson.PyStr(objAnyOr(d, "note", "")), 100)
	code := strings.ToUpper(pyStrip(pyjson.PyStr(objAnyOr(d, "code", ""))))
	if code != "" {
		if !inviteRe.MatchString(code) {
			panic(opsErr(400, "bad_request", "a code is 4-40 letters, digits or dashes"))
		}
	} else {
		pick := func() string {
			b := make([]byte, 4)
			for i := range b {
				var x [1]byte
				for {
					rand.Read(x[:])
					if int(x[0]) < 256-256%len(inviteAlphabet) {
						break
					}
				}
				b[i] = inviteAlphabet[int(x[0])%len(inviteAlphabet)]
			}
			return string(b)
		}
		code = "FH2-" + pick() + "-" + pick()
	}
	mustOK(o.C.Auth.AddInvite(code, int(uses), note))
	o.C.Log(r, fmt.Sprintf("  ops: invite created (%d use(s)) %s", uses, pyjson.PyRepr(note)))
	return pyjson.Obj{{K: "code", V: code}, {K: "uses", V: uses}, {K: "note", V: note}}
}

// ---------------------------------------------------------------- score reports (website reports and moderation)

var hex64Re = regexp.MustCompile(`^[0-9a-f]{64}$`)

func (o *Ops) reportRun(submissionID int64, payload pyjson.Obj) pyjson.Obj {
	c := o.C
	browser, ip := objAnyOr(payload, "browser", ""), objAnyOr(payload, "ip", "")
	bs, bok := isStr(browser)
	is, iok := isStr(ip)
	if !(bok && iok && hex64Re.MatchString(bs) && hex64Re.MatchString(is)) {
		reportsLog.Warn(fmt.Sprintf("report on run %d refused: the reporter identity is not a 64-digit hex hash (browser is %s, ip is %s); the website sends hashes, so this did not come from it",
			submissionID, pyjson.TypeName(browser), pyjson.TypeName(ip)))
		panic(opsErr(400, "bad_request", "invalid reporter identity"))
	}
	now := nowF()
	var total int64
	err := c.DB.Do(func(t *db.Tx) error {
		row, err := t.One("SELECT s.id FROM score_submissions s WHERE s.id = ? AND s.sb IN (1,3,6,9) AND "+scorereports.Visible, submissionID)
		if err != nil {
			return err
		}
		if row == nil {
			reportsLog.Info(fmt.Sprintf("report on run %d refused: not on a reviewable board, hidden already, or unknown", submissionID))
			return opsErr(404, "not_found", "this run is no longer available")
		}
		dup, err := t.One("SELECT 1 FROM score_reports WHERE submission_id = ? AND (browser = ? OR ip = ?)", submissionID, bs, is)
		if err != nil {
			return err
		}
		if dup != nil {
			reportsLog.Debug(fmt.Sprintf("report on run %d ignored: this browser or address reported it already", submissionID))
			return &reportDup{}
		}
		cnt, err := t.One("SELECT COUNT(*) FROM score_reports WHERE created >= ? AND (browser = ? OR ip = ?)", now-3600, bs, is)
		if err != nil {
			return err
		}
		if cnt.IntAt(0) >= 20 {
			reportsLog.Warn(fmt.Sprintf("report on run %d refused: %d reports from this reporter in the last hour (limit 20) %s", submissionID, cnt.IntAt(0),
				diag.KV(diag.KVP{K: "browser", V: bs[:8]})))
			return opsErr(429, "rate_limited", "Too many reports. Try again later.")
		}
		if _, err := t.Exec("INSERT INTO score_reports (submission_id, browser, ip, created) VALUES (?,?,?,?)", submissionID, bs, is, now); err != nil {
			return err
		}
		tr, err := t.One("SELECT COUNT(*) FROM score_reports WHERE submission_id = ? AND dismissed = 0", submissionID)
		total = tr.IntAt(0)
		return err
	})
	if _, dup := err.(*reportDup); dup {
		return pyjson.Obj{{K: "reported", V: false}, {K: "message", V: "You have already reported this run."}}
	}
	mustOK(err)
	var marked any
	switch {
	case total == scorereports.Threshold:
		marked = "now"
	case total > scorereports.Threshold:
		marked = true
	}
	reportsLog.Info(fmt.Sprintf("run %d reported %s", submissionID, diag.KV(diag.KVP{K: "open_reports", V: total}, diag.KVP{K: "threshold", V: scorereports.Threshold},
		diag.KVP{K: "marked_suspicious", V: marked}, diag.KVP{K: "browser", V: bs[:8]})))
	o.site.resetHistory()
	return pyjson.Obj{{K: "reported", V: true}, {K: "message", V: "Run reported. Thank you."}}
}

type reportDup struct{}

func (*reportDup) Error() string { return "duplicate report" }

var reportsLog = diag.Get("reports")

// reportsQueue lists reported runs, runs hidden by a person or a verdict, and runs a verdict flagged. A run still waiting for its ghost is not a decision.
func (o *Ops) reportsQueue(limit int, offset int64) pyjson.Obj {
	c := o.C
	rows := must(c.DB.Query(fmt.Sprintf(`SELECT s.*, COALESCE(p.gamertag, '') AS tag, %s AS report_count,
        (SELECT hidden_at FROM hidden_scores hs WHERE hs.submission_id = s.id AND hs.source <> 'pending') AS hidden_at,
        (SELECT hs.source FROM hidden_scores hs WHERE hs.submission_id = s.id) AS hidden_by,
        v.verdict, v.reasons
        FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid LEFT JOIN score_verdicts v ON v.submission_id = s.id
        WHERE EXISTS (SELECT 1 FROM score_reports sr WHERE sr.submission_id = s.id AND sr.dismissed = 0)
        OR EXISTS (SELECT 1 FROM hidden_scores hs WHERE hs.submission_id = s.id AND hs.source <> 'pending')
        OR (v.verdict = 'flag' AND v.reviewed = 0)
        ORDER BY report_count DESC, s.id DESC LIMIT ? OFFSET ?`, scorereports.Counts), limit+1, offset))
	names := must(c.Riv.Names()).Car
	out := []any{}
	for i := range rows {
		if i >= limit {
			break
		}
		r := &rows[i]
		unit := "m/s"
		if r.Int("st") == 1 {
			unit = "s"
		}
		cls := r.Int("cls")
		clsText := strconv.FormatInt(cls, 10)
		if cls >= 0 && int(cls) < len(rivals.ClassLetters) {
			clsText = rivals.ClassLetters[cls]
		}
		hidden := r.Any("hidden_at") != nil
		var hiddenBy any
		if hidden {
			hiddenBy = r.Any("hidden_by")
		}
		reasons := any([]any{})
		if rs := r.Str("reasons"); rs != "" {
			reasons = must(pyjson.Parse(rs))
		}
		out = append(out, pyjson.Obj{{K: "submissionId", V: r.Int("id")}, {K: "gamertag", V: o.gamertag(r.Str("tag"), r.Int("xuid"))}, {K: "xuid", V: r.Int("xuid")},
			{K: "score", V: r.Float("score")}, {K: "unit", V: unit}, {K: "car", V: names[r.Int("car")]}, {K: "class", V: clsText}, {K: "boardType", V: r.Int("sb")},
			{K: "track", V: r.Int("track")}, {K: "event", V: r.Int("gsid")}, {K: "at", V: int64(r.Float("submitted"))}, {K: "reports", V: r.Int("report_count")},
			{K: "hidden", V: hidden}, {K: "hiddenBy", V: hiddenBy}, {K: "verdict", V: r.Any("verdict")}, {K: "reasons", V: reasons}})
	}
	return pyjson.Obj{{K: "entries", V: out}, {K: "hasNext", V: len(rows) > limit}}
}

func (o *Ops) moderate(submissionID int64, action string) pyjson.Obj {
	if action != "hide" && action != "dismiss" && action != "unhide" {
		reportsLog.Warn(fmt.Sprintf("moderation of run %d refused: unknown action %s, expected hide, dismiss or unhide", submissionID, pyjson.PyRepr(cut(action, 20))))
		panic(opsErr(400, "bad_request", "unknown moderation action"))
	}
	now := nowF()
	mustOK(o.C.DB.Do(func(t *db.Tx) error {
		row, err := t.One("SELECT id FROM score_submissions WHERE id = ? AND sb IN "+scorereports.ReviewBoards, submissionID)
		if err != nil {
			return err
		}
		if row == nil {
			reportsLog.Warn(fmt.Sprintf("moderation %s of run %d refused: no such run on a reviewable board %s", action, submissionID, scorereports.ReviewBoards))
			return opsErr(404, "not_found", "unknown run")
		}
		switch action {
		case "hide":
			if _, err := t.Exec("INSERT INTO hidden_scores (submission_id, hidden_at, source) VALUES (?,?,'operator') ON CONFLICT (submission_id) DO UPDATE SET source = 'operator'", submissionID, now); err != nil {
				return err
			}
			_, err = t.Exec("DELETE FROM score_overrides WHERE submission_id = ?", submissionID)
		case "unhide":
			if _, err := t.Exec("DELETE FROM hidden_scores WHERE submission_id = ?", submissionID); err != nil {
				return err
			}
			if _, err := t.Exec("INSERT INTO score_overrides (submission_id, at) VALUES (?,?) ON CONFLICT (submission_id) DO NOTHING", submissionID, now); err != nil {
				return err
			}
			_, err = t.Exec("UPDATE score_verdicts SET reviewed = 1 WHERE submission_id = ?", submissionID)
		default:
			if _, err := t.Exec("UPDATE score_reports SET dismissed = 1 WHERE submission_id = ?", submissionID); err != nil {
				return err
			}
			_, err = t.Exec("UPDATE score_verdicts SET reviewed = 1 WHERE submission_id = ?", submissionID)
		}
		return err
	}))
	reportsLog.Info(fmt.Sprintf("operator %s of run %d", action, submissionID))
	o.site.resetHistory()
	return ok1()
}

var (
	_ = json.Marshal
	_ = math.Abs
	_ = unicode.IsSpace
)
