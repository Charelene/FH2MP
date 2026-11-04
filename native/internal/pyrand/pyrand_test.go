package pyrand

import (
	"encoding/json"
	"os"
	"testing"
)

type vec struct {
	Seed        uint64    `json:"seed"`
	Random      []float64 `json:"random"`
	Randbelow   []int     `json:"randbelow"`
	Uniform     float64   `json:"uniform"`
	Choice      []int     `json:"choice"`
	SampleSmall []int     `json:"sample_small"`
	SampleBig   []int     `json:"sample_big"`
	Shuffle     []int     `json:"shuffle"`
	Choices     []int     `json:"choices"`
}

func seq(n int) []int {
	s := make([]int, n)
	for i := range s {
		s[i] = i
	}
	return s
}

func TestMatchesPython(t *testing.T) {
	raw, err := os.ReadFile("../../testdata/pyrand.json")
	if err != nil {
		t.Fatal(err)
	}
	var cases []vec
	if err := json.Unmarshal(raw, &cases); err != nil {
		t.Fatal(err)
	}
	for _, c := range cases {
		r := New(c.Seed)
		for i, want := range c.Random {
			if got := r.Float64(); got != want {
				t.Fatalf("seed %d random[%d] = %v want %v", c.Seed, i, got, want)
			}
		}
		for i, n := range []int{2, 3, 7, 100, 86400000, 5, 1, 12} {
			if got := r.Intn(n); got != c.Randbelow[i] {
				t.Fatalf("seed %d randrange(%d) = %d want %d", c.Seed, n, got, c.Randbelow[i])
			}
		}
		if got := r.Uniform(90.0, 180.0); got != c.Uniform {
			t.Fatalf("seed %d uniform = %v want %v", c.Seed, got, c.Uniform)
		}
		for i := range c.Choice {
			if got := []int{3, 5}[r.Intn(2)]; got != c.Choice[i] {
				t.Fatalf("seed %d choice[%d] = %d want %d", c.Seed, i, got, c.Choice[i])
			}
		}
		eq := func(name string, got, want []int) {
			if len(got) != len(want) {
				t.Fatalf("seed %d %s = %v want %v", c.Seed, name, got, want)
			}
			for i := range got {
				if got[i] != want[i] {
					t.Fatalf("seed %d %s = %v want %v", c.Seed, name, got, want)
				}
			}
		}
		eq("sample_small", Sample(r, seq(10), 3), c.SampleSmall)
		eq("sample_big", Sample(r, seq(200), 7), c.SampleBig)
		xs := seq(9)
		Shuffle(r, xs)
		eq("shuffle", xs, c.Shuffle)
		for i := range c.Choices {
			x := r.Float64() * 1.0
			w := []float64{0.45, 0.35, 0.20}
			idx, acc := 2, 0.0
			for k, wk := range w {
				acc += wk
				if x < acc {
					idx = k
					break
				}
			}
			if idx+1 != c.Choices[i] {
				t.Fatalf("seed %d choices[%d] = %d want %d", c.Seed, i, idx+1, c.Choices[i])
			}
		}
	}
}

func TestStringSeeds(t *testing.T) {
	for _, c := range []struct {
		seed   string
		floats []float64
		below  int
		choice int
	}{
		{"fh2mp-rivals-12-3", []float64{0.3323293480449867, 0.10788649236979608, 0.4893067960614962}, 919, 2},
		{"", []float64{0.9602256525641875, 0.595411957851699, 0.3880117152955401}, 751, 6},
		{"Ünï", []float64{0.4437022086529785, 0.3707481024169975, 0.4795462545896777}, 428, 0},
		{"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", []float64{0.8500063097913054, 0.17305576775745346, 0.32172225320517356}, 920, 6},
		{"a", []float64{0.2720295377534757, 0.06733663088629693, 0.5999141237517758}, 659, 4},
		{"fh2mp-rivals-0-0", []float64{0.4852150158977303, 0.40632866567517945, 0.8776434818143267}, 862, 2},
	} {
		r := NewString(c.seed)
		for i, want := range c.floats {
			if got := r.Float64(); got != want {
				t.Fatalf("seed %q float %d: %v want %v", c.seed, i, got, want)
			}
		}
		if got := r.Intn(1000); got != c.below {
			t.Fatalf("seed %q Intn: %d want %d", c.seed, got, c.below)
		}
		if got := r.Intn(7); got != c.choice {
			t.Fatalf("seed %q choice: %d want %d", c.seed, got, c.choice)
		}
	}
}
