package game

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"strings"
	"testing"

	"fh2mp/internal/omap"
)

func quickchatTestRequest(key, epoch []byte, xuid, seq uint64, number uint32, phrase byte) []byte {
	d := make([]byte, quickchatBase)
	copy(d, sideHeader[:9])
	binary.LittleEndian.PutUint16(d[9:], quickchatRequest)
	copy(d[11:19], quickchatMagic)
	d[19] = 1
	binary.LittleEndian.PutUint64(d[20:], xuid)
	binary.LittleEndian.PutUint64(d[28:], seq)
	copy(d[36:52], epoch)
	binary.LittleEndian.PutUint32(d[52:], number)
	d[56] = phrase
	return signed(d, key)
}

// say has player i ask with its newest message (number, phrase); -> the server's reply to it (nil: none).
func (r *modsRig) say(i int, seq uint64, number uint32, phrase byte) []byte {
	r.t.Helper()
	p := r.peers[i]
	before := len(r.sock.sent)
	r.hub.Handle(quickchatTestRequest(r.keys[i], modsVecEpoch, p.Xuid, seq, number, phrase), p.Addr)
	if len(r.sock.sent) == before {
		return nil
	}
	reply := r.sock.sent[len(r.sock.sent)-1].data
	m := hmac.New(sha256.New, r.keys[i])
	m.Write(reply[:len(reply)-sideMacSize])
	if !hmac.Equal(m.Sum(nil), reply[len(reply)-sideMacSize:]) || reply[19] != 2 || int(binary.LittleEndian.Uint16(reply[9:])) != len(reply) ||
		len(reply) != quickchatBase+quickchatRecord*int(reply[57])+sideMacSize {
		r.t.Fatalf("player %d got a reply that is not a signed type 2 quick chat reply: % x", i, reply)
	}
	return reply
}

type quickchatRec struct {
	owner, phrase int
	number        uint32
	age           int
	name          string
}

func quickchatRecords(reply []byte) []quickchatRec {
	var out []quickchatRec
	for k := 0; k < int(reply[57]); k++ {
		rec := reply[quickchatBase+quickchatRecord*k : quickchatBase+quickchatRecord*(k+1)]
		out = append(out, quickchatRec{int(rec[0]), int(rec[1]), binary.LittleEndian.Uint32(rec[4:]), int(binary.LittleEndian.Uint16(rec[8:])), string(rec[12 : 12+int(rec[2])])})
	}
	return out
}

func quickchatTaken(reply []byte) (uint32, byte) {
	return binary.LittleEndian.Uint32(reply[52:]), reply[56]
}

// The frozen vectors the shim's test checks its wire code against (shim/tests/test_quickchat.c): the request it builds, and the reply the server
// gives it when one other player (owner 10, "Dusk Carton") said message 4, phrase 5 a quarter of a second ago.
func TestQuickchatVectors(t *testing.T) {
	req := quickchatTestRequest(modsVecKey, modsVecEpoch, modsVecXuid, 7, 3, 2)
	if len(req) != quickchatRequest {
		t.Fatalf("request of %d bytes", len(req))
	}
	checkVector(t, "quickchat_request.hex", req)

	r := newModsRig(t, 2, false)
	me, other := r.peers[0], r.peers[1]
	delete(r.hub.Bound, me.Addr)
	delete(r.hub.Keys, me.Xuid)
	me.Xuid, me.Owner, other.Owner = modsVecXuid, 9, 10
	r.hub.Bound[me.Addr] = boundEnt{xuid: me.Xuid, until: 5000, key: string(modsVecKey)}
	r.hub.Keys[me.Xuid] = map[string]float64{string(modsVecKey): 5000}
	r.keys[0] = modsVecKey
	r.hub.ControlText(fmt.Sprintf("name %x %x", other.Xuid, "Dusk Carton"), nil)
	if r.say(1, 1, 4, 5) == nil {
		t.Fatal("the other player's message got no reply")
	}
	r.clk.t += 0.25
	before := len(r.sock.sent)
	r.hub.Handle(req, me.Addr)
	if len(r.sock.sent) != before+1 {
		t.Fatal("the vector request got no reply")
	}
	reply := r.sock.sent[len(r.sock.sent)-1].data
	recs := quickchatRecords(reply)
	if n, p := quickchatTaken(reply); n != 3 || p != 2 || len(recs) != 1 || recs[0] != (quickchatRec{10, 5, 4, 250, "Dusk Carton"}) {
		t.Fatalf("the vector reply: taken %d/%d, records %+v", n, p, recs)
	}
	checkVector(t, "quickchat_reply.hex", reply)
}

// Two players: a message reaches the other with its sender's gamertag, once taken it is echoed back, and it is passed on for 10 s.
func TestQuickchatDelivery(t *testing.T) {
	r := newModsRig(t, 2, false)
	r.hub.ControlText("name 9000000000000100 "+hex.EncodeToString([]byte("Trinculo 54")), nil)
	a := r.say(0, 1, 1, 12)
	if n, p := quickchatTaken(a); a == nil || n != 1 || p != 12 || a[57] != 0 {
		t.Fatalf("A's first reply: % x", a)
	}
	if !strings.Contains(strings.Join(r.log, "\n"), "quickchat: owner 1 message 1 phrase 12") {
		t.Fatalf("no log line for the message: %q", r.log)
	}
	r.clk.t += 0.5
	recs := quickchatRecords(r.say(1, 1, 0, 0))
	if len(recs) != 1 || recs[0] != (quickchatRec{1, 12, 1, 500, "Trinculo 54"}) {
		t.Fatalf("B doesn't get A's message: %+v", recs)
	}
	// B has said nothing: A's reply carries no record, and B's own state takes nothing
	r.clk.t += 0.1
	if a = r.say(0, 2, 1, 12); a[57] != 0 {
		t.Fatalf("A sees a message of B's that was never sent: % x", a)
	}
	// still there after 9.9 s (A keeps asking), gone after 10
	for i, at := range []float64{3, 6, 9} {
		r.clk.t = 1000 + at
		r.say(0, uint64(3+i), 1, 12)
	}
	r.clk.t = 1009.9
	r.say(0, 6, 1, 12)
	if recs = quickchatRecords(r.say(1, 2, 0, 0)); len(recs) != 1 || recs[0].age < 9899 || recs[0].age > 9900 {
		t.Fatalf("A's message is gone before its 10 s: %+v", recs)
	}
	r.clk.t = 1010.1
	r.say(0, 7, 1, 12)
	if recs = quickchatRecords(r.say(1, 3, 0, 0)); len(recs) != 0 {
		t.Fatalf("A's message is still passed on after 10 s: %+v", recs)
	}
}

// A player whose client stopped asking has no live message after the lease; a second message inside a second waits; a restarted client (its
// numbers start again at 1) is taken as new.
func TestQuickchatLeaseGapAndRestart(t *testing.T) {
	r := newModsRig(t, 2, false)
	r.say(0, 1, 1, 2)
	r.clk.t += quickchatLease + 0.1
	if recs := quickchatRecords(r.say(1, 1, 0, 0)); len(recs) != 0 {
		t.Fatalf("a message of a client that stopped asking is passed on: %+v", recs)
	}
	// message 2 half a second after message 1 was taken at t0: not yet
	r = newModsRig(t, 2, false)
	r.say(0, 1, 1, 2)
	r.clk.t += 0.5
	if n, p := quickchatTaken(r.say(0, 2, 2, 3)); n != 1 || p != 2 {
		t.Fatalf("a second message inside a second was taken: %d/%d", n, p)
	}
	r.clk.t += 0.6
	if n, p := quickchatTaken(r.say(0, 3, 2, 3)); n != 2 || p != 3 {
		t.Fatalf("the second message was not taken once a second had passed: %d/%d", n, p)
	}
	// the game restarted: message 1 again, after 2
	r.clk.t += 1.5
	if n, p := quickchatTaken(r.say(0, 4, 1, 7)); n != 1 || p != 7 {
		t.Fatalf("a restarted client's message 1 was not taken: %d/%d", n, p)
	}
	// a request that isn't newer than the last one is dropped without a reply
	r.clk.t += 0.1
	if r.say(0, 4, 1, 7) != nil {
		t.Fatal("a replayed request got a reply")
	}
}

// A player in an event and one outside it don't get each other's messages; two inside do.
func TestQuickchatSameEvent(t *testing.T) {
	r := newModsRig(t, 3, false)
	r.w.Event = &Event{Started: r.clk.t, Finished: omap.New[*Peer, float64](), Quit: map[*Peer]bool{}, People: []*Peer{r.peers[0], r.peers[1]}}
	r.say(0, 1, 1, 0)
	r.clk.t += 0.1
	if recs := quickchatRecords(r.say(1, 1, 0, 0)); len(recs) != 1 || recs[0].owner != 1 {
		t.Fatalf("a racer doesn't get another racer's message: %+v", recs)
	}
	if recs := quickchatRecords(r.say(2, 1, 0, 0)); len(recs) != 0 {
		t.Fatalf("a player outside the event gets a racer's message: %+v", recs)
	}
}

// Everything that must be refused without a reply, and the switch.
func TestQuickchatRefusals(t *testing.T) {
	r := newModsRig(t, 2, false)
	p := r.peers[0]
	good := quickchatTestRequest(r.keys[0], modsVecEpoch, p.Xuid, 1, 1, 2)
	resign := func(d []byte) []byte { return signed(d[:quickchatBase], r.keys[0]) }
	for name, d := range map[string][]byte{
		"phrase 64":      quickchatTestRequest(r.keys[0], modsVecEpoch, p.Xuid, 1, 1, quickchatPhrases),
		"type 2":         resign(func() []byte { d := append([]byte(nil), good...); d[19] = 2; return d }()),
		"reserved byte":  resign(func() []byte { d := append([]byte(nil), good...); d[57] = 1; return d }()),
		"length field":   resign(func() []byte { d := append([]byte(nil), good...); d[9]++; return d }()),
		"sequence 0":     quickchatTestRequest(r.keys[0], modsVecEpoch, p.Xuid, 0, 1, 2),
		"another's key":  quickchatTestRequest(r.keys[1], modsVecEpoch, p.Xuid, 1, 1, 2),
		"another epoch":  quickchatTestRequest(r.keys[0], make([]byte, 16), p.Xuid, 1, 1, 2),
		"unknown player": quickchatTestRequest(r.keys[0], modsVecEpoch, 77, 1, 1, 2),
		"a byte short":   good[:len(good)-1],
	} {
		before := len(r.sock.sent)
		r.hub.Handle(d, p.Addr)
		if len(r.sock.sent) != before || p.Quickchat != nil {
			t.Errorf("%s: the request was acted on", name)
		}
	}
	t.Setenv("FH2MP_NO_QUICKCHAT", "1")
	if r.say(0, 1, 1, 2) != nil {
		t.Error("FH2MP_NO_QUICKCHAT=1: the request was answered")
	}
	t.Setenv("FH2MP_NO_QUICKCHAT", "")
	if r.say(0, 1, 1, 2) == nil {
		t.Error("a good request got no reply")
	}
}

// The control port's "name" line: a gamertag with spaces, cut at 32 characters and never mid-character on the wire; an unreadable one is ignored.
func TestQuickchatNames(t *testing.T) {
	r := newModsRig(t, 2, false)
	r.hub.ControlText("name 9000000000000100 "+hex.EncodeToString([]byte("Dusk Carton")), nil)
	if r.hub.Gamertags[0x9000000000000100] != "Dusk Carton" {
		t.Fatalf("the name line was not kept: %v", r.hub.Gamertags)
	}
	r.hub.ControlText("name 9000000000000100 zz", nil)
	r.hub.ControlText("name 9000000000000100 ff", nil) // not UTF-8
	if r.hub.Gamertags[0x9000000000000100] != "Dusk Carton" {
		t.Fatalf("an unreadable name line changed the gamertag: %v", r.hub.Gamertags)
	}
	long := strings.Repeat("é", 40) // 80 bytes of UTF-8
	r.hub.ControlText("name 9000000000000101 "+hex.EncodeToString([]byte(long)), nil)
	if got := r.hub.Gamertags[0x9000000000000101]; got != strings.Repeat("é", quickchatTagRunes) {
		t.Fatalf("a long gamertag is kept as %q", got)
	}
	if name := r.hub.quickchatName(0x9000000000000101); string(name) != strings.Repeat("é", quickchatNameSize/2) {
		t.Fatalf("the name on the wire is %q", name)
	}
	r.hub.Gamertags[5] = "a" + strings.Repeat("é", 30) // 61 bytes: a cut at 48 would split a character
	if name := r.hub.quickchatName(5); len(name) != 47 || string(name) != "a"+strings.Repeat("é", 23) {
		t.Fatalf("the cut name is %q (%d bytes)", name, len(name))
	}
	if name := r.hub.quickchatName(6); len(name) != 0 {
		t.Fatalf("a player without a name has %q", name)
	}
}
