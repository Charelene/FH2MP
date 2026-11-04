package auth

import (
	"crypto/ed25519"
	"encoding/hex"
	"encoding/json"
	"os"
	"testing"

	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
)

type vectors struct {
	Tags []struct {
		Tag     string `json:"tag"`
		Ok      string `json:"ok"`
		Error   string `json:"error"`
		Message string `json:"message"`
		Detail  string `json:"detail"`
		Fold    string `json:"fold"`
	} `json:"tags"`
	Sign      string   `json:"sign"`
	Pow       [][]bool `json:"pow"`
	TicketKey string   `json:"ticket_key"`
	Ban       []string `json:"ban"`
	Interop   struct {
		Now      float64 `json:"now"`
		Register struct {
			Challenge string `json:"challenge"`
			Pubkey    string `json:"pubkey"`
			Tag       string `json:"tag"`
			Pow       int64  `json:"pow"`
			Sig       string `json:"sig"`
			IP        string `json:"ip"`
		} `json:"register"`
		Login struct {
			Challenge string `json:"challenge"`
			Pubkey    string `json:"pubkey"`
			Sig       string `json:"sig"`
			IP        string `json:"ip"`
		} `json:"login"`
	} `json:"interop"`
}

func load(t *testing.T) vectors {
	raw, err := os.ReadFile("../../testdata/auth.json")
	if err != nil {
		t.Skip(err)
	}
	var v vectors
	if err := json.Unmarshal(raw, &v); err != nil {
		t.Fatal(err)
	}
	return v
}

func TestPureFunctionsMatchPython(t *testing.T) {
	v := load(t)
	for _, c := range v.Tags {
		got, err := ValidateGamertag(c.Tag, nil, nil)
		if c.Error == "" {
			if err != nil || got != c.Ok {
				t.Errorf("%q: got %q %v, want ok %q", c.Tag, got, err, c.Ok)
			}
		} else {
			e, ok := err.(*Error)
			if !ok || e.Code != c.Error || e.Message != c.Message || e.Detail != c.Detail {
				t.Errorf("%q: got %+v, want %s / %s / %s", c.Tag, err, c.Error, c.Message, c.Detail)
			}
		}
		if f := FoldTag(c.Tag); f != c.Fold {
			t.Errorf("fold %q = %q, want %q", c.Tag, f, c.Fold)
		}
	}
	raw := make([]byte, 41)
	pk := make([]byte, 32)
	for i := range raw {
		raw[i] = byte(i)
	}
	for i := range pk {
		pk[i] = byte(i)
	}
	if got := hex.EncodeToString(SignMessage("dev", "login", raw, pk, []byte("tag"))); got != v.Sign {
		t.Errorf("sign message %s want %s", got, v.Sign)
	}
	for c, want := range v.Pow {
		if PowOK(raw, pk, uint64(c), 4) != want[0] {
			t.Errorf("pow counter %d", c)
		}
	}
	if got := UdpTicketKey("fh2.sometoken"); got != v.TicketKey {
		t.Errorf("ticket key %s want %s", got, v.TicketKey)
	}
	f := func(x float64) *float64 { return &x }
	for i, got := range []string{BanMessage("cheating", nil), BanMessage("", f(1_800_000_000)), BanMessage("rude", f(1_800_086_400))} {
		if got != v.Ban[i] {
			t.Errorf("ban message %d: %q want %q", i, got, v.Ban[i])
		}
	}
}

func service(t *testing.T, now *float64) *Service {
	t.Setenv("FH2MP_AUTH_SECRET", "test-secret")
	t.Setenv("FH2MP_POW_BITS", "6,6")
	d, err := db.Open(dbtest.New(t, ""))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(d.Close)
	s, err := NewService(d, "dev", "optional", "open", "exact")
	if err != nil {
		t.Fatal(err)
	}
	s.Clock = func() float64 { return *now }
	return s
}

// A registration and a sign-in made by the Python service are accepted by the Go one on its own empty database.
func TestAcceptsWhatPythonMade(t *testing.T) {
	v := load(t)
	now := v.Interop.Now + 1
	s := service(t, &now)
	r := v.Interop.Register
	acct, err := s.Register(r.IP, r.Challenge, r.Pubkey, r.Tag, r.Pow, r.Sig, "")
	if err != nil {
		t.Fatalf("register: %v (%+v)", err, err)
	}
	if acct.Xuid>>48 != 9 || acct.Gamertag != r.Tag {
		t.Fatalf("account %+v", acct)
	}
	if _, err := s.Register(r.IP, r.Challenge, r.Pubkey, r.Tag, r.Pow, r.Sig, ""); err == nil || err.(*Error).Code != "challenge_used" {
		t.Fatalf("a challenge must work once: %v", err)
	}
	l := v.Interop.Login
	token, got, _, err := s.Login(l.IP, l.Challenge, l.Pubkey, l.Sig, "interop")
	if err != nil {
		t.Fatalf("login: %v", err)
	}
	if got.Xuid != acct.Xuid {
		t.Fatalf("login gave %x, register %x", got.Xuid, acct.Xuid)
	}
	a2, err := s.Authenticate(token, l.IP)
	if err != nil || a2.Xuid != acct.Xuid {
		t.Fatalf("authenticate: %v", err)
	}
	if _, err := s.Authenticate(token, "198.51.100.9"); err == nil || err.(*Error).Code != "ip_changed" {
		t.Fatalf("a token must stay on its address: %v", err)
	}
	reason := "test"
	if _, err := s.SetBanned(acct.Xuid, &reason, nil); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Authenticate(token, l.IP); err == nil || err.(*Error).Code != "banned" {
		t.Fatalf("a banned account must be refused: %v", err)
	}
}

// Go makes its own account and the pieces work together (challenge -> PoW -> signature).
func TestGoRoundTrip(t *testing.T) {
	now := 1_800_000_000.0
	s := service(t, &now)
	pub, priv, _ := ed25519.GenerateKey(nil)
	chal, bits, _, err := s.Challenge("203.0.113.8")
	if err != nil {
		t.Fatal(err)
	}
	raw, _ := unb64u(chal)
	var c uint64
	for !PowOK(raw, pub, c, bits) {
		c++
	}
	sig := ed25519.Sign(priv, SignMessage("dev", "register", raw, pub, []byte("Round Trip")))
	acct, err := s.Register("203.0.113.8", chal, B64u(pub), "Round Trip", int64(c), B64u(sig), "")
	if err != nil {
		t.Fatal(err)
	}
	chal2, _, _, _ := s.Challenge("203.0.113.8")
	raw2, _ := unb64u(chal2)
	sig2 := ed25519.Sign(priv, SignMessage("dev", "login", raw2, pub))
	_, who, _, err := s.Login("203.0.113.8", chal2, B64u(pub), B64u(sig2), "go")
	if err != nil || who.Xuid != acct.Xuid {
		t.Fatalf("login: %v", err)
	}
	if _, err := s.Rename(acct.Xuid, "Other Name", false); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Rename(acct.Xuid, "Third Name", false); err == nil || err.(*Error).Code != "rename_cooldown" {
		t.Fatalf("rename cooldown: %v", err)
	}
}
