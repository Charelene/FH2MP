package lsp

import (
	"errors"
	"fmt"
	"net"
	"os"
	"time"

	"fh2mp/internal/diag"
	"fh2mp/internal/mt"
	"fh2mp/internal/pyfmt"
)

// StartChat starts the MTProto chat server (native/internal/mt) next to the HTTP API when FH2MP_MT=on (default off). FH2MP_MT_ADDR is its TCP
// address (default 127.0.0.1:5070; the public port is opened by the ops step), FH2MP_MT_RSA_KEY the server's 2048-bit RSA key (PEM; clients pin
// its fingerprint, so each realm keeps its own and it never changes casually). The realm bound into chat sign-ins is FH2MP_ENV.
func (c *Core) StartChat() error {
	switch v := envOr("FH2MP_MT", "off"); v {
	case "off":
		return nil
	case "on":
	default:
		return fmt.Errorf("FH2MP_MT=%s: expected on or off", pyfmt.ReprStr(v))
	}
	if c.Auth == nil {
		return errors.New("FH2MP_MT=on needs the identity service (SetupAuth)")
	}
	keyPath := os.Getenv("FH2MP_MT_RSA_KEY")
	if keyPath == "" {
		return errors.New("FH2MP_MT=on needs FH2MP_MT_RSA_KEY, the chat server's RSA key file")
	}
	key, err := mt.LoadServerKey(keyPath)
	if err != nil {
		return fmt.Errorf("FH2MP_MT_RSA_KEY %s: %w", keyPath, err)
	}
	cfg := mt.DefaultConfig()
	cfg.Realm = c.Auth.Env
	store := &mt.DBStore{DB: c.DB}
	srv, err := mt.New(cfg, key, mt.AuthAccounts{Svc: c.Auth}, store)
	if err != nil {
		return err
	}
	addr := envOr("FH2MP_MT_ADDR", "127.0.0.1:5070")
	l, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("chat server: listen on %s: %w", addr, err)
	}
	c.Chat = srv
	go func() {
		if err := srv.Serve(l); err != nil {
			c.LogAt(nil, diag.Error, "", fmt.Sprintf("chat server stopped: accept on %s failed: %v; chat is down until the LSP restarts", addr, err))
		}
	}()
	go func() {
		for range time.Tick(time.Hour) {
			if n, err := store.Expire(); err != nil {
				c.LogAt(nil, diag.Warn, "", fmt.Sprintf("chat: dropping stored updates older than %s failed: %v", mt.UpdateTTL, err))
			} else if n > 0 {
				c.Log(nil, fmt.Sprintf("chat: %d stored updates older than %s dropped (their clients did not come back)", n, mt.UpdateTTL))
			}
		}
	}()
	return nil
}
