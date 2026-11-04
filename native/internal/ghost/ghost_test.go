package ghost

import (
	"compress/gzip"
	"encoding/hex"
	"encoding/json"
	"math"
	"os"
	"reflect"
	"strconv"
	"testing"
)

type fixture struct {
	Cases []struct {
		Blob  string  `json:"blob"`
		Score float64 `json:"score"`
		Car   int64   `json:"car"`
		Parse struct {
			Error     *string        `json:"error"`
			Other     *string        `json:"other"`
			HeaderLen int            `json:"header_len"`
			Frames    [][]float64    `json:"frames"`
			Keyframes [][]float64    `json:"keyframes"`
			LastT     float64        `json:"last_t"`
			Counts    map[string]int `json:"counts"`
		} `json:"parse"`
		Check struct {
			Other   *string            `json:"other"`
			Verdict string             `json:"verdict"`
			Reasons []string           `json:"reasons"`
			M       map[string]float64 `json:"m"`
		} `json:"check"`
	} `json:"cases"`
	Hypot [][3]float64 `json:"hypot"`
}

func load(t *testing.T) *fixture {
	f, err := os.Open("../../testdata/ghost.json.gz")
	if err != nil {
		t.Skip(err)
	}
	defer f.Close()
	zr, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	var fx fixture
	if err := json.NewDecoder(zr).Decode(&fx); err != nil {
		t.Fatal(err)
	}
	return &fx
}

func same(a, b float64) bool { return math.Float64bits(a) == math.Float64bits(b) }

func TestHypot(t *testing.T) {
	fx := load(t)
	bad := 0
	for _, h := range fx.Hypot {
		if !same(Hypot(h[0], h[1]), h[2]) {
			bad++
			if bad < 5 {
				t.Logf("hypot(%v, %v) = %v, python %v", h[0], h[1], Hypot(h[0], h[1]), h[2])
			}
		}
	}
	t.Logf("%d of %d pairs differ from Python's math.hypot", bad, len(fx.Hypot))
	if bad > 0 {
		t.Errorf("hypot differs on %d pairs", bad)
	}
}

func TestMatchesPython(t *testing.T) {
	fx := load(t)
	inexact := 0
	for i, c := range fx.Cases {
		blob, _ := hex.DecodeString(c.Blob)
		g, err := Parse(blob)
		switch {
		case c.Parse.Other != nil:
			t.Fatalf("case %d: Python raised %s, which Go has no counterpart for", i, *c.Parse.Other)
		case c.Parse.Error != nil:
			ge, ok := err.(*Error)
			if !ok || ge.Msg != *c.Parse.Error {
				t.Fatalf("case %d: error %v, python %q", i, err, *c.Parse.Error)
			}
		default:
			if err != nil {
				t.Fatalf("case %d: %v, python parsed it", i, err)
			}
			if len(g.Header) != c.Parse.HeaderLen || len(g.Frames) != len(c.Parse.Frames) || len(g.Keyframes) != len(c.Parse.Keyframes) || !same(g.LastT, c.Parse.LastT) {
				t.Fatalf("case %d: shape differs", i)
			}
			for j, f := range g.Frames {
				want := c.Parse.Frames[j]
				got := []float64{f.T, f.X, f.Y, f.Z, f.InputA, f.InputB, f.Steer, f.InputD, f.InputE, float64(f.Gear), float64(f.Flags), f.Rpm}
				for k := range got {
					if !same(got[k], want[k]) {
						t.Fatalf("case %d frame %d field %d: %v want %v", i, j, k, got[k], want[k])
					}
				}
			}
			for j, k := range g.Keyframes {
				want := c.Parse.Keyframes[j]
				got := []float64{k.T, k.X, k.Y, k.Z, k.VX, k.VY, k.VZ}
				for n := range got {
					if !same(got[n], want[n]) {
						t.Fatalf("case %d keyframe %d field %d differs", i, j, n)
					}
				}
			}
			counts := map[string]int{}
			for k, v := range g.Counts {
				counts[strconv.Itoa(k)] = v
			}
			if !reflect.DeepEqual(counts, c.Parse.Counts) {
				t.Fatalf("case %d: counts %v want %v", i, counts, c.Parse.Counts)
			}
		}
		verdict, reasons, m := Check(blob, c.Score, c.Car)
		if verdict != c.Check.Verdict || !reflect.DeepEqual(append([]string{}, reasons...), append([]string{}, c.Check.Reasons...)) {
			t.Fatalf("case %d: verdict %s %q, python %s %q", i, verdict, reasons, c.Check.Verdict, c.Check.Reasons)
		}
		if len(c.Check.M) == 0 {
			if m != nil {
				t.Fatalf("case %d: measurements given, python has none", i)
			}
			continue
		}
		if m == nil {
			t.Fatalf("case %d: no measurements, python has some", i)
		}
		for _, p := range m.Obj() {
			var got float64
			switch v := p.V.(type) {
			case int:
				got = float64(v)
			case float64:
				got = v
			}
			want, ok := c.Check.M[p.K]
			if !ok {
				t.Fatalf("case %d: key %s not in python's measurements", i, p.K)
			}
			if !same(got, want) {
				inexact++
				if math.Abs(got-want) > 1e-9*math.Max(1, math.Abs(want)) {
					t.Fatalf("case %d: %s = %v, python %v", i, p.K, got, want)
				}
			}
		}
		if len(m.Obj()) != len(c.Check.M) {
			t.Fatalf("case %d: %d measurements, python %d", i, len(m.Obj()), len(c.Check.M))
		}
	}
	t.Logf("%d cases; %d measurements differ in the last bits", len(fx.Cases), inexact)
}
