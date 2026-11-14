package ghost

// Sample is one second-by-second reading of a ghost, taken between two recorded frames. Speed, Accel, Power and Climb are derived from the
// positions exactly as Measure reads them; Gear, Rpm and Steer are the fields of the position record as decoded (their meaning is inferred).
type Sample struct {
	T, X, Z       float64
	Speed         float64 // m/s over the ground
	Accel         float64 // m/s^2 along the direction of travel
	Power         float64 // Accel*Speed, only above HighSpeed (the figure the power rule reads)
	Climb         float64 // m/s vertical
	Gear          int
	Rpm, Steer    float64
	Jump, Counted bool // Jump: more than TeleportSpeed between frames; Counted: the sample is one Measure reads
}

// Series gives the decoded ghost as samples, one per pair of recorded frames, for review.
func Series(g *Ghost) []Sample {
	f := g.Frames
	var out []Sample
	var prev *step
	for i := 0; i+1 < len(f); i++ {
		a, b := f[i], f[i+1]
		dt := b.T - a.T
		if !(0.02 < dt && dt <= 1) {
			continue
		}
		vx, vy, vz := (b.X-a.X)/dt, (b.Y-a.Y)/dt, (b.Z-a.Z)/dt
		cur := step{(a.T + b.T) / 2, vx, vy, vz, Hypot(vx, vz)}
		s := Sample{T: cur.t, X: (a.X + b.X) / 2, Z: (a.Z + b.Z) / 2, Speed: cur.h, Climb: vy, Gear: b.Gear, Rpm: b.Rpm, Steer: b.Steer, Jump: cur.h > TeleportSpeed}
		if prev != nil {
			sdt := cur.t - prev.t
			v := (prev.h + cur.h) / 2
			if sdt > 0.02 && sdt <= 1 && v >= 5 && prev.h <= TeleportSpeed && cur.h <= TeleportSpeed {
				s.Accel = ((cur.vx-prev.vx)*(prev.vx+cur.vx) + (cur.vz-prev.vz)*(prev.vz+cur.vz)) / (2 * v * sdt)
				s.Counted = true
				if v > HighSpeed {
					s.Power = s.Accel * v
				}
			}
		}
		out = append(out, s)
		c := cur
		prev = &c
	}
	return out
}
