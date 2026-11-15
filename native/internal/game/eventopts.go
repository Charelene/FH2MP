package game

import (
	"bytes"
	"encoding/binary"
	"fmt"

	"fh2mp/internal/diag"
)

// Event options side channel (magic FH2MPEO1, shim/src/eventopts_wire.h; the same ride on the game port as lighting, horns and the car select
// hold). The session leader's Event Options screen has a "Modded cars" row (shim/src/event_options.inc): Not allowed (the default) or Allowed. A
// car pack's own car (CustomCar: a car the game data does not have) never raced in an event; with the row set to Allowed it may, in that one
// Free Roam event, in a public session as in a private one. Road Trip never takes them.
//
// State, not events. Every client asks a few times a second and says what its leader row was set to when its player last started an event (0 Not
// allowed, 1 Allowed) for a few seconds after that start, and 255 the rest of the time. The server keeps the newest choice per player. When a
// player requests a race (MsgRequestRace) the choice it sent within eventoptsFresh seconds is the event's; a choice that arrives while that
// request is still pending (the datagram and the game's message race each other) is taken as well. The event's setting lasts until EndEvent. The
// reply tells every client of the world whether its pending or running event takes modded cars, so the shim's car pickers stop refusing them
// (pickerpi) and the row is only shown against a server that answers. A Road Trip world does not answer: no row there.
// FH2MP_NO_EVENTOPTS=1 turns it off (no answers, no row, custom cars refused as before).
const (
	eventoptsMagic       = "FH2MPEO1"
	eventoptsBase        = 58 // header 19, type 1, xuid 8, seq 8, epoch 16, choice / reply flags 1, reserved 1, reserved u32
	eventoptsRequest     = eventoptsBase + sideMacSize
	eventoptsMinInterval = 0.03
	eventoptsNone        = 255  // the request's choice byte: nothing to say
	eventoptsFresh       = 20.0 // s a choice counts for the race its player requests
	eventoptsModded      = 1    // reply flag: the world's pending or running event takes modded cars
)

var eventoptsLog = diag.Get("eventopts")

// EventoptsState is a player's last event options request, and the newest leader choice it carried (Choice eventoptsNone: none in this ticket epoch).
type EventoptsState struct {
	Epoch    []byte
	Seq      uint64
	At       float64
	Choice   byte
	ChoiceAt float64
}

func (h *Hub) eventoptsRoutes(only *World) map[uint64]Addr {
	if only == nil {
		return h.EvoRoutes
	}
	if only.eventoptsRoutes == nil {
		only.eventoptsRoutes = map[uint64]Addr{}
	}
	return only.eventoptsRoutes
}

// eventoptsStep is the player's state after one request (old: the previous state or nil).
func eventoptsStep(old *EventoptsState, epoch []byte, seq uint64, now float64, choice byte) *EventoptsState {
	st := &EventoptsState{Epoch: epoch, Seq: seq, At: now, Choice: eventoptsNone}
	if old != nil && bytes.Equal(old.Epoch, epoch) {
		st.Choice, st.ChoiceAt = old.Choice, old.ChoiceAt
	}
	if choice != eventoptsNone {
		st.Choice, st.ChoiceAt = choice, now
	}
	return st
}

// allowsModded says whether the player's newest leader choice is "Allowed" and fresh enough to belong to a race it requests now.
func (p *Peer) allowsModded(now float64) bool {
	st := p.Eventopts
	return st != nil && st.Choice == 1 && now-st.ChoiceAt <= eventoptsFresh
}

// SetModdedCars is the leader's Modded cars choice for the world's pending or running event; one line when it changes.
func (w *World) SetModdedCars(on bool, leader *Peer, why string) {
	if w.ModdedCars == on {
		return
	}
	w.ModdedCars = on
	word := "does not take"
	if on {
		word = "takes"
	}
	w.ev("modded_cars", kv("allowed", on), kv("leader", leader.Owner))
	w.Logf("event options: the event of leader owner %d %s modded cars (%s)", leader.Owner, word, why)
}

// customCarRefused: a car pack's own car that may not race here. Always in Road Trip; in a Free Roam event unless its leader allowed modded cars.
func (w *World) customCarRefused(peer *Peer) bool {
	return w.CustomCar(peer) && (w.Roadtrip || !w.ModdedCars)
}

// eventoptsHandle consumes an event options request: keeps the player's leader choice and answers with the world's setting.
func (h *Hub) eventoptsHandle(data []byte, addr Addr, only *World) {
	if len(data) != eventoptsRequest || !bytes.Equal(data[:9], sideHeader[:9]) || string(data[11:19]) != eventoptsMagic || envStr("FH2MP_NO_EVENTOPTS", "") != "" {
		return
	}
	lenField := int(binary.LittleEndian.Uint16(data[9:11]))
	if lenField != len(data) || data[19] != 1 || (data[52] > 1 && data[52] != eventoptsNone) || data[53] != 0 || !bytes.Equal(data[54:58], []byte{0, 0, 0, 0}) {
		if ok, skipped := sideRate.Allow("eo-malformed", rejectLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			eventoptsLog.Warn(fmt.Sprintf("request with the event options magic is malformed and ignored: length field %d of %d bytes, type %d (expected 1), choice %d (0, 1 or 255), reserved bytes %d and %x (expected 0)%s",
				lenField, len(data), data[19], data[52], data[53], data[54:58], extra))
		}
		return
	}
	xuid := binary.LittleEndian.Uint64(data[20:])
	seq := binary.LittleEndian.Uint64(data[28:])
	now := h.Clk.Mono()
	if seq == 0 || h.isRevoked(xuid) {
		return
	}
	// an event options request names no car: byte 52 is the choice
	world, peer, key := h.findAuthedCar(only, h.eventoptsRoutes(only), xuid, data, now, false)
	if world == nil {
		if ok, skipped := sideRate.Allow([2]any{"eo-unmatched", xuid}, rejectLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			eventoptsLog.Debug(fmt.Sprintf("request from xuid %d ignored: no live game connection of that player matches its address binding, ticket epoch and signature%s", xuid, extra))
		}
		return
	}
	epoch := append([]byte(nil), data[36:52]...)
	old := peer.Eventopts
	if old != nil && bytes.Equal(old.Epoch, epoch) && (seq <= old.Seq || now-old.At < eventoptsMinInterval) {
		return
	}
	peer.Eventopts = eventoptsStep(old, epoch, seq, now, data[52])
	if world.Roadtrip {
		return // Road Trip never takes custom cars: no answer, so the shim shows no row
	}
	// the leader's choice reaching the server after its race request: the event has not started, so nobody's car was finally refused yet
	if data[52] != eventoptsNone && world.PendingRace != nil && world.PendingRace.Leader == peer {
		world.SetModdedCars(data[52] == 1, peer, "its choice arrived after the race request")
	}
	reply := append([]byte(nil), data[:eventoptsBase]...)
	reply[19], reply[52] = 2, 0
	if world.ModdedCars {
		reply[52] = eventoptsModded
	}
	h.Sock.SendTo(signed(reply, key), addr)
}
