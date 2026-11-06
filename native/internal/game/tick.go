package game

import (
	"fmt"
	"math"
	"sort"
	"strings"

	"fh2mp/internal/bits"
	"fh2mp/internal/diag"
	"fh2mp/internal/netv2"
	"fh2mp/internal/pyfmt"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/reliable"
)

// HandleFailed is a datagram whose handling raised: the server keeps serving; the rest of that datagram is lost.
func (w *World) HandleFailed(rec any, data []byte, addr Addr) {
	known := w.rx.peer != nil && w.rx.peer.Addr == addr && w.rx.size == len(data)
	who := "no client state for this address"
	types := "?"
	failed := "?"
	if known {
		who = fmt.Sprintf("owner %d xuid %#x", w.rx.peer.Owner, w.rx.peer.Xuid)
		t := w.rx.types
		if len(t) > 8 {
			t = t[len(t)-8:]
		}
		types = intsRepr(t)
		if len(w.rx.types) > 0 {
			failed = fmt.Sprint(w.rx.types[len(w.rx.types)-1])
		}
	}
	ctx := fmt.Sprintf("%d %s, %d bytes, message types read %s, failed on type %s", addr.Port, who, len(data), types, failed)
	if err, ok := rec.(error); ok && err == bits.ErrTruncated {
		first := data
		if len(first) > 64 {
			first = first[:64]
		}
		w.LogEvery([2]any{"truncated", addr}, 10.0, fmt.Sprintf("%s: truncated message, datagram dropped; first bytes %x", ctx, first), diag.Warn)
		return
	}
	w.ExcEvery([2]any{"handle", fmt.Sprint(rec)}, 10.0, fmt.Sprintf("error handling a datagram (%s); datagram dropped; bytes %x", ctx, data), rec, diag.Error)
}

// DropPeer removes a client and tells the others its objects are gone.
func (w *World) DropPeer(peer *Peer, why string) {
	wasConnected := peer.Connected
	if w.GameOptionsOwner == peer {
		w.GameOptionsOwner = nil
	}
	if w.Leader == peer {
		w.Leader = nil
	}
	w.Peers.Delete(peer.Addr)
	if w.Dirty.Has(peer) {
		w.FlushPeer(peer)
	}
	peer.Connected = false
	w.Traffic.Leave(peer)
	other := w.Coop.Leave(peer)
	w.BlSlotsSync()
	if other != nil {
		w.Logf("%d left during a bucket-list challenge with owner %d", peer.Addr.Port, other.OwnerID())
	}
	w.RtVotes.Delete(peer)
	if w.Roadtrip && !w.anyConnected() {
		w.RtState, w.RtEndMs, w.RtWinner = RTInitialising, 0, -1
		w.RtLastRace, w.RtLastLoc = nil, nil
		w.RtPendingDrive, w.RtDriveWait, w.RtConfigured = false, false, false
		w.RtIdx = 0
	}
	mono := w.Mono()
	ev0 := w.Event
	state := "road trip"
	switch {
	case !wasConnected:
		state = "not connected yet"
	case ev0 != nil && inList(ev0.People, peer):
		state = "in the event"
	case !w.Roadtrip:
		state = "free roam"
	}
	run := w.runOf(peer.Xuid)
	runS := ""
	if run != "" {
		runS = " run=" + run
	}
	var conn string
	switch {
	case wasConnected && peer.ConnectedAt != 0:
		conn = fmt.Sprintf("connected %.0f s", mono-peer.ConnectedAt)
	case peer.OpenedAt != 0:
		conn = fmt.Sprintf("handshake open %.1f s", mono-peer.OpenedAt)
	default:
		conn = "never connected"
	}
	line := fmt.Sprintf("%d removed (%s); owner %d had %d objects; xuid %#x%s, %s, last packet %.1f s ago, %s, entered=%s",
		peer.Addr.Port, why, peer.Owner, peer.Objs.Len(), peer.Xuid, runS, conn, mono-peer.LastRx, state, pyBool(peer.Entered))
	if peer.KickReason != nil {
		line += ", kick pending: " + *peer.KickReason
	}
	if peer.MalformedCount != 0 {
		line += fmt.Sprintf(", %d malformed message(s)", peer.MalformedCount)
	}
	if strings.HasPrefix(why, "no packets") || strings.HasPrefix(why, "car policy") {
		w.Warn(line)
	} else {
		w.Log(line)
	}
	var secs any
	if peer.ConnectedAt != 0 {
		secs = math.RoundToEven((w.Mono()-peer.ConnectedAt)*10) / 10
	}
	var car any
	if peer.ModelID != nil {
		car = *peer.ModelID
	}
	var kicked any
	if peer.KickReason != nil {
		kicked = *peer.KickReason
	}
	w.ev("session_end", kv("xuid", fmt.Sprintf("%#x", peer.Xuid)), kv("owner", peer.Owner), kv("why", why), kv("entered", peer.Entered), kv("seconds", secs),
		kv("car", car), kv("top_kmh", math.RoundToEven(peer.TopKmh)), kv("predictions", peer.CarsRelayed), kv("kicked", kicked))
	if peer.CarsRelayed != 0 {
		stock := "None"
		if peer.ModelID != nil {
			if s, ok := w.cat().TopSpeed[int(*peer.ModelID)]; ok {
				stock = fmt.Sprint(s)
			}
		}
		w.Logf("%d session summary: xuid %#x car %s (stock %s km/h), top measured speed %.0f km/h, farthest position %.0f, highest point %.0f, fastest climb %.0f km/h",
			peer.Addr.Port, peer.Xuid, modelStr(peer), stock, peer.TopKmh, peer.MaxAbs, peer.MaxY, peer.TopClimb)
	}
	win := &peer.Window
	if win.Sent != 0 || peer.DupObjects != 0 || peer.DupRequests != 0 {
		msg := fmt.Sprintf("%d delivery: %d reliable packets, %d messages resent (%d creates), peak %d waiting, %d given up, unacked at the end %d; client resends seen: %d objects, %d race requests",
			peer.Addr.Port, win.Sent, win.Resent, win.ResentCreates, win.Peak, win.Lost, len(win.Pending), peer.DupObjects, peer.DupRequests)
		if win.Lost != 0 {
			w.Warn(msg)
		} else {
			w.Log(msg)
		}
	}
	nowM := w.Mono()
	w.drops = append(w.drops, dropRow{nowM, peer.Owner, peer.Xuid, peer.LastRx, peer.Pos, peer.CarID, why})
	if len(w.drops) > 64 {
		w.drops = w.drops[len(w.drops)-64:]
	}
	if w.WipeAt == 0 && nowM-w.WipeLast > 30.0 {
		n := 0
		for _, d := range w.drops {
			if nowM-d.t <= WipeWindowS {
				n++
			}
		}
		if n >= WipeMinDrops {
			w.WipeAt = nowM + 1.0
		}
	}
	if w.PendingRace != nil && w.PendingRace.Leader == peer {
		w.EndEvent("the event leader left before it started", false)
	}
	if ev := w.Event; ev != nil && inList(ev.People, peer) && !ev.Racing && !ev.Over {
		ev.People = removeFrom(ev.People, peer)
		w.Logf("event setup: participant %d is gone, %d participant(s) remain", peer.Owner, len(ev.People))
		any := false
		for _, q := range ev.People {
			any = any || q.Connected
		}
		if !any {
			w.EndEvent("every participant left during setup", false)
		} else {
			w.MaybeBeginRace()
			w.MaybeGoRace()
		}
	}
	if Reliable && peer.Owner != 0 {
		for _, q := range w.Peers.Values() {
			q.Window.ForgetOwner(peer.Owner, netv2.RouterBits)
		}
	}
	if peer.Owner != 0 {
		for _, key := range peer.Objs.Keys() {
			rb := netv2.RouterBits[key.Router]
			idBits := rb[0]
			n := peer.RelayNext[key]
			owner := peer.Owner
			k := key
			w.RelayNumbered(peer, key, n, func(sq int) netv2.Msg {
				return netv2.RelayDelete(uint64(k.Router), uint64(k.ID), uint64(owner), idBits, uint64(sq))
			})
		}
		w.OwnerQuarantine[peer.Owner] = w.Mono() + OwnerQuarantineS
		for _, q := range w.Peers.Values() {
			for k := range q.SeqShift {
				if k.Owner == peer.Owner {
					delete(q.SeqShift, k)
				}
			}
		}
	}
}

// ReportWipe: several clients vanished together. Logging only.
func (w *World) ReportWipe(now float64) {
	w.WipeAt, w.WipeLast = 0, now
	var gone []dropRow
	for _, d := range w.drops {
		if now-d.t <= WipeWindowS+2.0 {
			gone = append(gone, d)
		}
	}
	if len(gone) < WipeMinDrops {
		return
	}
	t0 := math.Inf(1)
	for _, d := range gone {
		t0 = math.Min(t0, d.lastRx)
	}
	w.Logf("WIPE: %d client(s) removed together, %d left in this lobby; they went silent about %.0f s ago", len(gone), w.Peers.Len(), now-t0)
	for _, d := range gone {
		p := "unknown"
		if d.pos != nil {
			p = fmt.Sprintf("(%.0f,%.0f,%.0f)", pyfmt.F(d.pos[0]), pyfmt.F(d.pos[1]), pyfmt.F(d.pos[2]))
		}
		car := "None"
		if d.car != nil {
			car = fmt.Sprint(*d.car)
		}
		w.Logf("WIPE:   owner %d xuid %#x last heard %+.1f s, last car position %s, car object %s, %s", d.owner, d.xuid, t0-d.lastRx, p, car, d.why)
	}
	shown, objs := 0, 0
	for _, r := range w.recent {
		if t0-WipeContextS <= r.t && r.t <= t0+3.0 {
			if r.kind == "obj" {
				if objs >= 12 {
					continue
				}
				objs++
			} else if shown >= 90 {
				continue
			} else {
				shown++
			}
			w.Logf("WIPE:   %+6.1f s %-5s owner %3d %s", r.t-t0, r.kind, r.owner, r.detail)
		}
	}
}

// AuditVisibility reports a player whose car the others cannot be shown.
func (w *World) AuditVisibility(now float64) {
	if now-w.VisibilityChecked < VisibilityCheckSecs {
		return
	}
	w.VisibilityChecked = now
	for _, q := range w.Peers.Values() {
		if q.Connected && q.Entered && q.ModelID == nil && q.CarsRelayed >= VisibilityMinPreds && !q.InvisibleLogged {
			q.InvisibleLogged = true
			others := len(w.Others(q))
			w.Logf("%d INVISIBLE? owner %d xuid %#x has sent %d car predictions but no car model: the other %d player(s) have no car to show for it", q.Addr.Port, q.Owner, q.Xuid, q.CarsRelayed, others)
			var secs any
			if q.ConnectedAt != 0 {
				secs = math.RoundToEven((now-q.ConnectedAt)*10) / 10
			}
			w.ev("no_car_model", kv("xuid", fmt.Sprintf("%#x", q.Xuid)), kv("owner", q.Owner), kv("predictions", q.CarsRelayed), kv("others", others), kv("seconds", secs))
		}
	}
}

// Tick is server-initiated traffic: the client times out after 10 s of silence.
func (w *World) Tick() {
	now := w.Mono()
	if len(w.Bots) > 0 {
		w.botMaintain()
	}
	if w.WipeAt != 0 && now >= w.WipeAt {
		w.ReportWipe(now)
	}
	w.KickForCars()
	w.AuditVisibility(now)
	w.BlFallback(now)
	ev := w.Event
	if ev != nil && !ev.Begun && !w.Roadtrip && now-orNow(ev.Started, now) > EventLoadTimeout {
		w.DropUnloaded(now)
	}
	if w.Event != nil && w.Event.GoAt != nil {
		if now >= *w.Event.GoAt {
			w.GoRace(fmt.Sprintf("backstop after %.0f s", RaceGridBackstopSecs))
		} else {
			w.GridWaitReport(now)
		}
	}
	w.CheckRaceEnd(now)
	if w.Leader != nil && w.Event == nil && w.PendingRace == nil && now-w.LeaderSince > LeaderSetupSeconds {
		former := w.Leader
		w.SetLeader(nil, "setup abandoned")
		w.ReleaseOptions(former, "setup abandoned")
	}
	carMeetHere := w.Carmeet
	for _, q := range w.Peers.Values() {
		carMeetHere = carMeetHere || q.LobbyType == LobbyTypeCarMeet
	}
	if carMeetHere {
		w.CmTick(w.NowMs())
	}
	if w.Roadtrip {
		w.RoadTripTick(now)
	}
	if w.PendingRace != nil && now >= w.PendingRace.Deadline {
		leader := w.PendingRace.Leader
		asked := w.RaceRequestedAt
		var joining []*Peer
		for _, q := range w.Others(leader) {
			if q.Connected && q.Entered && q.ConsoleVars[3] && !q.ConsoleVars[4] && math.Abs(q.GoptsWrittenAt-asked) > RaceOwnRequestSecs {
				joining = append(joining, q)
			}
		}
		if len(joining) > 0 && leader.Connected && now < w.RaceHardDeadline {
			w.PendingRace = &PendingRace{now + 2.0, leader}
			w.LogEvery([2]any{"invite-wait", asked}, 10.0, fmt.Sprintf("event start held for owner %s: accepted the invite but no Ready yet (still choosing a car?); leader owner %d, invite window of %.0f s is over, grace ends in %.0f s",
				owners(joining), leader.Owner, RaceInviteSeconds, w.RaceHardDeadline-now), diag.Info)
		} else {
			if len(joining) > 0 && leader.Connected {
				w.Warnf("invite grace over (%.0f s after the request): starting without owner %s, who accepted the invite but never reported Ready", RaceJoinGraceSeconds, owners(joining))
			}
			w.PendingRace = nil
			if leader.Connected {
				w.StartEvent(leader)
			}
		}
	}
	w.weatherTick(now)
	w.Traffic.Tick(now)
	if now >= w.MtNext {
		w.MtNext = now + 30
		if len(w.MtSeen) > 0 && w.anyConnected() {
			keys := make([]string, 0, len(w.MtSeen))
			vals := map[string]int{}
			for k, v := range w.MtSeen {
				ks := fmt.Sprint(k)
				keys = append(keys, ks)
				vals[ks] = v
			}
			sort.Strings(keys)
			parts := make([]string, len(keys))
			for i, k := range keys {
				parts[i] = fmt.Sprintf("%s:%d", k, vals[k])
			}
			w.Debug("net", "msg types seen (last 30 s): "+strings.Join(parts, " "))
		}
		w.MtSeen = map[int]int{}
	}
	for _, peer := range w.Peers.Values() {
		if !peer.Connected && now-peer.LastRx > PendingSeconds {
			w.Peers.Delete(peer.Addr)
			res := "None"
			if peer.OpenResult != nil {
				res = fmt.Sprint(*peer.OpenResult)
			}
			w.LogEvery("handshake-abandoned", 10.0, fmt.Sprintf("%d handshake abandoned: xuid %#x opened a connection %.0f s ago (answered with result %s) and has been silent for %.0f s (limit %.0f s); never sent the third leg",
				peer.Addr.Port, peer.Xuid, now-peer.OpenedAt, res, now-peer.LastRx, PendingSeconds), diag.Warn)
			continue
		}
		if peer.Connected && now-peer.LastRx > StaleSeconds {
			w.DropPeer(peer, fmt.Sprintf("no packets for %.0f s", StaleSeconds))
			continue
		}
		if len(peer.Window.Pending) > 0 {
			again := peer.Window.Due(now, peer.LastRx)
			if len(again) > 0 {
				peer.Outbox = append(append([]netv2.Baked(nil), again...), peer.Outbox...)
				w.Send(peer, false)
				w.LogEverySub([2]any{"resend", peer.Addr}, 30.0, fmt.Sprintf("%d reliable: %d message(s) unacked for %.1f s sent again (owner %d, %d resent so far, %d packet(s) waiting)",
					peer.Addr.Port, len(again), reliable.ResendSeconds, peer.Owner, peer.Window.Resent, len(peer.Window.Pending)), diag.Debug, "reliable")
			}
			if peer.Window.Lost != peer.LostSeen {
				w.LogEvery([2]any{"giveup", peer.Addr}, 30.0, fmt.Sprintf("%d reliable: gave up on %d unacked packet(s) (owner %d xuid %#x; more than %d waiting, last packet from it %.1f s ago): its objects may stay stuck; %d given up in total",
					peer.Addr.Port, peer.Window.Lost-peer.LostSeen, peer.Owner, peer.Xuid, reliable.MaxPending, now-peer.LastRx, peer.Window.Lost), diag.Warn)
				peer.LostSeen = peer.Window.Lost
			}
		}
		if peer.Connected && now-peer.LastKeepalive >= KeepaliveSeconds {
			peer.LastKeepalive = now
			msgs := []netv2.Msg{netv2.PingLive(func() uint64 { return uint64(w.NowMs()) })}
			if now-peer.LastTimeSync >= TimeSyncSeconds {
				peer.LastTimeSync = now
				peer.TimeSyncSeq = (peer.TimeSyncSeq + 1) & 0xFFFF
				msgs = append(msgs, netv2.TimeSyncLive(uint64(peer.TimeSyncSeq), func() uint64 { return uint64(w.NowMs()) }))
			}
			w.Send(peer, false, msgs...)
		}
	}
}

var _ = pyjson.Pair{}
