package game

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"fh2mp/internal/catalog"
	"fh2mp/internal/omap"
	"fh2mp/internal/pyrand"
)

// The frozen vectors the shim's test checks its wire code against (shim/tests/test_mods.c): key bytes 0..31, epoch 16..31, xuid 0x9000123456789.
var (
	modsVecKey = func() []byte {
		k := make([]byte, 32)
		for i := range k {
			k[i] = byte(i)
		}
		return k
	}()
	modsVecEpoch = func() []byte {
		e := make([]byte, 16)
		for i := range e {
			e[i] = byte(16 + i)
		}
		return e
	}()
)

const modsVecXuid = 0x9000123456789

// modsVecUG is a lit underglow: front and both sides, 10/6/6 LEDs, Rainbow Wave front, Comet right, Police rear, Static left, cyan and pink,
// brightness 0.25, 0.3 cycles a second, a 0.85 m half width, front edge 1.6 m, rear -1.4 m, side trim 80 cm, 1 cm up.
func modsVecUG() []byte {
	ug := make([]byte, modsUG)
	ug[0], ug[1], ug[2], ug[3], ug[4] = 1, 0x0b, 10, 6, 6
	ug[5], ug[6], ug[7], ug[8] = 2, 13, 7, 0
	copy(ug[9:15], []byte{0, 255, 255, 255, 25, 153})
	ug[15], ug[16] = 16, 15
	binary.LittleEndian.PutUint16(ug[17:], 850)
	binary.LittleEndian.PutUint16(ug[19:], 1600)
	binary.LittleEndian.PutUint16(ug[21:], uint16(0x10000-1400))
	ug[23], ug[24] = 80, 1
	return ug
}

func modsTestRequest(key, epoch []byte, xuid, seq uint64, car, nos byte, ug []byte) []byte {
	d := make([]byte, modsBase)
	copy(d, sideHeader[:9])
	binary.LittleEndian.PutUint16(d[9:], modsRequest)
	copy(d[11:19], modsMagic)
	d[19] = 1
	binary.LittleEndian.PutUint64(d[20:], xuid)
	binary.LittleEndian.PutUint64(d[28:], seq)
	copy(d[36:52], epoch)
	d[52], d[53] = car, nos
	copy(d[modsHead:], ug)
	return signed(d, key)
}

func vectorFile(t *testing.T, name string) string {
	t.Helper()
	return filepath.Join("..", "..", "..", "shim", "tests", "vectors", name)
}

// checkVector compares b with a frozen vector (FH2MP_WRITE_VECTORS=1 writes it instead).
func checkVector(t *testing.T, name string, b []byte) {
	t.Helper()
	path := vectorFile(t, name)
	if os.Getenv("FH2MP_WRITE_VECTORS") == "1" {
		if err := os.WriteFile(path, []byte(hex.EncodeToString(b)+"\n"), 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("vector %s: %v", name, err)
	}
	if got, want := hex.EncodeToString(b), strings.TrimSpace(string(raw)); got != want {
		t.Fatalf("vector %s differs:\n got %s\nwant %s", name, got, want)
	}
}

func TestModsWireRules(t *testing.T) {
	ug := modsVecUG()
	if !modsUGValid(ug) || !modsUGValid(make([]byte, modsUG)) {
		t.Fatal("a valid underglow state was refused")
	}
	bad := func(edit func(u []byte)) []byte { u := append([]byte(nil), ug...); edit(u); return u }
	for name, u := range map[string][]byte{
		"flags":         bad(func(u []byte) { u[0] = 2 }),
		"parts":         bad(func(u []byte) { u[1] = 16 }),
		"too many LEDs": bad(func(u []byte) { u[2], u[3], u[4] = 30, 3, 2 }),
		"effect":        bad(func(u []byte) { u[7] = modsUGEffects }),
		"width":         bad(func(u []byte) { binary.LittleEndian.PutUint16(u[17:], modsUGWidthMax+1) }),
		"front < rear":  bad(func(u []byte) { binary.LittleEndian.PutUint16(u[19:], uint16(0x10000-1500)) }),
		"reserved":      bad(func(u []byte) { u[26] = 1 }),
	} {
		if modsUGValid(u) {
			t.Errorf("%s: an invalid underglow state was accepted", name)
		}
	}
	req := modsTestRequest(modsVecKey, modsVecEpoch, modsVecXuid, 9, 6, modsNosSwitch|modsNosAllowed|modsNosBurning, ug)
	if len(req) != modsRequest || !modsParse(req) {
		t.Fatalf("request of %d bytes refused", len(req))
	}
	checkVector(t, "mods_request.hex", req)
	for name, edit := range map[string]func(d []byte){
		"type":     func(d []byte) { d[19] = 2 },
		"car":      func(d []byte) { d[52] = 128 },
		"nos":      func(d []byte) { d[53] = 0x10 },
		"reserved": func(d []byte) { d[54] = 1 },
		"length":   func(d []byte) { d[9]++ },
	} {
		d := append([]byte(nil), req...)
		edit(d)
		if modsParse(d) {
			t.Errorf("%s: a malformed request was accepted", name)
		}
	}
}

type modsRig struct {
	t     *testing.T
	clk   *fakeClock
	sock  *fakeSock
	hub   *Hub
	w     *World
	peers []*Peer
	keys  [][]byte
	log   []string
}

// newModsRig is a hub with one Free Roam world and n ticket-bound, entered players (owner i+1, car i+3, xuid 0x9000000000000100+i).
func newModsRig(t *testing.T, n int, roadtrip bool) *modsRig {
	t.Helper()
	t.Setenv("FH2MP_UDP_AUTH", "ticket")
	r := &modsRig{t: t, clk: &fakeClock{t: 1000}}
	r.sock = &fakeSock{clk: r.clk}
	cat, err := catalog.Load()
	if err != nil {
		t.Skip("catalog not available:", err)
	}
	out := NewLogOut(func(chunk string) error {
		r.log = append(r.log, strings.Split(strings.TrimRight(chunk, "\n"), "\n")...)
		return nil
	})
	hub, err := NewHub(HubConfig{Clock: r.clk, Rng: pyrand.New(1), Sock: r.sock, Out: out, Catalog: cat, CtlSend: func([]byte, Addr) {}, Sinks: &Sinks{}})
	if err != nil {
		t.Fatal(err)
	}
	r.hub = hub
	w, err := NewWorld("[test]", out, r.clk, pyrand.New(1), r.sock, nil, &Sinks{}, func() *catalog.Catalog { return cat }, roadtrip, false, 0)
	if err != nil {
		t.Fatal(err)
	}
	w.Hub = hub
	r.w = w
	for i := 0; i < n; i++ {
		p := NewPeer(Addr{"10.0.0.1", 40000 + i}, 1000)
		car := i + 3
		p.Owner, p.Xuid, p.Connected, p.Entered, p.ConnectedAt, p.CarID = i+1, 0x9000000000000100+uint64(i), true, true, 900, &car
		p.State, p.OpenedAt = PeerConnected, 900
		pos := [3]float64{float64(i) * 10, 0, 0}
		p.Pos = &pos
		w.Peers.Set(p.Addr, p)
		hub.Route.Set(p.Addr, w)
		key := make([]byte, 32)
		for k := range key {
			key[k] = byte(100 + i*7 + k) // never the vector key
		}
		hub.Bound[p.Addr] = boundEnt{xuid: p.Xuid, until: 5000, key: string(key)}
		hub.Keys[p.Xuid] = map[string]float64{string(key): 5000}
		hub.TicketEpochs[p.Addr] = modsVecEpoch
		r.peers = append(r.peers, p)
		r.keys = append(r.keys, key)
	}
	return r
}

// send has player i report its state; -> the server's reply to it (nil: none).
func (r *modsRig) send(i int, seq uint64, nos byte, ug []byte) []byte {
	r.t.Helper()
	p := r.peers[i]
	before := len(r.sock.sent)
	r.hub.Handle(modsTestRequest(r.keys[i], modsVecEpoch, p.Xuid, seq, byte(*p.CarID), nos, ug), p.Addr)
	if len(r.sock.sent) == before {
		return nil
	}
	reply := r.sock.sent[len(r.sock.sent)-1].data
	m := hmac.New(sha256.New, r.keys[i])
	m.Write(reply[:len(reply)-sideMacSize])
	if !hmac.Equal(m.Sum(nil), reply[len(reply)-sideMacSize:]) || reply[19] != 2 || int(binary.LittleEndian.Uint16(reply[9:])) != len(reply) {
		r.t.Fatalf("player %d got a reply that is not a signed type 2 mod state reply: % x", i, reply)
	}
	return reply
}

func modsRecords(reply []byte) [][]byte {
	var out [][]byte
	for k := 0; k < int(reply[54]); k++ {
		out = append(out, reply[modsHead+modsRecord*k:modsHead+modsRecord*(k+1)])
	}
	return out
}

// Two players in Free Roam: each sees the other's underglow and NOS, NOS is allowed; in an event it is not, and a burn there is not shown.
func TestModsRelayAndNosRule(t *testing.T) {
	r := newModsRig(t, 2, false)
	ug := modsVecUG()
	a := r.send(0, 1, modsNosSwitch|modsNosAllowed|modsNosBurning, ug)
	if a == nil || a[53] != modsServerNosOK || a[54] != 0 {
		t.Fatalf("first reply: % x", a)
	}
	b := r.send(1, 1, modsNosSwitch, make([]byte, modsUG))
	recs := modsRecords(b)
	if b[53] != modsServerNosOK || len(recs) != 1 || recs[0][0] != 1 || recs[0][1] != 3 || recs[0][2] != modsNosSwitch|modsNosAllowed|modsNosBurning ||
		!bytes.Equal(recs[0][4:], ug) {
		t.Fatalf("B's reply does not carry A's state: % x", b)
	}
	if !r.w.nosBurningRecently(r.peers[0]) || r.w.nosBurningRecently(r.peers[1]) {
		t.Fatal("the push check's NOS window is wrong")
	}

	// A enters an event: NOS is off for A (the reply says so) and A's burn is neither stored nor shown
	r.clk.t += 0.1
	r.w.Event = &Event{Started: r.clk.t, Finished: omap.New[*Peer, float64](), Quit: map[*Peer]bool{}, People: []*Peer{r.peers[0]}}
	a = r.send(0, 2, modsNosSwitch|modsNosBurning, ug)
	if a[53] != 0 {
		t.Fatalf("NOS allowed in an event: % x", a)
	}
	found := false
	for _, l := range r.log {
		found = found || strings.Contains(l, "reports NitroSystem's NOS burning in an event")
	}
	if !found {
		t.Fatalf("no warning for a NOS burn in an event; log %q", r.log)
	}
	b = r.send(1, 2, modsNosSwitch, make([]byte, modsUG))
	if recs = modsRecords(b); len(recs) != 1 || recs[0][2]&modsNosBurning != 0 {
		t.Fatalf("a NOS burn in an event was shown to B: % x", b)
	}
	r.clk.t += modsNosGrace + 0.1
	if r.w.nosBurningRecently(r.peers[0]) {
		t.Fatal("the push check stays quiet after the grace although NOS was off")
	}

	// a stale state is not relayed; a replayed sequence number gets no answer
	r.clk.t += modsLease + 0.1
	if b = r.send(1, 3, 0, make([]byte, modsUG)); len(modsRecords(b)) != 0 {
		t.Fatalf("a state older than the lease was relayed: % x", b)
	}
	if again := r.send(1, 3, 0, make([]byte, modsUG)); again != nil {
		t.Fatal("a replayed sequence number was answered")
	}
}

// A Road Trip world never allows NOS.
func TestModsNosOffInRoadTrip(t *testing.T) {
	r := newModsRig(t, 1, true)
	if a := r.send(0, 1, modsNosSwitch|modsNosBurning, make([]byte, modsUG)); a == nil || a[53] != 0 {
		t.Fatalf("NOS allowed in a Road Trip: % x", a)
	}
}

// A request signed with another key, or for another car than the server has, is ignored.
func TestModsRefusesForeignRequests(t *testing.T) {
	r := newModsRig(t, 1, false)
	p := r.peers[0]
	before := len(r.sock.sent)
	r.hub.Handle(modsTestRequest(modsVecKey, modsVecEpoch, p.Xuid, 1, byte(*p.CarID), 0, make([]byte, modsUG)), p.Addr)
	r.hub.Handle(modsTestRequest(r.keys[0], modsVecEpoch, p.Xuid, 2, byte(*p.CarID)+1, 0, make([]byte, modsUG)), p.Addr)
	if len(r.sock.sent) != before || p.Mods != nil {
		t.Fatal("a request with a foreign signature or the wrong car was acted on")
	}
}

// The reply the shim's test reads (tests/vectors/mods_reply.hex): to the vector request, from a Free Roam world where owner 10, car 5 burns NOS with
// the vector underglow.
func TestModsReplyVector(t *testing.T) {
	req := modsTestRequest(modsVecKey, modsVecEpoch, modsVecXuid, 9, 6, modsNosSwitch|modsNosAllowed|modsNosBurning, modsVecUG())
	reply := append([]byte(nil), req[:modsHead]...)
	reply[19], reply[53], reply[54] = 2, modsServerNosOK, 1
	rec := make([]byte, modsRecord)
	rec[0], rec[1], rec[2] = 10, 5, modsNosSwitch|modsNosAllowed|modsNosBurning
	copy(rec[4:], modsVecUG())
	reply = append(reply, rec...)
	binary.LittleEndian.PutUint16(reply[9:], uint16(len(reply)+sideMacSize))
	checkVector(t, "mods_reply.hex", signed(reply, modsVecKey))
}

// A NOS burn where it is allowed keeps the push check quiet; the same push without it is logged.
func TestPushCheckSkipsAllowedNos(t *testing.T) {
	g := func(nos bool) int {
		r := newLoadRig(t, 1)
		p := r.peers[0]
		r.w.Event = nil
		mid := int64(1234)
		p.ModelID = &mid
		speed, x := 30.0, 0.0
		hits := 0
		for i := 0; float64(i)*0.05 <= 6; i++ {
			if nos {
				p.Mods = &ModState{NosAt: r.clk.t}
			}
			r.w.TrackPush(p, pushBody([3]float64{speed, 0, 0}, 0), [3]float64{x, 0, 0}, uint64(uint32(i*50)), 0)
			speed += 25 * 0.05
			x += speed * 0.05
		}
		for _, l := range r.log {
			if strings.Contains(l, "PUSH") {
				hits++
			}
		}
		return hits
	}
	if g(false) == 0 {
		t.Fatal("the push without NOS was not logged (the test does not push hard enough)")
	}
	if n := g(true); n != 0 {
		t.Fatalf("%d PUSH line(s) for an allowed NOS burn", n)
	}
}
