package game

import (
	"encoding/binary"
	"strings"
	"testing"

	"fh2mp/internal/gameopts"
)

func eventoptsTestRequest(key, epoch []byte, xuid, seq uint64, choice byte) []byte {
	d := make([]byte, eventoptsBase)
	copy(d, sideHeader[:9])
	binary.LittleEndian.PutUint16(d[9:], eventoptsRequest)
	copy(d[11:19], eventoptsMagic)
	d[19] = 1
	binary.LittleEndian.PutUint64(d[20:], xuid)
	binary.LittleEndian.PutUint64(d[28:], seq)
	copy(d[36:52], epoch)
	d[52] = choice
	return signed(d, key)
}

// eoSend has player i ask with `choice`; -> the reply's flag byte, and whether the server answered at all.
func eoSend(r *modsRig, i int, seq uint64, choice byte) (byte, bool) {
	r.t.Helper()
	p := r.peers[i]
	before := len(r.sock.sent)
	r.hub.Handle(eventoptsTestRequest(r.keys[i], modsVecEpoch, p.Xuid, seq, choice), p.Addr)
	if len(r.sock.sent) == before {
		return 0, false
	}
	reply := r.sock.sent[len(r.sock.sent)-1].data
	want := signed(reply[:eventoptsBase], r.keys[i])
	if len(reply) != eventoptsRequest || string(reply) != string(want) || reply[19] != 2 || string(reply[11:19]) != eventoptsMagic || reply[53] != 0 {
		r.t.Fatalf("player %d got a reply that is not a signed type 2 event options reply: % x", i, reply)
	}
	return reply[52], true
}

// The frozen request and replies the shim's test reads too (shim/tests/test_eventopts.c).
func TestEventoptsVectors(t *testing.T) {
	req := eventoptsTestRequest(modsVecKey, modsVecEpoch, modsVecXuid, 9, 1)
	checkVector(t, "eventopts_request.hex", req)
	for flag, name := range map[byte]string{0: "eventopts_reply_0.hex", eventoptsModded: "eventopts_reply_1.hex", eventoptsComing: "eventopts_reply_2.hex"} {
		reply := append([]byte(nil), req[:eventoptsBase]...)
		reply[19], reply[52] = 2, flag
		checkVector(t, name, signed(reply, modsVecKey))
	}
}

func TestEventoptsStep(t *testing.T) {
	a, b := []byte("0123456789abcdef"), []byte("fedcba9876543210")
	st := eventoptsStep(nil, a, 1, 100, eventoptsNone)
	if st.Choice != eventoptsNone {
		t.Fatalf("no choice sent, state %+v", st)
	}
	st = eventoptsStep(st, a, 2, 101, 1)
	if st.Choice != 1 || st.ChoiceAt != 101 {
		t.Fatalf("after Allowed: %+v", st)
	}
	st = eventoptsStep(st, a, 3, 105, eventoptsNone) // nothing new: the choice and its time stay
	if st.Choice != 1 || st.ChoiceAt != 101 || st.At != 105 {
		t.Fatalf("after a plain poll: %+v", st)
	}
	p := &Peer{Eventopts: st}
	if !p.allowsModded(101+eventoptsFresh) || p.allowsModded(101+eventoptsFresh+0.1) {
		t.Fatal("a choice counts for eventoptsFresh seconds, no longer")
	}
	if st = eventoptsStep(st, b, 1, 106, eventoptsNone); st.Choice != eventoptsNone { // a new ticket epoch starts over
		t.Fatalf("after a new epoch: %+v", st)
	}
	if (&Peer{}).allowsModded(0) || (&Peer{Eventopts: eventoptsStep(nil, a, 1, 100, 0)}).allowsModded(100) {
		t.Fatal("no state, or Not allowed, allows nothing")
	}
}

// A custom car races in a Free Roam event only when its leader allowed modded cars; the class rule still holds, and Road Trip never takes one.
func TestModdedCarsRaceWhenTheLeaderAllows(t *testing.T) {
	r := newLoadRig(t, 3)
	cat := r.w.cat()
	stock := int64(-1)
	for id := range cat.KnownCars {
		if _, blocked := cat.Blocked[id]; !blocked {
			stock = int64(id)
			break
		}
	}
	if stock < 0 {
		t.Skip("the catalog has no obtainable car")
	}
	leader, custom := r.peers[0], r.peers[1]
	leader.ModelID, custom.ModelID, r.peers[2].ModelID = i64p(stock), i64p(987654), i64p(stock)
	for _, p := range r.peers {
		p.Choices = &Choices{true, 3, 0.7}
	}
	anyCar, classS := gameopts.Rule{Class: 255}, gameopts.Rule{Class: 5}

	if ok, _ := r.w.carFitsEvent(custom, &anyCar); ok || !r.w.NotForEvents(custom) {
		t.Fatal("a custom car fits an event nobody allowed it in")
	}
	r.w.SetModdedCars(true, leader, "test")
	if ok, _ := r.w.carFitsEvent(custom, &anyCar); !ok || r.w.NotForEvents(custom) || r.w.refusalNotice(custom) != "" {
		t.Fatal("a custom car is refused although the leader allowed modded cars")
	}
	if ok, notice := r.w.carFitsEvent(custom, &classS); ok || strings.Contains(notice, "Modded cars") {
		t.Fatalf("a class 3 custom car fits a class S event (notice %q): the class rule must still hold", notice)
	}
	if r.count("event options: the event of leader owner 1 takes modded cars (test)") != 1 {
		t.Fatalf("no line for the change: %q", r.log)
	}
	r.w.Roadtrip = true
	if ok, _ := r.w.carFitsEvent(custom, nil); ok {
		t.Fatal("a custom car was let into Road Trip")
	}
	r.w.Roadtrip = false
	r.w.EndEvent("test", false)
	if r.w.ModdedCars {
		t.Fatal("the setting outlived its event")
	}
	if ok, _ := r.w.carFitsEvent(custom, &anyCar); ok {
		t.Fatal("a custom car fits the next event without its leader allowing it")
	}
}

// The wire: every player is told the world's setting; a leader's choice counts at its race request, or when it arrives while that request is pending.
func TestEventoptsHandle(t *testing.T) {
	r := newModsRig(t, 2, false)
	leader, other := r.peers[0], r.peers[1]
	if flag, ok := eoSend(r, 1, 1, eventoptsNone); !ok || flag != 0 {
		t.Fatalf("a plain poll: answered %v, flag %d", ok, flag)
	}
	// the choice first, then the race request
	r.clk.t += 1
	if flag, _ := eoSend(r, 0, 1, 1); flag != 0 {
		t.Fatalf("the player with the row on Allowed is told flag %d about itself", flag)
	}
	if r.w.ModdedCars {
		t.Fatal("a choice with no race requested changed the world")
	}
	// the other player hears that an event with modded cars may be coming, for a moment
	if flag, _ := eoSend(r, 1, 2, eventoptsNone); flag != eventoptsComing {
		t.Fatalf("the other player is told flag %d while the leader's row is on Allowed", flag)
	}
	r.clk.t += eventoptsComingFor + 0.1
	if flag, _ := eoSend(r, 1, 3, eventoptsNone); flag != 0 {
		t.Fatalf("the other player is still told flag %d after the leader fell silent", flag)
	}
	r.w.PendingRace = &PendingRace{r.clk.t + 10, leader}
	r.w.SetModdedCars(leader.allowsModded(r.clk.t), leader, "its Event Options at the race request")
	r.clk.t += 1
	if flag, _ := eoSend(r, 1, 4, eventoptsNone); flag != eventoptsModded {
		t.Fatalf("the other player is told flag %d for an event that takes modded cars", flag)
	}
	// somebody who is not the leader cannot change it
	r.clk.t += 1
	if flag, _ := eoSend(r, 1, 5, 0); flag != eventoptsModded || !r.w.ModdedCars || other.Eventopts.Choice != 0 {
		t.Fatalf("a non-leader's choice: flag %d, world %v", flag, r.w.ModdedCars)
	}
	// the race request first, then the leader's choice
	r.w.EndEvent("test", false)
	r.w.PendingRace = &PendingRace{r.clk.t + 10, leader}
	r.clk.t += 1
	if flag, _ := eoSend(r, 0, 2, 1); flag != eventoptsModded || !r.w.ModdedCars {
		t.Fatalf("the leader's choice after its request: flag %d, world %v", flag, r.w.ModdedCars)
	}
	r.clk.t += 1
	if flag, _ := eoSend(r, 0, 3, 0); flag != 0 || r.w.ModdedCars {
		t.Fatalf("the leader's Not allowed after its request: flag %d, world %v", flag, r.w.ModdedCars)
	}
	// replays, a bad choice byte and a nonzero reserved byte are not answered
	before := len(r.sock.sent)
	r.clk.t += 1
	r.hub.Handle(eventoptsTestRequest(r.keys[0], modsVecEpoch, leader.Xuid, 3, eventoptsNone), leader.Addr)
	r.hub.Handle(eventoptsTestRequest(r.keys[0], modsVecEpoch, leader.Xuid, 4, 2), leader.Addr)
	bad := eventoptsTestRequest(r.keys[0], modsVecEpoch, leader.Xuid, 5, 1)
	bad[53] = 1
	r.hub.Handle(signed(bad[:eventoptsBase], r.keys[0]), leader.Addr)
	r.hub.Handle(eventoptsTestRequest(r.keys[1], modsVecEpoch, leader.Xuid, 6, 1), leader.Addr) // signed with another player's key
	if len(r.sock.sent) != before {
		t.Fatalf("%d of four bad requests were answered", len(r.sock.sent)-before)
	}
}

// Road Trip gets no answer (so the shim shows no row), and FH2MP_NO_EVENTOPTS=1 turns the channel off.
func TestEventoptsOffInRoadTripAndBySwitch(t *testing.T) {
	r := newModsRig(t, 1, true)
	if _, ok := eoSend(r, 0, 1, 1); ok {
		t.Fatal("a Road Trip world answered")
	}
	r = newModsRig(t, 1, false)
	t.Setenv("FH2MP_NO_EVENTOPTS", "1")
	if _, ok := eoSend(r, 0, 1, 1); ok {
		t.Fatal("the switched-off channel answered")
	}
}
