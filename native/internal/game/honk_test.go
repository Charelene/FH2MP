package game

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"math"
	"strings"
	"testing"
)

// The sound names in HonkHonk's banks, in bank order (the FSB5 name tables of fh2-mods\HonkHonk\HZ2_SFX_HonkHonk.bank, read 2026-11-18): 0.4.0
// (0.3.4's is the same) and 1.0.0 (README and Sound_List.txt of HonkHonk_v1.0.0.zip give the same order).
var (
	honkBank040 = []string{"Car", "Truck", "Classic", "Train", "Warthog", "Bicycle", "Reversing", "Police", "Boat", "Doom", "Doorbell", "Honk", "Horse",
		"Quack", "Rooster", "Starter", "Teams", "WindowsXP", "Wololo", "Augh", "Bad to the bone", "Bruh", "DejaVu", "Discord", "DJ", "Goofy", "Lego", "MGS",
		"Social credit", "Spiderman", "Wow"}
	honkBank100 = []string{"Car", "Truck", "Classic", "Train", "Warthog", "Bicycle", "Reversing", "Police", "Police2", "Police3", "Martinshorn",
		"Firetruck", "Boat", "Doom", "Doorbell", "Honk", "Horse", "Quack", "Rooster", "Starter", "Teams", "WindowsXP", "Wololo", "Augh", "Bad to the bone",
		"Bruh", "DejaVu", "Discord", "DJ", "Goofy", "Lego", "MGS", "Social credit", "Spiderman", "Wow", "Cat", "Meow", "Siren", "SleighBells", "Wheelspin",
		"Win10", "Xbox", "Nyooom", "La Cucaracha", "Cyberpunk", "Rizz", "Stinky"}
)

func honkTestRequest(key, epoch []byte, xuid, seq uint64, car, horn byte, press uint16, v2 bool) []byte {
	d := make([]byte, honkBase)
	copy(d, sideHeader[:9])
	binary.LittleEndian.PutUint16(d[9:], honkRequest)
	copy(d[11:19], honkMagic)
	d[19] = 1
	if v2 {
		d[19] = 3
	}
	binary.LittleEndian.PutUint64(d[20:], xuid)
	binary.LittleEndian.PutUint64(d[28:], seq)
	copy(d[36:52], epoch)
	d[52], d[53] = car, horn
	binary.LittleEndian.PutUint16(d[55:], press)
	return signed(d, key)
}

// honk has player i send a horn request; -> the server's reply (nil: none), checked to be signed with i's key and of the type that answers the request.
func (r *modsRig) honk(i int, seq uint64, horn byte, press uint16, v2 bool) []byte {
	r.t.Helper()
	p := r.peers[i]
	before := len(r.sock.sent)
	r.hub.Handle(honkTestRequest(r.keys[i], modsVecEpoch, p.Xuid, seq, byte(*p.CarID), horn, press, v2), p.Addr)
	if len(r.sock.sent) == before {
		return nil
	}
	reply := r.sock.sent[len(r.sock.sent)-1].data
	want := byte(2)
	if v2 {
		want = 4
	}
	m := hmac.New(sha256.New, r.keys[i])
	m.Write(reply[:len(reply)-sideMacSize])
	if !hmac.Equal(m.Sum(nil), reply[len(reply)-sideMacSize:]) || reply[19] != want || int(binary.LittleEndian.Uint16(reply[9:])) != len(reply) ||
		len(reply) != honkRequest+honkRecordSize*int(reply[54]) {
		r.t.Fatalf("player %d got a reply that is not a signed type %d horn list: % x", i, want, reply)
	}
	return reply
}

func honkRecords(reply []byte) [][]byte {
	var out [][]byte
	for k := 0; k < int(reply[54]); k++ {
		out = append(out, reply[honkBase+honkRecordSize*k:honkBase+honkRecordSize*(k+1)])
	}
	return out
}

func indexOf(list []string, s string) int {
	for i, v := range list {
		if v == s {
			return i
		}
	}
	return -1
}

// A horn keeps its sound across the two numberings: every 0.4.0 sound maps to the 1.0.0 sound of the same name and back; the sounds 0.4.0 lacks have
// no v1 number.
func TestHonkNumbersFollowTheSoundNames(t *testing.T) {
	for v1, name := range honkBank040 {
		if id := honkV1ToID(v1); id < 0 || id >= len(honkBank100) || honkBank100[id] != name {
			t.Errorf("v1 horn %d (%s) -> v2 %d", v1, name, id)
		}
	}
	for id := 0; id < honkIDs; id++ {
		want := -1
		if id < len(honkBank100) {
			want = indexOf(honkBank040, honkBank100[id])
		}
		got, ok := honkIDToV1(id)
		if !ok {
			got = -1
		}
		if got != want {
			t.Errorf("v2 horn %d -> v1 %d, want %d", id, got, want)
		}
	}
	for _, v1 := range []int{-1, honkV1Horns, honkOff} {
		if id := honkV1ToID(v1); id != -1 {
			t.Errorf("v1 horn %d is no sound but maps to %d", v1, id)
		}
	}
}

// A v2 shim (HonkHonk 1.0.0) and a released v1 shim (0.4.0) in one world: each hears the other's horn as the same sound; a sound the 31 sound bank
// does not have is left out of the v1 list instead of spoiling it.
func TestHonkRelayBetweenV1AndV2(t *testing.T) {
	r := newModsRig(t, 2, false)
	var seq [2]uint64
	send := func(i int, horn byte, press uint16, v2 bool) []byte {
		seq[i]++
		r.clk.t += 0.05
		return r.honk(i, seq[i], horn, press, v2)
	}
	boat040, boat100 := byte(indexOf(honkBank040, "Boat")), byte(indexOf(honkBank100, "Boat"))
	fire100, stinky100 := byte(indexOf(honkBank100, "Firetruck")), byte(indexOf(honkBank100, "Stinky"))
	police := byte(indexOf(honkBank040, "Police"))
	for k, c := range []struct {
		name       string
		aV2        bool
		aHorn      byte
		bV2        bool
		heard      bool
		bHearsHorn byte
	}{
		{"v2 Firetruck to v2", true, fire100, true, true, fire100},
		{"v2 Stinky to v2", true, stinky100, true, true, stinky100},
		{"v2 Firetruck to v1 (no such sound)", true, fire100, false, false, 0},
		{"v2 Boat to v1", true, boat100, false, true, boat040},
		{"v1 Boat to v2", false, boat040, true, true, boat100},
		{"v1 Police to v1", false, police, false, true, police},
	} {
		if a := send(0, c.aHorn, uint16(10+k), c.aV2); a == nil {
			t.Fatalf("%s: A's request was not answered", c.name)
		}
		b := send(1, honkOff, 0, c.bV2)
		if b == nil {
			t.Fatalf("%s: B's poll was not answered", c.name)
		}
		recs := honkRecords(b)
		switch {
		case !c.heard && len(recs) != 0:
			t.Errorf("%s: B got % x, want no record", c.name, recs)
		case c.heard && (len(recs) != 1 || recs[0][0] != 1 || recs[0][1] != 3 || recs[0][2] != c.bHearsHorn ||
			binary.LittleEndian.Uint16(recs[0][4:]) != uint16(10+k)):
			t.Errorf("%s: B got % x, want owner 1 car 3 horn %d press %d", c.name, recs, c.bHearsHorn, 10+k)
		}
	}
}

// A siren held on loop is heard for a minute (HonkHonk 1.0.0's sirens run up to 14 s a loop); past 60 s the press is silenced, and a new press after the
// 2 s pause is heard again.
func TestHonkHeldSirenLastsAMinute(t *testing.T) {
	r := newModsRig(t, 2, false)
	fire := byte(indexOf(honkBank100, "Firetruck"))
	var seqA, seqB uint64
	start := r.clk.t
	listed := func(at float64) (bool, int) {
		t.Helper()
		for r.clk.t+0.4 < start+at {
			r.clk.t += 0.4
			seqA++
			r.honk(0, seqA, fire, 1, true)
		}
		r.clk.t = start + at
		seqB++
		recs := honkRecords(r.honk(1, seqB, honkOff, 0, true))
		if len(recs) == 0 {
			return false, 0
		}
		return true, int(binary.LittleEndian.Uint16(recs[0][6:]))
	}
	seqA++
	r.honk(0, seqA, fire, 1, true)
	for _, at := range []float64{30, 59.5} {
		on, age := listed(at)
		if !on || math.Abs(float64(age)-at*1000) > 500 {
			t.Fatalf("held %g s: listed %v, age %d ms", at, on, age)
		}
	}
	if on, _ := listed(60.5); on {
		t.Fatal("held past 60 s: still listed")
	}
	found := false
	for _, l := range r.log {
		found = found || strings.Contains(l, "honk: owner 1 held press 1 for 60 s, silenced")
	}
	if !found {
		t.Fatalf("no cap line in the log: %q", r.log)
	}
	r.clk.t += 0.1
	seqA++
	r.honk(0, seqA, honkOff, 1, true)
	r.clk.t += honkCooldown
	seqA++
	r.honk(0, seqA, fire, 2, true)
	r.clk.t += 0.05
	seqB++
	if recs := honkRecords(r.honk(1, seqB, honkOff, 0, true)); len(recs) != 1 || binary.LittleEndian.Uint16(recs[0][4:]) != 2 {
		t.Fatalf("the next press after the pause is not heard: % x", recs)
	}
}

// Requests outside their version's horn range or with an unknown type get no answer; v2 takes up to 63 for sounds a later HonkHonk appends.
func TestHonkRefusesMalformedRequests(t *testing.T) {
	for _, c := range []struct {
		name     string
		horn     byte
		v2       bool
		kind     byte
		answered bool
	}{
		{"v1 last sound", honkV1Horns - 1, false, 0, true},
		{"v1 past the 31 sound bank", honkV1Horns, false, 0, false},
		{"v2 last 1.0.0 sound", byte(len(honkBank100) - 1), true, 0, true},
		{"v2 appended sound", honkIDs - 1, true, 0, true},
		{"v2 past 63", honkIDs, true, 0, false},
		{"type 5", 0, true, 5, false},
		{"reply type 4", 0, true, 4, false},
	} {
		t.Run(c.name, func(t *testing.T) {
			r := newModsRig(t, 1, false)
			p := r.peers[0]
			d := honkTestRequest(r.keys[0], modsVecEpoch, p.Xuid, 1, byte(*p.CarID), c.horn, 1, c.v2)
			if c.kind != 0 {
				d[19] = c.kind
				d = signed(d[:honkBase], r.keys[0])
			}
			r.hub.Handle(d, p.Addr)
			if got := len(r.sock.sent) == 1; got != c.answered {
				t.Fatalf("answered %v, want %v", got, c.answered)
			}
		})
	}
}

// The v2 vectors the shim's test checks its wire code against (shim/tests/test_honk.c --vector2 / --reply2): key bytes 0..31, epoch 16..31, xuid
// 0x9000123456789. The request holds Firetruck (11), press 0x1234, seq 7, car 5. The reply is the server's answer to a poll with seq 7 from that
// player while owner 1 (car 3, at 10/20/-30) has held Stinky (46), press 300, for 1.5 s.
func TestHonkV2Vectors(t *testing.T) {
	checkVector(t, "honk2_request.hex", honkTestRequest(modsVecKey, modsVecEpoch, modsVecXuid, 7, 5, 11, 0x1234, true))

	r := newModsRig(t, 2, false)
	b := r.peers[1]
	delete(r.hub.Keys, b.Xuid)
	b.Xuid = modsVecXuid
	r.hub.Bound[b.Addr] = boundEnt{xuid: b.Xuid, until: 5000, key: string(modsVecKey)}
	r.hub.Keys[b.Xuid] = map[string]float64{string(modsVecKey): 5000}
	r.keys[1] = modsVecKey
	pos := [3]float64{10, 20, -30}
	r.peers[0].Pos = &pos
	r.honk(0, 1, 46, 300, true)
	r.clk.t += 1.5
	r.honk(0, 2, 46, 300, true)
	reply := r.honk(1, 7, honkOff, 0, true)
	if recs := honkRecords(reply); len(recs) != 1 || fmt.Sprintf("% x", recs[0][:8]) != "01 03 2e 01 2c 01 dc 05" {
		t.Fatalf("reply records % x", recs)
	}
	checkVector(t, "honk2_reply.hex", reply)
}
