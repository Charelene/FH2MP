package game

import (
	"testing"
)

// A side-channel request is matched to its player's game connection: the address that verified last time when it still does, any other address of the
// same player when it does not (a reconnect), nobody when no live connection of that player signed it. And the work per request does not grow with the
// number of players on the server: with the last route still good, matching a request costs the same with 4 or 200 connections.
func TestSideAuthMatchesTheLastRouteFirstAndFallsBack(t *testing.T) {
	r := newModsRig(t, 4, false)
	p := r.peers[2]
	req := honkTestRequest(r.keys[2], modsVecEpoch, p.Xuid, 1, byte(*p.CarID), 1, 1, true)
	routes := map[uint64]Addr{}

	w, peer, key := r.hub.findAuthed(nil, routes, p.Xuid, req, 1000)
	if w != r.w || peer != p || string(key) != string(r.keys[2]) || routes[p.Xuid] != p.Addr {
		t.Fatalf("first request: world %v peer %v route %v", w, peer, routes[p.Xuid])
	}
	// the remembered route is used again
	if w, peer, _ = r.hub.findAuthed(nil, routes, p.Xuid, req, 1000); peer != p {
		t.Fatalf("second request matched %v", peer)
	}
	// a stale remembered route (another player's address) falls back to the scan and is corrected
	routes[p.Xuid] = r.peers[0].Addr
	if w, peer, _ = r.hub.findAuthed(nil, routes, p.Xuid, req, 1000); peer != p || routes[p.Xuid] != p.Addr {
		t.Fatalf("stale route: matched %v, route now %v", peer, routes[p.Xuid])
	}
	// signed with someone else's key: nobody, and the route is forgotten
	forged := honkTestRequest(r.keys[1], modsVecEpoch, p.Xuid, 2, byte(*p.CarID), 1, 1, true)
	if w, peer, _ = r.hub.findAuthed(nil, routes, p.Xuid, forged, 1000); w != nil || peer != nil {
		t.Fatalf("forged request matched %v", peer)
	}
	if _, ok := routes[p.Xuid]; ok {
		t.Fatal("a failed match keeps a route")
	}
	// a world filter: the player's world matches, another world does not
	other := &World{}
	if w, _, _ = r.hub.findAuthed(other, map[uint64]Addr{}, p.Xuid, req, 1000); w != nil {
		t.Fatal("matched outside the world asked for")
	}
}

func TestSideAuthCostDoesNotGrowWithTheServer(t *testing.T) {
	allocs := func(n int) float64 {
		r := newModsRig(t, n, false)
		p := r.peers[n-1]
		req := honkTestRequest(r.keys[n-1], modsVecEpoch, p.Xuid, 1, byte(*p.CarID), 1, 1, true)
		routes := map[uint64]Addr{}
		if _, peer, _ := r.hub.findAuthed(nil, routes, p.Xuid, req, 1000); peer != p {
			t.Fatalf("%d players: no match", n)
		}
		return testing.AllocsPerRun(200, func() { r.hub.findAuthed(nil, routes, p.Xuid, req, 1000) })
	}
	small, big := allocs(4), allocs(200)
	if big > small {
		t.Fatalf("matching a request with the last route still good allocates %.0f times with 200 connections and %.0f with 4", big, small)
	}
}
