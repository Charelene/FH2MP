package game

import (
	"fmt"
	"math"
	"os"
	"sort"
	"strings"

	"fh2mp/internal/bits"
	"fh2mp/internal/capacity"
	"fh2mp/internal/netv2"
	"fh2mp/internal/replay"
)

// Bot players for capacity and HUD tests on the dev server (= server/bots.py). A bot is a Peer that is NOT in World.Peers: it lives in World.Bots.
const (
	BotXuidBase  = 0x9F00000000000
	botRingStart = 30.0
	botRingStep  = 12.0
	botRingSize  = 8
	maxBots      = 75
	positionBits = 96
)

func botsEnabled() bool { return os.Getenv("FH2MP_BOTS") == "1" && os.Getenv("FH2MP_ENV") != "prod" }

func ringOffset(k int) (float64, float64) {
	radius := botRingStart + botRingStep*float64(k/botRingSize)
	angle := 2 * math.Pi * float64(k%botRingSize) / botRingSize
	return radius * math.Cos(angle), radius * math.Sin(angle)
}

// shiftedBody moves the position of a CarPredictionFull body by (dx, 0, dz); every other bit is the source's.
func shiftedBody(body bits.Int, dx, dz float64) bits.Int {
	p := carPosition(body)
	if !allFinite(p) {
		return body
	}
	mask := bits.FromUint64(1).Lsh(positionBits)
	mask = subOne(mask)
	var moved bits.Int
	vals := [3]float32{float32(p[0] + dx), float32(p[1]), float32(p[2] + dz)}
	for i, v := range vals {
		moved = moved.Or(bits.FromUint64(uint64(math.Float32bits(v))).Lsh(32 * i))
	}
	return andNot(body, mask).Or(moved)
}

func (w *World) botRealPeers() []*Peer {
	var out []*Peer
	for _, p := range w.Peers.Values() {
		if p.Connected {
			out = append(out, p)
		}
	}
	return out
}

func (w *World) botSources() []*Peer {
	var out []*Peer
	for _, p := range w.Peers.Values() {
		if !(p.Connected && p.Entered && p.Xuid != 0 && p.CarID != nil && p.CarPrediction != nil) {
			continue
		}
		has1, has2 := false, false
		for _, k := range p.Objs.Keys() {
			has1 = has1 || k.Router == 1
			has2 = has2 || k.Router == 2
		}
		if has1 && has2 {
			out = append(out, p)
		}
	}
	return out
}

func (w *World) botCloneObjects(src, bot *Peer) {
	for _, key := range src.Objs.Keys() {
		if key.Router != 1 && key.Router != 2 && key.Router != 3 {
			continue
		}
		hist, _ := src.Objs.Get(key)
		entries := make([]replay.Entry, len(hist))
		for i, e := range hist {
			e.Payload = replaceUserID(e.Payload, e.Size, src.Xuid, bot.Xuid)
			entries[i] = e
		}
		bot.Objs.Set(key, entries)
		bot.RelayNext[key] = len(entries)
	}
}

func replaceUserID(payload bits.Int, size int, old, nw uint64) bits.Int {
	for off := 0; off+64 <= size; {
		if payload.Field(off, 64) == old {
			mask := bits.FromUint64(^uint64(0)).Lsh(off)
			payload = andNot(payload, mask).Or(bits.FromUint64(nw).Lsh(off))
			off += 64
		} else {
			off++
		}
	}
	return payload
}

// BotAdd adds up to n bots to this world, never more than limit players in all.
func (w *World) BotAdd(n, limit int) (int, string) {
	srcs := w.botSources()
	if len(srcs) == 0 {
		return 0, "no player here has entered with a car yet"
	}
	room := minInt(minInt(n, maxBots-len(w.Bots)), limit-len(w.botRealPeers())-len(w.Bots))
	added, why := 0, ""
	for i := 0; i < maxInt(room, 0); i++ {
		owner := w.FreeOwner()
		if owner == 0 {
			why = "no relay owner id is free"
			break
		}
		k := len(w.Bots)
		src := srcs[k%len(srcs)]
		w.BotSerial++
		bot := NewPeer(Addr{"bot", w.BotSerial}, w.Mono())
		bot.Xuid, bot.Owner = BotXuidBase+uint64(w.BotSerial), owner
		bot.World, bot.LobbyType = src.World, src.LobbyType
		bot.Connected, bot.Entered, bot.Introduced = true, true, true
		bot.CarID, bot.ModelID = src.CarID, src.ModelID
		bot.ConnectedAt, bot.LastRx = w.Mono(), w.Mono()
		dx, dz := ringOffset(k)
		bot.BotSource, bot.BotOffset = src, [2]float64{dx, dz}
		w.botCloneObjects(src, bot)
		w.Bots = append(w.Bots, bot)
		for _, q := range w.Others(bot) {
			w.ReplayFrom(q, bot, nil, nil)
		}
		w.botFollowOne(src, src.CarPrediction, bot)
		added++
		w.Logf("bots: bot %#x added as owner %d, following owner %d, %.0f/%.0f m away, %d objects; %d players here", bot.Xuid, owner, src.Owner, dx, dz, bot.Objs.Len(), len(w.botRealPeers())+len(w.Bots))
	}
	if added < n && why == "" {
		if limit-len(w.botRealPeers())-len(w.Bots) <= 0 {
			why = fmt.Sprintf("the world holds %d players", limit)
		} else {
			why = fmt.Sprintf("the limit is %d bots", maxBots)
		}
	}
	return added, why
}

// BotRemove deletes the bot's objects for everybody; its owner id rests like a leaver's.
func (w *World) BotRemove(bot *Peer, why string) {
	for i, b := range w.Bots {
		if b == bot {
			w.Bots = append(w.Bots[:i:i], w.Bots[i+1:]...)
			break
		}
	}
	for _, key := range bot.Objs.Keys() {
		rb := netv2.RouterBits[key.Router]
		idBits := rb[0]
		n := bot.RelayNext[key]
		k, owner := key, bot.Owner
		w.RelayNumbered(bot, key, n, func(sq int) netv2.Msg {
			return netv2.RelayDelete(uint64(k.Router), uint64(k.ID), uint64(owner), idBits, uint64(sq))
		})
	}
	w.OwnerQuarantine[bot.Owner] = w.Mono() + OwnerQuarantineS
	bot.Connected = false
	w.Logf("bots: bot %#x (owner %d) %s; %d bot(s) left", bot.Xuid, bot.Owner, why, len(w.Bots))
}

// BotClear removes every bot; returns how many.
func (w *World) BotClear() int {
	n := len(w.Bots)
	for _, b := range append([]*Peer(nil), w.Bots...) {
		w.BotRemove(b, "cleared")
	}
	return n
}

func (w *World) botFollow(source *Peer, pred *Prediction) { w.botFollowOne(source, pred, nil) }

func (w *World) botFollowOne(source *Peer, pred *Prediction, only *Peer) {
	if len(w.Bots) == 0 || pred == nil {
		return
	}
	for _, bot := range w.Bots {
		if bot.BotSource == source && (only == nil || bot == only) {
			w.RelayPrediction(bot, false, &Prediction{pred.ID, shiftedBody(pred.Body, bot.BotOffset[0], bot.BotOffset[1]), pred.Size})
		}
	}
}

func (w *World) botMaintain() {
	if len(w.Bots) == 0 {
		return
	}
	now := w.Mono()
	if now < w.BotsNext {
		return
	}
	w.BotsNext = now + 1.0
	real := w.botSources()
	if len(w.botRealPeers()) == 0 {
		w.BotClear()
		return
	}
	for _, bot := range append([]*Peer(nil), w.Bots...) {
		src := bot.BotSource
		if cur, ok := w.Peers.Get(src.Addr); src.Connected && src.CarPrediction != nil && ok && cur == src {
			continue
		}
		if len(real) == 0 {
			w.BotRemove(bot, "lost its source")
			continue
		}
		bot.BotSource = real[bot.Owner%len(real)]
		w.Logf("bots: bot %#x now follows owner %d", bot.Xuid, bot.BotSource.Owner)
	}
}

func (w *World) botStatus() string {
	return fmt.Sprintf("%d bot(s), %d real player(s)", len(w.Bots), len(w.botRealPeers()))
}

// botsCommand is the control port's `bots` command.
func (h *Hub) botsCommand(parts []string) string {
	if os.Getenv("FH2MP_ENV") == "prod" {
		return "refused: bots never run on production"
	}
	if os.Getenv("FH2MP_BOTS") != "1" {
		return "off: set FH2MP_BOTS=1 on this server to use bots"
	}
	verb := "status"
	if len(parts) > 1 {
		verb = parts[1]
	}
	arg := ""
	if len(parts) > 2 {
		arg = parts[2]
	}
	lobby := ""
	if len(parts) > 3 {
		lobby = parts[3]
	} else if (verb == "clear" || verb == "status") && len(parts) > 2 {
		lobby = parts[2]
	}
	type kw struct {
		key WorldKey
		w   *World
	}
	var worlds []kw
	for _, key := range h.Worlds.Keys() {
		w, _ := h.Worlds.Get(key)
		if key.Type == 2 && (lobby == "" || key.Name == lobby) {
			worlds = append(worlds, kw{key, w})
		}
	}
	nameOf := func(k WorldKey) string {
		if k.Name == "" {
			return "default"
		}
		return k.Name
	}
	switch verb {
	case "clear", "status":
		if verb == "clear" {
			total := 0
			for _, e := range worlds {
				total += e.w.BotClear()
			}
			return fmt.Sprintf("cleared %d bot(s)", total)
		}
		if len(worlds) == 0 {
			return "no Free Roam world"
		}
		rows := make([]string, len(worlds))
		for i, e := range worlds {
			rows[i] = fmt.Sprintf("%s: %s", nameOf(e.key), e.w.botStatus())
		}
		return strings.Join(rows, "; ")
	case "add":
		var n int
		if _, err := fmt.Sscanf(arg, "%d", &n); err != nil || arg == "" {
			return "usage: bots add <n> [lobby] | bots clear [lobby] | bots status"
		}
		var withPlayers []kw
		for _, e := range worlds {
			if len(e.w.botRealPeers()) > 0 {
				withPlayers = append(withPlayers, e)
			}
		}
		if len(withPlayers) == 0 {
			s := "no Free Roam world with a player in it"
			if lobby != "" {
				s += " named " + lobby
			}
			return s
		}
		sort.SliceStable(withPlayers, func(i, j int) bool {
			return len(withPlayers[i].w.botRealPeers()) > len(withPlayers[j].w.botRealPeers())
		})
		best := withPlayers[0]
		added, why := best.w.BotAdd(n, capacity.ForType(2))
		s := fmt.Sprintf("added %d of %d in %s", added, n, nameOf(best.key))
		if why != "" {
			s += " (" + why + ")"
		}
		return s + "; " + best.w.botStatus()
	}
	return "usage: bots add <n> [lobby] | bots clear [lobby] | bots status"
}
