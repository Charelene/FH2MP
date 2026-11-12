package mt

import (
	"crypto/ed25519"
	"encoding/base64"
	"net"
	"testing"
	"time"

	"github.com/gotd/td/bin"

	"fh2mp/internal/auth"
	"fh2mp/internal/db"
	"fh2mp/internal/dbtest"
	"fh2mp/internal/mt/tl"
)

// The chat server on the real identity service and database: an account registered the launcher's way signs the chat in with the same key,
// gets a stored notice pushed, and a ban ends the chat session.
func TestEndToEndOnAuthService(t *testing.T) {
	t.Setenv("FH2MP_AUTH_SECRET", "test-secret")
	t.Setenv("FH2MP_POW_BITS", "6,6")
	d, err := db.Open(dbtest.New(t, ""))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(d.Close)
	svc, err := auth.NewService(d, "dev", "required", "open", "exact")
	if err != nil {
		t.Fatal(err)
	}
	pub, priv, _ := ed25519.GenerateKey(nil)
	chal, bits, _, err := svc.Challenge("127.0.0.1")
	if err != nil {
		t.Fatal(err)
	}
	raw, _ := base64.RawURLEncoding.DecodeString(chal)
	var pow uint64
	for !auth.PowOK(raw, pub, pow, bits) {
		pow++
	}
	acct, err := svc.Register("127.0.0.1", chal, auth.B64u(pub), "Chat Tester", int64(pow),
		auth.B64u(ed25519.Sign(priv, auth.SignMessage("dev", "register", raw, pub, []byte("Chat Tester")))), "")
	if err != nil {
		t.Fatal(err)
	}

	store := &DBStore{DB: d}
	s, err := New(Config{Realm: "dev", HandshakeWait: 20 * time.Second}, serverKeyForTest(t), AuthAccounts{Svc: svc}, store)
	if err != nil {
		t.Fatal(err)
	}
	svc.OnRevoke = func(x int64) { s.EndAccount(x, "signed out by the server") }
	l, _ := net.Listen("tcp", "127.0.0.1:0")
	go s.Serve(l)
	t.Cleanup(func() { s.Close() })

	c := dialRaw(t, l.Addr().String(), true).mustKey().init()
	a, e := c.signIn(priv, 42)
	if e != nil || a.User.ID != acct.Xuid || a.User.Gamertag != "Chat Tester" {
		t.Fatalf("chat sign-in: %+v %+v", a, e)
	}
	if err := s.Notify(acct.Xuid, "welcome"); err != nil {
		t.Fatal(err)
	}
	var u tl.Updates
	if err := u.Decode(&bin.Buffer{Buf: c.next(tl.UpdatesTypeID).body}); err != nil || u.Pts != 1 {
		t.Fatalf("push %+v %v", u, err)
	}
	var d1 tl.UpdatesDifferenceBox
	c.call(&tl.UpdatesGetDifferenceRequest{Pts: 0, Limit: 10}, &d1)
	if diff, ok := d1.Difference.(*tl.UpdatesDifference); !ok || len(diff.NewUpdates) != 1 || diff.State.Pts != 1 {
		t.Fatalf("difference from 0: %#v", d1.Difference)
	}
	reason := "test ban"
	if _, err := svc.SetBanned(acct.Xuid, &reason, nil); err != nil {
		t.Fatal(err)
	}
	var us tl.UpdateShort
	us.Decode(&bin.Buffer{Buf: c.next(tl.UpdateShortTypeID).body})
	if lo, ok := us.Update.(*tl.UpdateLoggedOut); !ok || lo.Reason != "signed out by the server" {
		t.Fatalf("after the ban: %#v", us.Update)
	}
	// Signing in again while banned is refused.
	c2 := dialRaw(t, l.Addr().String(), false).mustKey().init()
	if _, e := c2.signIn(priv, 42); e == nil || e.ErrorMessage != "ACCOUNT_BANNED" {
		t.Fatalf("banned sign-in: %+v", e)
	}
}
