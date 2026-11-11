// Package mt is the FH2MP chat server over MTProto 2.0 (core.telegram.org/mtproto): the transport, the auth-key exchange, the encrypted
// session layer (message ids, salts, acks, containers, result cache) and our own API (proto/fh2mp.tl, generated into mt/tl).
//
// Pieces of gotd/td (MIT, github.com/gotd/td) are used for TL encoding (bin, mt, the generated tl package), message-key derivation
// (crypto.Keys, crypto.MessageKey) and obfuscated2. The session layer follows gotd's tgtest server in outline; the exchange, the transport
// and the session rules are our own (see exchange.go for why gotd's server exchange is not used).
package mt

import (
	"context"
	"crypto/aes"
	"crypto/rand"
	"crypto/subtle"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gotd/ige"
	"github.com/gotd/td/bin"
	"github.com/gotd/td/crypto"
	"github.com/gotd/td/mt"
	"github.com/gotd/td/proto"

	"fh2mp/internal/auth"
	"fh2mp/internal/diag"
	"fh2mp/internal/mt/tl"
)

var log = diag.Get("mt")

// Config are the server's limits; zero fields take the defaults of DefaultConfig.
type Config struct {
	DC               int           // the dc id clients put in p_q_inner_data_temp_dc
	Realm            string        // prod | dev | test: named in auth.challenge, bound into the sign-in signature
	MaxFrame         int           // largest transport frame taken
	KeyMaxTTL        time.Duration // a temporary key lives at most this long (the client may ask for less)
	KeyMinTTL        time.Duration
	IdleTimeout      time.Duration // a connection without a frame for this long is closed (ping_delay_disconnect can shorten it)
	HandshakeWait    time.Duration // for the transport header and for each exchange step
	WriteWait        time.Duration
	MaxConnsPerIP    int
	HandshakesPerMin int // auth-key exchanges per minute per address
	MaxKeys          int // auth keys in memory
	MaxUnboundPerIP  int // keys of one address that have not signed in
	UnboundIdle      time.Duration
	MaxSessions      int // MTProto sessions per key
	OutQueue         int // frames queued for a slow connection before it is closed
	Now              func() time.Time
}

// DefaultConfig are the production limits.
func DefaultConfig() Config {
	return Config{DC: 1, Realm: "prod", MaxFrame: 1 << 20, KeyMaxTTL: 24 * time.Hour, KeyMinTTL: time.Minute, IdleTimeout: 120 * time.Second,
		HandshakeWait: 10 * time.Second, WriteWait: 15 * time.Second, MaxConnsPerIP: 16, HandshakesPerMin: 20, MaxKeys: 50000, MaxUnboundPerIP: 16,
		UnboundIdle: 10 * time.Minute, MaxSessions: 8, OutQueue: 256, Now: time.Now}
}

func (c Config) withDefaults() Config {
	d := DefaultConfig()
	if c.DC == 0 {
		c.DC = d.DC
	}
	if c.Realm == "" {
		c.Realm = d.Realm
	}
	if c.MaxFrame == 0 {
		c.MaxFrame = d.MaxFrame
	}
	if c.KeyMaxTTL == 0 {
		c.KeyMaxTTL = d.KeyMaxTTL
	}
	if c.KeyMinTTL == 0 {
		c.KeyMinTTL = d.KeyMinTTL
	}
	if c.IdleTimeout == 0 {
		c.IdleTimeout = d.IdleTimeout
	}
	if c.HandshakeWait == 0 {
		c.HandshakeWait = d.HandshakeWait
	}
	if c.WriteWait == 0 {
		c.WriteWait = d.WriteWait
	}
	if c.MaxConnsPerIP == 0 {
		c.MaxConnsPerIP = d.MaxConnsPerIP
	}
	if c.HandshakesPerMin == 0 {
		c.HandshakesPerMin = d.HandshakesPerMin
	}
	if c.MaxKeys == 0 {
		c.MaxKeys = d.MaxKeys
	}
	if c.MaxUnboundPerIP == 0 {
		c.MaxUnboundPerIP = d.MaxUnboundPerIP
	}
	if c.UnboundIdle == 0 {
		c.UnboundIdle = d.UnboundIdle
	}
	if c.MaxSessions == 0 {
		c.MaxSessions = d.MaxSessions
	}
	if c.OutQueue == 0 {
		c.OutQueue = d.OutQueue
	}
	if c.Now == nil {
		c.Now = d.Now
	}
	return c
}

// authKey is one temporary auth key, kept in memory only. Fields below mu are guarded by it.
type authKey struct {
	ak        crypto.AuthKey
	created   time.Time
	expires   time.Time
	ip        string
	firstSalt int64

	mu       sync.Mutex
	lastUsed time.Time
	xuid     int64
	tag      string
	device   int64
	sessions map[int64]*session
	dead     bool
}

func (k *authKey) idHex() string { return fmt.Sprintf("%x", k.ak.ID[:]) }

// clientConn is one TCP connection. Frames to it go through out, written by its own goroutine, so no lock is held across a network write.
type clientConn struct {
	id     uint64
	fc     *frameConn
	ip     string
	opened time.Time
	out    chan []byte
	done   chan struct{}
	once   sync.Once
	why    atomic.Value // string: why the server closed it
	idle   atomic.Int64 // read idle timeout in ns
	frames atomic.Int64
}

func (c *clientConn) close(why string) {
	c.once.Do(func() {
		c.why.Store(why)
		close(c.done)
		_ = c.fc.nc.Close()
	})
}

func (c *clientConn) closed() bool {
	select {
	case <-c.done:
		return true
	default:
		return false
	}
}

// enqueue hands a frame to the writer; a connection whose queue is full is closed (the client reads too slowly or not at all).
func (c *clientConn) enqueue(frame []byte) bool {
	if c.closed() {
		return false
	}
	select {
	case c.out <- frame:
		return true
	default:
		c.close(fmt.Sprintf("output queue full (%d frames): the client does not read", cap(c.out)))
		return false
	}
}

// Server is the chat server.
type Server struct {
	cfg        Config
	key        *serverKey
	accounts   Accounts
	store      UpdateStore
	disp       *tl.ServerDispatcher
	ids        msgIDGen
	saltSecret [32]byte
	limits     *auth.RateLimiter
	xcfg       exchangeConfig
	nextConn   atomic.Uint64

	mu      sync.Mutex
	ln      net.Listener
	closed  bool
	keys    map[[8]byte]*authKey
	byXuid  map[int64]*authKey
	ipConns map[string]int
	conns   map[*clientConn]struct{}
	wg      sync.WaitGroup
	stop    chan struct{}
}

// New builds a server around the RSA key; accounts verifies sign-ins and store keeps each account's update sequence.
func New(cfg Config, key *serverKey, accounts Accounts, store UpdateStore) (*Server, error) {
	if key == nil {
		return nil, errors.New("mt: no RSA key")
	}
	cfg = cfg.withDefaults()
	s := &Server{cfg: cfg, key: key, accounts: accounts, store: store, limits: auth.NewRateLimiter(), keys: map[[8]byte]*authKey{},
		byXuid: map[int64]*authKey{}, ipConns: map[string]int{}, conns: map[*clientConn]struct{}{}, stop: make(chan struct{})}
	s.ids.now = cfg.Now
	if _, err := rand.Read(s.saltSecret[:]); err != nil {
		return nil, err
	}
	s.xcfg = exchangeConfig{key: key, dc: cfg.DC, maxTTL: int(cfg.KeyMaxTTL / time.Second), minTTL: int(cfg.KeyMinTTL / time.Second),
		stepWait: cfg.HandshakeWait, now: cfg.Now, ids: &s.ids, keyExists: func(id [8]byte) bool {
			s.mu.Lock()
			defer s.mu.Unlock()
			return s.keys[id] != nil
		}}
	s.disp = tl.NewServerDispatcher(func(ctx context.Context, b *bin.Buffer) (bin.Encoder, error) {
		id, _ := b.PeekID()
		return nil, rpcErr(400, "INPUT_METHOD_INVALID", fmt.Sprintf("no method %#08x in layer %d", id, Layer))
	})
	s.registerAPI()
	if err := checkDHPrime(); err != nil {
		return nil, fmt.Errorf("mt: dh_prime: %w", err)
	}
	return s, nil
}

// Serve accepts connections until Close. The accept loop does no reads: each connection's transport header is read on its own goroutine
// under HandshakeWait, so a silent client cannot hold up the others.
func (s *Server) Serve(l net.Listener) error {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return errors.New("mt: server closed")
	}
	s.ln = l
	s.mu.Unlock()
	s.wg.Add(1)
	go s.janitor()
	log.Info("mt.listening " + diag.KV(diag.KVP{K: "addr", V: l.Addr().String()}, diag.KVP{K: "realm", V: s.cfg.Realm}, diag.KVP{K: "dc", V: s.cfg.DC},
		diag.KVP{K: "rsa_fingerprint", V: fmt.Sprintf("%016x", uint64(s.key.fp))}, diag.KVP{K: "layer", V: Layer}))
	var delay time.Duration
	for {
		nc, err := l.Accept()
		if err != nil {
			s.mu.Lock()
			closed := s.closed
			s.mu.Unlock()
			if closed {
				return nil
			}
			var ne net.Error
			if errors.As(err, &ne) && ne.Timeout() {
				if delay == 0 {
					delay = 5 * time.Millisecond
				} else if delay *= 2; delay > time.Second {
					delay = time.Second
				}
				log.Warn(fmt.Sprintf("mt.accept_failed: %v, retrying in %s", err, delay))
				time.Sleep(delay)
				continue
			}
			return err
		}
		delay = 0
		s.wg.Add(1)
		go func() {
			defer s.wg.Done()
			s.serveConn(nc)
		}()
	}
}

// Close stops the listener, closes every connection and forgets every key (clients make new ones on their next connect).
func (s *Server) Close() error {
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return nil
	}
	s.closed = true
	close(s.stop)
	ln := s.ln
	conns := make([]*clientConn, 0, len(s.conns))
	for c := range s.conns {
		conns = append(conns, c)
	}
	s.mu.Unlock()
	var err error
	if ln != nil {
		err = ln.Close()
	}
	for _, c := range conns {
		c.close("server shutting down")
	}
	s.wg.Wait()
	return err
}

func hostOf(a net.Addr) string {
	h, _, err := net.SplitHostPort(a.String())
	if err != nil {
		return a.String()
	}
	return auth.NormIP(h)
}

func (s *Server) serveConn(nc net.Conn) {
	ip := hostOf(nc.RemoteAddr())
	s.mu.Lock()
	if s.ipConns[ip] >= s.cfg.MaxConnsPerIP {
		s.mu.Unlock()
		if ok, skipped := rate.Allow([2]string{"conn-cap", ip}, 60); ok {
			log.Warn("mt.conn_refused " + diag.KV(diag.KVP{K: "ip", V: ip}, diag.KVP{K: "open", V: s.cfg.MaxConnsPerIP}, diag.KVP{K: "skipped", V: nz(skipped)}) +
				": the address has the most connections allowed open already")
		}
		_ = nc.Close()
		return
	}
	s.ipConns[ip]++
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		if s.ipConns[ip]--; s.ipConns[ip] <= 0 {
			delete(s.ipConns, ip)
		}
		s.mu.Unlock()
	}()
	now := s.cfg.Now()
	fc, err := acceptTransport(nc, s.cfg.MaxFrame, now.Add(s.cfg.HandshakeWait))
	if err != nil {
		var refused errTransportRefused
		if errors.As(err, &refused) {
			log.Info("mt.conn_refused " + diag.KV(diag.KVP{K: "ip", V: ip}) + ": " + refused.why)
		} else {
			log.Debug("mt.conn_dropped " + diag.KV(diag.KVP{K: "ip", V: ip}) + ": no transport header: " + err.Error())
		}
		_ = nc.Close()
		return
	}
	cc := &clientConn{id: s.nextConn.Add(1), fc: fc, ip: ip, opened: now, out: make(chan []byte, s.cfg.OutQueue), done: make(chan struct{})}
	cc.idle.Store(int64(s.cfg.IdleTimeout))
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		_ = nc.Close()
		return
	}
	s.conns[cc] = struct{}{}
	s.mu.Unlock()
	writerDone := make(chan struct{})
	go func() {
		defer close(writerDone)
		for {
			select {
			case f := <-cc.out:
				if err := fc.writeFrame(f, s.cfg.Now().Add(s.cfg.WriteWait)); err != nil {
					cc.close("write failed: " + err.Error())
					return
				}
			case <-cc.done:
				return
			}
		}
	}()
	var bound *authKey
	reason := s.readLoop(cc, &bound)
	cc.close(reason)
	<-writerDone
	s.mu.Lock()
	delete(s.conns, cc)
	s.mu.Unlock()
	why, _ := cc.why.Load().(string)
	fields := []diag.KVP{{K: "conn", V: cc.id}, {K: "ip", V: ip}, {K: "transport", V: fc.kind()}, {K: "frames", V: cc.frames.Load()},
		{K: "open_s", V: int(s.cfg.Now().Sub(cc.opened).Seconds())}}
	if bound != nil {
		bound.mu.Lock()
		x := bound.xuid
		bound.mu.Unlock()
		if x != 0 {
			fields = append(fields, diag.KVP{K: "xuid", V: fmt.Sprintf("%#x", x)})
			log.Info("mt.conn_closed " + diag.KV(fields...) + ": " + why)
			return
		}
	}
	log.Debug("mt.conn_closed " + diag.KV(fields...) + ": " + why)
}

// readLoop reads frames until the connection ends and returns why. *last is the key last used on it (for the close line).
func (s *Server) readLoop(cc *clientConn, last **authKey) string {
	unknown := 0
	for {
		frame, err := cc.fc.readFrame(s.cfg.Now().Add(time.Duration(cc.idle.Load())))
		if err != nil {
			if cc.closed() {
				why, _ := cc.why.Load().(string)
				return why
			}
			var ne net.Error
			switch {
			case errors.Is(err, io.EOF):
				return "the client closed the connection"
			case errors.As(err, &ne) && ne.Timeout():
				return fmt.Sprintf("no frame for %s", time.Duration(cc.idle.Load()))
			default:
				return "read failed: " + err.Error()
			}
		}
		cc.frames.Add(1)
		if len(frame) < 8 {
			return fmt.Sprintf("protocol violation: frame of %d bytes", len(frame))
		}
		var kid [8]byte
		copy(kid[:], frame)
		if kid == ([8]byte{}) {
			if why := s.exchange(cc, frame); why != "" {
				return why
			}
			continue
		}
		s.mu.Lock()
		k := s.keys[kid]
		s.mu.Unlock()
		if k == nil {
			// An unknown key: expired, made before a server restart, or ended. The client makes a new one on -404.
			unknown++
			if unknown > 3 {
				return "protocol violation: kept sending with an unknown auth key after -404"
			}
			_ = cc.fc.writeTransportError(transportKeyNotFound, s.cfg.Now().Add(s.cfg.WriteWait))
			continue
		}
		*last = k
		if why := s.handleEncrypted(cc, k, frame); why != "" {
			return why
		}
	}
}

// exchange runs a key exchange on this connection; "" = a key was made, else why the connection ends.
func (s *Server) exchange(cc *clientConn, first []byte) string {
	if !s.limits.Hit([2]string{"mt-dh", cc.ip}, s.cfg.HandshakesPerMin, 60) {
		_ = cc.fc.writeTransportError(transportFlood, s.cfg.Now().Add(s.cfg.WriteWait))
		if ok, skipped := rate.Allow([2]string{"dh-flood", cc.ip}, 60); ok {
			log.Warn("mt.exchange_refused " + diag.KV(diag.KVP{K: "ip", V: cc.ip}, diag.KVP{K: "limit_per_min", V: s.cfg.HandshakesPerMin},
				diag.KVP{K: "skipped", V: nz(skipped)}) + ": too many key exchanges from this address")
		}
		return "key exchange rate limit"
	}
	s.mu.Lock()
	full := len(s.keys) >= s.cfg.MaxKeys
	unbound := 0
	if !full {
		for _, k := range s.keys {
			if k.ip == cc.ip {
				k.mu.Lock()
				if k.xuid == 0 {
					unbound++
				}
				k.mu.Unlock()
			}
		}
	}
	s.mu.Unlock()
	if full || unbound >= s.cfg.MaxUnboundPerIP {
		_ = cc.fc.writeTransportError(transportFlood, s.cfg.Now().Add(s.cfg.WriteWait))
		why := fmt.Sprintf("%d keys in memory (limit %d)", s.cfg.MaxKeys, s.cfg.MaxKeys)
		if !full {
			why = fmt.Sprintf("%d keys of this address have not signed in (limit %d)", unbound, s.cfg.MaxUnboundPerIP)
		}
		if ok, _ := rate.Allow([2]string{"key-cap", cc.ip}, 60); ok {
			log.Warn("mt.exchange_refused " + diag.KV(diag.KVP{K: "ip", V: cc.ip}) + ": " + why)
		}
		return "key limit: " + why
	}
	res, err := runExchange(cc.fc, first, &s.xcfg)
	if err != nil {
		var xe *exchangeError
		if errors.As(err, &xe) {
			log.Info("mt.exchange_refused " + diag.KV(diag.KVP{K: "conn", V: cc.id}, diag.KVP{K: "ip", V: cc.ip}, diag.KVP{K: "step", V: xe.step}) + ": " + xe.why)
			return "key exchange failed at " + xe.step
		}
		log.Exc("mt.exchange_failed "+diag.KV(diag.KVP{K: "conn", V: cc.id}, diag.KVP{K: "ip", V: cc.ip}), err)
		return "key exchange failed: " + err.Error()
	}
	now := s.cfg.Now()
	k := &authKey{ak: res.key, created: now, expires: now.Add(time.Duration(res.expiresIn) * time.Second), ip: cc.ip, firstSalt: res.salt,
		lastUsed: now, sessions: map[int64]*session{}}
	s.mu.Lock()
	if s.keys[k.ak.ID] != nil {
		s.mu.Unlock()
		return "auth key id collision"
	}
	s.keys[k.ak.ID] = k
	s.mu.Unlock()
	log.Debug("mt.key_made " + diag.KV(diag.KVP{K: "conn", V: cc.id}, diag.KVP{K: "ip", V: cc.ip}, diag.KVP{K: "key", V: k.idHex()},
		diag.KVP{K: "ttl_s", V: res.expiresIn}, diag.KVP{K: "took_ms", V: res.took.Milliseconds()}))
	return ""
}

// inMsg is one decrypted client message (or one message inside a container).
type inMsg struct {
	id   int64
	seq  int32
	body []byte
}

func contentRelated(body []byte) bool {
	if len(body) < 4 {
		return true
	}
	switch binary.LittleEndian.Uint32(body) {
	case mt.MsgsAckTypeID, mt.MsgContainerTypeID:
		return false
	}
	return true
}

// handleEncrypted decrypts one frame, applies the session rules and runs what it carries. "" = keep the connection, else why it ends.
func (s *Server) handleEncrypted(cc *clientConn, k *authKey, frame []byte) string {
	frame, err := trimEncrypted(frame, cc.fc.padded)
	if err != nil {
		return "protocol violation: " + err.Error()
	}
	var msgKey bin.Int128
	copy(msgKey[:], frame[8:24])
	data := frame[24:]
	aesKey, iv := crypto.Keys(k.ak.Value, msgKey, crypto.Client)
	blk, err := aes.NewCipher(aesKey[:])
	if err != nil {
		return "internal: " + err.Error()
	}
	plain := make([]byte, len(data))
	ige.DecryptBlocks(blk, iv[:], plain, data)
	want := crypto.MessageKey(k.ak.Value, plain, crypto.Client)
	if subtle.ConstantTimeCompare(want[:], msgKey[:]) != 1 {
		return "protocol violation: msg_key does not match the decrypted message (wrong key, or tampered)"
	}
	// salt(8) session_id(8) msg_id(8) seq_no(4) length(4) body padding(12..1024)
	if len(plain) < 32+12 {
		return fmt.Sprintf("protocol violation: decrypted message of %d bytes", len(plain))
	}
	salt := int64(binary.LittleEndian.Uint64(plain[0:]))
	sid := int64(binary.LittleEndian.Uint64(plain[8:]))
	m := inMsg{id: int64(binary.LittleEndian.Uint64(plain[16:])), seq: int32(binary.LittleEndian.Uint32(plain[24:]))}
	n := int(binary.LittleEndian.Uint32(plain[28:]))
	pad := len(plain) - 32 - n
	if n < 4 || n%4 != 0 || pad < 12 || pad > 1024 {
		return fmt.Sprintf("protocol violation: body length %d with %d bytes of padding (4-byte aligned body, 12..1024 padding expected)", n, pad)
	}
	m.body = plain[32 : 32+n]
	now := s.cfg.Now()

	ss, created, why := s.sessionFor(k, sid, now)
	if why != "" {
		return why
	}
	ss.mu.Lock()
	if ss.conn != cc {
		ss.conn = cc
	}
	ss.lastUsed = now
	var out []outMsg
	if created {
		out = append(out, outMsg{obj: &mt.NewSessionCreated{FirstMsgID: m.id, UniqueID: randInt64(), ServerSalt: s.currentSalt(k, now)}, content: true})
	}
	if code, _ := ss.check(m.id, m.seq, contentRelated(m.body), now); code != 0 {
		out = append(out, outMsg{obj: &mt.BadMsgNotification{BadMsgID: m.id, BadMsgSeqno: int(m.seq), ErrorCode: code}})
		ss.mu.Unlock()
		s.send(ss, out...)
		return ""
	}
	if !s.saltOK(k, salt, now) {
		out = append(out, outMsg{obj: &mt.BadServerSalt{BadMsgID: m.id, BadMsgSeqno: int(m.seq), ErrorCode: badServerSaltCode, NewServerSalt: s.currentSalt(k, now)}})
		ss.mu.Unlock()
		s.send(ss, out...)
		return ""
	}
	ss.mu.Unlock()
	if len(out) > 0 {
		s.send(ss, out...)
	}
	return s.process(cc, k, ss, m, false)
}

// sessionFor returns the key's session, made on first use; a key with MaxSessions sessions drops its least recently used one.
func (s *Server) sessionFor(k *authKey, sid int64, now time.Time) (*session, bool, string) {
	k.mu.Lock()
	defer k.mu.Unlock()
	if k.dead {
		return nil, false, "the auth key was ended"
	}
	k.lastUsed = now
	if ss := k.sessions[sid]; ss != nil {
		return ss, false, ""
	}
	if len(k.sessions) >= s.cfg.MaxSessions {
		var oldest *session
		for _, o := range k.sessions {
			o.mu.Lock()
			if oldest == nil || o.lastUsed.Before(oldest.lastUsed) {
				oldest = o
			}
			o.mu.Unlock()
		}
		delete(k.sessions, oldest.id)
	}
	ss := newSession(sid, k, now)
	k.sessions[sid] = ss
	return ss, true, ""
}

// process runs one accepted (outer or contained) message: the msg_id/seq checks of contained messages, the service messages and API calls.
func (s *Server) process(cc *clientConn, k *authKey, ss *session, m inMsg, inner bool) string {
	now := s.cfg.Now()
	ss.mu.Lock()
	if inner {
		if code, _ := ss.check(m.id, m.seq, contentRelated(m.body), now); code != 0 {
			ss.mu.Unlock()
			s.send(ss, outMsg{obj: &mt.BadMsgNotification{BadMsgID: m.id, BadMsgSeqno: int(m.seq), ErrorCode: code}})
			return ""
		}
	}
	if _, dup := ss.seen[m.id]; dup {
		// A repeat (the client re-sent after a reconnect): the cached answer goes out again; without one (still running, or acked) it is dropped.
		r := ss.results[m.id]
		ss.mu.Unlock()
		if r != nil {
			s.send(ss, outMsg{raw: r.body, content: true, response: true, ackOf: m.id, cacheFor: m.id})
		}
		return ""
	}
	ss.record(m.id, m.seq)
	if contentRelated(m.body) {
		ss.acks = append(ss.acks, m.id)
	}
	ss.mu.Unlock()

	id := binary.LittleEndian.Uint32(m.body)
	b := &bin.Buffer{Buf: m.body}
	switch id {
	case mt.MsgContainerTypeID:
		if inner {
			s.send(ss, outMsg{obj: &mt.BadMsgNotification{BadMsgID: m.id, BadMsgSeqno: int(m.seq), ErrorCode: badContainerInvalid}})
			return ""
		}
		var c proto.MessageContainer
		if err := c.Decode(b); err != nil || len(c.Messages) > 1020 {
			s.send(ss, outMsg{obj: &mt.BadMsgNotification{BadMsgID: m.id, BadMsgSeqno: int(m.seq), ErrorCode: badContainerInvalid}})
			return ""
		}
		for _, im := range c.Messages {
			if len(im.Body) < 4 || im.ID >= m.id {
				s.send(ss, outMsg{obj: &mt.BadMsgNotification{BadMsgID: im.ID, BadMsgSeqno: im.SeqNo, ErrorCode: badContainerInvalid}})
				continue
			}
			if why := s.process(cc, k, ss, inMsg{id: im.ID, seq: int32(im.SeqNo), body: im.Body}, true); why != "" {
				return why
			}
		}
		s.flushAcks(ss)
		return ""
	case mt.MsgsAckTypeID:
		var a mt.MsgsAck
		if err := a.Decode(b); err != nil {
			return "protocol violation: msgs_ack does not decode: " + err.Error()
		}
		ss.mu.Lock()
		pts := ss.acked(a.MsgIDs)
		ss.mu.Unlock()
		if pts > 0 {
			s.ackPts(k, pts)
		}
		return ""
	case mt.PingRequestTypeID:
		var p mt.PingRequest
		if err := p.Decode(b); err != nil {
			return "protocol violation: ping does not decode"
		}
		s.send(ss, outMsg{obj: &mt.Pong{MsgID: m.id, PingID: p.PingID}, content: true, response: true, ackOf: m.id})
		return ""
	case mt.PingDelayDisconnectRequestTypeID:
		var p mt.PingDelayDisconnectRequest
		if err := p.Decode(b); err != nil {
			return "protocol violation: ping_delay_disconnect does not decode"
		}
		d := time.Duration(p.DisconnectDelay) * time.Second
		if d < 10*time.Second {
			d = 10 * time.Second
		}
		if d > s.cfg.IdleTimeout {
			d = s.cfg.IdleTimeout
		}
		cc.idle.Store(int64(d))
		s.send(ss, outMsg{obj: &mt.Pong{MsgID: m.id, PingID: p.PingID}, content: true, response: true, ackOf: m.id})
		return ""
	case mt.GetFutureSaltsRequestTypeID:
		var g mt.GetFutureSaltsRequest
		if err := g.Decode(b); err != nil {
			return "protocol violation: get_future_salts does not decode"
		}
		s.send(ss, outMsg{obj: &mt.FutureSalts{ReqMsgID: m.id, Now: int(now.Unix()), Salts: s.futureSalts(k, now, g.Num)}, content: true, response: true, ackOf: m.id})
		return ""
	case mt.DestroySessionRequestTypeID:
		var d mt.DestroySessionRequest
		if err := d.Decode(b); err != nil {
			return "protocol violation: destroy_session does not decode"
		}
		var res bin.Encoder = &mt.DestroySessionNone{SessionID: d.SessionID}
		k.mu.Lock()
		if d.SessionID != ss.id && k.sessions[d.SessionID] != nil {
			delete(k.sessions, d.SessionID)
			res = &mt.DestroySessionOk{SessionID: d.SessionID}
		}
		k.mu.Unlock()
		s.sendRPC(ss, m.id, res, nil)
		return ""
	case mt.RPCDropAnswerRequestTypeID:
		var d mt.RPCDropAnswerRequest
		if err := d.Decode(b); err != nil {
			return "protocol violation: rpc_drop_answer does not decode"
		}
		var res bin.Encoder = &mt.RPCAnswerUnknown{}
		ss.mu.Lock()
		if r := ss.results[d.ReqMsgID]; r != nil {
			delete(ss.bySrv, r.serverID)
			delete(ss.results, d.ReqMsgID)
			res = &mt.RPCAnswerDropped{MsgID: r.serverID, SeqNo: 0, Bytes: len(r.body)}
		}
		ss.mu.Unlock()
		s.sendRPC(ss, m.id, res, nil)
		return ""
	case mt.MsgsStateReqTypeID:
		var q mt.MsgsStateReq
		if err := q.Decode(b); err != nil {
			return "protocol violation: msgs_state_req does not decode"
		}
		info := make([]byte, len(q.MsgIDs))
		ss.mu.Lock()
		for i, qid := range q.MsgIDs {
			switch _, seen := ss.seen[qid]; {
			case seen:
				info[i] = 4 // received
			case qid <= ss.floor:
				info[i] = 2 // too old to tell
			default:
				info[i] = 1 // not received
			}
		}
		ss.mu.Unlock()
		s.send(ss, outMsg{obj: &mt.MsgsStateInfo{ReqMsgID: m.id, Info: info}, content: true, response: true, ackOf: m.id})
		return ""
	case mt.MsgResendReqTypeID, mt.HTTPWaitRequestTypeID:
		// Not used by our clients over TCP: acked and otherwise ignored.
		return ""
	case mt.GzipPackedTypeID:
		var g proto.GZIP
		if err := g.Decode(b); err != nil {
			return "protocol violation: gzip_packed does not unpack: " + err.Error()
		}
		if len(g.Data) > 4*s.cfg.MaxFrame || len(g.Data) < 4 {
			return fmt.Sprintf("protocol violation: gzip_packed unpacks to %d bytes", len(g.Data))
		}
		return s.call(cc, k, ss, m.id, g.Data)
	}
	return s.call(cc, k, ss, m.id, m.body)
}

// outMsg is one message to send: an object, whether it is content related (odd seq_no), whether it answers a client message.
type outMsg struct {
	obj      bin.Encoder
	raw      []byte
	content  bool
	response bool
	ackOf    int64 // the client message it answers (removed from the pending acks)
	pts      int   // an `updates` push: its pts, remembered until acked
	cacheFor int64 // an rpc_result: the request id it is cached under
}

// send seals messages (with pending acks first) into one frame and queues it on the session's connection. Without a live connection the
// messages are dropped: results stay in the cache for a repeated request and stored updates are fetched with updates.getDifference.
func (s *Server) send(ss *session, msgs ...outMsg) {
	k := ss.key
	now := s.cfg.Now()
	ss.mu.Lock()
	defer ss.mu.Unlock()
	cc := ss.conn
	if cc == nil || cc.closed() {
		return
	}
	for _, m := range msgs {
		if m.ackOf != 0 {
			for i, a := range ss.acks {
				if a == m.ackOf {
					ss.acks = append(ss.acks[:i], ss.acks[i+1:]...)
					break
				}
			}
		}
	}
	type sealed struct {
		id   int64
		seq  int32
		body []byte
	}
	var parts []sealed
	if len(ss.acks) > 0 {
		var b bin.Buffer
		_ = (&mt.MsgsAck{MsgIDs: ss.acks}).Encode(&b)
		parts = append(parts, sealed{id: s.ids.next(false), seq: 2 * ss.outContent, body: b.Buf})
		ss.acks = nil
	}
	for _, m := range msgs {
		body := m.raw
		if body == nil {
			var b bin.Buffer
			if err := m.obj.Encode(&b); err != nil {
				log.Exc(fmt.Sprintf("mt.encode_failed %T", m.obj), err)
				continue
			}
			body = b.Buf
		}
		seq := 2 * ss.outContent
		if m.content {
			seq++
			ss.outContent++
		}
		id := s.ids.next(m.response)
		if m.pts > 0 {
			ss.pushes[id] = m.pts
		}
		if m.cacheFor != 0 {
			ss.cacheResult(m.cacheFor, id, body)
		}
		parts = append(parts, sealed{id: id, seq: seq, body: body})
	}
	if len(parts) == 0 {
		return
	}
	var msgID int64
	var seq int32
	var body []byte
	if len(parts) == 1 {
		msgID, seq, body = parts[0].id, parts[0].seq, parts[0].body
	} else {
		c := proto.MessageContainer{}
		for _, p := range parts {
			c.Messages = append(c.Messages, proto.Message{ID: p.id, SeqNo: int(p.seq), Bytes: len(p.body), Body: p.body})
		}
		var b bin.Buffer
		_ = c.Encode(&b)
		msgID, seq, body = s.ids.next(false), 2*ss.outContent, b.Buf
	}
	frame, err := s.seal(k, ss.id, s.currentSalt(k, now), msgID, seq, body)
	if err != nil {
		log.Exc("mt.seal_failed", err)
		return
	}
	cc.enqueue(frame)
}

// seal encrypts one message for the client (MTProto 2.0, x = 8): salt, session_id, msg_id, seq_no, length, body, 12..1024 bytes of padding.
func (s *Server) seal(k *authKey, sid, salt, msgID int64, seq int32, body []byte) ([]byte, error) {
	n := 32 + len(body)
	pad := 12 + (16-(n+12)%16)%16
	var rb [1]byte
	_, _ = rand.Read(rb[:])
	pad += 16 * int(rb[0]%4)
	plain := make([]byte, n+pad)
	binary.LittleEndian.PutUint64(plain[0:], uint64(salt))
	binary.LittleEndian.PutUint64(plain[8:], uint64(sid))
	binary.LittleEndian.PutUint64(plain[16:], uint64(msgID))
	binary.LittleEndian.PutUint32(plain[24:], uint32(seq))
	binary.LittleEndian.PutUint32(plain[28:], uint32(len(body)))
	copy(plain[32:], body)
	if _, err := rand.Read(plain[n:]); err != nil {
		return nil, err
	}
	msgKey := crypto.MessageKey(k.ak.Value, plain, crypto.Server)
	aesKey, iv := crypto.Keys(k.ak.Value, msgKey, crypto.Server)
	blk, err := aes.NewCipher(aesKey[:])
	if err != nil {
		return nil, err
	}
	out := make([]byte, 24+len(plain))
	copy(out, k.ak.ID[:])
	copy(out[8:], msgKey[:])
	ige.EncryptBlocks(blk, iv[:], out[24:], plain)
	return out, nil
}

// flushAcks sends pending acks on their own (after a container that produced no answers).
func (s *Server) flushAcks(ss *session) {
	ss.mu.Lock()
	n := len(ss.acks)
	ss.mu.Unlock()
	if n > 0 {
		s.send(ss)
	}
}

// sendResult sends an rpc_result with an already encoded result body and caches it for a repeated request.
func (s *Server) sendResult(ss *session, reqID int64, result []byte) {
	var b bin.Buffer
	_ = (&proto.Result{RequestMessageID: reqID, Result: result}).Encode(&b)
	s.send(ss, outMsg{raw: b.Buf, content: true, response: true, ackOf: reqID, cacheFor: reqID})
}

// sendRPC answers a request with res, or with rpc_error for rerr.
func (s *Server) sendRPC(ss *session, reqID int64, res bin.Encoder, rerr *RPCError) {
	var b bin.Buffer
	if rerr != nil {
		_ = (&mt.RPCError{ErrorCode: rerr.Code, ErrorMessage: rerr.Message}).Encode(&b)
	} else if err := res.Encode(&b); err != nil {
		log.Exc(fmt.Sprintf("mt.encode_failed %T", res), err)
		_ = (&mt.RPCError{ErrorCode: 500, ErrorMessage: "INTERNAL"}).Encode(&b)
	}
	s.sendResult(ss, reqID, b.Buf)
}

// call runs one API request and answers it.
func (s *Server) call(cc *clientConn, k *authKey, ss *session, reqID int64, body []byte) string {
	if len(body) < 4 {
		return "protocol violation: request shorter than a constructor id"
	}
	c := &Call{s: s, ss: ss, k: k, ip: cc.ip, reqID: reqID}
	start := s.cfg.Now()
	res, rerr := s.dispatch(c, body)
	if took := s.cfg.Now().Sub(start); took > 2*time.Second {
		log.Warn("mt.slow_call " + diag.KV(diag.KVP{K: "method", V: methodName(binary.LittleEndian.Uint32(body))}, diag.KVP{K: "took_ms", V: took.Milliseconds()},
			diag.KVP{K: "xuid", V: c.xuidHex()}))
	}
	s.sendRPC(ss, reqID, res, rerr)
	return ""
}

func methodName(id uint32) string {
	if n, ok := tl.TypesMap()[id]; ok {
		return n
	}
	return fmt.Sprintf("%#08x", id)
}

// dispatch gates a request on help.init and sign-in, then runs it. Errors that are not *RPCError come from decoding the request.
func (s *Server) dispatch(c *Call, body []byte) (bin.Encoder, *RPCError) {
	id := binary.LittleEndian.Uint32(body)
	c.ss.mu.Lock()
	layer := c.ss.layer
	c.ss.mu.Unlock()
	if layer == 0 && id != tl.HelpInitRequestTypeID {
		return nil, &RPCError{Code: 400, Message: "INIT_REQUIRED"}
	}
	switch id {
	case tl.HelpInitRequestTypeID, tl.AuthGetChallengeRequestTypeID, tl.AuthSignInRequestTypeID:
	default:
		if c.xuid() == 0 {
			return nil, &RPCError{Code: 401, Message: "AUTH_KEY_UNREGISTERED"}
		}
	}
	res, err := s.disp.Handle(context.WithValue(context.Background(), callKey{}, c), &bin.Buffer{Buf: body})
	if err != nil {
		var re *RPCError
		if errors.As(err, &re) {
			if re.detail != "" {
				log.Debug("mt.call_refused " + diag.KV(diag.KVP{K: "method", V: methodName(id)}, diag.KVP{K: "code", V: re.Code}, diag.KVP{K: "error", V: re.Message},
					diag.KVP{K: "xuid", V: c.xuidHex()}, diag.KVP{K: "ip", V: c.ip}) + ": " + re.detail)
			}
			return nil, re
		}
		return nil, &RPCError{Code: 400, Message: "INPUT_FETCH_ERROR"}
	}
	return res, nil
}

// RPCError is an rpc_error answer: Telegram-style code and UPPER_SNAKE message.
type RPCError struct {
	Code    int
	Message string
	detail  string
}

func (e *RPCError) Error() string { return fmt.Sprintf("rpc error %d %s", e.Code, e.Message) }

func rpcErr(code int, msg, detail string) *RPCError {
	return &RPCError{Code: code, Message: msg, detail: detail}
}

// Call is the context of one API request.
type Call struct {
	s     *Server
	ss    *session
	k     *authKey
	ip    string
	reqID int64
}

type callKey struct{}

func callOf(ctx context.Context) *Call { c, _ := ctx.Value(callKey{}).(*Call); return c }

func (c *Call) xuid() int64 {
	c.k.mu.Lock()
	defer c.k.mu.Unlock()
	return c.k.xuid
}

func (c *Call) xuidHex() any {
	if x := c.xuid(); x != 0 {
		return fmt.Sprintf("%#x", x)
	}
	return nil
}

// bind signs key k in as xuid. Another key of the account is ended: its sessions get updateLoggedOut, then their connections close.
func (s *Server) bind(k *authKey, xuid int64, tag string, device int64) {
	s.mu.Lock()
	old := s.byXuid[xuid]
	s.byXuid[xuid] = k
	if old == k {
		old = nil
	}
	if old != nil {
		delete(s.keys, old.ak.ID)
	}
	s.mu.Unlock()
	k.mu.Lock()
	k.xuid, k.tag, k.device = xuid, tag, device
	k.mu.Unlock()
	if old != nil {
		old.mu.Lock()
		oldDevice := old.device
		old.mu.Unlock()
		reason := "signed in on another connection"
		level := log.Info
		if oldDevice != device {
			reason = "signed in on another device"
			level = log.Warn
		}
		level("mt.session_replaced " + diag.KV(diag.KVP{K: "xuid", V: fmt.Sprintf("%#x", xuid)}, diag.KVP{K: "old_key", V: old.idHex()}, diag.KVP{K: "new_key", V: k.idHex()},
			diag.KVP{K: "old_device", V: fmt.Sprintf("%016x", uint64(oldDevice))}, diag.KVP{K: "new_device", V: fmt.Sprintf("%016x", uint64(device))}) + ": one chat session per account, the older one ends")
		s.endKey(old, reason)
	}
}

// unbind signs key k out (auth.logOut): the key stays, without an account.
func (s *Server) unbind(k *authKey) int64 {
	k.mu.Lock()
	x := k.xuid
	k.xuid, k.tag = 0, ""
	k.mu.Unlock()
	if x != 0 {
		s.mu.Lock()
		if s.byXuid[x] == k {
			delete(s.byXuid, x)
		}
		s.mu.Unlock()
	}
	return x
}

// endKey ends a key that left the tables: its sessions are told why and their connections close once that is written.
func (s *Server) endKey(k *authKey, reason string) {
	k.mu.Lock()
	k.dead = true
	sessions := make([]*session, 0, len(k.sessions))
	for _, ss := range k.sessions {
		sessions = append(sessions, ss)
	}
	k.mu.Unlock()
	upd := &tl.UpdateShort{Update: &tl.UpdateLoggedOut{Reason: reason}, Date: int(s.cfg.Now().Unix())}
	for _, ss := range sessions {
		ss.mu.Lock()
		cc, layer := ss.conn, ss.layer
		ss.mu.Unlock()
		if layer > 0 {
			s.send(ss, outMsg{obj: upd, content: true})
		}
		if cc != nil {
			go func(cc *clientConn) {
				// Give the writer a moment to put updateLoggedOut on the wire before the close.
				select {
				case <-time.After(500 * time.Millisecond):
				case <-cc.done:
				}
				cc.close("session ended: " + reason)
			}(cc)
		}
	}
}

// EndAccount ends the account's chat session (a ban, a revoke): the client is told reason and its key is forgotten.
func (s *Server) EndAccount(xuid int64, reason string) bool {
	s.mu.Lock()
	k := s.byXuid[xuid]
	if k != nil {
		delete(s.byXuid, xuid)
		delete(s.keys, k.ak.ID)
	}
	s.mu.Unlock()
	if k == nil {
		return false
	}
	log.Info("mt.account_ended " + diag.KV(diag.KVP{K: "xuid", V: fmt.Sprintf("%#x", xuid)}, diag.KVP{K: "key", V: k.idHex()}) + ": " + reason)
	s.endKey(k, reason)
	return true
}

// Push sends stored updates (already appended to the store, pts = the last one's pts) to the account's signed-in sessions.
func (s *Server) Push(xuid int64, upds []tl.UpdateClass, pts int) {
	s.mu.Lock()
	k := s.byXuid[xuid]
	s.mu.Unlock()
	if k == nil || len(upds) == 0 {
		return
	}
	k.mu.Lock()
	sessions := make([]*session, 0, len(k.sessions))
	for _, ss := range k.sessions {
		sessions = append(sessions, ss)
	}
	k.mu.Unlock()
	msg := &tl.Updates{Updates: upds, Pts: pts, PtsCount: len(upds), Date: int(s.cfg.Now().Unix())}
	for _, ss := range sessions {
		ss.mu.Lock()
		live := ss.layer > 0 && ss.conn != nil && !ss.conn.closed()
		ss.mu.Unlock()
		if live {
			s.send(ss, outMsg{obj: msg, content: true, pts: pts})
		}
	}
}

// Notify stores an operator notice for the account and pushes it.
func (s *Server) Notify(xuid int64, text string) error {
	u := &tl.UpdateNotice{ID: randInt64(), Text: text, Date: int(s.cfg.Now().Unix())}
	var b bin.Buffer
	if err := u.Encode(&b); err != nil {
		return err
	}
	pts, err := s.store.Append(xuid, b.Buf)
	if err != nil {
		return err
	}
	s.Push(xuid, []tl.UpdateClass{u}, pts)
	return nil
}

// ackPts: the client confirmed stored updates up to pts.
func (s *Server) ackPts(k *authKey, pts int) {
	k.mu.Lock()
	x := k.xuid
	k.mu.Unlock()
	if x == 0 {
		return
	}
	if err := s.store.Ack(x, pts); err != nil {
		log.Exc("mt.ack_failed "+diag.KV(diag.KVP{K: "xuid", V: fmt.Sprintf("%#x", x)}, diag.KVP{K: "pts", V: pts}), err, diag.Warn)
	}
}

// janitor forgets expired keys, keys that never signed in and went idle, and message ids that left the window.
func (s *Server) janitor() {
	defer s.wg.Done()
	t := time.NewTicker(30 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-s.stop:
			return
		case <-t.C:
		}
		s.sweep(s.cfg.Now())
	}
}

func (s *Server) sweep(now time.Time) {
	type ended struct {
		k   *authKey
		why string
	}
	var gone []ended
	s.mu.Lock()
	for id, k := range s.keys {
		k.mu.Lock()
		why := ""
		switch {
		case now.After(k.expires):
			why = "the temporary key expired"
		case k.xuid == 0 && now.Sub(k.lastUsed) > s.cfg.UnboundIdle:
			why = "the key did not sign in and went idle"
		}
		if why != "" {
			delete(s.keys, id)
			if k.xuid != 0 && s.byXuid[k.xuid] == k {
				delete(s.byXuid, k.xuid)
			}
			gone = append(gone, ended{k, why})
		} else {
			for _, ss := range k.sessions {
				ss.mu.Lock()
				ss.expireSeen(now)
				ss.mu.Unlock()
			}
		}
		k.mu.Unlock()
	}
	s.mu.Unlock()
	for _, e := range gone {
		e.k.mu.Lock()
		x := e.k.xuid
		e.k.mu.Unlock()
		if x != 0 {
			log.Info("mt.key_expired " + diag.KV(diag.KVP{K: "xuid", V: fmt.Sprintf("%#x", x)}, diag.KVP{K: "key", V: e.k.idHex()}) + ": " + e.why + ", the client makes a new one")
		}
		s.endKey(e.k, e.why)
	}
}

// Stats counts what is in memory.
func (s *Server) Stats() map[string]int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return map[string]int{"connections": len(s.conns), "keys": len(s.keys), "signed_in": len(s.byXuid)}
}

var rate = diag.NewRateLimit(nil, 0)

func nz(n int) any {
	if n == 0 {
		return nil
	}
	return n
}

// envOr is os.Getenv with a default.
func envOr(name, def string) string {
	if v := strings.TrimSpace(os.Getenv(name)); v != "" {
		return v
	}
	return def
}
