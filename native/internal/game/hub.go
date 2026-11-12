package game

import (
	"crypto/hmac"
	cryptoRand "crypto/rand"
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
	"sync"
	"sync/atomic"
	"time"

	"fh2mp/internal/bits"
	"fh2mp/internal/capacity"
	"fh2mp/internal/catalog"
	"fh2mp/internal/diag"
	"fh2mp/internal/eventlog"
	"fh2mp/internal/netv2"
	"fh2mp/internal/omap"
	"fh2mp/internal/pyfmt"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/pyrand"
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
	cfg         HubConfig
	Clk         Clock
	Rng         Rand
	Sock        PacketConn
	Out         *LogOut
	Events      *eventlog.Log
	Sinks       *Sinks
	catalogPtr  atomic.Pointer[catalog.Catalog]
	catalogMu   sync.Mutex
	authMu      sync.Mutex
	deferMu     sync.Mutex
	deferred    []func()
	workers     []*Worker
	manual      bool
	syncer      *catalog.Syncer
	CtlSend     func(data []byte, to Addr)
	DefaultType int
	Assign      map[uint64]string
	// Follow: players assigned to a private game ("assign <xuid> <name> follow"): they go to the world of that name that has players, whatever
	// lobby type their game asks for (an invite accepted in a Car Meet joins the host's private Free Roam as a Car Meet, dev 2026-11-12)
	Follow         map[uint64]bool
	Standins       map[uint64]map[int64]int64 // a car-pack player's custom car -> the stock car the others see (control "standins")
	CustomCars     map[uint64]map[int64]bool  // every custom car a player's launcher listed (control "standins"), with or without a stand-in
	Runs           map[uint64]string
	Authed         map[uint64]authEnt
	Flagged        *omap.Map[uint64, flagEnt]
	FlagsError     string
	FlagsReleased  int // entries of a restricted-tools report dropped by loadFlags (NitroSystem stopped being one 2026-11-07)
	FlagFile       string
	Revoked        map[uint64]float64
	UDPAuth        string
	Keys           map[uint64]map[string]float64
	TicketEpochs   map[Addr][]byte
	Bound          map[Addr]boundEnt
	Nonces         *omap.Map[nonceKey, float64]
	NonceCount     map[uint64]int // live nonces per xuid (MaxTicketNoncesPerXuid)
	AuthLog        map[any]float64
	stopCalls      atomic.Int64 // worker pauses so far (tests check that unauthenticated datagrams cause none)
	Worlds         *omap.Map[WorldKey, *World]
	Route          *omap.Map[Addr, *World]
	EmptySince     map[WorldKey]float64
	Idle           *World
	LightingRoutes map[uint64]Addr
	HonkRoutes     map[uint64]Addr
	CarselRoutes   map[uint64]Addr
	probeSeen      map[Addr]probeEnt
	autoWorlds     int
	lastSweep      float64
	nextTick       float64
	nxt            float64
	Tunnel         TunnelHost
	// PickRng makes the generator of one Road Trip vote draw (Python: a fresh random.Random() per call).
	PickRng func() Rand
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
	h := &Hub{cfg: cfg, Clk: cfg.Clock, Rng: cfg.Rng, Sock: cfg.Sock, Out: cfg.Out, Sinks: cfg.Sinks, CtlSend: cfg.CtlSend,
		Assign: map[uint64]string{}, Follow: map[uint64]bool{}, Standins: map[uint64]map[int64]int64{}, CustomCars: map[uint64]map[int64]bool{}, Runs: map[uint64]string{}, Authed: map[uint64]authEnt{}, Flagged: omap.New[uint64, flagEnt](), Revoked: map[uint64]float64{},
		Keys: map[uint64]map[string]float64{}, TicketEpochs: map[Addr][]byte{}, Bound: map[Addr]boundEnt{}, Nonces: omap.New[nonceKey, float64](), NonceCount: map[uint64]int{},
		AuthLog: map[any]float64{}, Worlds: omap.New[WorldKey, *World](), Route: omap.New[Addr, *World](), EmptySince: map[WorldKey]float64{},
		LightingRoutes: map[uint64]Addr{}, HonkRoutes: map[uint64]Addr{}, CarselRoutes: map[uint64]Addr{}, probeSeen: map[Addr]probeEnt{}, nxt: math.Inf(1)}
	h.catalogPtr.Store(cfg.Catalog)
	h.DefaultType = 2
	h.PickRng = func() Rand { return pyrand.New(entropySeed()) }
	if cfg.Carmeet {
		h.DefaultType = LobbyTypeCarMeet
	} else if cfg.Roadtrip {
		h.DefaultType = LobbyTypeRoadTrip
	}
	DlcCars = loadDlcCars()
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
	if h.UDPAuth == "off" {
		idle.Log("WARNING: FH2MP_UDP_AUTH=off: any OpenConnection is taken at its word (set ticket, or required, on a public server)")
	}
	for _, p := range StartupProblems {
		idle.Error("startup.data_missing " + p)
	}
	if h.FlagsError != "" {
		idle.Warnf("cheater pool file %s not read (%s): the pool starts empty", h.FlagFile, h.FlagsError)
	}
	if h.FlagsReleased > 0 {
		h.saveFlags()
		idle.Logf("cheater pool: %d player(s) flagged only for a restricted-tools report released (no tool report flags a player any more); %d remain flagged", h.FlagsReleased, h.Flagged.Len())
	}
	return h, nil
}

// Catalog is the current car policy and part data.
func (h *Hub) Catalog() *catalog.Catalog { return h.catalogPtr.Load() }

// PollCatalog re-reads the synced car database when it changed.
func (h *Hub) PollCatalog() {
	h.catalogMu.Lock()
	defer h.catalogMu.Unlock()
	if n := h.syncer.Poll(h.Clk.Mono(), h.catalogPtr.Load(), true, h.Idle.Log); n != nil {
		h.catalogPtr.Store(n)
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
			gamedb = roadTripDBPath()
		}
		if _, err := os.Stat(gamedb); err == nil {
			w.RtGamedb = gamedb
			if cand, err := PickRoadTripCandidates(h.PickRng(), gamedb, 1); err == nil {
				w.RtCandidates = cand
				w.Logf("road trip candidates: locations %s events %s classes %s", locsRepr(cand), eventsRepr(cand), intList(cand.Classes[:]))
			}
		} else {
			w.Logf("road trip: no game database (%s); the vote will be empty. Build %s with tools/gamedata/make_roadtrip_db.py or point FH2MP_GAMEDB at gamedbRC.slt", gamedb, roadTripDBPath())
		}
	}
	h.Worlds.Set(key, w)
	h.attachWorld(w)
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

// ControlText handles one control message: assign, auth, logout, revoke, allow, unflag, flags, run, tools, pop, lobbies, positions, bots.
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
	case n == 1 && parts[0] == "flags":
		h.ctlReply(replyTo, "flags", []byte(jsonDumps(h.flagStatus())))
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
	case n == 3 && parts[0] == "standins":
		xuid, err := parseHex(parts[1])
		if err != nil {
			h.ctlIgnored(parts, fmt.Sprintf("standins message unreadable (%v)", err))
			return
		}
		if parts[2] == "-" {
			_, had := h.CustomCars[xuid]
			delete(h.Standins, xuid)
			delete(h.CustomCars, xuid)
			if had {
				h.Log(fmt.Sprintf("control: stand-ins for %s forgotten: its game runs the server's car database (no custom cars)", parts[1]))
			}
			return
		}
		got, owned, notes := h.parseStandins(parts[2])
		if len(h.Standins) >= 20000 {
			h.Standins, h.CustomCars = map[uint64]map[int64]int64{}, map[uint64]map[int64]bool{}
		}
		h.Standins[xuid] = got
		if h.CustomCars == nil {
			h.CustomCars = map[uint64]map[int64]bool{}
		}
		h.CustomCars[xuid] = owned
		h.Log(fmt.Sprintf("control: stand-ins for %s: %s", parts[1], strings.Join(notes, "; ")))
	case (n == 3 || n == 4 && parts[3] == "follow") && parts[0] == "assign":
		xuid, err := parseHex(parts[1])
		if err != nil {
			h.ctlIgnored(parts, fmt.Sprintf("assign message unreadable (%v)", err))
			return
		}
		h.Assign[xuid] = parts[2]
		if n == 4 {
			h.Follow[xuid] = true
			h.Log(fmt.Sprintf("control: player %s assigned to lobby %s, a private game: it joins that lobby's world with players in it, whatever lobby type its game asks for", parts[1], parts[2]))
		} else {
			delete(h.Follow, xuid)
			h.Log(fmt.Sprintf("control: player %s assigned to lobby %s", parts[1], parts[2]))
		}
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
	if fr := h.freeRoamLobbies(); PopFreeRoam && len(fr) > 0 {
		b.WriteString(" !freeroam=" + strings.Join(fr, ","))
	}
	return b.String()
}

// freeRoamLobbies are the lobby names that have a Free Roam world (type 2) with a connected player, in world order.
func (h *Hub) freeRoamLobbies() []string {
	var out []string
	for _, key := range h.Worlds.Keys() {
		if key.Type != LobbyTypeFreeRoam || key.Name == "" {
			continue
		}
		world, _ := h.Worlds.Get(key)
		for _, p := range world.Peers.Values() {
			if p.Connected {
				out = append(out, key.Name)
				break
			}
		}
	}
	return out
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
		if strings.HasPrefix(reason, "restricted tools") {
			h.FlagsReleased++
			continue
		}
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

// flagStatus is the cheater pool for the operator panel: who is flagged, why and until when (unix seconds). The empty "lobbies" list is there because
// the control client's JSON query only accepts a reply that carries one.
func (h *Hub) flagStatus() pyjson.Obj {
	list := []any{}
	for _, x := range h.FlaggedNow() {
		e, _ := h.Flagged.Get(x)
		list = append(list, pyjson.Obj{{K: "xuid", V: fmt.Sprintf("%x", x)}, {K: "reason", V: e.reason}, {K: "until", V: e.until}})
	}
	return pyjson.Obj{{K: "lobbies", V: []any{}}, {K: "flagged", V: list}}
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
	h.revokeTables(xuid)
	dropped := 0
	for _, w := range h.Worlds.Values() {
		dropped += w.dropAccount(xuid)
	}
	h.Log(fmt.Sprintf("control: revoked %#x (%d connection(s) ended)", xuid, dropped))
}

// dropAccount ends this world's connections of an account (CloseConnection, then removal).
func (w *World) dropAccount(xuid uint64) int {
	n := 0
	for _, peer := range w.Peers.Values() {
		if peer.Xuid == xuid {
			w.Send(peer, true, w.Goodbye(peer))
			w.DropPeer(peer, "revoked by the account service")
			n++
		}
	}
	return n
}

// revokeTables forgets everything the hub holds for the account; the connections are ended by the caller.
func (h *Hub) revokeTables(xuid uint64) {
	delete(h.Authed, xuid)
	delete(h.Keys, xuid)
	for addr, b := range h.Bound {
		if b.xuid == xuid {
			delete(h.Bound, addr)
		}
	}
	delete(h.Assign, xuid)
	delete(h.Follow, xuid)
	h.Revoked[xuid] = h.Clk.Mono() + 86400.0
}

// ---- tickets ----

// Ticket handles a UDP connection ticket: binds addr to its xuid when the MAC is right under one of the player's announced keys.
func (h *Hub) Ticket(data []byte, addr Addr) {
	now := h.Clk.Mono()
	key, why := h.ticketCheck(data, addr, now)
	if why != "" {
		h.ticketRefused(data, addr, now, why)
		return
	}
	h.ticketBind(data, addr, key, now)
}

// ticketCheck is the part of Ticket that only reads hub tables, so the hub goroutine (their only writer) may run it without pausing the workers:
// the signing key when the ticket is good, else why it is refused.
func (h *Hub) ticketCheck(data []byte, addr Addr, now float64) (key, why string) {
	xuid := binary.LittleEndian.Uint64(data[20:])
	ts := binary.LittleEndian.Uint64(data[28:])
	nonce, mac := data[36:52], data[52:84]
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
	case h.NonceCount[xuid] >= MaxTicketNoncesPerXuid:
		why = "replay protection full for this player"
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
	return key, why
}

// ticketRefused logs a refused ticket, one line per address per 10 s. It may run beside the workers (refusedOpen does), hence authMu.
func (h *Hub) ticketRefused(data []byte, addr Addr, now float64, why string) {
	xuid := binary.LittleEndian.Uint64(data[20:])
	h.authMu.Lock()
	defer h.authMu.Unlock()
	if now-h.authLogGet([2]string{"t", addr.IP}) > 10 {
		h.AuthLog[[2]string{"t", addr.IP}] = now
		h.Log(fmt.Sprintf("auth: ticket for %#x from %s:%d refused (%s)", xuid, addr.IP, addr.Port, why))
	}
}

// ticketBind records an accepted ticket: the nonce, the address binding and the ticket epoch (writes: under the pause, like every hub-table write).
func (h *Hub) ticketBind(data []byte, addr Addr, key string, now float64) {
	xuid := binary.LittleEndian.Uint64(data[20:])
	nonce := data[36:52]
	h.Nonces.Set(nonceKey{xuid, string(nonce)}, now+2*TicketSkewSeconds)
	h.NonceCount[xuid]++
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
		h.lightingHandle(data, addr, nil)
		return
	}
	if len(data) >= 19 && string(data[11:19]) == honkMagic {
		h.honkHandle(data, addr, nil)
		return
	}
	if len(data) >= 19 && string(data[11:19]) == carselMagic {
		h.carselHandle(data, addr, nil)
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
			target = h.WorldFor(h.followType(player, ltype, name, addr), name)
			if h.Follow[player] && !target.Private && !target.Roadtrip && !target.Carmeet {
				target.Private = true
				target.Logf("private game: this world is invite-only (its first member is %#x): its players are told PrivacyMode = InviteOnly and it keeps a leader", player)
			}
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
	h.authMu.Lock()
	defer h.authMu.Unlock()
	if now-h.authLogGet(addr.IP) > 10 { // the sweep clears the table past 5000 addresses
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

// followType is the lobby type of the world a player assigned to `name` goes to: the one its game asks for, except for a private game's
// member (Follow) when that world has nobody else and another world of that name has players: then that one's (the host's private Free Roam
// or Road Trip). The world tells the game its lobby type at the handshake.
func (h *Hub) followType(player uint64, ltype int, name string, addr Addr) int {
	if !h.Follow[player] {
		return ltype
	}
	if w, ok := h.Worlds.Get(WorldKey{ltype, name}); ok && h.worldLoad(w, &addr) > 0 {
		return ltype
	}
	for _, k := range h.Worlds.Keys() {
		if k.Name == name && k.Type != ltype {
			if w, _ := h.Worlds.Get(k); w != nil && h.worldLoad(w, &addr) > 0 {
				h.Log(fmt.Sprintf("%#x asks for lobby type %d in %.8s, where only the lobby type %d world has players (a private game): it goes there", player, ltype, name, k.Type))
				return k.Type
			}
		}
	}
	return ltype
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
		if h.NonceCount[k.xuid] > 1 {
			h.NonceCount[k.xuid]--
		} else {
			delete(h.NonceCount, k.xuid)
		}
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
				h.detachWorld(w)
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

// Opened reports whether addr has a game connection (half-open or connected) in a world: the route is set at the OpenConnection and dropped by
// the sweep once the address left its world. Route is a hub table, so the hub goroutine reads it without pausing the workers.
func (h *Hub) Opened(addr Addr) bool {
	return h.Route.Has(addr)
}

// Close flushes what must survive a clean exit: the cheater pool file and the event log.
func (h *Hub) Close() {
	h.saveFlags()
	if h.Events != nil {
		h.Events.Close()
	}
}

func entropySeed() uint64 {
	var b [8]byte
	if _, err := cryptoRand.Read(b[:]); err != nil {
		return uint64(time.Now().UnixNano())
	}
	return binary.LittleEndian.Uint64(b[:])
}

// Stop runs fn while every worker is idle (immediately when there are none).
func (h *Hub) Stop(fn func()) { h.stop(fn) }

// Workers is the number of worker goroutines (0 = everything on the calling goroutine).
func (h *Hub) Workers() int { return len(h.workers) }
