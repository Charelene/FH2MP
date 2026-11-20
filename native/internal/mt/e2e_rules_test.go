package mt

import (
	"bytes"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/hex"
	"math/big"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gotd/td/bin"
	"github.com/gotd/td/mt"

	"fh2mp/internal/diag"
	"fh2mp/internal/mt/e2e"
	"fh2mp/internal/mt/tl"
)

// openChat runs the whole exchange: a requests, b gets the request pushed and checks g_a_sig against a's device key like a client, accepts,
// and a gets the accept pushed; both make the key.
func openChat(t *testing.T, et *e2eTest, a, b *e2eUser) *chatPair {
	t.Helper()
	req, w := a.request(t, b, int32(randInt64()))
	wait, ok := w.(*tl.EncryptedChatWaiting)
	if !ok || wait.AdminID != a.x || wait.ParticipantID != b.x || wait.ParticipantDevice != b.device || wait.ID <= 0 {
		t.Fatalf("request answered %#v", w)
	}
	ur, ok := b.pushed(t).Updates[0].(*tl.UpdateEncryption).Chat.(*tl.EncryptedChatRequested)
	if !ok || ur.ID != wait.ID || !bytes.Equal(ur.GA, req.GA) || !bytes.Equal(ur.GASig, req.Sig) || ur.AdminDevice != a.device {
		t.Fatalf("request pushed %#v", ur)
	}
	ends := e2e.Ends{Admin: ur.AdminID, AdminDevice: ur.AdminDevice, Participant: ur.ParticipantID, ParticipantDevice: ur.ParticipantDevice}
	acc, err := e2e.AcceptRequest(b.dev, a.devPub(), int32(ur.ID), ends, ur.GA, ur.GASig, randBytes(256), nil)
	if err != nil {
		t.Fatal(err)
	}
	var ch tl.EncryptedChatBox
	b.c.call(&tl.MessagesAcceptEncryptionRequest{ChatID: ur.ID, GB: acc.GB, KeyFingerprint: acc.Fingerprint, GBSig: acc.Sig}, &ch)
	bc, ok := ch.EncryptedChat.(*tl.EncryptedChat)
	if !ok || !bytes.Equal(bc.GAOrB, req.GA) || !bytes.Equal(bc.Sig, req.Sig) || bc.KeyFingerprint != acc.Fingerprint {
		t.Fatalf("accept answered %#v", ch.EncryptedChat)
	}
	ua, ok := a.pushed(t).Updates[0].(*tl.UpdateEncryption).Chat.(*tl.EncryptedChat)
	if !ok || !bytes.Equal(ua.GAOrB, acc.GB) || !bytes.Equal(ua.Sig, acc.Sig) {
		t.Fatalf("accept pushed %#v", ua)
	}
	key, err := e2e.FinishRequest(req, b.devPub(), int32(ua.ID), ends, ua.GAOrB, ua.KeyFingerprint, ua.Sig)
	if err != nil {
		t.Fatal(err)
	}
	id := int32(ua.ID)
	return &chatPair{id: id, key: key, fp: ua.KeyFingerprint, a: e2e.NewChat(id, e2e.Admin, a.x, b.x, key, et.clock.now),
		b: e2e.NewChat(id, e2e.Participant, b.x, a.x, acc.Key, et.clock.now)}
}

func (u *e2eUser) sendSealed(t *testing.T, chat int32, o e2e.Outgoing) *tl.MessagesSentEncryptedMessage {
	t.Helper()
	var r tl.MessagesSentEncryptedMessage
	u.c.call(&tl.MessagesSendEncryptedRequest{ChatID: int(chat), RandomID: o.RandomID, Data: o.Data, Commitment: o.Commitment}, &r)
	return &r
}

func (u *e2eUser) sendErr(chat int, data, commitment []byte) *mt.RPCError {
	return u.c.callErr(&tl.MessagesSendEncryptedRequest{ChatID: chat, RandomID: randInt64(), Data: data, Commitment: commitment})
}

func mustSeal(t *testing.T, c *e2e.Chat, text string) e2e.Outgoing {
	t.Helper()
	out, err := c.Send(text)
	if err != nil || len(out) != 1 {
		t.Fatalf("seal %q: %d messages %v", text, len(out), err)
	}
	return out[0]
}

// newMessages are the sealed messages a difference carries.
func newMessages(d tl.UpdatesDifferenceClass) []tl.EncryptedMessage {
	var upds []tl.UpdateClass
	switch v := d.(type) {
	case *tl.UpdatesDifference:
		upds = v.NewUpdates
	case *tl.UpdatesDifferenceSlice:
		upds = v.NewUpdates
	}
	var out []tl.EncryptedMessage
	for _, u := range upds {
		if m, ok := u.(*tl.UpdateNewEncryptedMessage); ok {
			out = append(out, m.Message)
		}
	}
	return out
}

// boolResult decodes a Bool answer.
type boolResult bool

func (b *boolResult) Decode(buf *bin.Buffer) error {
	var box tl.BoolBox
	if err := box.Decode(buf); err != nil {
		return err
	}
	_, ok := box.Bool.(*tl.BoolTrue)
	*b = boolResult(ok)
	return nil
}

// waitDiscarded reads u's pushes until the discard of chat arrives.
func (u *e2eUser) waitDiscarded(t *testing.T, chat int32) {
	t.Helper()
	for {
		for _, up := range u.pushed(t).Updates {
			if ue, ok := up.(*tl.UpdateEncryption); ok {
				if d, ok := ue.Chat.(*tl.EncryptedChatDiscarded); ok && d.ID == int(chat) {
					return
				}
			}
		}
	}
}

func TestE2EDevices(t *testing.T) {
	et := startE2E(t)
	const ax, bx, cx = 0x9000000000501, 0x9000000000502, 0x9000000000503
	a, b, c := et.user(t, ax, "A", 11), et.user(t, bx, "B", 22), et.user(t, cx, "C", 33)
	et.friends.befriend(ax, bx)
	// A certificate for another realm, another device id or by another key, or of a short key, is refused.
	for name, sig := range map[string][]byte{
		"another realm":     e2e.SignDevice(a.acct, "prod", a.device, a.devPub()),
		"another device id": e2e.SignDevice(a.acct, "test", a.device+1, a.devPub()),
		"another account":   e2e.SignDevice(b.acct, "test", a.device, a.devPub()),
	} {
		e := a.c.callErr(&tl.AccountRegisterDeviceRequest{Pubkey: a.devPub(), Signature: sig})
		if e.ErrorMessage != "DEVICE_SIGNATURE_INVALID" {
			t.Fatalf("%s: %+v", name, e)
		}
	}
	expectErr(t, a.c.callErr(&tl.AccountRegisterDeviceRequest{Pubkey: a.devPub()[:31], Signature: e2e.SignDevice(a.acct, "test", a.device, a.devPub())}),
		"DEVICE_SIGNATURE_INVALID")
	d := a.register(t)
	if d.UserID != ax || d.DeviceID != 11 || !bytes.Equal(d.Pubkey, a.devPub()) || !bytes.Equal(d.AccountKey, a.acct.Public().(ed25519.PublicKey)) ||
		!e2e.VerifyDevice(d.AccountKey, "test", d.DeviceID, d.Pubkey, d.Signature) {
		t.Fatalf("registered %+v", d)
	}
	// The same key again changes nothing.
	et.clock.add(5 * time.Second)
	if again := a.register(t); again.Date != d.Date {
		t.Fatalf("re-registering the same key changed the date %d -> %d", d.Date, again.Date)
	}
	var ds, none, own tl.UsersDevices
	b.c.call(&tl.UsersGetDevicesRequest{UserID: ax}, &ds)
	if len(ds.Devices) != 1 || ds.Devices[0].DeviceID != 11 {
		t.Fatalf("B sees A's devices %+v", ds)
	}
	a.c.call(&tl.UsersGetDevicesRequest{UserID: bx}, &none)
	if len(none.Devices) != 0 {
		t.Fatalf("B has no device yet: %+v", none)
	}
	a.c.call(&tl.UsersGetDevicesRequest{UserID: ax}, &own)
	if len(own.Devices) != 1 {
		t.Fatal("own device not listed")
	}
	expectErr(t, c.c.callErr(&tl.UsersGetDevicesRequest{UserID: ax}), "USER_NOT_FRIEND")
}

func TestE2EDhConfig(t *testing.T) {
	et := startE2E(t)
	a := et.user(t, 0x9000000000511, "A", 1)
	var box tl.MessagesDhConfigBox
	a.c.call(&tl.MessagesGetDhConfigRequest{Version: 0, RandomLength: 32}, &box)
	cfg, ok := box.DhConfig.(*tl.MessagesDhConfig)
	if !ok || cfg.G != 2 || cfg.Version != 1 || !bytes.Equal(cfg.P, e2e.PBytes()) || len(cfg.Random) != 32 {
		t.Fatalf("dhConfig %#v", box.DhConfig)
	}
	if new(big.Int).SetBytes(cfg.P).Cmp(e2e.P) != 0 {
		t.Fatal("p is not the pinned prime")
	}
	a.c.call(&tl.MessagesGetDhConfigRequest{Version: 1, RandomLength: 256}, &box)
	if nm, ok := box.DhConfig.(*tl.MessagesDhConfigNotModified); !ok || len(nm.Random) != 256 {
		t.Fatalf("version 1: %#v", box.DhConfig)
	}
	a.c.call(&tl.MessagesGetDhConfigRequest{Version: 1, RandomLength: 0}, &box)
	expectErr(t, a.c.callErr(&tl.MessagesGetDhConfigRequest{Version: 0, RandomLength: 257}), "RANDOM_LENGTH_INVALID")
	expectErr(t, a.c.callErr(&tl.MessagesGetDhConfigRequest{Version: 0, RandomLength: -1}), "RANDOM_LENGTH_INVALID")
}

func TestE2ERequestRules(t *testing.T) {
	et := startE2E(t)
	const ax, bx, cx, dx = 0x9000000000521, 0x9000000000522, 0x9000000000523, 0x9000000000524
	a, b, c, d := et.user(t, ax, "A", 11), et.user(t, bx, "B", 22), et.user(t, cx, "C", 33), et.user(t, dx, "D", 44)
	req := func(u, to *e2eUser, device int64, rid int32, mod func(*tl.MessagesRequestEncryptionRequest)) *mt.RPCError {
		t.Helper()
		ends := e2e.Ends{Admin: u.x, AdminDevice: u.device, Participant: to.x, ParticipantDevice: device}
		r, err := e2e.MakeRequest(u.dev, ends, randBytes(256), nil)
		if err != nil {
			t.Fatal(err)
		}
		m := &tl.MessagesRequestEncryptionRequest{UserID: to.x, DeviceID: device, RandomID: int(rid), GA: r.GA, GASig: r.Sig}
		if mod != nil {
			mod(m)
		}
		return u.c.callErr(m)
	}
	et.friends.befriend(ax, bx)
	b.register(t)
	// The caller needs a registered device.
	expectErr(t, req(a, b, b.device, 1, nil), "DEVICE_NOT_REGISTERED")
	a.register(t)
	// Friendship is needed: C is nobody's friend; nobody chats with themselves.
	c.register(t)
	expectErr(t, req(a, c, c.device, 2, nil), "USER_NOT_FRIEND")
	expectErr(t, req(a, a, a.device, 2, nil), "USER_NOT_FRIEND")
	// The participant's current device only; a friend without one cannot be asked.
	expectErr(t, req(a, b, b.device+1, 3, nil), "DEVICE_CHANGED")
	et.friends.befriend(ax, dx)
	expectErr(t, req(a, d, d.device, 3, nil), "DEVICE_CHANGED")
	// g_a in range, 256 bytes, signed by the caller's device over the request's ends.
	expectErr(t, req(a, b, b.device, 4, func(m *tl.MessagesRequestEncryptionRequest) { m.GA = e2e.Num(big.NewInt(1)) }), "DH_G_A_INVALID")
	expectErr(t, req(a, b, b.device, 4, func(m *tl.MessagesRequestEncryptionRequest) { m.GA = m.GA[1:] }), "DH_G_A_INVALID")
	expectErr(t, req(a, b, b.device, 4, func(m *tl.MessagesRequestEncryptionRequest) {
		r, _ := e2e.MakeRequest(a.dev, e2e.Ends{Admin: ax, AdminDevice: 11, Participant: bx, ParticipantDevice: 22}, randBytes(256), nil)
		m.GASig = r.Sig // a signature over another g_a
	}), "DH_SIGNATURE_INVALID")
	expectErr(t, req(a, b, b.device, 4, func(m *tl.MessagesRequestEncryptionRequest) { m.GASig = m.GASig[:63] }), "DH_SIGNATURE_INVALID")
	// A session of A on another device than the registered one cannot request.
	a2 := dialRaw(t, et.addr, false).mustKey().init()
	if _, e := a2.signIn(a.acct, 99); e != nil {
		t.Fatal(e)
	}
	expectErr(t, req(&e2eUser{c: a2, x: ax, acct: a.acct, dev: a.dev, device: 99}, b, b.device, 6, nil), "DEVICE_NOT_REGISTERED")
	// Back on A's device: a request goes through, and the same random_id with the same g_a answers the same chat without a second update.
	a3 := dialRaw(t, et.addr, false).mustKey().init()
	if _, e := a3.signIn(a.acct, 11); e != nil {
		t.Fatal(e)
	}
	a.c = a3
	r, _ := e2e.MakeRequest(a.dev, e2e.Ends{Admin: ax, AdminDevice: 11, Participant: bx, ParticipantDevice: 22}, randBytes(256), nil)
	m := &tl.MessagesRequestEncryptionRequest{UserID: bx, DeviceID: 22, RandomID: 7, GA: r.GA, GASig: r.Sig}
	var w1, w2 tl.EncryptedChatBox
	a.c.call(m, &w1)
	before := b.state(t)
	a.c.call(m, &w2)
	if w1.EncryptedChat.(*tl.EncryptedChatWaiting).ID != w2.EncryptedChat.(*tl.EncryptedChatWaiting).ID {
		t.Fatal("a repeated random_id made a second chat")
	}
	if after := b.state(t); after != before {
		t.Fatalf("a repeated request stored another update for B: pts %d -> %d", before, after)
	}
}

func TestE2EAcceptAndRelay(t *testing.T) {
	et := startE2E(t)
	const ax, bx, cx = 0x9000000000531, 0x9000000000532, 0x9000000000533
	a, b, c := et.user(t, ax, "A", 11), et.user(t, bx, "B", 22), et.user(t, cx, "C", 33)
	et.friends.befriend(ax, bx)
	et.friends.befriend(cx, bx)
	a.register(t)
	b.register(t)
	c.register(t)

	req, w := a.request(t, b, 41)
	id := w.(*tl.EncryptedChatWaiting).ID
	b.pushed(t)
	ends := e2e.Ends{Admin: ax, AdminDevice: 11, Participant: bx, ParticipantDevice: 22}
	acc, err := e2e.AcceptRequest(b.dev, a.devPub(), int32(id), ends, req.GA, req.Sig, randBytes(256), nil)
	if err != nil {
		t.Fatal(err)
	}
	good := &tl.MessagesAcceptEncryptionRequest{ChatID: id, GB: acc.GB, KeyFingerprint: acc.Fingerprint, GBSig: acc.Sig}
	// Only the participant accepts; the chat must exist; g_b in range and signed by the participant's device.
	expectErr(t, a.c.callErr(good), "ENCRYPTION_ID_INVALID")
	expectErr(t, c.c.callErr(good), "ENCRYPTION_ID_INVALID")
	expectErr(t, b.c.callErr(&tl.MessagesAcceptEncryptionRequest{ChatID: id + 1000, GB: acc.GB, KeyFingerprint: acc.Fingerprint, GBSig: acc.Sig}), "ENCRYPTION_ID_INVALID")
	expectErr(t, b.c.callErr(&tl.MessagesAcceptEncryptionRequest{ChatID: id, GB: e2e.PBytes(), KeyFingerprint: acc.Fingerprint, GBSig: acc.Sig}), "DH_G_B_INVALID")
	expectErr(t, b.c.callErr(&tl.MessagesAcceptEncryptionRequest{ChatID: id, GB: acc.GB, KeyFingerprint: acc.Fingerprint + 1, GBSig: acc.Sig}), "DH_SIGNATURE_INVALID")
	// Nothing can be sent before the accept.
	expectErr(t, a.sendErr(id, make([]byte, 64), make([]byte, 32)), "ENCRYPTION_ID_INVALID")
	var ch tl.EncryptedChatBox
	b.c.call(good, &ch)
	expectErr(t, b.c.callErr(good), "ENCRYPTION_ALREADY_ACCEPTED")
	ua := a.pushed(t).Updates[0].(*tl.UpdateEncryption).Chat.(*tl.EncryptedChat)
	key, err := e2e.FinishRequest(req, b.devPub(), int32(id), ends, ua.GAOrB, ua.KeyFingerprint, ua.Sig)
	if err != nil {
		t.Fatal(err)
	}
	ca := e2e.NewChat(int32(id), e2e.Admin, ax, bx, key, et.clock.now)
	cb := e2e.NewChat(int32(id), e2e.Participant, bx, ax, acc.Key, et.clock.now)

	// A seals, the server relays it to B with pts, B opens it.
	o := mustSeal(t, ca, "first secret")
	sent := a.sendSealed(t, int32(id), o)
	msg := b.pushed(t).Updates[0].(*tl.UpdateNewEncryptedMessage).Message
	if msg.ChatID != id || msg.RandomID != o.RandomID || msg.Date != sent.Date || !bytes.Equal(msg.Bytes, o.Data) || !bytes.Equal(msg.Commitment, o.Commitment) {
		t.Fatalf("relayed %+v", msg)
	}
	evs, _, err := cb.Receive(msg.Bytes)
	if err != nil || len(evs) != 1 || evs[0].Text != "first secret" {
		t.Fatalf("B opened %+v %v", evs, err)
	}
	// The same random_id again: the first date, nothing stored.
	before := b.state(t)
	et.clock.add(10 * time.Second)
	if again := a.sendSealed(t, int32(id), o); again.Date != sent.Date {
		t.Fatalf("repeat answered date %d, first %d", again.Date, sent.Date)
	}
	if b.state(t) != before {
		t.Fatal("a repeated random_id was stored again")
	}
	// The 16 KB cap and the commitment length; a third party cannot send on the chat.
	expectErr(t, a.sendErr(id, make([]byte, e2e.MaxData+1), make([]byte, 32)), "MESSAGE_TOO_LONG")
	var full tl.MessagesSentEncryptedMessage
	a.c.call(&tl.MessagesSendEncryptedRequest{ChatID: id, RandomID: randInt64(), Data: make([]byte, e2e.MaxData), Commitment: make([]byte, 32)}, &full)
	if got := b.pushed(t).Updates[0].(*tl.UpdateNewEncryptedMessage).Message; len(got.Bytes) != e2e.MaxData {
		t.Fatalf("16384 bytes relayed as %d", len(got.Bytes))
	}
	expectErr(t, a.sendErr(id, make([]byte, 64), make([]byte, 31)), "COMMITMENT_INVALID")
	expectErr(t, a.sendErr(id, make([]byte, 64), make([]byte, 33)), "COMMITMENT_INVALID")
	expectErr(t, c.sendErr(id, make([]byte, 64), make([]byte, 32)), "ENCRYPTION_ID_INVALID")

	// B reports A's message with its text and franking key: on the operators' list. A wrong text or key, or the sender reporting its own
	// message, does not match.
	report := func(u *e2eUser, text string, fk []byte) *mt.RPCError {
		var ok boolResult
		body, e := u.c.result(u.c.send(&tl.MessagesReportEncryptedRequest{ChatID: id, RandomID: o.RandomID, Text: text, FrankingKey: fk, Reason: "spam"}))
		if e == nil {
			if err := ok.Decode(&bin.Buffer{Buf: body}); err != nil || !ok {
				t.Fatalf("report answered %x", body)
			}
		}
		return e
	}
	expectErr(t, report(b, "first secret!", evs[0].FrankingKey), "FRANKING_MISMATCH")
	expectErr(t, report(b, "first secret", randBytes(32)), "FRANKING_MISMATCH")
	expectErr(t, report(b, "first secret", evs[0].FrankingKey[:31]), "FRANKING_MISMATCH")
	expectErr(t, report(a, "first secret", evs[0].FrankingKey), "FRANKING_MISMATCH")
	if rs, _ := et.s.Reports(10); len(rs) != 0 {
		t.Fatalf("a mismatch reached the list: %+v", rs)
	}
	if e := report(b, "first secret", evs[0].FrankingKey); e != nil {
		t.Fatalf("matching report refused: %+v", e)
	}
	rs, _ := et.s.Reports(10)
	if len(rs) != 1 || rs[0].Reporter != bx || rs[0].Sender != ax || rs[0].Text != "first secret" || rs[0].Reason != "spam" || rs[0].RandomID != o.RandomID ||
		rs[0].Chat != int32(id) {
		t.Fatalf("report list %+v", rs)
	}
	// B answers; A opens it.
	r := mustSeal(t, cb, "echo: first secret")
	b.sendSealed(t, int32(id), r)
	am := a.pushed(t).Updates[0].(*tl.UpdateNewEncryptedMessage).Message
	if evs, _, err := ca.Receive(am.Bytes); err != nil || len(evs) != 1 || evs[0].Text != "echo: first secret" {
		t.Fatalf("A opened %+v %v", evs, err)
	}
	// A discarded chat cannot be accepted again or sent on; discarding it again is harmless; a stranger cannot discard it.
	var done boolResult
	a.c.call(&tl.MessagesDiscardEncryptionRequest{ChatID: id}, &done)
	a.c.call(&tl.MessagesDiscardEncryptionRequest{ChatID: id}, &done)
	if !done {
		t.Fatal("discard answered false")
	}
	expectErr(t, b.c.callErr(good), "ENCRYPTION_DECLINED")
	expectErr(t, b.sendErr(id, make([]byte, 64), make([]byte, 32)), "ENCRYPTION_ID_INVALID")
	expectErr(t, c.c.callErr(&tl.MessagesDiscardEncryptionRequest{ChatID: id}), "ENCRYPTION_ID_INVALID")
}

// A discard drops the sealed messages still waiting: B was away, comes back and finds the discard, not the messages.
func TestE2EDiscardDropsEnvelopes(t *testing.T) {
	et := startE2E(t)
	const ax, bx = 0x9000000000541, 0x9000000000542
	a, b := et.user(t, ax, "A", 11), et.user(t, bx, "B", 22)
	et.friends.befriend(ax, bx)
	a.register(t)
	b.register(t)
	cp := openChat(t, et, a, b)
	from := b.state(t)
	a.sendSealed(t, cp.id, mustSeal(t, cp.a, "one"))
	a.sendSealed(t, cp.id, mustSeal(t, cp.a, "two"))
	if n := len(newMessages(b.difference(t, from))); n != 2 {
		t.Fatalf("%d messages waiting before the discard", n)
	}
	var done boolResult
	a.c.call(&tl.MessagesDiscardEncryptionRequest{ChatID: int(cp.id)}, &done)
	d := b.difference(t, from)
	if n := len(newMessages(d)); n != 0 {
		t.Fatalf("%d sealed messages handed out after the discard", n)
	}
	full, ok := d.(*tl.UpdatesDifference)
	if !ok || len(full.NewUpdates) != 1 {
		t.Fatalf("difference after the discard %#v", d)
	}
	if u, ok := full.NewUpdates[0].(*tl.UpdateEncryption); !ok || u.Chat.(*tl.EncryptedChatDiscarded).ID != int(cp.id) {
		t.Fatalf("B's update %#v", full.NewUpdates[0])
	}
	a.waitDiscarded(t, cp.id)
}

// messages.ackEncrypted deletes the bytes: an acked push alone keeps them (a client that lost them asks again from its pts), the ack drops
// them and a difference from before does not hand them back.
func TestE2EAckDeletesBytes(t *testing.T) {
	et := startE2E(t)
	const ax, bx = 0x9000000000551, 0x9000000000552
	a, b := et.user(t, ax, "A", 11), et.user(t, bx, "B", 22)
	et.friends.befriend(ax, bx)
	a.register(t)
	b.register(t)
	cp := openChat(t, et, a, b)
	from := b.state(t)
	a.sendSealed(t, cp.id, mustSeal(t, cp.a, "keep me"))
	m := b.c.next(tl.UpdatesTypeID)
	var p tl.Updates
	if err := p.Decode(&bin.Buffer{Buf: m.body}); err != nil {
		t.Fatal(err)
	}
	b.c.ack(m.id)
	b.c.send(&mt.PingRequest{PingID: 1})
	b.c.next(mt.PongTypeID)
	if n := len(newMessages(b.difference(t, from))); n != 1 {
		t.Fatalf("after the transport ack: %d messages from pts %d, 1 expected", n, from)
	}
	var done boolResult
	b.c.call(&tl.MessagesAckEncryptedRequest{MaxPts: p.Pts}, &done)
	if !done {
		t.Fatal("ackEncrypted answered false")
	}
	if rows, _, _ := et.store.Since(bx, from, 10); len(rows) != 0 {
		t.Fatalf("%d rows kept after ackEncrypted", len(rows))
	}
	d := b.difference(t, from)
	if n := len(newMessages(d)); n != 0 {
		t.Fatalf("deleted bytes handed back: %#v", d)
	}
	if _, ok := d.(*tl.UpdatesDifferenceEmpty); !ok {
		t.Fatalf("difference after the ack %#v", d)
	}
}

// A new device discards every chat of the account (both ends told, waiting messages dropped) and tells its friends with updateChatDevice.
func TestE2ENewDeviceDiscardsChats(t *testing.T) {
	et := startE2E(t)
	const ax, bx = 0x9000000000561, 0x9000000000562
	a, b := et.user(t, ax, "A", 11), et.user(t, bx, "B", 22)
	et.friends.befriend(ax, bx)
	a.register(t)
	b.register(t)
	cp := openChat(t, et, a, b)
	a.sendSealed(t, cp.id, mustSeal(t, cp.a, "to the old PC"))
	b.pushed(t)
	// B signs in on another PC (device 23) and registers it there.
	b2c := dialRaw(t, et.addr, false).mustKey().init()
	if _, e := b2c.signIn(b.acct, 23); e != nil {
		t.Fatal(e)
	}
	_, dev2, _ := ed25519.GenerateKey(rand.Reader)
	b2 := &e2eUser{c: b2c, x: bx, acct: b.acct, dev: dev2, device: 23}
	b2.register(t)
	if u, ok := a.short(t).(*tl.UpdateChatDevice); !ok || u.UserID != bx || u.DeviceID != 23 {
		t.Fatalf("A was told %#v", u)
	}
	a.waitDiscarded(t, cp.id)
	b2.waitDiscarded(t, cp.id)
	if c, _ := et.chats.Chat(cp.id); c == nil || c.State != ChatDiscarded {
		t.Fatalf("chat after the new device: %+v", c)
	}
	rows, _, _ := et.store.Since(bx, 0, 100)
	for _, r := range rows {
		u, _ := tl.DecodeUpdate(&bin.Buffer{Buf: r.Body})
		if _, ok := u.(*tl.UpdateNewEncryptedMessage); ok {
			t.Fatal("a sealed message for the old device is still stored")
		}
	}
	// A's request to B's old device is refused; to the new one it goes through.
	r, _ := e2e.MakeRequest(a.dev, e2e.Ends{Admin: ax, AdminDevice: 11, Participant: bx, ParticipantDevice: 22}, randBytes(256), nil)
	expectErr(t, a.c.callErr(&tl.MessagesRequestEncryptionRequest{UserID: bx, DeviceID: 22, RandomID: 9, GA: r.GA, GASig: r.Sig}), "DEVICE_CHANGED")
	r, _ = e2e.MakeRequest(a.dev, e2e.Ends{Admin: ax, AdminDevice: 11, Participant: bx, ParticipantDevice: 23}, randBytes(256), nil)
	var w tl.EncryptedChatBox
	a.c.call(&tl.MessagesRequestEncryptionRequest{UserID: bx, DeviceID: 23, RandomID: 9, GA: r.GA, GASig: r.Sig}, &w)
	if _, ok := w.EncryptedChat.(*tl.EncryptedChatWaiting); !ok {
		t.Fatalf("request to the new device: %#v", w.EncryptedChat)
	}
}

// Removing a friend discards their chats; a new request needs the friendship again.
func TestE2EFriendRemovalDiscards(t *testing.T) {
	et := startE2E(t)
	const ax, bx = 0x9000000000571, 0x9000000000572
	a, b := et.user(t, ax, "A", 11), et.user(t, bx, "B", 22)
	et.friends.befriend(ax, bx)
	a.register(t)
	b.register(t)
	cp := openChat(t, et, a, b)
	from := b.state(t)
	a.sendSealed(t, cp.id, mustSeal(t, cp.a, "bye"))
	et.friends.unfriend(ax, bx)
	et.s.FriendshipEnded(bx, ax)
	if c, _ := et.chats.Chat(cp.id); c.State != ChatDiscarded {
		t.Fatalf("chat state %s after the friendship ended", c.State)
	}
	if n := len(newMessages(b.difference(t, from))); n != 0 {
		t.Fatalf("%d sealed messages left after the friendship ended", n)
	}
	a.waitDiscarded(t, cp.id)
	r, _ := e2e.MakeRequest(a.dev, e2e.Ends{Admin: ax, AdminDevice: 11, Participant: bx, ParticipantDevice: 22}, randBytes(256), nil)
	expectErr(t, a.c.callErr(&tl.MessagesRequestEncryptionRequest{UserID: bx, DeviceID: 22, RandomID: 10, GA: r.GA, GASig: r.Sig}), "USER_NOT_FRIEND")
}

// The sweeper: sealed bytes go after 7 days, commitments after 30 (a report then fails).
func TestE2ESweeper(t *testing.T) {
	et := startE2E(t)
	const ax, bx = 0x9000000000581, 0x9000000000582
	a, b := et.user(t, ax, "A", 11), et.user(t, bx, "B", 22)
	et.friends.befriend(ax, bx)
	a.register(t)
	b.register(t)
	cp := openChat(t, et, a, b)
	from := b.state(t)
	o := mustSeal(t, cp.a, "old news")
	a.sendSealed(t, cp.id, o)
	b.pushed(t)
	evs, _, err := cp.b.Receive(o.Data)
	if err != nil || len(evs) != 1 {
		t.Fatal(err)
	}
	et.s.SweepStores(et.later.now())
	if n := len(newMessages(b.difference(t, from))); n != 1 {
		t.Fatalf("a fresh message was swept: %d left", n)
	}
	et.later.add(UpdateTTL + time.Minute)
	et.s.SweepStores(et.later.now())
	if rows, _, _ := et.store.Since(bx, from, 10); len(rows) != 0 {
		t.Fatalf("%d rows older than 7 days kept", len(rows))
	}
	rep := &tl.MessagesReportEncryptedRequest{ChatID: int(cp.id), RandomID: o.RandomID, Text: "old news", FrankingKey: evs[0].FrankingKey, Reason: "x"}
	var done boolResult
	b.c.call(rep, &done) // the commitment outlives the bytes
	et.later.add(SentTTL)
	et.s.SweepStores(et.later.now())
	expectErr(t, b.c.callErr(rep), "FRANKING_MISMATCH")
}

// No log line of a whole chat carries a key, a secret, a plaintext, a ciphertext or a report's text.
func TestE2ELogsCarryNoSecrets(t *testing.T) {
	var mu sync.Mutex
	var lines []string
	diag.SetSink(func(level int, sub, msg string) {
		mu.Lock()
		lines = append(lines, msg)
		mu.Unlock()
	})
	t.Cleanup(func() { diag.SetSink(nil) })
	et := startE2E(t)
	const ax, bx = 0x9000000000591, 0x9000000000592
	a, b := et.user(t, ax, "A", 11), et.user(t, bx, "B", 22)
	et.friends.befriend(ax, bx)
	a.register(t)
	b.register(t)
	cp := openChat(t, et, a, b)
	const secret = "the password is swordfish"
	o := mustSeal(t, cp.a, secret)
	a.sendSealed(t, cp.id, o)
	evs, _, _ := cp.b.Receive(b.pushed(t).Updates[0].(*tl.UpdateNewEncryptedMessage).Message.Bytes)
	var done boolResult
	b.c.call(&tl.MessagesReportEncryptedRequest{ChatID: int(cp.id), RandomID: o.RandomID, Text: secret, FrankingKey: evs[0].FrankingKey, Reason: "a reason"}, &done)
	b.c.callErr(&tl.MessagesReportEncryptedRequest{ChatID: int(cp.id), RandomID: o.RandomID, Text: secret + "!", FrankingKey: evs[0].FrankingKey, Reason: "a reason"})
	a.c.callErr(&tl.MessagesSendEncryptedRequest{ChatID: int(cp.id), RandomID: 1, Data: o.Data, Commitment: o.Commitment[:5]})
	a.c.call(&tl.MessagesDiscardEncryptionRequest{ChatID: int(cp.id)}, &done)
	mu.Lock()
	defer mu.Unlock()
	if len(lines) < 5 {
		t.Fatalf("only %d log lines captured", len(lines))
	}
	needles := map[string]string{"plaintext": secret, "reason": "a reason", "key": hex.EncodeToString(cp.key[:16]), "data": hex.EncodeToString(o.Data[24:40]),
		"commitment": hex.EncodeToString(o.Commitment[:16]), "franking key": hex.EncodeToString(evs[0].FrankingKey[:16]),
		"device key": hex.EncodeToString(a.devPub()[:16])}
	for _, l := range lines {
		low := strings.ToLower(l)
		for what, n := range needles {
			if strings.Contains(low, strings.ToLower(n)) {
				t.Errorf("a log line carries the %s: %s", what, l)
			}
		}
	}
}
