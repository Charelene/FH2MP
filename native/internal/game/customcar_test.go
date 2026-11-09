package game

import (
	"fmt"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	"fh2mp/internal/bits"
	"fh2mp/internal/catalog"
	"fh2mp/internal/gameopts"
	"fh2mp/internal/netv2"
	"fh2mp/internal/parts"
	"fh2mp/internal/playersetup"
	"fh2mp/internal/pyrand"
)

// A car pack's own car (not in the game data, not a DLC car) drives in Free Roam but is refused by every event and by Road Trip, with a notice;
// a car from the game is not affected.
func TestCustomCarsDoNotRace(t *testing.T) {
	r := newLoadRig(t, 3)
	var mu sync.Mutex
	var sent []string
	r.w.Sinks.Notice = func(xuid uint64, text string, log func(string)) {
		mu.Lock()
		sent = append(sent, text)
		mu.Unlock()
	}
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
	custom, own, other := r.peers[0], r.peers[1], r.peers[2]
	custom.ModelID, own.ModelID, other.ModelID = i64p(987654), i64p(stock), i64p(stock)
	for _, p := range r.peers {
		p.Choices = &Choices{true, 3, 0.7}
	}
	if r.w.CustomCar(own) || !r.w.CustomCar(custom) {
		t.Fatalf("CustomCar: stock car %d = %v, car 987654 = %v", stock, r.w.CustomCar(own), r.w.CustomCar(custom))
	}

	anyCar := gameopts.Rule{Class: 255}
	if r.w.EligibleForEvent(custom, &anyCar) {
		t.Fatal("a custom car was let into an open event")
	}
	if !r.w.EligibleForEvent(own, &anyCar) {
		t.Fatal("a car from the game was refused by an open event")
	}
	if r.count("owner 1 car 987654 is not in the game data (a car pack's own car); custom cars do not race in events or Road Trip") != 1 {
		t.Fatalf("no refusal line: %q", r.log)
	}
	deadline := time.Now().Add(time.Second)
	for {
		mu.Lock()
		n := len(sent)
		mu.Unlock()
		if n > 0 || time.Now().After(deadline) {
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	mu.Lock()
	if len(sent) != 1 || !strings.Contains(sent[0], "Custom cars can't race") {
		t.Fatalf("notices %q", sent)
	}
	mu.Unlock()

	// Road Trip has no car rule: before, every car was let in.
	r.w.Roadtrip = true
	if r.w.EligibleForEvent(custom, nil) {
		t.Fatal("a custom car was let into Road Trip")
	}
	if !r.w.EligibleForEvent(own, nil) {
		t.Fatal("a car from the game was refused by Road Trip")
	}
	if !r.w.ValidateEventCars() {
		t.Fatal("the Road Trip race ended although two players drive cars from the game")
	}
	if r.in(custom) || !r.in(own) || !r.in(other) {
		t.Fatalf("Road Trip race participants after the car check: %s", owners(r.w.Event.People))
	}
}

// A refused Ready stays refused while the player still drives a custom car, even when its class/PI fit the event's rule after a car change.
func TestCustomCarRefusedReadyStaysRefused(t *testing.T) {
	r := newLoadRig(t, 2)
	r.w.Event = nil
	leader, custom := r.peers[0], r.peers[1]
	r.w.RaceRequestedAt = 990
	r.w.PendingRace = &PendingRace{Deadline: r.clk.t + RaceInviteSeconds, Leader: leader}
	custom.ModelID, custom.Choices = i64p(987654), &Choices{true, 3, 0.7}
	at := r.w.RaceRequestedAt
	custom.ReadyRetry = &at
	r.w.RetryRefusedReady(custom)
	if custom.ReadyRetry == nil || r.count("the refused Ready counts now") != 0 {
		t.Fatalf("a custom car's refused Ready was counted after a car change: %q", r.log)
	}
}

// The launcher names stand-ins for a car-pack player's custom cars (control "standins", closest stock car first): the first candidate the game
// data has, players can own and has a stock part list is used; the others see it instead of StandinCar.
func TestStandinsFromTheLauncher(t *testing.T) {
	r := newLoadRig(t, 1)
	cat := r.w.cat()
	good := int64(-1)
	for id := range cat.KnownCars {
		if _, blocked := cat.Blocked[id]; !blocked && int64(id) != StandinCar {
			if _, ok := parts.StockList(cat.Parts, int64(id)); ok {
				good = int64(id)
				break
			}
		}
	}
	if good < 0 {
		t.Skip("no obtainable car with a stock part list")
	}
	h := &Hub{Standins: map[uint64]map[int64]int64{}}
	h.catalogPtr.Store(cat)
	got, owned, notes := h.parseStandins(fmt.Sprintf("9024:987654/%d,9025:987655,x:1", good))
	if !owned[9024] || !owned[9025] || len(owned) != 2 {
		t.Fatalf("listed custom cars %v, want 9024 and 9025 (with or without a stand-in)", owned)
	}
	if len(got) != 1 || got[9024] != good || len(notes) != 3 || !strings.Contains(notes[1], "no usable candidate") || !strings.Contains(notes[2], "unreadable") {
		t.Fatalf("stand-ins %v, notes %q", got, notes)
	}
	p := r.peers[0]
	h.Standins[p.Xuid] = got
	r.w.Hub = h
	if s := r.w.standinFor(p, 9024); s != good {
		t.Fatalf("custom car 9024 shows as %d, want %d", s, good)
	}
	if s := r.w.standinFor(p, 9025); s != StandinCar {
		t.Fatalf("a custom car without a stand-in shows as %d, want the default %d", s, StandinCar)
	}
}

// The others' games draw the car named by the setup's Model value: a custom car's Model becomes its stand-in, as its part list does.
func TestStandinModel(t *testing.T) {
	r := newLoadRig(t, 1)
	p := r.peers[0]
	size, payload, err := playersetup.Encode(map[int]bits.Int{playersetup.Model: bits.FromUint64(9024)})
	if err != nil {
		t.Fatal(err)
	}
	f, err := playersetup.Fields(size, payload)
	if err != nil {
		t.Fatal(err)
	}
	read := func(pl bits.Int) uint64 {
		g, err := playersetup.Fields(size, pl)
		if err != nil {
			t.Fatal(err)
		}
		return g[playersetup.Model].Value.Uint64()
	}
	if got := read(r.w.standinModel(p, 9024, f, payload)); got != uint64(StandinCar) {
		t.Fatalf("Model %d without a stand-in, want %d", got, StandinCar)
	}
	r.w.Hub = &Hub{Standins: map[uint64]map[int64]int64{p.Xuid: {9024: 2045}}}
	if got := read(r.w.standinModel(p, 9024, f, payload)); got != 2045 {
		t.Fatalf("Model %d with the launcher's stand-in, want 2045", got)
	}
	r.w.Hub = nil // CheckParts refreshes the hub's catalog; without a hub the stand-in is StandinCar
	_, _, n, out := r.w.CheckParts(p, size, payload, 1)
	if n != size || read(out) != uint64(StandinCar) {
		t.Fatalf("a setup naming only custom model 9024 is relayed with Model %d", read(out))
	}
}

// A stand-in the launcher named gets the stand-in car's complete stock setup (parts, Model, UseStock, the custom car's paint and wheel style gone).
func TestStandinSetup(t *testing.T) {
	r := newLoadRig(t, 1)
	p := r.peers[0]
	cat := r.w.cat()
	good := int64(-1)
	for id := range cat.KnownCars {
		if _, blocked := cat.Blocked[id]; !blocked && int64(id) != StandinCar {
			if _, ok := parts.StockList(cat.Parts, int64(id)); ok {
				good = int64(id)
				break
			}
		}
	}
	if good < 0 {
		t.Skip("no obtainable car with a stock part list")
	}
	r.w.Hub = &Hub{Standins: map[uint64]map[int64]int64{p.Xuid: {9024: good}}}
	in := map[int]bits.Int{playersetup.Model: bits.FromUint64(9024), playersetup.WheelStyle: bits.FromUint64(7)}
	n, out, ok := r.w.standinSetup(p, 9024, in)
	if !ok {
		t.Fatal("no stand-in setup")
	}
	f, err := playersetup.Fields(n, out)
	if err != nil {
		t.Fatal(err)
	}
	stock, _ := parts.StockList(cat.Parts, good)
	words := playersetup.PartWords(f[playersetup.Parts].Value)
	if f[playersetup.Model].Value.Uint64() != uint64(good) || words[2] != stock[2] || f[playersetup.UseStock].Value.Uint64() != 1 {
		t.Fatalf("stand-in setup: Model %d body %d UseStock %d, want %d %d 1", f[playersetup.Model].Value.Uint64(), words[2], f[playersetup.UseStock].Value.Uint64(), good, stock[2])
	}
	if w, has := f[playersetup.WheelStyle]; has && !w.Value.IsZero() {
		t.Fatalf("the custom car's wheel style %d is still in the stand-in setup (the repair sends it as 0)", w.Value.Uint64())
	}
}

// Part (a): a player whose game has every custom car the sender's launcher listed (the same car pack) gets the sender's object as it was sent;
// everyone else gets the stand-in version; both histories are kept, numbered alike, for players who join later.
func TestSamePackPlayersSeeTheRealCar(t *testing.T) {
	r := newLoadRig(t, 3)
	r.w.Event = nil
	sender, pack, plain := r.peers[0], r.peers[1], r.peers[2]
	for _, p := range r.peers {
		p.Entered = true
	}
	r.w.Hub = &Hub{CustomCars: map[uint64]map[int64]bool{sender.Xuid: {9024: true}, pack.Xuid: {9024: true, 9030: true}}}
	if !r.w.RealCarsFor(sender, pack) || r.w.RealCarsFor(sender, plain) || r.w.RealCarsFor(pack, sender) {
		t.Fatalf("RealCarsFor: same pack %v, no pack %v, a sender missing one of the receiver's cars %v (want true false false)",
			r.w.RealCarsFor(sender, pack), r.w.RealCarsFor(sender, plain), r.w.RealCarsFor(pack, sender))
	}
	standin, realP := bits.FromUint64(0x5a5a), bits.FromUint64(0x3c3c)
	r.w.rememberAndRelay(sender, netv2.MsgObjectUpdate, 1, 0, nil, 0, 16, standin, &relayVariant{16, realP})
	rb := netv2.RouterBits[1]
	msg := func(p bits.Int) netv2.Baked {
		return netv2.Bake(netv2.RelayObjectMessage(uint64(netv2.MsgObjectUpdate), 1, 0, uint64(sender.Owner), 0, 0, rb[0], rb[1], 16, p))
	}
	last := func(q *Peer) netv2.Baked {
		if len(q.Outbox) == 0 {
			t.Fatalf("owner %d got nothing", q.Owner)
		}
		return q.Outbox[len(q.Outbox)-1]
	}
	if got := last(pack); !reflect.DeepEqual(got, msg(realP)) {
		t.Fatal("the player with the same car pack did not get the real object")
	}
	if got := last(plain); !reflect.DeepEqual(got, msg(standin)) {
		t.Fatal("the player without the car pack did not get the stand-in")
	}
	// a later message without a stand-in goes to both histories alike
	r.w.rememberAndRelay(sender, netv2.MsgObjectUpdate, 1, 0, nil, 0, 16, bits.FromUint64(0x0f0f), nil)
	hist, _ := sender.Objs.Get(ObjKey{1, 0})
	real := sender.RealObjs[ObjKey{1, 0}]
	if len(hist) != 2 || len(real) != 2 || !real[0].Payload.Equal(realP) || !hist[0].Payload.Equal(standin) || !real[1].Payload.Equal(hist[1].Payload) {
		t.Fatalf("histories: %d stand-in entries, %d real entries", len(hist), len(real))
	}
}

// "standins <xuid> -" (the LSP, when the player's game runs the server's database again) forgets the player's list and stand-ins.
func TestStandinsForgotten(t *testing.T) {
	clk := &fakeClock{t: 1000.0}
	cat, err := catalog.Load()
	if err != nil {
		t.Skip("catalog not available:", err)
	}
	var lines []string
	hub, err := NewHub(HubConfig{Clock: clk, Rng: pyrand.New(1), Sock: &fakeSock{clk: clk}, Out: NewLogOut(func(s string) error { lines = append(lines, s); return nil }),
		Catalog: cat, CtlSend: func([]byte, Addr) {}, Sinks: &Sinks{}})
	if err != nil {
		t.Fatal(err)
	}
	hub.ControlText("standins 90000e961ddae 9024:0", nil)
	if !hub.CustomCars[0x90000e961ddae][9024] {
		t.Fatalf("the list was not stored: %v", hub.CustomCars)
	}
	hub.ControlText("standins 90000e961ddae -", nil)
	if _, ok := hub.CustomCars[0x90000e961ddae]; ok || len(hub.Standins[0x90000e961ddae]) != 0 {
		t.Fatalf("the list was not forgotten: %v %v", hub.CustomCars, hub.Standins)
	}
	if !strings.Contains(strings.Join(lines, ""), "stand-ins for 90000e961ddae forgotten") {
		t.Fatalf("no line says the list was forgotten: %q", lines)
	}
}

// A car players cannot get (a traffic car) drives in Free Roam under the default CarPolicy "events": it is relayed and its owner stays; events
// and Road Trip refuse it with a notice.
func TestTrafficCarsDriveButDoNotRace(t *testing.T) {
	old := CarPolicy
	CarPolicy = "events"
	defer func() { CarPolicy = old }()
	r := newLoadRig(t, 2)
	cat := r.w.cat()
	traffic := int64(-1)
	for id := range cat.Blocked {
		traffic = int64(id)
		break
	}
	if traffic < 0 {
		t.Skip("the catalog blocks no car")
	}
	var mu sync.Mutex
	var sent []string
	r.w.Sinks.Notice = func(xuid uint64, text string, log func(string)) { mu.Lock(); sent = append(sent, text); mu.Unlock() }
	p := r.peers[0]
	if hide := r.w.CarModelSeen(p, traffic); hide || p.KickReason != nil {
		t.Fatalf("a traffic car was withheld (%v) or its owner kicked (%v)", hide, p.KickReason)
	}
	p.Choices = &Choices{true, 3, 0.7}
	if r.w.EligibleForEvent(p, &gameopts.Rule{Class: 255}) {
		t.Fatal("a traffic car was let into an event")
	}
	r.w.Roadtrip = true
	if r.w.EligibleForEvent(p, nil) {
		t.Fatal("a traffic car was let into Road Trip")
	}
	if r.count("such cars do not race in events or Road Trip") != 2 || r.count("it drives in Free Roam, events and Road Trip refuse it") != 1 {
		t.Fatalf("log: %q", r.log)
	}
	deadline := time.Now().Add(time.Second)
	for {
		mu.Lock()
		n := len(sent)
		mu.Unlock()
		if n > 0 || time.Now().After(deadline) {
			break
		}
		time.Sleep(5 * time.Millisecond)
	}
	mu.Lock()
	defer mu.Unlock()
	if len(sent) == 0 || !strings.Contains(sent[0], "Traffic cars") {
		t.Fatalf("notices %q", sent)
	}
}
