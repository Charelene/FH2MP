package xbl

import "testing"

// A car-pack player's stand-ins go to the game server as one control message; nothing is sent for an empty list or no player.
func TestAnnounceStandins(t *testing.T) {
	hub := &fakeHub{}
	s := New(hub)
	s.AnnounceStandins(0x90000e961ddae, "9024:2164/1215")
	s.AnnounceStandins(0, "9024:2164")
	s.AnnounceStandins(0x90000e961ddae, "")
	if len(hub.control) != 1 || hub.control[0] != "standins 90000e961ddae 9024:2164/1215" {
		t.Fatalf("control messages %q", hub.control)
	}
}
