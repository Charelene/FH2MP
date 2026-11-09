// Command fh2mp-ghost turns a stored Rivals ghost into telemetry for review: fh2mp-ghost json <ghost file> [label] prints one JSON object with the
// car's frames (time, position, inputs, gear, rpm), its velocity keyframes, the collision times and the ghost check's verdict, for a viewer.
// The file is the raw blob of score_ghosts.data. Nothing is written.
//
// fh2mp-ghost measure reads "<run id><TAB><base64 blob>" lines on stdin (psql -At output of score_ghosts) and prints one tab-separated line per run:
// id, p95 net power (W/kg: gain of speed^2/2 + g*height per second over >= 1 s windows with the car above 30 m/s, the last 3 s left out so the
// finish line does not count), p99 speed (km/h), windows that gained energy at over 5 m/s^2 with the throttle released, collision records,
// last record time; or id and "error: ..." for a ghost that does not parse.
package main

import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"sort"
	"strings"

	"fh2mp/internal/ghost"
)

type out struct {
	Label     string          `json:"label"`
	LastT     float64         `json:"last_t"`
	Fields    []string        `json:"fields"`
	Frames    [][]float64     `json:"frames"`
	KeyFields []string        `json:"key_fields"`
	Keyframes [][]float64     `json:"keyframes"`
	Collision []float64       `json:"collisions"`
	Counts    map[int]int     `json:"counts"`
	Verdict   string          `json:"verdict"`
	Reasons   []string        `json:"reasons"`
	Measures  *ghost.Measures `json:"measures"`
}

func main() {
	if len(os.Args) == 2 && os.Args[1] == "measure" {
		measureAll()
		return
	}
	if len(os.Args) < 3 || os.Args[1] != "json" {
		fmt.Fprintln(os.Stderr, "usage: fh2mp-ghost json <ghost file> [label] | fh2mp-ghost measure < id-tab-base64 lines")
		os.Exit(2)
	}
	blob, err := os.ReadFile(os.Args[2])
	if err != nil {
		fmt.Fprintln(os.Stderr, "fh2mp-ghost:", err)
		os.Exit(1)
	}
	g, err := ghost.Parse(blob)
	if err != nil {
		fmt.Fprintf(os.Stderr, "fh2mp-ghost: %s does not parse: %v\n", os.Args[2], err)
		os.Exit(1)
	}
	o := out{Label: os.Args[2], LastT: g.LastT, Counts: g.Counts, Collision: g.Collision,
		Fields:    []string{"t", "x", "y", "z", "throttle", "brake", "steer", "input_d", "input_e", "gear", "flags", "rpm"},
		KeyFields: []string{"t", "x", "y", "z", "vx", "vy", "vz"}}
	if len(os.Args) > 3 {
		o.Label = os.Args[3]
	}
	for _, f := range g.Frames {
		o.Frames = append(o.Frames, []float64{f.T, f.X, f.Y, f.Z, f.InputA, f.InputB, f.Steer, f.InputD, f.InputE, float64(f.Gear), float64(f.Flags), f.Rpm})
	}
	for _, k := range g.Keyframes {
		o.Keyframes = append(o.Keyframes, []float64{k.T, k.X, k.Y, k.Z, k.VX, k.VY, k.VZ})
	}
	o.Verdict, o.Reasons, o.Measures = ghost.Check(blob, g.LastT, -1)
	enc := json.NewEncoder(os.Stdout)
	if err := enc.Encode(o); err != nil {
		fmt.Fprintln(os.Stderr, "fh2mp-ghost:", err)
		os.Exit(1)
	}
}

type sample struct{ t, v, y, throttle float64 }

func measure(g *ghost.Ghost) (p95, top float64, coast int) {
	var rows []sample
	f := g.Frames
	for i := 0; i+1 < len(f); i++ {
		a, b := f[i], f[i+1]
		dt := b.T - a.T
		if dt <= 0.02 || dt > 1 {
			continue
		}
		v := math.Sqrt((b.X-a.X)*(b.X-a.X)+(b.Y-a.Y)*(b.Y-a.Y)+(b.Z-a.Z)*(b.Z-a.Z)) / dt
		if v < 120 {
			rows = append(rows, sample{(a.T + b.T) / 2, v, (a.Y + b.Y) / 2, a.InputA})
		}
	}
	var pw, sp []float64
	for _, r := range rows {
		sp = append(sp, r.v)
	}
	for i := range rows {
		j := i
		for j+1 < len(rows) && rows[j].t-rows[i].t < 1.0 {
			j++
		}
		if rows[j].t-rows[i].t < 1.0 || rows[j].t > g.LastT-3 {
			break
		}
		w := rows[i : j+1]
		dt := w[len(w)-1].t - w[0].t
		minV, maxThr, meanV := math.Inf(1), 0.0, 0.0
		for _, x := range w {
			minV, maxThr, meanV = math.Min(minV, x.v), math.Max(maxThr, x.throttle), meanV+x.v
		}
		meanV /= float64(len(w))
		if minV < 10 {
			continue
		}
		e := func(x sample) float64 { return x.v*x.v/2 + 9.81*x.y }
		p := (e(w[len(w)-1]) - e(w[0])) / dt
		if minV > 30 {
			pw = append(pw, p)
		}
		if maxThr <= 0.05 && p/meanV > 5 {
			coast++
		}
	}
	sort.Float64s(pw)
	sort.Float64s(sp)
	if len(pw) >= 10 {
		p95 = pw[int(float64(len(pw))*0.95)]
	} else {
		p95 = math.NaN()
	}
	if len(sp) > 0 {
		top = sp[int(float64(len(sp))*0.99)] * 3.6
	}
	return
}

func measureAll() {
	in := bufio.NewReaderSize(os.Stdin, 1<<20)
	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()
	for {
		line, err := in.ReadString('\n')
		if line = strings.TrimRight(line, "\r\n"); line != "" {
			id, b64, ok := strings.Cut(line, "\t")
			blob, derr := base64.StdEncoding.DecodeString(b64)
			switch {
			case !ok || derr != nil:
				fmt.Fprintf(out, "%s\terror: not an id<TAB>base64 line\n", id)
			default:
				if g, perr := ghost.Parse(blob); perr != nil {
					fmt.Fprintf(out, "%s\terror: %v\n", id, perr)
				} else {
					p95, top, coast := measure(g)
					fmt.Fprintf(out, "%s\t%.1f\t%.1f\t%d\t%d\t%.3f\n", id, p95, top, coast, len(g.Collision), g.LastT)
				}
			}
		}
		if err != nil {
			return
		}
	}
}
