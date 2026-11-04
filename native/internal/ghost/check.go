package ghost

import (
	"encoding/binary"
	"fmt"
	"math"
	"sort"

	"fh2mp/internal/pyfmt"
	"fh2mp/internal/pyjson"
)

// Verdicts. HARD means the run cannot be real, so it is hidden automatically; FLAG only puts it in the review queue.
const (
	CheckVersion    = 1
	Pass            = "pass"
	Flag            = "flag"
	Hard            = "hard"
	EndTolerance    = 0.5  // s, run time against the last record
	HeaderTolerance = 0.05 // s, run time against the ghost's header
	MaxFrameGap     = 2.0  // s, the game records about 5 Hz
	MinFrames       = 8
	HighSpeed       = 40.0 // m/s, specific power is read above this
	MinHighSamples  = 10
	MaxSpeed        = 127.4 // m/s, fastest a car really goes
	TeleportSpeed   = 160.0 // m/s between two frames; one is a track reset, a run of them is not
	MaxTeleports    = 5
)

type limit struct {
	rule string
	v    float64
}

var hardLimits = []limit{{"speed", MaxSpeed}, {"acceleration", 30.0}, {"climb", 30.0}, {"power", 1000.0}}
var flagLimits = map[string]float64{"speed": 120.0, "acceleration": 20.0, "power": 700.0}

// Measures are the numbers the rules read.
type Measures struct {
	Frames      int
	First, Gap  float64
	Speed       float64
	Accel       float64
	Climb       float64
	Power       float64
	HighSamples int
	Teleports   int
	Last        float64
}

// Obj is the measurements as the Python dict stores them (same keys and order).
func (m *Measures) Obj() pyjson.Obj {
	return pyjson.Obj{{K: "frames", V: m.Frames}, {K: "first", V: m.First}, {K: "gap", V: m.Gap}, {K: "speed", V: m.Speed},
		{K: "acceleration", V: m.Accel}, {K: "climb", V: m.Climb}, {K: "power", V: m.Power}, {K: "high_samples", V: m.HighSamples},
		{K: "teleports", V: m.Teleports}, {K: "last", V: m.Last}}
}

func (m *Measures) get(rule string) float64 {
	switch rule {
	case "speed":
		return m.Speed
	case "acceleration":
		return m.Accel
	case "climb":
		return m.Climb
	}
	return m.Power
}

func pct(v []float64, p float64) float64 {
	return v[min(len(v)-1, int(float64(len(v))*p))]
}

type step struct{ t, vx, vy, vz, h float64 }

// Hypot is math.hypot as Python 3.12 computes it: correctly rounded in nearly every case (Go's own is up to an ulp off).
func Hypot(x, y float64) float64 {
	x, y = math.Abs(x), math.Abs(y)
	if math.IsInf(x, 0) || math.IsInf(y, 0) {
		return math.Inf(1)
	}
	if math.IsNaN(x) || math.IsNaN(y) {
		return math.NaN()
	}
	if x == 0 || y == 0 {
		return x + y
	}
	// scale by a power of two so the squares stay in range
	_, e := math.Frexp(math.Max(x, y))
	sc := math.Ldexp(1, -e)
	a, b := x*sc, y*sc
	p1 := a * a
	e1 := math.FMA(a, a, -p1)
	p2 := b * b
	e2 := math.FMA(b, b, -p2)
	s := p1 + p2
	bb := s - p1
	es := (p1 - (s - bb)) + (p2 - bb) // exact error of the sum
	h0 := math.Sqrt(s)
	q := h0 * h0
	eq := math.FMA(h0, h0, -q)
	r := (s - q) + (es + e1 + e2 - eq)
	h := h0 + r/(2*h0)
	return h / sc
}

func steps(f []Frame) []step {
	var out []step
	for i := 0; i+1 < len(f); i++ {
		a, b := f[i], f[i+1]
		dt := b.T - a.T
		if 0.02 < dt && dt <= 1 {
			vx, vy, vz := (b.X-a.X)/dt, (b.Y-a.Y)/dt, (b.Z-a.Z)/dt
			out = append(out, step{(a.T + b.T) / 2, vx, vy, vz, Hypot(vx, vz)})
		}
	}
	return out
}

// Measure gives the numbers the rules read, or nil if the ghost holds too little driving to judge.
func Measure(g *Ghost) *Measures {
	f := g.Frames
	st := steps(f)
	var speeds, accels, high []float64
	for i := 0; i+1 < len(st); i++ {
		a, b := st[i], st[i+1]
		dt := b.t - a.t
		v := (a.h + b.h) / 2
		if dt <= 0.02 || dt > 1 || v < 5 || a.h > TeleportSpeed || b.h > TeleportSpeed {
			continue
		}
		along := ((b.vx-a.vx)*(a.vx+b.vx) + (b.vz-a.vz)*(a.vz+b.vz)) / (2 * v * dt)
		speeds = append(speeds, v)
		accels = append(accels, along)
		if v > HighSpeed {
			high = append(high, along*v)
		}
	}
	if len(speeds) < MinFrames {
		return nil
	}
	sort.Float64s(speeds)
	sort.Float64s(accels)
	sort.Float64s(high)
	climbs := make([]float64, len(st))
	tele := 0
	for i, s := range st {
		climbs[i] = s.vy
		if s.h > TeleportSpeed {
			tele++
		}
	}
	sort.Float64s(climbs)
	gap := f[1].T - f[0].T
	for i := 1; i+1 < len(f); i++ {
		gap = math.Max(gap, f[i+1].T-f[i].T)
	}
	m := &Measures{Frames: len(f), First: f[0].T, Gap: gap, Speed: pct(speeds, .99), Accel: pct(accels, .99), Climb: pct(climbs, .99),
		HighSamples: len(high), Teleports: tele}
	if len(high) >= MinHighSamples {
		m.Power = pct(high, .95)
	}
	return m
}

// HeaderTime says whether the ghost's header carries score. The field moves with the strings before it.
func HeaderTime(header []byte, score float64) bool {
	for o := 2040; o < min(2200, len(header)-8); o++ {
		if math.Abs(math.Float64frombits(binary.LittleEndian.Uint64(header[o:]))-score) < HeaderTolerance {
			return true
		}
	}
	return false
}

// HeaderCar says whether the ghost's header carries the car id.
func HeaderCar(header []byte, car int64) bool {
	for o := 2900; o < min(3500, len(header)-4); o++ {
		if int64(le32(header, o)) == car {
			return true
		}
	}
	return false
}

// Check judges a stored ghost for the submitted time and car: (verdict, reasons, measurements).
func Check(blob []byte, score float64, car int64) (string, []string, *Measures) {
	g, err := Parse(blob)
	if err != nil {
		if ge, ok := err.(*Error); ok {
			return Hard, []string{"unreadable ghost: " + ge.Msg}, nil
		}
		panic(err)
	}
	var hard, flags []string
	if math.Abs(g.LastT-score) > EndTolerance {
		hard = append(hard, fmt.Sprintf("ghost ends at %.3f s, the score says %.3f s", g.LastT, score))
	}
	if !HeaderTime(g.Header, score) {
		hard = append(hard, "the score is not in the ghost's header")
	}
	if !HeaderCar(g.Header, car) {
		flags = append(flags, "the car is not in the ghost's header") // also true of a real run of the maintainer's: not proof
	}
	m := Measure(g)
	if m == nil {
		hard = append(hard, "too little driving in the ghost")
		return Hard, hard, nil
	}
	m.Last = g.LastT
	if m.Gap > MaxFrameGap {
		hard = append(hard, fmt.Sprintf("%.1f s missing between frames", m.Gap))
	}
	if m.Teleports >= MaxTeleports {
		hard = append(hard, fmt.Sprintf("%d jumps across the map", m.Teleports))
	}
	for _, l := range hardLimits {
		v := m.get(l.rule)
		if v > l.v {
			hard = append(hard, fmt.Sprintf("%s %.1f is above %s", l.rule, v, pyfmt.FmtG(l.v)))
		} else if fl, ok := flagLimits[l.rule]; ok && v > fl {
			flags = append(flags, fmt.Sprintf("%s %.1f is above %s", l.rule, v, pyfmt.FmtG(fl)))
		}
	}
	if len(hard) > 0 {
		return Hard, append(hard, flags...), m
	}
	if len(flags) > 0 {
		return Flag, flags, m
	}
	return Pass, nil, m
}
