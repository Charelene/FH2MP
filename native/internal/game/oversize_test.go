package game

import (
	"strings"
	"testing"

	"fh2mp/internal/netv2"
	"github.com/Charelene/encore-paddock/bits"
)

// bytesMsg is a message of n bytes whose type is 43 (0xab & 0x7f).
func bytesMsg(n int) netv2.Baked {
	return netv2.Bake(func(w *bits.Writer) {
		for k := 0; k < n; k++ {
			w.Bits(0xab, 8)
		}
	})
}

// FH2 1.0.0.64 takes a datagram of up to 0x55c = 1372 bytes (Connection::ReadPacketHeader game+0x17672b0 disconnects above it with
// 0x80600007). A datagram above it is logged and, the check being log only, still sent.
func TestADatagramTheGameWouldRefuseIsLoggedAndStillSent(t *testing.T) {
	r := newLoadRig(t, 1)
	p := r.peers[0]
	sock := r.w.Sock.(*fakeSock)

	p.Outbox = []netv2.Baked{bytesMsg(1372 - 11)} // with the 11-byte header exactly the largest the game takes
	r.w.FlushPeer(p)
	if len(sock.sent) != 1 || len(sock.sent[0].data) != 1372 {
		t.Fatalf("sent %d datagram(s)", len(sock.sent))
	}
	for _, l := range r.log {
		if strings.Contains(l, "the game disconnects on more than") {
			t.Fatalf("a 1372-byte datagram logged: %s", l)
		}
	}

	p.Outbox = []netv2.Baked{bytesMsg(1372 - 11 + 1)}
	r.w.FlushPeer(p)
	if len(sock.sent) != 2 || len(sock.sent[1].data) != 1373 {
		t.Fatalf("the 1373-byte datagram was not sent as it is: %d datagram(s)", len(sock.sent))
	}
	found := false
	for _, l := range r.log {
		if strings.Contains(l, "WARN") && strings.Contains(l, "a datagram of 1373 bytes (1 message(s), the first of type 43 and 10896 bits) goes to owner 1 xuid 0x64; "+
			"the game disconnects on more than 1372 (reason 0x80600007") {
			found = true
		}
	}
	if !found {
		t.Fatalf("no line for the 1373-byte datagram in %q", r.log)
	}
}
