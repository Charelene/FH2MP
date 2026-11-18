package game

import (
	"fmt"

	"fh2mp/internal/diag"
)

// The Free Roam group route: the map's group waypoint, the game's group sat nav.
//
// From the game binary (1.0.0.64): the map's waypoint action (game+0xd6b430) raises GroupNavigateToPosition or GroupClearDestination;
// SatNavComponent turns the position into its nearest road node and sends message 52 SetGroupSatNavEvent with it, or with 0 for a clear
// (game+0xa52200 / +0xa4dd00). The game never draws the route from its own message: every game, the sender's too, plans the group route
// each frame to the node in SessionHostSynced.GroupSatNavNode, which only the server writes (game+0xa52ce0), so the server keeps it and
// sends it to everybody. The map offers the action in Free Roam only (SessionHost LobbyType 2 or 5), to anyone in a public game and only
// to the leader in a private one (game+0x6b2ee0); the server applies the same rule. A node the game does not know gives an empty route on
// the receiving side, no crash (the lookup game+0x1917bb0 returns null and the planner game+0xa52380 skips null nodes).

// groupRouteWorld says whether this world has a group route: Free Roam.
func (w *World) groupRouteWorld() bool { return !w.Roadtrip && !w.Carmeet }

// SetGroupSatNav handles message 52 from peer: node is the group route's road node, 0 = cleared.
func (w *World) SetGroupSatNav(peer *Peer, node int32) {
	if !w.groupRouteWorld() || peer.LobbyType != LobbyTypeFreeRoam {
		w.LogEvery([2]any{"groupnav-world", peer.Addr}, 60.0, fmt.Sprintf("%d group route (node %d) from owner %d ignored: the group route is a Free Roam feature (this world: road trip %s, car meet %s; the player's game is lobby type %d)",
			peer.Addr.Port, node, peer.Owner, pyBool(w.Roadtrip), pyBool(w.Carmeet), peer.LobbyType), diag.Info)
		return
	}
	who := "public game"
	if w.Private {
		if w.Leader != peer {
			leader := "nobody"
			if w.Leader != nil {
				leader = fmt.Sprintf("owner %d", w.Leader.Owner)
			}
			w.LogEvery([2]any{"groupnav-private", peer.Addr}, 30.0, fmt.Sprintf("%d group route (node %d) from owner %d ignored: this is a private game and only its leader (%s) sets the group route; the route stays node %d",
				peer.Addr.Port, node, peer.Owner, leader, w.GroupSatNav), diag.Info)
			return
		}
		who = "the private game's leader"
	}
	if node == w.GroupSatNav {
		return
	}
	old := w.GroupSatNav
	w.GroupSatNav = node
	sent := w.SendSessionHostVars([]shVar{{SHGroupSatNavNode, 32, uint64(uint32(node))}})
	if node == 0 {
		w.Logf("%d group route cleared by owner %d (%s; it was node %d): SessionHost GroupSatNavNode = 0 sent to %d player(s)", peer.Addr.Port, peer.Owner, who, old, sent)
		return
	}
	w.Logf("%d group route set to node %d by owner %d (%s; it was node %d): SessionHost GroupSatNavNode sent to %d player(s)", peer.Addr.Port, node, peer.Owner, who, old, sent)
}
