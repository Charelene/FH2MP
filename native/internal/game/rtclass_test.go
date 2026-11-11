package game

import (
	"strings"
	"testing"
)

// rtClassRig is a Road Trip whose vote chose a class B location, in its event, with one class B and one class A car.
func rtClassRig(t *testing.T, mode string) (*loadRig, *Peer, *Peer) {
	r := newLoadRig(t, 2)
	prev := RtClassMode
	RtClassMode = mode
	t.Cleanup(func() { RtClassMode = prev })
	r.w.Roadtrip = true
	r.w.RtCandidates = &RtCand{Classes: [3]int{2, 3, 4}}
	r.w.RtWinner = 0
	r.w.RtState = RTInEvent
	fits, above := r.peers[0], r.peers[1]
	fits.ModelID, fits.Choices = i64p(1658), &Choices{true, 2, 0.6178}
	above.ModelID, above.Choices = i64p(1658), &Choices{true, 3, 0.7012} // the same car with an A class tune loaded mid-trip
	return r, fits, above
}

// Reported 2026-11-10: FH2 ANNA's Tune Car loaded higher-class tunes during a B class Road Trip. By default the server only logs such a car,
// once per owner and class every 5 min, until prod logs confirm that the vote's class counts like the car's.
func TestRoadTripClassLoggedOnly(t *testing.T) {
	r, fits, above := rtClassRig(t, "log")
	if ok, _ := r.w.carFitsEvent(fits, nil); !ok {
		t.Fatalf("a class B car was refused in a class B Road Trip: %q", r.log)
	}
	if ok, _ := r.w.carFitsEvent(above, nil); !ok {
		t.Fatalf("log mode refused the class A car: %q", r.log)
	}
	r.w.carFitsEvent(above, nil)
	if r.count("road trip class: owner 2") != 1 || r.count("is class A PI 701, above the trip's class B") != 1 || r.count("logged only") != 1 {
		t.Fatalf("want one log line for owner 2: %q", r.log)
	}
	if r.count("road trip class: owner 1") != 0 {
		t.Fatalf("the class B car was logged: %q", r.log)
	}
}

// FH2MP_RT_CLASS=enforce refuses the car for the trip's events, with a notice that names the trip's class.
func TestRoadTripClassEnforced(t *testing.T) {
	r, fits, above := rtClassRig(t, "enforce")
	if ok, _ := r.w.carFitsEvent(fits, nil); !ok {
		t.Fatalf("a class B car was refused: %q", r.log)
	}
	ok, notice := r.w.carFitsEvent(above, nil)
	if ok || !strings.Contains(notice, "above this Road Trip's class B") {
		t.Fatalf("enforce: fits=%v notice %q", ok, notice)
	}
}

// No check before the vote closed, during car select (the picker offers the trip's class), for a corrected rental, or with the switch off.
func TestRoadTripClassNotChecked(t *testing.T) {
	r, _, above := rtClassRig(t, "enforce")
	r.w.RtState = RTCarSelect
	if a, _ := r.w.RtClassAbove(above); a {
		t.Fatal("checked during car select")
	}
	r.w.RtState, r.w.RtWinner = RTInEvent, -1
	if a, _ := r.w.RtClassAbove(above); a {
		t.Fatal("checked before a vote closed")
	}
	r.w.RtWinner = 0
	above.Choices = &Choices{true, RentalClassID, 0.9999}
	if a, _ := r.w.RtClassAbove(above); a {
		t.Fatal("a rental's own class R counted as above")
	}
	above.Choices = &Choices{true, 3, 0.7012}
	RtClassMode = "off"
	if a, _ := r.w.RtClassAbove(above); a || r.count("road trip class") != 0 {
		t.Fatalf("checked with the switch off: %q", r.log)
	}
}
