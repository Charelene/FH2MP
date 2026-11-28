package lsp

import (
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"fh2mp/internal/auth"
)

const encTestSecret = "0123456789abcdef0123456789abcdef0123456789abcdef"

// fakeIdentity answers like AuthService.Authenticate for a few sessions; the refusals are the ones docs/auth.md names (a session ends after 12 h
// idle or 72 h, a ban names its reason and, when temporary, its end).
type fakeIdentity struct {
	calls []string
	ips   []string
	wrong int
}

func (f *fakeIdentity) Authenticate(token, ip string) (*auth.Account, error) {
	f.calls = append(f.calls, token)
	f.ips = append(f.ips, ip)
	switch token {
	case "fh2.valid":
		return &auth.Account{Xuid: 0x0009000000012345, Gamertag: "Driver One"}, nil
	case "fh2.expired":
		return nil, auth.NewError(401, "token_expired", "the session expired: sign in again from the launcher").WithXuid(0x0009000000012346)
	case "fh2.banned":
		until := float64(4102444800) // 2100-01-01
		return nil, auth.NewError(403, "banned", auth.BanMessage("cheating", &until)).WithXuid(0x0009000000012347)
	case "fh2.banned-forever":
		return nil, auth.NewError(403, "banned", auth.BanMessage("", nil)).WithXuid(0x0009000000012348)
	case "fh2.moved":
		return nil, auth.NewError(401, "ip_changed", "your address changed: start the game from the launcher again")
	case "fh2.dbdown":
		return nil, errors.New("connection refused")
	}
	return nil, auth.NewError(401, "invalid_token", "").WithDetail("no live session for this token")
}

func (f *fakeIdentity) Hit(key any, limit int, window float64) bool {
	f.wrong++
	return f.wrong <= limit
}

func encCall(t *testing.T, e *EncoreID, method, path, secret, body string, peer string, proxy bool) (int, map[string]any) {
	t.Helper()
	h := http.Header{}
	if secret != "" {
		h.Set("Authorization", "Bearer "+secret)
	}
	status, out := e.Handle(method, path, h, []byte(body), peer, proxy)
	var m map[string]any
	if err := json.Unmarshal(out, &m); err != nil {
		t.Fatalf("%s %s: answer %q is not a JSON object: %v", method, path, out, err)
	}
	return status, m
}

func encAuth(t *testing.T, e *EncoreID, token string) (int, map[string]any) {
	t.Helper()
	b, _ := json.Marshal(map[string]string{"token": token})
	return encCall(t, e, "POST", encoreRouteAuth, encTestSecret, string(b), "127.0.0.1", false)
}

func TestEncoreAuthenticateAnswers(t *testing.T) {
	f := &fakeIdentity{}
	e := NewEncoreID(f, []byte(encTestSecret))
	status, m := encAuth(t, e, "fh2.valid")
	if status != 200 || m["xuid"] != float64(0x0009000000012345) || m["gamertag"] != "Driver One" || len(m) != 2 {
		t.Fatalf("valid token: %d %v", status, m)
	}
	cases := []struct {
		token  string
		status int
		code   string
		msg    string // "" = not checked
	}{
		{"fh2.expired", 401, "token_expired", "the session expired: sign in again from the launcher"},
		{"fh2.nobody", 401, "invalid_token", ""},
		{"fh2.moved", 401, "ip_changed", ""},
		{"fh2.banned", 403, "banned", "This account is banned until 2100-01-01 00:00 UTC. Reason: cheating"},
		{"fh2.banned-forever", 403, "banned", "This account is banned permanently."},
		{"fh2.dbdown", 503, "storage", ""},
	}
	for _, c := range cases {
		status, m := encAuth(t, e, c.token)
		if status != c.status || m["error"] != c.code || (c.msg != "" && m["message"] != c.msg) {
			t.Errorf("%s: got %d %v, want %d %s %q", c.token, status, m, c.status, c.code, c.msg)
		}
		if _, ok := m["xuid"]; ok != (c.code == "banned") {
			t.Errorf("%s: xuid in the answer = %v; only a ban names the account: %v", c.token, ok, m)
		}
		if c.code == "banned" && m["banned"] != true {
			t.Errorf("%s: a ban answers banned:true, got %v", c.token, m)
		}
		if c.code == "storage" && strings.Contains(m["message"].(string), "connection refused") {
			t.Errorf("the database's own error leaks to the caller: %v", m)
		}
	}
	if m["banned"] != nil {
		t.Errorf("a refusal other than a ban says banned: %v", m)
	}
}

func TestEncoreIPIsPassedOn(t *testing.T) {
	f := &fakeIdentity{}
	e := NewEncoreID(f, []byte(encTestSecret))
	if status, _ := encCall(t, e, "POST", encoreRouteAuth, encTestSecret, `{"token":"fh2.valid","ip":"203.0.113.7"}`, "127.0.0.1", false); status != 200 {
		t.Fatal(status)
	}
	encAuth(t, e, "fh2.valid")
	if len(f.ips) != 2 || f.ips[0] != "203.0.113.7" || f.ips[1] != "" {
		t.Fatalf("addresses given to the identity service: %q", f.ips)
	}
}

func TestEncoreRefusesTheServiceBeforeTheToken(t *testing.T) {
	f := &fakeIdentity{}
	e := NewEncoreID(f, []byte(encTestSecret))
	body := `{"token":"fh2.valid"}`
	for _, c := range []struct {
		name, secret string
		peer         string
		proxied      bool
		status       int
		code         string
	}{
		{"no secret", "", "127.0.0.1", false, 401, "service_unauthorized"},
		{"wrong secret", "x" + encTestSecret[1:], "127.0.0.1", false, 401, "service_unauthorized"},
		{"secret with a tail", encTestSecret + "0", "127.0.0.1", false, 401, "service_unauthorized"},
		{"secret cut short", encTestSecret[:40], "127.0.0.1", false, 401, "service_unauthorized"},
		{"other host", encTestSecret, "203.0.113.9", false, 403, "forbidden"},
		{"private network", encTestSecret, "10.0.0.5", false, 403, "forbidden"},
		{"through the public proxy", encTestSecret, "127.0.0.1", true, 403, "forbidden"},
	} {
		status, m := encCall(t, e, "POST", encoreRouteAuth, c.secret, body, c.peer, c.proxied)
		if status != c.status || m["error"] != c.code {
			t.Errorf("%s: got %d %v, want %d %s", c.name, status, m, c.status, c.code)
		}
	}
	if len(f.calls) != 0 {
		t.Fatalf("a refused service call reached the identity service: %v", f.calls)
	}
	if status, _ := encCall(t, e, "POST", encoreRouteAuth, encTestSecret, body, "::1", false); status != 200 {
		t.Fatalf("IPv6 loopback is refused: %d", status)
	}
}

func TestEncoreWrongSecretsAreRateLimitedButTheRightOneStillWorks(t *testing.T) {
	f := &fakeIdentity{}
	e := NewEncoreID(f, []byte(encTestSecret))
	var last int
	for i := 0; i < 12; i++ {
		last, _ = encCall(t, e, "POST", encoreRouteAuth, "guess", `{"token":"fh2.valid"}`, "127.0.0.1", false)
	}
	if last != 429 {
		t.Fatalf("the 12th wrong secret answered %d, want 429", last)
	}
	if status, _ := encAuth(t, e, "fh2.valid"); status != 200 {
		t.Fatalf("the right secret was refused after wrong ones: %d", status)
	}
}

func TestEncoreRoutesAndBodies(t *testing.T) {
	e := NewEncoreID(&fakeIdentity{}, []byte(encTestSecret))
	call := func(method, path, body string) (int, map[string]any) {
		return encCall(t, e, method, path, encTestSecret, body, "127.0.0.1", false)
	}
	if s, m := call("GET", "/encore/v1/id/ping", ""); s != 200 || m["service"] != "encore-id" || m["version"] != float64(1) {
		t.Errorf("ping: %d %v", s, m)
	}
	if s, m := call("GET", encoreRouteAuth, ""); s != 405 || m["error"] != "method_not_allowed" {
		t.Errorf("GET authenticate: %d %v", s, m)
	}
	if s, m := call("POST", "/encore/v1/id/nothing", "{}"); s != 404 || m["error"] != "not_found" {
		t.Errorf("unknown route: %d %v", s, m)
	}
	for _, body := range []string{"", "not json", "[]", `{}`, `{"token":5}`, `{"token":null}`} {
		if s, m := call("POST", encoreRouteAuth, body); s != 400 || m["error"] != "bad_request" {
			t.Errorf("body %q: %d %v", body, s, m)
		}
	}
	// an empty token is the identity service's to refuse, not a malformed call
	if s, m := call("POST", encoreRouteAuth, `{"token":""}`); s != 401 || m["error"] != "invalid_token" {
		t.Errorf("empty token: %d %v", s, m)
	}
	if s, _ := call("POST", encoreRouteAuth+"?x=1", `{"token":"fh2.valid"}`); s != 200 {
		t.Errorf("a query string breaks the route: %d", s)
	}
}

// Through the LSP's front: the body limit, a proxied request, the API off, and nothing else of the front changing.
func TestEncoreThroughTheFront(t *testing.T) {
	c := New()
	c.Out = func(string) {}
	serve := func(method, path, body string, hdr map[string]string, remote string) *httptest.ResponseRecorder {
		req := httptest.NewRequest(method, path, strings.NewReader(body))
		req.RemoteAddr = remote
		for k, v := range hdr {
			req.Header.Set(k, v)
		}
		rec := httptest.NewRecorder()
		c.ServeHTTP(rec, req)
		return rec
	}
	if rec := serve("GET", "/encore/v1/id/ping", "", nil, "127.0.0.1:50000"); rec.Code != 404 || !strings.Contains(rec.Body.String(), `"disabled"`) {
		t.Fatalf("API off: %d %s", rec.Code, rec.Body)
	}
	c.Encore = NewEncoreID(&fakeIdentity{}, []byte(encTestSecret))
	auth := map[string]string{"Authorization": "Bearer " + encTestSecret}
	rec := serve("POST", encoreRouteAuth, `{"token":"fh2.valid"}`, auth, "127.0.0.1:50000")
	if rec.Code != 200 || rec.Header().Get("Cache-Control") != "no-store" || !strings.HasPrefix(rec.Header().Get("Content-Type"), "application/json") ||
		!strings.Contains(rec.Body.String(), `"gamertag":"Driver One"`) {
		t.Fatalf("authenticate: %d %v %s", rec.Code, rec.Header(), rec.Body)
	}
	if rec := serve("POST", encoreRouteAuth, `{"token":"fh2.valid"}`, auth, "203.0.113.9:50000"); rec.Code != 403 {
		t.Fatalf("a remote peer: %d %s", rec.Code, rec.Body)
	}
	proxied := map[string]string{"Authorization": auth["Authorization"], "X-Real-IP": "203.0.113.9"}
	if rec := serve("POST", encoreRouteAuth, `{"token":"fh2.valid"}`, proxied, "127.0.0.1:50000"); rec.Code != 403 {
		t.Fatalf("a proxied request: %d %s", rec.Code, rec.Body)
	}
	if rec := serve("POST", encoreRouteAuth, `{"token":"`+strings.Repeat("a", 5000)+`"}`, auth, "127.0.0.1:50000"); rec.Code != 413 {
		t.Fatalf("a 5 KB body: %d", rec.Code)
	}
}

func TestEncoreSecretFile(t *testing.T) {
	dir := t.TempDir()
	write := func(name, content string) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte(content), 0o600); err != nil {
			t.Fatal(err)
		}
		return p
	}
	t.Setenv(encoreSecretEnv, "")
	if e, err := EncoreIDFromEnv(&fakeIdentity{}); e != nil || err != nil {
		t.Fatalf("unset: %v %v", e, err)
	}
	good := write("good", "  "+encTestSecret+"\n")
	t.Setenv(encoreSecretEnv, good)
	e, err := EncoreIDFromEnv(&fakeIdentity{})
	if err != nil || e == nil || string(e.secret) != encTestSecret {
		t.Fatalf("absolute path: %v %v", e, err)
	}
	t.Setenv("CREDENTIALS_DIRECTORY", dir)
	t.Setenv(encoreSecretEnv, "good")
	if e, err = EncoreIDFromEnv(&fakeIdentity{}); err != nil || e == nil || string(e.secret) != encTestSecret {
		t.Fatalf("credential name: %v %v", e, err)
	}
	write("short", "tooshort\n")
	for _, name := range []string{"short", "missing", filepath.Join(dir, "nope")} {
		t.Setenv(encoreSecretEnv, name)
		if e, err := EncoreIDFromEnv(&fakeIdentity{}); err == nil || e != nil {
			t.Errorf("%s: want an error, got %v %v", name, e, err)
		} else if strings.Contains(err.Error(), "tooshort") {
			t.Errorf("the error shows the secret: %v", err)
		}
	}
	t.Setenv("CREDENTIALS_DIRECTORY", "")
	t.Setenv(encoreSecretEnv, "good")
	if _, err := EncoreIDFromEnv(&fakeIdentity{}); err == nil {
		t.Error("a credential name without $CREDENTIALS_DIRECTORY must stop the start")
	}
}
