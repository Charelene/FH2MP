package traffic

import (
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"

	"fh2mp/internal/bits"
	"fh2mp/internal/diag"
	"fh2mp/internal/netv2"
	"fh2mp/internal/omap"
)

var (
	log  = diag.Get("traffic")
	rate = diag.NewRateLimit(nil, 0)
)

// Message ids.
const (
	MsgServerFullSync      = 16
	MsgServerRouteProgress = 17
	MsgServerMessage       = 71
	MsgServerFullRoute     = 73
	MsgServerStationary    = 74
	MsgClientFullSync      = 18
	MsgClientRouteProgress = 19
	MsgClientMessage       = 72
	MsgClientFullRoute     = 75
	MsgClientStationary    = 76
)

// TrafficServerMessageType (client -> server).
const (
	SRequestTraffic = iota
	SWithdrawRequest
	SRequestDespawn
	STrafficSpawned
	STrafficRemoved
	SCanTakeOwnership
	SRequestOwnership
	SCarModelLoaded
	SCarModelUnloaded
)

var sNames = []string{"RequestTraffic", "WithdrawTrafficRequest", "RequestDespawn", "TrafficSpawned", "TrafficRemoved", "CanTakeOwnership",
	"RequestOwnership", "CarModelLoaded", "CarModelUnloaded"}

// TrafficClientMessageType (server -> client).
const (
	CTrafficLimit = iota
	CSpawnGranted
	CSpawnCanceled
	CWithdrawAccepted
	CAddTraffic
	CRemoveTraffic
	CTakeOwnership
	CDropOwnership
	COwnershipDenied
	CLoadCarModel
	CRemoveCarModel
	CUnloadCarModel
	CAllModelsSent
	CInitialJoinComplete
)

var cNames = []string{"TrafficLimit", "SpawnRequestGranted", "SpawnRequestCanceled", "WithdrawRequestAccepted", "AddTraffic",
	"RemoveTraffic", "TakeOwnership", "DropOwnership", "OwnershipRequestDenied", "LoadCarModel", "RemoveCarModel",
	"UnloadCarModel", "AllModelIdsSent", "InitialJoinComplete"}

// Timers and limits.
const (
	IgnoredLogSeconds = 30.0
	GrantTimeoutS     = 15.0
	ReportSettleS     = 0.5
	NoReportS         = 2.0
	JoinWaitS         = 15.0
	SummaryS          = 30.0
	TickS             = 0.2
	MaxMessagesPerS   = 400
	TrafficLimit      = HardCap
)

var updateLayout = map[int][]int{
	MsgServerFullSync:      append([]int{8, 32, 32, 32, 32}, append(rep(16, 9), 32, 1, 1)...),
	MsgServerRouteProgress: {8, 32, 32, 32, 32, 32, 32},
	MsgServerFullRoute:     append(append([]int{8}, rep(16, 8)...), 2, 32, 32, 32, 32, 32, 32),
	MsgServerStationary:    {8, 32, 32, 32, 16, 16, 16, 32, 1},
}

func rep(v, n int) []int {
	out := make([]int, n)
	for i := range out {
		out[i] = v
	}
	return out
}

var relayLayout = map[int][]int{
	MsgServerFullSync:      updateLayout[MsgServerFullSync],
	MsgServerRouteProgress: updateLayout[MsgServerRouteProgress][:4],
	MsgServerFullRoute:     updateLayout[MsgServerFullRoute][:len(updateLayout[MsgServerFullRoute])-3],
	MsgServerStationary:    updateLayout[MsgServerStationary],
}

var relayID = map[int]int{MsgServerFullSync: MsgClientFullSync, MsgServerRouteProgress: MsgClientRouteProgress,
	MsgServerFullRoute: MsgClientFullRoute, MsgServerStationary: MsgClientStationary}

var updateNames = map[int]string{MsgServerFullSync: "FullSync", MsgServerRouteProgress: "RouteProgress", MsgServerFullRoute: "FullRoute",
	MsgServerStationary: "Stationary"}

// Layouts returns the update layouts the game server needs to parse a client's update messages.
func Layouts() map[int][]int { return updateLayout }

var radiusKinds = map[int]bool{0: true, 1: true, 2: true, 4: true, 5: true, 6: true}

// LoadModels reads the traffic car model ids (data/trafficcars.txt or FH2MP_TRAFFIC_CARS), comma or newline separated.
func LoadModels(path string) []int {
	if path == "" {
		path = os.Getenv("FH2MP_TRAFFIC_CARS")
	}
	if path == "" {
		path = "data/trafficcars.txt"
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		log.Warn(fmt.Sprintf("car model list %s cannot be read, so there is no online traffic (TrafficWorld stays disabled): %T: %v", path, err, err))
		return nil
	}
	var out []int
	for _, x := range strings.Split(strings.ReplaceAll(string(raw), "\n", ","), ",") {
		x = strings.TrimSpace(x)
		if x == "" {
			continue
		}
		if v, err := strconv.Atoi(x); err == nil {
			out = append(out, v)
		}
	}
	return out
}

func f32(b uint64) float64 { return float64(math.Float32frombits(uint32(b))) }

func signed14(v int) int {
	if v&(1<<13) != 0 {
		return v - (1 << 14)
	}
	return v
}

// CM options for ClientMessage; defaults are a=0xFF, s14=-1, u5=31, b=0xFF, flag=0.
type CM struct{ A, S14, U5, B, Flag int }

func defCM() CM { return CM{0xFF, -1, 31, 0xFF, 0} }

// ClientMessage is client_message: kind:4 | u8 | s14 | u5 | u8 | bool | seq:32.
func ClientMessage(kind int, seq uint32, c CM) netv2.Msg {
	return func(w *bits.Writer) {
		w.Bits(MsgClientMessage, 7)
		w.Bits(uint64(kind), 4)
		w.Bits(uint64(c.A&0xFF), 8)
		w.Bits(uint64(c.S14&0x3FFF), 14)
		w.Bits(uint64(c.U5&0x1F), 5)
		w.Bits(uint64(c.B&0xFF), 8)
		w.Bits(uint64(c.Flag&1), 1)
		w.Bits(uint64(seq), 32)
	}
}

// UpdateMessage is update_message.
func UpdateMessage(mtype int, fields []uint64) netv2.Msg {
	layout := relayLayout[mtype]
	return func(w *bits.Writer) {
		w.Bits(uint64(relayID[mtype]), 7)
		for i, width := range layout {
			if i >= len(fields) {
				break
			}
			w.Bits(fields[i], width)
		}
	}
}

// Peer is a client as the traffic world sees it.
type Peer interface {
	comparable
	PortNumber() int
	CarPos() ([3]float64, bool)
}

// peerState is the per-client wire state.
type peerState struct {
	seq       uint32
	joined    bool
	joinedAt  float64
	burstN    int
	burstAt   *float64
	grantedAt *omap.Map[int, float64]
	rx        int
	windowAt  float64
	windowN   int
	flooded   bool
	joinUntil *float64
	reportAt  *float64
	early     *omap.Map[int, bool]
}

// CarData is what the manager calls a car's info: its AddTraffic struct and the last state of each kind.
type CarData struct {
	Struct                          [5]int
	FullSync, FullRoute, Stationary []uint64
	Order                           map[int]int
}

type stored struct {
	n, m int
	f    []uint64
}

// Latest returns the stored states as a new holder must apply them.
func (d *CarData) Latest() []struct {
	M int
	F []uint64
} {
	var s []stored
	for _, e := range []struct {
		m int
		f []uint64
	}{{MsgServerFullSync, d.FullSync}, {MsgServerFullRoute, d.FullRoute}, {MsgServerStationary, d.Stationary}} {
		if len(e.f) > 0 {
			s = append(s, stored{d.Order[e.m], e.m, e.f})
		}
	}
	sort.SliceStable(s, func(i, j int) bool { return s[i].n < s[j].n })
	moved := -1
	for _, e := range s {
		if e.m != MsgServerStationary && e.n > moved {
			moved = e.n
		}
	}
	var out []struct {
		M int
		F []uint64
	}
	for _, e := range s {
		if e.m == MsgServerStationary && e.n < moved {
			continue
		}
		out = append(out, struct {
			M int
			F []uint64
		}{e.m, e.f})
	}
	return out
}

// UpdatePosition is the car's position in an update from its owner.
func UpdatePosition(mtype int, fields []uint64) [3]float64 {
	var words []uint64
	switch mtype {
	case MsgServerFullSync:
		words = fields[2:5]
	case MsgServerStationary:
		words = fields[1:4]
	default:
		words = fields[len(fields)-3:]
	}
	return [3]float64{f32(words[0]), f32(words[1]), f32(words[2])}
}

// Host is what the traffic world needs from the game server.
type Host[P Peer] interface {
	Send(peer P, msgs []netv2.Msg, fast bool)
	Peers() []P
	Log(string)
	Universe(peer P) int
	Bake(netv2.Msg) netv2.Msg
	Now() float64 // monotonic seconds
}

// World is traffic for one world (lobby).
type World[P Peer] struct {
	host        Host[P]
	Models      []int
	state       *omap.Map[P, *peerState]
	Enabled     bool
	Counters    map[string]int
	counterKeys []string
	Manager     *Manager
	nextTick    float64
	nextSummary float64
	arrivals    int
	summarySent map[string]int
	batch       *[]netv2.Msg
	out         *outAdapter[P]
}

// NewWorld builds the traffic of one world; models nil loads the default list.
func NewWorld[P Peer](host Host[P], models []int) *World[P] {
	if models == nil {
		models = LoadModels("")
	}
	w := &World[P]{host: host, Models: models, state: omap.New[P, *peerState](), Counters: map[string]int{}, summarySent: map[string]int{}}
	w.Enabled = len(models) > 0 && os.Getenv("FH2MP_TRAFFIC") != "0"
	w.out = &outAdapter[P]{w: w}
	w.Manager = NewManager(w.out)
	if !w.Enabled {
		host.Log("traffic: disabled (no model list or FH2MP_TRAFFIC=0)")
	} else {
		host.Log(fmt.Sprintf("traffic: %d car models", len(models)))
	}
	return w
}

func (w *World[P]) count(name string) {
	if _, ok := w.Counters[name]; !ok {
		w.counterKeys = append(w.counterKeys, name)
	}
	w.Counters[name]++
}

func (w *World[P]) st(p P) *peerState {
	if s, ok := w.state.Get(p); ok {
		return s
	}
	s := &peerState{grantedAt: omap.New[int, float64](), early: omap.New[int, bool]()}
	w.state.Set(p, s)
	return s
}

// Active lists the connected, entered clients that joined the traffic.
func (w *World[P]) Active() []P {
	var out []P
	for _, p := range w.host.Peers() {
		if s, ok := w.state.Get(p); ok && s.joined {
			out = append(out, p)
		}
	}
	return out
}

func (w *World[P]) push(p P, kind int, c CM) {
	s := w.st(p)
	msg := ClientMessage(kind, s.seq, c)
	s.seq++
	w.count(cNames[kind])
	if w.batch != nil {
		*w.batch = append(*w.batch, msg)
	} else {
		w.host.Send(p, []netv2.Msg{msg}, false)
	}
}

func (w *World[P]) emit(p P, msg netv2.Msg) {
	if w.batch != nil {
		*w.batch = append(*w.batch, msg)
	} else {
		w.host.Send(p, []netv2.Msg{msg}, true)
	}
}

// outAdapter implements Out on top of the world (traffic_manager.Out).
type outAdapter[P Peer] struct{ w *World[P] }

func (o *outAdapter[P]) Add(key Key, car *Car) {
	p := key.(P)
	d := car.Info.(*CarData)
	o.w.push(p, CAddTraffic, CM{d.Struct[0], d.Struct[1], d.Struct[2], d.Struct[3], d.Struct[4]})
	for _, e := range d.Latest() {
		o.w.emit(p, UpdateMessage(e.M, e.F))
	}
}

func (o *outAdapter[P]) Remove(key Key, vid int) {
	p := key.(P)
	if o.w.state.Has(p) {
		c := defCM()
		c.A = vid
		o.w.push(p, CRemoveTraffic, c)
	}
}

func (o *outAdapter[P]) Granted(key Key, vid int) {
	p := key.(P)
	o.w.st(p).grantedAt.Set(vid, o.w.host.Now())
	c := defCM()
	c.A = vid
	o.w.push(p, CSpawnGranted, c)
}

func (o *outAdapter[P]) Canceled(key Key, vid int) {
	p := key.(P)
	o.w.st(p).grantedAt.Delete(vid)
	c := defCM()
	c.A = vid
	o.w.push(p, CSpawnCanceled, c)
}

func (o *outAdapter[P]) Withdrawn(key Key) { o.w.push(key.(P), CWithdrawAccepted, defCM()) }

func (o *outAdapter[P]) Take(key Key, vid int) {
	c := defCM()
	c.A = vid
	o.w.push(key.(P), CTakeOwnership, c)
}

func (o *outAdapter[P]) Drop(key Key, vid int) {
	p := key.(P)
	if o.w.state.Has(p) {
		c := defCM()
		c.A = vid
		o.w.push(p, CDropOwnership, c)
	}
}

func (o *outAdapter[P]) Denied(key Key, vid int) {
	p := key.(P)
	c := defCM()
	c.A = vid
	o.w.push(p, COwnershipDenied, c)
	if ok, skipped := rate.Allow(fmt.Sprint("denied", p.PortNumber()), IgnoredLogSeconds); ok {
		extra := ""
		if skipped > 0 {
			extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
		}
		log.Debug(fmt.Sprintf("%d ownership of car %d denied: %s%s", p.PortNumber(), vid, o.w.Manager.Refusal, extra))
	}
}

// Join is the server's initial sequence for a client that entered the session.
func (w *World[P]) Join(p P, force bool) {
	if !w.Enabled {
		return
	}
	s := w.st(p)
	if s.joined && !force {
		return
	}
	if s.joined {
		w.Manager.Reset(p)
	}
	s.joined = true
	s.joinedAt = w.host.Now()
	pl := w.Manager.Join(p)
	for _, model := range s.early.Keys() {
		loaded, _ := s.early.Get(model)
		if loaded {
			w.Manager.ModelLoaded(p, model)
		} else {
			w.Manager.ModelUnloaded(p, model)
		}
	}
	s.early.Clear()
	if len(pl.Models) > 0 {
		t := s.joinedAt
		s.reportAt = &t
	} else {
		s.reportAt = nil
	}
	batch := []netv2.Msg{}
	w.batch = &batch
	for _, model := range w.Models {
		c := defCM()
		c.S14 = model
		w.push(p, CLoadCarModel, c)
	}
	w.push(p, CAllModelsSent, defCM())
	c := defCM()
	c.A = TrafficLimit
	w.push(p, CTrafficLimit, c)
	w.batch = nil
	w.host.Send(p, batch, false)
	until := w.host.Now() + JoinWaitS
	s.joinUntil = &until
	extra := ""
	if len(pl.Models) > 0 {
		extra = fmt.Sprintf(" (%d models reported before the join)", len(pl.Models))
	}
	w.host.Log(fmt.Sprintf("traffic: join %d: %d models, limit %d; InitialJoinComplete after its model report%s", p.PortNumber(), len(w.Models), TrafficLimit, extra))
}

// Leave removes a client from the traffic.
func (w *World[P]) Leave(p P) {
	if w.state.Delete(p) {
		w.Manager.Leave(p)
		w.host.Log(fmt.Sprintf("traffic: %d left; %d cars remain", p.PortNumber(), w.Manager.Cars.Len()))
	}
}

func (w *World[P]) flooding(p P, s *peerState) bool {
	now := w.host.Now()
	if now-s.windowAt >= 1.0 {
		s.windowAt, s.windowN = now, 0
	}
	s.windowN++
	if s.windowN <= MaxMessagesPerS {
		return false
	}
	if !s.flooded {
		s.flooded = true
		w.host.Log(fmt.Sprintf("traffic: %d over %d messages/s; the excess is ignored", p.PortNumber(), MaxMessagesPerS))
	}
	return true
}

// OnMessage handles a ServerTrafficMessage.
func (w *World[P]) OnMessage(p P, kind, a, s14, u5, b, flag int, seq uint64) {
	s := w.st(p)
	s.rx++
	nm := fmt.Sprintf("?%d", kind)
	if kind < len(sNames) {
		nm = sNames[kind]
	}
	if s.rx <= 60 || s.rx%100 == 0 {
		held := 0
		if pl, ok := w.Manager.Players.Get(p); ok {
			held = len(pl.Held)
		}
		w.host.Log(fmt.Sprintf("traffic: %d -> %s a=%d s14=%d u5=%d b=%d flag=%d seq=%d (held %d)", p.PortNumber(), nm, a, signed14(s14), u5, b, flag, seq, held))
	}
	if w.Enabled && !s.joined && (kind == SCarModelLoaded || kind == SCarModelUnloaded) && !w.flooding(p, s) {
		s.early.Delete(signed14(s14))
		if s.early.Len() < 64 {
			s.early.Set(signed14(s14), kind == SCarModelLoaded)
		}
		return
	}
	if !w.Enabled || !s.joined || w.flooding(p, s) {
		return
	}
	m, now := w.Manager, w.host.Now()*1000.0
	if radiusKinds[kind] {
		m.SetRadius(p, signed14(s14))
	}
	switch kind {
	case SRequestTraffic:
		m.SetUniverse(p, w.host.Universe(p))
		n := a
		if a == 0xFF {
			n = 1
		}
		if n < 1 {
			n = 1
		}
		m.Request(p, n)
	case SWithdrawRequest:
		m.Withdraw(p)
	case STrafficSpawned:
		s.grantedAt.Delete(a)
		if m.Spawned(p, a, signed14(s14), &CarData{Struct: [5]int{a, s14, u5, b, flag}, Order: map[int]int{}}, now) {
			c := defCM()
			c.A = a
			w.push(p, CTakeOwnership, c)
		} else {
			w.host.Log(fmt.Sprintf("traffic: %d spawned car %d it was not granted; ignored (%s)", p.PortNumber(), a, m.Refusal))
		}
	case STrafficRemoved:
		m.Removed(p, a)
	case SRequestDespawn:
		m.Despawn(p, a, now)
	case SCanTakeOwnership:
		m.CanTake(p, a)
	case SRequestOwnership:
		if m.RequestOwnership(p, a, now) {
			w.host.Log(fmt.Sprintf("traffic: car %d handed to %d (RequestOwnership)", a, p.PortNumber()))
		} else if m.Resent {
			if ok, skipped := rate.Allow(fmt.Sprint("resent", p.PortNumber()), IgnoredLogSeconds); ok {
				extra := ""
				if skipped > 0 {
					extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
				}
				log.Debug(fmt.Sprintf("%d asked for car %d, which the server has as its own: TakeOwnership sent again%s", p.PortNumber(), a, extra))
			}
		}
	case SCarModelLoaded:
		m.ModelLoaded(p, signed14(s14))
		if s.reportAt == nil {
			t := w.host.Now()
			s.reportAt = &t
		}
		t := w.host.Now()
		s.burstAt = &t
		s.burstN++
	case SCarModelUnloaded:
		m.ModelUnloaded(p, signed14(s14))
	}
}

// OnUpdate handles FullSync / RouteProgress / FullRoute / Stationary from the car's owner: copy to the others who hold it.
func (w *World[P]) OnUpdate(p P, mtype int, r *bits.Reader) error {
	layout := updateLayout[mtype]
	fields := make([]uint64, len(layout))
	for i, width := range layout {
		v, err := r.Bits(width)
		if err != nil {
			return err
		}
		fields[i] = v
	}
	vid := int(fields[0])
	key := "rx_" + updateNames[mtype]
	w.count(key)
	n := w.Counters[key]
	if n <= 6 || n%500 == 0 {
		w.host.Log(fmt.Sprintf("traffic: %d -> %s car %d (#%d)", p.PortNumber(), updateNames[mtype], vid, n))
	}
	m := w.Manager
	car, _ := m.Cars.Get(vid)
	if !w.Enabled || car == nil || car.Owner != Key(p) || w.flooding(p, w.st(p)) {
		if w.Enabled && (car == nil || car.Owner != Key(p)) {
			if ok, skipped := rate.Allow(fmt.Sprint("stale", p.PortNumber()), IgnoredLogSeconds); ok {
				why := "no such car"
				if car != nil {
					why = fmt.Sprintf("owned by %v", name(car.Owner))
				}
				extra := ""
				if skipped > 0 {
					extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
				}
				log.Debug(fmt.Sprintf("%d %s for car %d ignored: %s%s", p.PortNumber(), updateNames[mtype], vid, why, extra))
			}
		}
		return nil
	}
	info := car.Info.(*CarData)
	if mtype == MsgServerFullSync || mtype == MsgServerFullRoute || mtype == MsgServerStationary {
		w.arrivals++
		info.Order[mtype] = w.arrivals
	}
	switch mtype {
	case MsgServerFullSync:
		info.FullSync = fields
	case MsgServerFullRoute:
		info.FullRoute = fields
	case MsgServerStationary:
		info.Stationary = fields
	}
	m.CarPosition(p, vid, UpdatePosition(mtype, fields))
	var msg netv2.Msg
	for _, q := range car.Holders {
		if q != Key(p) {
			if msg == nil {
				msg = w.host.Bake(UpdateMessage(mtype, fields))
			}
			w.emit(q.(P), msg)
		}
	}
	return nil
}

// Tick runs the timers; now is monotonic seconds.
func (w *World[P]) Tick(now float64) {
	if !w.Enabled || w.state.Len() == 0 || now < w.nextTick {
		return
	}
	w.nextTick = now + TickS
	m := w.Manager
	for _, p := range w.state.Keys() {
		s, ok := w.state.Get(p)
		if !ok {
			continue
		}
		if s.burstAt != nil && now-*s.burstAt > 0.3 {
			n := s.burstN
			s.burstN, s.burstAt = 0, nil
			if n >= 4 && now-s.joinedAt > 25.0 {
				w.host.Log(fmt.Sprintf("traffic: %d reported %d loaded models at once; repeating the join sequence", p.PortNumber(), n))
				w.Join(p, true)
			}
		}
		if m.Players.Has(p) {
			if pos, ok := p.CarPos(); ok {
				m.SetPosition(p, pos)
			}
			m.SetUniverse(p, w.host.Universe(p))
		}
		for _, vid := range s.grantedAt.Keys() {
			t, _ := s.grantedAt.Get(vid)
			if now-t > GrantTimeoutS {
				s.grantedAt.Delete(vid)
				w.host.Log(fmt.Sprintf("traffic: grant of car %d to %d timed out", vid, p.PortNumber()))
				m.WithdrawOne(p, vid)
				c := defCM()
				c.A = vid
				w.push(p, CSpawnCanceled, c)
			}
		}
	}
	type ps struct {
		p P
		s *peerState
	}
	var done []ps
	for _, p := range w.state.Keys() {
		s, _ := w.state.Get(p)
		if s != nil && s.joinUntil != nil && (now >= *s.joinUntil || w.modelsIn(p, s, now)) {
			done = append(done, ps{p, s})
		}
	}
	m.Tick(now * 1000.0)
	for _, d := range done {
		p, s := d.p, d.s
		pl, _ := m.Players.Get(p)
		var reported []int
		if pl != nil {
			for _, mod := range w.Models {
				if _, ok := pl.Models[mod]; ok {
					reported = append(reported, mod)
				}
			}
			sort.Ints(reported)
			reported = dedup(reported)
		}
		ready := w.modelsIn(p, s, now)
		if len(reported) > 0 || !ready {
			held := 0
			if pl != nil {
				held = len(pl.Held)
			}
			tail := ""
			if !ready {
				why := "no model report"
				if pl != nil && pl.Pos == nil {
					why = "no position"
				}
				tail = " (gave up waiting: " + why + ")"
			}
			w.host.Log(fmt.Sprintf("traffic: %d InitialJoinComplete %s s after the join, %d/%d models with cars on the client, %d cars sent%s",
				p.PortNumber(), fmt.Sprintf("%.1f", now-s.joinedAt), len(reported), len(w.Models), held, tail))
			rep := "none"
			if len(reported) > 0 {
				ss := make([]string, len(reported))
				for i, v := range reported {
					ss[i] = strconv.Itoa(v)
				}
				rep = strings.Join(ss, " ")
			}
			extra := ""
			if pl != nil {
				known := map[int]bool{}
				for _, mod := range w.Models {
					known[mod] = true
				}
				var un []int
				for mod := range pl.Models {
					if !known[mod] {
						un = append(un, mod)
					}
				}
				sort.Ints(un)
				if len(un) > 0 {
					ss := make([]string, len(un))
					for i, v := range un {
						ss[i] = strconv.Itoa(v)
					}
					extra = "; also unannounced " + strings.Join(ss, " ")
				}
			}
			log.Debug(fmt.Sprintf("%d reported models: %s%s", p.PortNumber(), rep, extra))
		}
		s.joinUntil = nil
		w.push(p, CInitialJoinComplete, defCM())
	}
	if now >= w.nextSummary {
		w.nextSummary = now + SummaryS
		w.logSummary()
	}
}

func dedup(a []int) []int {
	var out []int
	for i, v := range a {
		if i == 0 || v != a[i-1] {
			out = append(out, v)
		}
	}
	return out
}

func (w *World[P]) modelsIn(p P, s *peerState, now float64) bool {
	pl, ok := w.Manager.Players.Get(p)
	if !ok || pl.Pos == nil {
		return false
	}
	all := true
	for _, model := range w.Models {
		if _, ok := pl.Models[model]; !ok {
			all = false
			break
		}
	}
	if all {
		return true
	}
	if s == nil {
		s, _ = w.state.Get(p)
	}
	if s == nil {
		return false
	}
	if s.reportAt != nil {
		return now-*s.reportAt >= ReportSettleS
	}
	return now-s.joinedAt >= NoReportS
}

func (w *World[P]) logSummary() {
	m := w.Manager
	if m.Players.Len() == 0 {
		return
	}
	sent := map[string]int{}
	var keys []string
	for _, k := range w.counterKeys {
		if !strings.HasPrefix(k, "rx_") {
			sent[k] = w.Counters[k]
			keys = append(keys, k)
		}
	}
	delta := map[string]int{}
	for k, v := range sent {
		if v != w.summarySent[k] {
			delta[k] = v - w.summarySent[k]
		}
	}
	w.summarySent = sent
	shared := 0
	m.Cars.Each(func(_ int, c *Car) {
		if len(c.Holders) >= 2 {
			shared++
		}
	})
	var people []string
	m.Players.Each(func(k Key, p *Player) {
		owns := 0
		m.Cars.Each(func(_ int, c *Car) {
			if c.Owner == k {
				owns++
			}
		})
		ready := 0
		for v := range p.Held {
			if p.CanTake.Has(v) {
				ready++
			}
		}
		nopos := ""
		if p.Pos == nil {
			nopos = " no-pos"
		}
		people = append(people, fmt.Sprintf("%v u%d r%d%s held %d owns %d ready %d granted %d", name(k), p.Universe, p.Radius, nopos, len(p.Held), owns, ready, len(p.Granted)))
	})
	dk := make([]string, 0, len(delta))
	for k := range delta {
		dk = append(dk, k)
	}
	sort.Strings(dk)
	parts := make([]string, len(dk))
	for i, k := range dk {
		parts[i] = fmt.Sprintf("%s %d", k, delta[k])
	}
	tail := "nothing"
	if len(parts) > 0 {
		tail = strings.Join(parts, ", ")
	}
	w.host.Log(fmt.Sprintf("traffic: %d cars, %d held by 2+; %s; sent %s", m.Cars.Len(), shared, strings.Join(people, "; "), tail))
}
