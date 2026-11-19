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

// StoredUpdate is one stored update with its pts.
type StoredUpdate struct {
	Pts  int
	Body []byte
}

// UpdateStore keeps each account's stored updates: one TL-encoded Update per pts, pts counting up from 1. The updates of secret chats
// (AppendChat: updateEncryption, updateNewEncryptedMessage) outlive Ack, so a client that lost what it was pushed can fetch them again:
// they go when messages.ackEncrypted covers them (AckChat), when their chat is discarded (sealed messages only, DropChat), or after UpdateTTL.
type UpdateStore interface {
	State(xuid int64) (pts int, err error)
	// Append stores one update and returns its pts.
	Append(xuid int64, body []byte) (pts int, err error)
	// AppendChat stores an update of secret chat `chat`; message: an updateNewEncryptedMessage (a sealed message's bytes).
	AppendChat(xuid int64, body []byte, chat int32, message bool) (pts int, err error)
	// Since returns the stored updates with pts > after, oldest first, at most limit, and gone: the highest pts of a row dropped by Ack (a
	// row of no secret chat) or by age. A client asking from below gone cannot be given everything it lacks.
	Since(xuid int64, after, limit int) (rows []StoredUpdate, gone int, err error)
	// Ack: the client has every update up to pts; the store drops them, except the rows of secret chats.
	Ack(xuid int64, pts int) error
	// AckChat: the client keeps every secret-chat update up to pts on its PC (messages.ackEncrypted); the store drops them.
	AckChat(xuid int64, pts int) (dropped int64, err error)
	// DropChat drops the sealed messages of a secret chat that still wait for their recipient (a discard).
	DropChat(chat int32) (dropped int64, err error)
	// Expire drops rows older than UpdateTTL.
	Expire() (dropped int64, err error)
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
				re.detail = "" // logged here, not again as a refused call
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
		rows, gone, err := s.store.Since(x, r.Pts, limit)
		if err != nil {
			return nil, s.internal(c, "updates.getDifference", err)
		}
		if r.Pts < gone {
			// Updates after the client's pts were dropped (it acked them before, or they were older than the store keeps): it reloads its
			// state instead.
			log.Info("mt.difference_too_long " + diag.KV(diag.KVP{K: "xuid", V: fmt.Sprintf("%#x", x)}, diag.KVP{K: "client_pts", V: r.Pts},
				diag.KVP{K: "dropped_up_to", V: gone}, diag.KVP{K: "server_pts", V: cur}))
			return &tl.UpdatesDifferenceTooLong{Pts: cur}, nil
		}
		_ = s.store.Ack(x, r.Pts)
		if len(rows) == 0 {
			// Every row after the client's pts was a secret chat's that went (acked with messages.ackEncrypted, or its chat discarded).
			return &tl.UpdatesDifferenceEmpty{Pts: cur, Date: now}, nil
		}
		upds := make([]tl.UpdateClass, 0, len(rows))
		for _, row := range rows {
			u, err := tl.DecodeUpdate(&bin.Buffer{Buf: row.Body})
			if err != nil {
				return nil, s.internal(c, "updates.getDifference", fmt.Errorf("stored update pts %d does not decode: %w", row.Pts, err))
			}
			upds = append(upds, u)
		}
		last := rows[len(rows)-1].Pts
		if len(rows) == limit && last < cur {
			return &tl.UpdatesDifferenceSlice{NewUpdates: upds, IntermediateState: tl.UpdatesState{Pts: last, Date: now}}, nil
		}
		return &tl.UpdatesDifference{NewUpdates: upds, State: tl.UpdatesState{Pts: max(cur, last), Date: now}}, nil
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

// MemStore is an UpdateStore in memory (tests, and the test server).
type MemStore struct {
	mu   sync.Mutex
	accs map[int64]*memAcc
	Now  func() time.Time
}

type memRow struct {
	pts     int
	body    []byte
	chat    int32 // 0: no secret chat's
	message bool
	created time.Time
}

type memAcc struct {
	pts  int
	gone int
	rows []memRow // pts order
}

// NewMemStore makes an empty store.
func NewMemStore() *MemStore { return &MemStore{accs: map[int64]*memAcc{}, Now: time.Now} }

func (m *MemStore) acc(x int64) *memAcc {
	a := m.accs[x]
	if a == nil {
		a = &memAcc{}
		m.accs[x] = a
	}
	return a
}

func (m *MemStore) State(x int64) (int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.acc(x).pts, nil
}

func (m *MemStore) Append(x int64, body []byte) (int, error) { return m.AppendChat(x, body, 0, false) }

func (m *MemStore) AppendChat(x int64, body []byte, chat int32, message bool) (int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	a := m.acc(x)
	a.pts++
	a.rows = append(a.rows, memRow{pts: a.pts, body: append([]byte(nil), body...), chat: chat, message: message && chat != 0, created: m.Now()})
	return a.pts, nil
}

func (m *MemStore) Since(x int64, after, limit int) ([]StoredUpdate, int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	a := m.acc(x)
	var out []StoredUpdate
	for _, r := range a.rows {
		if r.pts > after && len(out) < limit {
			out = append(out, StoredUpdate{Pts: r.pts, Body: append([]byte(nil), r.body...)})
		}
	}
	return out, a.gone, nil
}

// keep rebuilds the account's rows with only those keep says yes to.
func (a *memAcc) keep(keep func(r memRow) bool) int64 {
	var n int64
	rows := a.rows[:0]
	for _, r := range a.rows {
		if keep(r) {
			rows = append(rows, r)
		} else {
			n++
		}
	}
	a.rows = rows
	return n
}

func (m *MemStore) Ack(x int64, pts int) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	a := m.acc(x)
	a.keep(func(r memRow) bool {
		if r.pts <= pts && r.chat == 0 {
			a.gone = max(a.gone, r.pts)
			return false
		}
		return true
	})
	return nil
}

func (m *MemStore) AckChat(x int64, pts int) (int64, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.acc(x).keep(func(r memRow) bool { return r.pts > pts || r.chat == 0 }), nil
}

func (m *MemStore) DropChat(chat int32) (int64, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	var n int64
	for _, a := range m.accs {
		n += a.keep(func(r memRow) bool { return r.chat != chat || !r.message })
	}
	return n, nil
}

func (m *MemStore) Expire() (int64, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	cut := m.Now().Add(-UpdateTTL)
	var n int64
	for _, a := range m.accs {
		n += a.keep(func(r memRow) bool {
			if r.created.Before(cut) {
				a.gone = max(a.gone, r.pts)
				return false
			}
			return true
		})
	}
	return n, nil
}
