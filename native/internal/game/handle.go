package game

import (
	"encoding/hex"
	"fmt"
	"math"
	"os"
	"strings"

	"fh2mp/internal/bits"
	"fh2mp/internal/bucketlist"
	"fh2mp/internal/diag"
	"fh2mp/internal/gameopts"
	"fh2mp/internal/netv2"
	"fh2mp/internal/pyfmt"
	"fh2mp/internal/traffic"
)

// rd wraps a bit reader so a read past the end panics like Python's ValueError("read past end"); the hub recovers per datagram.
type rd struct{ r *bits.Reader }

func (m rd) bits(n int) uint64 {
	v, err := m.r.Bits(n)
	if err != nil {
		panic(err)
	}
	return v
}

func (m rd) intv(n int) bits.Int {
	v, err := m.r.Int(n)
	if err != nil {
		panic(err)
	}
	return v
}

func (m rd) varlen() uint64 {
	v, err := m.r.Varlen()
	if err != nil {
		panic(err)
	}
	return v
}

func (m rd) str() string {
	v, err := m.r.String()
	if err != nil {
		panic(err)
	}
	return v
}

func intsRepr(a []int) string { return intList(a) }

func tail6(t []int) []int {
	if len(t) > 6 {
		return t[len(t)-6:]
	}
	return t
}

func pyReprStr(s string) string { return pyfmt.ReprStr(s) }

func hexOf(p bits.Int, size int) string {
	b, err := p.Bytes((size + 7) / 8)
	if err != nil {
		panic(err)
	}
	return hex.EncodeToString(b)
}

// Handle is Server.handle: one datagram from addr.
func (w *World) Handle(data []byte, addr Addr) {
	w.rx = rxState{nil, len(data), nil}
	if TracePackets {
		w.Trace("packet", "packet.rx "+diag.KV(diag.KVP{K: "port", V: addr.Port}, diag.KVP{K: "bytes", V: len(data)}, diag.KVP{K: "first", V: hex.EncodeToString(data[:minInt(24, len(data))])}))
	}
	peer, _ := w.Peers.Get(addr)
	br := bits.NewReader(data)
	r := rd{br}
	hdr, err := netv2.ParseHeader(br)
	if err != nil {
		w.Noise(fmt.Sprintf("%s short packet %s", addr.pyRepr(), hex.EncodeToString(data[:minInt(16, len(data))])), diag.Info)
		return
	}
	if peer == nil {
		if hdr.Count < 1 {
			return
		}
		first := bits.NewReader(data)
		netv2.ParseHeader(first)
		firstType, err := first.Bits(7)
		if err != nil {
			return
		}
		if firstType != netv2.MsgOpenConnection {
			w.Noise(fmt.Sprintf("%d stale peer (no handshake); sending CloseConnection (first message type %d, %d bytes)", addr.Port, firstType, len(data)), diag.Info)
			w.Send(NewPeer(addr, w.Mono()), false, netv2.CloseConnection(0, 0, 0))
			return
		}
		if w.IsIdle || !w.PendingRoom(addr) {
			pending, mine := 0, 0
			for _, p := range w.Peers.Values() {
				if !p.Connected {
					pending++
					if p.Addr.IP == addr.IP {
						mine++
					}
				}
			}
			why := "this is the catch-all world, which keeps no peer"
			if !w.IsIdle {
				why = fmt.Sprintf("%d/%d here, %d/%d from this address", pending, MaxPendingPeers, mine, MaxPendingPerIP)
			}
			w.Noise(fmt.Sprintf("%s:%d too many half-open connections; ignored (%s)", addr.IP, addr.Port, why), diag.Warn)
			return
		}
		peer = NewPeer(addr, w.Mono())
		w.Peers.Set(addr, peer)
	}
	peer.NotePacketID(int(hdr.AckID))
	peer.Window.Ack(hdr.RemoteAck, hdr.AckBits)
	peer.LastRx = w.Mono()
	var replies []netv2.Msg
	early := ""
	types := []int{}
	w.rx = rxState{peer, len(data), types}
	broke := false
loop:
	for n := 0; n < int(hdr.Count) && peer.State != PeerRemoved; n++ { // a removed peer handles nothing further from this datagram
		mt, err := br.Bits(7)
		if err != nil {
			w.LogEvery([2]any{"short-count", addr}, 60.0, fmt.Sprintf("packet.short %s: the header announces more messages than the datagram holds, the rest is skipped",
				diag.KV(diag.KVP{K: "port", V: addr.Port}, diag.KVP{K: "owner", V: peer.Owner}, diag.KVP{K: "announced", V: hdr.Count}, diag.KVP{K: "read", V: len(types)},
					diag.KVP{K: "bytes", V: len(data)}, diag.KVP{K: "last_types", V: intsRepr(tail6(types))})), diag.Warn)
			broke = true
			break
		}
		mtype := int(mt)
		w.MtSeen[mtype]++
		types = append(types, mtype)
		w.rx.types = types
		switch {
		case mtype == netv2.MsgCarPredictionFull:
			a, b := r.bits(1), r.bits(1)
			cid := int(r.bits(netv2.CarKeyIDBits))
			restBits := br.Size - br.Pos
			if restBits >= netv2.CarPredictionBodyBits {
				restBits = netv2.CarPredictionBodyBits
			}
			rest := r.intv(restBits)
			if !w.admit(peer, mtype) {
				continue
			}
			if restBits != netv2.CarPredictionBodyBits {
				w.LogEvery([2]any{"short-prediction", addr}, 60.0, fmt.Sprintf("%d car prediction body of %d bits (a whole one is %d): not relayed (owner %d xuid %#x, %d bytes)",
					addr.Port, restBits, netv2.CarPredictionBodyBits, peer.Owner, peer.Xuid, len(data)), diag.Warn)
			}
			peer.CarsRelayed++
			var pos *[3]float64
			if restBits >= 96 {
				p := carPosition(rest)
				pos = &p
			}
			far := math.NaN()
			finite := false
			if pos != nil {
				far = math.Max(math.Abs(pos[0]), math.Max(math.Abs(pos[1]), math.Abs(pos[2])))
				finite = allFinite(*pos)
			}
			unusable := pos != nil && (!finite || far > PositionWithholdLim)
			if pos != nil && a == 0 && b == 0 {
				if !unusable {
					p := *pos
					peer.Pos = &p
				}
				if !math.IsNaN(far) && !math.IsInf(far, 0) {
					peer.MaxAbs = math.Max(peer.MaxAbs, far)
					peer.MaxY = math.Max(peer.MaxY, pos[1])
				}
				if !peer.PosFlagged && (!finite || far > PositionLimit) {
					peer.PosFlagged = true
					tail := fmt.Sprintf(" (limit %s), still relayed", pyfmt.FmtG(PositionLimit))
					if unusable {
						tail = fmt.Sprintf("; withheld (limit %s or not finite): its predictions are not relayed while they stay there", pyfmt.FmtG(PositionWithholdLim))
					}
					w.Warnf("%d car position out of range: (%.1f,%.1f,%.1f) owner %d xuid %#x%s", addr.Port, pyfmt.F(pos[0]), pyfmt.F(pos[1]), pyfmt.F(pos[2]), peer.Owner, peer.Xuid, tail)
				}
			}
			if unusable {
				continue
			}
			if peer.CarsRelayed <= 3 || peer.CarsRelayed%500 == 0 {
				ps := ""
				if pos != nil {
					ps = fmt.Sprintf(" pos=(%.0f,%.0f,%.0f)", pyfmt.F(pos[0]), pyfmt.F(pos[1]), pyfmt.F(pos[2]))
				}
				w.Debugf("car", "%d car prediction #%d flags=%d%d id=%d rest=%d bits -> %d peer(s)%s", addr.Port, peer.CarsRelayed, a, b, cid, restBits, len(w.Others(peer)), ps)
			}
			if pos != nil && a == 0 && b == 0 && SpeedPolicy != "off" && !math.IsNaN(far) && !math.IsInf(far, 0) {
				whole := restBits == netv2.CarPredictionBodyBits
				var stamp *uint64
				var scene *int64
				if whole {
					s := rest.Rsh(netv2.CarPredictionBodyBits - 32).Uint64()
					stamp = &s
					sc := int64(rest.Field(netv2.CarTeleportOffset, 2))*32 + int64(rest.Field(netv2.CarUniverseOffset, 5))
					scene = &sc
				}
				w.TrackSpeed(peer, *pos, stamp, scene)
				if whole {
					w.TrackSpin(peer, rest)
					w.TrackPush(peer, rest, *pos, *stamp, *scene)
				}
			}
			if peer.CarsRelayed%20 == 1 && pos != nil {
				w.note("car", peer.Owner, fmt.Sprintf("pos=(%.0f,%.0f,%.0f) id=%d", pyfmt.F(pos[0]), pyfmt.F(pos[1]), pyfmt.F(pos[2]), cid))
			}
			if a == 0 && b == 0 && peer.KickReason == nil {
				c := cid
				peer.CarID = &c
				if restBits == netv2.CarPredictionBodyBits {
					old := peer.CarPrediction
					stampOffset := netv2.CarPredictionBodyBits - 32
					if old == nil || old.ID != cid {
						peer.CarPredictionStamp = nil
						peer.CarPredictionRefreshable = true
					} else {
						diff := (int64(rest.Rsh(stampOffset).Uint64()) - int64(old.Body.Rsh(stampOffset).Uint64())) & 0xFFFFFFFF
						if !(0 < diff && diff < 0x80000000) {
							if !rest.Equal(old.Body) {
								peer.CarPredictionRefreshable = false
							}
							w.RelayPrediction(peer, false, &Prediction{cid, rest, restBits})
							continue
						}
						peer.CarPredictionRefreshable = true
					}
					peer.CarPrediction = &Prediction{cid, rest, restBits}
					w.RelayPrediction(peer, false, nil)
				}
			}
		case mtype == netv2.MsgOpenConnection:
			version, game := r.str(), r.str()
			flag2 := r.bits(2)
			playerID := r.bits(64)
			a, b, counter := r.bits(32), r.bits(32), r.bits(32)
			if !w.OpenAllowed(playerID, addr) {
				if !peer.Connected {
					w.Peers.Delete(addr)
				}
				return
			}
			if peer.Connected {
				w.Logf("%d reconnect: resetting peer state (owner %d xuid %#x, connected %.0f s, last packet %.1f s ago)", addr.Port, peer.Owner, peer.Xuid,
					w.Mono()-peer.ConnectedAt, w.Mono()-peer.LastRx)
				w.DropPeer(peer, "reconnect")
				peer = NewPeer(addr, w.Mono())
				w.Peers.Set(addr, peer)
			}
			result := ResultOK
			if version != netv2.VersionString {
				result = ResultVersionMismatch
			} else if game != netv2.GameString {
				result = ResultGameMismatch
			}
			peer.ClientCounter = counter
			peer.Xuid = playerID
			switch {
			case a == 1 || a == 2 || a == 3:
				peer.LobbyType = int(a)
			case w.Carmeet:
				peer.LobbyType = LobbyTypeCarMeet
			case w.Roadtrip:
				peer.LobbyType = LobbyTypeRoadTrip
			default:
				peer.LobbyType = 2
			}
			peer.World = &WorldKey{Type: 0, Name: ""}
			peer.OpenedAt = w.Mono()
			peer.OpenResult = &result
			line := fmt.Sprintf("%d OpenConnection version=%s game=%s id=%#x flag=%d a=%#x b=%#x counter=%d -> result %d (lobby type %d)",
				addr.Port, pyReprStr(version), pyReprStr(game), playerID, flag2, a, b, counter, result, peer.LobbyType)
			if result == ResultOK {
				w.Log(line)
			} else if result == ResultVersionMismatch {
				w.Warn(line + fmt.Sprintf("; REFUSED: client version %s, this server speaks %s", pyReprStr(version), pyReprStr(netv2.VersionString)))
			} else {
				w.Warn(line + fmt.Sprintf("; REFUSED: client game %s, this server serves %s", pyReprStr(game), pyReprStr(netv2.GameString)))
			}
			replies = append(replies, netv2.OpenConnectionResponse(flag2, uint64(result), counter, w.ServerCounter))
		case mtype == netv2.MsgOpenConnectionResponse:
			_ = r.bits(2)
			code := r.bits(3)
			echo, theirs := r.bits(32), r.bits(32)
			w.Logf("%d OpenConnectionResponse code=%d echo=%d counter=%d", addr.Port, code, echo, theirs)
			if code == ResponseAck && !peer.Connected && echo != w.ServerCounter {
				w.Noise(fmt.Sprintf("%d OpenConnectionResponse with echo %d, expected %d: ignored (xuid %#x, handshake open %.1f s)", addr.Port, echo, w.ServerCounter, peer.Xuid, w.Mono()-peer.OpenedAt), diag.Warn)
			} else if code == ResponseAck && !peer.Connected && (peer.OpenResult == nil || *peer.OpenResult != ResultOK) {
				res := "none"
				if peer.OpenResult != nil {
					res = fmt.Sprint(*peer.OpenResult)
				}
				w.Noise(fmt.Sprintf("%d OpenConnectionResponse ack after an OpenConnection answered with result %s: ignored (xuid %#x)", addr.Port, res, peer.Xuid), diag.Warn)
			} else if code == ResponseAck && !peer.Connected {
				owner := w.FreeOwner()
				if owner == 0 {
					w.Warnf("%d refused: no relay owner id free in this world (1..%d; %d taken or quarantined, %d connected); xuid %#x", addr.Port, RelayOwnerLast,
						len(w.OwnerIDsTaken()), countConnected(w), peer.Xuid)
					w.Send(peer, true, w.Goodbye(peer))
					w.DropPeer(peer, "no relay owner id free")
					continue
				}
				peer.Owner = owner
				w.setPeerState(peer, PeerConnected, "handshake ack")
				peer.ConnectedAt = w.Mono()
				run := w.runOf(peer.Xuid)
				runS := ""
				if run != "" {
					runS = " run=" + run
				}
				w.Logf("%d *** CONNECTED *** (relay owner id %d; xuid %#x%s, lobby type %d, handshake %.0f ms, %d other player(s) here)", addr.Port, peer.Owner, peer.Xuid, runS,
					peer.LobbyType, (peer.ConnectedAt-peer.OpenedAt)*1000, len(w.OthersConnected(peer)))
				w.note("join", peer.Owner, fmt.Sprintf("xuid %#x lobby type %d", peer.Xuid, peer.LobbyType))
				if peer.Objs.Len() > 0 {
					w.Logf("%d %d object(s) sent before the handshake ended: relayed now under owner %d", addr.Port, peer.Objs.Len(), peer.Owner)
					w.RelayHeldObjects(peer)
				}
				replies = append(replies, netv2.TimeSync(0, uint64(w.NowMs())), netv2.ConnectionSyncDone(w.ServerCounter))
				if w.Roadtrip || peer.LobbyType == LobbyTypeCarMeet {
					lobby := uint64(LobbyTypeCarMeet)
					if w.Roadtrip {
						lobby = LobbyTypeRoadTrip
					}
					replies = append(replies, netv2.SessionHostCreate(1, func(b *bits.Writer) {
						b.Bits(0, netv2.SessionHostVarBits)
						b.Bits(lobby, 3)
					}))
				} else {
					replies = append(replies, netv2.SessionHostCreate(1, nil))
				}
				replies = append(replies, netv2.WeatherCreate(uint64(peer.WxSeq), w.Wx))
				if !w.Roadtrip && peer.LobbyType != LobbyTypeCarMeet {
					peer.BlSlotSeq = 1
					replies = append(replies, netv2.ObjectMessage(netv2.MsgObjectCreate, bucketlist.Router, 0, bucketlist.TypeSlotArray, uint64(peer.BlSlotSeq),
						bucketlist.IDBits, bucketlist.TypeBits, w.Coop.Slots.Writer(w.Coop.Slots.Used())))
				}
				peer.WxReady = true
				if peer.LobbyType == LobbyTypeCarMeet {
					replies = append(replies, netv2.CarMeetCreate(1, nil))
				}
				w.Debugf("handshake", "%d -> TimeSync, ConnectionSyncDone, SessionHost + Weather create", addr.Port)
				if e := w.EarlyIntroduction(peer, "handshake"); e != "" {
					early = e
				}
				if w.Roadtrip {
					replies = append(replies, netv2.RoadTripCreate(uint64(peer.RtSeq), rtv("State", RTInitialising)))
					w.Debugf("handshake", "%d -> RoadTripSynced create (State=Initialising)", addr.Port)
				}
			}
		case mtype == netv2.MsgCloseConnection:
			reason, x, y := r.bits(3), r.bits(32), r.bits(32)
			w.Logf("%d CloseConnection reason=%d %#x %#x (the client closed its connection, #%d of its process)", addr.Port, reason, x, y, y)
			w.DropPeer(peer, "CloseConnection")
			return
		case mtype == netv2.MsgPing:
			value := r.bits(32)
			replies = append(replies, netv2.Pong(value, uint64(w.NowMs())))
		case mtype == netv2.MsgPong:
			echo, _ := r.bits(32), r.bits(32)
			if !w.admit(peer, mtype) {
				continue
			}
			if echo != 0 && peer.Connected {
				w.ClockSample(peer, echo)
			}
		case mtype == netv2.MsgServerTrafficMessage:
			kind := int(r.bits(4))
			a, s14, u5, b, flag := int(r.bits(8)), int(r.bits(14)), int(r.bits(5)), int(r.bits(8)), int(r.bits(1))
			seq := r.bits(32)
			if !w.admit(peer, mtype) {
				continue
			}
			w.Traffic.OnMessage(peer, kind, a, s14, u5, b, flag, seq)
		case traffic.Layouts()[mtype] != nil:
			if err := w.Traffic.OnUpdate(peer, mtype, br); err != nil {
				panic(err)
			}
		case mtype == netv2.MsgFinaliseSessionEntry:
			if !w.admit(peer, mtype) {
				continue
			}
			desc := "BEFORE the handshake completed"
			if peer.Connected {
				desc = fmt.Sprintf("%.0f ms after connect", (w.Mono()-peer.ConnectedAt)*1000)
			}
			rep := ""
			if peer.Entered {
				rep = " (already entered: repeated, answered again)"
			}
			w.Logf("%d FinaliseSessionEntry %s%s", addr.Port, desc, rep)
			replies = append(replies, netv2.FinaliseSessionEntry())
			if !peer.Entered {
				w.setPeerState(peer, PeerEntered, "FinaliseSessionEntry")
				w.Introduce(peer, "finalise")
				w.SendGameOptions(peer, nil)
				w.Traffic.Join(peer, false)
				w.AssignPrivateUniverse(peer)
				if peer.VipHidden != nil {
					w.SendPlayerVar(peer, PlayerVarFlags, peer.VipHidden.Width, uint64(peer.VipHidden.Value), false)
					peer.VipHidden = nil
				}
			}
			consoleVars := func(b *bits.Writer) {
				b.Bits(ConsoleVarSessionEntryFinalised, netv2.ConsoleVarIDBits)
				b.Bits(1, 1)
			}
			replies = append(replies, netv2.ServerUpdate(3, 0, netv2.RouterBits[3][0], uint64(peer.ConsoleSeq), consoleVars))
			peer.ConsoleSeq = (peer.ConsoleSeq + 1) % 0xFF
			if peer.ConsoleOid != nil {
				var vb bits.Writer
				consoleVars(&vb)
				w.RememberAndRelay(peer, netv2.MsgObjectUpdate, 3, *peer.ConsoleOid, nil, 0, vb.Pos, vb.Value())
			}
		case mtype == netv2.MsgObjectRequestOwnership:
			router := int(r.bits(4))
			rb, ok := netv2.RouterBits[router]
			if !ok {
				k := [2]int{mtype, router}
				if _, seen := peer.UnmappedLogged[k]; !seen {
					peer.UnmappedLogged[k] = struct{}{}
					w.Warnf("%d RequestOwnership on unmapped router %d; stop parsing (owner %d xuid %#x, %d bits left, types %s); not logged again for this client",
						addr.Port, router, peer.Owner, peer.Xuid, br.Size-br.Pos, intsRepr(tail6(types)))
				}
				broke = true
				break loop
			}
			idBits, typeBits := rb[0], rb[1]
			tid := 0
			if typeBits != 0 {
				tid = int(r.bits(typeBits))
			}
			a, b := r.bits(1), r.bits(1)
			oid := int(r.bits(idBits))
			if !w.admit(peer, mtype) {
				continue
			}
			if router == bucketlist.Router {
				w.Logf("%d RequestOwnership router=%d type=%d key=%d%d id=%d -> ignored", addr.Port, router, tid, a, b, oid)
				continue
			}
			w.Logf("%d RequestOwnership router=%d type=%d key=%d%d id=%d -> grant", addr.Port, router, tid, a, b, oid)
			replies = append(replies, w.OwnershipGrant(peer, router, tid, oid, idBits))
		case mtype == netv2.MsgRequestRace && w.RequestRaceBlocked(peer):
			if !w.admit(peer, mtype) {
				continue
			}
			ev := w.Event
			pr := w.PendingRace
			var parts string
			if ev != nil && !ev.Over {
				parts += fmt.Sprintf("event with %d participant(s), %s, %.0f s old", len(ev.People), ev.stage(), w.Mono()-orNow(ev.Started, w.Mono()))
			}
			if pr != nil && pr.Leader != peer {
				sep := ""
				if ev != nil && !ev.Over {
					sep = "; "
				}
				parts += fmt.Sprintf("%srequest of owner %d pending, %.0f s ago", sep, pr.Leader.Owner, w.Mono()-w.RaceRequestedAt)
			}
			w.Logf("%d MsgRequestRace (owner %d) ignored: an event is already being set up or running in this lobby (%s)", addr.Port, peer.Owner, parts)
		case mtype == netv2.MsgRequestRace:
			if !w.admit(peer, mtype) {
				continue
			}
			if w.PendingRace != nil && w.PendingRace.Leader == peer {
				peer.DupRequests++
			}
			busy := w.CloseFinishedEvent()
			w.AcceptRequestedOptions(peer)
			if !w.Roadtrip {
				w.GameOptionsOwner = peer
				w.SetLeader(peer, "requested the race")
			}
			w.Logf("%d MsgRequestRace (owner %d) -> RaceReady=1", addr.Port, peer.Owner)
			w.SetSessionHostBool(SHRaceReady, 1, "race requested")
			w.PendingRace = &PendingRace{w.Mono() + RaceInviteSeconds, peer}
			w.RaceRequestedAt = w.Mono()
			w.RaceHardDeadline = w.Mono() + RaceJoinGraceSeconds
			var invited []*Peer
			for _, q := range w.Others(peer) {
				if q.Connected && q.Entered && !busy[q] {
					invited = append(invited, q)
				}
			}
			for _, q := range invited {
				w.Send(q, false, netv2.RequestRaceRelay())
			}
			w.Logf("MsgRequestRace relayed to %d other client(s): %s (invite window %.0f s)", len(invited), owners(invited), RaceInviteSeconds)
		case mtype == netv2.MsgReadyRequest:
			a, b := r.bits(1), r.bits(1)
			oid := r.bits(2)
			if a == 0 && b == 1 {
				r.bits(8)
			}
			ready := r.bits(1)
			if !w.admit(peer, mtype) {
				continue
			}
			accepted, notice := true, ""
			if ready != 0 {
				accepted, notice = w.carFitsEvent(peer, nil)
			}
			full := accepted && ready != 0 && w.PendingRace != nil && peer != w.PendingRace.Leader && w.Event == nil && w.EventSeatsFull(peer)
			tail := ""
			if !accepted {
				tail = fmt.Sprintf("; REFUSED, car class/PI %s, see the 'not eligible' line above", choicesPtrRepr(peer.Choices))
			}
			w.Logf("%d MsgReadyRequest key=%d%d id=%d ready=%d accepted=%s (owner %d%s)", addr.Port, a, b, oid, ready, pyBool(accepted && !full), peer.Owner, tail)
			code := uint64(0)
			if !accepted || full {
				code = 1
			}
			replies = append(replies, netv2.ReadyRequestResponse(code))
			if full {
				w.RefuseEventReady(peer, "MsgReadyRequest")
				continue
			}
			if !accepted {
				w.SetConsoleBool(peer, 4, 0, "car does not meet the event restrictions")
				peer.ConsoleVars[4] = false
				if ready != 0 && w.PendingRace != nil && peer != w.PendingRace.Leader {
					// An invitee: its game's car select can still switch to a car that fits (RetryRefusedReady). The notice waits for StartEvent.
					t := w.RaceRequestedAt
					peer.ReadyRetry = &t
					w.CheckInviteesReady() // a refused answer is still an answer: the event need not wait out the invite grace for it
				} else {
					w.noticeRefused(peer, notice)
				}
				continue
			}
			counted := false
			if ready != 0 && w.Roadtrip && (w.RtState == RTDriveToEvent || w.RtState == RTPrepareForEvent) && w.Event == nil {
				counted = true
				w.SetConsoleBool(peer, 4, 1, "ready request accepted")
				peer.ConsoleVars[4] = true
				if w.RtState == RTPrepareForEvent {
					w.RtCheckReady(false)
				}
			}
			if ready != 0 && w.PendingRace != nil && peer != w.PendingRace.Leader {
				counted = true
				w.SetConsoleBool(peer, 4, 1, "ready request accepted")
				peer.ConsoleVars[4] = true
				w.CheckInviteesReady()
			}
			if ready != 0 && !counted && !(w.PendingRace != nil && peer == w.PendingRace.Leader) {
				why := fmt.Sprintf("road trip state %d takes no Ready", w.RtState)
				if w.Event != nil {
					why = "an event is already running"
				} else if !w.Roadtrip {
					why = "no event request is pending"
				}
				w.Logf("%d Ready from owner %d not recorded: %s", addr.Port, peer.Owner, why)
			}
		case mtype == netv2.MsgTimeSync:
			r.bits(16)
			r.bits(32)
			w.Debugf("handshake", "%d TimeSync", addr.Port)
		case mtype == netv2.MsgConnectionSyncDone:
			w.Debugf("handshake", "%d ConnectionSyncDone %d", addr.Port, r.bits(32))
		case mtype == netv2.MsgObjectCreate || mtype == netv2.MsgObjectUpdate:
			if w.handleObject(peer, r, br, mtype, data, addr, types, &replies, &early) == handleBreak {
				broke = true
				break loop
			}
		case mtype == netv2.MsgRTSelectedNextEvent:
			ok := r.bits(1)
			if !w.admit(peer, mtype) {
				continue
			}
			w.Logf("%d SelectedNextRoadTripEvent ok=%d", addr.Port, ok)
		case mtype == netv2.MsgRTConfiguredGameOptions:
			a := r.bits(32)
			b := r.bits(16)
			if !w.admit(peer, mtype) {
				continue
			}
			w.Logf("%d ConfiguredGameOptionsForRoadTripEvent a=%d b=%d", addr.Port, a, b)
			w.RtConfigured = true
		case mtype == netv2.MsgVoteForTrack:
			a, b := r.bits(1), r.bits(1)
			r.bits(2)
			if a == 0 && b == 1 {
				r.bits(8)
			}
			track := int(r.bits(32))
			if !w.admit(peer, mtype) {
				continue
			}
			w.RtVote(peer, track)
		case mtype == netv2.MsgInProgressScoreUpdate || mtype == netv2.MsgLapScore || mtype == netv2.MsgScoreKeyframe:
			a, b := r.bits(1), r.bits(1)
			cid := r.bits(netv2.CarKeyIDBits)
			if a == 0 && b == 1 {
				r.bits(8)
			}
			start := br.Pos
			want := map[int]int{netv2.MsgInProgressScoreUpdate: 0, netv2.MsgLapScore: 3, netv2.MsgScoreKeyframe: 5}[mtype]
			var blobs []uint64
			for k := 0; k < want; k++ {
				if r.varlen() != 8 {
					break
				}
				blobs = append(blobs, r.bits(64))
			}
			if len(blobs) != want {
				w.Malformed(peer, fmt.Sprintf("score message %d", mtype))
				broke = true
				break loop
			}
			var progressRaw uint64
			var lap int
			switch mtype {
			case netv2.MsgInProgressScoreUpdate:
				progressRaw = r.bits(32)
				r.bits(1)
			case netv2.MsgLapScore:
				lap = int(r.bits(32))
				r.bits(2)
				ts := make([]string, len(blobs))
				for i, x := range blobs {
					ts[i] = pyfmt.ReprFloat(roundTo(math.Float64frombits(x), 3))
				}
				w.Logf("%d LapScore lap=%d times=[%s]", addr.Port, lap, strings.Join(ts, ", "))
			default:
				r.bits(32 + 24 + 3)
			}
			end := br.Pos
			if !w.admit(peer, mtype) {
				continue
			}
			br.Pos = start
			rest := r.intv(end - start)
			w.NoteRaceHeard(peer)
			w.Relay(peer, netv2.RelayScore(uint64(mtype), cid, uint64(peer.Owner), rest, end-start), true)
			switch mtype {
			case netv2.MsgLapScore:
				w.NoteLapScore(peer, lap, blobs)
			case netv2.MsgScoreKeyframe:
				w.NoteScoreKeyframe(peer)
			default:
				w.NoteProgress(peer, progressRaw)
			}
		case mtype == netv2.MsgTagCollision:
			k1 := readKey(br)
			k2 := readKey(br)
			var v [4]int
			for i := range v {
				v[i] = int(r.bits(3))
			}
			if !w.admit(peer, mtype) {
				continue
			}
			w.TagCollision(peer, k1, k2, v[0], v[1], v[2], v[3])
		case mtype == netv2.MsgCarMeetInviteRequest:
			inviteType, value := r.bits(2), r.bits(32)
			if !w.admit(peer, mtype) {
				continue
			}
			w.Logf("%d CarMeet InviteRequest type=%d value=%#x (owner %d)", addr.Port, inviteType, value, peer.Owner)
			w.CmInvite(peer, int(inviteType))
		case mtype == netv2.MsgCarMeetJoinOrCancel:
			join, joinType, value := r.bits(1), r.bits(2), r.bits(32)
			if !w.admit(peer, mtype) {
				continue
			}
			w.Logf("%d CarMeet JoinOrCancel join=%d joinType=%d value=%#x (owner %d)", addr.Port, join, joinType, value, peer.Owner)
			w.CmJoin(peer, int(join), int(joinType))
		case mtype == netv2.MsgCarMeetSessionReady:
			at := br.Pos
			bitsLeft := br.Size - br.Pos
			tailv := bits.Int(nil)
			take := minInt(bitsLeft, 600)
			if bitsLeft > 0 {
				tailv = r.intv(take)
			}
			br.Pos = at
			raw, _ := tailv.Bytes((take + 7) / 8)
			w.Logf("%d CarMeet SessionReady raw: %d bits left, %x", addr.Port, bitsLeft, raw)
			name, template := r.str(), r.str()
			if !w.admit(peer, mtype) {
				continue
			}
			w.CmSessionReady(peer, name, template)
		case mtype == netv2.MsgCarMeetConfiguredWager:
			w.Logf("%d CarMeet ConfiguredGameOptionsForWagerRace value=%#x", addr.Port, r.bits(32))
		case mtype == bucketlist.MsgSync:
			slot, challenge := bucketlist.S32(r.bits(32)), bucketlist.S32(r.bits(32))
			if !w.admit(peer, mtype) {
				continue
			}
			sends, text := w.Coop.Request(peer, slot, challenge, w.blWorld(peer), memberName)
			w.blApply(peer, "RequestSynchronize", sends, text)
		case mtype == bucketlist.MsgDesync:
			if !w.admit(peer, mtype) {
				continue
			}
			dropped := w.Coop.Cancel(peer)
			w.BlSlotsSync()
			tail := ""
			if dropped {
				tail = "; its waiting request is dropped"
			}
			w.Logf("%d BucketList RequestDesynchronize (owner %d)%s", addr.Port, peer.Owner, tail)
		case mtype == bucketlist.MsgEnded:
			ok := r.bits(1)
			if !w.admit(peer, mtype) {
				continue
			}
			out, other := w.Coop.Ended(peer, ok != 0, w.blWorld(peer))
			partner := "none"
			var partnerV any
			if other != nil {
				partner = fmt.Sprint(other.OwnerID())
				partnerV = other.OwnerID()
			}
			w.blApply(peer, fmt.Sprintf("ChallengeEnded ok=%d", ok), out, "partner "+partner)
			w.ev("bucketlist_end", kv("owner", peer.Owner), kv("ok", ok != 0), kv("partner", partnerV))
		case mtype == netv2.MsgDropLeadership:
			if !w.admit(peer, mtype) {
				continue
			}
			if w.Leader == peer && w.Event == nil {
				w.SetLeader(nil, "dropped leadership")
				w.ReleaseOptions(peer, "dropped leadership")
			} else {
				why := "it is not the leader"
				if w.Leader == peer {
					why = "an event is running"
				}
				w.Logf("%d DropLeadership (owner %d) ignored: %s", addr.Port, peer.Owner, why)
			}
		default:
			if netv2.IsUnusedMessage(mtype) {
				ok, err := netv2.SkipUnusedMessage(br, mtype)
				if err != nil {
					panic(err)
				}
				if ok {
					continue
				}
			}
			if _, seen := peer.UnknownLogged[mtype]; !seen {
				peer.UnknownLogged[mtype] = struct{}{}
				w.Logf("%d id=%d types=%s first-unknown=%d bits-left=%d raw=%s", addr.Port, hdr.AckID, intsRepr(types), mtype, br.Size-br.Pos, hex.EncodeToString(data))
			}
			broke = true
			break loop
		}
	}
	if !broke {
		left := br.Size - br.Pos
		if left >= 16 && w.LeftoverLogged < 8 {
			w.LeftoverLogged++
			rem := bits.FromBytes(data).Rsh(br.Pos)
			tb, _ := rem.Bytes((left + 7) / 8)
			if len(tb) > 60 {
				tb = tb[:60]
			}
			w.Warnf("%d packet of %d bytes: %d bits left after %d messages types=%s tail=%x (owner %d; only the first 8 such packets of the process are logged)",
				addr.Port, len(data), left, hdr.Count, intsRepr(tail6(types)), tb, peer.Owner)
		}
	}
	if len(data) > 122 && !containsInt(types, 71) && w.BigLogged < 10 {
		w.BigLogged++
		w.Logf("%d big packet %d bytes types=%s: %s", addr.Port, len(data), intsRepr(types), hex.EncodeToString(data))
	}
	if len(replies) > 0 || hdr.AckID != netv2.UnreliableID {
		w.Send(peer, false, replies...)
	}
	if early != "" {
		w.Introduce(peer, early)
	}
}

func containsInt(a []int, v int) bool {
	for _, x := range a {
		if x == v {
			return true
		}
	}
	return false
}

func countConnected(w *World) int {
	n := 0
	for _, q := range w.Peers.Values() {
		if q.Connected {
			n++
		}
	}
	return n
}

func allFinite(p [3]float64) bool {
	for _, c := range p {
		if math.IsNaN(c) || math.IsInf(c, 0) {
			return false
		}
	}
	return true
}

func readKey(br *bits.Reader) carKey {
	kind, id, owner, err := netv2.ReadCarKey(br)
	if err != nil {
		panic(err)
	}
	return carKey{kind, id, owner}
}

func choicesPtrRepr(c *Choices) string {
	if c == nil {
		return "None"
	}
	return choicesRepr(c)
}

func (a Addr) pyRepr() string { return fmt.Sprintf("('%s', %d)", a.IP, a.Port) }

type handleResult int

const (
	handleContinue handleResult = iota
	handleBreak
)

var _ = os.Getenv
var _ = gameopts.IDBits
