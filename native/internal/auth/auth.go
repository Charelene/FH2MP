// Package auth is the accountless identity of FH2MP (= server/auth.py): an Ed25519 key pair per account, server-assigned xuids, challenge /
// proof-of-work registration, hashed session tokens bound to an address, gamertag rules, bans and rate limits. HTTP-free.
package auth

import (
	"crypto/ed25519"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"math"
	"math/big"
	"os"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/text/unicode/norm"

	"fh2mp/internal/db"
	"fh2mp/internal/diag"
	"fh2mp/internal/pyfmt"
)

// Constants (auth.py).
const (
	XuidBase     = 0x0009000000000000
	ChallengeTTL = 120.0
	SessionIdle  = 12 * 3600.0
	SessionMax   = 72 * 3600.0
	TouchEvery   = 300.0
	CacheSeconds = 20.0
	TagMin       = 3
	TagMax       = 15
	TagCooldown  = 7 * 86400.0
	PowBaseBits  = 18
	PowMaxBits   = 24
	SessionLiveS = 120
)

var signPrefix = []byte("FH2MP-AUTH-v1\x00")
var log = diag.Get("auth")
var rate = diag.NewRateLimit(nil, 0)

// UdpTicketKey is the key a game client signs its UDP connection ticket with (hex): HMAC-SHA256(session token, "fh2mp udp ticket v1").
func UdpTicketKey(token string) string {
	if token == "" {
		return ""
	}
	m := hmac.New(sha256.New, []byte(token))
	m.Write([]byte("fh2mp udp ticket v1"))
	return hex.EncodeToString(m.Sum(nil))
}

// BanMessage is what a banned player is told.
func BanMessage(reason string, until *float64) string {
	when := "permanently"
	if until != nil && *until != 0 {
		when = "until " + time.Unix(int64(*until), 0).UTC().Format("2006-01-02 15:04") + " UTC"
	}
	s := "This account is banned " + when + "."
	if reason != "" {
		s += " Reason: " + reason
	}
	return s
}

// BanActive: a ban applies while banned_at is set and, for a temporary one, until `until`.
func BanActive(bannedAt *float64, until *float64, now float64) bool {
	return bannedAt != nil && *bannedAt != 0 && (until == nil || now < *until)
}

// Error is an AuthError.
type Error struct {
	Status  int
	Code    string
	Message string
	Xuid    int64
	Detail  string
}

func (e *Error) Error() string { return e.Message }

// NewError makes an AuthError for other packages.
func NewError(status int, code, message string) *Error { return aerr(status, code, message) }

func aerr(status int, code, message string) *Error {
	if message == "" {
		message = code
	}
	return &Error{Status: status, Code: code, Message: message}
}

func (e *Error) withDetail(d string) *Error { e.Detail = d; return e }
func (e *Error) withXuid(x int64) *Error    { e.Xuid = x; return e }

// Account is a signed-in player.
type Account struct {
	Xuid     int64
	Gamertag string
	Pubkey   []byte
}

// B64u is URL-safe base64 without padding.
func B64u(b []byte) string { return base64.RawURLEncoding.EncodeToString(b) }

func unb64u(s any) ([]byte, error) {
	str, ok := s.(string)
	if !ok || len(str) > 4096 {
		d := "a field is not text"
		if ok {
			d = fmt.Sprintf("a field is str of %d characters, limit 4096", len(str))
		}
		return nil, aerr(400, "bad_request", "malformed field").withDetail(d)
	}
	pad := strings.Repeat("=", (4-len(str)%4)%4)
	b, err := base64.URLEncoding.DecodeString(str + pad)
	if err != nil {
		return nil, aerr(400, "bad_request", "malformed field").withDetail(fmt.Sprintf("a field is not base64 (%v)", err))
	}
	return b, nil
}

// SignMessage is the exact bytes the client signs; length-prefixed fields.
func SignMessage(realm, purpose string, challengeRaw []byte, fields ...[]byte) []byte {
	out := append([]byte(nil), signPrefix...)
	out = append(out, realm...)
	out = append(out, 0)
	out = append(out, purpose...)
	out = append(out, 0)
	out = append(out, challengeRaw...)
	for _, f := range fields {
		out = binary.BigEndian.AppendUint16(out, uint16(len(f)))
		out = append(out, f...)
	}
	return out
}

// PowOK is hashcash: SHA-256(challenge || pubkey || counter) must start with `bits` zero bits.
func PowOK(challengeRaw, pubkey []byte, counter uint64, bits int) bool {
	buf := append(append(append([]byte(nil), challengeRaw...), pubkey...), 0, 0, 0, 0, 0, 0, 0, 0)
	binary.BigEndian.PutUint64(buf[len(buf)-8:], counter)
	h := sha256.Sum256(buf)
	n := new(big.Int).SetBytes(h[:])
	return new(big.Int).Rsh(n, uint(256-bits)).Sign() == 0
}

// NormIP strips an IPv4-mapped prefix.
func NormIP(ip string) string {
	ip = strings.TrimSpace(ip)
	return strings.TrimPrefix(ip, "::ffff:")
}

var foldMap = strings.NewReplacer("0", "o", "1", "i", "l", "i", "3", "e", "4", "a", "5", "s", "7", "t", "$", "s", "@", "a", "!", "i")
var tagOK = regexp.MustCompile(`^[A-Za-z][A-Za-z0-9]*(?: [A-Za-z0-9]+)*$`)
var sepRe = regexp.MustCompile(`[ ._-]+`)
var playerRe = regexp.MustCompile(`^player[ ._-]*\d*$`)

// Reserved names are blocked anywhere in a gamertag.
var Reserved = []string{"admin", "moderator", "staff", "official", "support", "turn10", "microsoft", "xbox", "playground", "azimuth", "fh2mp",
	"server", "system", "host", "guest", "drivatar", "anonymous", "nobody", "everyone", "undefined", "null"}

// FoldTag is the form compared for uniqueness: case, separators and look-alike digits/symbols removed.
func FoldTag(tag string) string {
	t := strings.ToLower(norm.NFKC.String(tag))
	t = foldMap.Replace(t)
	return sepRe.ReplaceAllString(t, "")
}

var blockedSubstr = func() []string {
	out := make([]string, len(Reserved))
	for i, r := range Reserved {
		out[i] = FoldTag(r)
	}
	return out
}()

// ValidateGamertag returns the trimmed gamertag or a bad_gamertag error.
func ValidateGamertag(tag string, extraBlocked, extraWords []string) (string, error) {
	tag = strings.TrimSpace(tag)
	n := len([]rune(tag))
	if !(TagMin <= n && n <= TagMax) {
		return "", aerr(400, "bad_gamertag", fmt.Sprintf("a gamertag has %d to %d characters", TagMin, TagMax))
	}
	if !tagOK.MatchString(tag) || strings.Contains(tag, "  ") {
		return "", aerr(400, "bad_gamertag", "use letters, digits and single spaces, starting with a letter")
	}
	key := FoldTag(tag)
	if len(key) < 2 {
		return "", aerr(400, "bad_gamertag", "too short once spaces and symbols are ignored")
	}
	words := map[string]bool{}
	for _, w := range sepRe.Split(tag, -1) {
		words[FoldTag(w)] = true
	}
	var fragment *string
	for _, bad := range append(append([]string(nil), blockedSubstr...), extraBlocked...) {
		if strings.Contains(key, bad) {
			b := bad
			fragment = &b
			break
		}
	}
	var hitWords []string
	for _, w := range extraWords {
		if words[w] {
			hitWords = append(hitWords, w)
		}
	}
	sort.Strings(hitWords)
	if fragment != nil || len(hitWords) > 0 || playerRe.MatchString(strings.ToLower(tag)) {
		var why string
		switch {
		case fragment != nil:
			why = fmt.Sprintf("contains the blocked fragment %s", pyfmt.ReprStr(*fragment))
		case len(hitWords) > 0:
			why = fmt.Sprintf("is the blocked word %s", pyList(hitWords))
		default:
			why = "looks like a default Player N name"
		}
		return "", aerr(400, "bad_gamertag", "that name is not allowed").withDetail(fmt.Sprintf("%s (folded %s) %s", pyfmt.ReprStr(tag), pyfmt.ReprStr(key), why))
	}
	return tag, nil
}

func pyList(a []string) string {
	parts := make([]string, len(a))
	for i, s := range a {
		parts[i] = pyfmt.ReprStr(s)
	}
	return "[" + strings.Join(parts, ", ") + "]"
}

// RateLimiter is sliding-window counters keyed by anything; in memory.
type RateLimiter struct {
	mu   sync.Mutex
	hits map[any][]float64
	keys []any
}

// NewRateLimiter makes an empty limiter.
func NewRateLimiter() *RateLimiter { return &RateLimiter{hits: map[any][]float64{}} }

var start = time.Now()

func monotonic() float64 { return time.Since(start).Seconds() }

// Hit counts one event; true when it is within `limit` per `window` seconds.
func (r *RateLimiter) Hit(key any, limit int, window float64) bool {
	now := monotonic()
	r.mu.Lock()
	defer r.mu.Unlock()
	var q []float64
	for _, t := range r.hits[key] {
		if now-t < window {
			q = append(q, t)
		}
	}
	ok := len(q) < limit
	if ok {
		q = append(q, now)
	}
	if len(q) > 0 {
		if _, had := r.hits[key]; !had {
			r.keys = append(r.keys, key)
		}
		r.hits[key] = q
	} else {
		delete(r.hits, key)
	}
	if len(r.hits) > 50000 {
		for _, k := range r.keys[:minInt(10000, len(r.keys))] {
			delete(r.hits, k)
		}
		r.keys = r.keys[minInt(10000, len(r.keys)):]
	}
	return ok
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}

func loadBlocklist(path string) []string {
	if path == "" {
		return nil
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	var out []string
	for _, ln := range strings.Split(string(raw), "\n") {
		ln = strings.TrimSpace(ln)
		if ln != "" && !strings.HasPrefix(ln, "#") {
			out = append(out, FoldTag(ln))
		}
	}
	return out
}

// Allowed values of the settings.
var (
	Modes         = []string{"off", "optional", "required"}
	Registrations = []string{"open", "invite", "closed"}
	TokenIPModes  = []string{"exact", "off"}
)

type cacheEnt struct {
	xuid    int64
	ip      string
	expires float64
	hard    float64
	tag     string
	banned  *float64
	until   *float64
	reason  string
	written float64
	checked float64
}

type banEnt struct {
	checked float64
	text    string
	has     bool
}

// Service is the identity service.
type Service struct {
	DB           *db.DB
	Env          string
	Mode         string
	Registration string
	TokenIP      string
	Clock        func() float64
	Limits       *RateLimiter
	BlockedFile  []string
	Blocked      []string
	BlockedWords []string
	cache        map[string]*cacheEnt
	cacheMu      sync.Mutex
	banCache     map[int64]banEnt
	secret       []byte
	Scale        float64
	PerIPDaily   int
	GlobalHourly int
	PowBase      int
	PowMax       int
	OnChange     func(xuid int64)
	OnRevoke     func(xuid int64)
}

func oneOf(v string, allowed []string) bool {
	for _, a := range allowed {
		if a == v {
			return true
		}
	}
	return false
}

func envInt(name string, def int) int {
	if v, err := strconv.Atoi(strings.TrimSpace(os.Getenv(name))); err == nil {
		return v
	}
	return def
}

// NewService builds the service; settings are validated (a typo must not become a weaker setting).
func NewService(d *db.DB, env, mode, registration, tokenIP string) (*Service, error) {
	for _, c := range []struct {
		name, v string
		ok      []string
	}{{"FH2MP_AUTH", mode, Modes}, {"FH2MP_REGISTRATION", registration, Registrations}, {"FH2MP_TOKEN_IP", tokenIP, TokenIPModes}} {
		if !oneOf(c.v, c.ok) {
			return nil, fmt.Errorf("%s=%s: expected one of %s", c.name, pyfmt.ReprStr(c.v), strings.Join(c.ok, ", "))
		}
	}
	s := &Service{DB: d, Env: env, Mode: mode, Registration: registration, TokenIP: tokenIP, Clock: func() float64 { return float64(time.Now().UnixNano()) / 1e9 },
		Limits: NewRateLimiter(), cache: map[string]*cacheEnt{}, banCache: map[int64]banEnt{}}
	s.BlockedFile = loadBlocklist(os.Getenv("FH2MP_BLOCKLIST"))
	s.Blocked = s.BlockedFile
	scale := 1.0
	if v, err := strconv.ParseFloat(os.Getenv("FH2MP_RATE_SCALE"), 64); err == nil {
		scale = v
	}
	s.Scale = scale
	sc := int(math.Max(1, float64(int(scale))))
	s.PerIPDaily = envInt("FH2MP_REG_PER_IP_DAY", 5) * sc
	s.GlobalHourly = envInt("FH2MP_REG_PER_HOUR", 300) * sc
	powEnv := os.Getenv("FH2MP_POW_BITS")
	if powEnv == "" {
		powEnv = fmt.Sprintf("%d,%d", PowBaseBits, PowMaxBits)
	}
	parts := strings.Split(powEnv, ",")
	s.PowBase, _ = strconv.Atoi(strings.TrimSpace(parts[0]))
	s.PowMax, _ = strconv.Atoi(strings.TrimSpace(parts[len(parts)-1]))
	sec, err := s.loadSecret()
	if err != nil {
		return nil, err
	}
	s.secret = sec
	return s, nil
}

func (s *Service) hit(key any, limit int, window float64) bool {
	n := int(float64(limit) * s.Scale)
	if n < 1 {
		n = 1
	}
	return s.Limits.Hit(key, n, window)
}

func (s *Service) loadSecret() ([]byte, error) {
	if v := os.Getenv("FH2MP_AUTH_SECRET"); v != "" {
		return []byte(v), nil
	}
	v, err := s.DB.MetaGet("auth_secret", "")
	if err != nil {
		return nil, err
	}
	if v == "" {
		b := make([]byte, 32)
		rand.Read(b)
		v = hex.EncodeToString(b)
		if err := s.DB.Do(func(t *db.Tx) error {
			_, err := t.Exec("INSERT INTO meta (key, value) VALUES ('auth_secret', ?) ON CONFLICT (key) DO NOTHING", v)
			return err
		}); err != nil {
			return nil, err
		}
		if v, err = s.DB.MetaGet("auth_secret", ""); err != nil {
			return nil, err
		}
	}
	return []byte(v), nil
}

func (s *Service) mac(data []byte) []byte {
	m := hmac.New(sha256.New, s.secret)
	m.Write([]byte("chal|" + s.Env + "|"))
	m.Write(data)
	return m.Sum(nil)[:16]
}

// PowBits is the proof-of-work difficulty for new registrations.
func (s *Service) PowBits() (int, error) {
	r, err := s.DB.One("SELECT COUNT(*) FROM accounts WHERE created > ?", s.Clock()-600)
	if err != nil {
		return 0, err
	}
	n := int(r.IntAt(0))
	b := s.PowBase + n/10
	if b > s.PowMax {
		b = s.PowMax
	}
	return b, nil
}

// Challenge makes a stateless, MAC-protected challenge: timestamp | pow bits | random | mac. Returns (challenge, bits, ttl).
func (s *Service) Challenge(ip string) (string, int, float64, error) {
	if !s.hit([2]any{"chal", NormIP(ip)}, 60, 60) {
		return "", 0, 0, aerr(429, "rate_limited", "too many requests, wait a minute").withDetail("challenge requests: over 60 per minute from this address")
	}
	bits, err := s.PowBits()
	if err != nil {
		return "", 0, 0, err
	}
	body := make([]byte, 9, 25)
	binary.BigEndian.PutUint64(body, math.Float64bits(s.Clock()))
	body[8] = byte(bits)
	rnd := make([]byte, 16)
	rand.Read(rnd)
	body = append(body, rnd...)
	raw := append(append([]byte(nil), body...), s.mac(body)...)
	return B64u(raw), bits, ChallengeTTL, nil
}

func (s *Service) openChallenge(chal string) ([]byte, int, error) {
	raw, err := unb64u(chal)
	if err != nil {
		return nil, 0, err
	}
	if len(raw) != 8+1+16+16 {
		return nil, 0, aerr(400, "bad_challenge", "").withDetail(fmt.Sprintf("challenge is %d bytes, expected 41", len(raw)))
	}
	body, mac := raw[:len(raw)-16], raw[len(raw)-16:]
	if subtle.ConstantTimeCompare(mac, s.mac(body)) != 1 {
		return nil, 0, aerr(400, "bad_challenge", "").withDetail(fmt.Sprintf("challenge MAC does not verify for realm %s (made by another realm, or altered)", pyfmt.ReprStr(s.Env)))
	}
	ts := math.Float64frombits(binary.BigEndian.Uint64(body[:8]))
	bits := int(body[8])
	now := s.Clock()
	if !(now-ChallengeTTL <= ts && ts <= now+5) {
		return nil, 0, aerr(400, "challenge_expired", "the challenge is too old, ask for a new one").withDetail(
			fmt.Sprintf("challenge is %.0f s old, allowed 0 to %.0f s (negative = the client's clock is ahead)", now-ts, ChallengeTTL))
	}
	var used bool
	err = s.DB.Do(func(t *db.Tx) error {
		if _, err := t.Exec("DELETE FROM auth_nonces WHERE expires < ?", now); err != nil {
			return err
		}
		res, err := t.Exec("INSERT INTO auth_nonces (nonce, expires) VALUES (?, ?) ON CONFLICT DO NOTHING", body[9:25], ts+ChallengeTTL)
		if err != nil {
			return err
		}
		used = res.RowsAffected != 1
		return nil
	})
	if err != nil {
		return nil, 0, err
	}
	if used {
		return nil, 0, aerr(400, "challenge_used", "that challenge was already used").withDetail("its nonce is already burnt (a repeated request, or a replay)")
	}
	return raw, bits, nil
}

func verify(pubkey, sig, msg []byte) bool {
	if len(pubkey) != ed25519.PublicKeySize {
		return false
	}
	return ed25519.Verify(ed25519.PublicKey(pubkey), msg, sig)
}

func (s *Service) event(kind, ip string, xuid *int64, detail string) {
	if len(detail) > 200 {
		detail = detail[:200]
	}
	err := s.DB.Do(func(t *db.Tx) error {
		var x any
		if xuid != nil {
			x = *xuid
		}
		if _, err := t.Exec("INSERT INTO auth_events (at, kind, ip, xuid, detail) VALUES (?,?,?,?,?)", s.Clock(), kind, NormIP(ip), x, detail); err != nil {
			return err
		}
		n, _ := rand.Int(rand.Reader, big.NewInt(200))
		if n.Int64() == 0 {
			_, err := t.Exec("DELETE FROM auth_events WHERE at < ?", s.Clock()-90*86400)
			return err
		}
		return nil
	})
	if err != nil {
		if ok, skipped := rate.Allow("audit-write", 300); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (%d more failed since the last line)", skipped)
			}
			log.Exc(fmt.Sprintf("audit event %s not written, the sign-in carries on%s", pyfmt.ReprStr(kind), extra), err, diag.Warn)
		}
	}
}

func i64p(x int64) *int64 { return &x }

// Register makes an account. powCounter < 0 stands for a value that was not a non-negative integer.
func (s *Service) Register(ip, challenge, pubkeyB64, gamertag string, powCounter int64, sigB64, invite string) (*Account, error) {
	ip = NormIP(ip)
	if s.Registration == "closed" {
		return nil, aerr(403, "registration_closed", "new accounts are not being accepted right now").withDetail("FH2MP_REGISTRATION / the panel setting is 'closed'")
	}
	pubkey, err := unb64u(pubkeyB64)
	if err != nil {
		return nil, err
	}
	sig, err := unb64u(sigB64)
	if err != nil {
		return nil, err
	}
	if len(pubkey) != 32 || len(sig) != 64 || powCounter < 0 {
		return nil, aerr(400, "bad_request", "malformed field").withDetail(fmt.Sprintf("pubkey %d bytes (want 32), signature %d bytes (want 64), pow int %d", len(pubkey), len(sig), powCounter))
	}
	if !s.hit([2]any{"reg", ip}, 10, 3600) {
		return nil, aerr(429, "rate_limited", "too many attempts from this address").withDetail("registrations: over 10 per hour from this address")
	}
	tag, err := ValidateGamertag(gamertag, s.Blocked, s.BlockedWords)
	if err != nil {
		return nil, err
	}
	raw, bits, err := s.openChallenge(challenge)
	if err != nil {
		return nil, err
	}
	if !PowOK(raw, pubkey, uint64(powCounter), bits) {
		return nil, aerr(400, "bad_pow", "proof of work does not meet the target").withDetail(fmt.Sprintf("counter %d does not give %d zero bits", powCounter, bits))
	}
	if !verify(pubkey, sig, SignMessage(s.Env, "register", raw, pubkey, []byte(tag))) {
		s.event("register_badsig", ip, nil, "")
		return nil, aerr(401, "bad_signature", "").withDetail(fmt.Sprintf("registration signature does not verify for realm %s", pyfmt.ReprStr(s.Env)))
	}
	now := s.Clock()
	r, err := s.DB.One("SELECT COUNT(*) FROM accounts WHERE created_ip=? AND created > ?", ip, now-86400)
	if err != nil {
		return nil, err
	}
	if made := int(r.IntAt(0)); made >= s.PerIPDaily {
		return nil, aerr(429, "rate_limited", "too many accounts were created from this address today").withDetail(fmt.Sprintf("%d accounts in 24 h from this address, limit %d", made, s.PerIPDaily))
	}
	if r, err = s.DB.One("SELECT COUNT(*) FROM accounts WHERE created > ?", now-3600); err != nil {
		return nil, err
	}
	if made := int(r.IntAt(0)); made >= s.GlobalHourly {
		return nil, aerr(503, "busy", "a lot of accounts are being created, try again later").withDetail(fmt.Sprintf("%d accounts in the last hour, limit %d", made, s.GlobalHourly))
	}
	key := FoldTag(tag)
	var xuid int64
	err = s.DB.Do(func(t *db.Tx) error {
		if s.Registration == "invite" {
			code := sha256.Sum256([]byte(strings.ToUpper(strings.TrimSpace(invite))))
			res, err := t.Exec("UPDATE invites SET uses_left=uses_left-1 WHERE code_hash=? AND uses_left>0", code[:])
			if err != nil {
				return err
			}
			if res.RowsAffected != 1 {
				why := "is unknown or used up"
				if strings.TrimSpace(invite) == "" {
					why = "was not sent"
				}
				return aerr(403, "invite_required", "a valid invite code is needed to register").withDetail("registration is invite-only and the code " + why)
			}
		}
		if row, err := t.One("SELECT 1 FROM accounts WHERE pubkey=?", pubkey); err != nil {
			return err
		} else if row != nil {
			return aerr(409, "key_exists", "this key already has an account: sign in instead").withDetail("the public key is registered already")
		}
		if row, err := t.One("SELECT 1 FROM accounts WHERE gamertag_key=?", key); err != nil {
			return err
		} else if row != nil {
			return aerr(409, "gamertag_taken", "that gamertag is taken").withDetail(fmt.Sprintf("%s folds to %s, which an account has", pyfmt.ReprStr(tag), pyfmt.ReprStr(key)))
		}
		found := false
		for i := 0; i < 20; i++ {
			var b [4]byte
			rand.Read(b[:])
			xuid = int64(XuidBase | uint64(binary.BigEndian.Uint32(b[:])|0x10000))
			row, err := t.One("SELECT 1 FROM accounts WHERE xuid=? UNION ALL SELECT 1 FROM players WHERE xuid=?", xuid, xuid)
			if err != nil {
				return err
			}
			if row == nil {
				found = true
				break
			}
		}
		if !found {
			return aerr(503, "busy", "could not allocate an id").withDetail("20 random xuids were all taken")
		}
		if _, err := t.Exec("INSERT INTO accounts (xuid, pubkey, gamertag, gamertag_key, created, created_ip) VALUES (?,?,?,?,?,?)", xuid, pubkey, tag, key, now, ip); err != nil {
			return err
		}
		_, err := t.Exec("INSERT INTO players (xuid, gamertag, career_level, first_seen, last_seen) VALUES (?,?,0,?,?) ON CONFLICT (xuid) DO UPDATE SET gamertag=excluded.gamertag", xuid, tag, now, now)
		return err
	})
	if err != nil {
		return nil, err
	}
	s.event("register", ip, i64p(xuid), tag)
	return &Account{Xuid: xuid, Gamertag: tag, Pubkey: pubkey}, nil
}

func hashToken(token string) []byte { h := sha256.Sum256([]byte(token)); return h[:] }

func (s *Service) forget(hashes [][]byte) {
	s.cacheMu.Lock()
	for _, h := range hashes {
		delete(s.cache, string(h))
	}
	s.cacheMu.Unlock()
}

func fptr(r *db.Row, name string) *float64 {
	if r.IsNull(name) {
		return nil
	}
	v := r.Float(name)
	return &v
}

// Login signs an account in: returns the token, the account and how long the token stays valid when idle.
func (s *Service) Login(ip, challenge, pubkeyB64, sigB64, client string) (string, *Account, float64, error) {
	ip = NormIP(ip)
	pubkey, err := unb64u(pubkeyB64)
	if err != nil {
		return "", nil, 0, err
	}
	sig, err := unb64u(sigB64)
	if err != nil {
		return "", nil, 0, err
	}
	if len(pubkey) != 32 || len(sig) != 64 {
		return "", nil, 0, aerr(400, "bad_request", "malformed field").withDetail(fmt.Sprintf("pubkey %d bytes (want 32), signature %d bytes (want 64)", len(pubkey), len(sig)))
	}
	byIP := s.hit([2]any{"login", ip}, 30, 60)
	if !byIP || !s.hit([2]any{"login-key", string(pubkey)}, 12, 60) {
		d := "sign-ins: over 12 per minute for this key"
		if !byIP {
			d = "sign-ins: over 30 per minute from this address"
		}
		return "", nil, 0, aerr(429, "rate_limited", "too many sign-in attempts").withDetail(d)
	}
	raw, _, err := s.openChallenge(challenge)
	if err != nil {
		return "", nil, 0, err
	}
	if !verify(pubkey, sig, SignMessage(s.Env, "login", raw, pubkey)) {
		s.event("login_badsig", ip, nil, "")
		return "", nil, 0, aerr(401, "bad_signature", "").withDetail(fmt.Sprintf("sign-in signature does not verify for realm %s (a launcher of another realm, or a different key)", pyfmt.ReprStr(s.Env)))
	}
	row, err := s.DB.One("SELECT xuid, gamertag, banned_at, ban_until, ban_reason FROM accounts WHERE pubkey=?", pubkey)
	if err != nil {
		return "", nil, 0, err
	}
	if row == nil {
		return "", nil, 0, aerr(404, "unknown_account", "no account for this key: sign up first").withDetail(fmt.Sprintf("no account has this public key in realm %s", pyfmt.ReprStr(s.Env)))
	}
	xuid := row.Int("xuid")
	if BanActive(fptr(row, "banned_at"), fptr(row, "ban_until"), s.Clock()) {
		s.event("login_banned", ip, i64p(xuid), "")
		return "", nil, 0, aerr(403, "banned", BanMessage(row.Str("ban_reason"), fptr(row, "ban_until"))).withXuid(xuid)
	}
	tb := make([]byte, 32)
	rand.Read(tb)
	token := "fh2." + B64u(tb)
	now := s.Clock()
	type sess struct {
		client   string
		lastUsed float64
	}
	var old [][]byte
	var rows []sess
	err = s.DB.Do(func(t *db.Tx) error {
		rs, err := t.Query("SELECT token_hash, client, last_used FROM auth_sessions WHERE xuid=?", xuid)
		if err != nil {
			return err
		}
		for _, r := range rs {
			old = append(old, append([]byte(nil), r.Any("token_hash").([]byte)...))
			rows = append(rows, sess{r.Str("client"), r.Float("last_used")})
		}
		if _, err := t.Exec("DELETE FROM auth_sessions WHERE xuid=?", xuid); err != nil {
			return err
		}
		c := client
		if len(c) > 120 {
			c = c[:120]
		}
		if _, err := t.Exec("INSERT INTO auth_sessions (token_hash, xuid, ip, client, created, last_used, expires, hard_expires) VALUES (?,?,?,?,?,?,?,?)",
			hashToken(token), xuid, ip, c, now, now, now+SessionIdle, now+SessionMax); err != nil {
			return err
		}
		_, err = t.Exec("UPDATE accounts SET last_login=? WHERE xuid=?", now, xuid)
		return err
	})
	if err != nil {
		return "", nil, 0, err
	}
	s.forget(old)
	if len(rows) > 0 {
		newest := rows[0]
		for _, r := range rows[1:] {
			if r.lastUsed > newest.lastUsed {
				newest = r
			}
		}
		idle := int(now - newest.lastUsed)
		oldClient := newest.client
		if len(oldClient) > 80 {
			oldClient = oldClient[:80]
		}
		newClient := client
		if len(newClient) > 80 {
			newClient = newClient[:80]
		}
		var oc, nc any
		if oldClient != "" {
			oc = oldClient
		}
		if newClient != "" {
			nc = newClient
		}
		text := "auth.session_replaced " + diag.KV(diag.KVP{K: "xuid", V: fmt.Sprintf("%#x", xuid)}, diag.KVP{K: "replaced", V: len(rows)}, diag.KVP{K: "old_client", V: oc},
			diag.KVP{K: "old_idle_s", V: idle}, diag.KVP{K: "new_client", V: nc})
		if idle < SessionLiveS {
			log.Warn(text + ": the old token was in use moments ago, whoever holds it now gets 401")
		} else {
			log.Info(text)
		}
	}
	cl := client
	if len(cl) > 80 {
		cl = cl[:80]
	}
	s.event("login", ip, i64p(xuid), cl)
	return token, &Account{Xuid: xuid, Gamertag: row.Str("gamertag"), Pubkey: pubkey}, SessionIdle, nil
}

// Authenticate returns the Account for a live token used from the address that logged in.
func (s *Service) Authenticate(token, ip string) (*Account, error) {
	if token == "" || !strings.HasPrefix(token, "fh2.") || len(token) > 80 {
		d := "not a session token: empty"
		if token != "" {
			d = fmt.Sprintf("not a session token: %d characters, ", len(token))
			if !strings.HasPrefix(token, "fh2.") {
				d += "no fh2. prefix"
			} else {
				d += "longer than 80"
			}
		}
		return nil, aerr(401, "invalid_token", "").withDetail(d)
	}
	h, now, ip := hashToken(token), s.Clock(), NormIP(ip)
	s.cacheMu.Lock()
	ent := s.cache[string(h)]
	s.cacheMu.Unlock()
	if ent == nil || now-ent.checked > CacheSeconds {
		r, err := s.DB.One("SELECT s.xuid, s.ip, s.expires, s.hard_expires, s.last_used, a.gamertag, a.banned_at, a.ban_until, a.ban_reason "+
			"FROM auth_sessions s JOIN accounts a ON a.xuid = s.xuid WHERE s.token_hash=? AND s.revoked=0", h)
		if err != nil {
			return nil, err
		}
		if r == nil {
			s.cacheMu.Lock()
			delete(s.cache, string(h))
			s.cacheMu.Unlock()
			return nil, aerr(401, "invalid_token", "").withDetail("no live session for this token (signed out, replaced by a newer sign-in, revoked or kicked, or issued by another realm)")
		}
		ent = &cacheEnt{xuid: r.Int("xuid"), ip: r.Str("ip"), expires: r.Float("expires"), hard: r.Float("hard_expires"), tag: r.Str("gamertag"),
			banned: fptr(r, "banned_at"), until: fptr(r, "ban_until"), reason: r.Str("ban_reason"), written: r.Float("last_used"), checked: now}
		s.cacheMu.Lock()
		s.cache[string(h)] = ent
		s.cacheMu.Unlock()
	}
	if BanActive(ent.banned, ent.until, now) {
		return nil, aerr(403, "banned", BanMessage(ent.reason, ent.until)).withXuid(ent.xuid)
	}
	if now > ent.expires || now > ent.hard {
		d := ""
		if now > ent.expires {
			d += fmt.Sprintf("idle limit passed %.0f s ago", now-ent.expires)
		}
		if now > ent.expires && now > ent.hard {
			d += " and "
		}
		if now > ent.hard {
			d += fmt.Sprintf("72 h hard limit passed %.0f s ago", now-ent.hard)
		}
		return nil, aerr(401, "token_expired", "the session expired: sign in again from the launcher").withXuid(ent.xuid).withDetail(d)
	}
	if s.TokenIP == "exact" && ip != "" && ent.ip != ip {
		return nil, aerr(401, "ip_changed", "your address changed: start the game from the launcher again").withXuid(ent.xuid).withDetail("the request comes from another address than the sign-in (FH2MP_TOKEN_IP=exact)")
	}
	if now-ent.written > TouchEvery {
		ent.written, ent.expires = now, math.Min(now+SessionIdle, ent.hard)
		exp := ent.expires
		if err := s.DB.Do(func(t *db.Tx) error {
			_, err := t.Exec("UPDATE auth_sessions SET last_used=?, expires=? WHERE token_hash=?", now, exp, h)
			return err
		}); err != nil {
			return nil, err
		}
	}
	return &Account{Xuid: ent.xuid, Gamertag: ent.tag}, nil
}

// Logout ends a session.
func (s *Service) Logout(token string) error {
	h := hashToken(token)
	err := s.DB.Do(func(t *db.Tx) error {
		_, err := t.Exec("DELETE FROM auth_sessions WHERE token_hash=?", h)
		return err
	})
	s.forget([][]byte{h})
	return err
}

// Account returns the stored account row (nil = none).
func (s *Service) Account(xuid int64) (*db.Row, error) {
	return s.DB.One("SELECT xuid, pubkey, gamertag, created, last_login, tag_changed, banned_at, ban_until, ban_reason FROM accounts WHERE xuid=?", xuid)
}

// Rename changes a gamertag (once per cooldown unless forced).
func (s *Service) Rename(xuid int64, newTag string, force bool) (string, error) {
	tag, err := ValidateGamertag(newTag, s.Blocked, s.BlockedWords)
	if err != nil {
		return "", err
	}
	key := FoldTag(tag)
	now := s.Clock()
	err = s.DB.Do(func(t *db.Tx) error {
		r, err := t.One("SELECT tag_changed, gamertag_key FROM accounts WHERE xuid=?", xuid)
		if err != nil {
			return err
		}
		if r == nil {
			return aerr(404, "unknown_account", "").withXuid(xuid).withDetail("rename of an xuid without an account")
		}
		if !force && !r.IsNull("tag_changed") && r.Float("tag_changed") != 0 && now-r.Float("tag_changed") < TagCooldown {
			days := (TagCooldown - (now - r.Float("tag_changed"))) / 86400
			return aerr(429, "rename_cooldown", fmt.Sprintf("you can change your gamertag again in %.1f days", days)).withXuid(xuid).withDetail(
				fmt.Sprintf("last change %.1f days ago, cooldown %.0f days", (now-r.Float("tag_changed"))/86400, TagCooldown/86400))
		}
		if key != r.Str("gamertag_key") {
			if o, err := t.One("SELECT 1 FROM accounts WHERE gamertag_key=?", key); err != nil {
				return err
			} else if o != nil {
				return aerr(409, "gamertag_taken", "that gamertag is taken").withXuid(xuid).withDetail(fmt.Sprintf("%s folds to %s, which another account has", pyfmt.ReprStr(tag), pyfmt.ReprStr(key)))
			}
		}
		if _, err := t.Exec("UPDATE accounts SET gamertag=?, gamertag_key=?, tag_changed=? WHERE xuid=?", tag, key, now, xuid); err != nil {
			return err
		}
		_, err = t.Exec("UPDATE players SET gamertag=? WHERE xuid=?", tag, xuid)
		return err
	})
	if err != nil {
		return "", err
	}
	s.cacheMu.Lock()
	for _, e := range s.cache {
		if e.xuid == xuid {
			e.tag = tag
		}
	}
	s.cacheMu.Unlock()
	s.event("rename", "", i64p(xuid), tag)
	if s.OnChange != nil {
		s.OnChange(xuid)
	}
	return tag, nil
}

// TagAvailable says whether a gamertag is free.
func (s *Service) TagAvailable(ip, tag string) (bool, error) {
	if !s.hit([2]any{"tagcheck", NormIP(ip)}, 40, 60) {
		return false, aerr(429, "rate_limited", "slow down").withDetail("gamertag checks: over 40 per minute from this address")
	}
	t, err := ValidateGamertag(tag, s.Blocked, s.BlockedWords)
	if err != nil {
		return false, err
	}
	r, err := s.DB.One("SELECT 1 FROM accounts WHERE gamertag_key=?", FoldTag(t))
	return r == nil, err
}

// SetBanned bans (reason != nil) or unbans an account; until makes the ban temporary.
func (s *Service) SetBanned(xuid int64, reason *string, until *float64) (bool, error) {
	if until != nil && *until <= s.Clock() {
		return false, aerr(400, "bad_request", "a ban must end in the future").withXuid(xuid).withDetail(fmt.Sprintf("ban end %.0f is %.0f s in the past", *until, s.Clock()-*until))
	}
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		var at, un, rs any
		rs = ""
		if reason != nil {
			at, rs = s.Clock(), *reason
			if until != nil {
				un = *until
			}
		}
		res, err := t.Exec("UPDATE accounts SET banned_at=?, ban_until=?, ban_reason=? WHERE xuid=?", at, un, rs, xuid)
		n = res.RowsAffected
		return err
	})
	if err != nil {
		return false, err
	}
	s.cacheMu.Lock()
	delete(s.banCache, xuid)
	for h, e := range s.cache {
		if e.xuid == xuid {
			delete(s.cache, h)
		}
	}
	s.cacheMu.Unlock()
	kind, detail := "unban", ""
	if reason != nil {
		kind, detail = "ban", *reason
	}
	s.event(kind, "", i64p(xuid), detail)
	if s.OnChange != nil {
		s.OnChange(xuid)
	}
	if reason != nil && s.OnRevoke != nil {
		s.OnRevoke(xuid)
	}
	return n > 0, nil
}

// BanText is the ban message while the account is banned, else "" (for requests identified by X-UserId alone).
func (s *Service) BanText(xuid int64) (string, error) {
	now := s.Clock()
	s.cacheMu.Lock()
	hit, ok := s.banCache[xuid]
	s.cacheMu.Unlock()
	if !ok || now-hit.checked > CacheSeconds {
		r, err := s.DB.One("SELECT banned_at, ban_until, ban_reason FROM accounts WHERE xuid=?", xuid)
		if err != nil {
			return "", err
		}
		hit = banEnt{checked: now}
		if r != nil && BanActive(fptr(r, "banned_at"), fptr(r, "ban_until"), now) {
			hit.text, hit.has = BanMessage(r.Str("ban_reason"), fptr(r, "ban_until")), true
		}
		s.cacheMu.Lock()
		if len(s.banCache) > 20000 {
			s.banCache = map[int64]banEnt{}
		}
		s.banCache[xuid] = hit
		s.cacheMu.Unlock()
	}
	return hit.text, nil
}

// Revoke ends every session of an account.
func (s *Service) Revoke(xuid int64) (int64, error) {
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		res, err := t.Exec("DELETE FROM auth_sessions WHERE xuid=?", xuid)
		n = res.RowsAffected
		return err
	})
	if err != nil {
		return 0, err
	}
	s.cacheMu.Lock()
	for h, e := range s.cache {
		if e.xuid == xuid {
			delete(s.cache, h)
		}
	}
	s.cacheMu.Unlock()
	if s.OnRevoke != nil {
		s.OnRevoke(xuid)
	}
	return n, nil
}

// AddInvite stores an invite code.
func (s *Service) AddInvite(code string, uses int, note string) error {
	h := sha256.Sum256([]byte(strings.ToUpper(strings.TrimSpace(code))))
	return s.DB.Do(func(t *db.Tx) error {
		_, err := t.Exec("INSERT INTO invites (code_hash, uses_left, note, created) VALUES (?,?,?,?) ON CONFLICT (code_hash) DO UPDATE SET uses_left=excluded.uses_left",
			h[:], uses, note, s.Clock())
		return err
	})
}

// Stats counts accounts and live sessions.
func (s *Service) Stats() (map[string]int64, error) {
	now := s.Clock()
	out := map[string]int64{}
	for _, q := range []struct {
		k, sql string
		arg    []any
	}{{"accounts", "SELECT COUNT(*) FROM accounts", nil}, {"sessions", "SELECT COUNT(*) FROM auth_sessions WHERE expires > ?", []any{now}},
		{"registered_24h", "SELECT COUNT(*) FROM accounts WHERE created > ?", []any{now - 86400}}} {
		r, err := s.DB.One(q.sql, q.arg...)
		if err != nil {
			return nil, err
		}
		out[q.k] = r.IntAt(0)
	}
	return out, nil
}
