package game

import (
	"testing"

	"fh2mp/internal/netv2"
	"github.com/Charelene/encore-paddock/bits"
)

// The group route (the map's group waypoint; the game's group sat nav). The game sends message 52 SetGroupSatNavEvent with the road node
// of the destination (0 = cleared) from SatNavComponent (game+0xa52200 / +0xa4dd00); every game plans the group route to the node the
// server keeps in SessionHostSynced.GroupSatNavNode, var 12, a signed 32-bit int (SessionHostSynced ctor game+0x180ff70, read each frame
// by game+0xa52ce0). The map lets anyone in a public Free Roam set it and only the leader in a private one (game+0x6b2ee0).

// groupNav52 is message 52 as the game writes it (game+0x1815730): a SessionHost key (2 id bits) and the node, signed 32 bits.
func groupNav52(node int32) []byte {
	m := netv2.Bake(func(w *bits.Writer) {
		w.Bits(52, 7)
		w.Bits(0, 1)
		w.Bits(0, 1)
		w.Bits(0, 2)
		w.Bits(uint64(uint32(node)), 32)
	})
	return netv2.PackMessages([]netv2.Baked{m}, netv2.UnreliableID, 0, 0)
}

// groupNodesQueued lists the GroupSatNavNode values of the SessionHost updates queued for p, in order. A SessionHost update is router 6,
// key 0 0, object id 0 (2 bits), seq 8, size 14, then (var id 4 bits, value) pairs.
func groupNodesQueued(p *Peer) []int32 {
	var out []int32
	for _, m := range p.Outbox {
		if m.Type() != netv2.MsgObjectUpdate || m.V.Field(7, 4) != netv2.RouterLobby || m.V.Field(11, 2) != 0 || m.V.Field(13, 2) != 0 {
			continue
		}
		size := int(m.V.Field(23, 14))
		if size == 36 && m.V.Field(37, 4) == 12 {
			out = append(out, int32(uint32(m.V.Field(41, 32))))
		}
	}
	return out
}

func freeRoamRig(t *testing.T) (*stateRig, *Peer, *Peer) {
	r := newStateRig(t)
	key := &WorldKey{Type: LobbyTypeFreeRoam, Name: "a1b2c3d4"}
	mk := func(port, owner int) *Peer {
		p := r.halfOpen(port)
		p.Owner, p.LobbyType, p.World = owner, LobbyTypeFreeRoam, key
		p.Connected, p.Entered, p.State = true, true, PeerEntered
		return p
	}
	return r, mk(40001, 1), mk(40002, 2)
}

func sameNodes(got []int32, want ...int32) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if got[i] != want[i] {
			return false
		}
	}
	return true
}

// In a public Free Roam anyone sets the group route and everybody, the setter too, gets the node; a clear sends 0; the same node again
// sends nothing new.
func TestGroupRouteReachesEveryone(t *testing.T) {
	r, a, b := freeRoamRig(t)
	r.w.Handle(groupNav52(48213), b.Addr)
	for _, p := range []*Peer{a, b} {
		if got := groupNodesQueued(p); !sameNodes(got, 48213) {
			t.Fatalf("owner %d got group nodes %v, want [48213]", p.Owner, got)
		}
	}
	if !r.logHas("group route set to node 48213 by owner 2") {
		t.Errorf("no line for the new group route in %q", r.log)
	}
	r.w.Handle(groupNav52(48213), a.Addr)
	if got := groupNodesQueued(a); !sameNodes(got, 48213) {
		t.Errorf("the same node again was sent again: %v", got)
	}
	r.w.Handle(groupNav52(0), a.Addr)
	for _, p := range []*Peer{a, b} {
		if got := groupNodesQueued(p); !sameNodes(got, 48213, 0) {
			t.Fatalf("owner %d got group nodes %v after the clear, want [48213 0]", p.Owner, got)
		}
	}
	if !r.logHas("group route cleared by owner 1") {
		t.Errorf("no line for the clear in %q", r.log)
	}
	// a node is a signed 32-bit int on both sides
	r.w.Handle(groupNav52(-7), a.Addr)
	if got := groupNodesQueued(b); !sameNodes(got, 48213, 0, -7) {
		t.Errorf("a negative node arrived as %v", got)
	}
}

// In a private game only its leader sets or clears the group route, as the game's own map allows.
func TestGroupRouteInAPrivateGameIsTheLeaders(t *testing.T) {
	r, host, guest := privateRig(t)
	r.w.Handle(groupNav52(901), guest.Addr)
	if got := groupNodesQueued(host); len(got) != 0 {
		t.Fatalf("a guest set the private game's group route: %v", got)
	}
	if !r.logHas("only its leader (owner 1) sets the group route") {
		t.Errorf("no line for the guest's refused route in %q", r.log)
	}
	r.w.Handle(groupNav52(902), host.Addr)
	for _, p := range []*Peer{host, guest} {
		if got := groupNodesQueued(p); !sameNodes(got, 902) {
			t.Fatalf("owner %d got %v from the leader's route, want [902]", p.Owner, got)
		}
	}
}

// Road Trip and Car Meet worlds have no group route (the map offers it in Free Roam only); the message is read and nothing is sent.
func TestGroupRouteOnlyInFreeRoam(t *testing.T) {
	r, a, b := freeRoamRig(t)
	r.w.Roadtrip = true
	a.LobbyType, b.LobbyType = LobbyTypeRoadTrip, LobbyTypeRoadTrip
	r.w.Handle(groupNav52(55), a.Addr)
	r.w.Roadtrip, r.w.Carmeet = false, true
	a.LobbyType, b.LobbyType = LobbyTypeCarMeet, LobbyTypeCarMeet
	r.w.Handle(groupNav52(56), a.Addr)
	if got := append(groupNodesQueued(a), groupNodesQueued(b)...); len(got) != 0 {
		t.Fatalf("a group route was sent outside Free Roam: %v", got)
	}
	if a.MalformedCount != 0 {
		t.Errorf("message 52 counted as malformed")
	}
}

// A player who joins after the route was set gets it with the session vars at the handshake.
func TestGroupRouteForALateJoiner(t *testing.T) {
	r, a, _ := freeRoamRig(t)
	r.w.Handle(groupNav52(-2), a.Addr)
	late := r.halfOpen(40003)
	late.LobbyType = LobbyTypeFreeRoam
	if v, n := varsBits(r.w.sessionHostCreateVars(late)); n != 36 || v != 12|uint64(uint32(0xFFFFFFFE))<<4 {
		t.Fatalf("late joiner's session vars: %d bits %#x, want var 12 = -2 in 36 bits", n, v)
	}
	r.w.Handle(groupNav52(0), a.Addr)
	if r.w.sessionHostCreateVars(late) != nil {
		t.Error("a cleared route is still sent at the handshake")
	}
}
