package game

import (
	"fmt"
	"math"
	"os"
	"sort"
	"strings"

	"fh2mp/internal/bits"
	"fh2mp/internal/bucketlist"
	"fh2mp/internal/diag"
	"fh2mp/internal/gameopts"
	"fh2mp/internal/netv2"
	"fh2mp/internal/replay"
)

// GameOptionsView is the settings last delivered to / written by a client (size, payload).
type GameOptionsView = gameopts.Options

// GameOptionsTransferID is the transfer-object id of the lobby's GameOptions (router 6, type 1).
const GameOptionsTransferID = 0

func (w *World) blKey(subject, receiver bucketlist.Member) (bucketlist.Key, bool) {
	s, _ := subject.(*Peer)
	if s == nil || s.BlOid == nil {
		return bucketlist.Key{}, false
	}
	if subject == receiver {
		return bucketlist.Key{Own: true, ID: uint64(*s.BlOid)}, true
	}
	return bucketlist.Key{ID: uint64(*s.BlOid), Owner: uint64(s.Owner)}, true
}

func (w *World) blWorld(peer *Peer) []bucketlist.Member {
	out := make([]bucketlist.Member, 0, 8)
	for _, q := range w.Others(peer) {
		out = append(out, q)
	}
	return append(out, peer)
}

func memberName(m bucketlist.Member) string { return "" }

// BlObject handles a client's own bucket-list PlayerState (router 12).
func (w *World) BlObject(peer *Peer, mtype, oid int, tid *int, seq, size int, payload bits.Int) {
	if mtype == netv2.MsgObjectCreate {
		peer.BlOid = &oid
	} else if peer.BlOid == nil {
		peer.BlOid = &oid
	}
	if (tid != nil && *tid != 0) || size == 0 {
		kind := "update"
		if mtype == netv2.MsgObjectCreate {
			kind = "create"
		}
		t := "None"
		if tid != nil {
			t = fmt.Sprint(*tid)
		}
		w.Logf("%d bucket-list object %s id=%d type=%s bits=%d", peer.Addr.Port, kind, oid, t, size)
		return
	}
	state, err := bucketlist.DecodePlayerState(size, payload)
	if err != nil {
		w.Logf("%d bucket-list PlayerState update not understood (%v); %d bits = %#x", peer.Addr.Port, err, size, payload.Uint64())
		return
	}
	for _, n := range state.Names {
		if _, seen := peer.BlState[n]; !seen {
			peer.BlStateOrder = append(peer.BlStateOrder, n)
		}
		peer.BlState[n] = state.Values[n]
	}
	w.Logf("%d bucket-list PlayerState %s (owner %d)", peer.Addr.Port, blStateRepr(state), peer.Owner)
	if peer.BlState["RequestingPartner"] != 0 {
		peer.BlCheckAt = w.Mono() + BlRequestGraceSecs
	} else {
		peer.BlCheckAt = 0
	}
	if _, ok := state.Values["State"]; ok {
		sends, text := w.Coop.StateChanged(peer)
		w.blApply(peer, "state", sends, text)
	}
}

func blStateRepr(s *bucketlist.State) string {
	parts := make([]string, len(s.Names))
	for i, n := range s.Names {
		v := s.Values[n]
		if n == "Score" {
			parts[i] = fmt.Sprintf("'%s': %s", n, floatRepr(v))
		} else {
			parts[i] = fmt.Sprintf("'%s': %d", n, int64(v))
		}
	}
	return "{" + strings.Join(parts, ", ") + "}"
}

// BlFallback: a client that joins a challenge somebody else is already waiting at sent only its PlayerState.
func (w *World) BlFallback(now float64) {
	for _, peer := range w.Peers.Values() {
		if peer.BlCheckAt == 0 || now < peer.BlCheckAt {
			continue
		}
		peer.BlCheckAt = 0
		st := peer.BlState
		slot, challenge := int64(st["CurrentSlotId"]), int64(st["CurrentChallengeId"])
		_, paired := w.Coop.Partner[peer]
		_, running := w.Coop.Running[peer]
		if !peer.Connected || st["RequestingPartner"] == 0 || slot == 0 || challenge == 0 || w.Coop.Waiting(peer) || paired || running {
			continue
		}
		sends, text := w.Coop.Request(peer, slot, challenge, w.blWorld(peer), memberName)
		w.blApply(peer, "RequestSynchronize (from its PlayerState, no request message came)", sends, text)
	}
}

// BlSlotsSync sends the slot array entries that changed to every client of this world.
func (w *World) BlSlotsSync() {
	seen := map[int]bool{}
	var changed []int
	for _, i := range w.Coop.Changed {
		if !seen[i] {
			seen[i] = true
			changed = append(changed, i)
		}
	}
	sort.Ints(changed)
	w.Coop.Changed = nil
	if len(changed) == 0 {
		return
	}
	write := w.Coop.Slots.Writer(changed)
	for _, q := range w.Peers.Values() {
		if q.Connected && q.Entered && q.BlSlotSeq != 0 {
			q.BlSlotSeq = (q.BlSlotSeq + 1) % 0xFF
			w.Send(q, false, netv2.ObjectMessage(netv2.MsgObjectUpdate, bucketlist.Router, 0, 0, uint64(q.BlSlotSeq), bucketlist.IDBits, 0, write))
		}
	}
	rows := make([]string, len(changed))
	for k, i := range changed {
		e := w.Coop.Slots.Entries[i]
		rows[k] = fmt.Sprintf("[%d, %d, %d]", e[0], e[1], e[2])
	}
	w.Logf("bucket-list slot array: entries %s -> [%s]", intList(changed), strings.Join(rows, ", "))
}

func (w *World) blApply(peer *Peer, what string, out []bucketlist.Send, text string) {
	w.BlSlotsSync()
	if len(out) == 0 && text == "" {
		return
	}
	w.Logf("%d BucketList %s (owner %d): %s", peer.Addr.Port, what, peer.Owner, text)
	for _, s := range out {
		w.Send(s.To.(*Peer), false, s.Msg)
	}
	if len(out) > 0 {
		w.ev("bucketlist", kv("what", what), kv("owner", peer.Owner), kv("sent", len(out)))
	}
}

// RememberAndRelay stores a client's object message in its history and relays it to the others.
func (w *World) RememberAndRelay(peer *Peer, mtype, router, oid int, tid *int, seq, size int, payload bits.Int) {
	rb := netv2.RouterBits[router]
	idBits, typeBits := rb[0], rb[1]
	if peer.Owner != 0 || peer.Xuid != 0 {
		newID := peer.Xuid
		if newID == 0 {
			newID = netv2.XuidPattern + uint64(peer.Owner)
		}
		if newID != netv2.XuidPattern {
			var n int
			payload, n = netv2.RewriteXuid(payload, size, newID)
			if n > 0 {
				w.Logf("%d router %d object %d: user id rewritten x%d -> %#x for other clients", peer.Addr.Port, router, oid, n, newID)
			}
		}
	}
	key := ObjKey{router, oid}
	n := peer.RelayNext[key]
	peer.RelayNext[key] = n + 1
	seqN := n % 0xFF
	hist, _ := peer.Objs.Get(key)
	t := 0
	if tid != nil {
		t = *tid
	}
	hist = append(hist, replay.Entry{Type: mtype, TID: t, Seq: seqN, Size: size, Payload: payload})
	if router == 1 && size >= 200 {
		kind := "update"
		if mtype == netv2.MsgObjectCreate {
			kind = "create"
		}
		raw, _ := payload.Bytes((size + 7) / 8)
		w.note("obj", peer.Owner, fmt.Sprintf("%s router 1 %d bits hex=%x", kind, size, raw))
	}
	limit, ok := peer.CompactAt[key]
	if !ok {
		limit = 256
	}
	if len(hist) >= limit {
		hist = replay.Compact(router, hist, netv2.MsgObjectUpdate)
		peer.CompactAt[key] = maxInt(256, 2*len(hist))
		if len(hist) > RelayHistoryCap {
			hist = append(hist[:1], hist[len(hist)-RelayHistoryCap+1:]...)
		}
	}
	peer.Objs.Set(key, hist)
	if peer.Owner == 0 {
		return
	}
	var tidv uint64
	if tid != nil {
		tidv = uint64(*tid)
	}
	owner := peer.Owner
	w.RelayNumbered(peer, key, n, func(sq int) netv2.Msg {
		return netv2.RelayObjectMessage(uint64(mtype), uint64(router), uint64(oid), uint64(owner), tidv, uint64(sq), idBits, typeBits, size, payload)
	})
}

func maxInt(a, b int) int {
	if a > b {
		return a
	}
	return b
}

// RelayHeldObjects: at the end of the handshake, relay what the client sent before it had a relay owner id.
func (w *World) RelayHeldObjects(peer *Peer) {
	for _, key := range peer.Objs.Keys() {
		hist, _ := peer.Objs.Get(key)
		rb := netv2.RouterBits[key.Router]
		idBits, typeBits := rb[0], rb[1]
		for n := range hist {
			e := hist[n]
			if peer.Xuid == 0 {
				e.Payload, _ = netv2.RewriteXuid(e.Payload, e.Size, netv2.XuidPattern+uint64(peer.Owner))
				hist[n] = e
			}
			e2, owner := e, peer.Owner
			w.RelayNumbered(peer, key, n, func(sq int) netv2.Msg {
				return netv2.RelayObjectMessage(uint64(e2.Type), uint64(key.Router), uint64(key.ID), uint64(owner), uint64(e2.TID), uint64(sq), idBits, typeBits, e2.Size, e2.Payload)
			})
		}
		peer.Objs.Set(key, hist)
	}
}

// RelayNumbered is relay() for a message numbered by the server (its n-th of that object): build(seq) writes it.
func (w *World) RelayNumbered(sender *Peer, key ObjKey, n int, build func(seq int) netv2.Msg) {
	baked := netv2.Bake(build(n % 0xFF))
	alt := map[int]netv2.Baked{}
	due := w.Mono() + RelayFlushSecs
	skey := OwnerObj{sender.Owner, key.Router, key.ID}
	for _, q := range w.Audience(sender, key.Router) {
		b := baked
		if shift := q.SeqShift[skey]; shift != 0 {
			sq := replay.WireSeq(n, shift)
			if cached, ok := alt[sq]; ok {
				b = cached
			} else {
				b = netv2.Bake(build(sq))
				alt[sq] = b
			}
		}
		q.Outbox = append(q.Outbox, b)
		if due < q.Due {
			q.Due = due
		}
		w.Dirty.Set(q, struct{}{})
	}
}

// OwnershipGrant grants at the next accepted sequence, without overwriting pending local choices.
func (w *World) OwnershipGrant(peer *Peer, router, tid, oid, idBits int) netv2.Msg {
	if router == netv2.RouterLobby && tid == netv2.TypeGameOptions && !w.Roadtrip && os.Getenv("FH2MP_NO_LEADER") == "" {
		var other *Peer
		if w.Leader != peer && w.Leader != nil && w.Leader.Connected {
			other = w.Leader
		}
		if other != nil || w.RequestRaceBlocked(peer) {
			why := "an event is pending or running"
			if other != nil {
				why = fmt.Sprintf("owner %d is setting up an event", other.Owner)
			}
			w.Logf("%d GameOptions ownership refused: %s", peer.Addr.Port, why)
			return w.OwnershipTransfer(peer, router, oid, idBits)
		}
		old := w.GameOptionsOwner
		if old != nil && old != peer {
			w.ReleaseOptions(old, fmt.Sprintf("owner %d took the event options", peer.Owner))
		}
	}
	if router == netv2.RouterLobby && tid == netv2.TypeGameOptions {
		w.GameOptionsOwner = peer
		if !w.RequestRaceBlocked(peer) {
			w.SetLeader(peer, "asked for the event options")
		}
	}
	if router == netv2.RouterLobby && tid == netv2.TypeGameOptions && (w.GameOptions != nil || peer.GoptsReleased) && peer.GoptsSeq > 0 {
		peer.GoptsReleased = false
		seq := peer.GoptsSeq
		peer.GoptsSeq = (peer.GoptsSeq + 1) % 0xFF
		w.Logf("%d GameOptions ownership grant (seq %d, preserve local selections)", peer.Addr.Port, seq)
		return netv2.GrantOwnership(uint64(router), uint64(oid), idBits, uint64(seq), 0, nil)
	}
	if router == netv2.RouterLobby && tid == netv2.TypeGameOptions && peer.GoptsSeq == 0 {
		peer.GoptsSeq = 1
	}
	return netv2.GrantOwnership(uint64(router), uint64(oid), idBits, 0, 0, nil)
}

func pmod(a, m int) int {
	r := a % m
	if r < 0 {
		r += m
	}
	return r
}

// OwnershipTransfer is message 12 for a client, numbered like the other server messages of its GameOptions object.
func (w *World) OwnershipTransfer(peer *Peer, router, oid, idBits int) netv2.Msg {
	if peer.GoptsOwnSeq != nil && pmod(peer.GoptsSeq-*peer.GoptsOwnSeq-1, 0xFF) >= 0x80 {
		peer.GoptsSeq = (*peer.GoptsOwnSeq + 1) % 0xFF
	}
	seq := peer.GoptsSeq
	peer.GoptsSeq = (peer.GoptsSeq + 1) % 0xFF
	peer.GoptsReleased = true
	return netv2.TransferOwnership(uint64(router), uint64(oid), idBits, uint64(seq))
}

// ReleaseOptions takes the GameOptions ownership back from a client.
func (w *World) ReleaseOptions(peer *Peer, why string) {
	if peer == nil || w.Roadtrip || os.Getenv("FH2MP_NO_LEADER") != "" {
		return
	}
	if w.GameOptionsOwner == peer {
		w.GameOptionsOwner = nil
	}
	if !(peer.Connected && peer.Entered) {
		return
	}
	w.Send(peer, false, w.OwnershipTransfer(peer, netv2.RouterLobby, GameOptionsTransferID, netv2.LobbyIDBits))
	w.Logf("%d GameOptions ownership taken back (%s)", peer.Addr.Port, why)
}

// SendGameOptions forwards the leader's GameOptions to a client under the transfer-object key.
func (w *World) SendGameOptions(q *Peer, options *gameopts.Options) {
	if w.GameOptions == nil {
		return
	}
	opt := w.GameOptions
	if options != nil {
		opt = options
	}
	size, payload := opt.Size, opt.Payload
	if !w.Roadtrip {
		s, p, err := gameopts.Merge(q.GoptsView, size, payload)
		if err != nil {
			// Python lets the ValueError propagate out of the handler; the datagram handler logs it.
			panic(err)
		}
		q.GoptsView = &gameopts.Options{Size: s, Payload: p}
	}
	if q.GoptsOwnSeq != nil && pmod(q.GoptsSeq-*q.GoptsOwnSeq-1, 0xFF) >= 0x80 {
		q.GoptsSeq = (*q.GoptsOwnSeq + 1) % 0xFF
	}
	msg := netv2.TransferUpdateMessage(netv2.RouterLobby, GameOptionsTransferID, netv2.LobbyIDBits, uint64(q.GoptsSeq), size, payload)
	q.GoptsSeq = (q.GoptsSeq + 1) % 0xFF
	w.Send(q, false, msg)
	w.Logf("%d <- GameOptions forwarded as transfer-object update (%d bits, seq %d)", q.Addr.Port, size, pmod(q.GoptsSeq-1, 0xFF))
}

// ReplayTo announces everything the other clients already created to a late joiner.
func (w *World) ReplayTo(newcomer *Peer, routers map[int]bool, skip map[int]bool) {
	all := append(w.Peers.Values(), w.Bots...)
	for _, q := range all {
		if q == newcomer || !q.Connected || !sameWorld(q.World, newcomer.World) {
			continue
		}
		w.ReplayFrom(newcomer, q, routers, skip)
	}
}

// ReplayFrom announces what q created to newcomer.
func (w *World) ReplayFrom(newcomer, q *Peer, routers, skip map[int]bool) {
	var batch []netv2.Msg
	stored := 0
	for _, key := range q.Objs.Keys() {
		hist, _ := q.Objs.Get(key)
		if skip[key.Router] || (routers != nil && !routers[key.Router]) {
			continue
		}
		rb := netv2.RouterBits[key.Router]
		idBits, typeBits := rb[0], rb[1]
		entries := replay.Renumber(replay.Compact(key.Router, hist, netv2.MsgObjectUpdate))
		stored += len(hist)
		for _, e := range entries {
			e := e
			batch = append(batch, netv2.RelayObjectMessage(uint64(e.Type), uint64(key.Router), uint64(key.ID), uint64(q.Owner), uint64(e.TID), uint64(e.Seq),
				idBits, typeBits, e.Size, e.Payload))
		}
		next, ok := q.RelayNext[key]
		if !ok {
			next = len(hist)
		}
		shift := next - len(entries)
		sk := OwnerObj{q.Owner, key.Router, key.ID}
		if shift > 0 {
			newcomer.SeqShift[sk] = shift
		} else {
			delete(newcomer.SeqShift, sk)
		}
	}
	for i := 0; i < len(batch); i += 4 {
		j := i + 4
		if j > len(batch) {
			j = len(batch)
		}
		w.Send(newcomer, false, batch[i:j]...)
	}
	if len(batch) > 0 {
		extra := ""
		if stored != len(batch) {
			extra = fmt.Sprintf(", %d stored", stored)
		}
		w.Logf("%d <- replayed %d object messages of owner %d (%d)%s", newcomer.Addr.Port, len(batch), q.Owner, q.Addr.Port, extra)
	}
}

// EarlyIntroduction returns point when FH2MP_INTRODUCE_AT asks for it here and this connected client is in Free Roam, else "".
func (w *World) EarlyIntroduction(peer *Peer, point string) string {
	freeRoam := !w.Roadtrip && peer.LobbyType != LobbyTypeCarMeet
	if point == w.IntroduceAt && peer.Connected && freeRoam {
		return point
	}
	return ""
}

// Introduce shows a joiner the players already here, once.
func (w *World) Introduce(peer *Peer, point string) {
	if peer.Introduced {
		if point == "finalise" && peer.CarsHeld {
			peer.CarsHeld = false
			w.Logf("%d given the cars %.0f ms after connect", peer.Addr.Port, (w.Mono()-peer.ConnectedAt)*1000)
			w.ReplayTo(peer, EntryOnlyRouters, nil)
		}
		return
	}
	peer.Introduced = true
	peer.CarsHeld = point != "finalise"
	extra := ""
	if peer.CarsHeld {
		extra = ", cars at its FinaliseSessionEntry"
	}
	w.Logf("%d introduced (%s) %.0f ms after connect%s", peer.Addr.Port, point, (w.Mono()-peer.ConnectedAt)*1000, extra)
	if peer.CarsHeld {
		w.ReplayTo(peer, nil, EntryOnlyRouters)
	} else {
		w.ReplayTo(peer, nil, nil)
	}
}

// SetSessionHostBool sets a SessionHost bool for every connected Free Roam / Road Trip client.
func (w *World) SetSessionHostBool(varID, value int, why string) {
	for _, q := range w.Peers.Values() {
		if !q.Connected || q.LobbyType == LobbyTypeCarMeet {
			continue
		}
		vid, v := varID, value
		w.Send(q, false, netv2.SessionHostUpdate(uint64(q.HostSeq), func(b *bits.Writer) {
			b.Bits(uint64(vid), netv2.SessionHostVarBits)
			b.Bits(uint64(v), 1)
		}))
		q.HostSeq = (q.HostSeq + 1) % 0xFF
	}
	w.Logf("SessionHost var %d = %d sent to all (%s)", varID, value, why)
}

// SetRaceClock sets CountdownType + RaceStartTime in one update.
func (w *World) SetRaceClock(startMs int64, why string) {
	if w.Roadtrip {
		return
	}
	kind := int64(0)
	if startMs != 0 {
		kind = SHCountdownStartRace
	}
	if [2]int64{kind, startMs} == w.raceClock {
		return
	}
	w.raceClock = [2]int64{kind, startMs}
	w.raceProgress.done, w.raceProgress.frac = 0, 0
	w.SendSessionHostVars([]shVar{{SHCountdownType, SHCountdownTypeBits, uint64(kind)}, {SHRaceStartTime, 32, uint64(startMs) & 0xFFFFFFFF},
		{SHNumLapsCompleted, 8, 0}, {SHFurthestProgress, 32, 0}})
	w.Logf("SessionHost CountdownType = %d, RaceStartTime = %d, NumLapsCompleted = 0, FurthestProgress = 0 sent to all (%s)", kind, startMs, why)
}

type shVar struct {
	ID, Width int
	Value     uint64
}

// SendSessionHostVars sends one SessionHost update of (var id, width, value) triples to every connected Free Roam / Road Trip client.
func (w *World) SendSessionHostVars(values []shVar) {
	for _, q := range w.Peers.Values() {
		if !q.Connected || q.LobbyType == LobbyTypeCarMeet {
			continue
		}
		w.Send(q, false, netv2.SessionHostUpdate(uint64(q.HostSeq), func(b *bits.Writer) {
			for _, v := range values {
				b.Bits(uint64(v.ID), netv2.SessionHostVarBits)
				b.Bits(v.Value, v.Width)
			}
		}))
		q.HostSeq = (q.HostSeq + 1) % 0xFF
	}
}

// NoteProgress handles the MsgInProgressScoreUpdate of a racer.
func (w *World) NoteProgress(peer *Peer, raw uint64) {
	ev := w.Event
	if w.Roadtrip || ev == nil || !ev.Racing || ev.Tag != nil || ev.NumLaps == nil || *ev.NumLaps == 0 || !inList(ev.People, peer) {
		return
	}
	progress := math.Abs(float64(math.Float32frombits(uint32(raw))))
	if math.IsNaN(progress) || math.IsInf(progress, 0) || !(0.0 <= progress && progress <= float64(*ev.NumLaps)+1.0) {
		w.LogEvery([2]any{"progress-range", peer.Owner}, 30.0, fmt.Sprintf("%d in-progress score %s is not a lap progress of a %d-lap event; ignored", peer.Addr.Port, floatRepr(progress), *ev.NumLaps), diag.Warn)
		return
	}
	if ev.Progress == nil {
		ev.Progress = map[*Peer]float64{}
	}
	if progress > ev.Progress[peer] {
		ev.Progress[peer] = progress
	}
	w.PublishProgress()
}

// PublishProgress sends NumLapsCompleted / FurthestProgress of the leading racer.
func (w *World) PublishProgress() {
	ev := w.Event
	if w.Roadtrip || ev == nil || !ev.Racing || ev.NumLaps == nil || *ev.NumLaps == 0 {
		return
	}
	laps := int(*ev.NumLaps)
	maxLaps := 0
	if ev.Laps != nil {
		for _, v := range ev.Laps.Values() {
			if v > maxLaps {
				maxLaps = v
			}
		}
	}
	done := minInt(minInt(maxLaps, laps), 255)
	best := 0.0
	for _, v := range ev.Progress {
		if v > best {
			best = v
		}
	}
	frac := math.Min(1.0, best/float64(laps))
	frac = math.Max(frac, float64(done)/float64(laps))
	lastDone, lastFrac := w.raceProgress.done, w.raceProgress.frac
	now := w.Mono()
	if done == lastDone && (math.Abs(frac-lastFrac) < 0.005 || now-ev.ProgressAt < ProgressSendSeconds) {
		return
	}
	ev.ProgressAt = now
	w.raceProgress.done, w.raceProgress.frac = done, frac
	w.SendSessionHostVars([]shVar{{SHNumLapsCompleted, 8, uint64(done)}, {SHFurthestProgress, 32, uint64(math.Float32bits(float32(frac)))}})
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// SetConsoleBool is a server-side write of a Console var: to the owner (server update) and relayed to the others.
func (w *World) SetConsoleBool(peer *Peer, varID, value int, why string) {
	if peer.ConsoleOid == nil {
		return
	}
	vars := func(b *bits.Writer) {
		b.Bits(uint64(varID), netv2.ConsoleVarIDBits)
		b.Bits(uint64(value), 1)
	}
	w.Send(peer, false, netv2.ServerUpdate(3, uint64(*peer.ConsoleOid), netv2.RouterBits[3][0], uint64(peer.ConsoleSeq), vars))
	peer.ConsoleSeq = (peer.ConsoleSeq + 1) % 0xFF
	var vb bits.Writer
	vars(&vb)
	w.RememberAndRelay(peer, netv2.MsgObjectUpdate, 3, *peer.ConsoleOid, nil, 0, vb.Pos, vb.Value())
	name := ConsoleBoolVarNames[varID]
	if name == "" {
		name = fmt.Sprint(varID)
	}
	w.Logf("%d Console.%s = %d (%s)", peer.Addr.Port, name, value, why)
}

// SetLeader sets PlayerData.IsLeader on the one player who is setting up or running the online event (nil = nobody).
func (w *World) SetLeader(peer *Peer, why string) {
	if w.Roadtrip || os.Getenv("FH2MP_NO_LEADER") != "" {
		return
	}
	old := w.Leader
	if old == peer {
		return
	}
	if peer != nil && !(peer.Connected && peer.Entered) {
		w.Debugf("leader", "event leader: owner %d not set (%s): connected=%s entered=%s", peer.Owner, why, pyBool(peer.Connected), pyBool(peer.Entered))
		return
	}
	w.Leader = peer
	w.LeaderSince = w.Mono()
	if old != nil && old.Connected && old.Entered {
		w.SendLeaderFlag(old, 0)
	}
	if peer != nil {
		w.SendLeaderFlag(peer, 1)
	}
	who := "nobody"
	if peer != nil {
		who = fmt.Sprintf("owner %d", peer.Owner)
	}
	tail := ""
	if old != nil && old != peer {
		tail = fmt.Sprintf("; owner %d dropped", old.Owner)
	}
	w.Logf("event leader: %s (%s)%s", who, why, tail)
}

func pyBool(b bool) string {
	if b {
		return "True"
	}
	return "False"
}

// SendLeaderFlag writes IsLeader of a player.
func (w *World) SendLeaderFlag(peer *Peer, flag int) {
	w.SendPlayerVar(peer, PlayerVarIsLeader, 1, uint64(flag), true)
}

// HideVIP clears the VIP bit of PlayerFlags in a client's player-object write.
func (w *World) HideVIP(peer *Peer, oid, size int, payload bits.Int) (int, bits.Int) {
	fields, err := playersetupFields(size, payload)
	if err != nil {
		w.LogEvery([3]any{"setup-parse", "vip", peer.Owner}, 60.0, fmt.Sprintf("player.setup_unreadable %s: relayed as it came, a VIP bit in it stays set",
			diag.KV(diag.KVP{K: "what", V: "flags"}, diag.KVP{K: "owner", V: peer.Owner}, diag.KVP{K: "xuid", V: fmt.Sprintf("%#x", peer.Xuid)},
				diag.KVP{K: "bits", V: size}, diag.KVP{K: "error", V: err.Error()})), diag.Warn)
		return size, payload
	}
	f, ok := fields[PlayerVarFlags]
	if !ok || f.Value.Uint64()&PlayerFlagVIP == 0 {
		return size, payload
	}
	cleared := f.Value.Uint64() &^ PlayerFlagVIP
	payload = replaceBits(payload, f.Offset, f.Width, bits.FromUint64(cleared))
	if peer.Entered {
		w.SendPlayerVar(peer, PlayerVarFlags, f.Width, cleared, false)
	} else {
		peer.VipHidden = &VipHidden{f.Width, int(cleared)}
	}
	return size, payload
}

// SendPlayerVar writes one variable of a player's object: to the owner and (unless relay is false) relayed to the others.
func (w *World) SendPlayerVar(peer *Peer, varID, width int, value uint64, relay bool) {
	vars := func(b *bits.Writer) {
		b.Bits(uint64(varID), 8)
		b.Bits(value, width)
	}
	w.Send(peer, false, netv2.ServerUpdate(1, 0, netv2.RouterBits[1][0], uint64(peer.PlayerSeq), vars))
	peer.PlayerSeq = (peer.PlayerSeq + 1) % 0xFF
	if relay {
		var vb bits.Writer
		vars(&vb)
		w.RememberAndRelay(peer, netv2.MsgObjectUpdate, 1, 0, nil, 0, vb.Pos, vb.Value())
	}
}

// AssignPrivateUniverse gives the player an OnlinePrivateN no other player in this world has.
func (w *World) AssignPrivateUniverse(peer *Peer) {
	if os.Getenv("FH2MP_NO_PRIVATE_UNIVERSE") != "" {
		return
	}
	used := map[int]bool{}
	for _, q := range w.Peers.Values() {
		if q != peer && q.Connected && q.PrivateUniverse != nil {
			used[*q.PrivateUniverse] = true
		}
	}
	free := -1
	for u := PrivateUniverseFirst; u < PrivateUniverseFirst+PrivateUniverseCount; u++ {
		if !used[u] {
			free = u
			break
		}
	}
	if free < 0 {
		w.Warnf("%d no private universe left (%d are in use): its car is not kept apart from an event's", peer.Addr.Port, PrivateUniverseCount)
		return
	}
	peer.PrivateUniverse = &free
	vars := func(b *bits.Writer) {
		b.Bits(PlayerVarPrivateUniverse, 8)
		b.Bits(uint64(free), PrivateUniverseBits)
	}
	w.Send(peer, false, netv2.ServerUpdate(1, 0, netv2.RouterBits[1][0], uint64(peer.PlayerSeq), vars))
	peer.PlayerSeq = (peer.PlayerSeq + 1) % 0xFF
	var vb bits.Writer
	vars(&vb)
	w.RememberAndRelay(peer, netv2.MsgObjectUpdate, 1, 0, nil, 0, vb.Pos, vb.Value())
	w.Logf("%d LobbyPlayer.PrivateUniverseId = %d (OnlinePrivate%d)", peer.Addr.Port, free, free-1)
}

// SetPlayerTeam sets PlayerChoices.TeamId of a player.
func (w *World) SetPlayerTeam(peer *Peer, team int) {
	peer.TeamID = team & 1
	vars := func(b *bits.Writer) {
		b.Bits(PlayerVarTeamID, 8)
		b.Bits(uint64(team&1), 1)
	}
	w.Send(peer, false, netv2.ServerUpdate(1, 0, netv2.RouterBits[1][0], uint64(peer.PlayerSeq), vars))
	peer.PlayerSeq = (peer.PlayerSeq + 1) % 0xFF
	var vb bits.Writer
	vars(&vb)
	w.RememberAndRelay(peer, netv2.MsgObjectUpdate, 1, 0, nil, 0, vb.Pos, vb.Value())
}
