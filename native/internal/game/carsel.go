package game

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"strings"

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

	// Client reports (type 3): the shim says why its game sits where it sits (carsel_wire.h). Signed like a request, never answered.
	carselTypeReport    = 3
	carselReportMaxText = 200
	reportWindow        = 60.0 // seconds
	reportPerWindow     = 8    // reports accepted from one player per window
	reportSeenSeqs      = 8    // sequence numbers remembered for the shim's repeats
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
	if len(data) >= 20 && data[19] == carselTypeReport && bytes.Equal(data[:9], sideHeader[:9]) && string(data[11:19]) == carselMagic && envStr("FH2MP_NO_CARSEL", "") == "" {
		h.carselReport(data, addr, only)
		return
	}
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
			w.Logf("carsel: hold ends %.0f s after the group's deadline, still in the car select: %s", now-deadline, w.carselStates(waiting, now))
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

// ClientReports is a player's reports: the repeats of one report (same sequence number) are dropped, and a player that sends more than
// reportPerWindow in reportWindow seconds is ignored until the window ends.
type ClientReports struct {
	Epoch    []byte
	Seen     [reportSeenSeqs]uint64
	SeenNext int
	WinStart float64
	InWin    int
	Dropped  int
	Last     *ClientReport
}

// ClientReport is one accepted report.
type ClientReport struct {
	At     float64
	Code   uint8
	Detail uint32
	Text   string
}

// reportCodeName names the codes of carsel_wire.h.
func reportCodeName(code uint8) string {
	switch code {
	case 1:
		return "cars-loaded wait"
	case 2:
		return "car cache wait"
	case 3:
		return "stall trace"
	case 4:
		return "stall trace end"
	case 5:
		return "other"
	}
	return "unknown"
}

// Accept says whether a report of this epoch and sequence number is new and within the player's budget; it records it when so. dropped is how many
// reports the budget refused since the last accepted one (0 normally).
func (r *ClientReports) Accept(epoch []byte, seq uint64, now float64) (ok bool, dropped int) {
	if !bytes.Equal(r.Epoch, epoch) {
		*r = ClientReports{Epoch: append([]byte(nil), epoch...)}
	}
	for _, s := range r.Seen {
		if s == seq {
			return false, 0
		}
	}
	if r.WinStart == 0 || now-r.WinStart >= reportWindow {
		r.WinStart, r.InWin = now, 0
	}
	if r.InWin >= reportPerWindow {
		r.Dropped++
		return false, 0
	}
	r.InWin++
	r.Seen[r.SeenNext%reportSeenSeqs] = seq
	r.SeenNext++
	dropped = r.Dropped
	r.Dropped = 0
	return true, dropped
}

// carselReport consumes a client report: signed by the player's ticket key like every side-channel request, matched to its live game connection,
// then logged next to what the server knows. Nothing is sent back.
func (h *Hub) carselReport(data []byte, addr Addr, only *World) {
	n := len(data) - carselBase - sideMacSize
	lenField := int(binary.LittleEndian.Uint16(data[9:11]))
	bad := ""
	switch {
	case n < 0 || n > carselReportMaxText:
		bad = fmt.Sprintf("%d bytes (text %d, max %d)", len(data), n, carselReportMaxText)
	case lenField != len(data):
		bad = fmt.Sprintf("length field %d of %d bytes", lenField, len(data))
	case data[53] != 0:
		bad = fmt.Sprintf("reserved byte %d", data[53])
	default:
		for _, c := range data[carselBase : len(data)-sideMacSize] {
			if c < 0x20 || c >= 0x7f {
				bad = "text with a byte that is not printable ASCII"
				break
			}
		}
	}
	if bad != "" {
		if ok, skipped := sideRate.Allow("csr-malformed", rejectLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			carselLog.Warn(fmt.Sprintf("client report is malformed and ignored: %s%s", bad, extra))
		}
		return
	}
	xuid := binary.LittleEndian.Uint64(data[20:])
	seq := binary.LittleEndian.Uint64(data[28:])
	now := h.Clk.Mono()
	if seq == 0 || h.isRevoked(xuid) {
		return
	}
	world, peer, _ := h.findAuthedCar(only, h.carselRoutes(only), xuid, data, now, false)
	if world == nil {
		if ok, skipped := sideRate.Allow([2]any{"csr-unmatched", xuid}, rejectLogSeconds); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
			}
			carselLog.Debug(fmt.Sprintf("client report from xuid %d ignored: no live game connection of that player matches its address binding, ticket epoch and signature%s", xuid, extra))
		}
		return
	}
	ok, dropped := peer.ClientReports.Accept(data[36:52], seq, now)
	if !ok {
		return
	}
	world.ClientReport(peer, data[52], binary.LittleEndian.Uint32(data[54:]), string(data[carselBase:len(data)-sideMacSize]), dropped)
}

// ClientReport logs what a player's shim said about why its game waits, with the server's view of the same moment, and keeps it for the event's
// load timeout lines.
func (w *World) ClientReport(peer *Peer, code uint8, detail uint32, text string, dropped int) {
	now := w.Mono()
	peer.ClientReports.Last = &ClientReport{At: now, Code: code, Detail: detail, Text: text}
	state := "no event"
	if ev := w.Event; ev != nil {
		in := "not a participant"
		if ev.has(peer) {
			in = "a participant"
		}
		state = fmt.Sprintf("event %.0f s after its start, %s, begun=%v", now-ev.Started, in, ev.Begun)
	}
	extra := ""
	if dropped > 0 {
		extra = fmt.Sprintf(" (%d report(s) over the budget were not logged)", dropped)
	}
	w.Warnf("client report: owner %d xuid %#x, %s [code %d, detail %d]: %s (server: %s, last packet %.1f s ago, car %s)%s",
		peer.Owner, peer.Xuid, reportCodeName(code), code, detail, text, state, now-peer.LastRx, modelStr(peer), extra)
	w.ev("client_report", kv("xuid", fmt.Sprintf("%#x", peer.Xuid)), kv("code", int64(code)), kv("name", reportCodeName(code)), kv("detail", int64(detail)), kv("text", text))
}

// lastReport is the player's latest report when it is not older than window seconds, as a tail for a log line ("" = none).
func (p *Peer) lastReport(now, window float64) string {
	r := p.ClientReports.Last
	if r == nil || now-r.At > window {
		return ""
	}
	return fmt.Sprintf("; its game said %.0f s ago (%s): %s", now-r.At, reportCodeName(r.Code), r.Text)
}

// carselStates is the list of owners the hold waited for, each with what the server knows of it: the PlayerStatus its game last wrote (1 car select,
// 3 pre-race, none = it never wrote one), how long ago it did, and how long ago its last packet came.
func (w *World) carselStates(ps []*Peer, now float64) string {
	parts := make([]string, len(ps))
	for i, q := range ps {
		status := "no status written"
		if q.HasPlayerStatus {
			status = fmt.Sprintf("status %d", q.PlayerStatus)
		}
		parts[i] = fmt.Sprintf("owner%d (%s, last packet %.1f s ago)", q.Owner, status, now-q.LastRx)
	}
	return strings.Join(parts, " ")
}
