package mt

import (
	"crypto/ed25519"
	"crypto/rand"
	"sync"
	"testing"

	"github.com/gotd/td/bin"
	"github.com/gotd/td/mt"

	"fh2mp/internal/mt/e2e"
	"fh2mp/internal/mt/tl"
)

// friendsOnly is a Community for the end-to-end tests: friendships in a set, nothing else.
type friendsOnly struct {
	fakeCommunity
	mu    sync.Mutex
	pairs map[[2]int64]bool
}

func (f *friendsOnly) befriend(a, b int64) {
	f.mu.Lock()
	f.pairs[[2]int64{min(a, b), max(a, b)}] = true
	f.mu.Unlock()
}

func (f *friendsOnly) unfriend(a, b int64) {
	f.mu.Lock()
	delete(f.pairs, [2]int64{min(a, b), max(a, b)})
	f.mu.Unlock()
}

func (f *friendsOnly) AreFriends(a, b int64) (bool, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.pairs[[2]int64{min(a, b), max(a, b)}], nil
}

func (f *friendsOnly) FriendIDs(me int64) ([]int64, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	var out []int64
	for p := range f.pairs {
		if p[0] == me {
			out = append(out, p[1])
		} else if p[1] == me {
			out = append(out, p[0])
		}
	}
	return out, nil
}

type e2eTest struct {
	*testServer
	chats   *MemChatStore
	later   *fakeClock // the stores' clock: moved to age stored rows without moving the session layer's
	friends *friendsOnly
}

func startE2E(t *testing.T) *e2eTest {
	ts := startServer(t, nil)
	later := &fakeClock{}
	ts.store.Now = later.now
	f := &friendsOnly{pairs: map[[2]int64]bool{}}
	cs := NewMemChatStore()
	ts.s.SetCommunity(f)
	ts.s.SetChatStore(cs)
	return &e2eTest{testServer: ts, chats: cs, friends: f, later: later}
}

// e2eUser is a signed-in account with its chat device key.
type e2eUser struct {
	c      *rawClient
	x      int64
	acct   ed25519.PrivateKey
	dev    ed25519.PrivateKey
	device int64
}

func (et *e2eTest) user(t *testing.T, x int64, tag string, device int64) *e2eUser {
	acct := et.accs.add(x, tag)
	c := dialRaw(t, et.addr, false).mustKey().init()
	if _, e := c.signIn(acct, device); e != nil {
		t.Fatalf("sign-in %s: %+v", tag, e)
	}
	_, dev, _ := ed25519.GenerateKey(rand.Reader)
	return &e2eUser{c: c, x: x, acct: acct, dev: dev, device: device}
}

func (u *e2eUser) devPub() ed25519.PublicKey { return u.dev.Public().(ed25519.PublicKey) }

func (u *e2eUser) register(t *testing.T) *tl.ChatDevice {
	t.Helper()
	var d tl.ChatDevice
	u.c.call(&tl.AccountRegisterDeviceRequest{Pubkey: u.devPub(), Signature: e2e.SignDevice(u.acct, "test", u.device, u.devPub())}, &d)
	return &d
}

// pushed waits for the next stored-update push and returns its updates.
func (u *e2eUser) pushed(t *testing.T) *tl.Updates {
	t.Helper()
	var p tl.Updates
	if err := p.Decode(&bin.Buffer{Buf: u.c.next(tl.UpdatesTypeID).body}); err != nil {
		t.Fatal(err)
	}
	return &p
}

func (u *e2eUser) short(t *testing.T) tl.UpdateClass {
	t.Helper()
	var p tl.UpdateShort
	if err := p.Decode(&bin.Buffer{Buf: u.c.next(tl.UpdateShortTypeID).body}); err != nil {
		t.Fatal(err)
	}
	return p.Update
}

func (u *e2eUser) difference(t *testing.T, from int) tl.UpdatesDifferenceClass {
	t.Helper()
	var d tl.UpdatesDifferenceBox
	u.c.call(&tl.UpdatesGetDifferenceRequest{Pts: from, Limit: 100}, &d)
	return d.Difference
}

func (u *e2eUser) state(t *testing.T) int {
	var s tl.UpdatesState
	u.c.call(&tl.UpdatesGetStateRequest{}, &s)
	return s.Pts
}

func expectErr(t *testing.T, e *mt.RPCError, msg string) {
	t.Helper()
	if e == nil || e.ErrorMessage != msg {
		t.Fatalf("rpc_error %+v, %s expected", e, msg)
	}
}

// chatPair is an accepted secret chat between a (admin) and b (participant), with the key both made.
type chatPair struct {
	id  int32
	key []byte
	fp  int64
	a   *e2e.Chat
	b   *e2e.Chat
}

func (u *e2eUser) request(t *testing.T, to *e2eUser, randomID int32) (*e2e.Request, tl.EncryptedChatClass) {
	t.Helper()
	var cfg tl.MessagesDhConfigBox
	u.c.call(&tl.MessagesGetDhConfigRequest{Version: 0, RandomLength: 256}, &cfg)
	dh := cfg.DhConfig.(*tl.MessagesDhConfig)
	ends := e2e.Ends{Admin: u.x, AdminDevice: u.device, Participant: to.x, ParticipantDevice: to.device}
	req, err := e2e.MakeRequest(u.dev, ends, randBytes(256), dh.Random)
	if err != nil {
		t.Fatal(err)
	}
	var ch tl.EncryptedChatBox
	u.c.call(&tl.MessagesRequestEncryptionRequest{UserID: to.x, DeviceID: to.device, RandomID: int(randomID), GA: req.GA, GASig: req.Sig}, &ch)
	return req, ch.EncryptedChat
}

func randBytes(n int) []byte {
	b := make([]byte, n)
	rand.Read(b)
	return b
}
