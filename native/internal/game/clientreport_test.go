package game

import (
	"bytes"
	"encoding/binary"
	"math"
	"strings"
	"sync"
	"testing"
	"time"

	"fh2mp/internal/bits"
	"fh2mp/internal/gameopts"
)

// A client report is accepted once per sequence number (the shim sends it three times), within a budget per window, and a new ticket epoch starts afresh.
func TestClientReportsAccept(t *testing.T) {
	var r ClientReports
	epoch := bytes.Repeat([]byte{7}, 16)
	if ok, _ := r.Accept(epoch, 5, 100); !ok {
		t.Fatal("the first report was refused")
	}
	if ok, _ := r.Accept(epoch, 5, 100.4); ok {
		t.Fatal("a repeat of the same sequence number was accepted")
	}
	if ok, _ := r.Accept(epoch, 6, 100.5); !ok {
		t.Fatal("a new sequence number was refused")
	}
	if ok, _ := r.Accept(epoch, 5, 101); ok {
		t.Fatal("an older sequence number that is still remembered was accepted")
	}
	for seq := uint64(10); seq < 10+reportPerWindow; seq++ {
		r.Accept(epoch, seq, 102)
	}
	if ok, _ := r.Accept(epoch, 99, 103); ok {
		t.Fatalf("more than %d reports in one window were accepted", reportPerWindow)
	}
	if ok, _ := r.Accept(epoch, 100, 103); ok {
		t.Fatal("the budget came back inside the window")
	}
	ok, dropped := r.Accept(epoch, 101, 100+reportWindow+1)
	if !ok || dropped != 4 {
		t.Fatalf("the next window: accepted %v, %d dropped (want true, 4)", ok, dropped)
	}
	other := bytes.Repeat([]byte{8}, 16)
	if ok, _ := r.Accept(other, 101, 100+reportWindow+2); !ok {
		t.Fatal("a new ticket epoch (a new session) did not start afresh")
	}
}

func TestClientReportLogsAndShowsInLoadTimeout(t *testing.T) {
	r := newLoadRig(t, 3)
	for _, p := range r.peers {
		r.picked(p, 20)
	}
	r.loaded(r.peers[0])
	r.loaded(r.peers[1])
	slow := r.peers[2]
	r.clk.t = 1030
	r.w.ClientReport(slow, 1, 25, "the cars-loaded wait of this event has lasted 25 s; not held by the server", 0)
	if r.count("client report: owner 3 xuid 0x66, cars-loaded wait [code 1, detail 25]: the cars-loaded wait of this event has lasted 25 s") != 1 {
		t.Fatalf("no client report line: %q", r.log)
	}
	if !strings.Contains(strings.Join(r.log, "\n"), "event 30 s after its start, a participant, begun=false") {
		t.Fatalf("the report line does not say where the event stood: %q", r.log)
	}
	r.at(52)
	if r.in(slow) {
		t.Fatal("the slow player was not dropped")
	}
	if r.count("its game said 22 s ago (cars-loaded wait): the cars-loaded wait of this event has lasted 25 s") != 1 {
		t.Fatalf("the load timeout line lacks what the player's game said: %q", r.log)
	}
	if got := slow.lastReport(r.clk.t, 10); got != "" {
		t.Fatalf("an old report shows as current: %q", got)
	}
}

func TestRefusalAndDropNotices(t *testing.T) {
	r := newLoadRig(t, 2)
	var mu sync.Mutex
	var sent []string
	done := make(chan struct{}, 8)
	r.w.Sinks.Notice = func(xuid uint64, text string, log func(string)) {
		mu.Lock()
		sent = append(sent, text)
		mu.Unlock()
		done <- struct{}{}
	}
	p := r.peers[0]
	p.Choices = &Choices{true, 4, 0.8335}
	rule := gameopts.Rule{Class: 3, Min: 0, Max: 0}
	if r.w.EligibleForEvent(p, &rule) {
		t.Fatal("an S car was let into an A event")
	}
	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("the refused player got no notice")
	}
	if r.w.EligibleForEvent(p, &rule) {
		t.Fatal("still ineligible")
	}
	select {
	case <-done:
		t.Fatal("a second notice within a minute")
	case <-time.After(50 * time.Millisecond):
	}
	mu.Lock()
	text := sent[0]
	mu.Unlock()
	if !strings.Contains(text, "your car is class S1 834, the event needs class A") {
		t.Fatalf("the notice does not say why: %q", text)
	}
	if got := refusalText(gameopts.Rule{Class: 255, Min: 601, Max: 700}, &Choices{PI: math.NaN()}); !strings.Contains(got, "class unknown") || !strings.Contains(got, "any class with PI 601-700") {
		t.Fatalf("rule without a class or a car class: %q", got)
	}
	if classLetter(4) != "S1" || classLetter(5) != "S2" || classLetter(6) != "X" || classLetter(7) != "7" {
		t.Fatal("class letters")
	}
}

// An invitee whose Ready was refused on its car gets no notice while its game's car select can still switch cars; only the one still in a car
// that does not fit when the event starts gets it. The leader's car is checked again after loading, so the start sends the leader nothing.
// (2026-11-10: a player refused in a B 618 A45 AMG switched to a class A car 10 s later and raced, yet read "you were not let into the event".)
func TestRefusedReadyNoticeWaitsForTheStart(t *testing.T) {
	r := newLoadRig(t, 3)
	var mu sync.Mutex
	got := map[uint64][]string{}
	arrived := make(chan struct{}, 8)
	r.w.Sinks.Notice = func(xuid uint64, text string, log func(string)) {
		mu.Lock()
		got[xuid] = append(got[xuid], text)
		mu.Unlock()
		arrived <- struct{}{}
	}
	size, payload, err := gameopts.EncodeFields(map[int]gameopts.Field{30: {Width: 8, Value: bits.FromUint64(3)}})
	if err != nil {
		t.Fatal(err)
	}
	r.w.GameOptions = &gameopts.Options{Size: size, Payload: payload}
	r.w.Event = nil
	leader, fixed, stuck := r.peers[0], r.peers[1], r.peers[2]
	for _, p := range r.peers {
		p.Choices = &Choices{true, 2, 0.6178034543991089}
	}
	r.w.RaceRequestedAt = r.clk.t
	r.w.PendingRace = &PendingRace{Deadline: r.clk.t + RaceInviteSeconds, Leader: leader}
	for _, p := range []*Peer{fixed, stuck} {
		if fits, notice := r.w.carFitsEvent(p, nil); fits || !strings.Contains(notice, "your car is class B 618, the event needs class A") {
			t.Fatalf("B 618 against a class A rule: fits=%v notice %q", fits, notice)
		}
		at := r.w.RaceRequestedAt
		p.ReadyRetry = &at // a refused invite Ready (handle.go)
	}
	fixed.Choices = &Choices{true, 3, 0.8}
	r.w.RetryRefusedReady(fixed)
	if !fixed.ConsoleVars[4] || fixed.ReadyRetry != nil {
		t.Fatalf("the car change did not count the refused Ready: %q", r.log)
	}
	r.w.PendingRace = nil
	r.w.StartEvent(leader)
	if !r.in(leader) || !r.in(fixed) || r.in(stuck) {
		t.Fatalf("participants %s", owners(r.w.Event.People))
	}
	select {
	case <-arrived:
	case <-time.After(time.Second):
		t.Fatal("the invitee left out for its car got no notice")
	}
	time.Sleep(50 * time.Millisecond)
	mu.Lock()
	defer mu.Unlock()
	if len(got) != 1 || len(got[stuck.Xuid]) != 1 || !strings.Contains(got[stuck.Xuid][0], "your car is class B 618, the event needs class A") {
		t.Fatalf("notices %v, want one for owner %d only", got, stuck.Owner)
	}
}

// The side-channel request of the shim is parsed and checked before anything is logged.
func TestCarselReportMalformed(t *testing.T) {
	h := &Hub{}
	good := make([]byte, carselBase+5+sideMacSize)
	copy(good, sideHeader)
	copy(good[11:], carselMagic)
	good[19], good[52] = carselTypeReport, 1
	binary.LittleEndian.PutUint16(good[9:], uint16(len(good)))
	copy(good[carselBase:], "hello")
	for name, mutate := range map[string]func([]byte) []byte{
		"length field": func(b []byte) []byte { binary.LittleEndian.PutUint16(b[9:], 7); return b },
		"reserved":     func(b []byte) []byte { b[53] = 1; return b },
		"control byte": func(b []byte) []byte { b[carselBase] = 10; return b },
		"too long": func(b []byte) []byte {
			b = append(b, make([]byte, carselReportMaxText)...)
			binary.LittleEndian.PutUint16(b[9:], uint16(len(b)))
			return b
		},
	} {
		b := mutate(append([]byte(nil), good...))
		// With no clock and no tables the handler must stop at the shape check (a nil Clk would panic if it went further).
		h.carselReport(b, Addr{"127.0.0.1", 1}, nil)
		_ = name
	}
}
