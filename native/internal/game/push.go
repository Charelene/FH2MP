package game

import (
	"math"

	"fh2mp/internal/bits"
)

// Push check (log only): a car that gains energy its engine cannot give it. A mod that adds speed to the car's velocity
// (NitroSystem adds (TorqueScale-1)*22 m/s^2 along the nose while the car is on the ground, 13.2 at its default, at most 22)
// or a speed hack shows up as specific mechanical energy, speed^2/2 + g*height, rising faster than any car makes it, or
// rising with the throttle released, when a real car always loses energy to drag and rolling. Height takes slopes out:
// a car rolling downhill trades height for speed and its energy stays put. Measured from the relayed CarPrediction
// (velocity and throttle, docs/re/netv2.md 7.2) over windows of at least PushWindowS on the car's own clock, so no single
// packet decides; every quarter of a window must show the gain and a hit must hold for PushHoldS, so a knock from another car,
// which injects energy for a moment, does not count.
const (
	PushGravity     = 9.81
	PushWindowS     = 1.0   // s of car clock per measurement
	PushMaxGapS     = 0.5   // s between two predictions; a longer gap starts over
	PushHoldS       = 2.0   // s a rule must hold before it is logged
	PushQuarters    = 4     // parts of a window; each must show the gain
	PushMinSpeed    = 10.0  // m/s, below this the car is manoeuvring and no rule is read
	PushPowerSpeed  = 30.0  // m/s, the power rule is read above this
	PushCoastThrot  = 0.05  // throttle at or below this all window long counts as released
	PushCoastAccel  = 5.0   // m/s^2 of energy gain per metre travelled (power / speed) with the throttle released
	PushPowerLimit  = 700.0 // W/kg, net specific power (the Rivals ghost check flags the same figure)
	PushVelBit      = 0x0c * 8
	PushThrottleBit = 0x2b * 8
	PushVelocityMax = 200.0 // m/s, the int16 velocity range; a saturated axis is not a measurement
	pushSampleKeep  = 64
)

// PushPolicy: "log" (default) writes the PUSH lines, "off" skips the check.
var PushPolicy = envStr("FH2MP_PUSH_POLICY", "log")

type pushSample struct {
	t        float64 // s on the car's clock
	energy   float64 // J/kg: speed^2/2 + g*height
	speed    float64 // m/s
	throttle float64 // 0..1
}

// PushState is one peer's push check.
type PushState struct {
	samples          []pushSample
	scene            int64
	stamp            uint32  // network-clock ms of the last sample
	clock            float64 // s, the car's clock since the first sample (stamp differences added up, so the uint32 may wrap)
	coastS, powerS   float64
	coastAt, powerAt float64 // car clock of the last window counted into coastS / powerS
	TopPower         float64 // W/kg, highest window seen with speed above PushPowerSpeed (session summary)
	TopCoast         float64 // m/s^2, highest no-throttle gain seen
	CoastLogged      bool
	PowerLogged      bool
}

func carVelocity(rest bits.Int) ([3]float64, bool) {
	var v [3]float64
	ok := true
	for i := 0; i < 3; i++ {
		q := int(rest.Field(PushVelBit+16*i, 16))
		if q&0x8000 != 0 {
			q -= 0x10000
		}
		if q >= 0x7FFF || q <= -0x7FFF {
			ok = false
		}
		v[i] = float64(q) * PushVelocityMax / 0x7FFF
	}
	return v, ok
}

// TrackPush reads one whole prediction. stamp is its network-clock ms, scene its teleport counter and universe.
func (w *World) TrackPush(peer *Peer, rest bits.Int, pos [3]float64, stamp uint64, scene int64) {
	if PushPolicy == "off" {
		return
	}
	st := &peer.Push
	vel, ok := carVelocity(rest)
	speed := math.Sqrt(vel[0]*vel[0] + vel[1]*vel[1] + vel[2]*vel[2])
	s := pushSample{speed: speed, energy: speed*speed/2 + PushGravity*pos[1], throttle: float64(rest.Field(PushThrottleBit, 8)) / 255}
	if len(st.samples) > 0 {
		d := float64(int32(uint32(stamp)-st.stamp)) / 1000
		if !ok || scene != st.scene || d <= 0 || d > PushMaxGapS {
			st.samples, st.coastS, st.powerS = st.samples[:0], 0, 0
		} else {
			st.clock += d
		}
	}
	st.scene, st.stamp = scene, uint32(stamp)
	if !ok {
		return
	}
	s.t = st.clock
	st.samples = append(st.samples, s)
	if len(st.samples) > pushSampleKeep {
		st.samples = append(st.samples[:0], st.samples[len(st.samples)-pushSampleKeep:]...)
	}
	// the newest window of at least PushWindowS
	first := -1
	for i := len(st.samples) - 2; i >= 0; i-- {
		if s.t-st.samples[i].t >= PushWindowS {
			first = i
			break
		}
	}
	if first < 0 {
		return
	}
	win := st.samples[first:]
	dt := s.t - win[0].t
	minSpeed, maxThrottle, meanSpeed := math.Inf(1), 0.0, 0.0
	for _, x := range win {
		minSpeed = math.Min(minSpeed, x.speed)
		maxThrottle = math.Max(maxThrottle, x.throttle)
		meanSpeed += x.speed
	}
	meanSpeed /= float64(len(win))
	if minSpeed < PushMinSpeed {
		st.coastS, st.powerS = 0, 0
		return
	}
	// The window's power is its weakest quarter's: a push lasts the whole window, a knock from another car fills one quarter.
	power, gain := math.Inf(1), math.Inf(1)
	for q, j := 0, 0; q < PushQuarters; q++ {
		end := win[0].t + dt*float64(q+1)/PushQuarters
		k := j
		for k+1 < len(win) && win[k].t < end-1e-9 {
			k++
		}
		if k == j {
			continue // no sample in this quarter: the neighbours speak for it
		}
		p := (win[k].energy - win[j].energy) / (win[k].t - win[j].t)
		power = math.Min(power, p)
		gain = math.Min(gain, p/((win[k].speed+win[j].speed)/2))
		j = k
	}
	coast := maxThrottle <= PushCoastThrot
	if coast {
		st.TopCoast = math.Max(st.TopCoast, gain)
	}
	if minSpeed >= PushPowerSpeed {
		st.TopPower = math.Max(st.TopPower, power)
	}
	step := func(held *float64, at *float64, hit bool) {
		if !hit {
			*held = 0
			return
		}
		if *held == 0 {
			*held = dt // the first window counts whole
		} else {
			*held += s.t - *at
		}
		*at = s.t
	}
	step(&st.coastS, &st.coastAt, coast && gain > PushCoastAccel)
	step(&st.powerS, &st.powerAt, minSpeed >= PushPowerSpeed && power > PushPowerLimit)
	if st.coastS >= PushHoldS && !st.CoastLogged {
		st.CoastLogged = true
		w.Warnf("PUSH (log only): owner %d xuid %#x car %s gained energy with the throttle released: %.1f m/s^2 (limit %.0f; a coasting car loses energy) for %.1f s at %.0f km/h, throttle at most %.2f",
			peer.Owner, peer.Xuid, modelStr(peer), gain, PushCoastAccel, st.coastS, meanSpeed*3.6, maxThrottle)
		w.pushEvent(peer, "coast", gain, st.coastS, meanSpeed)
	}
	if st.powerS >= PushHoldS && !st.PowerLogged {
		st.PowerLogged = true
		w.Warnf("PUSH (log only): owner %d xuid %#x car %s made %.0f W/kg of speed and height energy (limit %.0f) for %.1f s at %.0f km/h, throttle up to %.2f",
			peer.Owner, peer.Xuid, modelStr(peer), power, PushPowerLimit, st.powerS, meanSpeed*3.6, maxThrottle)
		w.pushEvent(peer, "power", power, st.powerS, meanSpeed)
	}
}

func (w *World) pushEvent(peer *Peer, rule string, value, held, speed float64) {
	var car any
	if peer.ModelID != nil {
		car = *peer.ModelID
	}
	w.ev("push_suspect", kv("xuid", w.peerXuid(peer)), kv("owner", peer.Owner), kv("rule", rule), kv("value", math.Round(value*10)/10),
		kv("held_s", math.Round(held*10)/10), kv("kmh", math.Round(speed*3.6)), kv("car", car))
}

// pushSummary is the DEBUG (sub "push") line at the end of a car's session: its peaks, to calibrate the limits on real driving.
func (w *World) pushSummary(peer *Peer) {
	st := &peer.Push
	if st.TopPower == 0 && st.TopCoast == 0 {
		return
	}
	w.Debugf("push", "%d push peaks: xuid %#x car %s: top net power %.0f W/kg above %.0f km/h (limit %.0f), top energy gain with the throttle released %.1f m/s^2 (limit %.0f)",
		peer.Addr.Port, peer.Xuid, modelStr(peer), st.TopPower, PushPowerSpeed*3.6, PushPowerLimit, st.TopCoast, PushCoastAccel)
}
