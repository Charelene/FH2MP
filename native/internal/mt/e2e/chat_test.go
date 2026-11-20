package e2e

import (
	"errors"
	"fmt"
	"testing"
	"time"

	"fh2mp/internal/mt/tl"
)

type clock struct{ t time.Time }

func (c *clock) now() time.Time { return c.t }

func pair(t *testing.T) (*Chat, *Chat, *clock) {
	key := randBytes(256)
	clk := &clock{t: time.Unix(1_800_000_000, 0)}
	return NewChat(7, Admin, 10, 20, key, clk.now), NewChat(7, Participant, 20, 10, key, clk.now), clk
}

// deliver hands each sealed message to the other side and returns the texts it took; replies travel back (and their replies forth) until quiet.
func deliver(t *testing.T, from, to *Chat, msgs []Outgoing) []string {
	t.Helper()
	var texts []string
	for len(msgs) > 0 {
		var back []Outgoing
		for _, m := range msgs {
			if len(m.Commitment) != 32 {
				t.Fatalf("commitment of %d bytes", len(m.Commitment))
			}
			evs, replies, err := to.Receive(m.Data)
			if err != nil {
				t.Fatalf("%s receive: %v", to.Role, err)
			}
			for _, e := range evs {
				if e.Action == nil {
					texts = append(texts, e.Text)
					if e.RandomID == m.RandomID && !CheckCommitment(m.Commitment, e.FrankingKey, from.Self, to.Self, m.RandomID, e.Text) {
						t.Fatalf("commitment of %q does not match its franking key", e.Text)
					}
				}
			}
			back = append(back, replies...)
		}
		from, to, msgs = to, from, back
	}
	return texts
}

func TestChatEcho(t *testing.T) {
	a, p, _ := pair(t)
	out, err := a.Send("hi there")
	if err != nil {
		t.Fatal(err)
	}
	if got := deliver(t, a, p, out); len(got) != 1 || got[0] != "hi there" {
		t.Fatalf("participant took %q", got)
	}
	out, _ = p.Send("echo: hi there")
	if got := deliver(t, p, a, out); len(got) != 1 || got[0] != "echo: hi there" {
		t.Fatalf("admin took %q", got)
	}
	// A repeat is dropped silently.
	if evs, replies, err := p.Receive(mustSend(t, a, "x")[0].Data); err != nil || len(evs) != 1 || len(replies) != 0 {
		t.Fatalf("first copy: %v %v %v", evs, replies, err)
	}
	last := mustSend(t, a, "y")[0].Data
	p.Receive(last)
	if evs, _, err := p.Receive(last); err != nil || len(evs) != 0 {
		t.Fatalf("repeat taken: %v %v", evs, err)
	}
	if s := p.Seq(); s.Received != 3 || s.Sent != 1 {
		t.Fatalf("participant counts %+v", s)
	}
}

func mustSend(t *testing.T, c *Chat, text string) []Outgoing {
	t.Helper()
	o, err := c.Send(text)
	if err != nil {
		t.Fatal(err)
	}
	return o
}

// A lost message: the next one is held, Resend is asked for the missing number, the sender re-seals it under a fresh random_id, and both
// are then taken in order.
func TestChatGapAndResend(t *testing.T) {
	a, p, _ := pair(t)
	m1 := mustSend(t, a, "one")[0]
	lost := mustSend(t, a, "two")[0]
	m3 := mustSend(t, a, "three")[0]
	if evs, _, _ := p.Receive(m1.Data); len(evs) != 1 {
		t.Fatal("first not taken")
	}
	evs, replies, err := p.Receive(m3.Data)
	if err != nil || len(evs) != 0 || len(replies) != 1 {
		t.Fatalf("after the gap: %v %d replies %v", evs, len(replies), err)
	}
	// The Resend reaches the admin, who re-seals "two" (and only it).
	aev, resent, err := a.Receive(replies[0].Data)
	if err != nil || len(aev) != 1 || aev[0].Action == nil || len(resent) != 1 {
		t.Fatalf("admin on Resend: %v %d %v", aev, len(resent), err)
	}
	rs := aev[0].Action.(*tl.DecryptedMessageActionResend)
	if rs.StartSeqNo != 2 || rs.EndSeqNo != 2 {
		t.Fatalf("Resend %d..%d, 2..2 expected", rs.StartSeqNo, rs.EndSeqNo)
	}
	if resent[0].RandomID == lost.RandomID {
		t.Fatal("re-sealed under the same random_id")
	}
	evs, _, err = p.Receive(resent[0].Data)
	if err != nil || len(evs) != 2 || evs[0].Text != "two" || evs[1].Text != "three" {
		t.Fatalf("after the resend: %+v %v", evs, err)
	}
}

func TestChatBroken(t *testing.T) {
	a, p, _ := pair(t)
	// 101 messages held behind a gap: the chat is given up.
	mustSend(t, a, "lost")
	var err error
	for i := 0; i <= Kept && err == nil; i++ {
		_, _, err = p.Receive(mustSend(t, a, fmt.Sprint(i))[0].Data)
	}
	if !errors.Is(err, ErrBroken) {
		t.Fatalf("after %d held messages: %v", Kept+1, err)
	}
	// A message whose in_seq_no claims more than this side sent: broken.
	a2, p2, _ := pair(t)
	a2.seq.Received = 5 // the admin claims it took five of the participant's messages
	if _, _, err := p2.Receive(mustSend(t, a2, "x")[0].Data); !errors.Is(err, ErrBroken) {
		t.Fatalf("in_seq_no past what was sent: %v", err)
	}
	// A tampered message: refused and never shown; the next one finds the gap, Resend brings the original again, and both are taken.
	a3, p3, _ := pair(t)
	d := mustSend(t, a3, "x")[0].Data
	d[40] ^= 1
	var r *Refused
	if evs, _, err := p3.Receive(d); !errors.As(err, &r) || len(evs) != 0 {
		t.Fatalf("tampered: %v %v", evs, err)
	}
	if got := deliver(t, a3, p3, mustSend(t, a3, "after")); len(got) != 2 || got[0] != "x" || got[1] != "after" {
		t.Fatalf("after a refused message: %q", got)
	}
}

// After 100 messages on a key the sender re-keys (RequestKey, AcceptKey, CommitKey), both switch and the new key carries on; a message
// sealed with the old key just before the switch still opens.
func TestChatRekey(t *testing.T) {
	a, p, _ := pair(t)
	old := a.Fingerprint()
	for i := 0; i < RekeyAfter/2; i++ {
		deliver(t, a, p, mustSend(t, a, "ping"))
		deliver(t, p, a, mustSend(t, p, "pong"))
	}
	out := mustSend(t, a, "after 100")
	if len(out) != 2 {
		t.Fatalf("%d messages, RequestKey and the text expected", len(out))
	}
	if got := deliver(t, a, p, out); len(got) != 1 || got[0] != "after 100" {
		t.Fatalf("took %q", got)
	}
	if a.Fingerprint() == old || a.Fingerprint() != p.Fingerprint() {
		t.Fatalf("fingerprints after the re-key: admin %x participant %x (old %x)", a.Fingerprint(), p.Fingerprint(), old)
	}
	if got := deliver(t, p, a, mustSend(t, p, "on the new key")); len(got) != 1 {
		t.Fatal("new key does not carry")
	}
	// After 7 days the key is due again.
	a2, p2, clk := pair(t)
	clk.t = clk.t.Add(RekeyAge)
	if out := mustSend(t, a2, "a week later"); len(out) != 2 {
		t.Fatalf("no re-key after 7 days: %d messages", len(out))
	} else if got := deliver(t, a2, p2, out); len(got) != 1 || a2.Fingerprint() != p2.Fingerprint() {
		t.Fatal("re-key after 7 days failed")
	}
}

// Both sides start a re-key at once: the larger exchange_id wins, the other is aborted, and both end on the same new key.
func TestChatRekeyCollision(t *testing.T) {
	a, p, _ := pair(t)
	ra, err := a.startRekey()
	if err != nil {
		t.Fatal(err)
	}
	rp, err := p.startRekey()
	if err != nil {
		t.Fatal(err)
	}
	ida, idp := a.mine.id, p.mine.id
	_, repA, err := p.Receive(ra.Data)
	if err != nil {
		t.Fatal(err)
	}
	_, repP, err := a.Receive(rp.Data)
	if err != nil {
		t.Fatal(err)
	}
	deliver(t, p, a, repA)
	deliver(t, a, p, repP)
	if a.Fingerprint() != p.Fingerprint() || a.mine != nil || p.mine != nil || a.theirs != nil || p.theirs != nil {
		t.Fatalf("after the collision (ids %x %x): fingerprints %x %x", uint64(ida), uint64(idp), a.Fingerprint(), p.Fingerprint())
	}
	if got := deliver(t, a, p, mustSend(t, a, "still talking")); len(got) != 1 {
		t.Fatal("chat broken after the collision")
	}
}
