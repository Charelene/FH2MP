package game

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"math"

	"fh2mp/internal/diag"
)

// Side channels that ride the game port next to the NetV2 stream: lighting, horn, and the UDP reachability probe.
const (
	lightingMagic = "FH2MPLT1"
	honkMagic     = "FH2MPHN1"
	probeMagic    = "FH2MPPN1"

	sideMacSize      = 32
	lightingBase     = 55
	lightingRequest  = lightingBase + sideMacSize
	lightingMaxRecs  = 12
	lightingLease    = 2.0
	lightingMinInt   = 0.08
	lightingLogEvery = 60.0
	rejectLogSeconds = 60.0

	honkBase        = 57
	honkRequest     = honkBase + sideMacSize
	honkRecordSize  = 20
	honkMaxRecs     = 12
	honkOff         = 0xFF
	honkHorns       = 31
	honkLease       = 0.5
	honkCap         = 10.0
	honkCooldown    = 2.0
	honkMinInterval = 0.03
	honkMaxDistance = 400.0
	honkLogEvery    = 10.0

	probeLength    = 11 + len(probeMagic) + 8
	probePerWindow = 6
	probeWindow    = 60.0
	probeMaxAddrs  = 4096
)

var sideHeader = []byte{0xff, 0xff, 0xfe, 0xff, 0, 0, 0, 0, 0, 0, 0}

var (
	sideRate  = diag.NewRateLimit(nil, 0)
	lightLog  = diag.Get("lighting")
	honkLog   = diag.Get("honk")
	probeLog  = diag.Get("probe")
	probeRate = diag.NewRateLimit(nil, 0)
)

// LightState is a player's last lighting request.
type LightState struct {
	Epoch []byte
	Seq   uint64
	At    float64
	Flags byte
	Car   int
}

// HonkState is a player's horn state.
type HonkState struct {
	Epoch        []byte
	Seq          uint64
	At           float64
	Car          int
	Horn         *int
	HornAsked    int
	Press        int
	Started      *float64
	BlockedUntil float64
	Capped       *int
	CapHit       bool
}

func signed(payload, key []byte) []byte {
	m := hmac.New(sha256.New, key)
	m.Write(payload)
	return append(append([]byte(nil), payload...), m.Sum(nil)...)
}

func (h *Hub) whyUnmatched(xuid uint64, data []byte, now float64) (string, bool) {
	for mainAddr, binding := range h.Bound {
		if binding.xuid != xuid {
			continue
		}
		world, _ := h.Route.Get(mainAddr)
		var peer *Peer
		if world != nil {
			peer, _ = world.Peers.Get(mainAddr)
		}
		if peer == nil {
			return "its game connection is in no world", true
		}
		if !peer.Connected || !peer.Entered {
			if peer.Connected {
				return "its game connection has not entered the world yet", true
			}
			return "its game connection is gone", true
		}
		if peer.Xuid != xuid || !(1 <= peer.Owner && peer.Owner <= 254) {
			return fmt.Sprintf("the connection's xuid is %d and its owner id %d", peer.Xuid, peer.Owner), false
		}
		if peer.CarID == nil || *peer.CarID != int(data[52]) {
			cs := "None"
			if peer.CarID != nil {
				cs = fmt.Sprint(*peer.CarID)
			}
			return fmt.Sprintf("the request names car %d, the game server has car %s", data[52], cs), true
		}
		if h.Keys[xuid][binding.key] <= now {
			return "the ticket key has expired", false
		}
		if !bytes.Equal(h.TicketEpochs[mainAddr], data[36:52]) {
			return "the ticket epoch differs from the one of the game connection (the client holds an older or newer ticket)", false
		}
		return "the signature does not match the ticket key", false
	}
	return "no game connection of this player is bound to a ticket", false
}

// findAuthed matches a side-channel request to the live, ticket-bound game connection of its xuid.
func (h *Hub) findAuthed(routes map[uint64]Addr, xuid uint64, data []byte, now float64) (*World, *Peer, []byte) {
	last, hasLast := routes[xuid]
	type cand struct {
		addr Addr
		w    *World
	}
	var cands []cand
	if hasLast {
		if w, ok := h.Route.Get(last); ok {
			cands = append(cands, cand{last, w})
		}
	}
	for _, a := range h.Route.Keys() {
		w, _ := h.Route.Get(a)
		cands = append(cands, cand{a, w})
	}
	for _, c := range cands {
		binding, ok := h.Bound[c.addr]
		peer, _ := c.w.Peers.Get(c.addr)
		if !ok || binding.xuid != xuid || peer == nil || !peer.Connected || !peer.Entered || peer.Xuid != xuid || peer.CarID == nil || *peer.CarID != int(data[52]) ||
			!(1 <= peer.Owner && peer.Owner <= 254) {
			continue
		}
		key := []byte(binding.key)
		if h.Keys[xuid][binding.key] <= now || !bytes.Equal(h.TicketEpochs[c.addr], data[36:52]) {
			continue
		}
		m := hmac.New(sha256.New, key)
		m.Write(data[:len(data)-sideMacSize])
		if !hmac.Equal(m.Sum(nil), data[len(data)-sideMacSize:]) {
			continue
		}
		routes[xuid] = c.addr
		return c.w, peer, key
	}
	delete(routes, xuid)
	return nil, nil, nil
}

func (h *Hub) lightingHandle(data []byte, addr Addr) {
	if len(data) != lightingRequest || !bytes.Equal(data[:9], sideHeader[:9]) || string(data[11:19]) != lightingMagic {
		return
	}
	lenField := int(binary.LittleEndian.Uint16(data[9:11]))
	if lenField != len(data) || data[19] != 1 || data[54] != 0 || data[52] > 127 || data[53] > 7 {
		if ok, skipped := sideRate.Allow("lt-malformed", rejectLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			lightLog.Warn(fmt.Sprintf("request with the lighting magic is malformed and ignored: length field %d of %d bytes, type %d (expected 1), reserved byte %d (expected 0), car %d (max 127), flags %d (max 7)%s",
				lenField, len(data), data[19], data[54], data[52], data[53], extra))
		}
		return
	}
	xuid := binary.LittleEndian.Uint64(data[20:])
	seq := binary.LittleEndian.Uint64(data[28:])
	now := h.Clk.Mono()
	if seq == 0 || h.isRevoked(xuid) {
		return
	}
	world, peer, key := h.findAuthed(h.LightingRoutes, xuid, data, now)
	if world == nil {
		if ok, skipped := sideRate.Allow([2]any{"lt-unmatched", xuid}, rejectLogSeconds); ok {
			why, expected := h.whyUnmatched(xuid, data, now)
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			msg := fmt.Sprintf("request from xuid %d ignored: %s%s", xuid, why, extra)
			if expected {
				lightLog.Debug(msg)
			} else {
				lightLog.Warn(msg)
			}
		}
		return
	}
	old := peer.Lighting
	epoch := append([]byte(nil), data[36:52]...)
	if old != nil && bytes.Equal(old.Epoch, epoch) && (seq <= old.Seq || now-old.At < lightingMinInt) {
		return
	}
	carID := -1
	if peer.CarID != nil {
		carID = *peer.CarID
	}
	peer.Lighting = &LightState{epoch, seq, now, data[53], carID}
	if old == nil || old.Flags != data[53] || old.Car != carID {
		peer.LightingChanges++
		if now-peer.LightingLoggedAt >= lightingLogEvery || peer.LightingLoggedAt == 0 {
			world.Logf("lighting: owner %d car %d headlights=%d signal=%d (%d change(s) in the last minute)", peer.Owner, carID, data[53]&1, data[53]>>1, peer.LightingChanges)
			peer.LightingLoggedAt, peer.LightingChanges = now, 0
		}
	}
	var records []byte
	n := 0
	for _, other := range world.Others(peer) {
		st := other.Lighting
		binding, bound := h.Bound[other.Addr]
		otherCar := -1
		if other.CarID != nil {
			otherCar = *other.CarID
		}
		if st == nil || now-st.At > lightingLease || st.Car != otherCar || !other.Connected || !other.Entered || !(1 <= other.Owner && other.Owner <= 254) ||
			!bound || binding.xuid != other.Xuid || h.isRevoked(other.Xuid) || h.Keys[other.Xuid][binding.key] <= now ||
			!bytes.Equal(h.TicketEpochs[other.Addr], st.Epoch) {
			continue
		}
		records = append(records, byte(other.Owner), byte(st.Car), st.Flags, 0)
		n++
		if n == lightingMaxRecs {
			break
		}
	}
	reply := append([]byte(nil), data[:lightingBase]...)
	reply[19], reply[54] = 2, byte(n)
	reply = append(reply, records...)
	binary.LittleEndian.PutUint16(reply[9:], uint16(len(reply)+sideMacSize))
	h.Sock.SendTo(signed(reply, key), addr)
}

func (h *Hub) honkStateLive(other *Peer, now float64) bool {
	st := other.Honk
	binding, ok := h.Bound[other.Addr]
	if st == nil || st.Horn == nil || now-st.At > honkLease || other.CarID == nil || st.Car != *other.CarID || !other.Connected || !other.Entered ||
		!(1 <= other.Owner && other.Owner <= 254) || !ok || binding.xuid != other.Xuid || h.isRevoked(other.Xuid) || h.Keys[other.Xuid][binding.key] <= now {
		return false
	}
	return bytes.Equal(h.TicketEpochs[other.Addr], st.Epoch)
}

func honkStep(old *HonkState, epoch []byte, seq uint64, now float64, horn, press, car int) *HonkState {
	fresh := old != nil && bytes.Equal(old.Epoch, epoch)
	blockedUntil := 0.0
	if fresh {
		blockedUntil = old.BlockedUntil
	}
	var started *float64
	var capped *int
	capHit := false
	var allowed *int
	if horn != honkOff {
		if fresh && old.HornAsked != honkOff && old.Press == press {
			started, capped = old.Started, old.Capped
		} else {
			n := now
			started = &n
			capped = nil
			if now < blockedUntil {
				p := press
				capped = &p
			}
		}
		if (capped == nil || *capped != press) && started != nil && now-*started > honkCap {
			p := press
			capped, blockedUntil, capHit = &p, now+honkCooldown, true
		}
		if capped == nil || *capped != press {
			hv := horn
			allowed = &hv
		}
	}
	return &HonkState{Epoch: epoch, Seq: seq, At: now, Car: car, Horn: allowed, HornAsked: horn, Press: press, Started: started, BlockedUntil: blockedUntil, Capped: capped, CapHit: capHit}
}

func (h *Hub) honkHandle(data []byte, addr Addr) {
	if len(data) != honkRequest || !bytes.Equal(data[:9], sideHeader[:9]) || string(data[11:19]) != honkMagic || envStr("FH2MP_NO_HONK", "") != "" {
		return
	}
	lenField := int(binary.LittleEndian.Uint16(data[9:11]))
	if lenField != len(data) || data[19] != 1 || data[54] != 0 || data[52] > 127 || (data[53] != honkOff && data[53] >= honkHorns) {
		if ok, skipped := sideRate.Allow("hn-malformed", rejectLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			honkLog.Warn(fmt.Sprintf("request with the horn magic is malformed and ignored: length field %d of %d bytes, type %d (expected 1), reserved byte %d (expected 0), car %d (max 127), horn %d (max %d or %d)%s",
				lenField, len(data), data[19], data[54], data[52], data[53], honkHorns-1, honkOff, extra))
		}
		return
	}
	xuid := binary.LittleEndian.Uint64(data[20:])
	seq := binary.LittleEndian.Uint64(data[28:])
	now := h.Clk.Mono()
	if seq == 0 || h.isRevoked(xuid) {
		return
	}
	world, peer, key := h.findAuthed(h.HonkRoutes, xuid, data, now)
	if world == nil {
		if ok, skipped := sideRate.Allow([2]any{"hn-unmatched", xuid}, rejectLogSeconds); ok {
			why, expected := h.whyUnmatched(xuid, data, now)
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			msg := fmt.Sprintf("request from xuid %d ignored: %s%s", xuid, why, extra)
			if expected {
				honkLog.Debug(msg)
			} else {
				honkLog.Warn(msg)
			}
		}
		return
	}
	epoch := append([]byte(nil), data[36:52]...)
	old := peer.Honk
	if old != nil && bytes.Equal(old.Epoch, epoch) && (seq <= old.Seq || now-old.At < honkMinInterval) {
		return
	}
	press := int(binary.LittleEndian.Uint16(data[55:]))
	carID := -1
	if peer.CarID != nil {
		carID = *peer.CarID
	}
	nw := honkStep(old, epoch, seq, now, int(data[53]), press, carID)
	peer.Honk = nw
	startedNow := nw.Horn != nil && !(old != nil && bytes.Equal(old.Epoch, epoch) && old.Horn != nil && old.Press == press)
	if startedNow && (now-peer.HonkLoggedAt >= honkLogEvery || peer.HonkLoggedAt == 0) {
		peer.HonkLoggedAt = now
		world.Logf("honk: owner %d car %d horn %d press %d", peer.Owner, carID, *nw.Horn, press)
	}
	if nw.CapHit {
		world.Logf("honk: owner %d held press %d for %s s, silenced; next one allowed %s s after", peer.Owner, press, fmtG(honkCap), fmtG(honkCooldown))
	}
	var records []byte
	n := 0
	for _, other := range world.Others(peer) {
		if !h.honkStateLive(other, now) || !world.SameEvent(peer, other) {
			continue
		}
		st := other.Honk
		var pos [3]float64
		known := false
		if other.Pos != nil && allFinite(*other.Pos) {
			pos, known = *other.Pos, true
		}
		if known && peer.Pos != nil && allFinite(*peer.Pos) && dist3(pos, *peer.Pos) > honkMaxDistance {
			continue
		}
		age := 0
		if st.Started != nil {
			age = int(math.Min(0xFFFF, float64(int((now-*st.Started)*1000))))
		}
		rec := make([]byte, honkRecordSize)
		rec[0], rec[1], rec[2] = byte(other.Owner), byte(st.Car), byte(*st.Horn)
		if known {
			rec[3] = 1
		}
		binary.LittleEndian.PutUint16(rec[4:], uint16(st.Press))
		binary.LittleEndian.PutUint16(rec[6:], uint16(age))
		for i := 0; i < 3; i++ {
			v := float32(0)
			if known {
				v = float32(pos[i])
			}
			binary.LittleEndian.PutUint32(rec[8+4*i:], math.Float32bits(v))
		}
		records = append(records, rec...)
		n++
		if n == honkMaxRecs {
			break
		}
	}
	reply := append([]byte(nil), data[:honkBase]...)
	reply[19], reply[54] = 2, byte(n)
	reply = append(reply, records...)
	binary.LittleEndian.PutUint16(reply[9:], uint16(len(reply)+sideMacSize))
	h.Sock.SendTo(signed(reply, key), addr)
}

func fmtG(f float64) string { return fmt.Sprint(f) }

type probeEnt struct {
	start float64
	count int
}

func (h *Hub) probeHandle(data []byte, addr Addr) bool {
	if len(data) != probeLength || string(data[11:19]) != probeMagic {
		return false
	}
	if h.Tunnel != nil && h.Tunnel.HasConn(addr) {
		return true
	}
	now := h.Clk.Mono()
	e, ok := h.probeSeen[addr]
	if !ok {
		e = probeEnt{start: now}
	}
	if now-e.start >= probeWindow {
		e.start, e.count = now, 0
	}
	if e.count >= probePerWindow {
		if ok, skipped := probeRate.Allow("limit", probeWindow); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf("; %d more since the last line", skipped)
			}
			probeLog.Warn(fmt.Sprintf("probe over the limit of %d answers per address per %g s, the excess gets no answer (a launcher then thinks UDP is blocked)%s", probePerWindow, probeWindow, extra))
		}
		return true
	}
	if len(h.probeSeen) >= probeMaxAddrs {
		h.probeSeen = map[Addr]probeEnt{}
	}
	h.probeSeen[addr] = probeEnt{e.start, e.count + 1}
	if err := h.Sock.SendTo(data, addr); err != nil {
		if ok, skipped := probeRate.Allow("send", probeWindow); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (%d more since the last line)", skipped)
			}
			probeLog.Warn(fmt.Sprintf("probe answer not sent: %v%s", err, extra))
		}
	}
	return true
}
