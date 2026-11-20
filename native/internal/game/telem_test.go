package game

import (
	"fmt"
	"math"
	"strings"
	"testing"

	"fh2mp/internal/netv2"
	"fh2mp/internal/pyjson"
	"github.com/Charelene/encore-paddock/bits"
)

// telemBody is a prediction body with the fields the telemetry reads: velocity, throttle, gear and the teleport counter.
func telemBody(vel [3]float64, throttle float64, gear, tele uint64) bits.Int {
	return pushBody(vel, throttle).Or(bits.FromUint64(gear).Lsh(TelemGearBit)).Or(bits.FromUint64(tele).Lsh(netv2.CarTeleportOffset))
}

type telemRun struct {
	secs        float64                 // how long the car drives, one prediction per 50 ms
	speed       func(t float64) float64 // m/s the car really moves along x
	stateFactor float64                 // what its state says, as a multiple of that (1 = honest)
	jumpAt      float64                 // a 500 m jump forward at this time (0 = none)
	jumpTele    bool                    // the jump comes with the game's teleport counter (a reset to the road)
}

// telemFeed drives one peer through TrackTelem as the relay path does, the car's clock wrapping on the way.
func telemFeed(t *testing.T, run telemRun) (*loadRig, *Peer) {
	t.Helper()
	r := newLoadRig(t, 1)
	p := r.peers[0]
	x, tele := 0.0, uint64(0)
	for i := 0; float64(i)*0.05 <= run.secs+1e-9; i++ {
		tt := float64(i) * 0.05
		s := run.speed(tt)
		if run.jumpAt > 0 && math.Abs(tt-run.jumpAt) < 0.025 {
			x += 500
			if run.jumpTele {
				tele = tele%3 + 1
			}
		}
		r.w.TrackTelem(p, telemBody([3]float64{s * run.stateFactor, 0, 0}, 1, 4, tele), [3]float64{x, 10, 0}, uint64(uint32(0xFFFFF000)+uint32(i*50)))
		x += s * 0.05
		r.clk.t += 0.05
	}
	return r, p
}

func TestTelemJudge(t *testing.T) {
	ramp := func(tt float64) float64 { return 20 + 2*tt } // 20 -> 80 m/s (72 -> 288 km/h) over 30 s
	steady := func(float64) float64 { return 50 }
	cases := []struct {
		name    string
		run     telemRun
		kind    string
		value   float64
		stale   float64 // seconds between the end of the drive and the question
		verdict string
		reason  string // substring of the reasons ("" = none expected)
	}{
		{"trap at the speed the car reached", telemRun{secs: 30, speed: ramp, stateFactor: 1}, "trap", 79, 1, "pass", ""},
		{"trap faster than the car ever went", telemRun{secs: 30, speed: ramp, stateFactor: 1}, "trap", 100, 1, "hard", "never went over 288 km/h"},
		{"zone average under the top speed", telemRun{secs: 30, speed: ramp, stateFactor: 1}, "zone", 60, 1, "pass", ""},
		{"zone average over the top speed", telemRun{secs: 30, speed: ramp, stateFactor: 1}, "zone", 95, 1, "hard", "speed zone average 342 km/h"},
		{"state says 50% faster than the car moves", telemRun{secs: 10, speed: steady, stateFactor: 1.5}, "trap", 50, 1, "flag", "faster than it moved"},
		{"position jump without the teleport counter", telemRun{secs: 10, speed: steady, stateFactor: 1, jumpAt: 5}, "trap", 50, 1, "flag", "position jump"},
		{"reset to the road (teleport counter moves)", telemRun{secs: 10, speed: steady, stateFactor: 1, jumpAt: 5, jumpTele: true}, "trap", 50, 1, "pass", ""},
		{"no drive here", telemRun{secs: -1, speed: steady, stateFactor: 1}, "trap", 50, 1, "none", "not driving"},
		{"drove, but long before the result", telemRun{secs: 10, speed: steady, stateFactor: 1}, "trap", 50, 30, "none", "old"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			r, p := telemFeed(t, c.run)
			r.clk.t += c.stale
			now := r.clk.t
			span := TelemWindowS
			if c.kind == "trap" {
				span = TelemTrapS
			}
			got := TelemJudge(p.Telem.window(now-span), c.kind, c.value, now)
			if got.Verdict != c.verdict {
				t.Fatalf("verdict %q, want %q (reasons %q, measures %v)", got.Verdict, c.verdict, got.Reasons, got.Measures)
			}
			joined := strings.Join(got.Reasons, "; ")
			if c.reason == "" && c.verdict == "pass" && joined != "" {
				t.Fatalf("pass with reasons %q", joined)
			}
			if c.reason != "" && !strings.Contains(joined, c.reason) {
				t.Fatalf("reasons %q, want one containing %q", joined, c.reason)
			}
		})
	}
}

func TestTelemRingKeepsTheWindowAtTwentyAHertz(t *testing.T) {
	r := newLoadRig(t, 1)
	p := r.peers[0]
	body := telemBody([3]float64{40, 0, 0}, 1, 3, 0)
	// the relay sees 100 predictions a second for 120 s: only one per 50 ms is kept, and only the last 90 s fit
	for i := 0; i < 12000; i++ {
		r.w.TrackTelem(p, body, [3]float64{float64(i) * 0.4, 0, 0}, uint64(i*10))
		r.clk.t += 0.01
	}
	all := p.Telem.window(0)
	if len(all) != telemCap {
		t.Fatalf("%d samples kept, want the ring full at %d", len(all), telemCap)
	}
	for i := 1; i < len(all); i++ {
		if d := all[i].mono - all[i-1].mono; d < TelemStepS-1e-9 {
			t.Fatalf("samples %d and %d are %.3f s apart, under %.2f s", i-1, i, d, TelemStepS)
		}
	}
	if span := all[len(all)-1].mono - all[0].mono; span < TelemWindowS {
		t.Fatalf("ring covers %.1f s, want at least %.0f s", span, TelemWindowS)
	}
	last := all[len(all)-1]
	if math.Abs(float64(last.speed)-40) > 0.01 || last.throttle != 255 || last.gear != 3 {
		t.Fatalf("last sample speed %.3f throttle %d gear %d, want 40 m/s, 255, 3", last.speed, last.throttle, last.gear)
	}
	w60 := p.Telem.window(r.clk.t - 60)
	if n := len(w60); n < 1190 || n > 1201 {
		t.Fatalf("a 60 s window holds %d samples, want about 1200", n)
	}
}

// The LSP's question on the control port gets the verdict of the connected player's window, logged in its world; a player who is not
// connected, and a request that cannot be read, get "none".
func TestTelemetryControlCommand(t *testing.T) {
	r := newModsRig(t, 1, false)
	p := r.peers[0]
	var reply []byte
	r.hub.CtlSend = func(b []byte, _ Addr) { reply = append([]byte(nil), b...) }
	x := 0.0
	for i := 0; i <= 600; i++ { // 30 s from 20 to 80 m/s
		s := 20 + 2*float64(i)*0.05
		r.w.TrackTelem(p, telemBody([3]float64{s, 0, 0}, 1, 5, 0), [3]float64{x, 0, 0}, uint64(i*50))
		x += s * 0.05
		r.clk.t += 0.05
	}
	ask := func(text string) (string, string) {
		t.Helper()
		reply = nil
		from := Addr{"127.0.0.1", 5999}
		r.hub.ControlText(text, &from)
		pv, err := pyjson.Parse(string(reply))
		v, isObj := pv.(pyjson.Obj)
		if err != nil || !isObj {
			t.Fatalf("reply to %q is not JSON: %q", text, reply)
		}
		verdict, _ := v.Get("verdict")
		reasons, _ := v.Get("reasons")
		if l, ok := v.Get("lobbies"); !ok || l == nil {
			t.Fatalf("reply to %q has no lobbies list (the LSP's query needs it): %q", text, reply)
		}
		return verdict.(string), fmt.Sprint(reasons)
	}
	hex := fmt.Sprintf("%x", p.Xuid)
	if v, why := ask("telemetry " + hex + " trap 79"); v != "pass" {
		t.Fatalf("honest trap: %s %s", v, why)
	}
	if v, why := ask("telemetry " + hex + " trap 100"); v != "hard" || !strings.Contains(why, "never went over 288 km/h") {
		t.Fatalf("trap above the car: %s %s", v, why)
	}
	if v, why := ask("telemetry 9000000000000999 trap 50"); v != "none" || !strings.Contains(why, "not connected") {
		t.Fatalf("player not here: %s %s", v, why)
	}
	if v, _ := ask("telemetry zz trap fast"); v != "none" {
		t.Fatalf("unreadable request: %s", v)
	}
	found := false
	for _, l := range r.log {
		if strings.Contains(l, "telemetry: owner 1") && strings.Contains(l, "trap result 360 km/h -> hard") {
			found = true
		}
	}
	if !found {
		t.Fatalf("no world log line for the hard verdict; log: %q", r.log)
	}
}

// BenchmarkTrackTelem is the relay path's cost per prediction at 100 predictions a second (one in five is decoded and kept).
func BenchmarkTrackTelem(b *testing.B) {
	clk := &fakeClock{t: 1000}
	w := &World{Clk: clk} // TrackTelem reads only the clock and the peer
	p := NewPeer(Addr{"127.0.0.1", 40000}, 1000)
	body := telemBody([3]float64{55, 1, 2}, 0.8, 4, 1)
	pos := [3]float64{100, 20, 300}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		w.TrackTelem(p, body, pos, uint64(i*10))
		clk.t += 0.01
	}
}
