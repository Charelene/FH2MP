package game

import (
	"bytes"
	"encoding/binary"
	"fmt"

	"fh2mp/internal/diag"
)

// The car select hold (server/carsel.py): a Free Roam participant that left its event car select early waits at the shim's cars-loaded wait
// until every participant has, so everybody starts loading together. Same side channel as lighting and horns, own magic.
const (
	carselMagic       = "FH2MPCS2"
	carselBase        = 58
	carselRequest     = carselBase + sideMacSize
	carselMinInterval = 0.03
	carselNoDeadline  = 0xFFFFFFFF
	// The group's car select deadline: the first participant's entry into the car select plus the client's own 20 s countdown; a participant that
	// left early waits until everybody has left it or the deadline plus a grace to reload a changed car (Python CARSEL_* constants).
	CarselSelectSeconds        = 20.0
	CarselEntryFallbackSeconds = 3.0
	CarselHoldGraceSeconds     = 3.0
)

var carselLog = diag.Get("carsel")

// CarselState is a player's last car select request (epoch, sequence number, arrival time).
type CarselState struct {
	Epoch []byte
	Seq   uint64
	At    float64
}

func (h *Hub) carselRoutes(only *World) map[uint64]Addr {
	if only == nil {
		return h.CarselRoutes
	}
	if only.carselRoutes == nil {
		only.carselRoutes = map[uint64]Addr{}
	}
	return only.carselRoutes
}

// carselHandle consumes a car select request: the signed answer says whether the shim's cars-loaded wait keeps waiting. Never forwarded.
func (h *Hub) carselHandle(data []byte, addr Addr, only *World) {
	if len(data) != carselRequest || !bytes.Equal(data[:9], sideHeader[:9]) || string(data[11:19]) != carselMagic || envStr("FH2MP_NO_CARSEL", "") != "" {
		return
	}
	lenField := int(binary.LittleEndian.Uint16(data[9:11]))
	if lenField != len(data) || data[19] != 1 || data[52] > 3 || data[53] != 0 || !bytes.Equal(data[54:58], []byte{0, 0, 0, 0}) {
		if ok, skipped := sideRate.Allow("cs-malformed", rejectLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			carselLog.Warn(fmt.Sprintf("request with the car select magic is malformed and ignored: length field %d of %d bytes, type %d (expected 1), state %d (max 3), reserved bytes %d and %x (expected 0)%s",
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
	world, peer, key := h.findAuthedCar(only, h.carselRoutes(only), xuid, data, now, false)
	if world == nil {
		if ok, skipped := sideRate.Allow([2]any{"cs-unmatched", xuid}, rejectLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			carselLog.Debug(fmt.Sprintf("request from xuid %d ignored: no live game connection of that player matches its address binding, ticket epoch and signature%s", xuid, extra))
		}
		return
	}
	epoch := append([]byte(nil), data[36:52]...)
	old := peer.Carsel
	if old != nil && bytes.Equal(old.Epoch, epoch) && (seq <= old.Seq || now-old.At < carselMinInterval) {
		return
	}
	peer.Carsel = &CarselState{Epoch: epoch, Seq: seq, At: now}
	reply := append([]byte(nil), data[:carselBase]...)
	hold, left, has := world.CarselAnswer(peer)
	reply[19], reply[52] = 2, 0
	if hold {
		reply[52] = 1
	}
	ms := uint32(carselNoDeadline)
	if has {
		ms = left
		if ms >= carselNoDeadline {
			ms = carselNoDeadline - 1
		}
	}
	binary.LittleEndian.PutUint32(reply[54:], ms)
	h.Sock.SendTo(signed(reply, key), addr)
}

// carselDeadline is the group's car select deadline (monotonic): the first participant's entry into the car select, plus the client's own 20 s.
func (ev *Event) carselDeadline() float64 {
	if ev.SelectEnteredAt != nil {
		return *ev.SelectEnteredAt + CarselSelectSeconds
	}
	return ev.Started + CarselEntryFallbackSeconds + CarselSelectSeconds
}

// CarselAnswer is what the shim of this player is told (server/carsel.py): keep waiting?, the ms left to the group's deadline, and whether there is
// a deadline at all. Only during a Free Roam event's setup. Waiting while a connected participant has not reached PreRace yet and the deadline plus
// CarselHoldGraceSeconds has not passed.
func (w *World) CarselAnswer(peer *Peer) (bool, uint32, bool) {
	ev := w.Event
	if w.Roadtrip || ev == nil || ev.Begun || !ev.has(peer) {
		return false, 0, false
	}
	now := w.Mono()
	deadline := ev.carselDeadline()
	left := uint32(0)
	if ms := int((deadline - now) * 1000); ms > 0 {
		left = uint32(ms)
	}
	var waiting []*Peer
	for _, q := range ev.People {
		if q.Connected && (ev.PickedAfter == nil || !ev.PickedAfter.Has(q)) {
			waiting = append(waiting, q)
		}
	}
	if len(waiting) == 0 {
		return false, left, true
	}
	if now >= deadline+CarselHoldGraceSeconds {
		if !ev.HoldLimitLogged {
			ev.HoldLimitLogged = true
			w.Logf("carsel: hold ends %.0f s after the group's deadline, still in the car select: %s", now-deadline, ownersList(waiting))
		}
		return false, left, true
	}
	if ev.Held == nil {
		ev.Held = map[*Peer]bool{}
	}
	var others []*Peer
	for _, q := range waiting {
		if q != peer {
			others = append(others, q)
		}
	}
	if len(others) > 0 && !ev.Held[peer] {
		ev.Held[peer] = true
		w.Logf("carsel: owner%d waits for %s to leave the car select", peer.Owner, ownersList(others))
	}
	return true, left, true
}

// ownersList is "owner1 owner2" (the Python " ".join(f"owner{q.owner_id}" ...)).
func ownersList(ps []*Peer) string {
	var b bytes.Buffer
	for i, q := range ps {
		if i > 0 {
			b.WriteByte(' ')
		}
		fmt.Fprintf(&b, "owner%d", q.Owner)
	}
	return b.String()
}
