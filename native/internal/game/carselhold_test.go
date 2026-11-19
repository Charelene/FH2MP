package game

import (
	"strings"
	"testing"

	"fh2mp/internal/bits"
	"fh2mp/internal/playersetup"
)

// prod 2026-11-11 20:37: one participant was still choosing its car 43.8 s after the start; the others could not report loaded before it left the
// car select. With CarselLateRelease the load clock starts when it leaves, so a slow loader keeps its full 30 s.
func TestLoadClockWaitsForALatePicker(t *testing.T) {
	r := newLoadRig(t, 3)
	fast, slow, late := r.peers[0], r.peers[1], r.peers[2]
	r.picked(fast, 0) // the group's deadline: 1 + 20 s, the hold ends 3 s later (24 s)
	r.picked(slow, 0)
	late.HasPlayerStatus, late.PlayerStatus = true, PlayerStatusCarSelect
	r.loaded(fast)
	r.at(40)
	r.picked(late, 43.8)
	late.PlayerStatus = PlayerStatusPreRace
	r.at(60) // 36 s after the hold's end, 16.2 s after the last pick: the old clock would have dropped the slow loader here
	if !r.in(slow) || !r.in(late) {
		t.Fatalf("dropped before 30 s from the last pick: %s; log %q", owners(r.w.Event.People), r.log)
	}
	r.loaded(late)
	r.at(74.5) // 30.7 s after the last pick
	if r.in(slow) {
		t.Fatalf("the slow loader was not dropped 30 s after the last pick: %s", owners(r.w.Event.People))
	}
}

// A participant that never leaves the car select holds the clock for CarselLateReleaseSeconds at most.
func TestLoadClockLatePickerIsBounded(t *testing.T) {
	r := newLoadRig(t, 3)
	fast, slow, stuck := r.peers[0], r.peers[1], r.peers[2]
	r.picked(fast, 0)
	r.picked(slow, 0)
	stuck.HasPlayerStatus, stuck.PlayerStatus = true, PlayerStatusCarSelect
	r.loaded(fast)
	r.at(70) // hold end 24 s + 30 s = 54 s: the clock started there, 16 s ago
	if !r.in(slow) || !r.in(stuck) {
		t.Fatalf("dropped too early: %s", owners(r.w.Event.People))
	}
	r.at(85) // 31 s after 54 s
	if r.in(slow) || r.in(stuck) || !r.in(fast) {
		t.Fatalf("expected both unloaded participants dropped at 54 + 30 s: %s", owners(r.w.Event.People))
	}
}

// The switch off restores the old clock: the hold's end, whatever the participants still choose.
func TestLoadClockLateReleaseOff(t *testing.T) {
	old := CarselLateRelease
	CarselLateRelease = false
	defer func() { CarselLateRelease = old }()
	r := newLoadRig(t, 3)
	r.picked(r.peers[0], 0)
	r.picked(r.peers[1], 0)
	r.peers[2].HasPlayerStatus, r.peers[2].PlayerStatus = true, PlayerStatusCarSelect
	r.loaded(r.peers[0])
	r.at(55) // 31 s after the hold's end
	if r.in(r.peers[1]) {
		t.Fatalf("old clock: owner2 should be dropped 30 s after the hold's end: %s", owners(r.w.Event.People))
	}
}

// Participants whose game wrote PreRace after the race request count as done at the start; a status left from before the request does not.
func TestPickedBeforeStartEndsTheHold(t *testing.T) {
	r := newLoadRig(t, 3)
	r.w.RaceRequestedAt = 980
	a, b, stale := r.peers[0], r.peers[1], r.peers[2]
	for _, p := range []*Peer{a, b} {
		p.HasPlayerStatus, p.PlayerStatus, p.PlayerStatusAt = true, PlayerStatusPreRace, 990
	}
	stale.HasPlayerStatus, stale.PlayerStatus, stale.PlayerStatusAt = true, PlayerStatusPreRace, 900
	r.w.notePickedBeforeStart(r.w.Event)
	ev := r.w.Event
	if ev.PickedAfter == nil || !ev.PickedAfter.Has(a) || !ev.PickedAfter.Has(b) || ev.PickedAfter.Has(stale) {
		t.Fatalf("picked before the start: want owners 1 and 2 only, log %q", r.log)
	}
	if hold, _, _ := r.w.CarselAnswer(a); !hold {
		t.Fatal("owner3's status is older than the race request: owner1 should still wait for it")
	}
	stale.PlayerStatusAt = 995
	r.w.notePickedBeforeStart(ev)
	if hold, _, _ := r.w.CarselAnswer(a); hold {
		t.Fatal("everybody picked before the start: the hold should be over")
	}
	if r.count("left it before the start") != 2 {
		t.Fatalf("log: %q", r.log)
	}
	old := CarselPickedBeforeStart
	CarselPickedBeforeStart = false
	defer func() { CarselPickedBeforeStart = old }()
	ev.PickedAfter = nil
	r.w.notePickedBeforeStart(ev)
	if ev.PickedAfter != nil {
		t.Fatal("switched off: nobody should be counted")
	}
}

// A race wait detail report (code 6) names the participants its game waits for when the console ids are the owner ids, and says so when they are not.
func TestRaceWaitReportNamesTheCar(t *testing.T) {
	r := newLoadRig(t, 3)
	me, other, stuck := r.peers[0], r.peers[1], r.peers[2]
	stuck.Look = CarLook{HasLivery: true, NewLivery: true, LiveryID: 7, TuneID: -1, Seen: 15}
	text := "race wait 20 s: still loading 1 of 2 racers [3:d]; racers 2,3; cache free 0 wait 1 load 0 done 11; session f0=1 e0=0 100=0"
	r.w.ClientReport(me, 6, 20, text, 0)
	want := "race wait detail [code 6, detail 20]"
	if r.count(want) != 1 || r.count("its game waits for owner3 xuid 0x66 car ") != 1 || r.count("livery 7 (HasLivery=1 IsNewLivery=1), tune -1") != 1 {
		t.Fatalf("log: %q", r.log)
	}
	_ = other
	r.w.ClientReport(me, 6, 40, "race wait 40 s: still loading 1 of 2 racers [17:d]; racers 16,17; cache free 0 wait 1 load 0 done 11; session f0=1 e0=0 100=0", 0)
	if r.count("the report's console ids 16,17 are not the other participants' owner ids [2, 3]") != 1 {
		t.Fatalf("log: %q", r.log)
	}
}

// The livery and tune fields of a player object are kept, shown in the load timeout lines, and a change during the load is logged.
func TestCarLookFromPlayerObject(t *testing.T) {
	r := newLoadRig(t, 2)
	p := r.peers[1]
	size, payload, err := playersetup.Encode(map[int]bits.Int{PlayerVarHasLivery: bits.FromUint64(1), PlayerVarIsNewLivery: bits.FromUint64(1),
		PlayerVarLiveryID: bits.FromUint64(48873), PlayerVarTuneID: bits.FromUint64(^uint64(0))})
	if err != nil {
		t.Fatal(err)
	}
	r.w.NoteCarLook(p, size, payload)
	want := "livery 48873 (HasLivery=1 IsNewLivery=1), tune -1"
	if got := p.Look.String(); got != want {
		t.Fatalf("look %q, want %q", got, want)
	}
	if r.count("changed its car's livery or tune while the event loads") != 1 {
		t.Fatalf("the change during the load was not logged: %q", r.log)
	}
	size, payload, _ = playersetup.Encode(map[int]bits.Int{PlayerVarIsNewLivery: bits.FromUint64(0)})
	r.w.NoteCarLook(p, size, payload)
	if p.Look.NewLivery || p.Look.LiveryID != 48873 {
		t.Fatalf("a delta must change only its own fields: %+v", p.Look)
	}
	if !strings.Contains(r.w.loadState(p, 1000), "livery 48873 (HasLivery=1 IsNewLivery=0), tune -1") {
		t.Fatalf("loadState: %s", r.w.loadState(p, 1000))
	}
	if got := (CarLook{}).String(); got != "livery and tune not written yet" {
		t.Fatalf("empty look: %q", got)
	}
}
