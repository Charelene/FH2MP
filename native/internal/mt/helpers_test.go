package mt

import (
	"context"
	"crypto/aes"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/rsa"
	"crypto/subtle"
	"crypto/x509"
	"encoding/binary"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/gotd/ige"
	"github.com/gotd/td/bin"
	"github.com/gotd/td/crypto"
	"github.com/gotd/td/exchange"
	"github.com/gotd/td/mt"
	"github.com/gotd/td/mtproxy"
	"github.com/gotd/td/mtproxy/obfuscated2"
	"github.com/gotd/td/proto"

	"fh2mp/internal/auth"
	"fh2mp/internal/mt/tl"
)

var (
	testKeyOnce sync.Once
	testKey     *serverKey
	testKeyPEM  []byte
)

func serverKeyForTest(t testing.TB) *serverKey {
	testKeyOnce.Do(func() {
		k, err := rsa.GenerateKey(rand.Reader, 2048)
		if err != nil {
			panic(err)
		}
		testKeyPEM = pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(k)})
		if testKey, err = parseServerKey(testKeyPEM); err != nil {
			panic(err)
		}
	})
	return testKey
}

// fakeAccounts verifies sign-ins like auth.Service: single-use challenges, an Ed25519 signature over auth.SignMessage, accounts by public key.
type fakeAccounts struct {
	mu     sync.Mutex
	realm  string
	chals  map[string]bool
	byKey  map[string]int64
	tags   map[int64]string
	banned map[int64]bool
}

func newFakeAccounts(realm string) *fakeAccounts {
	return &fakeAccounts{realm: realm, chals: map[string]bool{}, byKey: map[string]int64{}, tags: map[int64]string{}, banned: map[int64]bool{}}
}

func (f *fakeAccounts) add(xuid int64, tag string) ed25519.PrivateKey {
	pub, priv, _ := ed25519.GenerateKey(rand.Reader)
	f.mu.Lock()
	f.byKey[string(pub)] = xuid
	f.tags[xuid] = tag
	f.mu.Unlock()
	return priv
}

func (f *fakeAccounts) Challenge(ip string) ([]byte, float64, error) {
	b := make([]byte, 41)
	rand.Read(b)
	f.mu.Lock()
	f.chals[string(b)] = true
	f.mu.Unlock()
	return b, 120, nil
}

func (f *fakeAccounts) SignIn(ip string, chal, pub, sig []byte, fields ...[]byte) (int64, string, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if !f.chals[string(chal)] {
		return 0, "", auth.NewError(400, "challenge_used", "that challenge was already used")
	}
	delete(f.chals, string(chal))
	if len(pub) != 32 || !ed25519.Verify(pub, auth.SignMessage(f.realm, "mtproto", chal, append([][]byte{pub}, fields...)...), sig) {
		return 0, "", auth.NewError(401, "bad_signature", "")
	}
	x, ok := f.byKey[string(pub)]
	if !ok {
		return 0, "", auth.NewError(404, "unknown_account", "")
	}
	if f.banned[x] {
		return 0, "", auth.NewError(403, "banned", "banned").WithXuid(x)
	}
	return x, f.tags[x], nil
}

type testServer struct {
	s     *Server
	addr  string
	accs  *fakeAccounts
	store *MemStore
	clock *fakeClock
}

// fakeClock is the server's clock; tests move it to test the message-id window and salts.
type fakeClock struct {
	mu  sync.Mutex
	off time.Duration
}

func (c *fakeClock) now() time.Time      { c.mu.Lock(); defer c.mu.Unlock(); return time.Now().Add(c.off) }
func (c *fakeClock) add(d time.Duration) { c.mu.Lock(); c.off += d; c.mu.Unlock() }

func startServer(t *testing.T, mod func(*Config)) *testServer {
	t.Helper()
	clk := &fakeClock{}
	cfg := Config{Realm: "test", Now: clk.now, IdleTimeout: 20 * time.Second, HandshakeWait: 20 * time.Second}
	if mod != nil {
		mod(&cfg)
	}
	accs := newFakeAccounts("test")
	store := NewMemStore()
	s, err := New(cfg, serverKeyForTest(t), accs, store)
	if err != nil {
		t.Fatal(err)
	}
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	go func() { _ = s.Serve(l) }()
	t.Cleanup(func() { _ = s.Close() })
	return &testServer{s: s, addr: l.Addr().String(), accs: accs, store: store, clock: clk}
}

// rawClient is a hand-driven MTProto client: intermediate framing (optionally inside obfuscated2), gotd's exchange client for the key,
// then messages built and sealed here so the tests control every msg_id, seq_no and salt.
type rawClient struct {
	t      *testing.T
	nc     net.Conn
	r      io.Reader
	w      io.Writer
	key    crypto.AuthKey
	salt   int64
	sid    int64
	nseq   int32
	lastID int64
	offset time.Duration
	queue  []inMsg
}

func dialRaw(t *testing.T, addr string, obf bool) *rawClient {
	t.Helper()
	nc, err := net.DialTimeout("tcp", addr, 3*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = nc.Close() })
	c := &rawClient{t: t, nc: nc, r: nc, w: nc, sid: randInt64()}
	if obf {
		o := obfuscated2.NewObfuscated2(rand.Reader, nc)
		if err := o.Handshake(tagIntermediate, 1, mtproxy.Secret{}); err != nil {
			t.Fatal(err)
		}
		c.r, c.w = o, o
	} else if _, err := nc.Write(tagIntermediate[:]); err != nil {
		t.Fatal(err)
	}
	return c
}

func (c *rawClient) writeFrame(b []byte) error {
	buf := make([]byte, 4+len(b))
	binary.LittleEndian.PutUint32(buf, uint32(len(b)))
	copy(buf[4:], b)
	_, err := c.w.Write(buf)
	return err
}

func (c *rawClient) readFrame(wait time.Duration) ([]byte, error) {
	_ = c.nc.SetReadDeadline(time.Now().Add(wait))
	var h [4]byte
	if _, err := io.ReadFull(c.r, h[:]); err != nil {
		return nil, err
	}
	n := binary.LittleEndian.Uint32(h[:])
	b := make([]byte, n)
	if _, err := io.ReadFull(c.r, b); err != nil {
		return nil, err
	}
	return b, nil
}

// rawTransport lets gotd's exchange client run over the raw connection.
type rawTransport struct{ c *rawClient }

func (r rawTransport) Send(ctx context.Context, b *bin.Buffer) error { return r.c.writeFrame(b.Buf) }
func (r rawTransport) Recv(ctx context.Context, b *bin.Buffer) error {
	f, err := r.c.readFrame(5 * time.Second)
	if err != nil {
		return err
	}
	if len(f) == 4 {
		return fmt.Errorf("transport error %d", int32(binary.LittleEndian.Uint32(f)))
	}
	b.ResetTo(f)
	return nil
}
func (r rawTransport) Close() error { return r.c.nc.Close() }

func (c *rawClient) makeKey(temp bool) error {
	ex := exchange.NewExchanger(rawTransport{c}, 1).WithTimeout(5 * time.Second)
	if temp {
		ex = ex.WithTempMode(3600)
	}
	res, err := ex.Client([]exchange.PublicKey{{RSA: &testKey.priv.PublicKey}}).Run(context.Background())
	if err != nil {
		return err
	}
	c.key, c.salt = res.AuthKey, res.ServerSalt
	return nil
}

func (c *rawClient) mustKey() *rawClient {
	c.t.Helper()
	if err := c.makeKey(true); err != nil {
		c.t.Fatalf("key exchange: %v", err)
	}
	return c
}

func (c *rawClient) newMsgID() int64 {
	t := time.Now().Add(c.offset)
	id := (t.Unix()<<32 | int64(uint64(t.Nanosecond())<<32/1e9)) &^ 3
	if id <= c.lastID {
		id = c.lastID + 4
	}
	c.lastID = id
	return id
}

func (c *rawClient) nextSeq(content bool) int32 {
	s := 2 * c.nseq
	if content {
		s++
		c.nseq++
	}
	return s
}

// sealMsg encrypts one client message (x = 0).
func (c *rawClient) sealMsg(salt, msgID int64, seq int32, body []byte) []byte {
	n := 32 + len(body)
	pad := 12 + (16-(n+12)%16)%16
	plain := make([]byte, n+pad)
	binary.LittleEndian.PutUint64(plain[0:], uint64(salt))
	binary.LittleEndian.PutUint64(plain[8:], uint64(c.sid))
	binary.LittleEndian.PutUint64(plain[16:], uint64(msgID))
	binary.LittleEndian.PutUint32(plain[24:], uint32(seq))
	binary.LittleEndian.PutUint32(plain[28:], uint32(len(body)))
	copy(plain[32:], body)
	rand.Read(plain[n:])
	mk := crypto.MessageKey(c.key.Value, plain, crypto.Client)
	k, iv := crypto.Keys(c.key.Value, mk, crypto.Client)
	blk, _ := aes.NewCipher(k[:])
	out := make([]byte, 24+len(plain))
	copy(out, c.key.ID[:])
	copy(out[8:], mk[:])
	ige.EncryptBlocks(blk, iv[:], out[24:], plain)
	return out
}

func encode(t *testing.T, o bin.Encoder) []byte {
	var b bin.Buffer
	if err := o.Encode(&b); err != nil {
		t.Fatal(err)
	}
	return b.Buf
}

// sendRaw sends one message with exact header fields.
func (c *rawClient) sendRaw(salt, msgID int64, seq int32, body []byte) {
	c.t.Helper()
	if err := c.writeFrame(c.sealMsg(salt, msgID, seq, body)); err != nil {
		c.t.Fatal(err)
	}
}

// send sends obj with a fresh msg_id and the right seq_no; returns the msg_id.
func (c *rawClient) send(obj bin.Encoder) int64 {
	c.t.Helper()
	body := encode(c.t, obj)
	id := c.newMsgID()
	c.sendRaw(c.salt, id, c.nextSeq(contentRelated(body)), body)
	return id
}

var errTransport = errors.New("transport error frame")

// recv decrypts the next server frame into its messages (a container is unpacked).
func (c *rawClient) recv(wait time.Duration) ([]inMsg, error) {
	f, err := c.readFrame(wait)
	if err != nil {
		return nil, err
	}
	if len(f) == 4 {
		return nil, fmt.Errorf("%w %d", errTransport, int32(binary.LittleEndian.Uint32(f)))
	}
	if [8]byte(f[:8]) != c.key.ID {
		return nil, fmt.Errorf("frame for key %x", f[:8])
	}
	var mk bin.Int128
	copy(mk[:], f[8:24])
	k, iv := crypto.Keys(c.key.Value, mk, crypto.Server)
	blk, _ := aes.NewCipher(k[:])
	plain := make([]byte, len(f)-24)
	ige.DecryptBlocks(blk, iv[:], plain, f[24:])
	if want := crypto.MessageKey(c.key.Value, plain, crypto.Server); subtle.ConstantTimeCompare(want[:], mk[:]) != 1 {
		return nil, errors.New("server msg_key does not match")
	}
	if sid := int64(binary.LittleEndian.Uint64(plain[8:])); sid != c.sid {
		return nil, fmt.Errorf("server message for session %x, ours is %x", sid, c.sid)
	}
	m := inMsg{id: int64(binary.LittleEndian.Uint64(plain[16:])), seq: int32(binary.LittleEndian.Uint32(plain[24:]))}
	n := binary.LittleEndian.Uint32(plain[28:])
	m.body = plain[32 : 32+n]
	if binary.LittleEndian.Uint32(m.body) == mt.MsgContainerTypeID {
		var ct proto.MessageContainer
		if err := ct.Decode(&bin.Buffer{Buf: m.body}); err != nil {
			return nil, err
		}
		var out []inMsg
		for _, im := range ct.Messages {
			out = append(out, inMsg{id: im.ID, seq: int32(im.SeqNo), body: im.Body})
		}
		return out, nil
	}
	return []inMsg{m}, nil
}

// next returns the next server message whose constructor is one of ids, skipping acks and new_session_created.
func (c *rawClient) next(ids ...uint32) inMsg {
	c.t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for {
		for i, m := range c.queue {
			got := binary.LittleEndian.Uint32(m.body)
			for _, w := range ids {
				if got == w {
					c.queue = append(c.queue[:i], c.queue[i+1:]...)
					return m
				}
			}
		}
		left := time.Until(deadline)
		if left <= 0 {
			c.t.Fatalf("no server message %x arrived; queued: %s", ids, c.queueNames())
		}
		ms, err := c.recv(left)
		if err != nil {
			c.t.Fatalf("waiting for %x: %v (queued: %s)", ids, err, c.queueNames())
		}
		c.queue = append(c.queue, ms...)
	}
}

func (c *rawClient) queueNames() string {
	s := ""
	for _, m := range c.queue {
		s += fmt.Sprintf("%#08x ", binary.LittleEndian.Uint32(m.body))
	}
	return s
}

// result waits for the rpc_result of reqID and returns its body, or the rpc_error.
func (c *rawClient) result(reqID int64) ([]byte, *mt.RPCError) {
	c.t.Helper()
	for {
		m := c.next(proto.ResultTypeID)
		var r proto.Result
		if err := r.Decode(&bin.Buffer{Buf: m.body}); err != nil {
			c.t.Fatal(err)
		}
		if r.RequestMessageID != reqID {
			continue
		}
		if binary.LittleEndian.Uint32(r.Result) == mt.RPCErrorTypeID {
			var e mt.RPCError
			_ = e.Decode(&bin.Buffer{Buf: r.Result})
			return nil, &e
		}
		return r.Result, nil
	}
}

// call sends req and decodes the answer into res; an rpc_error fails the test unless the caller asked for it with callErr.
func (c *rawClient) call(req bin.Encoder, res bin.Decoder) {
	c.t.Helper()
	body, e := c.result(c.send(req))
	if e != nil {
		c.t.Fatalf("%T: rpc_error %d %s", req, e.ErrorCode, e.ErrorMessage)
	}
	if res != nil {
		if err := res.Decode(&bin.Buffer{Buf: body}); err != nil {
			c.t.Fatalf("%T: decode answer: %v", req, err)
		}
	}
}

func (c *rawClient) callErr(req bin.Encoder) *mt.RPCError {
	c.t.Helper()
	_, e := c.result(c.send(req))
	if e == nil {
		c.t.Fatalf("%T: answered, rpc_error expected", req)
	}
	return e
}

func (c *rawClient) init() *rawClient {
	c.t.Helper()
	var cfg tl.HelpConfig
	c.call(&tl.HelpInitRequest{Layer: Layer, AppVersion: "test", Platform: "go"}, &cfg)
	if cfg.Layer != Layer {
		c.t.Fatalf("server layer %d", cfg.Layer)
	}
	return c
}

// signIn signs in with the account key priv.
func (c *rawClient) signIn(priv ed25519.PrivateKey, device int64) (*tl.AuthAuthorization, *mt.RPCError) {
	c.t.Helper()
	var ch tl.AuthChallenge
	c.call(&tl.AuthGetChallengeRequest{}, &ch)
	pub := priv.Public().(ed25519.PublicKey)
	var dev [8]byte
	binary.LittleEndian.PutUint64(dev[:], uint64(device))
	sig := ed25519.Sign(priv, auth.SignMessage(ch.Realm, "mtproto", ch.Challenge, pub, c.key.ID[:], dev[:]))
	body, e := c.result(c.send(&tl.AuthSignInRequest{Challenge: ch.Challenge, Pubkey: pub, Signature: sig, DeviceID: device}))
	if e != nil {
		return nil, e
	}
	var a tl.AuthAuthorization
	if err := a.Decode(&bin.Buffer{Buf: body}); err != nil {
		c.t.Fatal(err)
	}
	return &a, nil
}

func (c *rawClient) ack(ids ...int64) {
	c.t.Helper()
	c.send(&mt.MsgsAck{MsgIDs: ids})
}
