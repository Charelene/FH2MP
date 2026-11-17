package game

// FH2MP Eliminator — server groundwork (DRAFT). See docs/eliminator/design.md.
//
// This is the server half of the Eliminator battle-royale mode: an authoritative state machine per
// world, plus the FH2MPEL1 side-channel handler that carries the FH2MP-specific state (join/leave,
// challenge/accept, car-drop claims, eliminations, the shrink clock). It mirrors the shape of the
// other side channels (carsel.go / modsync.go) and of a world mode (roadtrip.go / carmeet.go).
//
// Groundwork status: the wire framing, authentication, reply snapshot and the state-machine skeleton
// are here and compile. A match is a Free Roam world of its own whose name starts with
// ElimWorldPrefix: only such a world has an Eliminator (Hub.WorldFor) and answers the side channel.
// There is no lobby type for the mode: the type only picks the session the game builds, and the six
// the game knows are taken. What is deliberately NOT wired yet (the next steps, all documented in
// docs/eliminator/IMPLEMENTATION.md):
//   - matchmaking: something that names a match's world and sends its players there. Today that is
//     the control message "assign <xuid> elim-...", sent by hand, as for a private Free Roam;
//   - spawning/collecting car-drop pickups (net-new: the game's entity/object creation path);
//   - driving the cinematic cameras (net-new; the client anchors the cam-anim transform).

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"

	"fh2mp/internal/diag"
)

// ElimWorldPrefix starts the name of every Eliminator match's world (a Free Roam world, lobby type 2).
// Private games' names are UUIDs, so no other lobby's name starts with it.
const ElimWorldPrefix = "elim-"

// Side-channel constants, kept in lockstep with shim/src/eliminator_wire.h.
const (
	elimMagic       = "FH2MPEL1"
	elimBase        = 58
	elimRequest     = elimBase + sideMacSize
	elimYou         = 4 // reply only, after the base: the asking player's tier, challenger owner id, 2 reserved
	elimRecord      = 12
	elimDropMax     = 24
	elimLimit       = elimBase + elimYou + elimRecord*elimDropMax + sideMacSize
	elimMinInterval = 0.05 // s between two requests of one player that are acted on
)

// Reply flags (reply byte 56), mirrored by EL_FLAG_* in eliminator_wire.h.
const (
	elimFlagShrinking  = 1 // the play area still has steps to shrink
	elimFlagEliminated = 2 // the asking player is out
	elimFlagJoined     = 4 // the asking player is in the mode
)

// Eliminator phases (reply byte 53), mirrored by EL_PHASE_* in eliminator_wire.h.
const (
	ElimPhaseNone       = 0
	ElimPhaseWaiting    = 1
	ElimPhaseDropping   = 2
	ElimPhaseActive     = 3
	ElimPhaseHeadToHead = 4
	ElimPhaseFinished   = 5
)

// Eliminator commands (request byte 53), mirrored by EL_CMD_* in eliminator_wire.h.
const (
	ElimCmdJoin      = 1
	ElimCmdLeave     = 2
	ElimCmdReady     = 3
	ElimCmdChallenge = 4 // arg0 = target owner id (1..254)
	ElimCmdAccept    = 5 // arg0 = challenger owner id
	ElimCmdDecline   = 6 // arg0 = challenger owner id
	ElimCmdDropClaim = 7 // arg0 = drop id
)

// Mode tuning (env-overridable in a later pass, like Road Trip's FH2MP_RT_*). Groundwork values.
const (
	ElimTiers          = 6     // car class tiers, D (0) .. S2 (5)
	ElimDropCount      = 12    // car drops on the map at once
	ElimDropRespawnS   = 30.0  // a claimed drop respawns after this long
	ElimShrinkSteps    = 5     // play-area shrink levels while active
	ElimShrinkStepS    = 60.0  // seconds between shrink steps
	ElimWaitMinPlayers = 2     // players needed to leave Waiting
	ElimDropDelayS     = 3.0   // Waiting -> Dropping -> Active once drops are placed
	ElimHeadToHeadS    = 120.0 // a pending challenge's time limit
	ElimArenaRadius    = 1500.0
)

var elimLog = diag.Get("eliminator")

// EliminatorDrop is one car drop: a class-tier pickup the server places in the world.
// Groundwork: placement is not yet implemented; the client already decodes these records.
type EliminatorDrop struct {
	ID    uint8
	Class uint8
	X, Z  float32
}

// EliminatorPlayer is one participant's Eliminator state, keyed by peer.Owner.
type EliminatorPlayer struct {
	Owner        int
	Tier         int // car class tier 0..ElimTiers-1
	ChallengedBy int // owner id of the challenger, 0 = none
	ChallengedAt float64
	Eliminated   bool
}

// Eliminator is the server-authoritative state of one world's Eliminator mode. It is owned by that
// world's goroutine (the one-world-one-goroutine rule); the side-channel handler and World.Tick are
// the only callers.
type Eliminator struct {
	Phase   int
	Area    int // shrink level 0..ElimShrinkSteps
	Players map[int]*EliminatorPlayer
	Drops   []EliminatorDrop

	nextDropID uint8
	dropAt     float64 // monotonic: next drop respawn/placement
	shrinkAt   float64 // monotonic: next shrink step
	phaseAt    float64 // monotonic: when the current phase began
	rng        Rand
}

// NewEliminator makes a waiting Eliminator for a world.
func NewEliminator(rng Rand) *Eliminator {
	return &Eliminator{Phase: ElimPhaseWaiting, Players: map[int]*EliminatorPlayer{}, rng: rng}
}

// remaining counts the players still in the mode (not eliminated).
func (e *Eliminator) remaining() int {
	n := 0
	for _, p := range e.Players {
		if !p.Eliminated {
			n++
		}
	}
	return n
}

// placeDrops fills the drop list up to ElimDropCount inside the arena circle. Placement is
// deterministic per world (the world's Rand), so every client can agree with the server's list.
// Groundwork: positions are random around the festival hub; a real map needs the arena boundary.
func (e *Eliminator) placeDrops() {
	for len(e.Drops) < ElimDropCount {
		e.nextDropID++
		ang := e.rng.Float64() * 2 * math.Pi
		rad := math.Sqrt(e.rng.Float64()) * ElimArenaRadius
		tier := e.rng.Intn(ElimTiers)
		e.Drops = append(e.Drops, EliminatorDrop{
			ID:    e.nextDropID,
			Class: uint8(tier),
			X:     float32(math.Cos(ang) * rad),
			Z:     float32(math.Sin(ang) * rad),
		})
	}
	e.dropAt = 0
}

// claimDrop awards a drop's class tier and schedules its respawn. No distance check yet: the pickup
// detection and radius are TODO-RE on the client, which sends the claim once it detects the pickup.
func (e *Eliminator) claimDrop(id uint8, p *EliminatorPlayer, now float64) {
	for i, d := range e.Drops {
		if d.ID == id {
			if int(d.Class) > p.Tier {
				p.Tier = int(d.Class)
			}
			e.Drops = append(e.Drops[:i], e.Drops[i+1:]...)
			e.dropAt = now + ElimDropRespawnS
			return
		}
	}
}

// Tick advances the mode: Waiting -> Dropping -> Active -> Finished, with the shrinking play area and
// drop respawns. Head-to-head races themselves are started by the game protocol (TODO); Tick only
// expires stale challenges.
func (e *Eliminator) Tick(now float64) {
	switch e.Phase {
	case ElimPhaseWaiting:
		if len(e.Players) >= ElimWaitMinPlayers {
			e.Phase = ElimPhaseDropping
			e.phaseAt = now
			e.placeDrops()
		}
	case ElimPhaseDropping:
		if now-e.phaseAt >= ElimDropDelayS {
			e.Phase = ElimPhaseActive
			e.phaseAt = now
			e.shrinkAt = now + ElimShrinkStepS
		}
	case ElimPhaseActive:
		if e.shrinkAt > 0 && now >= e.shrinkAt && e.Area < ElimShrinkSteps {
			e.Area++
			e.shrinkAt = now + ElimShrinkStepS
		}
		if e.dropAt > 0 && now >= e.dropAt {
			e.placeDrops()
		}
		for _, p := range e.Players {
			if p.ChallengedBy != 0 && now-p.ChallengedAt >= ElimHeadToHeadS {
				p.ChallengedBy, p.ChallengedAt = 0, 0
			}
		}
		if e.remaining() <= 1 {
			e.Phase = ElimPhaseFinished
		}
	}
}

// Handle applies one player command. The game-protocol pieces (actually starting a head-to-head race,
// eliminating the loser) are TODO; the bookkeeping around them is here.
func (e *Eliminator) Handle(cmd int, arg0, arg1 uint32, peer *Peer, now float64) {
	_ = arg1
	if peer == nil {
		return
	}
	switch cmd {
	case ElimCmdJoin:
		if _, ok := e.Players[peer.Owner]; !ok {
			e.Players[peer.Owner] = &EliminatorPlayer{Owner: peer.Owner}
		}
	case ElimCmdLeave:
		e.Forget(peer.Owner)
	case ElimCmdChallenge:
		// arg0 0 ("the nearest player") is resolved by the caller, which knows the cars' positions
		if p, ok := e.Players[peer.Owner]; ok && !p.Eliminated && int(arg0) != peer.Owner {
			if t, ok := e.Players[int(arg0)]; ok && !t.Eliminated && t.ChallengedBy == 0 {
				t.ChallengedBy = peer.Owner
				t.ChallengedAt = now
			}
		}
	case ElimCmdAccept:
		// arg0 0 = whoever challenged this player (the shim's pad chord does not know the owner id)
		if p, ok := e.Players[peer.Owner]; ok && p.ChallengedBy != 0 && (arg0 == 0 || p.ChallengedBy == int(arg0)) {
			// TODO: start the private 2-player race to a destination inside the play area.
			p.ChallengedBy, p.ChallengedAt = 0, 0
		}
	case ElimCmdDecline:
		if p, ok := e.Players[peer.Owner]; ok && p.ChallengedBy != 0 && (arg0 == 0 || p.ChallengedBy == int(arg0)) {
			p.ChallengedBy, p.ChallengedAt = 0, 0
		}
	case ElimCmdDropClaim:
		if p, ok := e.Players[peer.Owner]; ok && !p.Eliminated {
			e.claimDrop(uint8(arg0), p, now)
		}
	}
}

// Forget takes a player out of the mode (a Leave command, or the player's connection went): the owner
// id is free for the next player, and a challenge from it no longer stands.
func (e *Eliminator) Forget(owner int) {
	delete(e.Players, owner)
	for _, p := range e.Players {
		if p.ChallengedBy == owner {
			p.ChallengedBy, p.ChallengedAt = 0, 0
		}
	}
}

// snapshot builds the reply payload for the player with this owner id (58-byte base, the 4-byte
// block about that player, then the drop records; without the MAC). The caller signs it with
// signed() and sends it, exactly like carselHandle.
func (e *Eliminator) snapshot(data []byte, owner int) []byte {
	n := len(e.Drops)
	if n > elimDropMax {
		n = elimDropMax
	}
	reply := make([]byte, elimBase+elimYou+elimRecord*n)
	copy(reply, data[:elimBase])
	// the length field counts the whole datagram: the request's own would make the shim refuse every reply that is longer than a request
	binary.LittleEndian.PutUint16(reply[9:], uint16(len(reply)+sideMacSize))
	reply[19] = 2 // type 2 = reply
	reply[53] = byte(e.Phase)
	reply[54] = byte(e.remaining())
	reply[55] = byte(e.Area)
	reply[56] = 0
	if e.Phase == ElimPhaseActive && e.Area < ElimShrinkSteps {
		reply[56] |= elimFlagShrinking
	}
	reply[57] = byte(n)
	if p, ok := e.Players[owner]; ok {
		reply[56] |= elimFlagJoined
		if p.Eliminated {
			reply[56] |= elimFlagEliminated
		}
		reply[elimBase], reply[elimBase+1] = byte(p.Tier), byte(p.ChallengedBy)
	}
	for i := 0; i < n; i++ {
		rec := reply[elimBase+elimYou+elimRecord*i : elimBase+elimYou+elimRecord*(i+1)]
		d := e.Drops[i]
		rec[0], rec[1] = d.ID, d.Class
		binary.LittleEndian.PutUint32(rec[2:], math.Float32bits(d.X))
		binary.LittleEndian.PutUint32(rec[6:], math.Float32bits(d.Z))
	}
	return reply
}

// eliminatorRoutes is the per-feature xuid -> address cache, like carselRoutes.
func (h *Hub) eliminatorRoutes(only *World) map[uint64]Addr {
	if only == nil {
		return h.ElimRoutes
	}
	if only.eliminatorRoutes == nil {
		only.eliminatorRoutes = map[uint64]Addr{}
	}
	return only.eliminatorRoutes
}

// elimNearest is the owner id of the nearest other player still in the mode, 0 when there is none or
// the asking player's car position is not known: who a challenge without a target is for.
func (w *World) elimNearest(peer *Peer) int {
	here, ok := peer.CarPos()
	if !ok {
		return 0
	}
	best, bestD := 0, math.Inf(1)
	for _, o := range w.Peers.Values() {
		p, in := w.El.Players[o.Owner]
		pos, known := o.CarPos()
		if o == peer || !in || p.Eliminated || !known {
			continue
		}
		dx, dy, dz := pos[0]-here[0], pos[1]-here[1], pos[2]-here[2]
		if d := dx*dx + dy*dy + dz*dz; d < bestD {
			best, bestD = o.Owner, d
		}
	}
	return best
}

// eliminatorHandle consumes an Eliminator request and answers with the world's snapshot. Never
// forwarded. Only a world that hosts the mode (World.El) answers: in any other world the request is
// dropped without a reply, which is what tells the shim to keep its Eliminator HUD and buttons off.
func (h *Hub) eliminatorHandle(data []byte, addr Addr, only *World) {
	if len(data) != elimRequest || !bytes.Equal(data[:9], sideHeader[:9]) || string(data[11:19]) != elimMagic || envStr("FH2MP_NO_ELIMINATOR", "") != "" {
		return
	}
	lenField := int(binary.LittleEndian.Uint16(data[9:11]))
	if lenField != len(data) || data[19] != 1 || data[52] > 127 || data[57] != 0 {
		if ok, skipped := sideRate.Allow("el-malformed", rejectLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			elimLog.Warn(fmt.Sprintf("request with the eliminator magic is malformed and ignored: length field %d of %d bytes, type %d (expected 1), car %d (max 127), reserved byte %d (expected 0)%s",
				lenField, len(data), data[19], data[52], data[57], extra))
		}
		return
	}
	xuid := binary.LittleEndian.Uint64(data[20:])
	seq := binary.LittleEndian.Uint64(data[28:])
	now := h.Clk.Mono()
	if seq == 0 || h.isRevoked(xuid) {
		return
	}
	world, peer, key := h.findAuthedCar(only, h.eliminatorRoutes(only), xuid, data, now, false)
	if world == nil {
		if ok, skipped := sideRate.Allow([2]any{"el-unmatched", xuid}, rejectLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			elimLog.Debug(fmt.Sprintf("request from xuid %d ignored: no live game connection of that player matches its address binding, ticket epoch and signature%s", xuid, extra))
		}
		return
	}
	if world.El == nil {
		return
	}
	// one request is acted on once, and not more often than the shim polls (carselHandle's rule): a recorded Join or drop claim cannot be sent again
	epoch := append([]byte(nil), data[36:52]...)
	if old := peer.Elim; old != nil && bytes.Equal(old.Epoch, epoch) && (seq <= old.Seq || now-old.At < elimMinInterval) {
		return
	}
	peer.Elim = &CarselState{Epoch: epoch, Seq: seq, At: now}
	cmd := data[53]
	arg0 := uint32(data[54]) | uint32(data[55])<<8
	arg1 := uint32(data[56])
	if cmd == ElimCmdChallenge && arg0 == 0 {
		arg0 = uint32(world.elimNearest(peer))
	}
	world.El.Handle(int(cmd), arg0, arg1, peer, now)
	h.Sock.SendTo(signed(world.El.snapshot(data, peer.Owner), key), addr)
}
