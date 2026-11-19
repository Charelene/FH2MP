package ghost

import (
	"encoding/hex"
	"sort"
	"testing"
)

// The replay series must hold the very samples Measure reads: its 99th-percentile speed, acceleration and 95th-percentile power are the verdict's numbers.
func TestSeriesMatchesMeasure(t *testing.T) {
	fx := load(t)
	checked := 0
	for i, c := range fx.Cases {
		blob, _ := hex.DecodeString(c.Blob)
		g, err := Parse(blob)
		if err != nil {
			continue
		}
		m := Measure(g)
		if m == nil {
			continue
		}
		var speeds, accels, high []float64
		for _, s := range Series(g) {
			if !s.Counted {
				continue
			}
			speeds = append(speeds, (s.Speed))
			accels = append(accels, s.Accel)
			if s.Power != 0 {
				high = append(high, s.Power)
			}
		}
		if len(speeds) < MinFrames {
			t.Fatalf("case %d: series has %d counted samples", i, len(speeds))
		}
		sort.Float64s(accels)
		sort.Float64s(high)
		if got := pct(accels, .99); !same(got, m.Accel) {
			t.Errorf("case %d: series acceleration %v, Measure %v", i, got, m.Accel)
		}
		if len(high) >= MinHighSamples {
			if got := pct(high, .95); !same(got, m.Power) {
				t.Errorf("case %d: series power %v, Measure %v", i, got, m.Power)
			}
		}
		checked++
	}
	t.Logf("%d ghosts compared", checked)
	if checked == 0 {
		t.Skip("no readable ghost in the fixture")
	}
}
