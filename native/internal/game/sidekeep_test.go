package game

import "testing"

type keepRecorder struct{ kept []Addr }

func (k *keepRecorder) HasConn(Addr) bool { return false }
func (k *keepRecorder) KeepSide(a Addr)   { k.kept = append(k.kept, a) }

// A side-channel request that a live connection of its player signed tells the TCP carrier to keep the connection it came over; a request nobody
// signed does not (it must not keep a tunnel alive for anyone who can reach the port).
func TestSideAuthKeepsTheTunnelOnlyForSignedRequests(t *testing.T) {
	r := newModsRig(t, 3, false)
	k := &keepRecorder{}
	r.hub.Tunnel = k
	p := r.peers[1]
	from := Addr{"127.0.0.1", 51234}
	good := honkTestRequest(r.keys[1], modsVecEpoch, p.Xuid, 1, byte(*p.CarID), 1, 1, true)
	if w, _, _ := r.hub.sideAuthed(nil, map[uint64]Addr{}, p.Xuid, good, 1000, true, from); w == nil {
		t.Fatal("a signed request was not matched")
	}
	if len(k.kept) != 1 || k.kept[0] != from {
		t.Fatalf("kept %v, want [%v]", k.kept, from)
	}
	forged := honkTestRequest(r.keys[0], modsVecEpoch, p.Xuid, 2, byte(*p.CarID), 1, 1, true)
	if w, _, _ := r.hub.sideAuthed(nil, map[uint64]Addr{}, p.Xuid, forged, 1000, true, from); w != nil {
		t.Fatal("a forged request was matched")
	}
	if len(k.kept) != 1 {
		t.Fatalf("a forged request kept a tunnel: %v", k.kept)
	}
	// a horn request over the tunnel's address goes through the handler and keeps it too
	r.hub.Handle(honkTestRequest(r.keys[1], modsVecEpoch, p.Xuid, 3, byte(*p.CarID), 1, 1, true), from)
	if len(k.kept) != 2 || k.kept[1] != from {
		t.Fatalf("the horn handler did not keep the tunnel: %v", k.kept)
	}
}
