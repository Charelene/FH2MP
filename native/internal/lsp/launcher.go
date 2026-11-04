package lsp

import (
	"fmt"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"fh2mp/internal/auth"
	"fh2mp/internal/crashreports"
	"fh2mp/internal/db"
	"fh2mp/internal/diag"
	"fh2mp/internal/gamedbsync"
	"fh2mp/internal/pngtool"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/social"
)

// The public /v1/ API the player's launcher talks to: server status, launcher configuration, sign up / sign in, profile and gamerpic.
//
//	GET    /v1/status                 GET /v1/launcher/config      GET /v1/hotfix
//	GET    /v1/challenges[?top=3]     GET /v1/leaderboards[?limit=10]   GET /v1/home
//	GET    /v1/launcher/assets/<file> POST /v1/auth/challenge|register|login|logout
//	GET    /v1/gamertags/check?name=  GET|PATCH /v1/me     PUT|DELETE /v1/me/gamerpic     GET /v1/gamerpic/<xuid>.png
//	GET    /v1/gamedb[/gamedbRC.slt.xz]    POST /v1/crash-reports    /v1/community[/...]
//
// Errors are {"error": "<code>", "message": "..."} with the HTTP status of the auth.Error.

const (
	maxBody         = 1_200_000
	gamerpicMaxDim  = 512
	gamerpicMinDim  = 32
	serverVersion   = "1"
	refusalLogSecs  = 120.0 // one line per refusal code, route and player: a launcher retries a refused poll every few seconds
	assetMax        = 4_000_000
	boardsTTL       = 60 // seconds a /v1/challenges or /v1/leaderboards answer is reused: every launcher asks on refresh
	jsonBodyDefault = 64 * 1024
)

var assetTypes = map[string]string{".png": "image/png", ".jpg": "image/jpeg", ".webp": "image/webp", ".wav": "audio/wav"}

var launcherLog = diag.Get("launcher")

var verRe = regexp.MustCompile(`^v?(\d+)\.(\d+)\.(\d+)(?:-rc(\d+))?(-debug)?$`)

// versionAtLeast: `have` ("1.0.0-rc3", "1.0.0", "1.0.0-rc4-debug") is the same as or later than `need`. A pre-release sorts before its release; our own debug
// builds always pass; an empty or unreadable `have` fails unless nothing is required (need "0.0.0" or empty).
func versionAtLeast(have, need string) bool {
	n := verRe.FindStringSubmatch(strings.TrimSpace(need))
	if n == nil || (n[1] == "0" && n[2] == "0" && n[3] == "0") {
		return true
	}
	h := verRe.FindStringSubmatch(strings.TrimSpace(have))
	if h == nil {
		return false
	}
	if h[5] != "" {
		return true
	}
	key := func(m []string) [5]int {
		a, _ := strconv.Atoi(m[1])
		b, _ := strconv.Atoi(m[2])
		c, _ := strconv.Atoi(m[3])
		pre, rc := 1, 0
		if m[4] != "" {
			pre = 0
			rc, _ = strconv.Atoi(m[4])
		}
		return [5]int{a, b, c, pre, rc}
	}
	hk, nk := key(h), key(n)
	for i := range hk {
		if hk[i] != nk[i] {
			return hk[i] > nk[i]
		}
	}
	return true
}

// ---------------------------------------------------------------- launcher.json

// defaultConfig is the launcher configuration without a file, in the order the API shows it.
func defaultConfig() pyjson.Obj {
	return pyjson.Obj{{K: "motd", V: ""},
		{K: "links", V: pyjson.Obj{{K: "website", V: "https://forza.scuffed.af"}, {K: "patreon", V: ""}, {K: "discord", V: ""}}},
		{K: "minLauncher", V: "0.0.0"},
		{K: "minShim", V: "0.0.0"}, // oldest shim (X-FH2MP-Shim) the web services answer; older ones are refused with a message
		{K: "latestAzimuth", V: "0.9.10"},
		{K: "supportedAzimuth", V: []any{}}, // builds of azimuth.exe the shim's byte signatures were checked against: {"version", "sha256"}
		{K: "maintenance", V: ""},
		{K: "heroImage", V: ""}, // https URL of the launcher's home picture; empty = the drawn background
		{K: "news", V: []any{}}} // launcher home cards: {"tag", "title", "text", "url", "image"}
}

// LauncherConfig is launcher.json (path in FH2MP_LAUNCHER_CONFIG) merged over the defaults; re-read when the file changes, so the operator can set the
// Patreon link or a message of the day without restarting anything.
type LauncherConfig struct {
	Path string

	mu          sync.Mutex
	mtime       time.Time
	haveMtime   bool
	data        pyjson.Obj
	failedMtime *time.Time
	failedSet   bool
	Overlay     pyjson.Obj // set at run time through the ops API; wins over the file
}

// NewLauncherConfig reads path ("" = FH2MP_LAUNCHER_CONFIG).
func NewLauncherConfig(path string) *LauncherConfig {
	if path == "" {
		path = os.Getenv("FH2MP_LAUNCHER_CONFIG")
	}
	return &LauncherConfig{Path: path, data: defaultConfig()}
}

func objStr(o pyjson.Obj, k string) string { v, _ := o.Get(k); return pyjson.PyStr(v) }

func cloneObj(o pyjson.Obj) pyjson.Obj { return append(pyjson.Obj(nil), o...) }

// Get is the settings now in force.
func (lc *LauncherConfig) Get() pyjson.Obj {
	data := lc.file()
	lc.mu.Lock()
	overlay := lc.Overlay
	lc.mu.Unlock()
	if len(overlay) == 0 {
		return data
	}
	merged := cloneObj(data)
	def := defaultConfig()
	for _, p := range overlay {
		if _, known := def.Get(p.K); known && p.K != "links" {
			merged.Set(p.K, p.V)
		}
	}
	links := pyjson.Obj{}
	if lv, ok := data.Get("links"); ok {
		if lo, ok := lv.(pyjson.Obj); ok {
			links = cloneObj(lo)
		}
	}
	if ov, ok := overlay.Get("links"); ok {
		if oo, ok := ov.(pyjson.Obj); ok {
			for _, p := range oo {
				links.Set(p.K, p.V)
			}
		}
	}
	merged.Set("links", links)
	return merged
}

func (lc *LauncherConfig) file() pyjson.Obj {
	lc.mu.Lock()
	defer lc.mu.Unlock()
	if lc.Path == "" {
		return lc.data
	}
	st, err := os.Stat(lc.Path)
	if err == nil && (!lc.haveMtime || !st.ModTime().Equal(lc.mtime)) {
		var loaded pyjson.Obj
		var raw []byte
		if raw, err = os.ReadFile(lc.Path); err == nil {
			var v any
			if v, err = pyjson.Parse(string(raw)); err == nil {
				var ok bool
				if loaded, ok = v.(pyjson.Obj); !ok {
					err = fmt.Errorf("AttributeError: 'list' object has no attribute 'items'")
				}
			}
		}
		if err == nil {
			data := defaultConfig()
			def := defaultConfig()
			for _, p := range loaded {
				if _, known := def.Get(p.K); known {
					data.Set(p.K, p.V)
				}
			}
			links, _ := def.Get("links")
			merged := cloneObj(links.(pyjson.Obj))
			if lv, ok := loaded.Get("links"); ok {
				if lo, ok := lv.(pyjson.Obj); ok {
					for _, p := range lo {
						merged.Set(p.K, p.V)
					}
				}
			}
			data.Set("links", merged)
			lc.data, lc.mtime, lc.haveMtime = data, st.ModTime(), true
		}
	}
	if err != nil { // keep the last good copy; say so once per file version, or an edit with a typo does nothing unseen
		var cur *time.Time
		if st2, e2 := os.Stat(lc.Path); e2 == nil {
			t := st2.ModTime()
			cur = &t
		}
		if !lc.failedSet || !sameTime(lc.failedMtime, cur) {
			lc.failedMtime, lc.failedSet = cur, true
			launcherLog.Warn(fmt.Sprintf("%s not reloaded, the last good settings stay in force (minShim %s, maintenance %s): %v", lc.Path,
				pyjson.PyRepr(objAny(lc.data, "minShim")), pyjson.PyRepr(objAny(lc.data, "maintenance")), err))
		}
	}
	return lc.data
}

func sameTime(a, b *time.Time) bool {
	if a == nil || b == nil {
		return a == b
	}
	return a.Equal(*b)
}

func objAny(o pyjson.Obj, k string) any { v, _ := o.Get(k); return v }

// HotfixFile is the signed hotfix document (path in FH2MP_HOTFIX_FILE, made with release/hotfix.py): {"doc": base64 JSON, "sig": base64}. Served as it is at
// /v1/hotfix; the launcher checks the signature, so this only checks the shape. Re-read when the file changes; a broken file keeps the last good one in force.
type HotfixFile struct {
	Path string

	mu    sync.Mutex
	mtime time.Time
	have  bool
	data  pyjson.Obj
}

// NewHotfixFile reads path (the environment's FH2MP_HOTFIX_FILE when the argument is nil).
func NewHotfixFile(path *string) *HotfixFile {
	p := os.Getenv("FH2MP_HOTFIX_FILE")
	if path != nil {
		p = *path
	}
	return &HotfixFile{Path: p, data: pyjson.Obj{}}
}

// Get is the document to serve ({} = none).
func (h *HotfixFile) Get() pyjson.Obj {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.Path == "" {
		return pyjson.Obj{}
	}
	st, err := os.Stat(h.Path)
	if err != nil {
		if len(h.data) > 0 {
			launcherLog.Warn(fmt.Sprintf("hotfix: %s is gone, so no hotfix document is served now", h.Path))
		}
		h.data, h.have = pyjson.Obj{}, false
		return pyjson.Obj{}
	}
	if !h.have || !st.ModTime().Equal(h.mtime) {
		h.mtime, h.have = st.ModTime(), true
		var raw []byte
		raw, err = os.ReadFile(h.Path)
		if err == nil {
			var v any
			if v, err = pyjson.Parse(string(raw)); err == nil {
				o, isObj := v.(pyjson.Obj)
				doc, dok := objAny(o, "doc").(string)
				sig, sok := objAny(o, "sig").(string)
				if !(isObj && dok && sok) {
					err = fmt.Errorf("not {\"doc\": ..., \"sig\": ...}")
				} else {
					h.data = pyjson.Obj{{K: "doc", V: doc}, {K: "sig", V: sig}}
					launcherLog.Info(fmt.Sprintf("hotfix: serving %s (%d bytes of document)", h.Path, len(doc)))
				}
			}
		}
		if err != nil {
			launcherLog.Warn(fmt.Sprintf("hotfix: %s not reloaded, the last good document stays in force: %v", h.Path, err))
		}
	}
	return h.data
}

// ---------------------------------------------------------------- the API

// Launcher is the /v1 API's context.
type Launcher struct {
	C        *Core
	Config   *LauncherConfig
	Hotfix   *HotfixFile
	LspPub   pyjson.Obj
	GamePub  pyjson.Obj
	GameDB   *gamedbsync.GameDb
	Crashes  *crashreports.Reports // nil = the endpoint is off
	Assets   string
	ClubInv  func(sponsor, invitee int64) error // launcher club invites; nil = off
	Boards   func(kind string, n int) (any, error)
	comm     *communityState
	boardsMu sync.Mutex
	boards   map[string]boardsHit
	rate     *diag.RateLimit
}

type boardsHit struct {
	at  time.Time
	obj any
}

// NewLauncher makes the API for a core.
func NewLauncher(c *Core) *Launcher {
	return &Launcher{C: c, Config: NewLauncherConfig(""), Hotfix: NewHotfixFile(nil), GameDB: gamedbsync.New(""), Assets: os.Getenv("FH2MP_LAUNCHER_ASSETS"),
		boards: map[string]boardsHit{}, rate: diag.NewRateLimit(nil, 0)}
}

// MinShim, VersionAtLeast, BodyLimit and GameDB make a Launcher the front's LauncherAPI.
func (l *Launcher) MinShim() string { return objStr(l.Config.Get(), "minShim") }

// VersionAtLeast is versionAtLeast.
func (l *Launcher) VersionAtLeast(have, need string) bool { return versionAtLeast(have, need) }

// BodyLimit is the most bytes the HTTP server reads for a /v1 request (crash reports carry logs and a minidump).
func (l *Launcher) BodyLimit(path string) int64 {
	p, _, _ := strings.Cut(path, "?")
	if strings.TrimRight(p, "/") == "/v1/crash-reports" {
		return crashreports.MaxBody
	}
	return maxBody
}

// GameDBInfo is the sha256 and path of the car database; ok=false when it cannot be read.
func (l *Launcher) GameDBInfo() (sha, path string, ok bool) {
	m := l.GameDB.Manifest()
	if m == nil {
		return "", l.GameDB.Path, false
	}
	return m.Sha256, l.GameDB.Path, true
}

func (l *Launcher) boardsGet(kind string, n int) (any, error) {
	if l.Boards == nil {
		return nil, auth.NewError(404, "not_found", "no leaderboards on this server").WithDetail("Ctx.boards is not set")
	}
	key := fmt.Sprintf("%s/%d", kind, n)
	l.boardsMu.Lock()
	hit, ok := l.boards[key]
	l.boardsMu.Unlock()
	if ok && time.Since(hit.at).Seconds() < boardsTTL {
		return hit.obj, nil
	}
	obj, err := l.Boards(kind, n)
	if err != nil {
		return nil, err
	}
	l.boardsMu.Lock()
	l.boards[key] = boardsHit{time.Now(), obj}
	l.boardsMu.Unlock()
	return obj, nil
}

var assetNameRe = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]{0,63}\.[a-z0-9]{3,4}$`)

// asset is a file from the operator's launcher assets folder (hero and card pictures, sounds); only plain names, known types, 4 MB.
func (l *Launcher) asset(name string) ([]byte, string, error) {
	ctype, known := assetTypes[strings.ToLower(filepath.Ext(name))]
	if l.Assets == "" || !assetNameRe.MatchString(name) || !known {
		return nil, "", auth.NewError(404, "not_found", "")
	}
	f, err := os.Open(filepath.Join(l.Assets, name))
	if err != nil {
		return nil, "", auth.NewError(404, "not_found", "")
	}
	defer f.Close()
	buf := make([]byte, assetMax+1)
	n, _ := readFull(f, buf)
	if n > assetMax {
		return nil, "", auth.NewError(404, "not_found", "")
	}
	return buf[:n], ctype, nil
}

func readFull(f *os.File, buf []byte) (int, error) {
	n := 0
	for n < len(buf) {
		m, err := f.Read(buf[n:])
		n += m
		if err != nil {
			return n, err
		}
	}
	return n, nil
}

// qint is the clamped integer query parameter key, default when absent or not a number.
func qint(u *url.URL, key string, dflt, lo, hi int) int {
	vals := u.Query()[key]
	if len(vals) == 0 || vals[0] == "" {
		return dflt
	}
	n, err := strconv.Atoi(strings.TrimSpace(vals[0]))
	if err != nil {
		return dflt
	}
	return max(lo, min(hi, n))
}

func jsonBody(body []byte, limit int) (pyjson.Obj, error) {
	if len(body) > limit {
		return nil, auth.NewError(413, "too_large", "").WithDetail(fmt.Sprintf("body %d bytes, limit %d", len(body), limit))
	}
	if len(body) == 0 {
		return pyjson.Obj{}, nil
	}
	v, err := pyjson.Parse(string(body))
	if err != nil {
		return nil, auth.NewError(400, "bad_request", "invalid JSON").WithDetail(fmt.Sprintf("%v (body %d bytes)", err, len(body)))
	}
	o, ok := v.(pyjson.Obj)
	if !ok {
		return nil, auth.NewError(400, "bad_request", "expected a JSON object").WithDetail("body is a " + pyjson.TypeName(v))
	}
	return o, nil
}

func accountJSON(a *auth.Account, extra pyjson.Obj) pyjson.Obj {
	d := pyjson.Obj{{K: "xuid", V: a.Xuid}, {K: "xuidHex", V: fmt.Sprintf("0x%016x", a.Xuid)}, {K: "gamertag", V: a.Gamertag}}
	for _, p := range extra {
		d.Set(p.K, p.V)
	}
	return d
}

func bearer(h http.Header) string {
	v := h.Get("Authorization")
	if len(v) >= 7 && strings.EqualFold(v[:7], "bearer ") {
		return strings.TrimSpace(v[7:])
	}
	return ""
}

var pngSig = "\x89PNG\r\n\x1a\n"

// cleanGamerpic validates an uploaded PNG and returns a freshly encoded one (the original bytes never reach other players' game clients).
func cleanGamerpic(png []byte) ([]byte, error) {
	hasSig := len(png) >= 8 && string(png[:8]) == pngSig
	if !hasSig || len(png) > social.MaxGamerpic {
		why := "no PNG signature"
		if hasSig {
			why = "PNG signature present"
		}
		return nil, auth.NewError(400, "bad_image", "send a PNG of at most 1 MB").WithDetail(fmt.Sprintf("%d bytes (limit %d), %s", len(png), social.MaxGamerpic, why))
	}
	if len(png) < 24 || string(png[12:16]) != "IHDR" {
		return nil, auth.NewError(400, "bad_image", "that PNG cannot be read").WithDetail("no IHDR chunk after the signature")
	}
	w0, h0 := int(be32(png[16:])), int(be32(png[20:])) // checked before inflating, so a tiny file cannot claim gigabytes
	tooBig := fmt.Sprintf("the picture must be between %d and %d pixels each way", gamerpicMinDim, gamerpicMaxDim)
	if !(gamerpicMinDim <= w0 && w0 <= gamerpicMaxDim && gamerpicMinDim <= h0 && h0 <= gamerpicMaxDim) {
		return nil, auth.NewError(400, "bad_image", tooBig).WithDetail(fmt.Sprintf("header says %dx%d", w0, h0))
	}
	im, err := pngtool.Decode(png, gamerpicMaxDim)
	if err != nil {
		return nil, auth.NewError(400, "bad_image", "that PNG cannot be read (8-bit, not interlaced, please)").WithDetail("decoder: " + diag.Scrub(err.Error()))
	}
	if !(gamerpicMinDim <= im.W && im.W <= gamerpicMaxDim && gamerpicMinDim <= im.H && im.H <= gamerpicMaxDim) {
		return nil, auth.NewError(400, "bad_image", tooBig).WithDetail(fmt.Sprintf("decoded %dx%d, the header said %dx%d", im.W, im.H, w0, h0))
	}
	return pngtool.Encode(im.W, im.H, im.Ch, im.Rows), nil
}

func be32(b []byte) uint32 {
	return uint32(b[0])<<24 | uint32(b[1])<<16 | uint32(b[2])<<8 | uint32(b[3])
}

var launcherClientRe = regexp.MustCompile(`^launcher/(\S+)`)

// requireLauncher: launchers announce themselves as "launcher/<version> <os>" at sign-in; one older than minLauncher is refused with a message it shows.
// Other clients (test tools) are not held to it.
func (l *Launcher) requireLauncher(client string) error {
	m := launcherClientRe.FindStringSubmatch(client)
	c := l.Config.Get()
	if m != nil && !versionAtLeast(m[1], objStr(c, "minLauncher")) {
		site := ""
		if lv, ok := objAny(c, "links").(pyjson.Obj); ok {
			site = objStr(lv, "website")
		}
		msg := fmt.Sprintf("This launcher (%s) is out of date. Download the latest FH2MP launcher", m[1])
		if site != "" {
			msg += " from " + site
		}
		return auth.NewError(426, "launcher_too_old", msg+".").WithDetail(fmt.Sprintf("launcher %s is older than minLauncher %s", m[1], objStr(c, "minLauncher")))
	}
	return nil
}

var idRe = regexp.MustCompile(`/\d+`)

// Handle answers one /v1 request. It never fails: errors become {"error", "message"}.
func (l *Launcher) Handle(r *Req, method, path string, h http.Header, body []byte, ip string) (status int, obj any, ctype string) {
	defer func() {
		if p := recover(); p != nil {
			ae, ok := p.(*auth.Error)
			if !ok {
				panic(p)
			}
			l.noteRefusal(method, path, ae)
			status, obj, ctype = ae.Status, pyjson.Obj{{K: "error", V: ae.Code}, {K: "message", V: ae.Message}}, "application/json"
		}
	}()
	status, obj, ctype, err := l.route(r, method, path, h, body, ip)
	if err != nil {
		ae, ok := err.(*auth.Error)
		if !ok {
			panic(err)
		}
		l.noteRefusal(method, path, ae)
		return ae.Status, pyjson.Obj{{K: "error", V: ae.Code}, {K: "message", V: ae.Message}}, "application/json"
	}
	return status, obj, ctype
}

// noteRefusal: one line per refusal code, route and player (every 120 s), saying who, what and why. Policy refusals (401/403/426/429, 5xx) are WARN, the
// player's own mistakes (400/404/409 ...) INFO. The client sees Message only; Detail (the compared values) is for this line.
func (l *Launcher) noteRefusal(method, path string, e *auth.Error) {
	p, _, _ := strings.Cut(path, "?")
	route := cut(idRe.ReplaceAllString(strings.TrimRight(p, "/"), "/N"), 80)
	var who any
	if e.Xuid != 0 {
		who = fmt.Sprintf("%#x", e.Xuid)
	}
	ok, skipped := l.rate.Allow([5]any{"refused", e.Status, e.Code, route, who}, refusalLogSecs)
	if !ok {
		return
	}
	var det, msg any
	if e.Detail != "" {
		det = e.Detail
	}
	if e.Message != e.Code {
		msg = e.Message
	}
	line := fmt.Sprintf("refused %d %s: %s %s ", e.Status, e.Code, method, route) + diag.KV(diag.KVP{K: "xuid", V: who}, diag.KVP{K: "detail", V: det}, diag.KVP{K: "message", V: msg})
	if skipped > 0 {
		line += fmt.Sprintf(" (and %d more in the last %.0f s)", skipped, refusalLogSecs)
	}
	if e.Status == 401 || e.Status == 403 || e.Status == 426 || e.Status == 429 || e.Status >= 500 {
		launcherLog.Warn(line)
	} else {
		launcherLog.Info(line)
	}
}

func str(o pyjson.Obj, k string) string {
	v, ok := o.Get(k)
	if !ok {
		return ""
	}
	return pyjson.PyStr(v)
}

func (l *Launcher) route(r *Req, method, path string, h http.Header, body []byte, ip string) (int, any, string, error) {
	c, a := l.C, l.C.Auth
	u, perr := url.Parse(path)
	if perr != nil {
		return 0, nil, "", auth.NewError(404, "not_found", "unknown endpoint")
	}
	p := strings.TrimRight(u.Path, "/")
	const ok = "application/json"
	now := time.Now().Unix()
	switch {
	case p == "/v1/status" && method == "GET":
		return 200, pyjson.Obj{{K: "env", V: a.Env}, {K: "online", V: c.OnlineCount()}, {K: "auth", V: a.Mode}, {K: "registration", V: a.Registration},
			{K: "server", V: serverVersion}, {K: "time", V: now}, {K: "maintenance", V: objAny(l.Config.Get(), "maintenance")}}, ok, nil
	case p == "/v1/launcher/config" && method == "GET":
		cf := l.Config.Get()
		return 200, pyjson.Obj{{K: "env", V: a.Env}, {K: "motd", V: objAny(cf, "motd")}, {K: "links", V: objAny(cf, "links")}, {K: "minLauncher", V: objAny(cf, "minLauncher")},
			{K: "minShim", V: objAny(cf, "minShim")}, {K: "heroImage", V: objAny(cf, "heroImage")}, {K: "news", V: objAny(cf, "news")},
			{K: "latestAzimuth", V: objAny(cf, "latestAzimuth")}, {K: "supportedAzimuth", V: objAny(cf, "supportedAzimuth")}, {K: "maintenance", V: objAny(cf, "maintenance")},
			{K: "online", V: c.OnlineCount()}, {K: "registration", V: a.Registration}, {K: "auth", V: a.Mode}, {K: "lsp", V: l.LspPub}, {K: "game", V: l.GamePub},
			{K: "time", V: now}}, ok, nil
	case p == "/v1/hotfix" && method == "GET":
		return 200, l.Hotfix.Get(), ok, nil
	case p == "/v1/challenges" && method == "GET":
		o, err := l.boardsGet("challenges", qint(u, "top", 3, 0, 10))
		return 200, o, ok, err
	case p == "/v1/leaderboards" && method == "GET":
		o, err := l.boardsGet("leaderboards", qint(u, "limit", 10, 1, 50))
		return 200, o, ok, err
	case p == "/v1/home" && method == "GET":
		o, err := l.boardsGet("home", 0)
		return 200, o, ok, err
	case strings.HasPrefix(p, "/v1/launcher/assets/") && method == "GET":
		data, ctype, err := l.asset(p[len("/v1/launcher/assets/"):])
		return 200, data, ctype, err
	case p == "/v1/auth/challenge" && method == "POST":
		chal, bits, ttl, err := a.Challenge(ip)
		if err != nil {
			return 0, nil, "", err
		}
		return 200, pyjson.Obj{{K: "challenge", V: chal}, {K: "powBits", V: bits}, {K: "ttl", V: int64(ttl)}, {K: "realm", V: a.Env}}, ok, nil
	case p == "/v1/auth/register" && method == "POST":
		d, err := jsonBody(body, jsonBodyDefault)
		if err != nil {
			return 0, nil, "", err
		}
		pow := int64(-1)
		if pv, has := d.Get("pow"); has {
			pow = powValue(pv)
		}
		acct, err := a.Register(ip, str(d, "challenge"), str(d, "pubkey"), str(d, "gamertag"), pow, str(d, "sig"), str(d, "invite"))
		if err != nil {
			return 0, nil, "", err
		}
		c.Log(r, fmt.Sprintf("  v1 register %#x %s", acct.Xuid, pyjson.PyRepr(acct.Gamertag)))
		return 201, pyjson.Obj{{K: "account", V: accountJSON(acct, nil)}}, ok, nil
	case p == "/v1/auth/login" && method == "POST":
		d, err := jsonBody(body, jsonBodyDefault)
		if err != nil {
			return 0, nil, "", err
		}
		if err := l.requireLauncher(str(d, "client")); err != nil {
			return 0, nil, "", err
		}
		token, acct, ttl, err := a.Login(ip, str(d, "challenge"), str(d, "pubkey"), str(d, "sig"), str(d, "client"))
		if err != nil {
			return 0, nil, "", err
		}
		c.OnLogin(acct.Xuid, ip, token)
		var client, run any
		if s := cut(str(d, "client"), 80); s != "" {
			client = s
		}
		if s := diag.RunID(h.Get("X-FH2MP-Run")); s != "" {
			run = s
		}
		line := fmt.Sprintf("  v1 login %#x %s ", acct.Xuid, pyjson.PyRepr(acct.Gamertag)) + diag.KV(diag.KVP{K: "client", V: client}, diag.KVP{K: "run", V: run})
		c.Log(r, strings.TrimRight(line, " "))
		return 200, pyjson.Obj{{K: "token", V: token}, {K: "idleSeconds", V: int64(ttl)}, {K: "account", V: accountJSON(acct, nil)}, {K: "lsp", V: l.LspPub}, {K: "game", V: l.GamePub}}, ok, nil
	case p == "/v1/gamertags/check" && method == "GET":
		name := ""
		if v := u.Query()["name"]; len(v) > 0 {
			name = v[0]
		}
		free, err := a.TagAvailable(ip, name)
		if err != nil {
			ae, isAuth := err.(*auth.Error)
			if !isAuth {
				return 0, nil, "", err
			}
			if ae.Status == 429 {
				return 0, nil, "", err
			}
			return 200, pyjson.Obj{{K: "valid", V: false}, {K: "available", V: false}, {K: "reason", V: ae.Message}}, ok, nil
		}
		reason := ""
		if !free {
			reason = "that gamertag is taken"
		}
		return 200, pyjson.Obj{{K: "valid", V: true}, {K: "available", V: free}, {K: "reason", V: reason}}, ok, nil
	case strings.HasPrefix(p, "/v1/gamerpic/") && strings.HasSuffix(p, ".png") && method == "GET":
		x, err := strconv.ParseInt(strings.TrimSpace(p[len("/v1/gamerpic/"):len(p)-4]), 10, 64)
		if err != nil {
			return 0, nil, "", auth.NewError(404, "not_found", "")
		}
		png, err := c.Social.Gamerpic(x)
		return 200, png, "image/png", err
	case p == "/v1/crash-reports" && method == "POST":
		if l.Crashes == nil {
			return 0, nil, "", auth.NewError(503, "crash_reports_off", "this server does not take crash reports").WithDetail("no crash report store is configured")
		}
		byIP := a.Hit([2]any{"crash", ip}, 5, 3600)
		if !byIP || !a.Hit([2]any{"crash", "all"}, 300, 86400) {
			detail := "crash reports: over 300 per day in all"
			if !byIP {
				detail = "crash reports: over 5 per hour from this address"
			}
			return 0, nil, "", auth.NewError(429, "rate_limited", "too many crash reports, try again later").WithDetail(detail)
		}
		d, err := jsonBody(body, crashreports.MaxBody)
		if err != nil {
			return 0, nil, "", err
		}
		id, err := l.Crashes.Submit(d)
		if err != nil {
			return 0, nil, "", err
		}
		return 202, pyjson.Obj{{K: "id", V: id}}, ok, nil
	case p == "/v1/auth/logout" && method == "POST":
		acct, err := a.Authenticate(bearer(h), ip)
		if err != nil {
			return 0, nil, "", err
		}
		if err := a.Logout(bearer(h)); err != nil {
			return 0, nil, "", err
		}
		c.OnLogout(acct.Xuid, bearer(h))
		return 200, pyjson.Obj{{K: "ok", V: true}}, ok, nil
	case p == "/v1/gamedb" && method == "GET":
		acct, err := a.Authenticate(bearer(h), ip)
		if err != nil {
			return 0, nil, "", err
		}
		m := l.GameDB.Manifest()
		if m == nil {
			return 0, nil, "", auth.NewError(404, "no_gamedb", "this server does not set a car database").WithXuid(acct.Xuid).WithDetail("gamedb manifest is None")
		}
		return 200, pyjson.Obj{{K: "name", V: m.Name}, {K: "sha256", V: m.Sha256}, {K: "size", V: m.Size}, {K: "xzSize", V: m.XzSize}, {K: "path", V: m.Path}}, ok, nil
	case p == gamedbsync.DownloadPath && method == "GET":
		acct, err := a.Authenticate(bearer(h), ip)
		if err != nil {
			return 0, nil, "", err
		}
		if !a.Hit([2]any{"gamedb", acct.Xuid}, 10, 3600) {
			return 0, nil, "", auth.NewError(429, "rate_limited", "too many car database downloads, wait a while").WithXuid(acct.Xuid).WithDetail("gamedb downloads: over 10 per hour for this account")
		}
		blob := l.GameDB.Download()
		if blob == nil {
			return 0, nil, "", auth.NewError(404, "no_gamedb", "this server does not set a car database").WithXuid(acct.Xuid).WithDetail("manifest exists but the file could not be read")
		}
		c.Log(r, fmt.Sprintf("  v1 gamedb download %#x %d bytes", acct.Xuid, len(blob)))
		return 200, blob, "application/x-xz", nil
	case p == "/v1/community" || strings.HasPrefix(p, "/v1/community/"):
		acct, err := a.Authenticate(bearer(h), ip)
		if err != nil {
			return 0, nil, "", err
		}
		if !a.Hit([2]any{"community", acct.Xuid}, 120, 60) {
			return 0, nil, "", auth.NewError(429, "rate_limited", "Please wait a moment before trying again.").WithXuid(acct.Xuid).WithDetail("community: over 120 requests per minute for this account")
		}
		if method != "GET" && !a.Hit([2]any{"community-write", acct.Xuid}, 30, 60) {
			return 0, nil, "", auth.NewError(429, "rate_limited", "Please wait a moment before trying again.").WithXuid(acct.Xuid).WithDetail("community writes: over 30 per minute for this account")
		}
		d, err := jsonBody(body, jsonBodyDefault)
		if err != nil {
			return 0, nil, "", err
		}
		o, err := l.handleCommunity(r, method, p, u, d, acct)
		return 200, o, ok, err
	case p == "/v1/me" || strings.HasPrefix(p, "/v1/me/"):
		acct, err := a.Authenticate(bearer(h), ip)
		if err != nil {
			return 0, nil, "", err
		}
		switch {
		case p == "/v1/me" && method == "GET":
			row, err := a.Account(acct.Xuid)
			if err != nil {
				return 0, nil, "", err
			}
			prof, err := c.Social.Profile(acct.Xuid)
			if err != nil {
				return 0, nil, "", err
			}
			return 200, accountJSON(acct, pyjson.Obj{{K: "created", V: rowAny(row, "created")}, {K: "lastLogin", V: rowAny(row, "last_login")},
				{K: "gamertagChangedAt", V: rowAny(row, "tag_changed")}, {K: "gamerpic", V: prof.PicVersion > 0}}), ok, nil
		case p == "/v1/me" && method == "PATCH":
			d, err := jsonBody(body, jsonBodyDefault)
			if err != nil {
				return 0, nil, "", err
			}
			tag, err := a.Rename(acct.Xuid, str(d, "gamertag"), false)
			if err != nil {
				return 0, nil, "", err
			}
			return 200, accountJSON(&auth.Account{Xuid: acct.Xuid, Gamertag: tag}, nil), ok, nil
		case p == "/v1/me/gamerpic" && method == "PUT":
			if len(body) > maxBody {
				return 0, nil, "", auth.NewError(413, "too_large", "").WithXuid(acct.Xuid).WithDetail(fmt.Sprintf("picture %d bytes, limit %d", len(body), maxBody))
			}
			if !a.Hit([2]any{"gamerpic", acct.Xuid}, 6, 600) {
				return 0, nil, "", auth.NewError(429, "rate_limited", "too many picture uploads, wait a few minutes").WithXuid(acct.Xuid).WithDetail("gamerpic uploads: over 6 per 10 minutes for this account")
			}
			png, err := cleanGamerpic(body)
			if err != nil {
				return 0, nil, "", err
			}
			if err := c.Social.SetGamerpic(acct.Xuid, png); err != nil {
				return 0, nil, "", err
			}
			return 200, pyjson.Obj{{K: "bytes", V: len(png)}}, ok, nil
		case p == "/v1/me/gamerpic" && method == "DELETE":
			if err := c.Social.ClearGamerpic(acct.Xuid); err != nil {
				return 0, nil, "", err
			}
			return 200, pyjson.Obj{{K: "ok", V: true}}, ok, nil
		}
	}
	return 0, nil, "", auth.NewError(404, "not_found", "unknown endpoint").WithDetail(fmt.Sprintf("no route for %s %s", method, p))
}

// powValue is the `pow` field as the register call takes it: a whole number, else -1.
func powValue(v any) int64 {
	switch x := v.(type) {
	case int64:
		return x
	case uint64:
		return int64(x)
	case float64:
		return int64(x)
	}
	return -1
}

func rowAny(r *db.Row, k string) any {
	if r == nil || !r.Has(k) {
		return nil
	}
	return r.Any(k)
}
