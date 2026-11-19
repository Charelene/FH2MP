package game

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"sort"
)

// Mod state side channel (magic FH2MPMD1, shim/src/mods_wire.h; same ride on the game port as lighting and horns). Each player's game reports
// the state of its add-on mods that other players should see: NitroSystem's NOS burning (qiangqiang101/FH2-NitroSystem 2.0.0) and the lights of
// FH2-Underglow (qiangqiang101/FH2-Underglow 1.1.0). The reply carries the other players of the same world, and whether NOS may burn where this
// player is: user decision 2026-11-12, NOS is on in online Free Roam lobbies only, off in events, Rivals and Road Trip. The shim keeps
// NitroSystem's NOS switched off unless the server's last reply allows it, so a server without this channel leaves NOS off everywhere online.
const (
	modsMagic       = "FH2MPMD1"
	modsUG          = 28
	modsHead        = 55
	modsBase        = modsHead + modsUG
	modsRequest     = modsBase + sideMacSize
	modsRecord      = 4 + modsUG
	modsMaxRecs     = 12
	modsLease       = 1.0  // s a state stands without a refresh (the shim reports every 50-200 ms)
	modsMinInterval = 0.03 // s between two requests of one player that are acted on
	modsNosGrace    = 2.5  // s after the last allowed NOS burn the push check stays quiet (the boost ramps down; the check's window is 1 s)
	modsLogEvery    = 60.0

	modsNosBurning  = 0x01 // the player's NOS burns
	modsNosAllowed  = 0x02 // the shim lets it burn now
	modsNosSwitch   = 0x04 // NitroSystem with its off switch is loaded
	modsNosNoSwitch = 0x08 // a NitroSystem without the switch is loaded (its NOS cannot be turned off)
	modsNosAll      = 0x0F
	modsServerNosOK = 0x01

	modsUGLights    = 64
	modsUGEffects   = 61
	modsUGWidthMax  = 3000
	modsUGLengthMax = 6000
)

// ModState is a player's last mod state report.
type ModState struct {
	Epoch []byte
	Seq   uint64
	At    float64
	Car   int
	Nos   byte         // as reported, but the burning bit only where NOS is allowed
	UG    [modsUG]byte // the underglow state as reported (validated)
	NosAt float64      // the last report of an allowed NOS burn (0: none in this ticket epoch)
}

// NosAllowed says whether NitroSystem's NOS may burn for p: a Free Roam world (not a Road Trip, not a Car Meet) and not taking part in its event.
// FH2MP_NO_NOS=1 turns NOS off everywhere.
func (w *World) NosAllowed(p *Peer) bool {
	return !w.IsIdle && !w.Roadtrip && !w.Carmeet && !w.inEvent(p) && envStr("FH2MP_NO_NOS", "") == ""
}

// nosBurningRecently says whether p reported an allowed NOS burn within modsNosGrace (the push check does not read those windows).
func (w *World) nosBurningRecently(p *Peer) bool {
	st := p.Mods
	return st != nil && st.NosAt > 0 && w.Clk.Mono()-st.NosAt <= modsNosGrace
}

func modsS16(b []byte) int { return int(int16(binary.LittleEndian.Uint16(b))) }

// modsUGValid checks an underglow state against the rules of mods_wire.h.
func modsUGValid(ug []byte) bool {
	if len(ug) != modsUG || ug[0] > 1 || ug[1] > 15 || 2*int(ug[2])+int(ug[3])+int(ug[4]) > modsUGLights || ug[25] != 0 || ug[26] != 0 || ug[27] != 0 {
		return false
	}
	for _, e := range ug[5:9] {
		if e >= modsUGEffects {
			return false
		}
	}
	width, front, rear := int(binary.LittleEndian.Uint16(ug[17:])), modsS16(ug[19:]), modsS16(ug[21:])
	return width <= modsUGWidthMax && front <= modsUGLengthMax && front >= -modsUGLengthMax && rear <= modsUGLengthMax && rear >= -modsUGLengthMax && front >= rear
}

// modsParse checks a request's fields (its length, header and magic are checked by the caller).
func modsParse(data []byte) bool {
	return int(binary.LittleEndian.Uint16(data[9:11])) == len(data) && data[19] == 1 && data[52] <= 127 && data[53] <= modsNosAll && data[54] == 0 &&
		modsUGValid(data[modsHead:modsBase])
}

func (h *Hub) modsRoutes(only *World) map[uint64]Addr {
	if only == nil {
		return h.ModsRoutes
	}
	if only.modsRoutes == nil {
		only.modsRoutes = map[uint64]Addr{}
	}
	return only.modsRoutes
}

// modsStateLive says whether other's report stands: fresh, for the car the server has, from its live ticket.
func (h *Hub) modsStateLive(other *Peer, now float64) bool {
	st := other.Mods
	binding, ok := h.Bound[other.Addr]
	if st == nil || now-st.At > modsLease || other.CarID == nil || st.Car != *other.CarID || !other.Connected || !other.Entered ||
		!(1 <= other.Owner && other.Owner <= 254) || !ok || binding.xuid != other.Xuid || h.isRevoked(other.Xuid) || h.Keys[other.Xuid][binding.key] <= now {
		return false
	}
	return bytes.Equal(h.TicketEpochs[other.Addr], st.Epoch)
}

// modsRecordOf is other's record in a reply: the burning bit only while NOS is allowed for it now.
func modsRecordOf(world *World, other *Peer) []byte {
	st := other.Mods
	nos := st.Nos
	if !world.NosAllowed(other) {
		nos &^= modsNosBurning
	}
	rec := make([]byte, modsRecord)
	rec[0], rec[1], rec[2] = byte(other.Owner), byte(st.Car), nos
	copy(rec[4:], st.UG[:])
	return rec
}

func modsDescribe(nos byte, ug []byte) string {
	n := "no NitroSystem"
	switch {
	case nos&modsNosSwitch != 0:
		n = "NitroSystem"
	case nos&modsNosNoSwitch != 0:
		n = "NitroSystem without its off switch"
	}
	if nos&modsNosBurning != 0 {
		n += " burning"
	} else if nos&(modsNosSwitch|modsNosNoSwitch) != 0 && nos&modsNosAllowed == 0 {
		n += " off"
	}
	u := "underglow dark"
	if ug[0]&1 != 0 {
		u = fmt.Sprintf("underglow lit (parts %#x, %d/%d/%d LEDs, effects %d/%d/%d/%d)", ug[1], ug[2], ug[3], ug[4], ug[5], ug[6], ug[7], ug[8])
	}
	return n + ", " + u
}

// modsHandle consumes a mod state request: stores the player's state and answers with the others' of its world.
func (h *Hub) modsHandle(data []byte, addr Addr, only *World) {
	if len(data) != modsRequest || !bytes.Equal(data[:9], sideHeader[:9]) || string(data[11:19]) != modsMagic || envStr("FH2MP_NO_MODS", "") != "" {
		return
	}
	if !modsParse(data) {
		if ok, skipped := sideRate.Allow("md-malformed", rejectLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			modsLog.Warn(fmt.Sprintf("request with the mod state magic is malformed and ignored: length field %d of %d bytes, type %d (expected 1), car %d (max 127), NOS flags %#x (max %#x), reserved byte %d (expected 0), underglow % x%s",
				binary.LittleEndian.Uint16(data[9:11]), len(data), data[19], data[52], data[53], modsNosAll, data[54], data[modsHead:modsBase], extra))
		}
		return
	}
	xuid := binary.LittleEndian.Uint64(data[20:])
	seq := binary.LittleEndian.Uint64(data[28:])
	now := h.Clk.Mono()
	if seq == 0 || h.isRevoked(xuid) {
		return
	}
	world, peer, key := h.findAuthed(only, h.modsRoutes(only), xuid, data, now)
	if world == nil {
		if ok, skipped := sideRate.Allow([2]any{"md-unmatched", xuid}, rejectLogSeconds); ok {
			why, expected := h.whyUnmatched(xuid, data, now)
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			msg := fmt.Sprintf("request from xuid %d ignored: %s%s", xuid, why, extra)
			if expected {
				modsLog.Debug(msg)
			} else {
				modsLog.Warn(msg)
			}
		}
		return
	}
	epoch := append([]byte(nil), data[36:52]...)
	old := peer.Mods
	same := old != nil && bytes.Equal(old.Epoch, epoch)
	if same && (seq <= old.Seq || now-old.At < modsMinInterval) {
		return
	}
	carID := -1
	if peer.CarID != nil {
		carID = *peer.CarID
	}
	allowed := world.NosAllowed(peer)
	st := &ModState{Epoch: epoch, Seq: seq, At: now, Car: carID, Nos: data[53]}
	copy(st.UG[:], data[modsHead:modsBase])
	if same {
		st.NosAt = old.NosAt
	}
	if st.Nos&modsNosBurning != 0 {
		if allowed {
			st.NosAt = now
		} else {
			st.Nos &^= modsNosBurning
			if now-peer.ModsNosWarnedAt >= modsLogEvery || peer.ModsNosWarnedAt == 0 {
				peer.ModsNosWarnedAt = now
				where := "an event"
				switch {
				case world.Roadtrip:
					where = "a Road Trip"
				case world.Carmeet:
					where = "a Car Meet"
				}
				world.Warnf("mods: owner %d xuid %#x car %d reports NitroSystem's NOS burning in %s, where it is off (the shim's switch did not hold, or the report came before it saw the event); not shown to others",
					peer.Owner, peer.Xuid, carID, where)
			}
		}
	}
	peer.Mods = st
	if old == nil || old.Car != carID || old.Nos&^modsNosBurning != st.Nos&^modsNosBurning || old.UG != st.UG {
		peer.ModsChanges++
		if now-peer.ModsLoggedAt >= modsLogEvery || peer.ModsLoggedAt == 0 {
			world.Logf("mods: owner %d car %d %s (%d change(s) in the last minute)", peer.Owner, carID, modsDescribe(st.Nos, st.UG[:]), peer.ModsChanges)
			peer.ModsLoggedAt, peer.ModsChanges = now, 0
		}
	}
	others := world.Others(peer)
	if len(others) > modsMaxRecs && peer.Pos != nil && allFinite(*peer.Pos) {
		// more players than records: the nearest ones (a far car's lights and flames are not seen anyway)
		me := *peer.Pos
		dist := func(q *Peer) float64 {
			if q.Pos == nil || !allFinite(*q.Pos) {
				return 1e18
			}
			return dist3(*q.Pos, me)
		}
		sort.SliceStable(others, func(i, j int) bool { return dist(others[i]) < dist(others[j]) })
	}
	var records []byte
	n := 0
	for _, other := range others {
		if !h.modsStateLive(other, now) {
			continue
		}
		records = append(records, modsRecordOf(world, other)...)
		n++
		if n == modsMaxRecs {
			break
		}
	}
	reply := append([]byte(nil), data[:modsHead]...)
	reply[19], reply[53], reply[54] = 2, 0, byte(n)
	if allowed {
		reply[53] = modsServerNosOK
	}
	reply = append(reply, records...)
	binary.LittleEndian.PutUint16(reply[9:], uint16(len(reply)+sideMacSize))
	h.Sock.SendTo(signed(reply, key), addr)
}
