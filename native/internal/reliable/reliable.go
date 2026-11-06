// Package reliable is the server side of the game's reliable packets (= server/reliable.py).
package reliable

import (
	"fmt"

	"fh2mp/internal/diag"
	"fh2mp/internal/netv2"
)

// Constants (reliable.py).
const (
	IDWrap           = 0xFFFF
	UnreliableID     = 0xFFFF
	ResendSeconds    = 2.2
	AckGraceSeconds  = 0.5
	MaxPending       = 1024
	GiveUpLogSeconds = 10.0
	createMessage    = 7
	objectUpdateMsg  = 8
)

var unreliableMessages = [128]bool{}

func init() {
	for _, i := range []int{0, 1, 2, 3, 4, 5, 15, 16, 17, 18, 19, 20, 21, 24} {
		unreliableMessages[i] = true
	}
}

var log = diag.Get("reliable")
var rate = diag.NewRateLimit(nil, 0)

// RelayOwner is relay_owner: the relay owner named by a baked create or update, or -1.
func RelayOwner(m netv2.Baked, routerBits map[int][2]int) int {
	value := m.V.Uint64()
	if int(value&0x7F) != createMessage && int(value&0x7F) != objectUpdateMsg || (value>>11)&3 != 2 {
		return -1
	}
	rb, ok := routerBits[int((value>>7)&0xF)]
	if !ok {
		return -1
	}
	return int(m.V.Field(13+rb[0], 8))
}

// Entry is one packet waiting for an ack.
type Entry struct {
	ID       uint16
	At       float64
	Messages []netv2.Baked
}

// Window is the reliable packets sent to one client and not acked yet, in send order.
type Window struct {
	Label                                   string
	LastID                                  uint16
	Pending                                 []Entry
	Sent, Resent, ResentCreates, Peak, Lost int
}

// Stamp returns the id for a datagram of these baked messages; only its reliable messages are kept.
func (w *Window) Stamp(messages []netv2.Baked, now float64) uint16 {
	var keep []netv2.Baked
	for _, m := range messages {
		if !unreliableMessages[m.Type()] {
			keep = append(keep, m)
		}
	}
	if len(keep) == 0 {
		return UnreliableID
	}
	w.LastID = uint16((int(w.LastID) + 1) % IDWrap)
	w.Pending = append(w.Pending, Entry{w.LastID, now, keep})
	w.Sent++
	if len(w.Pending) > w.Peak {
		w.Peak = len(w.Pending)
	}
	for len(w.Pending) > MaxPending {
		old := w.Pending[0]
		w.Pending = w.Pending[1:]
		w.Lost++
		if ok, skipped := rate.Allow(fmt.Sprint("overflow", w.Label), GiveUpLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			log.Warn(fmt.Sprintf("%s%d reliable packets are waiting for an ack; giving up packet id %d (%d message(s), sent %.1f s ago), %d given up so far%s",
				prefix(w.Label), MaxPending, old.ID, len(old.Messages), now-old.At, w.Lost, extra))
		}
	}
	return w.LastID
}

func prefix(label string) string {
	if label == "" {
		return ""
	}
	return label + ": "
}

// Ack drops the packets the client acknowledged.
func (w *Window) Ack(remoteAck uint16, ackBits uint32) {
	if len(w.Pending) == 0 {
		return
	}
	kept := w.Pending[:0]
	for _, e := range w.Pending {
		d := (int(remoteAck) - int(e.ID)) % IDWrap
		if d < 0 {
			d += IDWrap
		}
		if d == 0 || (d <= 32 && (ackBits>>uint(d-1))&1 == 1) {
			continue
		}
		kept = append(kept, e)
	}
	w.Pending = kept
}

// Due takes the packets that count as lost and returns their messages, oldest first.
func (w *Window) Due(now, lastRx float64) []netv2.Baked {
	var out []netv2.Baked
	var kept []Entry
	i := 0
	for ; i < len(w.Pending); i++ {
		e := w.Pending[i]
		if now-e.At <= ResendSeconds {
			break
		}
		if lastRx <= e.At+AckGraceSeconds {
			kept = append(kept, e)
			continue
		}
		out = append(out, e.Messages...)
	}
	w.Pending = append(kept, w.Pending[i:]...)
	w.Resent += len(out)
	creates := 0
	for _, m := range out {
		if m.Type() == createMessage {
			creates++
		}
	}
	w.ResentCreates += creates
	if len(out) > 0 {
		log.Debug(fmt.Sprintf("%s%d message(s) (%d creates) resent, no ack after %g s; %d packet(s) still waiting", prefix(w.Label), len(out), creates, ResendSeconds, len(w.Pending)))
	}
	return out
}

// ForgetOwner drops what is pending for an owner that left.
func (w *Window) ForgetOwner(owner int, routerBits map[int][2]int) {
	var kept []Entry
	for _, e := range w.Pending {
		var k []netv2.Baked
		for _, m := range e.Messages {
			if RelayOwner(m, routerBits) != owner {
				k = append(k, m)
			}
		}
		if len(k) == 0 {
			continue
		}
		e.Messages = k
		kept = append(kept, e)
	}
	w.Pending = kept
}
