package game

import (
	"fmt"
	"os"
	"strings"

	"fh2mp/internal/bits"
	"fh2mp/internal/bucketlist"
	"fh2mp/internal/diag"
	"fh2mp/internal/gameopts"
	"fh2mp/internal/netv2"
	"fh2mp/internal/playersetup"
)

func kindName(mtype int) string {
	if mtype == netv2.MsgObjectCreate {
		return "create"
	}
	return "update"
}

// handleObject is the object create / update branch of Server.handle. handleBreak stops parsing the rest of the datagram.
func (w *World) handleObject(peer *Peer, r rd, br *bits.Reader, mtype int, data []byte, addr Addr, types []int, replies *[]netv2.Msg, early *string) handleResult {
	router := int(r.bits(4))
	rb, ok := netv2.RouterBits[router]
	if !ok {
		k := [2]int{mtype, router}
		if _, seen := peer.UnmappedLogged[k]; !seen {
			peer.UnmappedLogged[k] = struct{}{}
			w.Warnf("%d object msg %d on unmapped router %d; stop parsing bits-left=%d owner %d xuid %#x types=%s raw=%x; not logged again for this client",
				addr.Port, mtype, router, br.Size-br.Pos, peer.Owner, peer.Xuid, intsRepr(tail6(types)), data)
		}
		return handleBreak
	}
	idBits, typeBits := rb[0], rb[1]
	a, b := r.bits(1), r.bits(1)
	oid := int(r.bits(idBits))
	if a == 0 && b == 1 {
		r.bits(8)
	}
	var tid *int
	if mtype == netv2.MsgObjectCreate && typeBits != 0 {
		t := int(r.bits(typeBits))
		tid = &t
	}
	seq := int(r.bits(8))
	size := int(r.bits(14))
	payload := bits.Int(nil)
	if size != 0 {
		payload = r.intv(size)
	}
	key := ObjKey{router, oid}
	if a == 0 && b == 0 {
		last, seen := peer.RxSeq[key]
		d := pmod(seq-last, 0xFF)
		if seen && !(0 < d && d < 0x80) {
			peer.DupObjects++
		} else {
			peer.RxSeq[key] = seq
		}
	}
	if lim, ok := netv2.RouterLimits[router]; ok && a == 0 && b == 0 {
		if oid >= lim[0] || (tid != nil && *tid >= lim[1]) {
			t := "None"
			if tid != nil {
				t = fmt.Sprint(*tid)
			}
			w.Malformed(peer, fmt.Sprintf("object router=%d id=%d type=%s", router, oid, t))
			return handleBreak
		}
	}
	tidS := "None"
	if tid != nil {
		tidS = fmt.Sprint(*tid)
	}
	if mtype == netv2.MsgObjectCreate || CarCapture {
		w.Logf("%d obj %s router=%d flags=%d%d id=%d type=%s seq=%d varbits=%d", addr.Port, kindName(mtype), router, a, b, oid, tidS, seq, size)
	}
	if mtype == netv2.MsgObjectCreate || size >= 500 {
		w.note("obj", peer.Owner, fmt.Sprintf("%s router=%d id=%d bits=%d", kindName(mtype), router, oid, size))
	}
	suppress := false
	if CarCapture && router == 1 && size != 0 {
		w.Logf("%d CAPTURE player %s id=%d seq=%d bits=%d xuid=%#x hex=%s", addr.Port, kindName(mtype), oid, seq, size, peer.Xuid, hexOf(payload, size))
	}
	if router == 1 && a == 0 && b == 0 && size != 0 && os.Getenv("FH2MP_KEEP_VIP") == "" {
		size, payload = w.HideVIP(peer, oid, size, payload)
	}
	var real *relayVariant // the object as the player sent it, for those whose games have the same custom cars (RealCarsFor)
	if router == 1 && a == 0 && b == 0 {
		var sup bool
		rs, rp := size, payload
		_, sup, size, payload = w.CheckParts(peer, size, payload, oid)
		if sup {
			return handleContinue
		}
		if w.standinApplied && w.HasCustomCars(peer) {
			real = &relayVariant{rs, rp}
		}
		if size >= 240 {
			payload = w.NoteChoices(peer, size, payload)
		} else if size == 40 || size == 80 {
			payload = w.ShortRentalChoices(peer, size, payload)
		}
		payload = w.NoteTypedChoices(peer, size, payload)
		w.RetryRefusedReady(peer)
		w.NotePlayerStatus(peer, size, payload)
		w.NoteCarLook(peer, rs, rp)
		w.NoteChatStates(peer, size, payload)
	}
	if CarCapture && router == netv2.RouterCar && size != 0 {
		w.Logf("%d CAPTURE car %s id=%d seq=%d bits=%d hex=%s", addr.Port, kindName(mtype), oid, seq, size, hexOf(payload, size))
	}
	if router == netv2.RouterCar && 0 < size && size <= 80 {
		if peer.CarPayloadLogs < 8 {
			peer.CarPayloadLogs++
			w.Debugf("car", "%d car object %s payload %d bits = %#x", addr.Port, kindName(mtype), size, payload.Uint64())
		}
		if mid, ok := carModelID(size, payload); ok {
			suppress = w.CarModelSeen(peer, mid)
			c := w.cat()
			_, known := c.KnownCars[int(mid)]
			_, dlc := DlcCars[mid]
			_, blocked := c.Blocked[int(mid)]
			if !suppress && UnknownCarPolicy == "standin" && len(c.KnownCars) > 0 && !known && !dlc && !blocked {
				if w.HasCustomCars(peer) {
					real = &relayVariant{size, payload}
				}
				payload = replaceBits(payload, 5, 32, bits.FromUint64(uint64(w.standinFor(peer, mid))))
			}
		}
	}
	if router == 1 && mtype == netv2.MsgObjectUpdate && size >= 1000 && w.Roadtrip {
		w.RtCarPicked(peer)
	}
	if router == bucketlist.Router && a == 0 && b == 0 {
		w.BlObject(peer, mtype, oid, tid, seq, size, payload)
	}
	if router == netv2.RouterRoadTrip && mtype == netv2.MsgObjectUpdate && a == 0 && b == 0 && w.Roadtrip {
		w.roadTripPlayerUpdate(peer, addr, seq, size, payload)
	}
	if router == netv2.RouterLobby && a == 1 && b == 0 {
		if w.handleGameOptions(peer, addr, mtype, router, oid, seq, size, payload) {
			return handleContinue
		}
	} else if isRelayRouter(router) && a == 0 && b == 0 && !suppress && peer.KickReason == nil {
		w.rememberAndRelay(peer, mtype, router, oid, tid, seq, size, payload, real)
	}
	if router == 3 && mtype == netv2.MsgObjectUpdate && size != 0 {
		w.consoleUpdate(peer, addr, size, payload)
	}
	if mtype == netv2.MsgObjectCreate && router == 3 {
		peer.ConsoleOid = &oid
		if e := w.EarlyIntroduction(peer, "console"); e != "" {
			*early = e
		}
	}
	if mtype == netv2.MsgObjectCreate && router == 3 && !peer.ConsoleAcked {
		peer.ConsoleAcked = true
		*replies = append(*replies, netv2.ServerUpdate(uint64(router), uint64(oid), idBits, 0, nil))
		w.Debugf("handshake", "%d -> ServerUpdate for client Console object id=%d", addr.Port, oid)
	}
	return handleContinue
}

func isRelayRouter(router int) bool {
	for _, r := range netv2.RelayRouters {
		if r == router {
			return true
		}
	}
	return false
}

func (w *World) roadTripPlayerUpdate(peer *Peer, addr Addr, seq, size int, payload bits.Int) {
	raw, _ := payload.Bytes((size + 7) / 8)
	pr := bits.NewReader(raw)
	type sv struct {
		name string
		val  uint64
	}
	var seen []sv
	func() {
		defer func() { _ = recover() }()
		for pr.Pos+3 <= size {
			vid, err := pr.Bits(3)
			if err != nil {
				return
			}
			if vid == 0 {
				v, err := pr.Bits(4)
				if err != nil {
					return
				}
				seen = append(seen, sv{"State", v})
			} else {
				name := "XPOnDestinationJoin"
				if vid == 1 {
					name = "CompletedEventCount"
				}
				v, err := pr.Bits(32)
				if err != nil {
					return
				}
				seen = append(seen, sv{name, v})
			}
		}
	}()
	rows := make([]string, len(seen))
	for i, s := range seen {
		rows[i] = fmt.Sprintf("('%s', %d)", s.name, s.val)
	}
	w.Logf("%d RoadTripPlayerSynced update seq=%d: [%s] (%d bits)", addr.Port, seq, strings.Join(rows, ", "), size)
	for _, s := range seen {
		if s.name == "State" {
			peer.RtPstate = int(s.val)
		}
	}
	if w.RtState == RTDriveToEvent {
		voters := w.rtVoters()
		arrived := 0
		for _, q := range voters {
			if q.RtPstate >= RTArrivedAtDestination {
				arrived++
			}
		}
		if len(voters) > 0 && arrived == len(voters) {
			w.RtEndMs = w.NowMs()
			w.Log("everybody arrived: ending the drive")
		} else if arrived > 0 && w.RtEndMs > w.NowMs()+RTFirstArrivedMs {
			w.RtEndMs = w.NowMs() + RTFirstArrivedMs
			w.RtBroadcast(rtv("StateEndTime", w.RtEndMs))
			w.Logf("first arrival: the others get %d s", RTFirstArrivedMs/1000)
		}
	}
}

// handleGameOptions handles the leader's GameOptionsSynced after ownership transfer; true = the rest of the message is done (Python `continue`).
func (w *World) handleGameOptions(peer *Peer, addr Addr, mtype, router, oid, seq, size int, payload bits.Int) bool {
	_, existed := peer.Objs.Get(ObjKey{router, oid})
	first := !existed
	var writer *Peer
	if w.PendingRace != nil && !w.Roadtrip {
		writer = w.PendingRace.Leader
	} else {
		writer = w.GameOptionsOwner
	}
	if writer != nil && writer != peer {
		if !w.Roadtrip {
			base := peer.GoptsView
			if base == nil {
				base = w.GameOptions
			}
			ps, pp, err := gameopts.Merge(base, size, payload)
			if err == nil {
				_, _, _, err = gameopts.BoundedLaps(ps, pp, w.FreeRoamLaps, nil, w.FreeRoamEndCondition)
			}
			if err != nil {
				w.LogEvery([2]any{"gopts-invalid", addr}, 10.0, fmt.Sprintf("%d invalid pending GameOptions: %v; ignored (%d bits, owner %d xuid %#x, not the options owner; owner %d holds them)",
					addr.Port, err, size, peer.Owner, peer.Xuid, writer.Owner), diag.Warn)
				return true
			}
			pend := &gameopts.Options{Size: ps, Payload: pp}
			peer.GoptsView, peer.GoptsPending = pend, pend
			peer.GoptsWrittenAt = w.Mono()
			s := seq
			peer.GoptsOwnSeq = &s
			w.Logf("%d GameOptions staged pending race request (%d bits, own seq %d)", addr.Port, size, seq)
		}
		return true
	}
	s := seq
	peer.GoptsOwnSeq = &s
	peer.GoptsWrittenAt = w.Mono()
	rawSize, rawPayload := size, payload
	var correction *gameopts.Options
	if !w.Roadtrip {
		var fixedLaps *int64
		if w.Event != nil && !w.Event.Over {
			fixedLaps = w.Event.NumLaps
		}
		err := func() error {
			var previous map[int]gameopts.Field
			if w.GameOptions != nil {
				var e error
				if previous, e = gameopts.Fields(w.GameOptions.Size, w.GameOptions.Payload); e != nil {
					return e
				}
			}
			var e error
			size, payload, e = gameopts.Merge(w.GameOptions, size, payload)
			if e != nil {
				return e
			}
			if w.Event != nil && !w.Event.Over && w.Event.CarRule != nil {
				values, e := gameopts.Fields(size, payload)
				if e != nil {
					return e
				}
				rule := w.Event.CarRule
				for i, vid := range []int{30, 31, 32} {
					width := []int{8, 16, 16}[i]
					val := []uint64{rule.Class, rule.Min, rule.Max}[i]
					values[vid] = gameopts.Field{Width: width, Value: bits.FromUint64(val)}
				}
				if size, payload, e = gameopts.EncodeFields(values); e != nil {
					return e
				}
			}
			var laps int64
			size, payload, laps, e = gameopts.BoundedLaps(size, payload, w.FreeRoamLaps, fixedLaps, w.FreeRoamEndCondition)
			if e != nil {
				return e
			}
			incoming, e := gameopts.Fields(rawSize, rawPayload)
			if e != nil {
				return e
			}
			corrected, e := gameopts.Fields(size, payload)
			if e != nil {
				return e
			}
			changed := map[int]gameopts.Field{}
			for _, vid := range []int{30, 31, 32, 418, 419} {
				c, ok := corrected[vid]
				if !ok {
					continue
				}
				in, inOk := incoming[vid]
				_, prevOk := previous[vid]
				if (inOk && !(in.Width == c.Width && in.Value.Equal(c.Value))) || ((vid == 418 || vid == 419) && !inOk && !prevOk) {
					changed[vid] = c
				}
			}
			if len(changed) > 0 {
				cs, cp, e := gameopts.EncodeFields(changed)
				if e != nil {
					return e
				}
				correction = &gameopts.Options{Size: cs, Payload: cp}
			}
			w.FreeRoamLaps = laps
			return nil
		}()
		if err != nil {
			stored := "none"
			if w.GameOptions != nil {
				stored = fmt.Sprint(w.GameOptions.Size)
			}
			w.LogEvery([2]any{"gopts-invalid", addr}, 10.0, fmt.Sprintf("%d invalid GameOptions: %v; ignored (%d bits, owner %d xuid %#x); the stored options (%s bits) stay",
				addr.Port, err, rawSize, peer.Owner, peer.Xuid, stored), diag.Warn)
			return true
		}
		if et, e := gameopts.EndConditionType(size, payload); e == nil && et != nil {
			w.FreeRoamEndCondition = int64(*et)
		}
		if rawSize != size || !rawPayload.Equal(payload) {
			w.Logf("%d GameOptions NumLaps corrected to %d (maximum %d)", addr.Port, w.FreeRoamLaps, gameopts.MaxLaps)
		}
	} else {
		var fixed *gameopts.Options
		if s, p, c := w.rtFixLaps(addr.Port, size, payload); c != nil {
			size, payload, fixed = s, p, c
		}
		correction = fixed
	}
	kind := "update"
	if first {
		kind = "create"
	}
	w.Logf("%d GameOptions %s relayed (%d bits, own seq %d, mtype %d)", addr.Port, kind, size, seq, mtype)
	if CarCapture {
		w.Logf("  GameOptions payload hex=%s", hexOf(payload, size))
	}
	if w.Roadtrip {
		w.logRoadTripLaps(addr.Port, rawSize, rawPayload)
	}
	unchanged := w.GameOptions != nil && w.GameOptions.Size == size && w.GameOptions.Payload.Equal(payload)
	w.GameOptions = &gameopts.Options{Size: size, Payload: payload}
	if !w.Roadtrip {
		peer.GoptsView = w.GameOptions
		peer.GoptsPending = nil
	}
	if correction != nil {
		w.SendGameOptions(peer, correction)
	}
	if unchanged {
		w.GoptsEchoDropped++
	} else {
		for _, q := range w.Others(peer) {
			w.SendGameOptions(q, nil)
		}
	}
	return false
}

// consoleUpdate parses the bool variables of a Console update.
func (w *World) consoleUpdate(peer *Peer, addr Addr, size int, payload bits.Int) {
	raw, _ := payload.Bytes((size + 7) / 8)
	pr := bits.NewReader(raw)
	var seen []string
	func() {
		for pr.Pos+5 <= size {
			vid, err := pr.Bits(netv2.ConsoleVarIDBits)
			if err != nil {
				return
			}
			if _, ok := ConsoleBoolVarNames[int(vid)]; !ok {
				return
			}
			val, err := pr.Bits(1)
			if err != nil {
				return
			}
			peer.ConsoleVars[int(vid)] = val != 0
			seen = append(seen, fmt.Sprintf("%s=%d", ConsoleBoolVarNames[int(vid)], val))
		}
	}()
	if len(seen) == 0 {
		return
	}
	w.Logf("%d Console vars: %s", addr.Port, strings.Join(seen, " "))
	if containsStr(seen, "Ready=1") && w.PendingRace != nil && peer != w.PendingRace.Leader && w.Event == nil && w.EventSeatsFull(peer) {
		w.RefuseEventReady(peer, "Console.Ready") // a client that set it without asking (or after a refused ask)
	}
	for _, item := range seen {
		if strings.HasPrefix(item, "RenderReady=") {
			w.NoteRenderReady(peer, int(item[len(item)-1]-'0'))
		} else if item == "HasLoadedCarsForAllParticipants=1" {
			w.NoteLoaded(peer)
		}
	}
	v3, has3 := peer.ConsoleVars[3]
	if w.Event != nil && w.Event.has(peer) && has3 && !v3 && containsStr(seen, "RaceInProgress=0") {
		w.ParticipantLeft(peer)
	}
	w.MaybeBeginRace()
	w.MaybeGoRace()
	if w.PendingRace != nil && peer != w.PendingRace.Leader && peer.ConsoleVars[4] {
		w.CheckInviteesReady()
	}
}

func containsStr(a []string, s string) bool {
	for _, x := range a {
		if x == s {
			return true
		}
	}
	return false
}

var _ = playersetup.IDBits
