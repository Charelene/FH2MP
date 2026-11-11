package mt

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"sync"
	"time"

	"github.com/gotd/td/bin"

	"fh2mp/internal/auth"
	"fh2mp/internal/diag"
	"fh2mp/internal/mt/tl"
)

// Layer is the schema layer of proto/fh2mp.tl this server speaks; MinLayer the oldest client layer it still serves.
const (
	Layer    = 1
	MinLayer = 1
	// Updates per getDifference answer at most.
	maxDifference = 100
)

// Accounts verifies sign-ins against the account keys (auth.Service in the LSP, a fake in tests).
type Accounts interface {
	// Challenge makes a single-use sign-in challenge for the address; ttl in seconds.
	Challenge(ip string) (raw []byte, ttl float64, err error)
	// SignIn checks signature over SignMessage(realm, "mtproto", challenge, pubkey, fields...) and returns the account. Errors are *auth.Error.
	SignIn(ip string, challenge, pubkey, signature []byte, fields ...[]byte) (xuid int64, gamertag string, err error)
}

// UpdateStore keeps each account's stored updates: one TL-encoded Update per pts, pts counting up from 1.
type UpdateStore interface {
	State(xuid int64) (pts int, err error)
	// Append stores one update and returns its pts.
	Append(xuid int64, body []byte) (pts int, err error)
	// Since returns the stored updates with pts > after, oldest first, at most limit; first is the pts of the first one returned.
	Since(xuid int64, after, limit int) (bodies [][]byte, first int, err error)
	// Ack: the client has every update up to pts; the store may drop them.
	Ack(xuid int64, pts int) error
}

func (s *Server) registerAPI() {
	d := s.disp
	d.OnHelpInit(func(ctx context.Context, r *tl.HelpInitRequest) (*tl.HelpConfig, error) {
		c := callOf(ctx)
		if r.Layer < MinLayer {
			return nil, rpcErr(400, "LAYER_INVALID", fmt.Sprintf("client layer %d, at least %d served", r.Layer, MinLayer))
		}
		layer := r.Layer
		if layer > Layer {
			layer = Layer
		}
		c.ss.mu.Lock()
		c.ss.layer, c.ss.appVersion, c.ss.platform = layer, clip(r.AppVersion, 40), clip(r.Platform, 40)
		c.ss.mu.Unlock()
		return &tl.HelpConfig{Layer: Layer, Date: int(s.cfg.Now().Unix()), MaxRequestBytes: s.cfg.MaxFrame - 1024,
			KeyTTL: int(time.Until(c.k.expires).Seconds())}, nil
	})
	d.OnAuthGetChallenge(func(ctx context.Context) (*tl.AuthChallenge, error) {
		c := callOf(ctx)
		raw, ttl, err := s.accounts.Challenge(c.ip)
		if err != nil {
			return nil, authRPC(err)
		}
		return &tl.AuthChallenge{Challenge: raw, Realm: s.cfg.Realm, ExpiresIn: int(ttl)}, nil
	})
	d.OnAuthSignIn(func(ctx context.Context, r *tl.AuthSignInRequest) (*tl.AuthAuthorization, error) {
		c := callOf(ctx)
		if bound := c.xuid(); bound != 0 {
			return nil, rpcErr(400, "ALREADY_SIGNED_IN", fmt.Sprintf("the key is signed in as %#x; auth.logOut first", bound))
		}
		var dev [8]byte
		binary.LittleEndian.PutUint64(dev[:], uint64(r.DeviceID))
		xuid, tag, err := s.accounts.SignIn(c.ip, r.Challenge, r.Pubkey, r.Signature, c.k.ak.ID[:], dev[:])
		if err != nil {
			re := authRPC(err)
			var ae *auth.Error
			if errors.As(err, &ae) {
				log.Info("mt.sign_in_refused " + diag.KV(diag.KVP{K: "ip", V: c.ip}, diag.KVP{K: "key", V: c.k.idHex()}, diag.KVP{K: "error", V: re.Message},
					diag.KVP{K: "xuid", V: xuidOrNil(ae.Xuid)}) + ": " + firstNonEmpty(ae.Detail, ae.Message))
			}
			return nil, re
		}
		s.bind(c.k, xuid, tag, r.DeviceID)
		c.ss.mu.Lock()
		app, platform := c.ss.appVersion, c.ss.platform
		c.ss.mu.Unlock()
		log.Info("mt.signed_in " + diag.KV(diag.KVP{K: "xuid", V: fmt.Sprintf("%#x", xuid)}, diag.KVP{K: "ip", V: c.ip}, diag.KVP{K: "key", V: c.k.idHex()},
			diag.KVP{K: "device", V: fmt.Sprintf("%016x", uint64(r.DeviceID))}, diag.KVP{K: "app", V: app}, diag.KVP{K: "platform", V: platform}))
		return &tl.AuthAuthorization{User: tl.User{ID: xuid, Gamertag: tag}}, nil
	})
	d.OnAuthLogOut(func(ctx context.Context) (bool, error) {
		c := callOf(ctx)
		if x := s.unbind(c.k); x != 0 {
			log.Info("mt.logged_out " + diag.KV(diag.KVP{K: "xuid", V: fmt.Sprintf("%#x", x)}, diag.KVP{K: "key", V: c.k.idHex()}) + ": the client signed out")
		}
		return true, nil
	})
	d.OnUpdatesGetState(func(ctx context.Context) (*tl.UpdatesState, error) {
		c := callOf(ctx)
		pts, err := s.store.State(c.xuid())
		if err != nil {
			return nil, s.internal(c, "updates.getState", err)
		}
		return &tl.UpdatesState{Pts: pts, Date: int(s.cfg.Now().Unix())}, nil
	})
	d.OnUpdatesGetDifference(func(ctx context.Context, r *tl.UpdatesGetDifferenceRequest) (tl.UpdatesDifferenceClass, error) {
		c := callOf(ctx)
		x := c.xuid()
		cur, err := s.store.State(x)
		if err != nil {
			return nil, s.internal(c, "updates.getDifference", err)
		}
		now := int(s.cfg.Now().Unix())
		switch {
		case r.Pts < 0:
			return nil, rpcErr(400, "PTS_INVALID", fmt.Sprintf("pts %d", r.Pts))
		case r.Pts > cur:
			// The client is ahead of the server (its pts came from a database that was restored since): it starts again from ours.
			log.Warn("mt.pts_ahead " + diag.KV(diag.KVP{K: "xuid", V: fmt.Sprintf("%#x", x)}, diag.KVP{K: "client_pts", V: r.Pts}, diag.KVP{K: "server_pts", V: cur}) +
				": answered differenceTooLong")
			return &tl.UpdatesDifferenceTooLong{Pts: cur}, nil
		case r.Pts == cur:
			_ = s.store.Ack(x, cur)
			return &tl.UpdatesDifferenceEmpty{Pts: cur, Date: now}, nil
		}
		limit := r.Limit
		if limit <= 0 || limit > maxDifference {
			limit = maxDifference
		}
		bodies, first, err := s.store.Since(x, r.Pts, limit)
		if err != nil {
			return nil, s.internal(c, "updates.getDifference", err)
		}
		if len(bodies) == 0 || first != r.Pts+1 {
			// Updates the client never got were dropped (older than the store keeps): it reloads its state instead.
			log.Info("mt.difference_too_long " + diag.KV(diag.KVP{K: "xuid", V: fmt.Sprintf("%#x", x)}, diag.KVP{K: "client_pts", V: r.Pts},
				diag.KVP{K: "oldest_kept", V: first}, diag.KVP{K: "server_pts", V: cur}))
			return &tl.UpdatesDifferenceTooLong{Pts: cur}, nil
		}
		_ = s.store.Ack(x, r.Pts)
		upds := make([]tl.UpdateClass, 0, len(bodies))
		for i, b := range bodies {
			u, err := tl.DecodeUpdate(&bin.Buffer{Buf: b})
			if err != nil {
				return nil, s.internal(c, "updates.getDifference", fmt.Errorf("stored update pts %d does not decode: %w", first+i, err))
			}
			upds = append(upds, u)
		}
		last := first + len(bodies) - 1
		if last < cur {
			return &tl.UpdatesDifferenceSlice{NewUpdates: upds, IntermediateState: tl.UpdatesState{Pts: last, Date: now}}, nil
		}
		return &tl.UpdatesDifference{NewUpdates: upds, State: tl.UpdatesState{Pts: cur, Date: now}}, nil
	})
}

// internal logs a failure the client cannot fix and answers 500.
func (s *Server) internal(c *Call, method string, err error) *RPCError {
	log.Exc("mt.call_failed "+diag.KV(diag.KVP{K: "method", V: method}, diag.KVP{K: "xuid", V: c.xuidHex()}), err)
	return &RPCError{Code: 500, Message: "INTERNAL"}
}

// authRPC maps an auth.Error of a challenge or sign-in to an rpc_error.
func authRPC(err error) *RPCError {
	var ae *auth.Error
	if !errors.As(err, &ae) {
		return &RPCError{Code: 500, Message: "INTERNAL", detail: err.Error()}
	}
	m := map[string]*RPCError{
		"rate_limited":      {Code: 420, Message: "FLOOD_WAIT_60"},
		"bad_challenge":     {Code: 400, Message: "CHALLENGE_INVALID"},
		"challenge_expired": {Code: 400, Message: "CHALLENGE_EXPIRED"},
		"challenge_used":    {Code: 400, Message: "CHALLENGE_USED"},
		"bad_request":       {Code: 400, Message: "INPUT_INVALID"},
		"bad_signature":     {Code: 401, Message: "SIGNATURE_INVALID"},
		"unknown_account":   {Code: 400, Message: "ACCOUNT_UNKNOWN"},
		"banned":            {Code: 403, Message: "ACCOUNT_BANNED"},
	}
	if r := m[ae.Code]; r != nil {
		return &RPCError{Code: r.Code, Message: r.Message, detail: firstNonEmpty(ae.Detail, ae.Message)}
	}
	return &RPCError{Code: 500, Message: "INTERNAL", detail: fmt.Sprintf("auth error %s: %s", ae.Code, firstNonEmpty(ae.Detail, ae.Message))}
}

func clip(s string, n int) string {
	if len(s) > n {
		return s[:n]
	}
	return s
}

func firstNonEmpty(a ...string) string {
	for _, s := range a {
		if s != "" {
			return s
		}
	}
	return ""
}

func xuidOrNil(x int64) any {
	if x == 0 {
		return nil
	}
	return fmt.Sprintf("%#x", x)
}

// MemStore is an UpdateStore in memory (tests, and the server with FH2MP_MT_STORE=memory).
type MemStore struct {
	mu   sync.Mutex
	accs map[int64]*memAcc
}

type memAcc struct {
	pts   int
	first int // pts of rows[0]
	rows  [][]byte
}

// NewMemStore makes an empty store.
func NewMemStore() *MemStore { return &MemStore{accs: map[int64]*memAcc{}} }

func (m *MemStore) acc(x int64) *memAcc {
	a := m.accs[x]
	if a == nil {
		a = &memAcc{first: 1}
		m.accs[x] = a
	}
	return a
}

func (m *MemStore) State(x int64) (int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.acc(x).pts, nil
}

func (m *MemStore) Append(x int64, body []byte) (int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	a := m.acc(x)
	a.pts++
	if len(a.rows) == 0 {
		a.first = a.pts
	}
	a.rows = append(a.rows, append([]byte(nil), body...))
	return a.pts, nil
}

func (m *MemStore) Since(x int64, after, limit int) ([][]byte, int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	a := m.acc(x)
	if len(a.rows) == 0 {
		return nil, a.pts + 1, nil
	}
	i := after + 1 - a.first
	first := after + 1
	if i < 0 {
		i, first = 0, a.first
	}
	if i >= len(a.rows) {
		return nil, a.pts + 1, nil
	}
	j := i + limit
	if j > len(a.rows) {
		j = len(a.rows)
	}
	return append([][]byte(nil), a.rows[i:j]...), first, nil
}

func (m *MemStore) Ack(x int64, pts int) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	a := m.acc(x)
	n := pts + 1 - a.first
	if n <= 0 {
		return nil
	}
	if n > len(a.rows) {
		n = len(a.rows)
	}
	a.rows = a.rows[n:]
	a.first += n
	return nil
}
