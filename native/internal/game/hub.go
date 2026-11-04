package game

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"fh2mp/internal/bits"
	"fh2mp/internal/capacity"
	"fh2mp/internal/catalog"
	"fh2mp/internal/diag"
	"fh2mp/internal/eventlog"
	"fh2mp/internal/netv2"
	"fh2mp/internal/omap"
	"fh2mp/internal/pyfmt"
	"fh2mp/internal/pyjson"
)

type authEnt struct {
	ip    string
	until float64
}

type flagEnt struct {
	until  float64
	reason string
}

type boundEnt struct {
	xuid  uint64
	until float64
	key   string
}

type nonceKey struct {
	xuid  uint64
	nonce string
}

// UDP auth modes (FH2MP_UDP_AUTH).
var UDPAuthModes = []string{"off", "required", "ticket"}

// HubConfig builds a Hub.
type HubConfig struct {
	Port     int
	LogPath  string
	Roadtrip bool
	Carmeet  bool
	Clock    Clock
	Rng      Rand
	Sock     PacketConn
	Out      *LogOut
	Sinks    *Sinks
	Catalog  *catalog.Catalog
	SyncPath string
	CtlSend  func(data []byte, to Addr)
	DataDir  string
}

// Hub is one UDP port, many worlds (= fh2mp_server.Hub). It is driven from one goroutine: Handle, ControlText, Step.
type Hub struct {
	cfg            HubConfig
	Clk            Clock
	Rng            Rand
	Sock           PacketConn
	Out            *LogOut
	Events         *eventlog.Log
	Sinks          *Sinks
	catalog        *catalog.Catalog
	syncer         *catalog.Syncer
	CtlSend        func(data []byte, to Addr)
	DefaultType    int
	Assign         map[uint64]string
	Runs           map[uint64]string
	Authed         map[uint64]authEnt
	Flagged        *omap.Map[uint64, flagEnt]
	FlagsError     string
	FlagFile       string
	Revoked        map[uint64]float64
	UDPAuth        string
	Keys           map[uint64]map[string]float64
	TicketEpochs   map[Addr][]byte
	Bound          map[Addr]boundEnt
	Nonces         *omap.Map[nonceKey, float64]
	AuthLog        map[any]float64
	Worlds         *omap.Map[WorldKey, *World]
	Route          *omap.Map[Addr, *World]
	EmptySince     map[WorldKey]float64
	Idle           *World
	LightingRoutes map[uint64]Addr
	HonkRoutes     map[uint64]Addr
	probeSeen      map[Addr]probeEnt
	autoWorlds     int
	lastSweep      float64
	nextTick       float64
	nxt            float64
	Tunnel         TunnelHost
}

// TunnelHost is the TCP carrier as the hub sees it.
type TunnelHost interface {
	HasConn(addr Addr) bool
}

// NewHub opens the hub: the idle world, the flag file, the start-up checks.
func NewHub(cfg HubConfig) (*Hub, error) {
	if err := ValidatePolicies(); err != nil {
		return nil, err
	}
	h := &Hub{cfg: cfg, Clk: cfg.Clock, Rng: cfg.Rng, Sock: cfg.Sock, Out: cfg.Out, Sinks: cfg.Sinks, catalog: cfg.Catalog, CtlSend: cfg.CtlSend,
		Assign: map[uint64]string{}, Runs: map[uint64]string{}, Authed: map[uint64]authEnt{}, Flagged: omap.New[uint64, flagEnt](), Revoked: map[uint64]float64{},
		Keys: map[uint64]map[string]float64{}, TicketEpochs: map[Addr][]byte{}, Bound: map[Addr]boundEnt{}, Nonces: omap.New[nonceKey, float64](),
		AuthLog: map[any]float64{}, Worlds: omap.New[WorldKey, *World](), Route: omap.New[Addr, *World](), EmptySince: map[WorldKey]float64{},
		LightingRoutes: map[uint64]Addr{}, HonkRoutes: map[uint64]Addr{}, probeSeen: map[Addr]probeEnt{}, nxt: math.Inf(1)}
	h.DefaultType = 2
	if cfg.Carmeet {
		h.DefaultType = LobbyTypeCarMeet
	} else if cfg.Roadtrip {
		h.DefaultType = LobbyTypeRoadTrip
	}
	if cfg.LogPath != "" {
		dir := filepath.Dir(cfg.LogPath)
		h.Events = eventlog.Open(filepath.Join(dir, "events.jsonl"))
		h.FlagFile = filepath.Join(dir, "cheaters.json")
	}
	h.loadFlags()
	h.UDPAuth = envStr("FH2MP_UDP_AUTH", "off")
	okMode := false
	for _, m := range UDPAuthModes {
		okMode = okMode || m == h.UDPAuth
	}
	if !okMode {
		return nil, fmt.Errorf("FH2MP_UDP_AUTH=%s: expected one of %s", pyReprStr(h.UDPAuth), strings.Join(UDPAuthModes, ", "))
	}
	h.syncer = catalog.NewSyncer(cfg.SyncPath)
	idle, err := NewWorld("[idle] ", cfg.Out, cfg.Clock, cfg.Rng, cfg.Sock, h.Events, cfg.Sinks, h.Catalog, false, false, cfg.Port)
	if err != nil {
		return nil, err
	}
	h.Idle = idle
	idle.IsIdle = true
	diag.SetSink(func(level int, sub, msg string) { idle.logAt(level, sub, msg) })
	idle.Logf("hub listening on udp/%d, control udp/%s", cfg.Port, "-")
	if h.UDPAuth == "off" {
		idle.Log("WARNING: FH2MP_UDP_AUTH=off: any OpenConnection is taken at its word (set ticket, or required, on a public server)")
	}
	for _, p := range StartupProblems {
		idle.Error("startup.data_missing " + p)
	}
	if h.FlagsError != "" {
		idle.Warnf("cheater pool file %s not read (%s): the pool starts empty", h.FlagFile, h.FlagsError)
	}
	return h, nil
}

// Catalog is the current car policy and part data.
func (h *Hub) Catalog() *catalog.Catalog { return h.catalog }

// PollCatalog re-reads the synced car database when it changed.
func (h *Hub) PollCatalog() {
	if n := h.syncer.Poll(h.Clk.Mono(), h.catalog, true, h.Idle.Log); n != nil {
		h.catalog = n
	}
}

// Log writes an INFO line of the hub.
func (h *Hub) Log(msg string) { h.Idle.Log(msg) }

// Warn writes a WARN line of the hub.
func (h *Hub) Warn(msg string) { h.Idle.Warn(msg) }

// WorldFor finds or makes the world of a lobby.
func (h *Hub) WorldFor(lobbyType int, name string) *World {
	key := WorldKey{lobbyType, name}
	if w, ok := h.Worlds.Get(key); ok {
		return w
	}
	kind := map[int]string{1: "roadtrip", 2: "freeroam", 3: "carmeet"}[lobbyType]
	if kind == "" {
		kind = fmt.Sprint(lobbyType)
	}
	short := name
	if len(short) > 8 {
		short = short[:8]
	}
	if short == "" {
		short = "default"
	}
	tag := fmt.Sprintf("[%s:%s] ", kind, short)
	w, err := NewWorld(tag, h.Out, h.Clk, h.Rng, h.Sock, h.Events, h.Sinks, h.Catalog, lobbyType == LobbyTypeRoadTrip, lobbyType == LobbyTypeCarMeet, 0)
	if err != nil {
		panic(err)
	}
	k := key
	w.HubKey = &k
	w.Hub = h
	if w.Roadtrip {
		gamedb := envStr("FH2MP_GAMEDB", "")
		if gamedb == "" {
			gamedb = roadTripDBDefault
		}
		if _, err := os.Stat(gamedb); err == nil {
			w.RtGamedb = gamedb
			if cand, err := PickRoadTripCandidates(h.Rng, gamedb, 1); err == nil {
				w.RtCandidates = cand
				w.Logf("road trip candidates: locations %s events %s classes %s", locsRepr(cand), eventsRepr(cand), intList(cand.Classes[:]))
			}
		} else {
			w.Logf("road trip: no game database (%s); the vote will be empty. Build %s with tools/gamedata/make_roadtrip_db.py or point FH2MP_GAMEDB at gamedbRC.slt", gamedb, roadTripDBDefault)
		}
	}
	h.Worlds.Set(key, w)
	h.Log(fmt.Sprintf("new world (%d, %s)", lobbyType, pyReprStr(name)))
	return w
}

// ---- control protocol ----

func (h *Hub) ctlIgnored(parts []string, what string) {
	cmd := "(empty)"
	key := ""
	if len(parts) > 0 {
		cmd, key = parts[0], parts[0]
	}
	cut := what
	if len(cut) > 40 {
		cut = cut[:40]
	}
	h.Idle.LogEvery([3]any{"ctl", key, cut}, 30.0, fmt.Sprintf("control: %s: %s", cmd, what), diag.Warn)
}

func (h *Hub) ctlReply(to *Addr, cmd string, data []byte) {
	if to == nil || h.CtlSend == nil {
		return
	}
	h.CtlSend(data, *to)
}

func parseHex(s string) (uint64, error) {
	s = strings.TrimPrefix(strings.TrimPrefix(s, "0x"), "0X")
	return strconv.ParseUint(s, 16, 64)
}

// ControlText handles one control message: assign, auth, logout, revoke, allow, unflag, run, tools, pop, lobbies, positions, bots.
func (h *Hub) ControlText(text string, replyTo *Addr) {
	parts := strings.Fields(text)
	n := len(parts)
	switch {
	case n == 1 && parts[0] == "pop":
		h.ctlReply(replyTo, "pop", []byte(h.populationsText()))
	case n == 1 && parts[0] == "lobbies":
		h.ctlReply(replyTo, "lobbies", []byte(jsonDumps(h.lobbyStatus())))
	case n > 0 && parts[0] == "bots":
		h.ctlReply(replyTo, "bots", []byte(h.botsCommand(parts)))
	case n == 1 && parts[0] == "positions":
		h.ctlReply(replyTo, "positions", []byte(jsonDumps(h.mapStatus())))
	case n == 3 && parts[0] == "tools":
		tools := map[string]bool{}
		for _, t := range strings.Split(parts[2], ",") {
			tools[t] = true
		}
		valid := len(tools) > 0
		for t := range tools {
			if t != "redline" && t != "nitrosystem" {
				valid = false
			}
		}
		if !valid || CheaterPoolHours <= 0 {
			why := "the cheater pool is off"
			if CheaterPoolHours > 0 {
				names := make([]string, 0, len(tools))
				for t := range tools {
					names = append(names, t)
				}
				sort.Strings(names)
				if len(names) > 4 {
					names = names[:4]
				}
				why = fmt.Sprintf("unknown tool name in %s", strListRepr(names))
			}
			h.ctlIgnored(parts, "tools message ignored: "+why)
			return
		}
		xuid, err := parseHex(parts[1])
		if err != nil {
			h.ctlIgnored(parts, fmt.Sprintf("tools message ignored: xuid not hex (%v)", err))
			return
		}
		if xuid == 0 {
			h.ctlIgnored(parts, fmt.Sprintf("tools message ignored: xuid %#x out of range", xuid))
			return
		}
		already := false
		for _, x := range h.FlaggedNow() {
			already = already || x == xuid
		}
		names := make([]string, 0, len(tools))
		for t := range tools {
			names = append(names, t)
		}
		sort.Strings(names)
		h.Flag(xuid, "restricted tools: "+strings.Join(names, ","))
		mixed := false
		if !already {
			for _, w := range h.Worlds.Values() {
				for _, q := range w.Peers.Values() {
					if q.Xuid == xuid && !h.worldPoolFits(w, xuid) {
						mixed = true
					}
				}
			}
		}
		if mixed {
			h.Revoke(xuid)
		}
		if replyTo != nil {
			if mixed {
				h.ctlReply(replyTo, "tools", []byte("rejoin"))
			} else {
				h.ctlReply(replyTo, "tools", []byte("ok"))
			}
		}
	case (n == 3 || n == 4) && parts[0] == "auth":
		xuid, err := parseHex(parts[1])
		if err != nil {
			h.ctlIgnored(parts, fmt.Sprintf("auth message unreadable (%v)", err))
			return
		}
		if _, rev := h.Revoked[xuid]; rev {
			h.ctlIgnored(parts, fmt.Sprintf("sign-in of %#x ignored: the account is revoked (kicked or banned) and stays refused until 'allow'", xuid))
			return
		}
		until := h.Clk.Mono() + AuthTTLSeconds
		h.Authed[xuid] = authEnt{parts[2], until}
		if n == 4 {
			key, err := hex.DecodeString(parts[3])
			if err != nil {
				h.ctlIgnored(parts, fmt.Sprintf("auth message unreadable (%v)", err))
				return
			}
			keys := h.Keys[xuid]
			if keys == nil {
				keys = map[string]float64{}
				h.Keys[xuid] = keys
			}
			if _, has := keys[string(key)]; len(keys) < 16 || has {
				keys[string(key)] = until
			} else {
				h.ctlIgnored(parts, fmt.Sprintf("ticket key of %#x not stored: the account already has %d live sessions (limit 16), tickets of this one are refused", xuid, len(keys)))
			}
		}
	case n == 3 && parts[0] == "logout":
		xuid, err := parseHex(parts[1])
		var key []byte
		if err == nil {
			key, err = hex.DecodeString(parts[2])
		}
		if err != nil {
			h.ctlIgnored(parts, fmt.Sprintf("logout message unreadable (%v)", err))
			return
		}
		h.Logout(xuid, key)
	case n == 2 && parts[0] == "revoke":
		xuid, err := parseHex(parts[1])
		if err != nil {
			h.ctlIgnored(parts, fmt.Sprintf("revoke message unreadable (%v)", err))
			return
		}
		h.Revoke(xuid)
	case n == 2 && parts[0] == "unflag":
		xuid, err := parseHex(parts[1])
		if err != nil {
			h.ctlIgnored(parts, fmt.Sprintf("unflag message unreadable (%v)", err))
			return
		}
		if h.Flagged.Delete(xuid) {
			h.saveFlags()
			h.Log(fmt.Sprintf("control: %s unflagged (out of the cheater pool)", parts[1]))
		}
	case n == 2 && parts[0] == "allow":
		xuid, err := parseHex(parts[1])
		if err != nil {
			h.ctlIgnored(parts, fmt.Sprintf("allow message unreadable (%v)", err))
			return
		}
		if _, ok := h.Revoked[xuid]; ok {
			delete(h.Revoked, xuid)
			h.Log(fmt.Sprintf("control: %s allowed again (revoke lifted)", parts[1]))
		}
	case n == 3 && parts[0] == "run":
		xuid, err := parseHex(parts[1])
		if err != nil {
			h.ctlIgnored(parts, fmt.Sprintf("run message unreadable (%v)", err))
			return
		}
		if len(h.Runs) >= 20000 {
			h.Runs = map[uint64]string{}
		}
		h.Runs[xuid] = diag.RunID(parts[2])
	case n == 3 && parts[0] == "assign":
		xuid, err := parseHex(parts[1])
		if err != nil {
			h.ctlIgnored(parts, fmt.Sprintf("assign message unreadable (%v)", err))
			return
		}
		h.Assign[xuid] = parts[2]
		h.Log(fmt.Sprintf("control: player %s assigned to lobby %s", parts[1], parts[2]))
	default:
		h.ctlIgnored(parts, fmt.Sprintf("unknown or malformed control message (%d field(s))", n))
	}
}

func strListRepr(a []string) string {
	parts := make([]string, len(a))
	for i, s := range a {
		parts[i] = pyReprStr(s)
	}
	return "[" + strings.Join(parts, ", ") + "]"
}

func jsonDumps(v any) string {
	s, err := pyjson.Dumps(v, pyjson.Opts{})
	if err != nil {
		return "{}"
	}
	return s
}

func (h *Hub) lobbyStatus() pyjson.Obj {
	stages := map[int]string{0: "Starting", 1: "Voting for a destination", 2: "Choosing cars", 3: "Getting ready", 4: "Driving to the event",
		5: "Arrived at destination", 6: "Preparing the event", 7: "Racing", 8: "Event results"}
	type row struct {
		id, kind string
		players  int
		obj      pyjson.Obj
	}
	var out []row
	for _, key := range h.Worlds.Keys() {
		world, _ := h.Worlds.Get(key)
		if key.Type != 2 && key.Type != LobbyTypeRoadTrip {
			continue
		}
		var peers []*Peer
		for _, p := range world.Peers.Values() {
			if p.Connected {
				peers = append(peers, p)
			}
		}
		if len(peers) == 0 {
			continue
		}
		id := key.Name
		if id == "" {
			id = "default"
		}
		kind := "freeroam"
		if key.Type == LobbyTypeRoadTrip {
			kind = "roadtrip"
		}
		members := []any{}
		for _, p := range peers {
			if p.Xuid != 0 {
				members = append(members, fmt.Sprintf("%x", p.Xuid))
			}
		}
		obj := pyjson.Obj{{K: "id", V: id}, {K: "kind", V: kind}, {K: "players", V: len(peers)}, {K: "capacity", V: capacity.ForType(key.Type)}, {K: "memberXuids", V: members}}
		if key.Type == LobbyTypeRoadTrip {
			st, ok := stages[world.RtState]
			if !ok {
				st = "Getting ready"
			}
			obj = append(obj, pyjson.Pair{K: "status", V: st}, pyjson.Pair{K: "event", V: world.RtIdx + 1}, pyjson.Pair{K: "events", V: 4})
		} else {
			obj = append(obj, pyjson.Pair{K: "status", V: "Free roaming"})
		}
		out = append(out, row{id, kind, len(peers), obj})
	}
	sort.SliceStable(out, func(i, j int) bool {
		if out[i].kind != out[j].kind {
			return out[i].kind < out[j].kind
		}
		if out[i].players != out[j].players {
			return out[i].players > out[j].players
		}
		return out[i].id < out[j].id
	})
	lobbies := make([]any, len(out))
	for i, r := range out {
		lobbies[i] = r.obj
	}
	return pyjson.Obj{{K: "lobbies", V: lobbies}}
}

func (h *Hub) mapStatus() pyjson.Obj {
	status := h.lobbyStatus()
	lobbies := status[0].V.([]any)
	rows := map[[2]string]*pyjson.Obj{}
	for i := range lobbies {
		o := lobbies[i].(pyjson.Obj)
		id, _ := o.Get("id")
		kind, _ := o.Get("kind")
		oc := o
		lobbies[i] = oc
		rows[[2]string{kind.(string), id.(string)}] = &oc
		defer func(i int, p *pyjson.Obj) { lobbies[i] = *p }(i, &oc)
	}
	for _, key := range h.Worlds.Keys() {
		world, _ := h.Worlds.Get(key)
		kind := "freeroam"
		if key.Type == LobbyTypeRoadTrip {
			kind = "roadtrip"
		}
		name := key.Name
		if name == "" {
			name = "default"
		}
		if row := rows[[2]string{kind, name}]; row != nil {
			cars := []any{}
			for _, p := range world.Peers.Values() {
				if p.Connected && p.Xuid != 0 && p.Pos != nil {
					var car any
					if p.ModelID != nil {
						car = *p.ModelID
					}
					cars = append(cars, pyjson.Obj{{K: "id", V: p.Owner}, {K: "xuid", V: fmt.Sprintf("%x", p.Xuid)}, {K: "x", V: int64(math.RoundToEven(p.Pos[0]))},
						{K: "z", V: int64(math.RoundToEven(p.Pos[2]))}, {K: "car", V: car}})
				}
			}
			row.Set("cars", cars)
		}
	}
	return status
}

func (h *Hub) populations() *omap.Map[string, []string] {
	out := omap.New[string, []string]()
	for _, addr := range h.Route.Keys() {
		w, _ := h.Route.Get(addr)
		if w.HubKey == nil || w.HubKey.Name == "" {
			continue
		}
		x := uint64(0)
		if peer, ok := w.Peers.Get(addr); ok && peer.Xuid != 0 {
			x = peer.Xuid
		}
		l, _ := out.Get(w.HubKey.Name)
		out.Set(w.HubKey.Name, append(l, fmt.Sprintf("%x", x)))
	}
	seen := map[*World]bool{}
	for _, w := range h.Route.Values() {
		if seen[w] {
			continue
		}
		seen[w] = true
		if w.HubKey != nil && w.HubKey.Name != "" {
			l, _ := out.Get(w.HubKey.Name)
			for _, b := range w.Bots {
				l = append(l, fmt.Sprintf("%x", b.Xuid))
			}
			out.Set(w.HubKey.Name, l)
		}
	}
	return out
}

func (h *Hub) populationsText() string {
	var b strings.Builder
	b.WriteString("pop")
	pop := h.populations()
	for _, name := range pop.Keys() {
		xs, _ := pop.Get(name)
		b.WriteString(" " + name + "=" + strings.Join(xs, ","))
	}
	flagged := h.FlaggedNow()
	if len(flagged) > 0 {
		parts := make([]string, len(flagged))
		for i, x := range flagged {
			parts[i] = fmt.Sprintf("%x", x)
		}
		b.WriteString(" !flagged=" + strings.Join(parts, ","))
	}
	return b.String()
}

// ---- cheater pool ----

func (h *Hub) loadFlags() {
	if h.FlagFile == "" {
		return
	}
	raw, err := os.ReadFile(h.FlagFile)
	if err != nil {
		if !os.IsNotExist(err) {
			h.FlagsError = fmt.Sprintf("%T: %v", err, err)
		}
		return
	}
	var m map[string][]json.RawMessage
	if err := json.Unmarshal(raw, &m); err != nil {
		h.FlagsError = fmt.Sprintf("%T: %v", err, err)
		return
	}
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		v := m[k]
		x, err := strconv.ParseUint(k, 10, 64)
		if err != nil || len(v) < 2 {
			h.FlagsError = "ValueError: bad entry " + k
			h.Flagged = omap.New[uint64, flagEnt]()
			return
		}
		var until float64
		var reason string
		_ = json.Unmarshal(v[0], &until)
		_ = json.Unmarshal(v[1], &reason)
		h.Flagged.Set(x, flagEnt{until, reason})
	}
}

func (h *Hub) saveFlags() {
	if h.FlagFile == "" {
		return
	}
	obj := pyjson.Obj{}
	for _, x := range h.Flagged.Keys() {
		e, _ := h.Flagged.Get(x)
		obj = append(obj, pyjson.Pair{K: strconv.FormatUint(x, 10), V: []any{e.until, e.reason}})
	}
	text, err := pyjson.Dumps(obj, pyjson.Opts{})
	if err == nil {
		tmp := h.FlagFile + ".tmp"
		if err = os.WriteFile(tmp, []byte(text), 0o644); err == nil {
			err = os.Rename(tmp, h.FlagFile)
		}
	}
	if err != nil {
		h.Idle.LogEvery("flags-save", 60.0, fmt.Sprintf("cheater pool file %s not written (%T: %v): %d flagged player(s) are lost on a restart", h.FlagFile, err, err, h.Flagged.Len()), diag.Error)
	}
}

// Flag puts a kicked player into the cheater pool for CheaterPoolHours (renewed by every kick).
func (h *Hub) Flag(xuid uint64, reason string) {
	if CheaterPoolHours <= 0 || xuid == 0 {
		return
	}
	h.Flagged.Set(xuid, flagEnt{h.Clk.Wall() + CheaterPoolHours*3600.0, reason})
	h.saveFlags()
	h.Log(fmt.Sprintf("control: %#x flagged for %s h (%s): matched only with other flagged players", xuid, pyfmt.FmtG(CheaterPoolHours), reason))
}

// FlaggedNow lists the flagged xuids, dropping expired ones.
func (h *Hub) FlaggedNow() []uint64 {
	now := h.Clk.Wall()
	gone := false
	for _, x := range h.Flagged.Keys() {
		e, _ := h.Flagged.Get(x)
		if e.until < now {
			h.Flagged.Delete(x)
			gone = true
		}
	}
	if gone {
		h.saveFlags()
	}
	return h.Flagged.Keys()
}

// Logout ends the connections a ticket key opened; not a ban.
func (h *Hub) Logout(xuid uint64, key []byte) {
	if keys, ok := h.Keys[xuid]; ok {
		delete(keys, string(key))
	}
	dropped := 0
	for addr, b := range h.Bound {
		if b.xuid != xuid || b.key != string(key) {
			continue
		}
		delete(h.Bound, addr)
		w, _ := h.Route.Get(addr)
		h.Route.Delete(addr)
		if w != nil {
			if peer, ok := w.Peers.Get(addr); ok {
				w.Send(peer, true, w.Goodbye(peer))
				w.DropPeer(peer, "signed out")
				dropped++
			}
		}
	}
	h.Log(fmt.Sprintf("control: %#x signed out (%d connection(s) ended)", xuid, dropped))
}

// Revoke is a ban or kick: forget the sign-in, the lobby assignment, and end every live connection of this player.
func (h *Hub) Revoke(xuid uint64) {
	delete(h.Authed, xuid)
	delete(h.Keys, xuid)
	for addr, b := range h.Bound {
		if b.xuid == xuid {
			delete(h.Bound, addr)
		}
	}
	delete(h.Assign, xuid)
	h.Revoked[xuid] = h.Clk.Mono() + 86400.0
	dropped := 0
	for _, w := range h.Worlds.Values() {
		for _, peer := range w.Peers.Values() {
			if peer.Xuid == xuid {
				w.Send(peer, true, w.Goodbye(peer))
				w.DropPeer(peer, "revoked by the account service")
				dropped++
			}
		}
	}
	h.Log(fmt.Sprintf("control: revoked %#x (%d connection(s) ended)", xuid, dropped))
}

// ---- tickets ----

// Ticket handles a UDP connection ticket: binds addr to its xuid when the MAC is right under one of the player's announced keys.
func (h *Hub) Ticket(data []byte, addr Addr) {
	xuid := binary.LittleEndian.Uint64(data[20:])
	ts := binary.LittleEndian.Uint64(data[28:])
	nonce, mac := data[36:52], data[52:84]
	now := h.Clk.Mono()
	why := ""
	var key string
	haveKey := false
	switch {
	case data[19] != 1:
		why = "unknown version"
	case h.isRevoked(xuid):
		why = "revoked"
	case math.Abs(h.Clk.Wall()-float64(ts)) > TicketSkewSeconds:
		why = "clock too far off"
	case h.Nonces.Has(nonceKey{xuid, string(nonce)}):
		why = "replayed"
	case h.Nonces.Len() >= MaxTicketNonces:
		why = "replay protection full"
	default:
		for k, until := range h.Keys[xuid] {
			if until > now {
				m := hmac.New(sha256.New, []byte(k))
				m.Write(data[:52])
				if hmac.Equal(m.Sum(nil), mac) {
					key, haveKey = k, true
					break
				}
			}
		}
		if !haveKey {
			why = "bad signature or no session"
		}
	}
	if why != "" {
		if len(h.AuthLog) < 5000 && now-h.authLogGet([2]string{"t", addr.IP}) > 10 {
			h.AuthLog[[2]string{"t", addr.IP}] = now
			h.Log(fmt.Sprintf("auth: ticket for %#x from %s:%d refused (%s)", xuid, addr.IP, addr.Port, why))
		}
		return
	}
	h.Nonces.Set(nonceKey{xuid, string(nonce)}, now+2*TicketSkewSeconds)
	held, has := h.Bound[addr]
	if has && held.xuid != xuid && held.until > now {
		h.Log(fmt.Sprintf("auth: ticket for %#x from %s:%d refused (address bound to %#x)", xuid, addr.IP, addr.Port, held.xuid))
		return
	}
	if !has || held.xuid != xuid || held.key != key {
		h.Log(fmt.Sprintf("auth: ticket for %#x from %s:%d accepted", xuid, addr.IP, addr.Port))
	}
	h.Bound[addr] = boundEnt{xuid, now + TicketBindSeconds, key}
	h.TicketEpochs[addr] = append([]byte(nil), nonce...)
}

func (h *Hub) isRevoked(x uint64) bool { _, ok := h.Revoked[x]; return ok }

func (h *Hub) authLogGet(k any) float64 {
	if v, ok := h.AuthLog[k]; ok {
		return v
	}
	return -1e9
}

// OpenConnectionFields returns (player id, lobby type) when the datagram starts with an OpenConnection.
func (h *Hub) OpenConnectionFields(data []byte) (uint64, int, bool) {
	if len(data) < 12 || data[8] == 0 || data[11]&0x7F != netv2.MsgOpenConnection {
		return 0, 0, false
	}
	br := bits.NewReader(data)
	hdr, err := netv2.ParseHeader(br)
	if err != nil || hdr.Count < 1 {
		return 0, 0, false
	}
	if t, err := br.Bits(7); err != nil || t != netv2.MsgOpenConnection {
		return 0, 0, false
	}
	if _, err := br.String(); err != nil {
		return 0, 0, false
	}
	if _, err := br.String(); err != nil {
		return 0, 0, false
	}
	if _, err := br.Bits(2); err != nil {
		return 0, 0, false
	}
	pid, err := br.Bits(64)
	if err != nil {
		return 0, 0, false
	}
	a, err := br.Bits(32)
	if err != nil {
		return 0, 0, false
	}
	if a == 1 || a == 2 || a == 3 {
		return pid, int(a), true
	}
	return pid, h.DefaultType, true
}

// Handle is Hub.handle: one datagram from addr.
func (h *Hub) Handle(data []byte, addr Addr) {
	if len(data) >= 19 && string(data[11:19]) == lightingMagic {
		h.lightingHandle(data, addr)
		return
	}
	if len(data) >= 19 && string(data[11:19]) == honkMagic {
		h.honkHandle(data, addr)
		return
	}
	if h.probeHandle(data, addr) {
		return
	}
	if len(data) == TicketLen && string(data[11:19]) == TicketMagic {
		h.Ticket(data, addr)
		return
	}
	player, ltype, opened := h.OpenConnectionFields(data)
	cur, _ := h.Route.Get(addr)
	if opened {
		if !h.OpenAllowed(player, addr) {
			return
		}
		var held *Peer
		if cur != nil {
			held, _ = cur.Peers.Get(addr)
		}
		if held != nil && held.Xuid != 0 && held.Xuid != player {
			h.refusedOpen(player, addr, h.Clk.Mono(), fmt.Sprintf("this address already holds a connection of %#x", held.Xuid))
			return
		}
		h.replaceConnections(player, addr)
		if held == nil && !h.pendingRoom(addr) {
			h.refusedOpen(player, addr, h.Clk.Mono(), fmt.Sprintf("too many half-open connections (limit %d in total, %d per address)", MaxPendingPeers, MaxPendingPerIP))
			return
		}
		name, assigned := h.Assign[player]
		var target *World
		if assigned && name != "" {
			target = h.WorldFor(ltype, name)
		} else {
			target = h.openWorldFor(ltype, player, &addr)
		}
		if !h.worldPoolFits(target, player) {
			flag := "not flagged"
			if h.Flagged.Has(player) {
				flag = "flagged"
			}
			h.Log(fmt.Sprintf("%#x world %s mixes cheater-pool and other players (this player is %s): sent to another world", player, strings.TrimSpace(target.Tag), flag))
			target = h.openWorldFor(ltype, player, &addr)
		}
		if !h.ownerRoom(target, &addr) {
			h.Idle.Warnf("%#x world %s has no relay owner ids left (%d of %d taken or quarantined): sent to another world", player, strings.TrimSpace(target.Tag), len(target.OwnerIDsTaken()), RelayOwnerLast)
			target = h.openWorldFor(ltype, player, &addr)
		}
		if target != cur && h.worldLoad(target, &addr) >= capacity.ForType(ltype) {
			nm := name
			if nm == "" {
				nm = "default"
			}
			h.Log(fmt.Sprintf("%#x assigned to full lobby %s: sent to another world (%d/%d players there)", player, nm, h.worldLoad(target, &addr), capacity.ForType(ltype)))
			target = h.openWorldFor(ltype, player, &addr)
		}
		if cur != nil && cur != target {
			if old, ok := cur.Peers.Get(addr); ok {
				cur.DropPeer(old, "moved to another lobby")
				cur.Peers.Delete(addr)
			}
		}
		h.Route.Set(addr, target)
		cur = target
	}
	if cur == nil {
		cur = h.Idle
	}
	h.handleSafe(cur, data, addr)
}

// handleSafe runs a world's handler and turns a panic (Python's exception) into the world's handle_failed.
func (h *Hub) handleSafe(w *World, data []byte, addr Addr) {
	defer func() {
		if r := recover(); r != nil {
			target := w
			if cw, ok := h.Route.Get(addr); ok && cw != nil {
				target = cw
			}
			target.HandleFailed(r, data, addr)
		}
	}()
	w.Handle(data, addr)
}

func (h *Hub) pendingRoom(addr Addr) bool {
	total, mine := 0, 0
	for _, w := range h.Worlds.Values() {
		for _, p := range w.Peers.Values() {
			if !p.Connected {
				total++
				if p.Addr.IP == addr.IP {
					mine++
				}
			}
		}
	}
	return total < MaxPendingPeers && mine < MaxPendingPerIP
}

// OpenAllowed is the identity check of an OpenConnection.
func (h *Hub) OpenAllowed(playerID uint64, addr Addr) bool {
	now := h.Clk.Mono()
	if h.isRevoked(playerID) {
		h.refusedOpen(playerID, addr, now, "revoked: kicked or banned by the account service, refused until its next sign-in")
		return false
	}
	if h.UDPAuth == "off" {
		return true
	}
	held, has := h.Bound[addr]
	var why string
	if has && held.until > now {
		if held.xuid == playerID {
			return true
		}
		why = fmt.Sprintf("this address showed a ticket for %#x", held.xuid)
	} else if h.UDPAuth == "required" && h.isAuthed(playerID, addr.IP) {
		return true
	} else if h.UDPAuth == "ticket" {
		why = "no valid ticket"
	} else {
		why = "not signed in from this address"
	}
	h.refusedOpen(playerID, addr, now, why)
	return false
}

func (h *Hub) refusedOpen(playerID uint64, addr Addr, now float64, why string) {
	if len(h.AuthLog) < 5000 && now-h.authLogGet(addr.IP) > 10 {
		h.AuthLog[addr.IP] = now
		h.Idle.Warnf("auth: refused OpenConnection of %#x from %s (%s; mode %s)", playerID, addr.IP, why, h.UDPAuth)
	}
}

func (h *Hub) isAuthed(playerID uint64, ip string) bool {
	e, ok := h.Authed[playerID]
	return ok && e.until > h.Clk.Mono() && e.ip == ip
}

// TunnelAdmit: a TCP connection is only taken from an address a player is signed in from (with UDP auth required).
func (h *Hub) TunnelAdmit(ip string) bool {
	if h.UDPAuth != "required" {
		return true
	}
	now := h.Clk.Mono()
	for _, e := range h.Authed {
		if e.ip == ip && e.until > now {
			return true
		}
	}
	return false
}

func (h *Hub) worldLoad(w *World, addr *Addr) int {
	n := 0
	for _, a := range h.Route.Keys() {
		rw, _ := h.Route.Get(a)
		if rw == w && (addr == nil || a != *addr) {
			n++
		}
	}
	return n + len(w.Bots)
}

func (h *Hub) replaceConnections(xuid uint64, addr Addr) {
	for _, a := range h.Route.Keys() {
		w, _ := h.Route.Get(a)
		peer, ok := w.Peers.Get(a)
		if a == addr || !ok || peer.Xuid != xuid {
			continue
		}
		w.Send(peer, true, w.Goodbye(peer))
		w.DropPeer(peer, "replaced by a newer connection of the same player")
		h.Route.Delete(a)
	}
}

func (h *Hub) worldPoolFits(world *World, xuid uint64) bool {
	flagged := map[uint64]bool{}
	for _, x := range h.FlaggedNow() {
		flagged[x] = true
	}
	mine := flagged[xuid]
	for _, p := range world.Peers.Values() {
		if p.Xuid != 0 && p.Xuid != xuid && flagged[p.Xuid] != mine {
			return false
		}
	}
	return true
}

func (h *Hub) ownerRoom(w *World, addr *Addr) bool {
	waiting := 0
	for _, a := range w.Peers.Keys() {
		p, _ := w.Peers.Get(a)
		if (addr == nil || a != *addr) && !p.Connected {
			waiting++
		}
	}
	return RelayOwnerLast-len(w.OwnerIDsTaken())-waiting >= 1
}

// openWorldFor: a player with no lobby assignment goes to the busiest world of that type that has room.
func (h *Hub) openWorldFor(lobbyType int, playerID uint64, addr *Addr) *World {
	load := map[*World]int{}
	for _, w := range h.Route.Values() {
		load[w]++
	}
	var best *World
	bestN := 0
	for _, key := range h.Worlds.Keys() {
		w, _ := h.Worlds.Get(key)
		n := load[w]
		if key.Type == lobbyType && 0 < n && n < capacity.ForType(lobbyType) && n > bestN && h.worldPoolFits(w, playerID) && h.ownerRoom(w, addr) {
			best, bestN = w, n
		}
	}
	if best != nil {
		return best
	}
	shared, ok := h.Worlds.Get(WorldKey{lobbyType, ""})
	if !ok || (load[shared] == 0 && h.worldPoolFits(shared, playerID) && h.ownerRoom(shared, addr)) {
		return h.WorldFor(lobbyType, "")
	}
	h.autoWorlds++
	return h.WorldFor(lobbyType, fmt.Sprintf("auto%d", h.autoWorlds))
}

// Sweep expires tickets, sign-ins and nonces and closes empty worlds (once a second).
func (h *Hub) Sweep() {
	now := h.Clk.Mono()
	for x, e := range h.Authed {
		if e.until < now {
			delete(h.Authed, x)
		}
	}
	for x, keys := range h.Keys {
		for k, until := range keys {
			if until < now {
				delete(keys, k)
			}
		}
		if len(keys) == 0 {
			delete(h.Keys, x)
		}
	}
	for addr, b := range h.Bound {
		if b.until < now && !h.Route.Has(addr) {
			delete(h.Bound, addr)
		}
	}
	for addr := range h.TicketEpochs {
		if _, ok := h.Bound[addr]; !ok {
			delete(h.TicketEpochs, addr)
		}
	}
	for {
		k, ok := h.Nonces.First()
		if !ok {
			break
		}
		if v, _ := h.Nonces.Get(k); v >= now {
			break
		}
		h.Nonces.Delete(k)
	}
	for x, until := range h.Revoked {
		if until < now {
			delete(h.Revoked, x)
		}
	}
	if len(h.AuthLog) > 5000 {
		h.AuthLog = map[any]float64{}
	}
	for _, addr := range h.Route.Keys() {
		w, _ := h.Route.Get(addr)
		if !w.Peers.Has(addr) {
			h.Route.Delete(addr)
		}
	}
	for _, key := range h.Worlds.Keys() {
		w, _ := h.Worlds.Get(key)
		if w.anyConnected() {
			delete(h.EmptySince, key)
		} else {
			since, ok := h.EmptySince[key]
			if !ok {
				since = now
				h.EmptySince[key] = since
			}
			if now-since > IdleWorldSeconds {
				h.Log(fmt.Sprintf("world (%d, %s) closed (empty)", key.Type, pyReprStr(key.Name)))
				h.Worlds.Delete(key)
				delete(h.EmptySince, key)
			}
		}
	}
}

// Step runs one pass of the serve loop's timers (tick when due, flush, sweep each second) and returns when the next flush is due.
func (h *Hub) Step() float64 {
	now := h.Clk.Mono()
	if now >= h.nextTick {
		h.nextTick = now + WorldTickSeconds
		for _, w := range h.Worlds.Values() {
			h.tickSafe(w)
		}
		now = h.Clk.Mono()
	}
	nxt := h.Idle.Flush(now)
	for _, w := range h.Worlds.Values() {
		nxt = math.Min(nxt, w.Flush(now))
	}
	if now-h.lastSweep > 1.0 {
		h.lastSweep = now
		h.Sweep()
	}
	h.nxt = nxt
	return nxt
}

func (h *Hub) tickSafe(w *World) {
	defer func() {
		if r := recover(); r != nil {
			ev := w.Event
			desc := "none"
			if ev != nil {
				desc = fmt.Sprintf("%d participant(s), begun=%s racing=%s over=%s", len(ev.People), pyBool(ev.Begun), pyBool(ev.Racing), pyBool(ev.Over))
			}
			w.ExcEvery([2]any{"tick", fmt.Sprint(r)}, 10.0, fmt.Sprintf("tick failed (%d peer(s), event %s, road trip state %d); the world keeps ticking", w.Peers.Len(), desc, w.RtState), r, diag.Error)
		}
	}()
	w.Tick()
}

// TunnelClosed drops the player of a TCP connection that ended.
func (h *Hub) TunnelClosed(addr Addr) {
	w, _ := h.Route.Get(addr)
	h.Route.Delete(addr)
	if w != nil {
		if peer, ok := w.Peers.Get(addr); ok {
			w.DropPeer(peer, "tunnel connection closed")
		}
	}
}

// Opened reports whether addr has a game connection in a world.
func (h *Hub) Opened(addr Addr) bool {
	w, ok := h.Route.Get(addr)
	return ok && w.Peers.Has(addr)
}

// Close flushes what must survive a clean exit: the cheater pool file and the event log.
func (h *Hub) Close() {
	h.saveFlags()
	if h.Events != nil {
		h.Events.Close()
	}
}
