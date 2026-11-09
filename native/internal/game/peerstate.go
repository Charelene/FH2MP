package game

import (
	"fmt"

	"fh2mp/internal/diag"
	"fh2mp/internal/netv2"
)

// PeerState is where a connection stands. Until 2026-11-07 the booleans Connected and Entered, Owner == 0 and KickReason stood in for it, and a
// half-open connection (OpenConnection answered, ack never sent) was handled like a participant with relay owner id 0. The booleans stay as
// mirrors for the code that reads them; every change goes through World.setPeerState.
type PeerState uint8

const (
	PeerHalfOpen  PeerState = iota // OpenConnection answered; Owner == 0; objects are held, nothing is relayed
	PeerConnected                  // the ack echoed the server's counter after an accepted OpenConnection: Owner 1..127
	PeerEntered                    // FinaliseSessionEntry answered: a participant
	PeerLeaving                    // a kick is pending: KickForCars ends the connection on this tick
	PeerRemoved                    // DropPeer ran; the struct is dead
)

func (s PeerState) String() string {
	switch s {
	case PeerHalfOpen:
		return "half-open"
	case PeerConnected:
		return "connected"
	case PeerEntered:
		return "entered"
	case PeerLeaving:
		return "leaving"
	case PeerRemoved:
		return "removed"
	}
	return fmt.Sprintf("state %d", uint8(s))
}

// peerMoves is the transition table; anything else is a programming error and is refused.
var peerMoves = map[PeerState][]PeerState{
	PeerHalfOpen:  {PeerConnected, PeerLeaving, PeerRemoved},
	PeerConnected: {PeerEntered, PeerLeaving, PeerRemoved},
	PeerEntered:   {PeerLeaving, PeerRemoved},
	PeerLeaving:   {PeerRemoved},
	PeerRemoved:   {},
}

// setPeerState moves a peer and keeps the mirror booleans the way the old code wrote them (Connected on the ack, Entered at FinaliseSessionEntry,
// Connected off at removal; a pending kick changes neither). A move the table does not allow is refused with one WARN line.
func (w *World) setPeerState(p *Peer, to PeerState, trigger string) bool {
	from := p.State
	if from == to {
		return true
	}
	allowed := false
	for _, s := range peerMoves[from] {
		allowed = allowed || s == to
	}
	if !allowed {
		w.Warnf("%d peer state %s -> %s refused (%s): the connection stays %s (xuid %#x, owner %d)", p.Addr.Port, from, to, trigger, from, p.Xuid, p.Owner)
		return false
	}
	p.State = to
	switch to {
	case PeerConnected:
		p.Connected = true
	case PeerEntered:
		p.Connected, p.Entered = true, true
	case PeerRemoved:
		p.Connected = false
	}
	w.Debugf("state", "%d peer %s -> %s (%s; xuid %#x, owner %d)", p.Addr.Port, from, to, trigger, p.Xuid, p.Owner)
	return true
}

// leaving marks a kick pending (KickForCars ends the connection on this tick). No world is at hand where the car checks run, so no line here;
// the kick line names the reason.
func (p *Peer) leaving(reason *string) {
	p.KickReason = reason
	if p.State < PeerLeaving {
		p.State = PeerLeaving
	}
}

// minState says, per message type, the least state in which the message is acted on. Types not listed need a connection (the handshake ack):
// a half-open peer may only finish the handshake, keep time, ping, close, and send the object creates/updates that are held until it has a
// relay owner id (RememberAndRelay holds while Owner == 0).
var minState = map[int]PeerState{
	netv2.MsgOpenConnection:         PeerHalfOpen,
	netv2.MsgOpenConnectionResponse: PeerHalfOpen,
	netv2.MsgCloseConnection:        PeerHalfOpen,
	netv2.MsgPing:                   PeerHalfOpen,
	netv2.MsgTimeSync:               PeerHalfOpen,
	netv2.MsgConnectionSyncDone:     PeerHalfOpen,
	netv2.MsgObjectCreate:           PeerHalfOpen,
	netv2.MsgObjectUpdate:           PeerHalfOpen,
}

// Admits says whether a message of this type is acted on in this state. A pending kick (PeerLeaving) still admits what a connected peer admits:
// refusing there is the second step, after a hand check (the car policy traces have kicks).
func (s PeerState) Admits(mtype int) bool {
	switch s {
	case PeerRemoved:
		return false
	case PeerConnected, PeerEntered, PeerLeaving:
		return true
	}
	m, ok := minState[mtype]
	return ok && s >= m
}

// admit is the one place that decides whether a parsed message is acted on; a refusal is counted per type and said once per connection and
// 10 s (the totals go on the removal line).
func (w *World) admit(peer *Peer, mtype int) bool {
	if peer.State.Admits(mtype) {
		return true
	}
	peer.Refused[mtype]++
	w.LogEvery([2]any{"admit", peer.Addr}, 10.0, fmt.Sprintf("%d message type %d not handled: the connection is %s, it needs %s (xuid %#x); counted, the removal line states the total",
		peer.Addr.Port, mtype, peer.State, minStateName(mtype), peer.Xuid), diag.Warn)
	return false
}

func minStateName(mtype int) string {
	if m, ok := minState[mtype]; ok {
		return m.String()
	}
	return PeerConnected.String()
}

func refusedTotal(peer *Peer) int {
	n := 0
	for _, c := range peer.Refused {
		n += c
	}
	return n
}
