package game

import (
	"fmt"
	"math"
	"strconv"
	"strings"

	"fh2mp/internal/bits"
	"fh2mp/internal/netv2"
	"fh2mp/internal/pyjson"
)

// Live telemetry for speed traps and speed zones. Every client streams its own car state (CarPredictionFull, docs/re/netv2.md 7)
// through this server so the other players can see the car; each peer keeps the last TelemWindowS seconds of that stream, at most one
// sample per TelemStepS, in a ring allocated at its first sample. When the LSP takes a trap or zone result it asks "telemetry <xuid>
// <trap|zone> <m/s>" on the control port and gets the verdict of that window. Nothing runs or is sent on the player's PC; a player
// who is not connected to this server at the time has no window ("no live data").
const (
	TelemWindowS = 90.0
	TelemStepS   = 0.05
	telemCap     = int(TelemWindowS/TelemStepS) + 8
	TelemVersion = 1001 // score_verdicts.version of a telemetry verdict (ghost verdicts use ghost.CheckVersion)

	TelemTrapS      = 60.0 // seconds of the window a trap result is judged on
	TelemFreshS     = 20.0 // the newest sample must be this recent, or the player was not driving here
	TelemSlackRatio = 1.05 // a trap/zone result may exceed the fastest recorded speed by this factor ...
	TelemSlackMS    = 1.5  // ... plus this much (m/s): quantisation, the 50 ms sampling, the trap's own timing
	TelemGearBit    = 0x35*8 - 16
	TelemBrakeBit   = 0x2c * 8
)

type telemSample struct {
	mono     float64 // server clock (s)
	stamp    uint32  // the car's own network clock (ms)
	pos      [3]float32
	speed    float32 // |velocity| from the car's state (m/s); NaN when an axis is saturated
	throttle uint8
	brake    uint8
	gear     uint8
	tele     uint8 // teleport counter (netv2.md 7.6)
}

// TelemRing is one peer's recent car states.
type TelemRing struct {
	buf  []telemSample
	next int
	n    int
}

// due says whether a sample taken at now (server clock) is kept: at most one per TelemStepS.
func (r *TelemRing) due(now float64) bool {
	return r.n == 0 || now-r.buf[(r.next-1+len(r.buf))%len(r.buf)].mono >= TelemStepS-1e-6 // clock sums are not exact
}

func (r *TelemRing) add(s telemSample) {
	if !r.due(s.mono) {
		return
	}
	if r.buf == nil {
		r.buf = make([]telemSample, telemCap)
	}
	r.buf[r.next] = s
	r.next = (r.next + 1) % len(r.buf)
	if r.n < len(r.buf) {
		r.n++
	}
}

// window is the samples newer than since, oldest first.
func (r *TelemRing) window(since float64) []telemSample {
	out := make([]telemSample, 0, r.n)
	for i := 0; i < r.n; i++ {
		s := r.buf[(r.next-r.n+i+len(r.buf))%len(r.buf)]
		if s.mono >= since {
			out = append(out, s)
		}
	}
	return out
}

// TrackTelem stores one whole prediction of the peer's car (called with the decoded position and the car's clock stamp).
func (w *World) TrackTelem(peer *Peer, rest bits.Int, pos [3]float64, stamp uint64) {
	now := w.Clk.Mono()
	if !peer.Telem.due(now) {
		return // between samples nothing is decoded
	}
	speed := float32(math.NaN())
	if v, ok := carVelocity(rest); ok {
		speed = float32(math.Sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]))
	}
	peer.Telem.add(telemSample{
		mono: now, stamp: uint32(stamp),
		pos:   [3]float32{float32(pos[0]), float32(pos[1]), float32(pos[2])},
		speed: speed, throttle: uint8(rest.Field(PushThrottleBit, 8)), brake: uint8(rest.Field(TelemBrakeBit, 8)),
		gear: uint8(rest.Field(TelemGearBit, 4)), tele: uint8(rest.Field(netv2.CarTeleportOffset, 2)),
	})
}

// TelemResult is the verdict on one trap or zone result.
type TelemResult struct {
	Verdict  string // pass, flag, hard, none (no live data)
	Reasons  []string
	Measures pyjson.Obj
}

// TelemJudge judges a trap ("trap", the speed at one point) or zone ("zone", an average) result in m/s against the samples, oldest
// first, ending at now (server clock).
func TelemJudge(samples []telemSample, kind string, value, now float64) TelemResult {
	if len(samples) < 10 || now-samples[len(samples)-1].mono > TelemFreshS {
		why := "the player was not driving on this server at the time"
		if len(samples) > 0 {
			why = fmt.Sprintf("the newest car state is %.0f s old", now-samples[len(samples)-1].mono)
		}
		return TelemResult{Verdict: "none", Reasons: []string{why}, Measures: pyjson.Obj{{K: "samples", V: len(samples)}}}
	}
	var maxState, maxPos float64
	jumps, teleports, inconsistent := 0, 0, 0.0
	// speed from positions over >= 0.5 s of the car's own clock, so one late packet does not decide
	j := 0
	for i := range samples {
		if s := float64(samples[i].speed); !math.IsNaN(s) {
			maxState = math.Max(maxState, s)
		}
		if i > 0 && samples[i].tele != samples[i-1].tele {
			teleports++
		}
		if i > 0 {
			dt := float64(uint32(samples[i].stamp-samples[i-1].stamp)) / 1000
			if dt > 0 && dt < 2 && dist32(samples[i].pos, samples[i-1].pos)/dt*3.6 > TeleportKmh && samples[i].tele == samples[i-1].tele {
				jumps++
			}
		}
		for j < i && float64(uint32(samples[i].stamp-samples[j+1].stamp))/1000 >= 0.5 {
			j++
		}
		dt := float64(uint32(samples[i].stamp-samples[j].stamp)) / 1000
		if j < i && dt >= 0.5 && dt <= 2 && samples[i].tele == samples[j].tele {
			ps := dist32(samples[i].pos, samples[j].pos) / dt
			if ps*3.6 <= TeleportKmh {
				maxPos = math.Max(maxPos, ps)
				// the car says it goes much faster than it moves (a velocity written into the state)
				st := float64(samples[i].speed)
				if !math.IsNaN(st) && st > 20 && st > 1.25*ps+3 {
					inconsistent += float64(uint32(samples[i].stamp-samples[i-1].stamp)) / 1000
				}
			}
		}
	}
	recorded := math.Max(maxState, maxPos)
	limit := recorded*TelemSlackRatio + TelemSlackMS
	what := "speed trap"
	if kind == "zone" {
		what = "speed zone average"
	}
	r := TelemResult{Verdict: "pass", Measures: pyjson.Obj{
		{K: "source", V: "telemetry"}, {K: "kind", V: kind}, {K: "samples", V: len(samples)},
		{K: "seconds", V: round1(samples[len(samples)-1].mono - samples[0].mono)},
		{K: "value_kmh", V: round1(value * 3.6)}, {K: "max_state_kmh", V: round1(maxState * 3.6)}, {K: "max_moved_kmh", V: round1(maxPos * 3.6)},
		{K: "teleports", V: teleports}, {K: "jumps", V: jumps}, {K: "inconsistent_s", V: round1(inconsistent)},
	}}
	if value > limit {
		r.Verdict = "hard"
		r.Reasons = append(r.Reasons, fmt.Sprintf("%s %.0f km/h, the car never went over %.0f km/h in the last %.0f s", what, value*3.6, recorded*3.6,
			samples[len(samples)-1].mono-samples[0].mono))
	}
	if jumps > 0 {
		r.Reasons = append(r.Reasons, fmt.Sprintf("%d position jump(s) over %.0f km/h without the game's teleport counter", jumps, TeleportKmh))
		if r.Verdict == "pass" {
			r.Verdict = "flag"
		}
	}
	if inconsistent >= 1 {
		r.Reasons = append(r.Reasons, fmt.Sprintf("for %.1f s the car's state said it was 25%% faster than it moved", inconsistent))
		if r.Verdict == "pass" {
			r.Verdict = "flag"
		}
	}
	return r
}

func dist32(a, b [3]float32) float64 {
	dx, dy, dz := float64(a[0]-b[0]), float64(a[1]-b[1]), float64(a[2]-b[2])
	return math.Sqrt(dx*dx + dy*dy + dz*dz)
}

func round1(v float64) float64 { return math.Round(v*10) / 10 }

// telemetryCommand answers "telemetry <xuid hex> <trap|zone> <m/s>": the verdict of that player's live window, logged in the player's
// world. The empty "lobbies" list is there for the control client's JSON query (xbl.UDPHub.Query).
func (h *Hub) telemetryCommand(parts []string) pyjson.Obj {
	reply := func(r TelemResult) pyjson.Obj {
		reasons := make([]any, 0, len(r.Reasons))
		for _, s := range r.Reasons {
			reasons = append(reasons, s)
		}
		return pyjson.Obj{{K: "lobbies", V: []any{}}, {K: "verdict", V: r.Verdict}, {K: "reasons", V: reasons}, {K: "measures", V: r.Measures}}
	}
	xuid, err := parseHex(parts[1])
	value, err2 := strconv.ParseFloat(parts[3], 64)
	kind := parts[2]
	if err != nil || err2 != nil || xuid == 0 || (kind != "trap" && kind != "zone") || math.IsNaN(value) || value < 0 {
		h.ctlIgnored(parts, fmt.Sprintf("telemetry message unreadable (%s)", strings.Join(parts[1:], " ")))
		return reply(TelemResult{Verdict: "none", Reasons: []string{"unreadable request"}})
	}
	var best *Peer
	var world *World
	for addr, b := range h.Bound {
		if b.xuid != xuid {
			continue
		}
		w, _ := h.Route.Get(addr)
		if w == nil {
			continue
		}
		if p, ok := w.Peers.Get(addr); ok && (best == nil || p.Telem.n > best.Telem.n) {
			best, world = p, w
		}
	}
	if best == nil {
		return reply(TelemResult{Verdict: "none", Reasons: []string{"the player was not connected to this server at the time"}})
	}
	now := world.Clk.Mono()
	span := TelemWindowS
	if kind == "trap" {
		span = TelemTrapS
	}
	r := TelemJudge(best.Telem.window(now-span), kind, value, now)
	why := "consistent with the car"
	if len(r.Reasons) > 0 {
		why = strings.Join(r.Reasons, "; ")
	}
	ms, _ := r.Measures.Get("max_state_kmh")
	mm, _ := r.Measures.Get("max_moved_kmh")
	n, _ := r.Measures.Get("samples")
	world.Logf("telemetry: owner %d xuid %#x %s result %.0f km/h -> %s: %s (car state max %v km/h, moved max %v km/h, %v samples)",
		best.Owner, best.Xuid, kind, value*3.6, r.Verdict, why, ms, mm, n)
	return reply(r)
}
