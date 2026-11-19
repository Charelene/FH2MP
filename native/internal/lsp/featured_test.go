package lsp

import (
	"testing"

	"fh2mp/internal/ugc"
)

// A featured livery without a car became a car card with no car, and the game's drivetrain converter read NULL for it (#412).
func TestFeaturedPickLeavesOutLiveriesWithoutACar(t *testing.T) {
	car := func(id int64) *int64 { return &id }
	files := []*ugc.File{
		{ID: 1, CarID: nil},
		{ID: 2, CarID: car(0)},
		{ID: 3, CarID: car(1215)},
		{ID: 4, CarID: car(-5)},
		{ID: 5, CarID: car(1215)}, // a second livery for the same car
		{ID: 6, CarID: car(2384)},
		{ID: 7, CarID: car(338)},
	}
	var got []int64
	for _, f := range featuredPick(files, 2) {
		got = append(got, f.ID)
	}
	if len(got) != 2 || got[0] != 3 || got[1] != 6 {
		t.Errorf("featuredPick = %v, want [3 6]", got)
	}
	if n := len(featuredPick(files[:2], 5)); n != 0 {
		t.Errorf("only car-less liveries: %d picked, want 0", n)
	}
}
