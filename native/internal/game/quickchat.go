package game

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"unicode/utf8"

	"fh2mp/internal/diag"
)

// Quick chat side channel (magic FH2MPQC1, shim/src/quickchat_wire.h; the same ride on the game port as lighting, horns and mod state). A client
// says what the newest message its player sent is ("message 4, phrase 2"), never an event: it repeats that until the reply shows it was taken, so
// a lost datagram heals on the next one. The reply lists the live messages of the other players of the world (of the same event, when in one),
// each with its sender's gamertag, so every client shows each (player, message) once. Only a phrase number crosses the network: every client has
// the same fixed list (the QuickChat mod, chosen from FH2 ANNA's menu), so there is no free text to check. FH2MP_NO_QUICKCHAT=1 turns it off.
const (
	quickchatMagic       = "FH2MPQC1"
	quickchatBase        = 58 // header 19, type 1, xuid 8, seq 8, epoch 16, message number u32, phrase 1, reserved / reply count 1
	quickchatRequest     = quickchatBase + sideMacSize
	quickchatNameSize    = 48                     // a gamertag in UTF-8, zero-padded
	quickchatRecord      = 12 + quickchatNameSize // owner, phrase, name length, reserved, message number u32, age ms u16, reserved u16, name
	quickchatMaxRecs     = 12
	quickchatPhrases     = 64   // phrase numbers 0-63 are accepted: QuickChat has 16, and new ones only ever go at the end
	quickchatLease       = 3.0  // s: a player whose client stopped asking for this long has no live message
	quickchatMessage     = 10.0 // s a message is passed on after it arrived
	quickchatMinGap      = 1.0  // s: a new message sooner than this after the last one waits until it may be taken
	quickchatMinInterval = 0.03
	quickchatLogEvery    = 10.0
	quickchatMaxNames    = 50000
	quickchatTagRunes    = 32
)

var quickchatLog = diag.Get("quickchat")

// QuickchatState is a player's quick chat state: the newest message taken from it, which may lag what its client asks for while that is too soon
// after the last one.
type QuickchatState struct {
	Epoch   []byte
	Seq     uint64
	At      float64
	Number  uint32 // 0: none in this ticket epoch
	Phrase  byte
	TakenAt float64
}

func (h *Hub) quickchatRoutes(only *World) map[uint64]Addr {
	if only == nil {
		return h.ChatRoutes
	}
	if only.quickchatRoutes == nil {
		only.quickchatRoutes = map[uint64]Addr{}
	}
	return only.quickchatRoutes
}

// quickchatLive says whether other's newest message may be passed on: fresh, inside its 10 s, from its live ticket.
func (h *Hub) quickchatLive(other *Peer, now float64) bool {
	st := other.Quickchat
	binding, ok := h.Bound[other.Addr]
	if st == nil || st.Number == 0 || now-st.At > quickchatLease || now-st.TakenAt > quickchatMessage || !other.Connected || !other.Entered ||
		!(1 <= other.Owner && other.Owner <= 254) || !ok || binding.xuid != other.Xuid || h.isRevoked(other.Xuid) || h.Keys[other.Xuid][binding.key] <= now {
		return false
	}
	return bytes.Equal(h.TicketEpochs[other.Addr], st.Epoch)
}

// quickchatStep is the player's state after one request (old: the previous state or nil); -> the state, and whether a new message was taken.
func quickchatStep(old *QuickchatState, epoch []byte, seq uint64, now float64, number uint32, phrase byte) (*QuickchatState, bool) {
	st := &QuickchatState{Epoch: epoch, Seq: seq, At: now, TakenAt: -1e18}
	if old != nil && bytes.Equal(old.Epoch, epoch) {
		st.Number, st.Phrase, st.TakenAt = old.Number, old.Phrase, old.TakenAt
	}
	if number != 0 && number != st.Number && now-st.TakenAt >= quickchatMinGap {
		st.Number, st.Phrase, st.TakenAt = number, phrase, now
		return st, true
	}
	return st, false
}

// quickchatName is the gamertag the account service announced for xuid ("name" on the control port), as at most quickchatNameSize bytes of UTF-8.
func (h *Hub) quickchatName(xuid uint64) []byte {
	raw := []byte(h.Gamertags[xuid])
	if len(raw) > quickchatNameSize {
		raw = raw[:quickchatNameSize]
	}
	for len(raw) > 0 && !utf8.Valid(raw) { // never half a character
		raw = raw[:len(raw)-1]
	}
	return raw
}

// setGamertag keeps a gamertag for quick chat's senders ("name <xuid hex> <gamertag as UTF-8 hex>" on the control port).
func (h *Hub) setGamertag(xuid uint64, tag string) {
	if _, has := h.Gamertags[xuid]; len(h.Gamertags) >= quickchatMaxNames && !has {
		h.Gamertags = map[uint64]string{}
	}
	if r := []rune(tag); len(r) > quickchatTagRunes {
		tag = string(r[:quickchatTagRunes])
	}
	h.Gamertags[xuid] = tag
}

// quickchatHandle consumes a quick chat request: takes the player's newest message and answers with the others' live ones.
func (h *Hub) quickchatHandle(data []byte, addr Addr, only *World) {
	if len(data) != quickchatRequest || !bytes.Equal(data[:9], sideHeader[:9]) || string(data[11:19]) != quickchatMagic || envStr("FH2MP_NO_QUICKCHAT", "") != "" {
		return
	}
	if int(binary.LittleEndian.Uint16(data[9:11])) != len(data) || data[19] != 1 || data[57] != 0 || data[56] >= quickchatPhrases {
		if ok, skipped := sideRate.Allow("qc-malformed", rejectLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			quickchatLog.Warn(fmt.Sprintf("request with the quick chat magic is malformed and ignored: length field %d of %d bytes, type %d (expected 1), reserved byte %d (expected 0), phrase %d (max %d)%s",
				binary.LittleEndian.Uint16(data[9:11]), len(data), data[19], data[57], data[56], quickchatPhrases-1, extra))
		}
		return
	}
	xuid := binary.LittleEndian.Uint64(data[20:])
	seq := binary.LittleEndian.Uint64(data[28:])
	now := h.Clk.Mono()
	if seq == 0 || h.isRevoked(xuid) {
		return
	}
	// a quick chat request names no car: byte 52 is the message number's
	world, peer, key := h.findAuthedCar(only, h.quickchatRoutes(only), xuid, data, now, false)
	if world == nil {
		if ok, skipped := sideRate.Allow([2]any{"qc-unmatched", xuid}, rejectLogSeconds); ok {
			why, expected := h.whyUnmatchedCar(xuid, data, now, false)
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			msg := fmt.Sprintf("request from xuid %d ignored: %s%s", xuid, why, extra)
			if expected {
				quickchatLog.Debug(msg)
			} else {
				quickchatLog.Warn(msg)
			}
		}
		return
	}
	epoch := append([]byte(nil), data[36:52]...)
	old := peer.Quickchat
	if old != nil && bytes.Equal(old.Epoch, epoch) && (seq <= old.Seq || now-old.At < quickchatMinInterval) {
		return
	}
	st, taken := quickchatStep(old, epoch, seq, now, binary.LittleEndian.Uint32(data[52:]), data[56])
	peer.Quickchat = st
	if taken && (now-peer.QuickchatLogAt >= quickchatLogEvery || peer.QuickchatLogAt == 0) {
		peer.QuickchatLogAt = now
		world.Logf("quickchat: owner %d message %d phrase %d", peer.Owner, st.Number, st.Phrase)
	}
	var records []byte
	n := 0
	for _, other := range world.Others(peer) {
		if !h.quickchatLive(other, now) || !world.SameEvent(peer, other) {
			continue
		}
		ost := other.Quickchat
		name := h.quickchatName(other.Xuid)
		age := int((now - ost.TakenAt) * 1000)
		if age > 0xFFFF {
			age = 0xFFFF
		}
		rec := make([]byte, quickchatRecord)
		rec[0], rec[1], rec[2] = byte(other.Owner), ost.Phrase, byte(len(name))
		binary.LittleEndian.PutUint32(rec[4:], ost.Number)
		binary.LittleEndian.PutUint16(rec[8:], uint16(age))
		copy(rec[12:], name)
		records = append(records, rec...)
		n++
		if n == quickchatMaxRecs {
			break
		}
	}
	reply := append([]byte(nil), data[:quickchatBase]...)
	reply[19] = 2
	binary.LittleEndian.PutUint32(reply[52:], st.Number) // what was taken: the client stops waiting for its message once it's here
	reply[56], reply[57] = st.Phrase, byte(n)
	reply = append(reply, records...)
	binary.LittleEndian.PutUint16(reply[9:], uint16(len(reply)+sideMacSize))
	h.Sock.SendTo(signed(reply, key), addr)
}
