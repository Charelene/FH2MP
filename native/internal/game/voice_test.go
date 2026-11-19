package game

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"testing"

	"fh2mp/internal/bits"
	"fh2mp/internal/netv2"
	"fh2mp/internal/playersetup"
)

// A client's voice datagram as the game writes it (game+0x37f860): a header with no messages, then HeaderToServer and the payload for each.
func clientVoice(payloads ...[]byte) []byte {
	var out []byte
	out = binary.LittleEndian.AppendUint16(out, netv2.UnreliableID)
	out = binary.LittleEndian.AppendUint16(out, 0)
	out = binary.LittleEndian.AppendUint32(out, 0)
	out = append(out, 0, 0, 0)
	for _, p := range payloads {
		out = binary.LittleEndian.AppendUint16(out, VoiceMagic)
		out = binary.LittleEndian.AppendUint16(out, uint16(len(p)))
		out = append(out, p...)
	}
	binary.LittleEndian.PutUint16(out[9:], uint16(len(out)))
	return out
}

func voiceRig(t *testing.T) (*stateRig, *Peer, *Peer, *Peer) {
	r := newStateRig(t)
	world := &WorldKey{Type: 1, Name: "freeroam"}
	other := &WorldKey{Type: 1, Name: "another"}
	mk := func(port int, k *WorldKey) *Peer {
		p := r.halfOpen(port)
		p.Connected, p.Entered, p.State, p.World = true, true, PeerEntered, k
		return p
	}
	return r, mk(40001, world), mk(40002, world), mk(40003, other)
}

func sentTo(r *stateRig, a Addr) [][]byte {
	var out [][]byte
	for _, row := range r.w.Sock.(*fakeSock).sent {
		if row.addr == a {
			out = append(out, row.data)
		}
	}
	return out
}

// Each chat packet is copied, unread, to the other players of the sender's world as HeaderFromServer with the sender's xuid; not back
// to the sender and not to another world.
func TestVoiceRelayStampsTheSender(t *testing.T) {
	r, a, b, c := voiceRig(t)
	p1 := bytes.Repeat([]byte{'V'}, 40)
	p2 := append([]byte{'V', 1, 2, 0, 2}, bytes.Repeat([]byte{7}, 60)...)
	r.w.Handle(clientVoice(p1, p2), a.Addr)
	got := sentTo(r, b.Addr)
	if len(got) != 2 {
		t.Fatalf("b got %d datagrams, want 2", len(got))
	}
	for i, want := range [][]byte{p1, p2} {
		d := got[i]
		if d[8] != 0 || int(binary.LittleEndian.Uint16(d[9:])) != len(d) {
			t.Errorf("datagram %d header: count %d size %d of %d bytes", i, d[8], binary.LittleEndian.Uint16(d[9:]), len(d))
		}
		if binary.LittleEndian.Uint16(d[11:]) != VoiceMagic || int(binary.LittleEndian.Uint16(d[13:])) != len(want) {
			t.Errorf("datagram %d chat header %x", i, d[11:15])
		}
		if from := binary.LittleEndian.Uint64(d[15:]); from != a.Xuid {
			t.Errorf("datagram %d from %#x, want %#x", i, from, a.Xuid)
		}
		if !bytes.Equal(d[23:], want) {
			t.Errorf("datagram %d payload changed", i)
		}
	}
	if n := len(sentTo(r, a.Addr)); n != 0 {
		t.Errorf("the sender got %d datagrams back", n)
	}
	if n := len(sentTo(r, c.Addr)); n != 0 {
		t.Errorf("a player of another world got %d datagrams", n)
	}
	if !r.logHas("voice: first chat packet from owner") {
		t.Errorf("no first-packet line in %q", r.log)
	}
}

// What the game's voice loop reads (game+0x379390 -> game+0x1767030): the relayed datagram parses as a header with no messages, then
// magic, size and the 64-bit id read as eight bytes.
func TestVoiceDatagramParses(t *testing.T) {
	d := VoiceDatagram(5, 0xff, 0x0009000012345678, []byte("0123456789abcdef"))
	r := bits.NewReader(d)
	h, err := netv2.ParseHeader(r)
	if err != nil || h.Count != 0 || h.AckID != netv2.UnreliableID || h.RemoteAck != 5 {
		t.Fatalf("header %+v %v", h, err)
	}
	if left := r.Size - r.Pos; left <= 0xc0 {
		t.Fatalf("%d bits after the header: the game's loop only reads past 0xc0", left)
	}
	magic, _ := r.Bits(16)
	size, _ := r.Bits(16)
	var id uint64
	for i := 0; i < 8; i++ {
		v, _ := r.Bits(8)
		id |= v << (8 * i)
	}
	if magic != VoiceMagic || size != 16 || id != 0x0009000012345678 {
		t.Errorf("magic %#x size %d id %#x", magic, size, id)
	}
}

// Over the budget, a sender's packets are dropped; a refused size is said once and nothing after it is relayed.
func TestVoiceRelayLimits(t *testing.T) {
	r, a, b, _ := voiceRig(t)
	p := bytes.Repeat([]byte{1}, 30)
	var many [][]byte
	for i := 0; i < 40; i++ {
		many = append(many, p)
	}
	r.w.Handle(clientVoice(many...), a.Addr)
	if n := len(sentTo(r, b.Addr)); n != voiceBurstTokens {
		t.Errorf("a burst of 40 relayed %d, want %d", n, voiceBurstTokens)
	}
	if a.VoiceDropped != 40-voiceBurstTokens {
		t.Errorf("dropped %d", a.VoiceDropped)
	}
	big := bytes.Repeat([]byte{2}, VoiceMaxPayload+1)
	r.clk.t += 10
	before := len(sentTo(r, b.Addr))
	r.w.Handle(clientVoice(big, p), a.Addr)
	if n := len(sentTo(r, b.Addr)) - before; n != 0 {
		t.Errorf("after a refused packet %d were relayed", n)
	}
	if !r.logHas("voice: a chat packet of 513 bytes (the limit is 512) refused") {
		t.Errorf("no refusal line in %q", r.log)
	}
}

// A player that has not entered the world sends no voice.
func TestVoiceRelayNeedsAnEnteredSender(t *testing.T) {
	r, a, b, _ := voiceRig(t)
	a.Entered, a.State = false, PeerConnected
	r.w.Handle(clientVoice(bytes.Repeat([]byte{1}, 30)), a.Addr)
	if n := len(sentTo(r, b.Addr)); n != 0 {
		t.Errorf("relayed %d from a peer that has not entered", n)
	}
}

// The game's Mute writes PlayerChatStates (var 41 id, 42 Muted) on the listener's LobbyPlayer: from then on the muted sender's voice is
// not relayed to that listener; unmuting brings it back.
func TestVoiceRelayHonoursTheListenersMute(t *testing.T) {
	r, a, b, _ := voiceRig(t)
	mute := func(on bool) {
		v := uint64(0)
		if on {
			v = 1
		}
		size, payload, err := playersetup.Encode(map[int]bits.Int{41: bits.FromUint64(a.Xuid), 42: bits.FromUint64(v)})
		if err != nil {
			t.Fatal(err)
		}
		r.w.NoteChatStates(b, size, payload)
	}
	mute(true)
	r.w.Handle(clientVoice(bytes.Repeat([]byte{1}, 30)), a.Addr)
	if n := len(sentTo(r, b.Addr)); n != 0 {
		t.Fatalf("relayed %d to a listener who muted the sender", n)
	}
	if !r.logHas("muted 0x9000000000009c41") {
		t.Errorf("no mute line in %q", r.log)
	}
	mute(false)
	r.w.Handle(clientVoice(bytes.Repeat([]byte{1}, 30)), a.Addr)
	if n := len(sentTo(r, b.Addr)); n != 1 {
		t.Fatalf("after unmuting %d relayed, want 1", n)
	}
}

// Dev 2026-11-12 17:00: voice came after the messages of ordinary datagrams, padded to a byte, and was not relayed ("426 bits left after
// 1 messages"). This is that datagram's tail as the server logged it (2 padding bits, then 0xC4A7, 49 bytes), behind 6 message bits.
func TestVoiceAfterMessagesIsReadFromTheNextByte(t *testing.T) {
	r, a, b, _ := voiceRig(t)
	tail, _ := hex.DecodeString("9c12c70058050c00086040242f7cc37abcded28eb3c475978c8bbaceba0908e7b580484004bb570a6ead21eddb373ed37996737c1a01")
	body, err := bits.FromBytes(tail).Lsh(6).Or(bits.FromUint64(0x2a)).Bytes(len(tail)) // 6 bits of a message, then the logged tail (426 bits)
	if err != nil {
		t.Fatal(err)
	}
	data := append(make([]byte, 11), body...)
	br := bits.NewReader(data)
	br.Pos = 88 + 6
	r.w.relayVoice(a, br)
	got := sentTo(r, b.Addr)
	if len(got) != 1 {
		t.Fatalf("relayed %d datagrams, want the one chat packet", len(got))
	}
	p := got[0][23:]
	if len(p) != 49 || p[0] != 'V' || p[1] != 1 || p[4] != 2 {
		t.Fatalf("payload of %d bytes starting %x: want the Guide's 49-byte packet 'V' 1 .. with 2 frames", len(p), p[:5])
	}
	if left := br.Size - br.Pos; left >= 8 {
		t.Errorf("%d bits left unread after the chat packet", left)
	}
	// messages that end on a byte, and a datagram with no chat packet, as before
	br = bits.NewReader(append(make([]byte, 11), 0x15, 0x00, 0x00, 0x00, 0x00))
	br.Pos = 88 + 3
	r.w.relayVoice(a, br)
	if br.Pos != 91 || len(sentTo(r, b.Addr)) != 1 {
		t.Errorf("a datagram without a chat packet moved the reader to %d or was relayed", br.Pos)
	}
}
