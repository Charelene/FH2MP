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
