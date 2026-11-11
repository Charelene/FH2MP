// Command fh2mp-mt-testserver runs the chat server (internal/mt) with in-memory accounts and updates, for the Rust client's interop tests.
// It never touches a database or a real account.
//
//	fh2mp-mt-testserver -account <ed25519 pubkey hex>=<xuid hex>:<gamertag> ...
//	fh2mp-mt-testserver -demo -addr 127.0.0.1:5170 -pubout key.pem   (made-up friends and club to try the chat window; see demo.go)
//
// stdout: "listening <host:port>", then "pubkey <PKCS#1 public key, base64 DER>". stdin, one command per line:
// "notify <xuid hex> <text>" (stores and pushes a notice) and "end <xuid hex> <reason>" (ends the account's session).
package main

import (
	"bufio"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/base64"
	"encoding/hex"
	"encoding/pem"
	"flag"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"

	"fh2mp/internal/auth"
	"fh2mp/internal/diag"
	"fh2mp/internal/mt"
)

type accounts struct {
	mu    sync.Mutex
	chals map[string]bool
	byKey map[string][2]string // pubkey -> xuid hex, gamertag
}

func (a *accounts) Challenge(ip string) ([]byte, float64, error) {
	b := make([]byte, 41)
	rand.Read(b)
	a.mu.Lock()
	a.chals[string(b)] = true
	a.mu.Unlock()
	return b, 120, nil
}

func (a *accounts) SignIn(ip string, chal, pub, sig []byte, fields ...[]byte) (int64, string, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.chals[string(chal)] {
		return 0, "", auth.NewError(400, "challenge_used", "that challenge was already used")
	}
	delete(a.chals, string(chal))
	if len(pub) != 32 || !ed25519.Verify(pub, auth.SignMessage("test", mt.Purpose, chal, append([][]byte{pub}, fields...)...), sig) {
		return 0, "", auth.NewError(401, "bad_signature", "")
	}
	acc, ok := a.byKey[string(pub)]
	if !ok {
		return 0, "", auth.NewError(404, "unknown_account", "")
	}
	x, _ := strconv.ParseInt(acc[0], 16, 64)
	return x, acc[1], nil
}

type multi []string

func (m *multi) String() string     { return strings.Join(*m, ",") }
func (m *multi) Set(v string) error { *m = append(*m, v); return nil }

func main() {
	var accs multi
	addr := flag.String("addr", "127.0.0.1:0", "listen address")
	demoMode := flag.Bool("demo", false, "made-up friends and club, any correctly signed key is let in (to try the chat window)")
	pubout := flag.String("pubout", "", "also write the server's public key as PEM here (FH2MP_CHAT_KEY for the chat core)")
	flag.Var(&accs, "account", "<pubkey hex>=<xuid hex>:<gamertag>, repeatable")
	flag.Parse()
	// Log lines go to stderr; stdout carries the protocol with the test.
	diag.SetSink(func(level int, sub, msg string) { fmt.Fprintln(os.Stderr, diag.LevelName(level), msg) })
	a := &accounts{chals: map[string]bool{}, byKey: map[string][2]string{}}
	for _, s := range accs {
		k, rest, ok1 := strings.Cut(s, "=")
		x, tag, ok2 := strings.Cut(rest, ":")
		pub, err := hex.DecodeString(k)
		if !ok1 || !ok2 || err != nil || len(pub) != 32 {
			fmt.Fprintln(os.Stderr, "bad -account", s)
			os.Exit(2)
		}
		a.byKey[string(pub)] = [2]string{strings.TrimPrefix(x, "0x"), tag}
	}
	priv, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		panic(err)
	}
	key, err := mt.ParseServerKey(pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(priv)}))
	if err != nil {
		panic(err)
	}
	cfg := mt.DefaultConfig()
	cfg.Realm = "test"
	var accounts mt.Accounts = a
	var d *demo
	if *demoMode {
		d = newDemo(cfg.Realm)
		accounts = d
	}
	s, err := mt.New(cfg, key, accounts, mt.NewMemStore())
	if err != nil {
		panic(err)
	}
	if d != nil {
		d.s = s
		s.SetCommunity(d)
	}
	if *pubout != "" {
		if err := os.WriteFile(*pubout, pem.EncodeToMemory(&pem.Block{Type: "RSA PUBLIC KEY", Bytes: x509.MarshalPKCS1PublicKey(&priv.PublicKey)}), 0o644); err != nil {
			panic(err)
		}
	}
	l, err := net.Listen("tcp", *addr)
	if err != nil {
		panic(err)
	}
	fmt.Printf("listening %s\npubkey %s\n", l.Addr(), base64.StdEncoding.EncodeToString(x509.MarshalPKCS1PublicKey(&priv.PublicKey)))
	go func() {
		sc := bufio.NewScanner(os.Stdin)
		for sc.Scan() {
			if d != nil {
				if out, ok := d.command(sc.Text()); ok {
					fmt.Println(out)
					continue
				}
			}
			f := strings.SplitN(sc.Text(), " ", 3)
			if len(f) < 3 {
				continue
			}
			x, err := strconv.ParseInt(strings.TrimPrefix(f[1], "0x"), 16, 64)
			if err != nil {
				continue
			}
			switch f[0] {
			case "notify":
				if err := s.Notify(x, f[2]); err != nil {
					fmt.Fprintln(os.Stderr, "notify:", err)
				}
			case "end":
				s.EndAccount(x, f[2])
			}
			fmt.Println("done", f[0])
		}
		// stdin closed: the test is over (a demo keeps serving).
		if d == nil {
			s.Close()
			os.Exit(0)
		}
	}()
	if err := s.Serve(l); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
