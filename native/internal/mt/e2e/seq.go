package e2e

// Seq counts one side's messages on a secret chat (contract 2.5, Telegram's seq_no rule, core.telegram.org/api/end-to-end/seq_no).
// Service messages count like normal ones.
type Seq struct {
	Role     Role
	Sent     int // messages this side sent on the chat
	Received int // messages this side took, in order, from the peer
}

// Next is in_seq_no and out_seq_no of the next message this side sends, and counts it as sent:
// out_seq_no = 2*sent + r, in_seq_no = 2*received + (1 - r).
func (s *Seq) Next() (in, out int) {
	r := int(s.Role)
	in, out = 2*s.Received+(1-r), 2*s.Sent+r
	s.Sent++
	return in, out
}

// Verdict is what the receiver does with a message by its sequence numbers.
type Verdict int

const (
	SeqOK     Verdict = iota // the next one in order: take it (Take counts it)
	SeqRepeat                // already taken: drop it
	SeqGap                   // later than the next one: hold it and ask Resend for the missing range
	SeqBroken                // the peer claims to have received more than this side sent, or the numbers have the wrong parity: discard the chat
)

func (v Verdict) String() string {
	return [...]string{"ok", "repeat", "gap", "broken"}[v]
}

// Expect is the out_seq_no the next message from the peer must carry: 2*received + r_peer.
func (s *Seq) Expect() int { return 2*s.Received + int(s.Role.Other()) }

// Check classifies a message from the peer. For SeqGap, start..end is the missing range in the peer's numbering (what Resend asks for).
func (s *Seq) Check(inSeq, outSeq int) (v Verdict, start, end int) {
	rPeer := int(s.Role.Other())
	if outSeq < 0 || inSeq < 0 || outSeq%2 != rPeer || inSeq%2 != int(s.Role) {
		return SeqBroken, 0, 0
	}
	// The peer's in_seq_no = 2*received_peer + r_self; received_peer may not exceed what this side sent.
	if inSeq > 2*s.Sent+int(s.Role) {
		return SeqBroken, 0, 0
	}
	want := s.Expect()
	switch {
	case outSeq < want:
		return SeqRepeat, 0, 0
	case outSeq > want:
		return SeqGap, want, outSeq - 2
	}
	return SeqOK, 0, 0
}

// Take counts one message from the peer as received (after Check said SeqOK).
func (s *Seq) Take() { s.Received++ }
