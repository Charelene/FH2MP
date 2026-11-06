// Package rentals picks the rental cars to add to the car database so every Rivals theme has some (= server/rentals.py).
package rentals

import "sort"

// Targets.
const (
	RentalTarget = 2
	RentalMin    = 2
)

// PlanExtra is the greedy cover: themeCars maps a theme key to the car ids it allows. Returns the cars to add in the order picked and, per theme,
// the cars still missing.
func PlanExtra(themeCars map[string]map[int]bool, rentals map[int]bool, candidates []int, target int) ([]int, map[string]int) {
	need := map[string]int{}
	for k, cars := range themeCars {
		have := 0
		for c := range cars {
			if rentals[c] {
				have++
			}
		}
		n := len(cars)
		if target < n {
			n = target
		}
		if n-have > 0 {
			need[k] = n - have
		} else {
			need[k] = 0
		}
	}
	byCar := map[int][]string{}
	keys := make([]string, 0, len(themeCars))
	for k := range themeCars {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		if need[k] > 0 {
			for c := range themeCars[k] {
				byCar[c] = append(byCar[c], k)
			}
		}
	}
	pool := map[int]bool{}
	for _, c := range candidates {
		if !rentals[c] {
			pool[c] = true
		}
	}
	var picks []int
	for {
		best, gain := -1, 0
		ids := make([]int, 0, len(pool))
		for c := range pool {
			ids = append(ids, c)
		}
		sort.Ints(ids)
		for _, c := range ids {
			g := 0
			for _, k := range byCar[c] {
				if need[k] > 0 {
					g++
				}
			}
			if g > gain {
				best, gain = c, g
			}
		}
		if best < 0 {
			break
		}
		picks = append(picks, best)
		delete(pool, best)
		for _, k := range byCar[best] {
			if need[k] > 0 {
				need[k]--
			}
		}
	}
	missing := map[string]int{}
	for k, n := range need {
		if n > 0 {
			missing[k] = n
		}
	}
	return picks, missing
}

// Covered counts the rentals an event with this set of allowed cars has.
func Covered(cars []int, rentals map[int]bool) int {
	n := 0
	seen := map[int]bool{}
	for _, c := range cars {
		if !seen[c] && rentals[c] {
			n++
		}
		seen[c] = true
	}
	return n
}

// Enough: the event offers `minimum` rentals, or every car it allows when it allows fewer.
func Enough(cars []int, rentals map[int]bool, minimum int) bool {
	distinct := map[int]bool{}
	for _, c := range cars {
		distinct[c] = true
	}
	m := len(distinct)
	if minimum < m {
		m = minimum
	}
	return Covered(cars, rentals) >= m
}
