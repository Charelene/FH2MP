package achievements

import (
	"testing"
	"time"
)

func TestTheListIsTheGames88(t *testing.T) {
	count, score := [3]int{}, [3]int{}
	seen := map[string]bool{}
	for _, d := range Defs() {
		if seen[d.ID] {
			t.Fatalf("%s is listed twice", d.ID)
		}
		seen[d.ID] = true
		count[d.Group]++
		score[d.Group] += d.Gamerscore
	}
	// Forza Horizon 2: 50 for 1000G, Storm Island: 23 for 500G, the Porsche Expansion: 15 for 500G
	if count != [3]int{50, 23, 15} || score != [3]int{1000, 500, 500} {
		t.Fatalf("counts %v, Gamerscore %v", count, score)
	}
}

func TestGamerscoreCountsEachKnownIdOnce(t *testing.T) {
	if g := Gamerscore([]string{"welcome_festival", "welcome_sisteron", "welcome_festival", "not_an_achievement"}); g != 35 {
		t.Fatalf("got %d, want 10 + 25", g)
	}
}

func TestAcceptKeepsKnownIdsWithTheirTimes(t *testing.T) {
	now := time.Date(2026, 11, 11, 20, 0, 0, 0, time.UTC)
	kept, unknown := Accept(map[string]string{
		"welcome_sisteron": "2026-10-20 18:40:03 UTC",
		"welcome_festival": "2026-10-20 18:02:11 UTC",
		"made_up":          "2026-10-20 18:02:11 UTC",
	}, now)
	if unknown != 1 || len(kept) != 2 {
		t.Fatalf("kept %v, unknown %d", kept, unknown)
	}
	// the list's order, not the report's
	if kept[0].ID != "welcome_festival" || kept[1].ID != "welcome_sisteron" {
		t.Fatalf("order: %v", kept)
	}
	if got := When(kept[0].At); got != "2026-10-20 18:02:11 UTC" {
		t.Fatalf("time came back as %q", got)
	}
}

func TestAcceptTakesAnImpossibleTimeAsNow(t *testing.T) {
	now := time.Date(2026, 11, 11, 20, 0, 0, 0, time.UTC)
	for _, when := range []string{"", "yesterday", "2013-01-01 00:00:00 UTC", "2031-01-01 00:00:00 UTC"} {
		kept, _ := Accept(map[string]string{"rain_master": when}, now)
		if len(kept) != 1 || When(kept[0].At) != "2026-11-11 20:00:00 UTC" {
			t.Fatalf("%q kept as %v", when, kept)
		}
	}
}
