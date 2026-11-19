// Package achievements is Forza Horizon 2's 88 Xbox achievements as the server knows them: which ids exist, what each is worth, and
// which reported unlocks are kept.
//
// The server does not decide achievements. A player's client works them out (the Achievements mod, from the save and the game's own
// events) and the Guide reports what that mod's unlock file holds. So what is stored is what a client said: enough to show on a
// gamercard, not a record to rank or reward players by.
package achievements

import (
	"sort"
	"time"
)

// Def is one achievement.
type Def struct {
	ID, Name, Description string
	Gamerscore            int
	Group                 int // 0 the game, 1 Storm Island, 2 the Porsche Expansion
}

var byID = func() map[string]Def {
	m := make(map[string]Def, len(defs))
	for _, d := range defs {
		m[d.ID] = d
	}
	return m
}()

// Defs is every achievement in the list's order.
func Defs() []Def { return defs }

// Gamerscore is what the unlocked ids are worth together; ids the list does not have count nothing.
func Gamerscore(ids []string) int {
	g := 0
	seen := map[string]bool{}
	for _, id := range ids {
		if d, ok := byID[id]; ok && !seen[id] {
			g += d.Gamerscore
			seen[id] = true
		}
	}
	return g
}

// TimeLayout is how the Achievements mod writes when an achievement was unlocked.
const TimeLayout = "2006-01-02 15:04:05 UTC"

// released is the day the game came out: nothing was unlocked before it.
var released = time.Date(2014, 9, 30, 0, 0, 0, 0, time.UTC)

// Unlock is one reported unlock the server keeps.
type Unlock struct {
	ID string
	At float64 // seconds since 1970
}

// Accept is what is kept of a report (id -> when, in the mod's form): the ids the list has, each with its time. A time that cannot be
// read, lies before the game's release or after now is taken as now. unknown counts the ids the list does not have. The result is in the
// list's order.
func Accept(reported map[string]string, now time.Time) (kept []Unlock, unknown int) {
	for id, when := range reported {
		if _, ok := byID[id]; !ok {
			unknown++
			continue
		}
		at, err := time.Parse(TimeLayout, when)
		if err != nil || at.Before(released) || at.After(now) {
			at = now
		}
		kept = append(kept, Unlock{ID: id, At: float64(at.UnixNano()) / 1e9})
	}
	order := make(map[string]int, len(defs))
	for i, d := range defs {
		order[d.ID] = i
	}
	sort.Slice(kept, func(i, j int) bool { return order[kept[i].ID] < order[kept[j].ID] })
	return kept, unknown
}

// When is a stored time in the mod's form.
func When(at float64) string {
	return time.Unix(int64(at), 0).UTC().Format(TimeLayout)
}
