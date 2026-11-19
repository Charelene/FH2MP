package e2e

import (
	"crypto/rand"
	"encoding/binary"
	"errors"
	"io"
	"math/big"
	"time"

	"github.com/gotd/td/bin"

	"fh2mp/internal/mt/tl"
)

// Limits of one secret chat (contract 2.5 and 2.6).
const (
	// Layer is the decryptedMessageLayer.layer this package writes and takes.
	Layer = 1
	// MinRandomBytes is the least random_bytes a decryptedMessageLayer carries.
	MinRandomBytes = 15
	// Kept is how many sent messages are kept to answer Resend, how many may be held waiting for a gap, and how many messages the
	// previous key may still open after a re-key.
	Kept = 100
	// RekeyAfter / RekeyAge: a key is replaced after this many messages (sent + received) or this long.
	RekeyAfter = 100
	RekeyAge   = 7 * 24 * time.Hour
)

// ErrBroken: the chat cannot go on (the peer's numbers are impossible, or too many messages wait for a gap); the caller discards it.
var ErrBroken = errors.New("secret chat broken")

type chatKey struct {
	key  []byte
	fp   int64
	made time.Time
	uses int
}

// exchange is a re-key in flight: started by this side (a set) or accepted by it (key set, waiting for CommitKey).
type exchange struct {
	id  int64
	a   *big.Int
	key *chatKey
}

type sentMsg struct {
	in, out int
	date    int
	text    string
	action  tl.DecryptedMessageActionClass // nil: a text message
}

// Outgoing is one sealed message for messages.sendEncrypted (RandomID = the inner message's random_id).
type Outgoing struct {
	RandomID   int64
	Data       []byte
	Commitment []byte
}

// Event is a message taken from the peer, in sequence order.
type Event struct {
	RandomID    int64
	Date        int
	Text        string
	FrankingKey []byte                         // a text message's key, kept by the recipient for a report
	Action      tl.DecryptedMessageActionClass // a service message's action (nil for text)
}

// Chat is one side of a secret chat once its key is known: it seals what this side sends, opens what the peer sends in sequence order
// (holding messages after a gap and asking Resend), answers Resend, re-keys and keeps the previous key for late messages. It is not safe
// for concurrent use.
type Chat struct {
	ID         int32
	Role       Role
	Self, Peer int64
	Now        func() time.Time
	Rand       io.Reader

	cur, prev *chatKey
	prevLeft  int
	mine      *exchange // a re-key this side started
	theirs    *exchange // a re-key this side accepted, waiting for CommitKey
	seq       Seq
	outbox    []sentMsg
	held      map[int]*tl.DecryptedMessageLayer
	asked     int // the highest out_seq_no a Resend already asked for (-1: none)
}

// NewChat is a chat on key (from FinishRequest for the admin, AcceptRequest for the participant).
func NewChat(id int32, role Role, self, peer int64, key []byte, now func() time.Time) *Chat {
	if now == nil {
		now = time.Now
	}
	return &Chat{ID: id, Role: role, Self: self, Peer: peer, Now: now, Rand: rand.Reader,
		cur: &chatKey{key: key, fp: Fingerprint(key), made: now()}, seq: Seq{Role: role}, held: map[int]*tl.DecryptedMessageLayer{}, asked: -1}
}

// Fingerprint is the current key's fingerprint.
func (c *Chat) Fingerprint() int64 { return c.cur.fp }

// Seq is the chat's message counts.
func (c *Chat) Seq() Seq { return c.seq }

func (c *Chat) random(n int) []byte {
	b := make([]byte, n)
	if _, err := io.ReadFull(c.Rand, b); err != nil {
		panic(err) // crypto/rand does not fail
	}
	return b
}

func (c *Chat) randomID() int64 { return int64(binary.LittleEndian.Uint64(c.random(8))) }

// seal wraps a message in the layer with the given numbers and seals it with the current key.
func (c *Chat) seal(in, out int, msg tl.DecryptedMessageClass) ([]byte, error) {
	layer := &tl.DecryptedMessageLayer{RandomBytes: c.random(16), Layer: Layer, InSeqNo: in, OutSeqNo: out, Message: msg}
	var b bin.Buffer
	if err := layer.Encode(&b); err != nil {
		return nil, err
	}
	data, err := SealRandom(c.cur.key, c.cur.fp, c.Role, b.Buf)
	if err != nil {
		return nil, err
	}
	if len(data) > MaxData {
		return nil, refuse("sealed message of %d bytes, at most %d", len(data), MaxData)
	}
	c.cur.uses++
	return data, nil
}

// outgoing builds and seals one message with its numbers (fresh random_id and, for text, a fresh franking key and commitment).
func (c *Chat) outgoing(m sentMsg) (Outgoing, error) {
	rid := c.randomID()
	var msg tl.DecryptedMessageClass
	text := ""
	if m.action == nil {
		fk := c.random(FrankingKeyLen)
		msg = &tl.DecryptedMessage{RandomID: rid, Date: m.date, Text: m.text, FrankingKey: fk}
		text = m.text
		data, err := c.seal(m.in, m.out, msg)
		if err != nil {
			return Outgoing{}, err
		}
		return Outgoing{RandomID: rid, Data: data, Commitment: Commitment(fk, c.Self, c.Peer, rid, text)}, nil
	}
	msg = &tl.DecryptedMessageService{RandomID: rid, Action: m.action}
	data, err := c.seal(m.in, m.out, msg)
	if err != nil {
		return Outgoing{}, err
	}
	// A service message carries a commitment over an empty text under its own fresh key (a report of it simply fails).
	return Outgoing{RandomID: rid, Data: data, Commitment: Commitment(c.random(FrankingKeyLen), c.Self, c.Peer, rid, "")}, nil
}

// send numbers a new message, seals it and keeps it for Resend.
func (c *Chat) send(text string, action tl.DecryptedMessageActionClass) (Outgoing, error) {
	in, out := c.seq.Next()
	m := sentMsg{in: in, out: out, date: int(c.Now().Unix()), text: text, action: action}
	o, err := c.outgoing(m)
	if err != nil {
		return Outgoing{}, err
	}
	if c.outbox = append(c.outbox, m); len(c.outbox) > Kept {
		c.outbox = c.outbox[len(c.outbox)-Kept:]
	}
	return o, nil
}

// due says whether the current key should be replaced and no exchange is in flight.
func (c *Chat) due() bool {
	return c.mine == nil && c.theirs == nil && (c.cur.uses >= RekeyAfter || c.Now().Sub(c.cur.made) >= RekeyAge)
}

// Send seals a text message. When the key is due for replacing, a RequestKey goes first (both are returned, in order).
func (c *Chat) Send(text string) ([]Outgoing, error) {
	var out []Outgoing
	if c.due() {
		o, err := c.startRekey()
		if err != nil {
			return nil, err
		}
		out = append(out, o)
	}
	o, err := c.send(text, nil)
	if err != nil {
		return nil, err
	}
	return append(out, o), nil
}

// Service seals a service message (typing, read, noop ...).
func (c *Chat) Service(a tl.DecryptedMessageActionClass) (Outgoing, error) { return c.send("", a) }

func (c *Chat) startRekey() (Outgoing, error) {
	a, err := Secret(c.random(NumLen), nil)
	if err != nil {
		return Outgoing{}, err
	}
	gA := Public(a)
	if err := CheckPublic(gA); err != nil {
		return Outgoing{}, err
	}
	c.mine = &exchange{id: c.randomID(), a: a}
	return c.send("", &tl.DecryptedMessageActionRequestKey{ExchangeID: c.mine.id, GA: gA})
}

// open finds the key a sealed message names (current, previous, or one accepted in a re-key) and decrypts it.
func (c *Chat) open(data []byte) (*tl.DecryptedMessageLayer, *chatKey, error) {
	fp, ok := DataFingerprint(data)
	if !ok {
		return nil, nil, refuse("sealed message of %d bytes", len(data))
	}
	var k *chatKey
	switch {
	case fp == c.cur.fp:
		k = c.cur
	case c.prev != nil && fp == c.prev.fp:
		k = c.prev
	case c.theirs != nil && c.theirs.key != nil && fp == c.theirs.key.fp:
		k = c.theirs.key
	default:
		return nil, nil, refuse("no key with this fingerprint")
	}
	inner, err := Open(k.key, k.fp, c.Role.Other(), data)
	if err != nil {
		return nil, nil, err
	}
	b := &bin.Buffer{Buf: inner}
	var l tl.DecryptedMessageLayer
	if err := l.Decode(b); err != nil {
		return nil, nil, refuse("inner message does not decode")
	}
	if b.Len() != 0 {
		return nil, nil, refuse("%d bytes after the inner message", b.Len())
	}
	if l.Layer != Layer {
		return nil, nil, refuse("layer %d, %d expected", l.Layer, Layer)
	}
	if len(l.RandomBytes) < MinRandomBytes {
		return nil, nil, refuse("random_bytes of %d bytes, at least %d", len(l.RandomBytes), MinRandomBytes)
	}
	return &l, k, nil
}

// Receive opens one sealed message from the peer. It returns the messages taken in order (this one and any held ones it releases) and the
// service messages this side answers with (Resend answers, AcceptKey, CommitKey, AbortKey), in order. A *Refused error drops just this
// message; ErrBroken means the caller discards the chat.
func (c *Chat) Receive(data []byte) ([]Event, []Outgoing, error) {
	l, k, err := c.open(data)
	if err != nil {
		return nil, nil, err
	}
	if k == c.prev {
		if c.prevLeft--; c.prevLeft <= 0 {
			c.prev = nil
		}
	} else {
		k.uses++
	}
	var events []Event
	var replies []Outgoing
	v, start, end := c.seq.Check(l.InSeqNo, l.OutSeqNo)
	switch v {
	case SeqRepeat:
		return nil, nil, nil
	case SeqBroken:
		return nil, nil, ErrBroken
	case SeqGap:
		c.held[l.OutSeqNo] = l
		if len(c.held) > Kept {
			return nil, nil, ErrBroken
		}
		if end > c.asked {
			if start <= c.asked {
				start = c.asked + 2
			}
			c.asked = end
			o, err := c.send("", &tl.DecryptedMessageActionResend{StartSeqNo: start, EndSeqNo: end})
			if err != nil {
				return nil, nil, err
			}
			replies = append(replies, o)
		}
		return nil, replies, nil
	}
	for {
		c.seq.Take()
		ev, out, err := c.take(l)
		if err != nil {
			return events, replies, err
		}
		events = append(events, ev)
		replies = append(replies, out...)
		next, ok := c.held[c.seq.Expect()]
		if !ok {
			break
		}
		delete(c.held, next.OutSeqNo)
		switch v, _, _ := c.seq.Check(next.InSeqNo, next.OutSeqNo); v {
		case SeqBroken:
			return events, replies, ErrBroken
		case SeqOK:
			l = next
		default:
			return events, replies, nil
		}
	}
	return events, replies, nil
}

// take acts on one message taken in order.
func (c *Chat) take(l *tl.DecryptedMessageLayer) (Event, []Outgoing, error) {
	switch m := l.Message.(type) {
	case *tl.DecryptedMessage:
		return Event{RandomID: m.RandomID, Date: m.Date, Text: m.Text, FrankingKey: m.FrankingKey}, nil, nil
	case *tl.DecryptedMessageService:
		ev := Event{RandomID: m.RandomID, Action: m.Action}
		out, err := c.service(m.Action)
		return ev, out, err
	}
	return Event{}, nil, refuse("unknown inner message")
}

// service runs a service action from the peer (contract 2.5 Resend, 2.6 re-key); typing, read and noop need nothing.
func (c *Chat) service(a tl.DecryptedMessageActionClass) ([]Outgoing, error) {
	switch a := a.(type) {
	case *tl.DecryptedMessageActionResend:
		var out []Outgoing
		for _, m := range c.outbox {
			if m.out >= a.StartSeqNo && m.out <= a.EndSeqNo {
				// Re-sealed with the current key under a fresh random_id (the server answers a repeated random_id without relaying it again);
				// the sequence numbers stay the original ones.
				o, err := c.outgoing(m)
				if err != nil {
					return out, err
				}
				out = append(out, o)
			}
		}
		return out, nil
	case *tl.DecryptedMessageActionRequestKey:
		if c.mine != nil {
			if uint64(a.ExchangeID) <= uint64(c.mine.id) {
				o, err := c.send("", &tl.DecryptedMessageActionAbortKey{ExchangeID: a.ExchangeID})
				return []Outgoing{o}, err
			}
			c.mine = nil // both started at once: the larger exchange_id wins
		}
		if c.theirs != nil {
			o, err := c.send("", &tl.DecryptedMessageActionAbortKey{ExchangeID: a.ExchangeID})
			return []Outgoing{o}, err
		}
		b, err := Secret(c.random(NumLen), nil)
		if err != nil {
			return nil, err
		}
		key, err := SharedKey(a.GA, b)
		if err != nil {
			o, err2 := c.send("", &tl.DecryptedMessageActionAbortKey{ExchangeID: a.ExchangeID})
			if err2 != nil {
				return nil, err2
			}
			return []Outgoing{o}, nil
		}
		ck := &chatKey{key: key, fp: Fingerprint(key), made: c.Now()}
		c.theirs = &exchange{id: a.ExchangeID, key: ck}
		o, err := c.send("", &tl.DecryptedMessageActionAcceptKey{ExchangeID: a.ExchangeID, GB: Public(b), KeyFingerprint: ck.fp})
		return []Outgoing{o}, err
	case *tl.DecryptedMessageActionAcceptKey:
		if c.mine == nil || c.mine.id != a.ExchangeID {
			return nil, nil
		}
		key, err := SharedKey(a.GB, c.mine.a)
		if err != nil || Fingerprint(key) != a.KeyFingerprint {
			id := c.mine.id
			c.mine = nil
			o, err := c.send("", &tl.DecryptedMessageActionAbortKey{ExchangeID: id})
			return []Outgoing{o}, err
		}
		// CommitKey is sealed with the old key; everything after it with the new one.
		o, err := c.send("", &tl.DecryptedMessageActionCommitKey{ExchangeID: a.ExchangeID, KeyFingerprint: a.KeyFingerprint})
		if err != nil {
			return nil, err
		}
		c.switchTo(&chatKey{key: key, fp: a.KeyFingerprint, made: c.Now()})
		c.mine = nil
		return []Outgoing{o}, nil
	case *tl.DecryptedMessageActionCommitKey:
		if c.theirs == nil || c.theirs.id != a.ExchangeID {
			return nil, nil
		}
		ex := c.theirs
		c.theirs = nil
		if ex.key.fp != a.KeyFingerprint {
			o, err := c.send("", &tl.DecryptedMessageActionAbortKey{ExchangeID: ex.id})
			return []Outgoing{o}, err
		}
		c.switchTo(ex.key)
		return nil, nil
	case *tl.DecryptedMessageActionAbortKey:
		if c.mine != nil && c.mine.id == a.ExchangeID {
			c.mine = nil
		}
		if c.theirs != nil && c.theirs.id == a.ExchangeID {
			c.theirs = nil
		}
		return nil, nil
	}
	return nil, nil
}

func (c *Chat) switchTo(k *chatKey) {
	c.prev, c.prevLeft = c.cur, Kept
	c.cur = k
}
