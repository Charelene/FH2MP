package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/pem"
	"fmt"
	"strings"
	"testing"
	"time"

	"fh2mp/internal/mt"
	"fh2mp/internal/mt/e2e"
	"fh2mp/internal/mt/tl"
)

// you is the account under test, as an in-process account: it does what the launcher does, by the e2e reference.
type you struct {
	t     *testing.T
	l     *mt.Local
	x     int64
	dev   ed25519.PrivateKey
	chats map[int32]*e2e.Chat
	reqs  map[int32]*e2e.Request
}

const youX, youDevice = 0x9000000010001, 0x4242

func startDemo(t *testing.T) (*demo, *you) {
	k, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	key, err := mt.ParseServerKey(pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(k)}))
	if err != nil {
		t.Fatal(err)
	}
	d := newDemo("test")
	s, err := mt.New(mt.Config{Realm: "test"}, key, d, mt.NewMemStore())
	if err != nil {
		t.Fatal(err)
	}
	s.SetChatStore(mt.NewMemChatStore())
	d.s = s
	s.SetCommunity(d)
	if d.peer, err = newPeer(s, d.friend("DebugFox"), "test"); err != nil {
		t.Fatal(err)
	}
	_, acct, _ := ed25519.GenerateKey(rand.Reader)
	_, dev, _ := ed25519.GenerateKey(rand.Reader)
	u := &you{t: t, l: s.NewLocal(youX, youDevice, acct.Public().(ed25519.PublicKey)), x: youX, dev: dev, chats: map[int32]*e2e.Chat{},
		reqs: map[int32]*e2e.Request{}}
	devPub := dev.Public().(ed25519.PublicKey)
	if _, err := u.l.RegisterDevice(devPub, e2e.SignDevice(acct, "test", youDevice, devPub)); err != nil {
		t.Fatal(err)
	}
	return d, u
}

// peerDevice is DebugFox's device as a client pins it: the certificate checked against its account key.
func (u *you) peerDevice(id int64) tl.ChatDevice {
	ds, err := u.l.Devices(id)
	if err != nil || len(ds.Devices) != 1 {
		u.t.Fatalf("DebugFox's devices: %+v %v", ds, err)
	}
	dv := ds.Devices[0]
	if !e2e.VerifyDevice(dv.AccountKey, "test", dv.DeviceID, dv.Pubkey, dv.Signature) {
		u.t.Fatal("DebugFox's device certificate does not verify")
	}
	return dv
}

// next waits for stored updates and handles them like a client; it returns the texts taken.
func (u *you) next(peer tl.ChatDevice) []string {
	u.t.Helper()
	select {
	case <-u.l.Wake:
	case <-time.After(5 * time.Second):
		u.t.Fatal("no update arrived")
	}
	ups, err := u.l.Updates()
	if err != nil {
		u.t.Fatal(err)
	}
	var texts []string
	for _, up := range ups {
		switch up := up.(type) {
		case *tl.UpdateEncryption:
			switch c := up.Chat.(type) {
			case *tl.EncryptedChat:
				req := u.reqs[int32(c.ID)]
				ends := e2e.Ends{Admin: c.AdminID, AdminDevice: c.AdminDevice, Participant: c.ParticipantID, ParticipantDevice: c.ParticipantDevice}
				key, err := e2e.FinishRequest(req, peer.Pubkey, int32(c.ID), ends, c.GAOrB, c.KeyFingerprint, c.Sig)
				if err != nil {
					u.t.Fatalf("DebugFox's accept: %v", err)
				}
				u.chats[int32(c.ID)] = e2e.NewChat(int32(c.ID), e2e.Admin, u.x, c.ParticipantID, key, nil)
			case *tl.EncryptedChatRequested:
				ends := e2e.Ends{Admin: c.AdminID, AdminDevice: c.AdminDevice, Participant: c.ParticipantID, ParticipantDevice: c.ParticipantDevice}
				if c.AdminDevice != peer.DeviceID {
					u.t.Fatalf("request from device %x, pinned %x", c.AdminDevice, peer.DeviceID)
				}
				acc, err := e2e.AcceptRequest(u.dev, peer.Pubkey, int32(c.ID), ends, c.GA, c.GASig, randomBytes(256), nil)
				if err != nil {
					u.t.Fatalf("DebugFox's request: %v", err)
				}
				if _, err := u.l.AcceptEncryption(&tl.MessagesAcceptEncryptionRequest{ChatID: c.ID, GB: acc.GB, KeyFingerprint: acc.Fingerprint, GBSig: acc.Sig}); err != nil {
					u.t.Fatal(err)
				}
				u.chats[int32(c.ID)] = e2e.NewChat(int32(c.ID), e2e.Participant, u.x, c.AdminID, acc.Key, nil)
			case *tl.EncryptedChatDiscarded:
				u.t.Fatalf("chat %d discarded", c.ID)
			}
		case *tl.UpdateNewEncryptedMessage:
			ch := u.chats[int32(up.Message.ChatID)]
			evs, replies, err := ch.Receive(up.Message.Bytes)
			if err != nil {
				u.t.Fatalf("open DebugFox's message: %v", err)
			}
			for _, e := range evs {
				if e.Action == nil {
					if !e2e.CheckCommitment(up.Message.Commitment, e.FrankingKey, ch.Peer, u.x, up.Message.RandomID, e.Text) && e.RandomID == up.Message.RandomID {
						u.t.Fatalf("commitment of %q does not match", e.Text)
					}
					texts = append(texts, e.Text)
				}
			}
			u.relay(int32(up.Message.ChatID), replies)
		}
	}
	return texts
}

func (u *you) relay(id int32, out []e2e.Outgoing) {
	for _, o := range out {
		if _, err := u.l.SendEncrypted(&tl.MessagesSendEncryptedRequest{ChatID: int(id), RandomID: o.RandomID, Data: o.Data, Commitment: o.Commitment}); err != nil {
			u.t.Fatal(err)
		}
	}
}

func randomBytes(n int) []byte {
	b := make([]byte, n)
	rand.Read(b)
	return b
}

// waitTexts reads updates until n texts arrived.
func (u *you) waitTexts(peer tl.ChatDevice, n int) []string {
	u.t.Helper()
	var got []string
	for len(got) < n {
		got = append(got, u.next(peer)...)
	}
	return got
}

// The demo peer starts a chat on `dm DebugFox <text>` (the text arrives once it is accepted, a second one on the same chat), accepts a
// secret chat it is asked for, and answers each sealed message with a sealed echo, through a re-key after 100 messages.
func TestDemoPeer(t *testing.T) {
	d, u := startDemo(t)
	fox := d.friend("DebugFox")
	pd := u.peerDevice(fox.id)
	if pd.DeviceID != peerDeviceID {
		t.Fatalf("DebugFox's device id %x", pd.DeviceID)
	}
	if out := d.peer.do(func() string { return d.peer.dm(u.x, "You", "are you there?") }); !strings.Contains(out, "asked You for secret chat") {
		t.Fatalf("dm: %s", out)
	}
	if got := u.waitTexts(pd, 1); got[0] != "are you there?" {
		t.Fatalf("DebugFox's dm arrived as %q", got)
	}
	if out := d.peer.do(func() string { return d.peer.dm(u.x, "You", "still there?") }); !strings.Contains(out, "sent it on secret chat") {
		t.Fatalf("second dm: %s", out)
	}
	if got := u.waitTexts(pd, 1); got[0] != "still there?" {
		t.Fatalf("second dm arrived as %q", got)
	}
	if out := d.dmCommand(d.friend("Maplefox21378"), "hi"); !strings.Contains(out, "usage") {
		t.Fatalf("dm from a friend without a device: %s", out)
	}

	// You ask DebugFox for a chat; DebugFox accepts; you send, DebugFox echoes, through a re-key.
	ends := e2e.Ends{Admin: u.x, AdminDevice: youDevice, Participant: fox.id, ParticipantDevice: pd.DeviceID}
	req, err := e2e.MakeRequest(u.dev, ends, randomBytes(256), nil)
	if err != nil {
		t.Fatal(err)
	}
	w, err := u.l.RequestEncryption(&tl.MessagesRequestEncryptionRequest{UserID: fox.id, DeviceID: pd.DeviceID, RandomID: 5, GA: req.GA, GASig: req.Sig})
	if err != nil {
		t.Fatal(err)
	}
	id := int32(w.(*tl.EncryptedChatWaiting).ID)
	u.reqs[id] = req
	u.next(pd)
	ch := u.chats[id]
	if ch == nil {
		t.Fatal("DebugFox did not accept")
	}
	fp := ch.Fingerprint()
	for i := 0; i < 60; i++ {
		text := fmt.Sprintf("hello %d ✓", i)
		out, err := ch.Send(text)
		if err != nil {
			t.Fatal(err)
		}
		u.relay(id, out)
		if got := u.waitTexts(pd, 1); got[0] != "echo: "+text {
			t.Fatalf("DebugFox answered %q to %q", got[0], text)
		}
	}
	if ch.Fingerprint() == fp {
		t.Fatal("no re-key after 120 messages")
	}
}
