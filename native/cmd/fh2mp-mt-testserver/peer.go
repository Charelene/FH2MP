package main

// The demo's end-to-end peer: one made-up friend (DebugFox) with a registered chat device that accepts every secret chat request, opens each
// sealed message and answers it with a sealed "echo: <text>", and starts a chat itself on `dm DebugFox <text>`. It is an in-process
// account (mt.Local) under the same server rules as a client, and does what a client does: pins each peer's device on first use (TOFU) and
// checks the device certificate, checks g_a_sig / g_b_sig against the pinned key, keeps the seq numbers, answers Resend and re-keys
// (internal/mt/e2e). Its log lines name chats and sizes, never a key or a text.

import (
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"errors"
	"fmt"
	"strings"

	"fh2mp/internal/diag"
	"fh2mp/internal/mt"
	"fh2mp/internal/mt/e2e"
	"fh2mp/internal/mt/tl"
)

var peerLog = diag.Get("demo")

// The peer's fixed keys (made-up, test realm only): the same every run, so a client's TOFU pin of it holds across restarts.
const peerDeviceID = 0x0d0d0d0d0d0d0d03

func seededKey(seed string) ed25519.PrivateKey {
	h := sha256.Sum256([]byte("fh2mp-mt-testserver demo " + seed))
	return ed25519.NewKeyFromSeed(h[:])
}

type pin struct {
	device int64
	pubkey []byte
}

type peerChat struct {
	with  int64
	req   *e2e.Request // the admin's secret until the accept comes
	ends  e2e.Ends
	chat  *e2e.Chat // nil until the key is made
	queue []string  // texts waiting for the accept
}

type peer struct {
	fr    *demoFriend
	realm string
	local *mt.Local
	dev   ed25519.PrivateKey
	pins  map[int64]pin
	chats map[int32]*peerChat
	cmds  chan func()
}

func newPeer(s *mt.Server, fr *demoFriend, realm string) (*peer, error) {
	acct, dev := seededKey(fr.tag+" account"), seededKey(fr.tag+" device")
	p := &peer{fr: fr, realm: realm, dev: dev, pins: map[int64]pin{}, chats: map[int32]*peerChat{}, cmds: make(chan func())}
	p.local = s.NewLocal(fr.id, peerDeviceID, acct.Public().(ed25519.PublicKey))
	devPub := dev.Public().(ed25519.PublicKey)
	if _, err := p.local.RegisterDevice(devPub, e2e.SignDevice(acct, realm, peerDeviceID, devPub)); err != nil {
		return nil, fmt.Errorf("demo peer %s: register its chat device: %w", fr.tag, err)
	}
	go p.run()
	return p, nil
}

// do runs f on the peer's goroutine and waits for it.
func (p *peer) do(f func() string) string {
	out := make(chan string, 1)
	p.cmds <- func() { out <- f() }
	return <-out
}

func (p *peer) run() {
	for {
		select {
		case <-p.local.Wake:
			p.handleStored()
		case u := <-p.local.Shorts:
			if cd, ok := u.(*tl.UpdateChatDevice); ok {
				// The account is on a new PC: its chats were discarded by the server; the next request pins the new device.
				delete(p.pins, cd.UserID)
				peerLog.Info("demo.e2e_unpinned " + diag.KV(diag.KVP{K: "user", V: fmt.Sprintf("%#x", cd.UserID)}, diag.KVP{K: "device", V: fmt.Sprintf("%016x", uint64(cd.DeviceID))}))
			}
		case f := <-p.cmds:
			f()
		}
	}
}

// device returns user's pinned chat device, pinning it on first use after checking its certificate.
func (p *peer) device(user int64) (pin, error) {
	if pn, ok := p.pins[user]; ok {
		return pn, nil
	}
	ds, err := p.local.Devices(user)
	if err != nil {
		return pin{}, err
	}
	if len(ds.Devices) == 0 {
		return pin{}, errors.New("no chat device registered")
	}
	d := ds.Devices[0]
	if d.UserID != user || !e2e.VerifyDevice(d.AccountKey, p.realm, d.DeviceID, d.Pubkey, d.Signature) {
		return pin{}, errors.New("its device certificate does not verify against its account key")
	}
	pn := pin{device: d.DeviceID, pubkey: d.Pubkey}
	p.pins[user] = pn
	return pn, nil
}

func (p *peer) handleStored() {
	ups, err := p.local.Updates()
	if err != nil {
		peerLog.Exc("demo.e2e_updates_failed", err)
	}
	for _, u := range ups {
		switch u := u.(type) {
		case *tl.UpdateEncryption:
			p.chatUpdate(u.Chat)
		case *tl.UpdateNewEncryptedMessage:
			p.message(u.Message)
		}
	}
}

func (p *peer) chatUpdate(c tl.EncryptedChatClass) {
	switch c := c.(type) {
	case *tl.EncryptedChatRequested:
		if c.ParticipantID != p.fr.id {
			return
		}
		p.accept(c)
	case *tl.EncryptedChat:
		pc := p.chats[int32(c.ID)]
		if pc == nil || pc.req == nil {
			return
		}
		pn, err := p.device(pc.with)
		if err != nil || pn.device != c.ParticipantDevice {
			p.drop(int32(c.ID), "the participant's device is not the pinned one")
			return
		}
		key, err := e2e.FinishRequest(pc.req, pn.pubkey, int32(c.ID), pc.ends, c.GAOrB, c.KeyFingerprint, c.Sig)
		if err != nil {
			p.refused(int32(c.ID), err)
			p.drop(int32(c.ID), "the accept does not verify")
			return
		}
		pc.req, pc.chat = nil, e2e.NewChat(int32(c.ID), e2e.Admin, p.fr.id, pc.with, key, nil)
		peerLog.Info("demo.e2e_chat_open " + diag.KV(diag.KVP{K: "chat", V: c.ID}, diag.KVP{K: "with", V: fmt.Sprintf("%#x", pc.with)}, diag.KVP{K: "role", V: "admin"}))
		for _, text := range pc.queue {
			p.send(int32(c.ID), pc, text)
		}
		pc.queue = nil
	case *tl.EncryptedChatDiscarded:
		if _, ok := p.chats[int32(c.ID)]; ok {
			delete(p.chats, int32(c.ID))
			peerLog.Info("demo.e2e_chat_closed " + diag.KV(diag.KVP{K: "chat", V: c.ID}) + ": discarded")
		}
	}
}

// accept takes a request: the admin's device pinned and checked, g_a_sig checked against it, then b, the key and the signed accept.
func (p *peer) accept(c *tl.EncryptedChatRequested) {
	id := int32(c.ID)
	pn, err := p.device(c.AdminID)
	if err != nil || pn.device != c.AdminDevice {
		why := "the admin's device is not the pinned one"
		if err != nil {
			why = err.Error()
		}
		p.drop(id, why)
		return
	}
	cfg, err := p.local.DhConfig(e2e.GroupVersion, e2e.NumLen)
	if err != nil {
		peerLog.Exc("demo.e2e_accept_failed "+diag.KV(diag.KVP{K: "chat", V: id}), err)
		return
	}
	nm, ok := cfg.(*tl.MessagesDhConfigNotModified)
	if !ok {
		p.drop(id, "the server's DH config is not the pinned group")
		return
	}
	ends := e2e.Ends{Admin: c.AdminID, AdminDevice: c.AdminDevice, Participant: c.ParticipantID, ParticipantDevice: c.ParticipantDevice}
	fresh := make([]byte, e2e.NumLen)
	rand.Read(fresh)
	acc, err := e2e.AcceptRequest(p.dev, pn.pubkey, id, ends, c.GA, c.GASig, fresh, nm.Random)
	if err != nil {
		p.refused(id, err)
		p.drop(id, "the request does not verify")
		return
	}
	if _, err := p.local.AcceptEncryption(&tl.MessagesAcceptEncryptionRequest{ChatID: c.ID, GB: acc.GB, KeyFingerprint: acc.Fingerprint, GBSig: acc.Sig}); err != nil {
		peerLog.Exc("demo.e2e_accept_failed "+diag.KV(diag.KVP{K: "chat", V: id}), err, diag.Warn)
		return
	}
	p.chats[id] = &peerChat{with: c.AdminID, ends: ends, chat: e2e.NewChat(id, e2e.Participant, p.fr.id, c.AdminID, acc.Key, nil)}
	peerLog.Info("demo.e2e_chat_open " + diag.KV(diag.KVP{K: "chat", V: id}, diag.KVP{K: "with", V: fmt.Sprintf("%#x", c.AdminID)}, diag.KVP{K: "role", V: "participant"}))
}

func (p *peer) refused(chat int32, err error) {
	var r *e2e.Refused
	if errors.As(err, &r) {
		peerLog.Warn("chat.e2e_refused " + diag.KV(diag.KVP{K: "chat", V: chat}, diag.KVP{K: "reason", V: r.Reason}))
		return
	}
	peerLog.Exc("chat.e2e_refused "+diag.KV(diag.KVP{K: "chat", V: chat}), err, diag.Warn)
}

func (p *peer) drop(chat int32, why string) {
	delete(p.chats, chat)
	if err := p.local.DiscardEncryption(int(chat)); err != nil {
		peerLog.Exc("demo.e2e_discard_failed "+diag.KV(diag.KVP{K: "chat", V: chat}), err, diag.Warn)
	}
	peerLog.Info("demo.e2e_chat_dropped " + diag.KV(diag.KVP{K: "chat", V: chat}) + ": " + why)
}

// message opens a sealed message in order and answers every text in it with "echo: <text>".
func (p *peer) message(m tl.EncryptedMessage) {
	id := int32(m.ChatID)
	pc := p.chats[id]
	if pc == nil || pc.chat == nil {
		peerLog.Warn("chat.e2e_refused " + diag.KV(diag.KVP{K: "chat", V: id}, diag.KVP{K: "reason", V: "no open chat with this id"}))
		return
	}
	evs, replies, err := pc.chat.Receive(m.Bytes)
	if errors.Is(err, e2e.ErrBroken) {
		p.drop(id, "the peer's sequence numbers broke the chat")
		return
	}
	if err != nil {
		p.refused(id, err)
	}
	p.relay(id, replies)
	for _, ev := range evs {
		if ev.Action == nil {
			peerLog.Debug("demo.e2e_received " + diag.KV(diag.KVP{K: "chat", V: id}, diag.KVP{K: "bytes", V: len(ev.Text)}))
			p.send(id, pc, "echo: "+ev.Text)
		}
	}
}

func (p *peer) send(id int32, pc *peerChat, text string) {
	out, err := pc.chat.Send(text)
	if err != nil {
		peerLog.Exc("demo.e2e_seal_failed "+diag.KV(diag.KVP{K: "chat", V: id}), err, diag.Warn)
		return
	}
	p.relay(id, out)
}

func (p *peer) relay(id int32, out []e2e.Outgoing) {
	for _, o := range out {
		if _, err := p.local.SendEncrypted(&tl.MessagesSendEncryptedRequest{ChatID: int(id), RandomID: o.RandomID, Data: o.Data, Commitment: o.Commitment}); err != nil {
			peerLog.Exc("demo.e2e_send_failed "+diag.KV(diag.KVP{K: "chat", V: id}, diag.KVP{K: "bytes", V: len(o.Data)}), err, diag.Warn)
		}
	}
}

// dm sends text to user on the open chat with them, or starts one (the text goes once it is accepted).
func (p *peer) dm(user int64, tag, text string) string {
	for id, pc := range p.chats {
		if pc.with != user {
			continue
		}
		if pc.chat != nil {
			p.send(id, pc, text)
			return fmt.Sprintf("%s sent it on secret chat %d", p.fr.tag, id)
		}
		pc.queue = append(pc.queue, text)
		return fmt.Sprintf("%s queued it until %s accepts secret chat %d", p.fr.tag, tag, id)
	}
	pn, err := p.device(user)
	if err != nil {
		return fmt.Sprintf("%s cannot write to %s: %v", p.fr.tag, tag, err)
	}
	cfg, err := p.local.DhConfig(e2e.GroupVersion, e2e.NumLen)
	if err != nil {
		return fmt.Sprintf("%s cannot write to %s: %v", p.fr.tag, tag, err)
	}
	nm, ok := cfg.(*tl.MessagesDhConfigNotModified)
	if !ok {
		return "the server's DH config is not the pinned group"
	}
	ends := e2e.Ends{Admin: p.fr.id, AdminDevice: peerDeviceID, Participant: user, ParticipantDevice: pn.device}
	fresh := make([]byte, e2e.NumLen)
	rand.Read(fresh)
	req, err := e2e.MakeRequest(p.dev, ends, fresh, nm.Random)
	if err != nil {
		return fmt.Sprintf("%s cannot write to %s: %v", p.fr.tag, tag, err)
	}
	var rid [4]byte
	rand.Read(rid[:])
	ch, err := p.local.RequestEncryption(&tl.MessagesRequestEncryptionRequest{UserID: user, DeviceID: pn.device,
		RandomID: int(int32(uint32(rid[0]) | uint32(rid[1])<<8 | uint32(rid[2])<<16 | uint32(rid[3])<<24)), GA: req.GA, GASig: req.Sig})
	if err != nil {
		return fmt.Sprintf("%s cannot write to %s: %v", p.fr.tag, tag, err)
	}
	w, ok := ch.(*tl.EncryptedChatWaiting)
	if !ok {
		return fmt.Sprintf("%s's request was answered %T", p.fr.tag, ch)
	}
	p.chats[int32(w.ID)] = &peerChat{with: user, req: req, ends: ends, queue: []string{text}}
	return fmt.Sprintf("%s asked %s for secret chat %d; the text goes once it is accepted", p.fr.tag, tag, w.ID)
}

// dmCommand is `dm <friend> <text>`: the demo friend writes to every signed-in account with a chat device (normally the one under test).
func (d *demo) dmCommand(fr *demoFriend, text string) string {
	if d.peer == nil || d.peer.fr != fr {
		name := "none"
		if d.peer != nil {
			name = d.peer.fr.tag
		}
		return fmt.Sprintf("usage: dm <friend> <text>; the friend with a chat device is %s", name)
	}
	var out []string
	for _, x := range d.s.SignedIn() {
		d.mu.Lock()
		tag := d.tags[x]
		d.mu.Unlock()
		out = append(out, d.peer.do(func() string { return d.peer.dm(x, tag, text) }))
	}
	if len(out) == 0 {
		return "nobody is signed in to the chat"
	}
	return strings.Join(out, "\n")
}
