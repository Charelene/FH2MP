package mt

import (
	"crypto/ed25519"
	"encoding/binary"
	"errors"
	"net"
	"testing"
	"time"

	"github.com/gotd/td/bin"
	"github.com/gotd/td/mt"
	"github.com/gotd/td/proto"

	"fh2mp/internal/auth"
	"fh2mp/internal/mt/tl"
)

func TestPingOverTemporaryKey(t *testing.T) {
	for _, obf := range []bool{false, true} {
		ts := startServer(t, nil)
		c := dialRaw(t, ts.addr, obf).mustKey()
		id := c.send(&mt.PingRequest{PingID: 77})
		created := c.next(mt.NewSessionCreatedTypeID)
		var nsc mt.NewSessionCreated
		if err := nsc.Decode(&bin.Buffer{Buf: created.body}); err != nil || nsc.FirstMsgID != id {
			t.Fatalf("obf=%v new_session_created %+v %v, first_msg_id %d expected", obf, nsc, err, id)
		}
		var p mt.Pong
		if err := p.Decode(&bin.Buffer{Buf: c.next(mt.PongTypeID).body}); err != nil || p.PingID != 77 || p.MsgID != id {
			t.Fatalf("obf=%v pong %+v %v", obf, p, err)
		}
	}
}

func TestPermanentKeyRefused(t *testing.T) {
	ts := startServer(t, nil)
	c := dialRaw(t, ts.addr, false)
	if err := c.makeKey(false); err == nil {
		t.Fatal("a permanent key was made; only temporary keys are expected")
	}
}

func TestAbridgedRefused(t *testing.T) {
	ts := startServer(t, nil)
	nc, err := net.Dial("tcp", ts.addr)
	if err != nil {
		t.Fatal(err)
	}
	defer nc.Close()
	nc.Write([]byte{0xef, 1, 2, 3, 4})
	nc.SetReadDeadline(time.Now().Add(3 * time.Second))
	// The server closes without reading the rest: EOF, or a reset on Windows.
	if n, err := nc.Read(make([]byte, 1)); err == nil {
		t.Fatalf("abridged connection: read %d bytes, close expected", n)
	}
}

func TestOversizedFrameCloses(t *testing.T) {
	ts := startServer(t, func(c *Config) { c.MaxFrame = 4096 })
	c := dialRaw(t, ts.addr, false)
	var h [4]byte
	binary.LittleEndian.PutUint32(h[:], 1<<20)
	c.w.Write(h[:])
	if _, err := c.readFrame(3 * time.Second); err == nil {
		t.Fatal("a 1 MiB frame header was taken with MaxFrame 4096")
	}
}

func TestUnknownKeyGets404(t *testing.T) {
	ts := startServer(t, nil)
	c := dialRaw(t, ts.addr, false).mustKey()
	c.key.ID[0] ^= 0xff
	c.send(&mt.PingRequest{PingID: 1})
	if _, err := c.recv(3 * time.Second); err == nil || !errors.Is(err, errTransport) {
		t.Fatalf("unknown key: %v, transport error -404 expected", err)
	}
}

func badMsg(t *testing.T, c *rawClient) mt.BadMsgNotification {
	t.Helper()
	var b mt.BadMsgNotification
	if err := b.Decode(&bin.Buffer{Buf: c.next(mt.BadMsgNotificationTypeID).body}); err != nil {
		t.Fatal(err)
	}
	return b
}

func TestMessageIDRules(t *testing.T) {
	ts := startServer(t, nil)
	c := dialRaw(t, ts.addr, false).mustKey()
	ping := encode(t, &mt.PingRequest{PingID: 1})
	cases := []struct {
		name string
		off  time.Duration
		bits int64
		code int
	}{
		{"too old", -10 * time.Minute, 0, badMsgIDTooLow},
		{"too new", 2 * time.Minute, 0, badMsgIDTooHigh},
		{"low bits", 0, 1, badMsgIDBits},
	}
	for _, tc := range cases {
		c.offset = tc.off
		id := c.newMsgID() | tc.bits
		c.offset = 0
		c.lastID = 0
		c.sendRaw(c.salt, id, c.nextSeq(true), ping)
		if b := badMsg(t, c); b.ErrorCode != tc.code || b.BadMsgID != id {
			t.Errorf("%s: bad_msg %+v, code %d expected", tc.name, b, tc.code)
		}
	}
	// seq_no parity: a content message with an even seq_no, an ack with an odd one.
	id := c.newMsgID()
	c.sendRaw(c.salt, id, 2*c.nseq, ping)
	if b := badMsg(t, c); b.ErrorCode != badSeqOddExpected {
		t.Errorf("even seq on ping: %+v", b)
	}
	id = c.newMsgID()
	c.sendRaw(c.salt, id, 2*c.nseq+1, encode(t, &mt.MsgsAck{MsgIDs: []int64{1}}))
	if b := badMsg(t, c); b.ErrorCode != badSeqEvenExpected {
		t.Errorf("odd seq on msgs_ack: %+v", b)
	}
	// A later message must not reuse an odd seq_no.
	seq := c.nextSeq(true)
	c.sendRaw(c.salt, c.newMsgID(), seq, ping)
	c.next(mt.PongTypeID)
	c.sendRaw(c.salt, c.newMsgID(), seq, ping)
	if b := badMsg(t, c); b.ErrorCode != badSeqTooLow {
		t.Errorf("reused seq: %+v", b)
	}
}

func TestReplayAnsweredFromCache(t *testing.T) {
	ts := startServer(t, nil)
	c := dialRaw(t, ts.addr, false).mustKey()
	c.init()
	body := encode(t, &tl.UpdatesGetStateRequest{})
	id, seq := c.newMsgID(), c.nextSeq(true)
	c.sendRaw(c.salt, id, seq, body)
	if _, e := c.result(id); e == nil || e.ErrorMessage != "AUTH_KEY_UNREGISTERED" {
		t.Fatalf("getState before sign-in: %+v", e)
	}
	// The same message again (a replay, or a resend after a lost answer): the cached answer comes back and nothing runs twice.
	c.sendRaw(c.salt, id, seq, body)
	if _, e := c.result(id); e == nil || e.ErrorMessage != "AUTH_KEY_UNREGISTERED" {
		t.Fatalf("repeated request: %+v", e)
	}
}

func TestBadServerSalt(t *testing.T) {
	ts := startServer(t, nil)
	c := dialRaw(t, ts.addr, false).mustKey()
	id := c.newMsgID()
	c.sendRaw(c.salt^0x5555, id, c.nextSeq(true), encode(t, &mt.PingRequest{PingID: 9}))
	var b mt.BadServerSalt
	if err := b.Decode(&bin.Buffer{Buf: c.next(mt.BadServerSaltTypeID).body}); err != nil || b.BadMsgID != id || b.ErrorCode != badServerSaltCode {
		t.Fatalf("bad_server_salt %+v %v", b, err)
	}
	// The refused message was not remembered: the same msg_id with the new salt is answered.
	c.salt = b.NewServerSalt
	c.sendRaw(c.salt, id, c.nseq*2-1, encode(t, &mt.PingRequest{PingID: 9}))
	c.next(mt.PongTypeID)
	// The exchange's first salt runs out 30 minutes after the key was made; the hourly salt keeps working.
	ts.clock.add(31 * time.Minute)
	c.offset = 31 * time.Minute
	c.send(&mt.PingRequest{PingID: 10})
	c.next(mt.PongTypeID)
}

func TestFutureSalts(t *testing.T) {
	ts := startServer(t, nil)
	c := dialRaw(t, ts.addr, false).mustKey()
	id := c.send(&mt.GetFutureSaltsRequest{Num: 3})
	var fs mt.FutureSalts
	if err := fs.Decode(&bin.Buffer{Buf: c.next(mt.FutureSaltsTypeID).body}); err != nil || fs.ReqMsgID != id || len(fs.Salts) != 3 {
		t.Fatalf("future_salts %+v %v", fs, err)
	}
	for _, s := range fs.Salts[1:] {
		ts.clock.add(time.Hour)
		c.offset += time.Hour
		c.salt = s.Salt
		c.send(&mt.PingRequest{PingID: 2})
		c.next(mt.PongTypeID)
	}
}

func TestInitAndSignInGates(t *testing.T) {
	ts := startServer(t, nil)
	c := dialRaw(t, ts.addr, false).mustKey()
	if e := c.callErr(&tl.AuthGetChallengeRequest{}); e.ErrorCode != 400 || e.ErrorMessage != "INIT_REQUIRED" {
		t.Fatalf("before help.init: %+v", e)
	}
	c.init()
	if e := c.callErr(&tl.UpdatesGetStateRequest{}); e.ErrorCode != 401 {
		t.Fatalf("before sign-in: %+v", e)
	}
}

func TestSignIn(t *testing.T) {
	ts := startServer(t, nil)
	priv := ts.accs.add(0x9000000012345, "Maplefox")
	c := dialRaw(t, ts.addr, false).mustKey().init()
	// A key without an account.
	_, other, _ := ed25519.GenerateKey(nil)
	if _, e := c.signIn(other, 1); e == nil || e.ErrorMessage != "ACCOUNT_UNKNOWN" {
		t.Fatalf("unknown key: %+v", e)
	}
	a, e := c.signIn(priv, 1)
	if e != nil || a.User.ID != 0x9000000012345 || a.User.Gamertag != "Maplefox" {
		t.Fatalf("sign-in: %+v %+v", a, e)
	}
	var st tl.UpdatesState
	c.call(&tl.UpdatesGetStateRequest{}, &st)
	if st.Pts != 0 {
		t.Fatalf("fresh account pts %d", st.Pts)
	}
	if _, e := c.signIn(priv, 1); e == nil || e.ErrorMessage != "ALREADY_SIGNED_IN" {
		t.Fatalf("second sign-in on the same key: %+v", e)
	}
}

func TestSignatureBoundToKey(t *testing.T) {
	ts := startServer(t, nil)
	priv := ts.accs.add(0x9000000000001, "A")
	c := dialRaw(t, ts.addr, false).mustKey().init()
	var ch tl.AuthChallenge
	c.call(&tl.AuthGetChallengeRequest{}, &ch)
	pub := priv.Public().(ed25519.PublicKey)
	var dev [8]byte
	// Signed for another auth key id: refused.
	other := [8]byte{1, 2, 3, 4, 5, 6, 7, 8}
	sig := ed25519.Sign(priv, auth.SignMessage(ch.Realm, "mtproto", ch.Challenge, pub, other[:], dev[:]))
	_, e := c.result(c.send(&tl.AuthSignInRequest{Challenge: ch.Challenge, Pubkey: pub, Signature: sig}))
	if e == nil || e.ErrorMessage != "SIGNATURE_INVALID" {
		t.Fatalf("signature for another key: %+v", e)
	}
}

func TestUpdatesPushAndDifference(t *testing.T) {
	ts := startServer(t, nil)
	const x = 0x9000000000002
	priv := ts.accs.add(x, "B")
	c := dialRaw(t, ts.addr, false).mustKey().init()
	if _, e := c.signIn(priv, 5); e != nil {
		t.Fatal(e)
	}
	if err := ts.s.Notify(x, "hello"); err != nil {
		t.Fatal(err)
	}
	m := c.next(tl.UpdatesTypeID)
	var u tl.Updates
	if err := u.Decode(&bin.Buffer{Buf: m.body}); err != nil || u.Pts != 1 || u.PtsCount != 1 || len(u.Updates) != 1 {
		t.Fatalf("push %+v %v", u, err)
	}
	if n, ok := u.Updates[0].(*tl.UpdateNotice); !ok || n.Text != "hello" {
		t.Fatalf("pushed update %#v", u.Updates[0])
	}
	// Two more while the client is not looking, then the client catches up from pts 1.
	ts.s.Notify(x, "two")
	ts.s.Notify(x, "three")
	var d tl.UpdatesDifferenceBox
	c.call(&tl.UpdatesGetDifferenceRequest{Pts: 1, Limit: 1}, &d)
	sl, ok := d.Difference.(*tl.UpdatesDifferenceSlice)
	if !ok || len(sl.NewUpdates) != 1 || sl.IntermediateState.Pts != 2 {
		t.Fatalf("difference from 1 limit 1: %#v", d.Difference)
	}
	c.call(&tl.UpdatesGetDifferenceRequest{Pts: 2, Limit: 10}, &d)
	full, ok := d.Difference.(*tl.UpdatesDifference)
	if !ok || len(full.NewUpdates) != 1 || full.State.Pts != 3 || full.NewUpdates[0].(*tl.UpdateNotice).Text != "three" {
		t.Fatalf("difference from 2: %#v", d.Difference)
	}
	c.call(&tl.UpdatesGetDifferenceRequest{Pts: 3, Limit: 10}, &d)
	if _, ok := d.Difference.(*tl.UpdatesDifferenceEmpty); !ok {
		t.Fatalf("difference at 3: %#v", d.Difference)
	}
	// Asking from pts 0 after rows up to 2 were acked: the client cannot be given 1 and 2 again, so it reloads.
	c.call(&tl.UpdatesGetDifferenceRequest{Pts: 0, Limit: 10}, &d)
	if tl, ok := d.Difference.(*tl.UpdatesDifferenceTooLong); !ok || tl.Pts != 3 {
		t.Fatalf("difference from 0 after acks: %#v", d.Difference)
	}
}

func TestPushAckDropsStoredRows(t *testing.T) {
	ts := startServer(t, nil)
	const x = 0x9000000000003
	priv := ts.accs.add(x, "C")
	c := dialRaw(t, ts.addr, false).mustKey().init()
	c.signIn(priv, 1)
	ts.s.Notify(x, "n1")
	m := c.next(tl.UpdatesTypeID)
	c.ack(m.id)
	// msgs_ack has no answer: a ping after it proves the ack was handled.
	c.send(&mt.PingRequest{PingID: 3})
	c.next(mt.PongTypeID)
	if rows, _, _ := ts.store.Since(x, 0, 10); len(rows) != 0 {
		t.Fatalf("%d rows kept after the push was acked", len(rows))
	}
}

func TestNewSignInEndsOldSession(t *testing.T) {
	ts := startServer(t, nil)
	const x = 0x9000000000004
	priv := ts.accs.add(x, "D")
	a := dialRaw(t, ts.addr, false).mustKey().init()
	a.signIn(priv, 1)
	b := dialRaw(t, ts.addr, false).mustKey().init()
	if _, e := b.signIn(priv, 2); e != nil {
		t.Fatal(e)
	}
	var us tl.UpdateShort
	if err := us.Decode(&bin.Buffer{Buf: a.next(tl.UpdateShortTypeID).body}); err != nil {
		t.Fatal(err)
	}
	if lo, ok := us.Update.(*tl.UpdateLoggedOut); !ok || lo.Reason != "signed in on another device" {
		t.Fatalf("old session got %#v", us.Update)
	}
	if _, err := a.recv(3 * time.Second); err == nil {
		t.Fatal("the old connection stayed open")
	}
	var st tl.UpdatesState
	b.call(&tl.UpdatesGetStateRequest{}, &st)
}

func TestEndAccount(t *testing.T) {
	ts := startServer(t, nil)
	const x = 0x9000000000005
	priv := ts.accs.add(x, "E")
	c := dialRaw(t, ts.addr, false).mustKey().init()
	c.signIn(priv, 1)
	if !ts.s.EndAccount(x, "banned") {
		t.Fatal("EndAccount found no session")
	}
	var us tl.UpdateShort
	us.Decode(&bin.Buffer{Buf: c.next(tl.UpdateShortTypeID).body})
	if lo, ok := us.Update.(*tl.UpdateLoggedOut); !ok || lo.Reason != "banned" {
		t.Fatalf("ended session got %#v", us.Update)
	}
	if st := ts.s.Stats(); st["keys"] != 0 || st["signed_in"] != 0 {
		t.Fatalf("after EndAccount: %v", st)
	}
}

func TestContainer(t *testing.T) {
	ts := startServer(t, nil)
	c := dialRaw(t, ts.addr, false).mustKey()
	p1, p2 := encode(t, &mt.PingRequest{PingID: 1}), encode(t, &mt.PingRequest{PingID: 2})
	id1, s1 := c.newMsgID(), c.nextSeq(true)
	id2, s2 := c.newMsgID(), c.nextSeq(true)
	ct := proto.MessageContainer{Messages: []proto.Message{{ID: id1, SeqNo: int(s1), Bytes: len(p1), Body: p1}, {ID: id2, SeqNo: int(s2), Bytes: len(p2), Body: p2}}}
	c.sendRaw(c.salt, c.newMsgID(), c.nextSeq(false), encode(t, &ct))
	seen := map[int64]bool{}
	for i := 0; i < 2; i++ {
		var p mt.Pong
		p.Decode(&bin.Buffer{Buf: c.next(mt.PongTypeID).body})
		seen[p.PingID] = true
	}
	if !seen[1] || !seen[2] {
		t.Fatalf("pongs %v", seen)
	}
	// A container inside a container is refused.
	inner := encode(t, &ct)
	nid, ns := c.newMsgID(), c.nextSeq(false)
	outer := proto.MessageContainer{Messages: []proto.Message{{ID: nid, SeqNo: int(ns), Bytes: len(inner), Body: inner}}}
	c.sendRaw(c.salt, c.newMsgID(), c.nextSeq(false), encode(t, &outer))
	if b := badMsg(t, c); b.ErrorCode != badContainerInvalid {
		t.Fatalf("nested container: %+v", b)
	}
}

func TestHandshakeRateLimit(t *testing.T) {
	ts := startServer(t, func(c *Config) { c.HandshakesPerMin = 2 })
	for i := 0; i < 2; i++ {
		dialRaw(t, ts.addr, false).mustKey()
	}
	if err := dialRaw(t, ts.addr, false).makeKey(true); err == nil {
		t.Fatal("a third exchange within a minute was allowed with HandshakesPerMin 2")
	}
}
