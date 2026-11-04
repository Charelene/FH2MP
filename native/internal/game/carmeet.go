package game

import (
	"fmt"
	"sort"

	"fh2mp/internal/bits"
	"fh2mp/internal/netv2"
	"fh2mp/internal/omap"
)

type bitsWriter = bits.Writer

// Car Meet constants.
const (
	CMInviteNone     = 0
	CMInviteFreeRoam = 1
	CMInviteRoadTrip = 2
	CMJoinNone       = 0
	CMJoinWager      = 1
	CMJoinGamemode   = 2
	CMJoinWindowMs   = 20000
	CMPendingWait    = 1
)

func i64v(v int64) *int64 { return &v }

func (w *World) cmMembers() []*Peer {
	var out []*Peer
	for _, q := range w.Peers.Values() {
		if q.Connected && q.Entered && q.LobbyType == LobbyTypeCarMeet {
			out = append(out, q)
		}
	}
	return out
}

func (w *World) cmSlot(peer *Peer) int {
	order := sortedPeersByOwner(w.cmMembers())
	for i, q := range order {
		if q == peer {
			return i
		}
	}
	return 0
}

func (w *World) cmBroadcast(f netv2.CarMeetFields, only []*Peer) {
	targets := only
	if targets == nil {
		targets = w.cmMembers()
	}
	vars := netv2.CarMeetVarsWriter(f)
	for _, q := range targets {
		q.CmSeq = (q.CmSeq + 1) % 0xFF
		w.Send(q, false, netv2.CarMeetUpdate(uint64(q.CmSeq), vars))
	}
}

// CmInvite handles MsgCarMeetInviteRequest.
func (w *World) CmInvite(peer *Peer, inviteType int) {
	g := &w.Gm
	if inviteType == CMInviteFreeRoam || inviteType == CMInviteRoadTrip {
		if g.Phase != "idle" {
			w.Logf("%d CarMeet invite ignored: a game-mode invite is already %s", peer.Addr.Port, g.Phase)
			return
		}
		g.Type, g.Phase, g.Starter = inviteType, "joining", peer
		g.End = (w.NowMs() + CMJoinWindowMs) & 0xFFFFFFFF
		g.Joined = omap.New[*Peer, int]()
		g.Joined.Set(peer, CMJoinGamemode)
		g.Session = nil
		w.cmBroadcast(netv2.CarMeetFields{InviteType: i64v(int64(inviteType)), Countdown: i64v(CMJoinWindowMs), ModeEnd: i64v(g.End),
			Joins: map[int]int{w.cmSlot(peer): CMJoinGamemode}}, nil)
		w.Logf("CarMeet: game-mode invite type %d started by owner %d, join window %d ms", inviteType, peer.Owner, CMJoinWindowMs)
	} else {
		w.Logf("%d CarMeet showdown (wager race) invite: not served yet", peer.Addr.Port)
	}
}

func (w *World) cmJoins() map[int]int {
	out := map[int]int{}
	for _, q := range w.cmMembers() {
		v := CMJoinNone
		if w.Gm.Joined.Has(q) {
			v = CMJoinGamemode
		}
		out[w.cmSlot(q)] = v
	}
	return out
}

// CmJoin handles MsgCarMeetJoinOrCancel.
func (w *World) CmJoin(peer *Peer, join, joinType int) {
	g := &w.Gm
	if joinType == CMJoinWager || g.Phase != "joining" {
		return
	}
	// The bit means the opposite of its name for the game-mode invite (observed in game 2026-10-17).
	if join != 0 {
		join = 0
	} else {
		join = 1
	}
	if join != 0 {
		g.Joined.Set(peer, CMJoinGamemode)
	} else {
		g.Joined.Delete(peer)
	}
	w.cmBroadcast(netv2.CarMeetFields{Joins: w.cmJoins()}, nil)
	verb := "left"
	if join != 0 {
		verb = "joined"
	}
	w.Logf("CarMeet: owner %d %s the game-mode invite (%d joined)", peer.Owner, verb, g.Joined.Len())
}

// CmTick runs the game-mode invite timers; nowMs is the world's network clock.
func (w *World) CmTick(nowMs int64) {
	g := &w.Gm
	if g.Phase == "ready" && ((nowMs-g.Reset)&0xFFFFFFFF) < 0x80000000 {
		g.Phase, g.Type, g.Starter, g.Joined, g.Session = "idle", 0, nil, omap.New[*Peer, int](), nil
		w.cmBroadcast(netv2.CarMeetFields{InviteType: i64v(CMInviteNone)}, nil)
		return
	}
	if g.Phase == "joining" && nowMs-g.Snap >= 1000 {
		g.Snap = nowMs
		w.cmBroadcast(netv2.CarMeetFields{Joins: w.cmJoins()}, nil)
	}
	if g.Phase == "joining" && ((nowMs-g.End)&0xFFFFFFFF) < 0x80000000 {
		starter := g.Starter
		var joined []*Peer
		for _, q := range g.Joined.Keys() {
			if q.Connected {
				joined = append(joined, q)
			}
		}
		if starter == nil || !starter.Connected || len(joined) == 0 {
			w.Log("CarMeet: game-mode invite cancelled (nobody left)")
			g.Phase, g.Type, g.Starter, g.Joined = "idle", 0, nil, omap.New[*Peer, int]()
			w.cmBroadcast(netv2.CarMeetFields{InviteType: i64v(CMInviteNone)}, nil)
			return
		}
		g.Phase = "creating"
		for _, q := range joined {
			w.Send(q, false, netv2.CarMeetNotifyPending(CMPendingWait))
		}
		var xuids []uint64
		for _, q := range joined {
			if q.Xuid != 0 {
				xuids = append(xuids, q.Xuid)
			}
		}
		name, template, err := "", "", fmt.Errorf("no session service")
		if w.Sinks != nil && w.Sinks.Gamemode != nil {
			name, template, err = w.Sinks.Gamemode(g.Type, xuids)
		}
		if err != nil {
			w.Logf("CarMeet: could not create a game-mode session through the LSP: %v", err)
			name = ""
		}
		if name == "" {
			g.Phase, g.Type, g.Starter, g.Joined = "idle", 0, nil, omap.New[*Peer, int]()
			w.cmBroadcast(netv2.CarMeetFields{InviteType: i64v(CMInviteNone)}, nil)
			return
		}
		g.Phase = "ready"
		g.Session = &[2]string{name, template}
		g.Reset = (nowMs + 30000) & 0xFFFFFFFF
		for _, q := range joined {
			if w.Hub != nil {
				w.Hub.Assign[q.Xuid] = name
			}
		}
		w.cmBroadcast(netv2.CarMeetFields{Session: &name, Template: &template}, nil)
		for _, q := range joined {
			w.Send(q, false, netv2.CarMeetSessionReady(name, template))
		}
		w.Logf("CarMeet: join window over (%d joined): session %s (%s) sent to %s", len(joined), name, template, owners(joined))
	}
}

// CmSessionReady handles MsgCarMeetSessionReady.
func (w *World) CmSessionReady(peer *Peer, name, template string) {
	g := &w.Gm
	w.Logf("CarMeet: GameModeSessionReady from owner %d: session %s template %s", peer.Owner, pyReprStr(name), pyReprStr(template))
	if g.Phase != "creating" || peer != g.Starter {
		return
	}
	g.Phase = "ready"
	g.Session = &[2]string{name, template}
	w.cmBroadcast(netv2.CarMeetFields{Session: &name, Template: &template}, nil)
	for _, q := range g.Joined.Keys() {
		if q != peer && q.Connected {
			w.Send(q, false, netv2.CarMeetSessionReady(name, template))
		}
	}
}

var _ = sort.Ints
