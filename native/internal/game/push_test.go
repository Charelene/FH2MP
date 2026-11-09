package game

import (
	"math"
	"strings"
	"testing"

	"fh2mp/internal/bits"
)

// pushBody is the part of a prediction the push check reads: velocity (int16 over +-200 m/s) and throttle (uint8 0..1).
func pushBody(vel [3]float64, throttle float64) bits.Int {
	body := bits.Int(nil)
	for i, v := range vel {
		q := int(math.Max(-0x7FFF, math.Min(0x7FFF, math.Round(v/PushVelocityMax*0x7FFF)))) // the game's quantiser clamps
		body = body.Or(bits.FromUint64(uint64(uint16(int16(q)))).Lsh(PushVelBit + 16*i))
	}
	return body.Or(bits.FromUint64(uint64(math.Round(throttle * 255))).Lsh(PushThrottleBit))
}

// drive feeds the check 20 predictions a second for secs: speed and height follow accel(t) (m/s^2 along the road) on a slope of
// climb metres per metre, with the given throttle. Returns the PUSH lines.
func drive(t *testing.T, speed0, secs, climb float64, throttle float64, accel func(t float64) float64) []string {
	t.Helper()
	r := newLoadRig(t, 1)
	p := r.peers[0]
	mid := int64(1234)
	p.ModelID = &mid
	speed, x, y := speed0, 0.0, 0.0
	norm := math.Sqrt(1 + climb*climb)
	for i := 0; float64(i)*0.05 <= secs; i++ {
		tt := float64(i) * 0.05
		v := [3]float64{speed / norm, speed * climb / norm, 0}
		r.w.TrackPush(p, pushBody(v, throttle), [3]float64{x, y, 0}, uint64(0xFFFFFF00+uint32(i*50)), 0) // the clock wraps on the way
		a := accel(tt)
		speed += a * 0.05
		x += v[0] * 0.05
		y += v[1] * 0.05
	}
	var out []string
	for _, l := range r.log {
		if strings.Contains(l, "PUSH") {
			out = append(out, l)
		}
	}
	return out
}

func TestPushCheck(t *testing.T) {
	g := PushGravity
	cases := []struct {
		name                     string
		speed0, secs, climb, thr float64
		accel                    func(float64) float64
		want                     []string // substrings, one per expected line
	}{
		{"coasting on the flat", 40, 6, 0, 0, func(float64) float64 { return -0.4 }, nil},
		{"coasting downhill (height turns into speed)", 30, 6, -0.1, 0, func(float64) float64 { return g*0.1/math.Sqrt(1.01) - 0.4 }, nil},
		{"strong car at full throttle", 30, 6, 0, 1, func(float64) float64 { return 6 }, nil},
		{"full throttle up a hill", 30, 6, 0.08, 1, func(float64) float64 { return 2 }, nil},
		{"a knock from another car", 30, 6, 0, 0, func(t float64) float64 {
			if t > 2 && t < 2.4 {
				return 30
			}
			return -0.4
		}, nil},
		{"nitro with the throttle released", 30, 4, 0, 0, func(float64) float64 { return 13.2 }, []string{"throttle released", "W/kg"}},
		{"nitro at 1.3x on a slow car", 15, 5, 0, 0, func(float64) float64 { return 6.6 }, []string{"throttle released"}},
		{"nitro at full throttle", 40, 4, 0, 1, func(float64) float64 { return 13.2 + 4 }, []string{"W/kg"}},
		{"slow car under 10 m/s", 2, 1, 0, 0, func(float64) float64 { return 5 }, nil},
	}
	for _, c := range cases {
		got := drive(t, c.speed0, c.secs, c.climb, c.thr, c.accel)
		if len(got) != len(c.want) {
			t.Errorf("%s: %d PUSH line(s), want %d: %q", c.name, len(got), len(c.want), got)
			continue
		}
		for i, w := range c.want {
			if !strings.Contains(got[i], w) {
				t.Errorf("%s: line %q lacks %q", c.name, got[i], w)
			}
		}
	}
}

func TestPushResetsOnTeleport(t *testing.T) {
	r := newLoadRig(t, 1)
	p := r.peers[0]
	speed := 30.0
	for i := 0; i < 80; i++ { // 4 s of nitro, with a track reset (teleport counter) every 1.5 s
		scene := int64(i / 30)
		r.w.TrackPush(p, pushBody([3]float64{speed, 0, 0}, 0), [3]float64{}, uint64(1000+i*50), scene)
		speed += 13.2 * 0.05
	}
	for _, l := range r.log {
		if strings.Contains(l, "PUSH") {
			t.Errorf("logged across teleports: %s", l)
		}
	}
}

func TestCarVelocitySaturated(t *testing.T) {
	if _, ok := carVelocity(pushBody([3]float64{250, 0, 0}, 0)); ok {
		t.Error("a velocity axis at the int16 limit was taken as a measurement")
	}
	v, ok := carVelocity(pushBody([3]float64{-37.5, 2, 61}, 0))
	if !ok || math.Abs(v[0]+37.5) > 0.01 || math.Abs(v[1]-2) > 0.01 || math.Abs(v[2]-61) > 0.01 {
		t.Errorf("velocity %v ok=%v", v, ok)
	}
}
