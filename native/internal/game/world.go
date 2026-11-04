package game

import (
	"fmt"
	"math"
	"os"
	"sort"
	"strings"
	"sync"
	"time"

	"fh2mp/internal/bits"
	"fh2mp/internal/bucketlist"
	"fh2mp/internal/capacity"
	"fh2mp/internal/catalog"
	"fh2mp/internal/diag"
	"fh2mp/internal/eventlog"
	"fh2mp/internal/gameopts"
	"fh2mp/internal/netv2"
	"fh2mp/internal/omap"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/traffic"
)

// Clock is the time source of the engine: monotonic seconds (time.monotonic) and wall-clock seconds (time.time).
type Clock interface {
	Mono() float64
	Wall() float64
}

// RealClock is the process clock.
type RealClock struct{ start time.Time }

// NewRealClock starts a clock at zero.
func NewRealClock() *RealClock { return &RealClock{time.Now()} }

// Mono is monotonic seconds since the clock started plus a large offset so that values never look like "unset" zero.
func (c *RealClock) Mono() float64 { return 1000 + time.Since(c.start).Seconds() }

// Wall is unix seconds.
func (c *RealClock) Wall() float64 { return float64(time.Now().UnixNano()) / 1e9 }

// PacketConn sends a datagram to a client address (UDP socket or TCP tunnel mux).
type PacketConn interface {
	SendTo(data []byte, addr Addr) error
}

// Sinks are the outward calls of a world: they leave the game loop (HTTP to the LSP).
type Sinks struct {
	Result   func(mode string, detail pyjson.Obj, log func(string))
	Notice   func(xuid uint64, text string, log func(string))
	Gamemode func(mode int, xuids []uint64) (name, template string, err error)
}

// LogOut writes log lines: one file shared by every world.
type LogOut struct {
	mu  sync.Mutex
	w   func(string) error
	day int
}

// NewLogOut writes through w (nil = stdout).
func NewLogOut(w func(string) error) *LogOut {
	if w == nil {
		w = func(s string) error { _, err := fmt.Print(s); return err }
	}
	return &LogOut{w: w, day: -1}
}

// Write writes one line (level word for non-INFO, tag, scrubbed message) with a date line at the start and at every midnight.
func (l *LogOut) Write(now time.Time, level int, tag, msg string) {
	word := ""
	if level != diag.Info {
		word = diag.LevelName(level) + " "
	}
	line := fmt.Sprintf("%s %s%s%s", now.Format("15:04:05"), word, tag, diag.Scrub(msg))
	l.mu.Lock()
	defer l.mu.Unlock()
	if now.YearDay() != l.day {
		l.day = now.YearDay()
		zone, _ := now.Zone()
		line = fmt.Sprintf("%s date %s %s\n", now.Format("15:04:05"), now.Format("2006-01-02"), zone) + line
	}
	diag.WriteLine(l.w, line+"\n")
}

// Rand is the random source (Python's random module).
type Rand interface {
	Intn(n int) int
	Float64() float64
}

func randrange(r Rand, n int) int          { return r.Intn(n) }
func uniform(r Rand, a, b float64) float64 { return a + (b-a)*r.Float64() }

// choices is random.choices(population, weights)[0].
func choices(r Rand, n int, weights []float64) int {
	total := 0.0
	for _, w := range weights {
		total += w
	}
	x := r.Float64() * total
	acc := 0.0
	for i, w := range weights {
		acc += w
		if x < acc {
			return i
		}
	}
	return n - 1
}

func shuffle[T any](r Rand, a []T) {
	for i := len(a) - 1; i > 0; i-- {
		j := r.Intn(i + 1)
		a[i], a[j] = a[j], a[i]
	}
}

func sample[T any](r Rand, a []T, k int) []T {
	cp := append([]T(nil), a...)
	shuffle(r, cp)
	if k > len(cp) {
		k = len(cp)
	}
	return cp[:k]
}

// PendingRace is (deadline, leader).
type PendingRace struct {
	Deadline float64
	Leader   *Peer
}

// Tag is the tag-mode state of an event.
type Tag struct {
	Mode     *TagMode
	Buckets  *omap.Map[*Peer, int]
	Assigned bool
	AssignAt *float64
	Repeats  []float64
	Guessed  bool
	Seconds  float64
	First    []uint64
	SentAt   *float64
	EndAt    *float64
	GoAt     *float64
	Marked   *float64
	Held     *omap.Map[uint64, float64]
	Hits     *omap.Map[uint64, int]
}

// Event is the online event being set up or raced (the Python dict `event`).
type Event struct {
	People               []*Peer
	Begun                bool
	StartAt              *float64
	Started              float64
	NumLaps              *int64
	CarRule              *gameopts.Rule
	Team                 *omap.Map[*Peer, int]
	Order                []*Peer
	Tag                  *Tag
	Racing               bool
	Over                 bool
	Finished             *omap.Map[*Peer, float64]
	Quit                 map[*Peer]bool
	Progress             map[*Peer]float64
	ProgressAt           float64
	Laps                 *omap.Map[*Peer, int]
	LoadedAfter          *omap.Map[*Peer, float64]
	LoadChecked          bool
	LoadNext             float64
	GridAt, GridReportAt float64
	GridReadyAfter       *omap.Map[*Peer, float64]
	GoAt                 *float64
	GoTime               float64
	LeftBehind           map[*Peer]bool
	ClearAt              *float64
	RacingSince          float64
}

// Gm is the Car Meet game-mode invite state.
type Gm struct {
	Type    int
	Phase   string
	End     int64
	Starter *Peer
	Joined  *omap.Map[*Peer, int]
	Session *[2]string
	Reset   int64
	Snap    int64
}

// RtCand is the Road Trip vote: 3 locations, 4 events each, a class per location, minimum players.
type RtCand struct {
	Locs    [3]int64
	Events  [3][4]int64
	Classes [3]int
	Mins    [3][4]int
}

type recentRow struct {
	t      float64
	kind   string
	owner  int
	detail string
}

type dropRow struct {
	t      float64
	owner  int
	xuid   uint64
	lastRx float64
	pos    *[3]float64
	car    *int
	why    string
}

type rxState struct {
	peer  *Peer
	size  int
	types []int
}

// World is one lobby: the peers of one world with their own race / Road Trip state (= fh2mp_server.Server).
type World struct {
	Tag      string
	Out      *LogOut
	Clk      Clock
	Rng      Rand
	Sock     PacketConn
	Events   *eventlog.Log
	Sinks    *Sinks
	Cat      func() *catalog.Catalog
	Hub      *Hub
	HubKey   *WorldKey
	IsIdle   bool
	Carmeet  bool
	Roadtrip bool

	VisibilityChecked               float64
	RtState                         int
	RtEndMs                         int64
	RtVotes                         *omap.Map[*Peer, int]
	RtWinner                        int
	RtCfgDeadline, RtDriveNotBefore float64
	RtDriveWait                     bool
	RtDriveDeadline, RtDriveReadyAt float64
	RtIdx                           int
	RtLastRace                      *int64
	RtLastLoc                       *int64
	RtDriveMs                       int64
	RtPendingDrive                  bool
	RtPicked                        map[*Peer]bool
	RtConfigured                    bool
	RtGamedb                        string
	RtCandidates                    *RtCand
	RtPickPlayers                   int
	Gm                              Gm
	IntroduceAt                     string
	RtSeq                           int
	ServerCounter                   uint64
	Start                           float64
	ClockOffsetMs                   int64
	RaceHardDeadline                float64

	Traffic                   *traffic.World[*Peer]
	Coop                      *bucketlist.Coop
	rx                        rxState
	LeftoverLogged, BigLogged int
	MtSeen                    map[int]int
	MtNext                    float64
	recent                    []recentRow
	drops                     []dropRow
	WipeAt, WipeLast          float64
	Wx                        *netv2.Weather
	WxLevel                   int
	WxNext                    float64
	Peers                     *omap.Map[Addr, *Peer]
	Bots                      []*Peer
	BotSerial                 int
	BotsNext                  float64
	Dirty                     *omap.Map[*Peer, struct{}]
	OwnerQuarantine           map[int]float64
	PendingRace               *PendingRace
	RaceRequestedAt           float64
	GameOptions               *gameopts.Options
	GameOptionsOwner          *Peer
	LeaderSince               float64
	Leader                    *Peer
	FreeRoamLaps              int64
	FreeRoamEndCondition      int64
	Event                     *Event

	raceClock    [2]int64
	raceProgress struct {
		done int
		frac float64
	}
	GoptsEchoDropped int
	noiseAt          float64
	rate             *diag.RateLimit
	AutoWorlds       int
}

// NewWorld is Server(...) for a hub-owned world.
func NewWorld(tag string, out *LogOut, clk Clock, rng Rand, sock PacketConn, events *eventlog.Log, sinks *Sinks, cat func() *catalog.Catalog,
	roadtrip, carmeet bool, port int) (*World, error) {
	w := &World{Tag: tag, Out: out, Clk: clk, Rng: rng, Sock: sock, Events: events, Sinks: sinks, Cat: cat, Roadtrip: roadtrip, Carmeet: carmeet,
		RtState: RTInitialising, RtVotes: omap.New[*Peer, int](), RtWinner: -1, RtDriveMs: RTDriveMs, RtPicked: map[*Peer]bool{},
		Gm:            Gm{Phase: "idle", Joined: omap.New[*Peer, int]()},
		ServerCounter: 1, MtSeen: map[int]int{}, Peers: omap.New[Addr, *Peer](), Dirty: omap.New[*Peer, struct{}](),
		OwnerQuarantine: map[int]float64{}, FreeRoamLaps: 1, FreeRoamEndCondition: 1, RtSeq: 1, rate: diag.NewRateLimit(clk.Mono, 0)}
	w.IntroduceAt = strings.ToLower(strings.TrimSpace(envStr("FH2MP_INTRODUCE_AT", "handshake")))
	ok := false
	for _, p := range IntroducePoints {
		ok = ok || p == w.IntroduceAt
	}
	if !ok {
		return nil, fmt.Errorf("FH2MP_INTRODUCE_AT=%q: expected one of %s", w.IntroduceAt, strings.Join(IntroducePoints, ", "))
	}
	now := clk.Mono()
	w.Start = now
	w.MtNext = now + 30
	w.LastKeepaliveInit()
	off := int64(envInt("FH2MP_CLOCK_OFFSET_MS", -1))
	if os.Getenv("FH2MP_CLOCK_OFFSET_MS") == "" || off < 0 {
		off = int64(randrange(rng, ClockOffsetMaxMs))
	}
	w.ClockOffsetMs = off
	w.Traffic = traffic.NewWorld[*Peer](&trafficHost{w}, nil)
	w.Coop = bucketlist.NewCoop(w.blKey, func() uint64 { return uint64(w.NowMs()) }, w.Clk.Mono)
	w.weatherInit()
	scale := 48 * (off / 1000)
	w.Logf("listening on udp/%d version=%s game=%s clock offset %d ms (time of day at start %02d:%02d if the time scale is 48)",
		port, netv2.VersionString, netv2.GameString, off, scale%86400/3600, scale%3600/60)
	return w, nil
}

// LastKeepaliveInit is a no-op hook kept for symmetry with Python (peers carry their own timers).
func (w *World) LastKeepaliveInit() {}

// Mono is monotonic seconds.
func (w *World) Mono() float64 { return w.Clk.Mono() }

// NowMs is the world's network clock in milliseconds (32 bit).
func (w *World) NowMs() int64 {
	return (int64((w.Clk.Mono()-w.Start)*1000) + w.ClockOffsetMs) & 0xFFFFFFFF
}

// ---- logging ----

func (w *World) logAt(level int, sub, msg string) {
	if !Verbosity.Enabled(level, sub) {
		return
	}
	if w.Out == nil {
		return
	}
	w.Out.Write(time.Now(), level, w.Tag, msg)
}

// Log writes an INFO line.
func (w *World) Log(msg string) { w.logAt(diag.Info, "", msg) }

// Logf writes an INFO line.
func (w *World) Logf(format string, a ...any) { w.logAt(diag.Info, "", fmt.Sprintf(format, a...)) }

// Warn writes a WARN line.
func (w *World) Warn(msg string) { w.logAt(diag.Warn, "", msg) }

// Warnf writes a WARN line.
func (w *World) Warnf(format string, a ...any) { w.logAt(diag.Warn, "", fmt.Sprintf(format, a...)) }

// Error writes an ERROR line.
func (w *World) Error(msg string) { w.logAt(diag.Error, "", msg) }

// Debug writes a DEBUG line of a subsystem.
func (w *World) Debug(sub, msg string) { w.logAt(diag.Debug, sub, msg) }

// Debugf writes a DEBUG line of a subsystem.
func (w *World) Debugf(sub, format string, a ...any) {
	w.logAt(diag.Debug, sub, fmt.Sprintf(format, a...))
}

// Trace writes a TRACE line of a subsystem.
func (w *World) Trace(sub, msg string) { w.logAt(diag.Trace, sub, msg) }

// Exc logs a recovered failure: what was being done, then the error.
func (w *World) Exc(msg string, err any, level int) {
	w.logAt(level, "", fmt.Sprintf("%s: %s", msg, diag.Scrub(fmt.Sprintf("%T: %v", err, err))))
}

// LogEvery writes a repeating condition once per interval seconds per key, saying how many it stood in for.
func (w *World) LogEvery(key any, interval float64, msg string, level int) {
	if ok, skipped := rate.Allow([2]any{w.Tag, key}, interval); ok {
		if skipped > 0 {
			msg += fmt.Sprintf(" (and %d more in the last %.0f s)", skipped, interval)
		}
		w.logAt(level, "", msg)
	}
}

var rate = diag.NewRateLimit(nil, 0)

// ExcEvery is Exc for a failure that can repeat on every tick or packet.
func (w *World) ExcEvery(key any, interval float64, msg string, err any, level int) {
	if ok, skipped := rate.Allow([2]any{w.Tag, key}, interval); ok {
		if skipped > 0 {
			msg += fmt.Sprintf(" (and %d more in the last %.0f s)", skipped, interval)
		}
		w.Exc(msg, err, level)
	}
}

// ev writes a line in events.jsonl: the world is named by its log tag.
func (w *World) ev(kind string, fields ...pyjson.Pair) {
	if w.Events == nil {
		return
	}
	world := strings.Trim(w.Tag, " []")
	kindOf := strings.SplitN(world, ":", 2)[0]
	if kindOf == "" {
		kindOf = "idle"
	}
	all := []pyjson.Pair{{K: "world", V: world}, {K: "world_kind", V: kindOf}}
	var xuid any
	for _, f := range fields {
		if f.K == "xuid" {
			xuid = f.V
		}
	}
	if run := w.runOf(xuid); run != "" {
		fields = append(fields, pyjson.Pair{K: "run", V: run})
	}
	w.Events.Emit(kind, append(all, fields...)...)
}

func (w *World) runOf(xuid any) string {
	if w.Hub == nil || xuid == nil {
		return ""
	}
	switch x := xuid.(type) {
	case string:
		var v uint64
		if _, err := fmt.Sscanf(strings.TrimPrefix(x, "0x"), "%x", &v); err != nil || v == 0 {
			return ""
		}
		return w.Hub.Runs[v]
	case uint64:
		if x == 0 {
			return ""
		}
		return w.Hub.Runs[x]
	}
	return ""
}

func (w *World) note(kind string, owner int, detail string) {
	w.recent = append(w.recent, recentRow{w.Mono(), kind, owner, detail})
	if len(w.recent) > 2500 {
		w.recent = w.recent[len(w.recent)-2500:]
	}
}

// ---- weather ----

var weatherLevels = [4]float64{0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0}

const (
	weatherRampMs    = 165000
	weatherRainStart = 0.603
)

var (
	weatherWeights = [][2]float64{{1, 0.45}, {2, 0.35}, {3, 0.20}}
	weatherHoldS   = [2]float64{90.0, 180.0}
)

func (w *World) weatherPickLevel(current int) int {
	var levels []int
	var ws []float64
	for _, lw := range weatherWeights {
		if int(lw[0]) != current {
			levels = append(levels, int(lw[0]))
			ws = append(ws, lw[1])
		}
	}
	return levels[choices(w.Rng, len(levels), ws)]
}

func (w *World) weatherPresets() [4]int64 {
	c := int64(3)
	if w.Rng.Intn(2) == 1 {
		c = 5
	}
	return [4]int64{0, 1, c, 7}
}

func (w *World) weatherHold() float64 {
	scale := envFloat("FH2MP_WEATHER_SCALE", 1.0)
	return uniform(w.Rng, weatherHoldS[0], weatherHoldS[1]) * scale
}

func (w *World) weatherInit() {
	level := -1
	if v := os.Getenv("FH2MP_WEATHER_START"); v != "" {
		level = envInt("FH2MP_WEATHER_START", 0)
	} else {
		level = w.weatherPickLevel(-1)
	}
	now := int64(w.NowMs())
	rain := 0.0
	if level == 3 {
		rain = 1.0
	}
	w.WxLevel = level
	w.Wx = &netv2.Weather{ACur: weatherLevels[level], ADes: weatherLevels[level], PCur: rain, PDes: rain,
		ACurTick: now, ADesTick: now, PCurTick: now, PDesTick: now, Presets: w.weatherPresets()}
	w.WxNext = w.Mono() + w.weatherHold()
	w.Logf("weather: start level %d (%.3f), presets %s", level, weatherLevels[level], intList(w.Wx.Presets[:]))
}

func intList[T ~int | ~int64](a []T) string {
	parts := make([]string, len(a))
	for i, v := range a {
		parts[i] = fmt.Sprint(v)
	}
	return "[" + strings.Join(parts, ", ") + "]"
}

func (w *World) anyConnected() bool {
	for _, p := range w.Peers.Values() {
		if p.Connected {
			return true
		}
	}
	return false
}

func (w *World) weatherTick(now float64) {
	if now < w.WxNext || !w.anyConnected() {
		return
	}
	wx := w.Wx
	t := w.NowMs()
	span := (wx.ADesTick - wx.ACurTick) & 0xFFFFFFFF
	done := (t - wx.ACurTick) & 0xFFFFFFFF
	var cur float64
	if span == 0 || done >= span {
		cur = wx.ADes
	} else {
		cur = wx.ACur + (wx.ADes-wx.ACur)*float64(done)/float64(span)
	}
	level := w.weatherPickLevel(w.WxLevel)
	target := weatherLevels[level]
	rampMs := int64(math.Abs(target-cur) * weatherRampMs)
	aEnd := t + rampMs
	pNow := wx.PCur
	if wx.PDesTick == wx.PCurTick {
		pNow = wx.PDes
	}
	var pDes float64
	var pStart, pEnd int64
	if level == 3 {
		pDes = 1.0
		pStart = t + maxI64(0, int64((weatherRainStart-cur)*weatherRampMs))
	} else {
		pDes = 0.0
		pStart = t
	}
	if level == 3 {
		pEnd = aEnd
	} else {
		pEnd = t + maxI64(1000, int64(math.Max(0.0, cur-weatherRainStart)*weatherRampMs))
	}
	if pDes == pNow {
		pStart, pEnd = t, t
	}
	w.WxLevel = level
	wx.Presets = w.weatherPresets()
	wx.ACur, wx.ACurTick, wx.ADes, wx.ADesTick = cur, t, target, aEnd
	wx.PCur, wx.PCurTick, wx.PDes, wx.PDesTick = pNow, pStart, pDes, pEnd
	w.WxNext = now + float64(rampMs)/1000.0 + w.weatherHold()
	w.Logf("weather: change to level %d (%.3f) from %.3f, ramp %.0f s, rain %.2f -> %.2f, presets %s, next change in %.0f s",
		level, target, cur, float64(rampMs)/1000, pNow, pDes, intList(wx.Presets[:]), w.WxNext-now)
	for _, q := range w.Peers.Values() {
		if q.Connected && q.WxReady {
			q.WxSeq = (q.WxSeq + 1) % 0xFF
			w.Send(q, false, netv2.WeatherUpdate(uint64(q.WxSeq), wx))
		}
	}
}

func maxI64(a, b int64) int64 {
	if a > b {
		return a
	}
	return b
}

// ---- sending ----

// Send queues messages for one client; Flush sends them.
func (w *World) Send(peer *Peer, fast bool, msgs ...netv2.Msg) {
	baked := make([]netv2.Baked, len(msgs))
	for i, m := range msgs {
		baked[i] = netv2.Bake(m)
	}
	w.SendBaked(peer, fast, baked...)
}

// SendBaked queues already baked messages.
func (w *World) SendBaked(peer *Peer, fast bool, msgs ...netv2.Baked) {
	now := w.Mono()
	peer.Outbox = append(peer.Outbox, msgs...)
	if fast {
		peer.Due = math.Min(peer.Due, now+RelayFlushSecs)
	} else {
		peer.Due = math.Min(peer.Due, now)
		peer.Hold = math.Max(peer.Hold, peer.LastRx+ReplyHoldSeconds)
	}
	if cur, ok := w.Peers.Get(peer.Addr); ok && cur == peer {
		w.Dirty.Set(peer, struct{}{})
	} else {
		w.FlushPeer(peer)
	}
}

func (w *World) people() []*Peer {
	if w.Event == nil {
		return nil
	}
	return w.Event.People
}

func inList(l []*Peer, p *Peer) bool {
	for _, q := range l {
		if q == p {
			return true
		}
	}
	return false
}

func removeFrom(l []*Peer, p *Peer) []*Peer {
	for i, q := range l {
		if q == p {
			return append(l[:i:i], l[i+1:]...)
		}
	}
	return l
}

// Relay sends one message to everybody else in the sender's world (the hot path).
func (w *World) Relay(sender *Peer, msg netv2.Msg, sameEvent bool) {
	baked := netv2.Bake(msg)
	due := w.Mono() + RelayFlushSecs
	var people []*Peer
	if sameEvent && w.Event != nil {
		people = w.Event.People
	}
	inside := inList(people, sender)
	for _, q := range w.Others(sender) {
		if inList(people, q) != inside {
			continue
		}
		q.Outbox = append(q.Outbox, baked)
		if due < q.Due {
			q.Due = due
		}
		w.Dirty.Set(q, struct{}{})
	}
}

// SameEvent says whether two peers are on the same side of the event boundary.
func (w *World) SameEvent(a, b *Peer) bool {
	people := w.people()
	return inList(people, a) == inList(people, b)
}

// RelayPrediction relays a client's cached (or given) car prediction to the others, phasing it across the event boundary.
func (w *World) RelayPrediction(sender *Peer, refresh bool, pred *Prediction) {
	if refresh && !sender.CarPredictionRefreshable {
		return
	}
	if pred == nil {
		pred = sender.CarPrediction
	}
	if pred == nil {
		return
	}
	cid, body, size := pred.ID, pred.Body, pred.Size
	if size != netv2.CarPredictionBodyBits {
		return
	}
	stampOffset := netv2.CarPredictionBodyBits - 32
	stamp := body.Rsh(stampOffset).Uint64()
	previous := sender.CarPredictionStamp
	if refresh && previous != nil {
		stamp = (*previous + 1) & 0xFFFFFFFF
		body = body.Low(stampOffset).Or(bits.FromUint64(stamp).Lsh(stampOffset))
	}
	if previous == nil || (func() bool { d := (stamp - *previous) & 0xFFFFFFFF; return d > 0 && d < 0x80000000 })() {
		s := stamp
		sender.CarPredictionStamp = &s
	}
	visible := netv2.Bake(netv2.RelayCarPrediction(uint64(cid), uint64(sender.Owner), body, size))
	var hidden *netv2.Baked
	due := w.Mono() + RelayFlushSecs
	people := w.people()
	inside := inList(people, sender)
	for _, q := range w.Others(sender) {
		msg := visible
		if inList(people, q) != inside {
			if sender.PrivateUniverse == nil {
				continue
			}
			if hidden == nil {
				mask := bits.FromUint64(31).Lsh(netv2.CarUniverseOffset)
				phased := andNot(body, mask).Or(bits.FromUint64(uint64(*sender.PrivateUniverse)).Lsh(netv2.CarUniverseOffset))
				h := netv2.Bake(netv2.RelayCarPrediction(uint64(cid), uint64(sender.Owner), phased, size))
				hidden = &h
			}
			msg = *hidden
		}
		q.Outbox = append(q.Outbox, msg)
		if due < q.Due {
			q.Due = due
		}
		w.Dirty.Set(q, struct{}{})
	}
	if len(w.Bots) > 0 && !refresh && sender.BotSource == nil {
		w.botFollow(sender, pred)
	}
}

func andNot(x, mask bits.Int) bits.Int {
	out := make([]uint64, len(x))
	copy(out, x)
	for i := range out {
		if i < len(mask) {
			out[i] &^= mask[i]
		}
	}
	n := len(out)
	for n > 0 && out[n-1] == 0 {
		n--
	}
	return bits.Int(out[:n])
}

// RefreshCarPhasing rephases stationary cars too.
func (w *World) RefreshCarPhasing() {
	for _, q := range w.Peers.Values() {
		if q.Connected && q.Entered && q.KickReason == nil {
			w.RelayPrediction(q, true, nil)
		}
	}
}

// FlushPeer sends everything queued for a client.
func (w *World) FlushPeer(peer *Peer) {
	out := peer.Outbox
	peer.Outbox = nil
	peer.Due, peer.Hold = math.Inf(1), 0.0
	w.Dirty.Delete(peer)
	ack, ackBits := peer.AckFields()
	limit := MaxPacketBytes*8 - netv2.HeaderBits
	reliableNow := Reliable && peer.Connected
	now := 0.0
	if reliableNow {
		now = w.Mono()
	}
	i := 0
	for {
		j, size := i, 0
		for j < len(out) && j-i < 255 && (j == i || size+out[j].N <= limit) {
			size += out[j].N
			j++
		}
		pid := uint16(netv2.UnreliableID)
		if reliableNow {
			pid = peer.Window.Stamp(out[i:j], now)
		}
		if err := w.Sock.SendTo(netv2.PackMessages(out[i:j], pid, uint16(ack), ackBits), peer.Addr); err != nil {
			w.Noise(fmt.Sprintf("%d send failed: %v", peer.Addr.Port, err), diag.Info)
		}
		i = j
		if i >= len(out) {
			return
		}
	}
}

// Flush sends every queue that is due; returns when the next one will be.
func (w *World) Flush(now float64) float64 {
	nxt := math.Inf(1)
	for _, peer := range w.Dirty.Keys() {
		at := math.Max(peer.Due, peer.Hold)
		if at <= now {
			w.FlushPeer(peer)
		} else {
			nxt = math.Min(nxt, at)
		}
	}
	return nxt
}

// Others are the other connected, entered clients of the same world.
func (w *World) Others(peer *Peer) []*Peer {
	var out []*Peer
	for _, q := range w.Peers.Values() {
		if q != peer && q.Connected && q.Entered && sameWorld(q.World, peer.World) {
			out = append(out, q)
		}
	}
	return out
}

func sameWorld(a, b *WorldKey) bool {
	if a == nil || b == nil {
		return a == b
	}
	return *a == *b
}

// Audience: who gets peer's object messages (others plus anyone introduced before entering).
func (w *World) Audience(peer *Peer, router int) []*Peer {
	early := !EntryOnlyRouters[router]
	var out []*Peer
	for _, q := range w.Peers.Values() {
		if q != peer && q.Connected && (q.Entered || (q.Introduced && early)) && sameWorld(q.World, peer.World) {
			out = append(out, q)
		}
	}
	return out
}

// ---- owner ids ----

// OwnerIDsTaken: relay owner ids that cannot be handed out now.
func (w *World) OwnerIDsTaken() map[int]bool {
	now := w.Mono()
	for owner, until := range w.OwnerQuarantine {
		if until <= now {
			delete(w.OwnerQuarantine, owner)
		}
	}
	taken := map[int]bool{}
	for _, p := range w.Peers.Values() {
		if p.Owner != 0 {
			taken[p.Owner] = true
		}
	}
	for _, b := range w.Bots {
		taken[b.Owner] = true
	}
	for o := range w.OwnerQuarantine {
		taken[o] = true
	}
	return taken
}

// FreeOwner is the lowest relay owner id in 1..RelayOwnerLast nobody holds, or 0.
func (w *World) FreeOwner() int {
	taken := w.OwnerIDsTaken()
	for o := 1; o <= RelayOwnerLast; o++ {
		if !taken[o] {
			return o
		}
	}
	return 0
}

// Noise logs about datagrams from strangers at most once a second.
func (w *World) Noise(msg string, level int) {
	now := w.Mono()
	if now-w.noiseAt >= 1.0 || w.noiseAt == 0 {
		w.noiseAt = now
		w.logAt(level, "", msg)
	}
}

// Goodbye is CloseConnection with this connection's ids.
func (w *World) Goodbye(peer *Peer) netv2.Msg {
	return netv2.CloseConnection(0, peer.ClientCounter, w.ServerCounter)
}

// OthersConnected lists the other connected clients.
func (w *World) OthersConnected(peer *Peer) []*Peer {
	var out []*Peer
	for _, q := range w.Peers.Values() {
		if q != peer && q.Connected {
			out = append(out, q)
		}
	}
	return out
}

// PendingRoom reports whether another half-open connection from addr is allowed in this world.
func (w *World) PendingRoom(addr Addr) bool {
	total, mine := 0, 0
	for _, p := range w.Peers.Values() {
		if !p.Connected {
			total++
			if p.Addr.IP == addr.IP {
				mine++
			}
		}
	}
	return total < MaxPendingPeers && mine < MaxPendingPerIP
}

// Malformed counts a message no real client writes.
func (w *World) Malformed(peer *Peer, what string) {
	peer.MalformedCount++
	if !peer.MalformedLogged {
		peer.MalformedLogged = true
		t := w.rx.types
		if len(t) > 6 {
			t = t[len(t)-6:]
		}
		w.Warnf("%d malformed %s; dropped with the rest of its datagram (owner %d xuid %#x, %d bytes, message types in it so far %s); further ones from this client are only counted",
			peer.Addr.Port, what, peer.Owner, peer.Xuid, w.rx.size, intList(t))
	}
}

// TrafficUniverse keeps traffic on its side of an event boundary.
func (w *World) trafficUniverse(peer *Peer) int {
	if w.Event != nil && inList(w.Event.People, peer) {
		return 1
	}
	return 0
}

// trafficHost adapts the world to traffic.Host.
type trafficHost struct{ w *World }

func (h *trafficHost) Send(peer *Peer, msgs []netv2.Msg, fast bool) { h.w.Send(peer, fast, msgs...) }
func (h *trafficHost) Peers() []*Peer {
	var out []*Peer
	for _, p := range h.w.Peers.Values() {
		if p.Connected && p.Entered {
			out = append(out, p)
		}
	}
	return out
}
func (h *trafficHost) Log(s string)         { h.w.Log(s) }
func (h *trafficHost) Universe(p *Peer) int { return h.w.trafficUniverse(p) }
func (h *trafficHost) Now() float64         { return h.w.Mono() }
func (h *trafficHost) Bake(m netv2.Msg) netv2.Msg {
	b := netv2.Bake(m)
	return func(w *bits.Writer) { w.Raw(b.V, b.N) }
}

func sortedPeersByOwner(l []*Peer) []*Peer {
	out := append([]*Peer(nil), l...)
	sort.SliceStable(out, func(i, j int) bool { return out[i].Owner < out[j].Owner })
	return out
}

var _ = capacity.ClientLimit

// OpenAllowed asks the hub whether this OpenConnection passes the identity check, and never lets a different player id take over a connection that already opened.
func (w *World) OpenAllowed(playerID uint64, addr Addr) bool {
	if peer, ok := w.Peers.Get(addr); ok && peer.Xuid != 0 && peer.Xuid != playerID {
		w.Noise(fmt.Sprintf("%s:%d refused OpenConnection of %#x: this connection belongs to %#x", addr.IP, addr.Port, playerID, peer.Xuid), diag.Warn)
		return false
	}
	return w.Hub == nil || w.Hub.OpenAllowed(playerID, addr)
}
