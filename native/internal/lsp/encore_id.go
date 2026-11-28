package lsp

// Encore ID, first slice: the accounts API of the FH2MP LSP.
//
// Accounts are centralized (decision 2026-11-17): the launcher-made Ed25519 accounts, their sessions and bans (docs/auth.md) live behind this
// server, and every game server of the platform (FH3MP today, FM6 next) asks it who a token belongs to instead of reading the tables. The API
// is internal: it answers only a caller on this host and only one that knows the service secret.
//
// Access (all of these, else the call is refused and the body is never looked at):
//
//   - FH2MP_ENCORE_ID_SECRET_FILE names a file holding the service secret (at least 32 characters, surrounding white space ignored). A name
//     that is not an absolute path is looked up in $CREDENTIALS_DIRECTORY, so systemd's  LoadCredential=encore-id-secret:/etc/encore/id.secret
//     with FH2MP_ENCORE_ID_SECRET_FILE=encore-id-secret  hands it over without a copy readable by other services. The secret is never in the
//     environment, in the code or in a log. A set but unreadable or too short file stops the start (a typo must not turn the API off silently);
//     unset = the API is off and every /encore/ path answers 404 disabled.
//   - the TCP peer is a loopback address and the request carries no X-Real-IP / X-Forwarded-For (the public reverse proxy sets one of them),
//     else 403 forbidden.
//   - the request carries  Authorization: Bearer <secret>, else 401 service_unauthorized. Wrong secrets are counted per peer: more than 10 a
//     minute answer 429 rate_limited (a right secret is never refused for that).
//
// Every answer is JSON, Cache-Control: no-store. A refusal is {"error": <code>, "message": <text a player may be told>}. Bodies are at most 4 KB.
// Nothing here is logged with a token in it; refusals are logged once per two minutes per route and code.
//
//	POST /encore/v1/id/authenticate   {"token": "fh2.…", "ip": "203.0.113.7"?}
//	    The account of a live session token, the rules of AuthService.Authenticate (docs/auth.md): the SHA-256 of the token names a session that
//	    is not revoked, not past its 12 h idle or 72 h hard limit, and whose account is not banned (a temporary ban ends at ban_until by
//	    itself). A session in use is kept alive (last_used, expires) at most every 5 minutes, exactly as for a game of this server. "ip" is
//	    optional: with FH2MP_TOKEN_IP=exact the sign-in address is compared when it is given and skipped when it is not (a game server that sits
//	    behind another proxy cannot know the player's address). The call does NOT announce the player to this server's UDP game server.
//	    200 {"xuid": 2533274790396001, "gamertag": "Name"}          xuid is a JSON number below 2^53; 200 always means a usable identity
//	    401 invalid_token      no live session: unknown, signed out, replaced by a newer sign-in, revoked, kicked, or not a token ("fh2." + at most 76)
//	    401 token_expired      the session passed its idle or hard limit
//	    401 ip_changed         FH2MP_TOKEN_IP=exact and "ip" is not the sign-in address
//	    403 banned             {"error":"banned","message":"This account is banned … Reason: …","banned":true,"xuid":N}; never a 200 with a flag, so a
//	                           caller that forgets to look cannot let a banned player in
//	    400 bad_request        not JSON, no "token" string
//	    503 storage            the database failed (retry; the caller must not let the player in)
//
//	GET  /encore/v1/id/ping
//	    200 {"service": "encore-id", "version": 1}: the secret and the route are right. For a game server's start-up check.
//
// Errors of the service itself, as above for every route: 404 disabled, 403 forbidden, 401 service_unauthorized, 429 rate_limited, 404
// not_found (unknown path under /encore/), 405 method_not_allowed. A caller maps a 401/403 to a player's refusal only for the codes
// invalid_token, token_expired, ip_changed and banned; every other answer or no answer is a storage failure on its side.

import (
	"crypto/subtle"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strings"

	"fh2mp/internal/auth"
	"fh2mp/internal/diag"
)

const (
	encorePrefix       = "/encore/"
	encoreBodyLimit    = 4096
	encoreSecretMin    = 32
	encoreRefusalSecs  = 120.0
	encoreSecretEnv    = "FH2MP_ENCORE_ID_SECRET_FILE"
	encoreRouteAuth    = "/encore/v1/id/authenticate"
	encoreRoutePing    = "/encore/v1/id/ping"
	encoreWrongPerMin  = 10
	encoreWrongWindowS = 60
)

var encoreLog = diag.Get("encore-id")

// encoreAuthenticator is what the API needs of the identity service (*auth.Service).
type encoreAuthenticator interface {
	Authenticate(token, ip string) (*auth.Account, error)
	Hit(key any, limit int, window float64) bool
}

// EncoreID is the accounts API.
type EncoreID struct {
	auth   encoreAuthenticator
	secret []byte
	rate   *diag.RateLimit
}

// NewEncoreID makes the API for an identity service and a service secret.
func NewEncoreID(a encoreAuthenticator, secret []byte) *EncoreID {
	return &EncoreID{auth: a, secret: secret, rate: diag.NewRateLimit(nil, 0)}
}

// EncoreIDFromEnv reads the service secret named by FH2MP_ENCORE_ID_SECRET_FILE. nil without an error means the API is off.
func EncoreIDFromEnv(a encoreAuthenticator) (*EncoreID, error) {
	name := strings.TrimSpace(os.Getenv(encoreSecretEnv))
	if name == "" {
		return nil, nil
	}
	path := name
	if !filepath.IsAbs(path) {
		dir := os.Getenv("CREDENTIALS_DIRECTORY")
		if dir == "" {
			return nil, fmt.Errorf("%s=%s is not an absolute path and there is no $CREDENTIALS_DIRECTORY (systemd LoadCredential=) to look it up in", encoreSecretEnv, name)
		}
		path = filepath.Join(dir, name)
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("%s: the Encore ID service secret cannot be read: %v", encoreSecretEnv, err)
	}
	secret := strings.TrimSpace(string(raw))
	if len(secret) < encoreSecretMin {
		return nil, fmt.Errorf("%s: the service secret in %s is %d characters, at least %d are needed (openssl rand -hex 32)", encoreSecretEnv, path, len(secret), encoreSecretMin)
	}
	return NewEncoreID(a, []byte(secret)), nil
}

type encoreError struct {
	status        int
	code, message string
	detail        string
	extra         map[string]any
}

func encErr(status int, code, message string) *encoreError {
	return &encoreError{status: status, code: code, message: message}
}

// Handle answers one /encore/ request: the status and the JSON body. It never fails.
func (e *EncoreID) Handle(method, path string, h http.Header, body []byte, peer string, proxied bool) (status int, out []byte) {
	route := path
	if i := strings.IndexByte(route, '?'); i >= 0 {
		route = route[:i]
	}
	res, err := e.serve(method, route, h, body, peer, proxied)
	if err != nil {
		e.refused(method, route, err)
		obj := map[string]any{"error": err.code, "message": err.message}
		for k, v := range err.extra {
			obj[k] = v
		}
		out, _ = json.Marshal(obj)
		return err.status, out
	}
	out, _ = json.Marshal(res)
	return 200, out
}

func (e *EncoreID) serve(method, route string, h http.Header, body []byte, peer string, proxied bool) (any, *encoreError) {
	if proxied || !isLoopback(peer) {
		d := "the sender is not a loopback address"
		if proxied {
			d = "the request carries proxy headers (X-Real-IP / X-Forwarded-For): it came through the public proxy"
		}
		return nil, encErr(403, "forbidden", "").detailed(d)
	}
	given := bearer(h)
	if subtle.ConstantTimeCompare([]byte(given), e.secret) != 1 {
		if !e.auth.Hit([2]any{"encore-fail", peer}, encoreWrongPerMin, encoreWrongWindowS) {
			return nil, encErr(429, "rate_limited", "too many wrong service secrets").detailed("over 10 wrong secrets per minute from this sender")
		}
		d := "no bearer secret sent"
		if given != "" {
			d = fmt.Sprintf("the bearer secret does not match (%d characters sent)", len([]rune(given)))
		}
		return nil, encErr(401, "service_unauthorized", "").detailed(d)
	}
	switch route {
	case encoreRoutePing:
		if method != "GET" {
			return nil, encErr(405, "method_not_allowed", "")
		}
		return map[string]any{"service": "encore-id", "version": 1}, nil
	case encoreRouteAuth:
		if method != "POST" {
			return nil, encErr(405, "method_not_allowed", "")
		}
		return e.authenticate(body)
	}
	return nil, encErr(404, "not_found", "")
}

func (x *encoreError) detailed(d string) *encoreError { x.detail = d; return x }

func (e *EncoreID) authenticate(body []byte) (any, *encoreError) {
	var in struct {
		Token *string `json:"token"`
		IP    string  `json:"ip"`
	}
	if err := json.Unmarshal(body, &in); err != nil || in.Token == nil {
		return nil, encErr(400, "bad_request", "expected a JSON object with a \"token\" string")
	}
	acct, err := e.auth.Authenticate(*in.Token, in.IP)
	if err != nil {
		var ae *auth.Error
		if !errors.As(err, &ae) {
			return nil, encErr(503, "storage", "the account service could not read its database").detailed(err.Error())
		}
		out := encErr(ae.Status, ae.Code, ae.Message).detailed(ae.Detail)
		if ae.Code == "banned" {
			out.extra = map[string]any{"banned": true, "xuid": ae.Xuid}
		}
		return nil, out
	}
	return map[string]any{"xuid": acct.Xuid, "gamertag": acct.Gamertag}, nil
}

func (e *EncoreID) refused(method, route string, err *encoreError) {
	ok, skipped := e.rate.Allow([3]any{"refused", err.status, err.code + route}, encoreRefusalSecs)
	if !ok {
		return
	}
	line := fmt.Sprintf("refused %d %s: %s %s", err.status, err.code, method, cut(route, 80))
	if err.detail != "" {
		line += ": " + err.detail
	}
	if skipped > 0 {
		line += fmt.Sprintf(" (and %d more in the last %.0f s)", skipped, encoreRefusalSecs)
	}
	if err.status == 401 || err.status == 403 || err.status == 429 || err.status >= 500 {
		encoreLog.Warn(line)
	} else {
		encoreLog.Info(line)
	}
}

// serveEncore answers /encore/*; the API is off (404) until the service secret is configured.
func (c *Core) serveEncore(rw respWriter, req *http.Request, body []byte, proxied bool) {
	if c.Encore == nil {
		out, _ := json.Marshal(map[string]string{"error": "disabled", "message": "the Encore ID API is not enabled on this server"})
		rw.send(404, "application/json", out, [2]string{"Cache-Control", "no-store"})
		return
	}
	status, out := c.Encore.Handle(req.Method, req.RequestURI, req.Header, body, peerIP(req), proxied)
	rw.send(status, "application/json", out, [2]string{"Cache-Control", "no-store"})
}
