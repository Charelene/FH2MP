package lsp

import (
	"errors"
	"fmt"
	"hash/fnv"
	"io"
	"net"
	"net/http"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"fh2mp/internal/auth"
	"fh2mp/internal/diag"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/social"
	"fh2mp/internal/ugc"
	"fh2mp/internal/xbl"
)

// LauncherAPI is what the HTTP front needs of the launcher API (/v1/*): the minimum shim, the car database manifest and the request handler.
type LauncherAPI interface {
	MinShim() string
	VersionAtLeast(have, need string) bool
	BodyLimit(path string) int64
	// GameDBInfo is the sha256 of the server's car database and its path; ok=false when it cannot be read.
	GameDBInfo() (sha, path string, ok bool)
	Handle(r *Req, method, path string, h http.Header, body []byte, ip string) (status int, obj any, ctype string)
}

// Download is a file the ops API hands out.
type Download struct {
	Data              []byte
	ContentType, Name string
}

// OpsAPI is the operator API (/ops/*).
type OpsAPI interface {
	Handle(r *Req, method, path string, h http.Header, body []byte, peer string, proxied bool) (status int, obj any, dl *Download)
}

const ctypeXtsw = "bin/xtsw"

var (
	socialHosts      = []string{"profile.xboxlive.com", "social.xboxlive.com", "peoplehub.xboxlive.com", "userpresence.xboxlive.com"}
	multiplayerHosts = []string{"momatch.xboxlive.com", "sessiondirectory.xboxlive.com", "gameserverds.xboxlive.com", "gameserverms.xboxlive.com"}
)

func inList(l []string, s string) bool {
	for _, x := range l {
		if x == s {
			return true
		}
	}
	return false
}

// frontState is the front's memory of what it has already told the log or the game server.
type frontState struct {
	mu            sync.Mutex
	oldRemoved    map[int64]time.Time
	ownCars       map[int64]time.Time
	toolsAnnounce map[int64]toolsEntry
	authAnnounce  map[authKey]time.Time
	lastPoll      map[string]uint64
}

type toolsEntry struct {
	tools string
	at    time.Time
}

type authKey struct {
	xuid int64
	ip   string
	tkey string
}

var started = time.Now()

func (c *Core) slowSeconds() float64 {
	if s := os.Getenv("FH2MP_LSP_SLOW_S"); s != "" {
		if f, err := strconv.ParseFloat(s, 64); err == nil {
			return f
		}
	}
	return 2
}

// unchanged is true when a poll got the same answer as the same player's last poll of the same resource: a repeat is not news.
func (c *Core) unchanged(key string, sig uint64) bool {
	f := &c.front
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.lastPoll == nil || len(f.lastPoll) > 5000 {
		f.lastPoll = map[string]uint64{}
	}
	old, ok := f.lastPoll[key]
	f.lastPoll[key] = sig
	return ok && old == sig
}

func hashBytes(b []byte) uint64 {
	h := fnv.New64a()
	h.Write(b)
	return h.Sum64()
}

func headerCallName(h http.Header) string {
	return callName(h.Get("X-ClassName"), h.Get("X-Method"), h.Get("X-FH2MP-Host"))
}

// claimedXuid is the X-UserId a request claims (not verified), 0 when absent or unreadable.
func claimedXuid(h http.Header) int64 {
	s := h.Get("X-UserId")
	if s == "" {
		return 0
	}
	n, err := parseIntBase0(s)
	if err != nil {
		return 0
	}
	return n
}

func peerIP(req *http.Request) string {
	host, _, err := net.SplitHostPort(req.RemoteAddr)
	if err != nil {
		return req.RemoteAddr
	}
	return host
}

// clientIP is the caller's address: behind the local reverse proxy (nginx sets X-Real-IP) the header, otherwise the socket peer.
func clientIP(req *http.Request) string {
	peer := peerIP(req)
	if peer == "127.0.0.1" || peer == "::1" {
		if real := req.Header.Get("X-Real-IP"); real != "" {
			return auth.NormIP(real)
		}
	}
	return auth.NormIP(peer)
}

func isLoopback(ip string) bool { return ip == "127.0.0.1" || ip == "::1" }

type respWriter struct {
	w http.ResponseWriter
}

func (rw respWriter) send(status int, ctype string, body []byte, extra ...[2]string) {
	h := rw.w.Header()
	h.Set("Content-Type", ctype)
	for _, kv := range extra {
		h.Set(kv[0], kv[1])
	}
	h.Set("Content-Length", strconv.Itoa(len(body)))
	rw.w.WriteHeader(status)
	rw.w.Write(body)
}

func (rw respWriter) json(status int, obj any, ctype string, extra ...[2]string) {
	var b []byte
	switch x := obj.(type) {
	case []byte:
		b = x
	default:
		s, err := pyjson.Dumps(obj, pyjson.Opts{})
		if err != nil {
			panic(err)
		}
		b = []byte(s)
	}
	if ctype == "" {
		ctype = "application/json"
	}
	rw.send(status, ctype, b, extra...)
}

// ServeHTTP is the LSP's front: it identifies the caller and routes the request. Anything unhandled is logged with the request's number and how long it ran,
// and the connection is closed without a reply.
func (c *Core) ServeHTTP(w http.ResponseWriter, req *http.Request) {
	started := time.Now()
	r := &Req{Seq: c.nextSeq()}
	path := req.RequestURI
	short := func() string { return cut(strings.SplitN(path, "?", 2)[0], 80) }
	defer func() {
		took := time.Since(started).Seconds()
		if p := recover(); p != nil {
			if err, ok := p.(error); ok && isClientGone(err) {
				c.LogEvery(r, "client-gone/"+fmt.Sprintf("%T", err), 300, fmt.Sprintf("  client went away or stalled before the exchange finished (%T) after %.1f s: %s %s %s",
					err, took, req.Method, short(), headerCallName(req.Header)), diag.Info, "")
				panic(http.ErrAbortHandler)
			}
			if p == http.ErrAbortHandler {
				panic(p)
			}
			err, _ := p.(error)
			if err == nil {
				err = fmt.Errorf("%v", p)
			}
			c.LogExc(r, fmt.Sprintf("  unhandled error in %s %s %s after %.1f s; the connection is closed without a reply", req.Method, short(), headerCallName(req.Header), took), err, diag.Error)
			panic(http.ErrAbortHandler)
		}
		if took >= c.slowSeconds() {
			c.LogEvery(r, "slow/"+headerCallName(req.Header), 60, fmt.Sprintf("  slow request: %s %s %s took %.1f s", req.Method, short(), headerCallName(req.Header), took), diag.Warn, "")
		}
	}()
	c.handle(respWriter{w}, r, req)
}

func isClientGone(err error) bool {
	var ne net.Error
	return errors.Is(err, io.ErrUnexpectedEOF) || errors.Is(err, io.EOF) || (errors.As(err, &ne) && ne.Timeout()) || strings.Contains(err.Error(), "connection reset") ||
		strings.Contains(err.Error(), "broken pipe") || strings.Contains(err.Error(), "connection aborted")
}

func (c *Core) handle(rw respWriter, r *Req, req *http.Request) {
	h := req.Header
	path := req.RequestURI
	rawPath, _, _ := strings.Cut(path, "?")
	short := cut(rawPath, 80)
	r.Run = diag.RunID(h.Get("X-FH2MP-Run"))
	n := req.ContentLength
	if n < 0 {
		n = 0
	}
	limit := int64(ugc.MaxRequestBody)
	if strings.HasPrefix(path, "/v1/") && c.Launcher != nil {
		limit = c.Launcher.BodyLimit(path)
	}
	shown := limit
	if strings.HasPrefix(path, "/ops/") {
		shown = min(limit, 65536)
	}
	if n > limit || (strings.HasPrefix(path, "/ops/") && n > 65536) {
		c.LogEvery(r, "too-large/"+cut(rawPath, 40), 300, fmt.Sprintf("  refused: 413 too_large: %s %s announces %d bytes, limit %d", req.Method, short, n, shown), diag.Warn, "")
		rw.w.Header().Set("Connection", "close")
		rw.json(413, pyjson.Obj{{K: "error", V: "too_large"}}, "")
		return
	}
	var body []byte
	if n > 0 {
		body = make([]byte, n)
		if _, err := io.ReadFull(req.Body, body); err != nil {
			panic(err)
		}
	}
	proxied := h.Get("X-Real-IP") != "" || h.Get("X-Forwarded-For") != ""
	peer := peerIP(req)
	if strings.HasPrefix(path, "/ops/") {
		c.serveOps(rw, r, req, body, proxied)
		return
	}
	if (strings.HasPrefix(path, "/admin/") || strings.HasPrefix(path, "/api/")) && (!isLoopback(peer) || proxied) {
		c.LogEvery(r, "local-only/"+cut(rawPath, 20), 300, fmt.Sprintf("  refused: 403 on %s: operator and game-server endpoints answer loopback only, never through the proxy (came through the proxy: %s)",
			short, pyBool(proxied)), diag.Warn, "")
		rw.send(403, "text/plain", nil) // operator and game-server endpoints are loopback only (and never through the proxy)
		return
	}
	if strings.HasPrefix(path, "/v1/") {
		c.serveV1(rw, r, req, body)
		return
	}
	if req.Method == "GET" && strings.HasPrefix(path, "/gamerpic/") { // public: the client downloads pictures from here
		c.serveGamerpic(rw, r, path)
		return
	}
	if strings.HasPrefix(path, "/admin/") {
		c.serveAdmin(rw, r, req, body)
		return
	}
	if strings.HasPrefix(path, "/api/") {
		c.serveAPI(rw, r, req, body)
		return
	}
	host, cls, method := h.Get("X-FH2MP-Host"), h.Get("X-ClassName"), h.Get("X-Method")
	if !c.identify(rw, r, req) {
		return
	}
	who := r.Xuid
	if who == 0 {
		who, _ = strconv.ParseInt(c.xblXuid(r, h), 10, 64)
	}
	social.Touch(who)
	if r.Run != "" && r.Authed { // only a signed-in caller can put a run id on a player's lines
		c.noteRun(r, r.Run, r.Xuid)
	}
	runPart := ""
	if r.Run != "" {
		runPart = " run=" + r.Run
	}
	reqLine := fmt.Sprintf("#%d %s %s%s %s %s body=%d user=%#x%s", r.Seq, req.Method, host, path, cls, method, len(body), r.Xuid, runPart)
	if (method == "RetrieveForUser" && strings.HasPrefix(cls, "Xls.WebServices.Notifications")) || inList(socialHosts, host) {
		name := host
		if method != "" {
			name = headerCallName(h)
		}
		if line := c.pollShare.note(name, r.Xuid); line != "" { // polls are counted, not listed
			c.Log(nil, line)
		}
		c.Debug(r, "poll", reqLine)
	} else {
		c.Log(r, reqLine)
	}
	if r.OwnCars && inList(multiplayerHosts, host) {
		mine, theirs := r.GameDB[0], r.GameDB[1]
		if mine == "" {
			mine = "(none sent)"
		}
		shim := r.Shim
		if shim == "" {
			shim = "(none)"
		}
		c.LogEvery(r, fmt.Sprintf("mp-refused/%d", r.Xuid), 120, "  multiplayer refused: this player's car database is not the server's "+diag.KV(diag.KVP{K: "xuid", V: fmt.Sprintf("%#x", r.Xuid)},
			diag.KVP{K: "host", V: host}, diag.KVP{K: "client_db", V: mine}, diag.KVP{K: "server_db", V: theirs}, diag.KVP{K: "shim", V: shim}), diag.Info, "")
		rw.json(409, pyjson.Obj{{K: "error", V: "gamedb_mismatch"}, {K: "message", V: "Playing with other people needs the server's car database. Press Play in the FH2MP launcher without keeping your own car packs."}}, "")
		return
	}
	var out []byte
	if strings.HasSuffix(host, "forzamotorsport.net") && cls != "" {
		out = c.xlsSafe(r, req, cls, method, body)
	}
	ctype, status := ctypeXtsw, 200
	var extra [][2]string
	trail := strings.TrimRight(path, "/")
	switch {
	case host == "gameserverds.xboxlive.com" && strings.HasSuffix(path, "/variants"):
		out, ctype = gsdsVariants(), "application/json"
		c.Log(r, fmt.Sprintf("  GSDS variants -> %d sets Release_%s.*, max %d players", 3, GSIVersion, maxLobbyPlayers()))
		c.Debug(r, "gsds", "  GSDS variants body: "+string(out))
	case host == "gameserverds.xboxlive.com" && strings.Contains(path, "qosservers"):
		out, ctype = gsdsQosServers(), "application/json"
		c.Log(r, "  GSDS qos -> 1 server(s)")
		c.Debug(r, "gsds", "  GSDS qos body: "+string(out))
	case host == "gameserverms.xboxlive.com" && strings.HasSuffix(trail, "/clusters"):
		out, ctype = gsmsCluster(), "application/json"
		c.Log(r, "  GSMS cluster -> "+string(out))
	case host == "client-strings.xboxlive.com" && strings.HasSuffix(trail, "/system/strings/validate"):
		verified, err := verifyStrings(body)
		if err != nil {
			c.Warn(r, fmt.Sprintf("  refused: 400 invalid_strings on strings/validate: %v (body %d bytes, user=%#x)", err, len(body), r.Xuid))
			rw.json(400, pyjson.Obj{{K: "error", V: "invalid_strings"}}, "")
			return
		}
		s, _ := pyjson.Dumps(verified, pyjson.Opts{})
		out, ctype = []byte(s), "application/json"
		vr, _ := verified.Get("verifyStringResult")
		c.Log(r, fmt.Sprintf("  strings/validate: %d string(s) accepted", len(anyList(vr))))
	case inList(socialHosts, host) && c.trySocial(r, host, req, path, body, &status, &out):
		ctype = "application/json"
	case host == "achievements.xboxlive.com":
		// The game polls GetAchievementsForTitleIdAsync every frame it is set; a 200 with no usable list makes the embedded XSAPI complete with a NULL result
		// and the game dereferences it (docs/matchmaking.md). Fail it.
		out, ctype, status = []byte(`{"code":0,"description":"achievements unavailable"}`), "application/json", 404
		c.LogEvery(r, fmt.Sprintf("achievements/%d", r.Xuid), 300, "  achievements -> 404 (on purpose: any 200 makes the game dereference a null result)", diag.Info, "")
	case host == "sessiondirectory.xboxlive.com" && mpsdPathOK(path):
		out, status, ctype, extra = c.serveMPSD(r, req, path, body)
	case host == "momatch.xboxlive.com" && momatchOK(path):
		out, status, ctype = c.serveMomatch(r, req, path, body)
	case host == "sessiondirectory.xboxlive.com" && req.Method == "GET" && strings.Contains(path, "/sessions"):
		// MPSD session query (MultiplayerService.GetSessionsAsync). Any 200 answer makes the embedded XSAPI complete the async with a NULL result and the game
		// dereferences it (crash at game+0x41fd28). An HTTP error leaves the async in the Error state, which the game's poll loop skips.
		out, ctype, status = []byte(`{"code":0,"description":"no sessions"}`), "application/json", 404
		c.LogEvery(r, fmt.Sprintf("mpsd-sessions/%d", r.Xuid), 300, "  MPSD sessions -> 404 (on purpose: any 200 makes the game dereference a null result)", diag.Info, "")
	case !(strings.HasSuffix(host, "forzamotorsport.net") && cls != ""):
		c.LogEvery(r, fmt.Sprintf("unhandled-host/%s/%s", host, req.Method), 3600, fmt.Sprintf("  unhandled request %s %s%s: answered 200 with an empty body (user %#x, body %d bytes)",
			req.Method, host, cut(rawPathOf(path), 60), r.Xuid, len(body)), diag.Info, "")
	}
	rw.send(status, ctype, out, extra...)
}

func rawPathOf(p string) string { s, _, _ := strings.Cut(p, "?"); return s }

func mpsdPathOK(path string) bool {
	_, ok := xbl.ParsePath(path)
	return ok
}

func momatchOK(path string) bool {
	_, _, _, ok := xbl.ParseMomatch(path)
	return ok
}

// xlsSafe runs an Xls call; a failure in its handler is logged and answered with an error result.
func (c *Core) xlsSafe(r *Req, req *http.Request, cls, method string, body []byte) (out []byte) {
	defer func() {
		if p := recover(); p != nil {
			if p == http.ErrAbortHandler {
				panic(p)
			}
			err, ok := p.(error)
			if !ok {
				err = fmt.Errorf("%v", p)
			}
			c.LogExc(r, fmt.Sprintf("  %s failed in its handler (user=%#x, body %d bytes), answered with an error result", headerCallName(req.Header), r.Xuid, len(body)), err, diag.Error)
			out = frame(nil, 2, 1, []byte(fmt.Sprintf("fh2mp: %v", err)))
		}
	}()
	return c.handleXls(r, cls, method, body)
}

func (c *Core) trySocial(r *Req, host string, req *http.Request, path string, body []byte, status *int, out *[]byte) bool {
	st, obj, ok := c.xblSocial(r, host, req.Method, path, body, req.Header)
	if !ok {
		return false
	}
	s, err := pyjson.Dumps(obj, pyjson.Opts{})
	if err != nil {
		panic(err)
	}
	*status, *out = st, []byte(s)
	return true
}

func objGet(v any, keys ...string) any {
	for _, k := range keys {
		o, ok := v.(pyjson.Obj)
		if !ok {
			return nil
		}
		v, _ = o.Get(k)
	}
	return v
}

func (c *Core) serveMPSD(r *Req, req *http.Request, path string, body []byte) (out []byte, status int, ctype string, extra [][2]string) {
	cmd := req.Method
	caller := c.xblXuid(r, req.Header)
	var doc pyjson.Obj
	switch {
	case caller == "" && c.Auth != nil && c.Auth.Mode == "required" && cmd != "GET":
		out, status = []byte("{}"), 401 // session writes need a signed-in caller
		c.LogEvery(r, fmt.Sprintf("mpsd-401/%d", r.Xuid), 300, fmt.Sprintf("  MPSD %s refused 401: no signed-in caller (FH2MP_AUTH=required, user %#x)", cmd, r.Xuid), diag.Warn, "")
	case cmd == "DELETE":
		out, status = nil, c.Xbl.DeleteSession(path, caller)
	default:
		var st int
		if cmd == "PUT" || cmd == "POST" {
			st, doc = c.Xbl.PutSession(path, body, caller)
		} else {
			st, doc = c.Xbl.GetSession(path, caller)
		}
		status = st
		if doc != nil && req.Header.Get("X-FH2MP-Tunnel") != "" {
			doc = xbl.ForTunnel(doc) // a client on the TCP fallback reaches the game server through its own machine
		}
		if doc != nil {
			s, _ := pyjson.Dumps(doc, pyjson.Opts{})
			out = []byte(s)
		} else {
			out = []byte("{}")
		}
	}
	ctype = "application/json"
	if doc != nil {
		extra = append(extra, [2]string{"ETag", xbl.ETag(doc)})
	}
	who := ""
	if doc != nil && !strings.Contains(path, "defaultTemplate_qos") {
		var parts []string
		if mem, ok := objGet(doc, "members").(pyjson.Obj); ok {
			for _, m := range mem {
				x, _ := objGet(m.V, "constants", "system", "xuid").(string)
				sign := "-"
				if truthy(objGet(m.V, "properties", "system", "active")) {
					sign = "+"
				}
				parts = append(parts, lastN(x, 5)+sign)
			}
		}
		who = " members=" + strings.Join(parts, ",")
	}
	line := fmt.Sprintf("  MPSD %s session -> %d (%d bytes, %d session(s))%s", cmd, status, len(out), c.Xbl.Count(), who)
	if cmd == "GET" && c.unchanged(fmt.Sprintf("mpsd/%s/%s", caller, rawPathOf(path)), hashBytes(append([]byte{byte(status), byte(status >> 8)}, out...))) {
		c.Debug(r, "poll", line) // the same answer as this player's last poll of this session
	} else {
		c.Log(r, line)
	}
	return
}

func (c *Core) serveMomatch(r *Req, req *http.Request, path string, body []byte) (out []byte, status int, ctype string) {
	caller := c.xblXuid(r, req.Header)
	var doc pyjson.Obj
	switch {
	case caller == "" && c.Auth != nil && c.Auth.Mode == "required":
		status = 401 // matchmaking needs a signed-in caller
		c.LogEvery(r, fmt.Sprintf("momatch-401/%d", r.Xuid), 300, fmt.Sprintf("  MOMATCH %s refused 401: no signed-in caller (FH2MP_AUTH=required, user %#x)", req.Method, r.Xuid), diag.Warn, "")
	case req.Method == "DELETE":
		status = c.Xbl.DeleteTicket(path, caller)
	case req.Method == "POST":
		status, doc = c.Xbl.CreateTicket(path, body, caller)
	default:
		status, doc = c.Xbl.GetTicket(path, caller)
	}
	if doc != nil {
		s, _ := pyjson.Dumps(doc, pyjson.Opts{})
		out = []byte(s)
	}
	ctype = "application/json"
	line := fmt.Sprintf("  MOMATCH %s -> %d %s", req.Method, status, cut(string(out), 300))
	switch {
	case req.Method == "GET" && status < 400 && c.unchanged(fmt.Sprintf("momatch/%s/%s", caller, rawPathOf(path)), hashBytes(append([]byte{byte(status), byte(status >> 8)}, out...))):
		c.Debug(r, "poll", line) // the ticket is polled every 5 s: only a changed answer is news
	case req.Method == "GET" && status == 404:
		c.Warn(r, line+" (no such ticket: expired, swept, or never made)")
	default:
		c.Log(r, line)
	}
	return
}

func (c *Core) serveV1(rw respWriter, r *Req, req *http.Request, body []byte) {
	if c.Launcher == nil {
		rw.json(404, pyjson.Obj{{K: "error", V: "not_found"}}, "")
		return
	}
	status, obj, ctype := c.Launcher.Handle(r, req.Method, req.RequestURI, req.Header, body, clientIP(req))
	cache := "no-store"
	if ctype != "application/json" {
		cache = "max-age=60"
	}
	rw.json(status, obj, ctype, [2]string{"Cache-Control", cache})
}

func (c *Core) serveOps(rw respWriter, r *Req, req *http.Request, body []byte, proxied bool) {
	if c.Ops == nil {
		rw.json(404, pyjson.Obj{{K: "error", V: "not_found"}}, "")
		return
	}
	status, obj, dl := c.Ops.Handle(r, req.Method, req.RequestURI, req.Header, body, peerIP(req), proxied)
	if dl != nil {
		rw.json(status, dl.Data, dl.ContentType, [2]string{"Cache-Control", "private, no-store"}, [2]string{"X-Content-Type-Options", "nosniff"},
			[2]string{"Content-Disposition", fmt.Sprintf("attachment; filename=%q", dl.Name)})
		return
	}
	ctype := "application/json"
	if b, ok := obj.([]byte); ok {
		ctype = "image/png"
		if len(b) >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff {
			ctype = "image/jpeg"
		}
	}
	rw.json(status, obj, ctype, [2]string{"Cache-Control", "no-store"})
}

var gamerpicRe = regexp.MustCompile(`^/gamerpic/(\d+)\.png`)

func (c *Core) serveGamerpic(rw respWriter, r *Req, path string) {
	var png []byte
	m := gamerpicRe.FindStringSubmatch(path)
	if m != nil {
		x, _ := strconv.ParseInt(m[1], 10, 64)
		var err error
		if png, err = c.Social.Gamerpic(x); err != nil {
			panic(err)
		}
	}
	status := 404
	if len(png) > 0 {
		status = 200
	}
	rw.send(status, "image/png", png, [2]string{"Cache-Control", "max-age=60"})
	if len(png) > 0 {
		c.Debug(r, "social", fmt.Sprintf("  gamerpic %s served (%dB)", lastN(m[1], 5), len(png)))
	}
}

func (c *Core) serveAPI(rw respWriter, r *Req, req *http.Request, body []byte) {
	path := req.RequestURI
	if res := c.socialAPI(r, req.Method, path, body); res != nil {
		if res.raw != nil || res.obj == nil {
			rw.send(res.status, "image/png", res.raw)
			return
		}
		rw.send(res.status, "application/json", dumpIndent(res.obj))
		return
	}
	status, obj, err := c.Api(r, req.Method, path, body)
	if err != nil {
		ae, ok := err.(*ApiError)
		if !ok {
			panic(err)
		}
		c.Warn(r, fmt.Sprintf("  admin api refused %d: %s %s: %s", ae.Status, req.Method, cut(rawPathOf(path), 60), ae.Msg))
		status, obj = ae.Status, pyjson.Obj{{K: "error", V: ae.Msg}}
	}
	rw.send(status, "application/json", dumpIndent(obj))
}

func dumpIndent(obj any) []byte {
	s, err := pyjson.Dumps(obj, pyjson.Opts{Indent: 1})
	if err != nil {
		panic(err)
	}
	return []byte(s)
}

func (c *Core) serveAdmin(rw respWriter, r *Req, req *http.Request, body []byte) {
	path := req.RequestURI
	if rawPathOf(path) == "/admin/result" && req.Method == "POST" {
		if c.AddResult == nil {
			rw.json(404, pyjson.Obj{{K: "error", V: "not_found"}}, "")
			return
		}
		rid, err := c.AddResult(body)
		if err != nil {
			if _, bad := err.(*valErr); bad {
				c.Warn(r, fmt.Sprintf("  refused: 400 bad_request on /admin/result: %v (body %d bytes); the game server's event result is not stored", err, len(body)))
				rw.json(400, pyjson.Obj{{K: "error", V: "bad_request"}, {K: "message", V: err.Error()}}, "")
				return
			}
			panic(err)
		}
		rw.json(201, pyjson.Obj{{K: "id", V: rid}}, "")
		return
	}
	out, found, err := c.Admin(r, req.Method, path, body)
	if err != nil {
		panic(err)
	}
	if !found {
		c.Warn(r, fmt.Sprintf("  refused: 404 on %s %s: not an admin endpoint", req.Method, cut(rawPathOf(path), 60)))
		rw.w.Header().Set("Connection", "close")
		rw.send(404, "text/html;charset=utf-8", []byte(errorPage404))
		return
	}
	rw.send(200, "text/plain", []byte(out))
}

// ---------------------------------------------------------------- who is calling

func headerRunSuffix(r *Req) string {
	if r.Run != "" {
		return " run=" + r.Run
	}
	return ""
}

func (c *Core) shimOrNone(r *Req) string {
	if r.Shim == "" {
		return "(none)"
	}
	return r.Shim
}

// logAuthRefusal: a session token that does not check out: who (when the token's account is known), which call, why, with the compared values. One line per
// code, player and call every 5 minutes. The token itself is never written.
func (c *Core) logAuthRefusal(r *Req, h http.Header, e *auth.Error, sent string) {
	shim := c.shimOrNone(r)
	var who any
	if e.Xuid != 0 {
		who = fmt.Sprintf("%#x", e.Xuid)
	}
	claimed := claimedXuid(h)
	var claims any
	if who == nil && claimed != 0 {
		claims = fmt.Sprintf("%#x (unverified)", claimed)
	}
	cls := strings.TrimSpace(strings.SplitN(h.Get("X-ClassName"), ",", 2)[0])
	key := [2]string{cls, h.Get("X-Method")}
	_, known := handlers[key]
	if _, raw := rawHandlers[key]; raw {
		known = true
	}
	name := "other"
	if known {
		name = headerCallName(h)
	}
	var det any
	if e.Detail != "" {
		det = e.Detail
	}
	var id any = e.Xuid
	if e.Xuid == 0 {
		id = shim
	}
	msg := fmt.Sprintf("  refused: %d %s on %s (%s) ", e.Status, e.Code, headerCallName(h), sent) +
		diag.KV(diag.KVP{K: "xuid", V: who}, diag.KVP{K: "claims", V: claims}, diag.KVP{K: "shim", V: shim}, diag.KVP{K: "detail", V: det})
	if e.Code == "banned" {
		msg += " " + e.Message
	}
	c.LogEvery(r, fmt.Sprintf("auth-refused/%s/%v/%s", e.Code, id, name), 300, msg, diag.Warn, "")
}

// removeOldClient: a client with a shim below minShim that proves who it is (a live launcher token) is also taken out of the game. Only a verified token
// counts, because anyone can send an X-UserId header for somebody else. At most once a minute per player.
func (c *Core) removeOldClient(r *Req, req *http.Request) {
	token := req.Header.Get("X-FH2MP-Token")
	if token == "" || c.Auth == nil {
		return
	}
	acct, err := c.Auth.Authenticate(token, clientIP(req))
	if err != nil {
		if _, ok := err.(*auth.Error); ok {
			return
		}
		panic(err)
	}
	f := &c.front
	f.mu.Lock()
	if t, ok := f.oldRemoved[acct.Xuid]; ok && time.Since(t) < time.Minute {
		f.mu.Unlock()
		return
	}
	if f.oldRemoved == nil || len(f.oldRemoved) > 20000 {
		f.oldRemoved = map[int64]time.Time{}
	}
	f.oldRemoved[acct.Xuid] = time.Now()
	f.mu.Unlock()
	c.Log(r, fmt.Sprintf("  old shim %s: removing %s %#x from the game (minShim %s, call=%s)", pyjson.PyRepr(c.shimOrNone(r)), pyjson.PyRepr(acct.Gamertag), acct.Xuid, c.Launcher.MinShim(),
		headerCallName(req.Header)))
	c.OnRevoke(acct.Xuid)
}

// gamedbOK: a game whose car database is not the server's (X-FH2MP-GameDb) may use every service but not play with other people: a car the others do not
// have crashes them. The request is marked (OwnCars); the multiplayer entry points and the public boards refuse marked requests. Only a server whose
// database cannot be read refuses everything.
func (c *Core) gamedbOK(rw respWriter, r *Req, req *http.Request, acct *auth.Account) bool {
	have := req.Header.Get("X-FH2MP-GameDb")
	if c.Launcher == nil {
		return true
	}
	sha, path, ok := c.Launcher.GameDBInfo()
	if !ok {
		if path != "" {
			c.LogEvery(r, "gamedb-unavailable", 60, fmt.Sprintf("  refused: 503 gamedb_unavailable: the server's car database %s cannot be read, so every signed-in request is refused until it can (xuid %#x, call=%s)",
				pyjson.PyRepr(path), acct.Xuid, headerCallName(req.Header)), diag.Error, "")
			rw.json(503, pyjson.Obj{{K: "error", V: "gamedb_unavailable"}, {K: "message", V: "The server's car database is unavailable. Please try again later."}}, "")
			return false
		}
		return true
	}
	have = strings.ToLower(strings.TrimSpace(have))
	f := &c.front
	if have == sha {
		f.mu.Lock()
		delete(f.ownCars, acct.Xuid)
		f.mu.Unlock()
		return true
	}
	r.OwnCars = true
	r.GameDB = [2]string{cut(have, 16), cut(sha, 16)} // for the lines that refuse this request later
	f.mu.Lock()
	_, seen := f.ownCars[acct.Xuid]
	if !seen {
		if f.ownCars == nil || len(f.ownCars) > 20000 {
			f.ownCars = map[int64]time.Time{}
		}
		f.ownCars[acct.Xuid] = time.Now()
	}
	f.mu.Unlock()
	if !seen {
		extra := ""
		if have == "" {
			extra = "; the client sent no hash"
		}
		c.Log(r, fmt.Sprintf("  car database %s is not the server's %s: %s %#x keeps the services but is out of play with other people (shim %s%s)",
			pyjson.PyRepr(cut(have, 16)), cut(sha, 16), pyjson.PyRepr(acct.Gamertag), acct.Xuid, c.shimOrNone(r), extra))
		c.OnRevoke(acct.Xuid) // a game in progress had the server's database: it must not carry on with another one
	}
	return true
}

func (c *Core) restrictedToolsOK(rw respWriter, r *Req, req *http.Request, acct *auth.Account) bool {
	value := req.Header.Get("X-FH2MP-Tools")
	var tokens []string
	if len(value) <= 80 {
		tokens = strings.Split(value, ",")
	}
	if len(tokens) == 0 || tokens[0] != "v1" {
		var what string
		switch {
		case len(value) > 80:
			what = fmt.Sprintf("%d characters (limit 80)", len(value))
		case value != "":
			what = pyjson.PyRepr(value)
		default:
			what = "nothing"
		}
		c.LogEvery(r, fmt.Sprintf("tools-426/%d", acct.Xuid), 300, fmt.Sprintf("  refused: 426 tool_check_required: xuid=%#x sent X-FH2MP-Tools %s, expected 'v1,...' (shim %s)", acct.Xuid, what, c.shimOrNone(r)), diag.Warn, "")
		rw.json(426, pyjson.Obj{{K: "error", V: "tool_check_required"}, {K: "message", V: "Update FH2MP and restart the game from the launcher."}}, "")
		return false
	}
	set := map[string]bool{}
	for _, t := range tokens[1:] {
		set[t] = true
	}
	var tools []string
	for t := range set {
		tools = append(tools, t)
	}
	sort.Strings(tools)
	var unknown []string
	for _, t := range tools {
		if t != "redline" && t != "nitrosystem" {
			unknown = append(unknown, t)
		}
	}
	if len(unknown) > 0 {
		l := make([]any, len(unknown))
		for i, u := range unknown {
			l[i] = u
		}
		c.LogEvery(r, fmt.Sprintf("tools-400/%d", acct.Xuid), 300, fmt.Sprintf("  refused: 400 bad_tool_report: xuid=%#x reported unknown tool(s) %s, known are redline and nitrosystem (shim %s)",
			acct.Xuid, pyjson.PyRepr(l), c.shimOrNone(r)), diag.Warn, "")
		rw.json(400, pyjson.Obj{{K: "error", V: "bad_tool_report"}}, "")
		return false
	}
	if len(tools) == 0 {
		return true
	}
	joined := strings.Join(tools, ",")
	f := &c.front
	f.mu.Lock()
	old, had := f.toolsAnnounce[acct.Xuid]
	f.mu.Unlock()
	if had && old.tools == joined && time.Since(old.at) < time.Minute {
		return true
	}
	result := c.Xbl.QuarantineTools(acct.Xuid, tools)
	if result == "" {
		c.LogEvery(r, fmt.Sprintf("tools-503/%d", acct.Xuid), 60, fmt.Sprintf("  refused: 503 pool_unavailable: xuid=%#x reports %s but the game server did not answer the quarantine query, so the player cannot be placed in a pool",
			acct.Xuid, joined), diag.Error, "")
		rw.json(503, pyjson.Obj{{K: "error", V: "pool_unavailable"}, {K: "message", V: "Matchmaking is unavailable. Please try again later."}}, "")
		return false
	}
	f.mu.Lock()
	if f.toolsAnnounce == nil || len(f.toolsAnnounce) >= 10000 {
		f.toolsAnnounce = map[int64]toolsEntry{}
	}
	f.toolsAnnounce[acct.Xuid] = toolsEntry{joined, time.Now()}
	f.mu.Unlock()
	if result == "rejoin" {
		c.Log(r, fmt.Sprintf("  tools: xuid=%#x reports %s: moved to the restricted pool, dropped from matchmaking until the game restarts", acct.Xuid, joined))
		c.Xbl.DropPlayer(fmt.Sprint(acct.Xuid))
		rw.json(409, pyjson.Obj{{K: "error", V: "pool_changed"}, {K: "message", V: "Restart the game from the launcher to join the restricted matchmaking pool."}}, "")
		return false
	}
	return true
}

// identify sets the request's xuid / authed / tag from the session token. False (after answering) when the request must not go on.
func (c *Core) identify(rw respWriter, r *Req, req *http.Request) bool {
	h := req.Header
	r.Authed, r.Tag, r.OwnCars = false, "", false
	r.Shim = h.Get("X-FH2MP-Shim") // the client's shim version ("1.0.0-rc2"); empty for clients that do not send it
	need := "0.0.0"
	if c.Launcher != nil {
		need = c.Launcher.MinShim()
	}
	if c.Launcher != nil && !c.Launcher.VersionAtLeast(r.Shim, need) {
		shim := r.Shim
		if shim == "" {
			shim = "(none)"
		}
		c.LogEvery(r, "old-shim/"+r.Shim, 60, fmt.Sprintf("  refused: shim %s is older than %s (426, call=%s)", pyjson.PyRepr(shim), need, headerCallName(h)), diag.Info, "")
		c.removeOldClient(r, req)
		cls, method := h.Get("X-ClassName"), h.Get("X-Method")
		if c.SC != nil && strings.HasPrefix(cls, "Xls.WebServices.Notifications") && method == "RetrieveForUser" {
			b, err := c.outdatedNotice(need)
			if err != nil {
				panic(err)
			}
			rw.json(200, b, ctypeXtsw) // the game shows this, unlike the 426 below
			return false
		}
		// Logon is refused like the rest. An empty Logon result was tried (prod 2026-10-30 05:07) so that the game would reach the Message Center: every shim
		// 1.1.2 that sent a crash report afterwards (12 of 12, 05:14-15:29) died 10-30 s after starting at game+0x9c898f, a null CGameplayLog.
		rw.json(426, pyjson.Obj{{K: "error", V: "shim_too_old"}, {K: "message", V: fmt.Sprintf("FH2MP is out of date. Update the launcher and press Play (needs %s).", need)}}, "")
		return false
	}
	token := h.Get("X-FH2MP-Token")
	if c.Auth != nil && c.Auth.Mode != "off" {
		if token != "" {
			acct, err := c.Auth.Authenticate(token, clientIP(req))
			if err != nil {
				ae, ok := err.(*auth.Error)
				if !ok {
					panic(err)
				}
				if ae.Code == "banned" && c.SC != nil && strings.HasPrefix(h.Get("X-ClassName"), "Xls.WebServices.Notifications") && h.Get("X-Method") == "RetrieveForUser" {
					c.logAuthRefusal(r, h, ae, "answered 200 with the ban notice")
					b, err := c.banNotice(ae.Message)
					if err != nil {
						panic(err)
					}
					rw.json(200, b, ctypeXtsw) // the game shows this, unlike the 403 below
					return false
				}
				c.logAuthRefusal(r, h, ae, fmt.Sprintf("answered %d", ae.Status))
				rw.json(ae.Status, pyjson.Obj{{K: "error", V: ae.Code}, {K: "message", V: ae.Message}}, "")
				return false
			}
			r.Xuid, r.Tag, r.Authed = acct.Xuid, acct.Gamertag, true
			if line := c.authShare.note(acct.Xuid, true); line != "" {
				c.Log(nil, line)
			}
			if os.Getenv("FH2MP_RESTRICTED_TOOLS") == "quarantine" && !c.restrictedToolsOK(rw, r, req, acct) {
				return false
			}
			if !c.gamedbOK(rw, r, req, acct) {
				return false
			}
			if !r.OwnCars {
				c.AnnounceAuth(acct.Xuid, clientIP(req), token) // the game server only admits the server's car database
			}
			return true
		}
		if c.Auth.Mode == "required" {
			claimed := claimedXuid(h)
			c.LogEvery(r, fmt.Sprintf("no-token/%d/%s", claimed, r.Shim), 300, fmt.Sprintf("  refused: 401 login_required: no X-FH2MP-Token on %s (FH2MP_AUTH=required; shim %s, claims user %#x, unverified): the game was not started from the launcher",
				headerCallName(h), c.shimOrNone(r), claimed), diag.Warn, "")
			rw.json(401, pyjson.Obj{{K: "error", V: "login_required"}, {K: "message", V: "start the game from the FH2MP launcher"}}, "")
			return false
		}
	}
	r.Xuid = claimedXuid(h)
	banned, via := "", ""
	if c.Auth != nil { // both ways a tokenless request names its sender (X-UserId, the XSAPI token)
		xt, _ := strconv.ParseInt(c.xblXuid(r, h), 10, 64)
		for _, p := range []struct {
			how string
			who int64
		}{{"X-UserId", r.Xuid}, {"XSAPI token", xt}} {
			if banned == "" && p.who != 0 {
				t, err := c.Auth.BanText(p.who)
				if err != nil {
					panic(err)
				}
				banned = t
				if banned != "" {
					via = fmt.Sprintf("%s %#x", p.how, p.who)
				}
			}
		}
	}
	if banned != "" { // no token needed to be refused: nothing a banned player sends is accepted
		c.LogEvery(r, "banned-tokenless/"+via, 300, fmt.Sprintf("  refused: 403 banned (no token, named by %s) on %s: %s", via, headerCallName(h), banned), diag.Warn, "")
		if c.SC != nil && strings.HasPrefix(h.Get("X-ClassName"), "Xls.WebServices.Notifications") && h.Get("X-Method") == "RetrieveForUser" {
			b, err := c.banNotice(banned)
			if err != nil {
				panic(err)
			}
			rw.json(200, b, ctypeXtsw)
		} else {
			rw.json(403, pyjson.Obj{{K: "error", V: "banned"}, {K: "message", V: banned}}, "")
		}
		return false
	}
	if line := c.authShare.note(r.Xuid, false); line != "" {
		c.Log(nil, line)
	}
	return true
}

// ---------------------------------------------------------------- tell the game server who is signed in

// AnnounceAuth tells the game server (UDP, loopback) that this xuid is signed in from this address, so it accepts that player's OpenConnection, with the
// UDP ticket key of the session token. Repeated at most every 20 s per player and session.
func (c *Core) AnnounceAuth(xuid int64, ip, token string) {
	tkey := auth.UdpTicketKey(token)
	k := authKey{xuid, ip, tkey}
	f := &c.front
	f.mu.Lock()
	if t, ok := f.authAnnounce[k]; ok && time.Since(t) < 20*time.Second {
		f.mu.Unlock()
		return
	}
	if f.authAnnounce == nil || len(f.authAnnounce) > 20000 {
		f.authAnnounce = map[authKey]time.Time{}
	}
	f.authAnnounce[k] = time.Now()
	f.mu.Unlock()
	c.Xbl.AnnounceAuth(xuid, ip, tkey)
}

// OnLogin is a fresh sign-in (launcher login / sign-up): it lifts an earlier ban or kick on the game server, then announces the address.
func (c *Core) OnLogin(xuid int64, ip, token string) {
	c.Xbl.AnnounceAllow(xuid)
	f := &c.front
	f.mu.Lock()
	delete(f.authAnnounce, authKey{xuid, ip, auth.UdpTicketKey(token)})
	f.mu.Unlock()
	c.AnnounceAuth(xuid, ip, token)
}

// OnLogout is a launcher signing out (after the game exits, or a profile window closed): the connections that session's ticket opened end.
func (c *Core) OnLogout(xuid int64, token string) {
	tkey := auth.UdpTicketKey(token)
	f := &c.front
	f.mu.Lock()
	for k := range f.authAnnounce {
		if k.xuid == xuid && k.tkey == tkey {
			delete(f.authAnnounce, k)
		}
	}
	f.mu.Unlock()
	c.Xbl.AnnounceLogout(xuid, tkey)
}

// OnRevoke is a ban or kick: sessions and tickets go, and the game server ends the player's live UDP connections.
func (c *Core) OnRevoke(xuid int64) {
	f := &c.front
	f.mu.Lock()
	for k := range f.authAnnounce {
		if k.xuid == xuid {
			delete(f.authAnnounce, k)
		}
	}
	f.mu.Unlock()
	c.Xbl.DropPlayer(fmt.Sprint(xuid))
	c.Xbl.AnnounceRevoke(xuid)
}

// errorPage404 is what Python's http.server answers with for send_error(404).
const errorPage404 = `<!DOCTYPE HTML>
<html lang="en">
    <head>
        <meta charset="utf-8">
        <title>Error response</title>
    </head>
    <body>
        <h1>Error response</h1>
        <p>Error code: 404</p>
        <p>Message: Not Found.</p>
        <p>Error code explanation: 404 - Nothing matches the given URI.</p>
    </body>
</html>
`
